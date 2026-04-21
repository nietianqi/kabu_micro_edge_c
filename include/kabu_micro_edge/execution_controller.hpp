#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/execution_types.hpp"
#include "kabu_micro_edge/gateway.hpp"
#include "kabu_micro_edge/oms.hpp"

namespace kabu::execution {

class ExecutionController {
  public:
    enum class TseTickProfile { Other, Topix100 };
    using EntryOrderSender = std::function<std::string(int side, int qty, double price, bool is_market)>;
    using ExitOrderSender = std::function<std::string(int position_side, int qty, double price, bool is_market)>;
    using CancelOrderSender = std::function<void(const std::string& order_id)>;

    ExecutionController(
        std::string symbol,
        int exchange,
        config::OrderProfile order_profile,
        bool dry_run,
        double tick_size,
        double strong_threshold,
        double min_edge_ticks,
        int max_pending_ms,
        int min_order_lifetime_ms,
        int max_requotes_per_minute,
        bool allow_aggressive_exit,
        bool topix100 = false,
        bool queue_model = true
    )
        : symbol(std::move(symbol)),
          exchange(exchange),
          order_profile(std::move(order_profile)),
          dry_run(dry_run),
          tick_size_(std::max(tick_size, 1e-9)),
          strong_threshold_(strong_threshold),
          min_edge_ticks_(min_edge_ticks),
          max_pending_ns_(std::max(max_pending_ms, 0) * 1'000'000LL),
          min_order_lifetime_ns_(std::max(min_order_lifetime_ms, 0) * 1'000'000LL),
          allow_aggressive_exit_(allow_aggressive_exit),
          topix100_(topix100),
          queue_model_(queue_model),
          requotes(max_requotes_per_minute) {
        stats = {
            {"sent_orders", 0},
            {"cancel_orders", 0},
            {"fills", 0},
            {"open_attempts", 0},
            {"close_attempts", 0},
        };
    }

    std::string symbol;
    int exchange{1};
    config::OrderProfile order_profile;
    bool dry_run{true};
    std::optional<WorkingOrder> working_order;
    std::optional<WorkingOrder> exit_order;
    Inventory inventory;
    std::deque<RoundTrip> closed_trades;
    std::string paper_last_fill_reason;
    bool has_stranded_partial{false};
    std::int64_t entry_blocked_until_ns{0};
    std::int64_t exit_blocked_until_ns{0};
    bool manual_close_lock{false};
    bool has_external_inventory{false};
    bool has_external_active_orders{false};
    int broker_hold_qty{0};
    int broker_closable_qty{0};
    std::vector<std::string> broker_active_order_ids;
    std::map<std::string, int> stats;
    RequoteBudget requotes;

    [[nodiscard]] ExecutionState state() const {
        if (working_order.has_value()) {
            return ExecutionState::Opening;
        }
        if (exit_order.has_value() && inventory.qty > 0) {
            return ExecutionState::Closing;
        }
        if (inventory.qty > 0) {
            return ExecutionState::Open;
        }
        return ExecutionState::Flat;
    }

    [[nodiscard]] std::string current_order_id() const {
        if (working_order.has_value()) {
            return working_order->order_id;
        }
        if (exit_order.has_value()) {
            return exit_order->order_id;
        }
        return "";
    }

    [[nodiscard]] std::vector<std::string> active_order_ids() const {
        std::vector<std::string> ids;
        if (working_order.has_value()) {
            ids.push_back(working_order->order_id);
        }
        if (exit_order.has_value()) {
            ids.push_back(exit_order->order_id);
        }
        return ids;
    }

    [[nodiscard]] bool has_working_entry() const { return working_order.has_value(); }
    [[nodiscard]] bool has_working_exit() const { return exit_order.has_value(); }
    [[nodiscard]] bool has_working_orders() const { return has_working_entry() || has_working_exit(); }
    [[nodiscard]] bool has_external_inventory_conflict() const {
        return has_external_inventory || manual_close_lock || has_external_active_orders;
    }

    [[nodiscard]] bool can_manage_local_exit() const {
        if (inventory.qty <= 0 || manual_close_lock) {
            return false;
        }
        return !has_external_inventory || broker_closable_qty >= inventory.qty;
    }

    void set_live_order_callbacks(EntryOrderSender entry_sender, ExitOrderSender exit_sender, CancelOrderSender cancel_sender) {
        entry_order_sender_ = std::move(entry_sender);
        exit_order_sender_ = std::move(exit_sender);
        cancel_order_sender_ = std::move(cancel_sender);
    }

    [[nodiscard]] PriceDecision preview_entry(
        int direction,
        const gateway::BoardSnapshot& snapshot,
        double score,
        double microprice,
        QuoteMode mode = QuoteMode::PassiveFairValue,
        std::optional<double> reservation_price = std::nullopt,
        int queue_qty_threshold = 0
    ) const {
        const double reference = reservation_price.has_value() && *reservation_price > 0 ? *reservation_price : microprice;
        double price = direction > 0 ? snapshot.bid : snapshot.ask;
        if (mode == QuoteMode::QueueDefense) {
            const int best_qty = direction > 0 ? snapshot.bid_size : snapshot.ask_size;
            if (best_qty < std::max(queue_qty_threshold, 1)) {
                price = direction > 0 ? std::max(snapshot.bid - tick_size_, tick_size_)
                                      : std::max(snapshot.ask + tick_size_, tick_size_);
            }
        } else if (score >= strong_threshold_ && snapshot.spread() >= 2 * tick_size_) {
            price = direction > 0 ? std::min(snapshot.bid + tick_size_, snapshot.ask - tick_size_)
                                  : std::max(snapshot.ask - tick_size_, snapshot.bid + tick_size_);
        }
        const double aligned = align_order_price_to_tse_tick(price, direction, reservation_price, snapshot);
        const double edge_ticks = direction > 0 ? (reference - aligned) / tick_size_ : (aligned - reference) / tick_size_;
        return PriceDecision{aligned, false, edge_ticks};
    }

    bool open_explicit(
        int direction,
        int qty,
        double price,
        const gateway::BoardSnapshot& snapshot,
        const std::string& reason,
        bool is_market = false,
        const std::string& mode = "EXPLICIT",
        int entry_score = 0
    ) {
        const auto now_ns = event_now_ns(snapshot.ts_ns);
        if (!can_open_entry(direction, qty, now_ns)) {
            return false;
        }
        ++stats["open_attempts"];
        const double order_price = is_market ? 0.0 : align_order_price_to_tse_tick(price, direction, price, snapshot);
        const bool should_fill_immediately = is_market || (direction > 0 && order_price >= snapshot.ask && snapshot.ask > 0) ||
                                             (direction < 0 && order_price <= snapshot.bid && snapshot.bid > 0);
        const std::string order_id =
            dry_run ? next_paper_order_id() : request_live_entry_order(direction, qty, order_price, is_market, now_ns);
        if (order_id.empty()) {
            return false;
        }
        working_order = WorkingOrder{
            order_id, "entry", direction, qty, order_price, is_market, now_ns, reason, mode,
            0, 0.0, false, 0, 0, entry_score, ""
        };
        order_ledger_.add({working_order->order_id, symbol, direction, qty, order_price});
        order_ledger_.mark_working(working_order->order_id);
        ++stats["sent_orders"];

        if (dry_run && should_fill_immediately) {
            working_order->fill_reason = is_market ? "market_cross" : "quote_cross";
            apply_fill(*working_order, qty, direction > 0 ? snapshot.ask : snapshot.bid, now_ns);
            finalize_order(true, "filled");
        } else if (dry_run && queue_model_) {
            working_order->queue_ahead_qty = direction > 0 ? snapshot.bid_size : snapshot.ask_size;
            working_order->initial_queue_ahead_qty = working_order->queue_ahead_qty;
        }
        return true;
    }

    bool open(
        int direction,
        int qty,
        const gateway::BoardSnapshot& snapshot,
        double score,
        double microprice,
        const std::string& reason,
        QuoteMode mode = QuoteMode::PassiveFairValue,
        int entry_score = 0
    ) {
        const auto decision = preview_entry(direction, snapshot, score, microprice, mode);
        if (!decision.is_market && decision.edge_ticks < min_edge_ticks_) {
            return false;
        }
        return open_explicit(direction, qty, decision.price, snapshot, reason, decision.is_market, to_string(mode), entry_score);
    }

    bool close(
        const gateway::BoardSnapshot& snapshot,
        double score,
        const std::string& reason,
        bool force,
        std::optional<double> target_price = std::nullopt
    ) {
        const auto now_ns = event_now_ns(snapshot.ts_ns);
        if (inventory.qty <= 0 || now_ns < exit_blocked_until_ns) {
            return false;
        }
        ++stats["close_attempts"];
        const bool is_market = force && allow_aggressive_exit_;
        const double raw_price =
            is_market ? (inventory.side > 0 ? snapshot.bid : snapshot.ask)
                      : target_price.value_or(inventory.side > 0 ? snapshot.ask : snapshot.bid);
        const double order_price = is_market ? raw_price : align_close_price_to_tse_tick(raw_price, inventory.side, target_price, snapshot);

        if (exit_order.has_value()) {
            const bool same_order =
                exit_order->qty == inventory.qty && exit_order->side == -inventory.side &&
                std::abs(exit_order->price - order_price) <= 1e-9 && exit_order->is_market == is_market;
            if (same_order || exit_order->cancel_requested) {
                return false;
            }
            if (!force && order_age_ns(exit_order, now_ns) < min_order_lifetime_ns_) {
                return false;
            }
            return cancel_exit_order("refresh_" + reason);
        }

        const std::string order_id =
            dry_run ? next_paper_order_id() : request_live_exit_order(inventory.side, inventory.qty, order_price, is_market, now_ns);
        if (order_id.empty()) {
            return false;
        }

        exit_order = WorkingOrder{order_id, "exit", -inventory.side, inventory.qty, order_price, is_market, now_ns, reason};
        order_ledger_.add({exit_order->order_id, symbol, exit_order->side, exit_order->qty, order_price});
        order_ledger_.mark_working(exit_order->order_id);
        ++stats["sent_orders"];

        if (dry_run && is_market) {
            exit_order->fill_reason = "market_cross";
            apply_fill(*exit_order, inventory.qty, inventory.side > 0 ? snapshot.bid : snapshot.ask, now_ns);
            finalize_order(false, "filled");
        }
        (void)score;
        return true;
    }

    bool cancel_working(const std::string& reason) { return cancel_order(true, reason); }
    bool cancel_exit_order(const std::string& reason) { return cancel_order(false, reason); }

    bool check_timeout(std::int64_t now_ns) {
        if (!working_order.has_value() || working_order->purpose != "entry") {
            return false;
        }
        if (std::max<std::int64_t>(0, now_ns - working_order->sent_ts_ns) <= max_pending_ns_) {
            return false;
        }
        return cancel_working("pending_timeout");
    }

    void sync_paper_board(const gateway::BoardSnapshot& snapshot) {
        if (!dry_run) {
            return;
        }
        sync_paper_board_order(working_order, snapshot);
        sync_paper_board_order(exit_order, snapshot);
    }

    void sync_paper_trade(const gateway::TradePrint& trade) {
        if (!dry_run) {
            return;
        }
        sync_paper_trade_order(working_order, trade);
        sync_paper_trade_order(exit_order, trade);
    }

    std::vector<RoundTrip> drain_round_trips() {
        std::vector<RoundTrip> trades(closed_trades.begin(), closed_trades.end());
        closed_trades.clear();
        return trades;
    }

    [[nodiscard]] double get_tse_order_tick(double ref_price) const {
        const double price = std::max(ref_price, tick_size_);
        if (!tse_tick_profile_.has_value() && price > 0.0) {
            const double configured_tick = std::max(tick_size_, 1e-9);
            const double other_tick = tick_from_table(price, false);
            const double topix_tick = tick_from_table(price, true);
            const bool other_match = std::abs(configured_tick - other_tick) <= 1e-9;
            const bool topix_match = std::abs(configured_tick - topix_tick) <= 1e-9;
            if (other_match && !topix_match) {
                tse_tick_profile_ = TseTickProfile::Other;
            } else if (topix_match && !other_match) {
                tse_tick_profile_ = TseTickProfile::Topix100;
            }
        }
        if (tse_tick_profile_.has_value()) {
            return tick_from_table(price, *tse_tick_profile_ == TseTickProfile::Topix100);
        }
        return std::max(tick_size_, 1e-9);
    }

    [[nodiscard]] nlohmann::json snapshot() const {
        const auto* primary_order = working_order.has_value() ? &*working_order : (exit_order.has_value() ? &*exit_order : nullptr);
        return {
            {"state", to_string(state())},
            {"inventory_side", inventory.side},
            {"inventory_qty", inventory.qty},
            {"inventory_price", inventory.avg_price},
            {"broker_hold_qty", broker_hold_qty},
            {"broker_closable_qty", broker_closable_qty},
            {"manual_close_lock", manual_close_lock},
            {"external_inventory", has_external_inventory},
            {"external_active_orders", has_external_active_orders},
            {"has_stranded_partial", has_stranded_partial},
            {"entry_blocked_until_ns", entry_blocked_until_ns},
            {"exit_blocked_until_ns", exit_blocked_until_ns},
            {"working_order_id", current_order_id()},
            {"working_order_side", primary_order != nullptr ? primary_order->side : 0},
            {"working_order_price", primary_order != nullptr ? primary_order->price : 0.0},
            {"working_order_mode", primary_order != nullptr ? primary_order->mode : ""},
            {"working_order_entry_score", primary_order != nullptr ? primary_order->entry_score : 0},
            {"working_order_fill_reason", primary_order != nullptr ? primary_order->fill_reason : ""},
            {"working_order_queue_ahead_qty", primary_order != nullptr ? primary_order->queue_ahead_qty : 0},
            {"working_order_initial_queue_ahead_qty", primary_order != nullptr ? primary_order->initial_queue_ahead_qty : 0},
            {"entry_order_id", working_order.has_value() ? working_order->order_id : ""},
            {"exit_order_id", exit_order.has_value() ? exit_order->order_id : ""},
            {"inventory_entry_mode", inventory.entry_mode},
            {"inventory_entry_score", inventory.entry_score},
            {"inventory_fill_reason", inventory.entry_fill_reason},
            {"inventory_entry_queue_ahead_qty", inventory.entry_queue_ahead_qty},
            {"paper_last_fill_reason", paper_last_fill_reason},
            {"active_order_ids", active_order_ids()},
            {"broker_active_order_ids", broker_active_order_ids},
            {"stats", stats},
            {"ledger", order_ledger_.snapshot()},
        };
    }

    static double incremental_fill_price(int prev_qty, double prev_avg, int new_qty, double new_avg) {
        const int incremental_qty = std::max(new_qty - prev_qty, 0);
        if (incremental_qty == 0 || prev_qty <= 0 || prev_avg <= 0) {
            return new_avg;
        }
        return std::max(new_qty * new_avg - prev_qty * prev_avg, 0.0) / incremental_qty;
    }

    void apply_broker_snapshot(const gateway::OrderSnapshot& snapshot) {
        auto* slot = order_slot(snapshot.order_id);
        if (slot == nullptr || !slot->has_value()) {
            return;
        }

        auto& order = **slot;
        const int new_qty = std::max(snapshot.cum_qty - order.cum_qty, 0);
        if (new_qty > 0) {
            if (order.fill_reason.empty()) {
                order.fill_reason = "broker_snapshot_fill";
            }
            const double fill_price = incremental_fill_price(
                order.cum_qty,
                order.avg_fill_price,
                snapshot.cum_qty,
                snapshot.avg_fill_price > 0 ? snapshot.avg_fill_price : snapshot.price
            );
            const auto fill_ts_ns = snapshot.fill_ts_ns > 0 ? snapshot.fill_ts_ns : wall_clock_ns();
            apply_fill(order, new_qty, fill_price, fill_ts_ns);
            order.cum_qty = snapshot.cum_qty;
            order.avg_fill_price = snapshot.avg_fill_price > 0 ? snapshot.avg_fill_price : fill_price;
        }

        if (snapshot.is_final) {
            finalize_order(order.purpose == "entry", snapshot.status());
        }
    }

    void sync_broker_position_snapshot(const std::vector<nlohmann::json>& positions, bool force = true) {
        (void)force;
        int long_hold = 0;
        int long_closable = 0;
        int short_hold = 0;
        int short_closable = 0;
        for (const auto& raw : positions) {
            const auto lot = gateway::KabuAdapter::position_lot(raw);
            if (!lot.has_value() || lot->symbol != symbol) {
                continue;
            }
            if (lot->side > 0) {
                long_hold += lot->qty;
                long_closable += std::max(lot->closable_qty, 0);
            } else if (lot->side < 0) {
                short_hold += lot->qty;
                short_closable += std::max(lot->closable_qty, 0);
            }
        }

        if (inventory.side > 0) {
            broker_hold_qty = long_hold;
            broker_closable_qty = long_closable;
        } else if (inventory.side < 0) {
            broker_hold_qty = short_hold;
            broker_closable_qty = short_closable;
        } else {
            broker_hold_qty = long_hold + short_hold;
            broker_closable_qty = long_closable + short_closable;
        }

        const auto now_ns = wall_clock_ns();
        if (inventory.qty == 0) {
            has_external_inventory = broker_hold_qty > 0;
            manual_close_lock = false;
            if (!has_external_inventory) {
                exit_blocked_until_ns = 0;
            }
            return;
        }

        if (!has_working_orders() && broker_hold_qty == 0) {
            inventory = Inventory{};
            has_external_inventory = false;
            manual_close_lock = false;
            exit_blocked_until_ns = 0;
            broker_hold_qty = 0;
            broker_closable_qty = 0;
            has_stranded_partial = false;
            return;
        }

        bool qty_mismatch = broker_hold_qty != inventory.qty;
        if (qty_mismatch && has_working_orders()) {
            int working_remaining = 0;
            if (working_order.has_value()) {
                working_remaining += std::max(working_order->qty - working_order->cum_qty, 0);
            }
            if (exit_order.has_value()) {
                working_remaining += std::max(exit_order->qty - exit_order->cum_qty, 0);
            }
            if (std::abs(broker_hold_qty - inventory.qty) <= working_remaining) {
                qty_mismatch = false;
            }
        }

        const bool local_exit_manageable = broker_closable_qty >= inventory.qty;
        if (qty_mismatch) {
            has_external_inventory = true;
            if (local_exit_manageable) {
                exit_blocked_until_ns = 0;
            } else {
                exit_blocked_until_ns = std::max(exit_blocked_until_ns, now_ns + 15'000'000'000LL);
            }
        } else {
            has_external_inventory = false;
        }

        const bool locked = broker_hold_qty > 0 && broker_closable_qty < std::min(broker_hold_qty, inventory.qty);
        const bool expected_strategy_lock = locked && has_working_exit();
        if (!expected_strategy_lock) {
            manual_close_lock = locked;
        } else {
            manual_close_lock = false;
        }
        if (locked && !expected_strategy_lock) {
            exit_blocked_until_ns = std::max(exit_blocked_until_ns, now_ns + 15'000'000'000LL);
        } else if (!has_external_inventory || local_exit_manageable) {
            exit_blocked_until_ns = 0;
        }
    }

    void sync_external_order_snapshots(const std::map<std::string, gateway::OrderSnapshot>& snapshots) {
        std::vector<std::string> active_ids;
        const auto local_ids = active_order_ids();
        const std::set<std::string> local_id_set(local_ids.begin(), local_ids.end());
        for (const auto& [order_id, snapshot] : snapshots) {
            const std::string snapshot_symbol =
                snapshot.symbol.empty() ? gateway::parse_string(snapshot.raw.value("Symbol", nlohmann::json()), std::string())
                                        : snapshot.symbol;
            if (snapshot_symbol != symbol || snapshot.is_final || local_id_set.contains(order_id)) {
                continue;
            }
            active_ids.push_back(order_id);
        }
        broker_active_order_ids = std::move(active_ids);
        has_external_active_orders = !broker_active_order_ids.empty();
    }

  private:
    [[nodiscard]] std::optional<WorkingOrder>* order_slot(const std::string& order_id) {
        if (working_order.has_value() && working_order->order_id == order_id) {
            return &working_order;
        }
        if (exit_order.has_value() && exit_order->order_id == order_id) {
            return &exit_order;
        }
        return nullptr;
    }

    [[nodiscard]] std::string request_live_entry_order(int side, int qty, double price, bool is_market, std::int64_t now_ns) {
        if (!entry_order_sender_) {
            throw std::runtime_error("live entry order sender has not been configured");
        }
        try {
            return entry_order_sender_(side, qty, price, is_market);
        } catch (const gateway::KabuApiError& error) {
            const auto code = extract_error_code(error.payload());
            if (code.has_value() && *code == 4002004) {
                entry_blocked_until_ns = std::max(entry_blocked_until_ns, now_ns + 15'000'000'000LL);
                return {};
            }
            if (code.has_value() && *code == 100313) {
                entry_blocked_until_ns = std::max(entry_blocked_until_ns, now_ns + 1'000'000'000LL);
                return {};
            }
            throw;
        }
    }

    [[nodiscard]] std::string request_live_exit_order(
        int position_side,
        int qty,
        double price,
        bool is_market,
        std::int64_t now_ns
    ) {
        if (!exit_order_sender_) {
            throw std::runtime_error("live exit order sender has not been configured");
        }
        try {
            return exit_order_sender_(position_side, qty, price, is_market);
        } catch (const gateway::KabuApiError& error) {
            const auto code = extract_error_code(error.payload());
            if (code.has_value() && *code == 8) {
                exit_blocked_until_ns = std::max(exit_blocked_until_ns, now_ns + 15'000'000'000LL);
                return {};
            }
            throw;
        }
    }

    [[nodiscard]] static double tick_from_table(double reference, bool topix100) {
        const double price = std::max(reference, 0.0);
        if (topix100) {
            if (price <= 1000.0) return 0.1;
            if (price <= 3000.0) return 0.5;
            if (price <= 5000.0) return 1.0;
            if (price <= 10000.0) return 1.0;
            if (price <= 30000.0) return 5.0;
            if (price <= 50000.0) return 10.0;
            if (price <= 100000.0) return 10.0;
            if (price <= 300000.0) return 50.0;
            if (price <= 500000.0) return 100.0;
            if (price <= 1000000.0) return 100.0;
            if (price <= 3000000.0) return 500.0;
            if (price <= 10000000.0) return 1000.0;
            if (price <= 30000000.0) return 5000.0;
            return 10000.0;
        }
        if (price <= 3000.0) return 1.0;
        if (price <= 5000.0) return 5.0;
        if (price <= 30000.0) return 10.0;
        if (price <= 50000.0) return 50.0;
        if (price <= 300000.0) return 100.0;
        if (price <= 500000.0) return 500.0;
        if (price <= 3000000.0) return 1000.0;
        if (price <= 5000000.0) return 5000.0;
        if (price <= 30000000.0) return 10000.0;
        if (price <= 50000000.0) return 50000.0;
        return 100000.0;
    }

    [[nodiscard]] static std::int64_t wall_clock_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
               )
            .count();
    }

    [[nodiscard]] static std::int64_t order_age_ns(const std::optional<WorkingOrder>& order, std::int64_t now_ns) {
        return order.has_value() ? std::max<std::int64_t>(0, now_ns - order->sent_ts_ns) : 0;
    }

    [[nodiscard]] std::int64_t event_now_ns(std::int64_t hinted) const { return hinted > 0 ? hinted : wall_clock_ns(); }

    [[nodiscard]] bool can_open_entry(int direction, int qty, std::int64_t now_ns) const {
        if (working_order.has_value() || qty <= 0 || now_ns < entry_blocked_until_ns) {
            return false;
        }
        return inventory.qty <= 0 || inventory.side == 0 || inventory.side == direction;
    }

  public:
    [[nodiscard]] double align_order_price_to_tse_tick(
        double raw_price,
        int side,
        std::optional<double> reference_price,
        const gateway::BoardSnapshot& snapshot
    ) const {
        if (raw_price <= 0) {
            return 0.0;
        }
        const double ref = reference_price.value_or(side > 0 ? snapshot.ask : snapshot.bid);
        const double tick = get_tse_order_tick(ref);
        const double steps = raw_price / tick;
        const double snapped = side > 0 ? std::ceil(steps - 1e-9) : std::floor(steps + 1e-9);
        return std::round(std::max(snapped * tick, tick) * 1e10) / 1e10;
    }

    [[nodiscard]] double align_close_price_to_tse_tick(
        double raw_price,
        int position_side,
        std::optional<double> reference_price,
        const gateway::BoardSnapshot& snapshot
    ) const {
        if (raw_price <= 0) {
            return 0.0;
        }
        const double ref = reference_price.value_or(position_side > 0 ? snapshot.bid : snapshot.ask);
        const double tick = get_tse_order_tick(ref);
        const double steps = raw_price / tick;
        const double snapped = position_side > 0 ? std::ceil(steps - 1e-9) : std::floor(steps + 1e-9);
        return std::round(std::max(snapped * tick, tick) * 1e10) / 1e10;
    }

  private:
    bool cancel_order(bool entry_side, const std::string& reason) {
        auto& slot = entry_side ? working_order : exit_order;
        if (!slot.has_value() || slot->cancel_requested) {
            return false;
        }
        slot->cancel_requested = true;
        paper_last_fill_reason = reason;
        ++stats["cancel_orders"];
        if (dry_run) {
            finalize_order(entry_side, "cancelled");
        } else {
            if (!cancel_order_sender_) {
                slot->cancel_requested = false;
                throw std::runtime_error("live cancel order sender has not been configured");
            }
            try {
                cancel_order_sender_(slot->order_id);
            } catch (const gateway::KabuApiError& error) {
                const auto code = extract_error_code(error.payload());
                if (!(error.status() == 500 && code.has_value() && *code == 43)) {
                    slot->cancel_requested = false;
                    throw;
                }
            }
        }
        return true;
    }

    void sync_paper_board_order(std::optional<WorkingOrder>& order, const gateway::BoardSnapshot& snapshot) {
        if (!order.has_value() || order->is_market) {
            return;
        }
        if (order->side > 0 && snapshot.ask <= order->price) {
            if (queue_model_ && order->queue_ahead_qty > 0) {
                order->queue_ahead_qty = 0;
            } else {
                paper_fill(order, std::min(order->price, snapshot.ask), "quote_cross", snapshot.ts_ns);
            }
        } else if (order->side < 0 && snapshot.bid >= order->price) {
            if (queue_model_ && order->queue_ahead_qty > 0) {
                order->queue_ahead_qty = 0;
            } else {
                paper_fill(order, std::max(order->price, snapshot.bid), "quote_cross", snapshot.ts_ns);
            }
        }
    }

    void sync_paper_trade_order(std::optional<WorkingOrder>& order, const gateway::TradePrint& trade) {
        if (!order.has_value() || order->is_market) {
            return;
        }
        const bool buy_fill = order->side > 0 && trade.price <= order->price;
        const bool sell_fill = order->side < 0 && trade.price >= order->price;
        if (!buy_fill && !sell_fill) {
            return;
        }
        if (queue_model_) {
            order->queue_ahead_qty = std::max(0, order->queue_ahead_qty - trade.size);
            if (order->queue_ahead_qty > 0) {
                return;
            }
        }
        paper_fill(order, buy_fill ? std::min(order->price, trade.price) : std::max(order->price, trade.price), "trade_through", trade.ts_ns);
    }

    void paper_fill(std::optional<WorkingOrder>& order, double limit_price, const std::string& reason, std::int64_t ts_hint_ns) {
        if (!order.has_value()) {
            return;
        }
        paper_last_fill_reason = reason;
        order->fill_reason = reason;
        apply_fill(*order, order->qty - order->cum_qty, limit_price, event_now_ns(ts_hint_ns));
        finalize_order(order->purpose == "entry", "filled");
    }

    void apply_fill(WorkingOrder& order, int qty, double fill_price, std::int64_t fill_ts_ns) {
        if (qty <= 0) {
            return;
        }
        order_ledger_.apply_fill(order.order_id, qty, fill_price);
        const int prev_filled_qty = order.cum_qty;
        order.cum_qty += qty;
        order.avg_fill_price = prev_filled_qty > 0 ? ((order.avg_fill_price * prev_filled_qty) + (fill_price * qty)) / order.cum_qty
                                                   : fill_price;
        ++stats["fills"];

        if (order.purpose == "entry") {
            const int prev_qty = inventory.qty;
            const int new_qty = prev_qty + qty;
            inventory.avg_price = new_qty > 0 ? ((inventory.avg_price * prev_qty) + (fill_price * qty)) / new_qty : 0.0;
            inventory.qty = new_qty;
            inventory.side = order.side;
            inventory.opened_ts_ns = fill_ts_ns;
            inventory.entry_qty += qty;
            update_inventory_entry_metadata(order);
            return;
        }

        inventory.exit_qty += qty;
        inventory.exit_value += fill_price * qty;
        inventory.qty = std::max(0, inventory.qty - qty);
        if (inventory.qty == 0) {
            const double exit_price = inventory.exit_value / std::max(inventory.exit_qty, 1);
            closed_trades.push_back(RoundTrip{
                symbol,
                inventory.side,
                inventory.exit_qty,
                inventory.avg_price,
                exit_price,
                inventory.opened_ts_ns,
                fill_ts_ns,
                inventory.side * (exit_price - inventory.avg_price) * inventory.exit_qty,
                order.reason,
                inventory.entry_mode,
                inventory.entry_score,
                inventory.entry_fill_reason,
                inventory.entry_queue_ahead_qty,
            });
        }
    }

    void update_inventory_entry_metadata(const WorkingOrder& order) {
        if (inventory.entry_mode.empty()) {
            inventory.entry_mode = order.mode;
        } else if (!order.mode.empty() && inventory.entry_mode != order.mode) {
            inventory.entry_mode = "mixed";
        }
        inventory.entry_score = std::max(inventory.entry_score, order.entry_score);
        if (inventory.entry_fill_reason.empty()) {
            inventory.entry_fill_reason = order.fill_reason;
        } else if (!order.fill_reason.empty() && inventory.entry_fill_reason != order.fill_reason) {
            inventory.entry_fill_reason = "mixed";
        }
        inventory.entry_queue_ahead_qty =
            std::max(inventory.entry_queue_ahead_qty, std::max(order.initial_queue_ahead_qty, order.queue_ahead_qty));
    }

    void finalize_order(bool entry_side, const std::string& final_status) {
        auto& slot = entry_side ? working_order : exit_order;
        if (!slot.has_value()) {
            return;
        }
        if (final_status == "filled") {
            order_ledger_.mark_filled(slot->order_id);
        } else if (final_status == "cancelled") {
            order_ledger_.mark_canceled(slot->order_id);
            if (entry_side && inventory.qty > 0 && slot->cum_qty > 0) {
                has_stranded_partial = true;
            }
        } else if (final_status == "rejected") {
            order_ledger_.mark_rejected(slot->order_id);
        }
        slot.reset();
        if (!entry_side && inventory.qty == 0) {
            inventory = Inventory{};
            has_stranded_partial = false;
        }
    }

    [[nodiscard]] std::string next_paper_order_id() { return "PAPER-" + symbol + "-" + std::to_string(++paper_order_counter_); }

    double tick_size_{1e-9};
    double strong_threshold_{0.0};
    double min_edge_ticks_{0.0};
    std::int64_t max_pending_ns_{0};
    std::int64_t min_order_lifetime_ns_{0};
    bool allow_aggressive_exit_{false};
    bool topix100_{false};
    bool queue_model_{true};
    mutable std::optional<TseTickProfile> tse_tick_profile_;
    int paper_order_counter_{0};
    oms::OrderLedger order_ledger_;
    EntryOrderSender entry_order_sender_;
    ExitOrderSender exit_order_sender_;
    CancelOrderSender cancel_order_sender_;
};

}  // namespace kabu::execution
