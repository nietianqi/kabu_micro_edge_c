#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/gateway.hpp"
#include "kabu_micro_edge/risk.hpp"
#include "kabu_micro_edge/strategy.hpp"

namespace kabu::app {

struct ReconcilePlan {
    std::string mode;
    double sleep_s{0.0};
    bool poll_positions{true};
    std::vector<std::string> order_ids;
};

inline void to_json(nlohmann::json& json, const ReconcilePlan& value) {
    json = {
        {"mode", value.mode},
        {"sleep_s", value.sleep_s},
        {"poll_positions", value.poll_positions},
        {"order_ids", value.order_ids},
    };
}

inline void from_json(const nlohmann::json& json, ReconcilePlan& value) {
    value.mode = json.value("mode", std::string{});
    value.sleep_s = json.value("sleep_s", 0.0);
    value.poll_positions = json.value("poll_positions", true);
    value.order_ids = json.value("order_ids", std::vector<std::string>{});
}

struct KillSwitchRequest {
    std::string reason;
    bool hard_close{true};
};

inline void to_json(nlohmann::json& json, const KillSwitchRequest& value) {
    json = {
        {"reason", value.reason},
        {"hard_close", value.hard_close},
    };
}

inline void from_json(const nlohmann::json& json, KillSwitchRequest& value) {
    value.reason = json.value("reason", std::string{});
    value.hard_close = json.value("hard_close", true);
}

struct StrategyRuntime {
    config::SymbolConfig symbol;
    std::shared_ptr<strategy::MicroEdgeStrategy> strategy;
    double next_reconcile_at_monotonic{0.0};
};

struct ReconcileFetchResult {
    std::optional<std::vector<nlohmann::json>> positions;
    std::optional<std::map<std::string, gateway::OrderSnapshot>> order_snapshots;
};

enum class RecoveryState {
    Ready,
    Startup,
    Reconnect,
};

inline std::string to_string(RecoveryState state) {
    switch (state) {
    case RecoveryState::Ready:
        return "ready";
    case RecoveryState::Startup:
        return "startup_recovery";
    case RecoveryState::Reconnect:
        return "reconnect_recovery";
    }
    return "ready";
}

class MicroEdgeApp {
  public:
    static constexpr int RECONCILE_BACKLOG_THRESHOLD = 32;

    explicit MicroEdgeApp(config::AppConfig config)
        : config_(std::move(config)),
          rest_(
              config_.base_url,
              config_.rate_limit_per_second,
              config_.order_rate_limit_per_second,
              config_.poll_rate_limit_per_second
          ),
          account_risk_(config_.account_risk) {}

    void set_strategy(std::shared_ptr<strategy::MicroEdgeStrategy> strategy_ptr) {
        register_strategy(config_.symbol(), std::move(strategy_ptr));
    }

    void register_strategy(const config::SymbolConfig& symbol, std::shared_ptr<strategy::MicroEdgeStrategy> strategy_ptr) {
        StrategyRuntime runtime{symbol, std::move(strategy_ptr)};
        strategy_runtimes_[symbol.key()] = runtime;
        for (const auto& key : symbol.stream_keys()) {
            strategy_routes_[key] = symbol.key();
        }
    }

    void on_board(const gateway::BoardSnapshot& snapshot) {
        last_market_event_receive_ts_ns_ = wall_clock_ns();
        last_market_data_ts_ns_ = std::max(last_market_data_ts_ns_, snapshot.ts_ns);
        symbol_last_board_ts_ns_[snapshot.symbol + ":" + std::to_string(snapshot.exchange)] = snapshot.ts_ns;
        if (auto* runtime = resolve_runtime(snapshot.symbol, snapshot.exchange); runtime != nullptr) {
            runtime->strategy->on_board(snapshot);
        }
    }

    void on_trade(const gateway::TradePrint& trade) {
        last_market_event_receive_ts_ns_ = wall_clock_ns();
        last_market_data_ts_ns_ = std::max(last_market_data_ts_ns_, trade.ts_ns);
        symbol_last_trade_ts_ns_[trade.symbol + ":" + std::to_string(trade.exchange)] = trade.ts_ns;
        if (auto* runtime = resolve_runtime(trade.symbol, trade.exchange); runtime != nullptr) {
            runtime->strategy->on_trade(trade);
        }
    }

    void activate_kill_switch(const std::string& reason, bool hard_close = true) {
        kill_switch_active_ = true;
        kill_switch_reason_ = reason.empty() ? "manual_kill_switch" : reason;
        kill_switch_hard_close_ = kill_switch_hard_close_ || hard_close;
        for (auto& [_, runtime] : strategy_runtimes_) {
            runtime.strategy->activate_kill_switch(kill_switch_reason_, kill_switch_hard_close_);
        }
    }

    void set_rest_request_executor(gateway::KabuRestClient::RequestExecutor executor) {
        rest_.set_request_executor(std::move(executor));
    }

    [[nodiscard]] gateway::KabuRestClient& rest() { return rest_; }
    [[nodiscard]] const gateway::KabuRestClient& rest() const { return rest_; }

    [[nodiscard]] std::vector<nlohmann::json> build_register_payload() const {
        std::vector<nlohmann::json> payload;
        payload.reserve(config_.symbols.size());
        for (const auto& symbol : config_.symbols) {
            payload.push_back({
                {"Symbol", symbol.symbol},
                {"Exchange", symbol.register_exchange()},
            });
        }
        return payload;
    }

    nlohmann::json register_symbols() {
        const auto response = rest_.register_symbols(build_register_payload());
        last_register_success_ts_ns_ = wall_clock_ns();
        return response;
    }

    void startup_with_retry(const std::function<void(double)>& sleeper = {}) {
        const int max_retries = std::max(config_.startup_retry_count, 0);
        const double delay_s = std::max(config_.startup_retry_delay_s, 0.0);
        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                rest_.get_token(config_.api_password);
                break;
            } catch (...) {
                if (attempt >= max_retries) {
                    throw;
                }
                if (sleeper) {
                    sleeper(delay_s);
                }
            }
        }
        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                register_symbols();
                break;
            } catch (...) {
                note_rest_error("startup register_symbols failed");
                if (attempt >= max_retries) {
                    throw;
                }
                if (sleeper) {
                    sleeper(delay_s);
                }
            }
        }
    }

    void reregister_symbols() {
        try {
            register_symbols();
        } catch (const gateway::KabuApiError& error) {
            if (error.status() != 401 && error.status() != 403) {
                note_rest_error(error.what());
                throw;
            }
            rest_.get_token(config_.api_password);
            ++token_refresh_count_;
            register_symbols();
        }
    }

    template <typename Fn>
    decltype(auto) with_authorization_retry(Fn&& fn) {
        try {
            return fn();
        } catch (const gateway::KabuApiError& error) {
            if (error.status() != 401 && error.status() != 403) {
                note_rest_error(error.what());
                throw;
            }
            rest_.get_token(config_.api_password);
            ++token_refresh_count_;
            register_symbols();
            return fn();
        }
    }

    void set_websocket_status_provider(std::function<nlohmann::json()> provider) {
        websocket_status_provider_ = std::move(provider);
    }

    void note_rest_error(std::string message) { last_rest_error_ = std::move(message); }
    void note_websocket_error(std::string message) { last_websocket_error_ = std::move(message); }
    void note_reconcile_failure(std::string message) {
        ++reconcile_failure_count_;
        last_reconcile_error_ = std::move(message);
    }
    void note_reconcile_success() { last_reconcile_error_.clear(); }
    void begin_startup_recovery(std::string reason = "startup_recovery") {
        set_recovery_state_impl(RecoveryState::Startup, std::move(reason));
    }
    void begin_reconnect_recovery(std::string reason = "websocket_reconnect_recovery") {
        set_recovery_state_impl(RecoveryState::Reconnect, std::move(reason));
    }
    void finish_recovery() {
        recovery_state_ = RecoveryState::Ready;
        recovery_in_progress_ = false;
        recovery_reason_.clear();
        recovery_completed_ts_ns_ = wall_clock_ns();
    }
    void set_recovery_state(bool in_progress, std::string reason = {}) {
        if (!in_progress) {
            finish_recovery();
            return;
        }
        if (reason == "websocket_reconnect_recovery") {
            begin_reconnect_recovery(reason);
            return;
        }
        begin_startup_recovery(reason.empty() ? "recovery_in_progress" : std::move(reason));
    }
    [[nodiscard]] bool recovery_in_progress() const { return recovery_in_progress_; }
    [[nodiscard]] const std::string& recovery_reason() const { return recovery_reason_; }
    [[nodiscard]] RecoveryState recovery_state() const { return recovery_state_; }

    [[nodiscard]] ReconcilePlan build_reconcile_plan(strategy::MicroEdgeStrategy& strategy_obj) const {
        const auto base_sleep_s = base_reconcile_interval_s();
        const auto idle_sleep_s = std::max(base_sleep_s * 4.0, 2.0);
        const auto& execution = strategy_obj.execution();
        const auto active_order_ids = execution.active_order_ids();
        std::set<std::string> order_id_set(active_order_ids.begin(), active_order_ids.end());
        order_id_set.insert(execution.broker_active_order_ids.begin(), execution.broker_active_order_ids.end());
        const std::vector<std::string> reconcile_order_ids(order_id_set.begin(), order_id_set.end());
        const bool needs_positions = execution.inventory.qty > 0 || execution.has_external_inventory || execution.manual_close_lock ||
                                     execution.has_stranded_partial;
        if (execution.has_external_inventory_conflict() || execution.has_external_inventory) {
            return {"drift", base_sleep_s, needs_positions, reconcile_order_ids};
        }
        if (!reconcile_order_ids.empty()) {
            return {"orders", base_sleep_s, needs_positions, reconcile_order_ids};
        }
        if (execution.inventory.qty > 0) {
            return {"inventory", base_sleep_s, true, {}};
        }
        return {"idle", idle_sleep_s, false, {}};
    }

    [[nodiscard]] bool should_fast_track_reconcile(const ReconcilePlan& plan, strategy::MicroEdgeStrategy& strategy_obj) const {
        return plan.mode != "idle" || has_abnormal_event_backlog(strategy_obj);
    }

    [[nodiscard]] bool has_abnormal_event_backlog(strategy::MicroEdgeStrategy& strategy_obj) const {
        const auto runtime = strategy_obj.runtime_metrics();
        const int backlog = runtime.event_backlog;
        const double queue_wait_p99_ms = runtime.queue_wait_p99_ms;
        return backlog >= RECONCILE_BACKLOG_THRESHOLD || queue_wait_p99_ms >= base_reconcile_interval_s() * 1000.0;
    }

    [[nodiscard]] risk::AccountRiskSnapshot account_risk_snapshot(
        int additional_qty = 0,
        double additional_price = 0.0
    ) {
        return account_risk_.evaluate(account_exposures(), account_realized_pnl(), additional_qty, additional_price);
    }

    [[nodiscard]] std::pair<bool, std::string> can_enter_account(int additional_qty = 0, double additional_price = 0.0) {
        if (recovery_in_progress_) {
            return {false, recovery_reason_.empty() ? "recovery_in_progress" : recovery_reason_};
        }
        const auto snapshot = account_risk_snapshot(additional_qty, additional_price);
        return {!snapshot.entry_blocked, snapshot.block_reason};
    }

    [[nodiscard]] std::function<std::pair<bool, std::string>(int, double)> make_account_entry_guard() {
        return [this](int additional_qty, double additional_price) {
            return this->can_enter_account(additional_qty, additional_price);
        };
    }

    [[nodiscard]] std::optional<KillSwitchRequest> read_kill_switch_request() const {
        const auto raw_path = std::filesystem::path(config_.kill_switch_path);
        if (raw_path.empty() || !std::filesystem::exists(raw_path)) {
            return std::nullopt;
        }
        std::ifstream in(raw_path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.empty()) {
            return KillSwitchRequest{"kill_switch_file:" + raw_path.filename().string(), true};
        }
        try {
            const auto payload = nlohmann::json::parse(text);
            if (!payload.is_object()) {
                return KillSwitchRequest{"kill_switch_file:" + raw_path.filename().string(), true};
            }
            if (payload.contains("active") && !payload.at("active").get<bool>()) {
                return std::nullopt;
            }
            const std::string mode = payload.value("mode", std::string("hard"));
            bool hard_close = mode != "soft";
            if (payload.contains("hard_close")) {
                hard_close = payload.at("hard_close").get<bool>();
            }
            return KillSwitchRequest{payload.value("reason", std::string("kill_switch_file:" + raw_path.filename().string())), hard_close};
        } catch (...) {
            return KillSwitchRequest{text.size() <= 120 ? text : text.substr(0, 117) + "...", true};
        }
    }

    bool poll_kill_switch() {
        const auto request = read_kill_switch_request();
        if (!request.has_value()) {
            return false;
        }
        if (kill_switch_active_ && (kill_switch_hard_close_ || !request->hard_close)) {
            return false;
        }
        activate_kill_switch(request->reason, request->hard_close);
        return true;
    }

    [[nodiscard]] std::optional<std::map<std::string, gateway::OrderSnapshot>> collect_active_order_snapshots(
        const std::vector<std::string>& order_ids
    ) {
        if (order_ids.empty()) {
            return std::nullopt;
        }

        std::set<std::string> requested_ids;
        for (const auto& order_id : order_ids) {
            if (!order_id.empty()) {
                requested_ids.insert(order_id);
            }
        }
        if (requested_ids.empty()) {
            return std::nullopt;
        }

        std::map<std::string, gateway::OrderSnapshot> snapshots;
        for (const auto& order_id : requested_ids) {
            std::vector<nlohmann::json> raws;
            try {
                raws = with_authorization_retry(
                    [&]() { return rest_.get_orders(order_id, 0, gateway::RequestLane::Poll); }
                );
            } catch (...) {
                return std::nullopt;
            }
            merge_order_snapshots(snapshots, raws, requested_ids);
        }
        return snapshots.empty() ? std::nullopt : std::optional<std::map<std::string, gateway::OrderSnapshot>>(snapshots);
    }

    [[nodiscard]] ReconcileFetchResult collect_startup_recovery_inputs() {
        const auto positions = with_authorization_retry(
            [&]() { return rest_.get_positions(std::nullopt, 0, gateway::RequestLane::Poll); }
        );
        const auto orders = with_authorization_retry(
            [&]() { return rest_.get_orders(std::nullopt, 0, gateway::RequestLane::Poll); }
        );

        std::map<std::string, gateway::OrderSnapshot> snapshots;
        merge_order_snapshots(snapshots, orders);
        return {
            positions,
            snapshots.empty() ? std::optional<std::map<std::string, gateway::OrderSnapshot>>(std::nullopt)
                              : std::optional<std::map<std::string, gateway::OrderSnapshot>>(snapshots),
        };
    }

    [[nodiscard]] ReconcileFetchResult collect_reconcile_inputs(const std::vector<ReconcilePlan>& plans) {
        ReconcileFetchResult result;
        bool poll_positions = false;
        std::set<std::string> order_ids;
        for (const auto& plan : plans) {
            poll_positions = poll_positions || plan.poll_positions;
            order_ids.insert(plan.order_ids.begin(), plan.order_ids.end());
        }

        if (poll_positions) {
            result.positions = with_authorization_retry(
                [&]() { return rest_.get_positions(std::nullopt, 0, gateway::RequestLane::Poll); }
            );
        }
        if (!order_ids.empty()) {
            result.order_snapshots = collect_active_order_snapshots(std::vector<std::string>(order_ids.begin(), order_ids.end()));
        }
        return result;
    }

    [[nodiscard]] nlohmann::json status_snapshot() {
        nlohmann::json strategies = nlohmann::json::array();
        int total_consistency_issue_count = 0;
        for (const auto& [_, runtime] : strategy_runtimes_) {
            auto status = runtime.strategy->status();
            total_consistency_issue_count += status
                                                 .at("execution")
                                                 .value("consistency_issue_count", 0);
            strategies.push_back(std::move(status));
        }
        nlohmann::json market_data = {
            {"last_market_data_ts_ns", last_market_data_ts_ns_},
            {"last_market_event_receive_ts_ns", last_market_event_receive_ts_ns_},
            {"per_symbol_last_board_ts_ns", symbol_last_board_ts_ns_},
            {"per_symbol_last_trade_ts_ns", symbol_last_trade_ts_ns_},
        };
        return {
            {"running", running_},
            {"kill_switch_active", kill_switch_active_},
            {"kill_switch_reason", kill_switch_reason_},
            {"kill_switch_hard_close", kill_switch_hard_close_},
            {"symbol_count", strategy_runtimes_.size()},
            {"recovery_in_progress", recovery_in_progress_},
            {"recovery_state", to_string(recovery_state_)},
            {"recovery_reason", recovery_reason_},
            {"recovery_started_ts_ns", recovery_started_ts_ns_},
            {"recovery_completed_ts_ns", recovery_completed_ts_ns_},
            {"websocket", websocket_status_provider_ ? websocket_status_provider_() : nlohmann::json{{"status", "unconfigured"}}},
            {"market_data", market_data},
            {"token_refresh_count", token_refresh_count_},
            {"last_register_success_ts_ns", last_register_success_ts_ns_},
            {"last_rest_error", last_rest_error_},
            {"last_websocket_error", last_websocket_error_},
            {"reconcile_failure_count", reconcile_failure_count_},
            {"last_reconcile_error", last_reconcile_error_},
            {"consistency_ok", total_consistency_issue_count == 0},
            {"consistency_issue_count", total_consistency_issue_count},
            {"account_risk", account_risk_snapshot()},
            {"strategies", strategies},
        };
    }

    void set_running(bool running) { running_ = running; }

    [[nodiscard]] const config::AppConfig& config() const { return config_; }
    [[nodiscard]] std::map<std::pair<std::string, int>, StrategyRuntime>& strategy_runtimes() { return strategy_runtimes_; }
    [[nodiscard]] const std::map<std::pair<std::string, int>, StrategyRuntime>& strategy_runtimes() const { return strategy_runtimes_; }
    [[nodiscard]] bool kill_switch_active() const { return kill_switch_active_; }
    [[nodiscard]] const std::string& kill_switch_reason() const { return kill_switch_reason_; }
    [[nodiscard]] bool kill_switch_hard_close() const { return kill_switch_hard_close_; }

  private:
    void set_recovery_state_impl(RecoveryState state, std::string reason) {
        recovery_state_ = state;
        recovery_in_progress_ = state != RecoveryState::Ready;
        recovery_reason_ = recovery_in_progress_ ? std::move(reason) : std::string{};
        recovery_started_ts_ns_ = wall_clock_ns();
    }

    static void merge_order_snapshots(
        std::map<std::string, gateway::OrderSnapshot>& snapshots,
        const std::vector<nlohmann::json>& raws,
        const std::set<std::string>& requested_ids = {}
    ) {
        for (const auto& raw : raws) {
            const auto snapshot = gateway::KabuAdapter::order_snapshot(raw);
            if (!snapshot.has_value()) {
                continue;
            }
            if (!requested_ids.empty() && !requested_ids.contains(snapshot->order_id)) {
                continue;
            }
            snapshots[snapshot->order_id] = *snapshot;
        }
    }

    [[nodiscard]] static std::int64_t wall_clock_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
               )
            .count();
    }

    [[nodiscard]] double base_reconcile_interval_s() const {
        return std::max(config_.reconcile_interval_ms / 1000.0, 0.1);
    }

    [[nodiscard]] std::vector<risk::AccountExposure> account_exposures() const {
        std::vector<risk::AccountExposure> exposures;
        for (const auto& [_, runtime] : strategy_runtimes_) {
            const auto& execution = runtime.strategy->execution();
            int pending_entry_qty = 0;
            double pending_entry_price = 0.0;
            if (execution.has_working_entry() && execution.working_order.has_value()) {
                pending_entry_qty = std::max(execution.working_order->qty - execution.working_order->cum_qty, 0);
                pending_entry_price = execution.working_order->price;
            }
            exposures.push_back({
                runtime.symbol.symbol,
                execution.inventory.qty,
                execution.inventory.avg_price,
                pending_entry_qty,
                pending_entry_price,
            });
        }
        return exposures;
    }

    [[nodiscard]] double account_realized_pnl() {
        double total = 0.0;
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                            )
                                .count();
        for (auto& [_, runtime] : strategy_runtimes_) {
            total += runtime.strategy->risk().snapshot(now_ns).daily_pnl;
        }
        return total;
    }

    [[nodiscard]] StrategyRuntime* resolve_runtime(const std::string& symbol, int exchange) {
        const auto route_it = strategy_routes_.find({symbol, exchange});
        if (route_it == strategy_routes_.end()) {
            return nullptr;
        }
        auto it = strategy_runtimes_.find(route_it->second);
        return it == strategy_runtimes_.end() ? nullptr : &it->second;
    }

    config::AppConfig config_;
    gateway::KabuRestClient rest_;
    risk::AccountRiskController account_risk_;
    std::map<std::pair<std::string, int>, StrategyRuntime> strategy_runtimes_;
    std::map<std::pair<std::string, int>, std::pair<std::string, int>> strategy_routes_;
    std::function<nlohmann::json()> websocket_status_provider_;
    bool running_{false};
    bool kill_switch_active_{false};
    std::string kill_switch_reason_;
    bool kill_switch_hard_close_{false};
    std::int64_t last_market_data_ts_ns_{0};
    std::int64_t last_market_event_receive_ts_ns_{0};
    std::map<std::string, std::int64_t> symbol_last_board_ts_ns_;
    std::map<std::string, std::int64_t> symbol_last_trade_ts_ns_;
    int token_refresh_count_{0};
    std::int64_t last_register_success_ts_ns_{0};
    std::string last_rest_error_;
    std::string last_websocket_error_;
    int reconcile_failure_count_{0};
    std::string last_reconcile_error_;
    RecoveryState recovery_state_{RecoveryState::Ready};
    bool recovery_in_progress_{false};
    std::string recovery_reason_;
    std::int64_t recovery_started_ts_ns_{0};
    std::int64_t recovery_completed_ts_ns_{0};
};

}  // namespace kabu::app
