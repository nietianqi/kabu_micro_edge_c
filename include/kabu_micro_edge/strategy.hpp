#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/execution.hpp"
#include "kabu_micro_edge/journal.hpp"
#include "kabu_micro_edge/risk.hpp"
#include "kabu_micro_edge/signals.hpp"
#include "kabu_micro_edge/strategy_policy.hpp"

namespace kabu::strategy {

enum class SessionPhase {
    Startup,
    PreOpen,
    Trading,
    LunchBreak,
    EntryCutoff,
    Closed,
};

inline std::string to_string(SessionPhase phase) {
    switch (phase) {
    case SessionPhase::Startup:
        return "startup";
    case SessionPhase::PreOpen:
        return "pre_open";
    case SessionPhase::Trading:
        return "trading";
    case SessionPhase::LunchBreak:
        return "lunch_break";
    case SessionPhase::EntryCutoff:
        return "entry_cutoff";
    case SessionPhase::Closed:
        return "closed";
    }
    return "startup";
}

struct SessionGateState {
    SessionPhase phase{SessionPhase::Startup};
    bool observe_allowed{true};
    bool entry_allowed{false};
    std::string block_reason{"startup"};

    [[nodiscard]] nlohmann::json to_json() const {
        return {
            {"phase", to_string(phase)},
            {"observe_allowed", observe_allowed},
            {"entry_allowed", entry_allowed},
            {"block_reason", block_reason},
        };
    }
};

struct DecisionTrace {
    std::int64_t ts_ns{0};
    std::string stage{"startup"};
    std::string session_phase{"startup"};
    bool allow{false};
    std::string reason{"startup"};
    std::string entry_mode;
    int entry_score{0};
    int required_confirm{0};
    bool market_ok{false};
    std::string market_reason{"startup"};
    bool local_risk_ok{true};
    bool account_guard_ok{true};
    std::string account_guard_reason;
    int confirm_progress{0};
    EntryLayerDiagnostics layers;

    [[nodiscard]] nlohmann::json to_json() const {
        return {
            {"ts_ns", ts_ns},
            {"stage", stage},
            {"session_phase", session_phase},
            {"allow", allow},
            {"reason", reason},
            {"entry_mode", entry_mode},
            {"entry_score", entry_score},
            {"required_confirm", required_confirm},
            {"market_ok", market_ok},
            {"market_reason", market_reason},
            {"local_risk_ok", local_risk_ok},
            {"account_guard_ok", account_guard_ok},
            {"account_guard_reason", account_guard_reason},
            {"confirm_progress", confirm_progress},
            {"layers", layers.to_json()},
        };
    }
};

class MicroEdgeStrategy {
  public:
    using EntryGuard = std::function<std::pair<bool, std::string>(int, double)>;
    using DecisionTraceSink = std::function<void(const DecisionTrace&)>;
    struct RuntimeMetrics {
        int queue_depth{0};
        int queue_maxsize{0};
        int max_queue_depth{0};
        int events_enqueued{0};
        int events_processed{0};
        int event_backlog{0};
        bool event_worker_running{false};
        bool event_worker_busy{false};
        double last_queue_wait_ms{0.0};
        double queue_wait_p95_ms{0.0};
        double queue_wait_p99_ms{0.0};
        double last_process_ms{0.0};
        double process_p95_ms{0.0};
        double process_p99_ms{0.0};
    };

    MicroEdgeStrategy(
        config::SymbolConfig symbol,
        config::StrategyConfig strategy,
        config::OrderProfile order_profile,
        bool dry_run = true,
        std::shared_ptr<TradeJournal> journal = nullptr,
        EntryGuard entry_guard = {},
        DecisionTraceSink decision_trace_sink = {},
        int event_queue_maxsize = 512
    )
        : symbol_config_(std::move(symbol)),
          config_(std::move(strategy)),
          order_profile_(std::move(order_profile)),
          dry_run_(dry_run),
          journal_(journal),
          entry_guard_(std::move(entry_guard)),
          decision_trace_sink_(std::move(decision_trace_sink)),
          event_queue_maxsize_(std::max(event_queue_maxsize, 1)),
          execution_(
              symbol_config_.symbol,
              symbol_config_.exchange,
              order_profile_,
              dry_run_,
              symbol_config_.tick_size,
              std::max(config_.book_imbalance_long, config_.tape_imbalance_long),
              0.0,
              static_cast<int>(config_.entry_order_timeout * 1000),
              config_.min_order_lifetime_ms,
              config_.max_requotes_per_minute,
              config_.allow_aggressive_exit,
              symbol_config_.lot_size,
              symbol_config_.topix100,
              true
          ),
          signal_engine_(
              symbol_config_.tick_size,
              config_.book_depth_levels,
              config_.book_decay,
              config_.tape_window_seconds,
              config_.mid_std_window,
              config_.min_best_volume,
              config_.kabu_bidask_reversed,
              config_.auto_fix_negative_spread,
              config_.use_microprice_tilt
          ),
          risk_(config_.max_daily_loss, config_.cooling_seconds, config_.cooling_consecutive_losses) {}

    void start() { started_ = true; }
    void stop() { started_ = false; }
    void on_board(const gateway::BoardSnapshot& snapshot) { process_board(snapshot); }
    void on_trade(const gateway::TradePrint& trade) { process_trade(trade); }

    void process_board(const gateway::BoardSnapshot& snapshot) {
        if (!started_) return;
        latest_board_ = signal_engine_.sanitize_snapshot(snapshot);
        // Jump detection: large time gap + large mid move → block entries briefly
        if (config_.enable_jump_filter && latest_board_.has_value() && latest_board_->event_gap_ns > 0) {
            const double gap_s = static_cast<double>(latest_board_->event_gap_ns) / 1e9;
            if (gap_s > config_.jump_gap_seconds && pre_gap_mid_ > 0.0) {
                const double tick = execution_.get_tse_order_tick(latest_board_->mid());
                const double mid_move = std::abs(latest_board_->mid() - pre_gap_mid_) / std::max(tick, 1e-9);
                if (mid_move > config_.jump_mid_ticks) {
                    last_jump_detected_ns_ = latest_board_->ts_ns;
                }
            }
        }
        if (latest_board_.has_value() && !latest_board_->out_of_order) {
            pre_gap_mid_ = latest_board_->mid();
        }
        latest_signal_ = signal_engine_.on_board(*latest_board_);
        latest_entry_score_ = compute_entry_score();
        latest_taker_breakout_ready_ = latest_board_.has_value() && latest_signal_.has_value()
                                           ? has_taker_breakout_signal(*latest_board_, *latest_signal_, config_)
                                           : false;
        mid_ref_ = {latest_board_->mid()};
        record_tp_touch(*latest_board_);
        if (dry_run_) execution_.sync_paper_board(*latest_board_);
        const auto now_ns = latest_board_->ts_ns;
        post_execution_update(now_ns);
        enforce_kill_switch(now_ns);
        handle_exit(now_ns);
        submit_delayed_tp(now_ns);
        handle_entry(now_ns);
    }

    void process_trade(const gateway::TradePrint& trade) {
        if (!started_) return;
        last_trade_ts_ns_ = std::max(last_trade_ts_ns_, trade.ts_ns);
        signal_engine_.on_trade(trade);
        if (latest_board_.has_value()) {
            latest_signal_ = signal_engine_.refresh_from_latest_board(*latest_board_, trade.ts_ns);
            latest_entry_score_ = compute_entry_score();
            latest_taker_breakout_ready_ = has_taker_breakout_signal(*latest_board_, *latest_signal_, config_);
        }
        if (dry_run_) execution_.sync_paper_trade(trade);
        post_execution_update(trade.ts_ns);
        enforce_kill_switch(trade.ts_ns);
        handle_exit(trade.ts_ns);
        submit_delayed_tp(trade.ts_ns);
    }

    void on_timer(std::int64_t now_ns) {
        if (!started_) return;
        execution_.check_timeout(now_ns);
        if (latest_board_.has_value() && latest_signal_.has_value()) {
            handle_exit(now_ns);
            submit_delayed_tp(now_ns);
        }
        post_execution_update(now_ns);
        enforce_kill_switch(now_ns);
    }

    void reconcile_with_prefetched(
        const std::optional<std::vector<nlohmann::json>>& positions = std::nullopt,
        const std::optional<std::map<std::string, gateway::OrderSnapshot>>& order_snapshots_by_id = std::nullopt,
        std::int64_t now_ns = 0
    ) {
        if (!started_) {
            return;
        }
        const auto reconcile_now_ns = now_ns > 0 ? now_ns : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                               std::chrono::system_clock::now().time_since_epoch()
                                                           )
                                                               .count();

        if (order_snapshots_by_id.has_value()) {
            execution_.sync_external_order_snapshots(*order_snapshots_by_id);
            for (const auto& order_id : execution_.active_order_ids()) {
                const auto it = order_snapshots_by_id->find(order_id);
                if (it != order_snapshots_by_id->end()) {
                    execution_.apply_broker_snapshot(it->second);
                }
            }
        }
        if (!dry_run_ && positions.has_value()) {
            execution_.sync_broker_position_snapshot(*positions, true, reconcile_now_ns);
        }
        post_execution_update(reconcile_now_ns);
        enforce_kill_switch(reconcile_now_ns);
        if (latest_board_.has_value() && latest_signal_.has_value()) {
            handle_exit(reconcile_now_ns);
            submit_delayed_tp(reconcile_now_ns);
        }
    }

    void activate_kill_switch(const std::string& reason, bool hard_close = true) {
        kill_switch_active_ = true;
        kill_switch_reason_ = reason.empty() ? "manual_kill_switch" : reason;
        kill_switch_hard_close_ = kill_switch_hard_close_ || hard_close;
        last_entry_block_reason_ = "kill_switch";
    }

    [[nodiscard]] execution::ExecutionController& execution() { return execution_; }
    [[nodiscard]] const execution::ExecutionController& execution() const { return execution_; }
    [[nodiscard]] risk::RiskController& risk() { return risk_; }
    [[nodiscard]] const risk::RiskController& risk() const { return risk_; }
    [[nodiscard]] const config::SymbolConfig& symbol_config() const { return symbol_config_; }
    [[nodiscard]] const std::optional<gateway::BoardSnapshot>& latest_board() const { return latest_board_; }
    [[nodiscard]] const DecisionTrace& last_entry_decision() const { return last_entry_decision_; }
    [[nodiscard]] RuntimeMetrics runtime_metrics() const {
        return {
            0,
            event_queue_maxsize_,
            max_queue_depth_,
            events_enqueued_,
            events_processed_,
            std::max(events_enqueued_ - events_processed_, 0),
            started_,
            false,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
        };
    }

    [[nodiscard]] nlohmann::json status() {
        const auto risk_snapshot = risk_.snapshot(latest_signal_.has_value() ? latest_signal_->ts_ns : 0);
        const auto session_gate = current_session_gate(reference_now_ns());
        const auto layers =
            latest_board_.has_value() && latest_signal_.has_value()
                ? entry_layer_diagnostics(*latest_board_, *latest_signal_, config_).to_json()
                : EntryLayerDiagnostics{}.to_json();
        const auto account_guard = entry_guard_ ? entry_guard_(0, latest_board_.has_value() ? latest_board_->ask : 0.0)
                                                : std::pair<bool, std::string>{true, ""};
        nlohmann::json execution_json = execution_.snapshot();
        execution_json["pending_tp_price"] = pending_tp_price_;
        execution_json["pending_tp_volume"] = pending_tp_volume_;
        execution_json["pending_tp_retry_count"] = pending_tp_retry_count_;
        execution_json["tp_retry_exit_required"] = tp_retry_exit_required_;
        execution_json["position_timeout_exit_required"] = position_timeout_exit_required_;
        execution_json["max_limit_tp_retries"] = config_.max_limit_tp_retries;
        execution_json["last_trade_ts_ns"] = last_trade_ts_ns_;
        execution_json["last_entry_mode"] = last_entry_mode_;
        execution_json["last_entry_score"] = last_entry_score_;
        execution_json["scale_in_count"] = round_scale_in_count_;

        const auto runtime = runtime_metrics();
        return {
            {"symbol", symbol_config_.symbol},
            {"state", execution::to_string(execution_.state())},
            {"signal",
             {
                 {"book", latest_signal_.has_value() ? latest_signal_->obi_raw : 0.0},
                 {"lob_ofi", latest_signal_.has_value() ? latest_signal_->lob_ofi_raw : 0.0},
                 {"tape_ofi", latest_signal_.has_value() ? latest_signal_->tape_ofi_raw : 0.0},
                 {"micro_momentum", latest_signal_.has_value() ? latest_signal_->micro_momentum_raw : 0.0},
                 {"microprice_tilt", latest_signal_.has_value() ? latest_signal_->microprice_tilt_raw : 0.0},
                 {"microprice_gap_ticks", latest_signal_.has_value() ? latest_signal_->microprice_gap_ticks : 0.0},
                 {"integrated_ofi", latest_signal_.has_value() ? latest_signal_->integrated_ofi : 0.0},
                 {"trade_burst_score", latest_signal_.has_value() ? latest_signal_->trade_burst_score : 0.0},
                 {"mid_std_ticks", latest_signal_.has_value() ? latest_signal_->mid_std_ticks : 0.0},
                 {"composite", latest_signal_.has_value() ? latest_signal_->composite : 0.0},
                 {"entry_score", latest_entry_score_},
                 {"taker_breakout_ready", latest_taker_breakout_ready_},
                 {"entry_layers", layers},
                 {"decision_trace", last_entry_decision_.to_json()},
                 {"exec_quality", [&]() -> nlohmann::json {
                     if (!latest_board_.has_value() || !latest_signal_.has_value()) return nlohmann::json::object();
                     const double tick = execution_.get_tse_order_tick(latest_board_->mid());
                     return compute_execution_quality(*latest_board_, *latest_signal_, config_, tick).to_json();
                 }()},
             }},
            {"risk",
             {
                 {"daily_pnl", risk_snapshot.daily_pnl},
                 {"consecutive_losses", risk_snapshot.consecutive_losses},
                 {"cooldown_until_ns", risk_snapshot.cooldown_until_ns},
                 {"local_entry_blocked", risk_snapshot.entry_blocked},
                 {"account_entry_blocked", !account_guard.first},
                 {"account_entry_block_reason", account_guard.second},
                 {"entry_blocked", risk_snapshot.entry_blocked || !account_guard.first},
                 {"last_entry_block_reason", last_entry_block_reason_},
                 {"kill_switch_active", kill_switch_active_},
                 {"kill_switch_reason", kill_switch_reason_},
                 {"kill_switch_hard_close", kill_switch_hard_close_},
                 {"session_phase", to_string(session_gate.phase)},
                 {"session_entry_allowed", session_gate.entry_allowed},
                 {"session_entry_block_reason", session_gate.block_reason},
             }},
            {"execution", execution_json},
            {"runtime",
             {
                 {"queue_depth", runtime.queue_depth},
                 {"queue_maxsize", runtime.queue_maxsize},
                 {"max_queue_depth", runtime.max_queue_depth},
                 {"events_enqueued", runtime.events_enqueued},
                 {"events_processed", runtime.events_processed},
                 {"event_backlog", runtime.event_backlog},
                 {"queue_overflow_count", 0},
                 {"dropped_board_events", 0},
                 {"dropped_trade_events", 0},
                 {"deferred_board_pending", false},
                 {"last_queue_drop_kind", ""},
                 {"event_worker_running", runtime.event_worker_running},
                 {"event_worker_busy", runtime.event_worker_busy},
                 {"last_event_kind", last_event_kind_},
                 {"last_queue_wait_ms", runtime.last_queue_wait_ms},
                 {"queue_wait_p95_ms", runtime.queue_wait_p95_ms},
                 {"queue_wait_p99_ms", runtime.queue_wait_p99_ms},
                 {"last_process_ms", runtime.last_process_ms},
                 {"process_p95_ms", runtime.process_p95_ms},
                 {"process_p99_ms", runtime.process_p99_ms},
             }},
            {"analytics",
             {
                 {"entry_modes",
                  {
                      {"maker_fills", entry_fill_counts_.at(ENTRY_MODE_MAKER)},
                      {"taker_fills", entry_fill_counts_.at(ENTRY_MODE_TAKER)},
                      {"mixed_fills", entry_fill_counts_.at("mixed")},
                      {"unknown_fills", entry_fill_counts_.at("unknown")},
                  }},
                 {"tp", {{"touch_count", tp_touch_count_}, {"fill_count", tp_fill_count_}}},
                 {"near_miss", {
                     {"count", near_miss_count_},
                     {"last_ts_ns", last_near_miss_ts_ns_},
                     {"last_reason", last_near_miss_reason_},
                 }},
                 {"markout", journal_ != nullptr ? journal_->snapshot() : nlohmann::json{{"trade_count", 0}, {"pending_markouts", 0}, {"pending_post_exit_markouts", 0}, {"pending_entry_markouts", 0}, {"markout_horizons", nlohmann::json::object()}, {"post_exit_horizons", nlohmann::json::object()}, {"entry_horizons", nlohmann::json::object()}}},
             }},
        };
    }

  private:
    int compute_entry_score() const {
        return latest_board_.has_value() && latest_signal_.has_value()
                   ? entry_layer_diagnostics(*latest_board_, *latest_signal_, config_).entry_score()
                   : 0;
    }

    [[nodiscard]] bool can_scale_in_with_live_tp() const {
        return !position_timeout_exit_required_ && execution_.managed_tp_exit_is_scale_in_compatible() &&
               round_scale_in_count_ < config_.max_scale_in_per_round_trip;
    }

    [[nodiscard]] std::int64_t reference_now_ns() const {
        if (latest_board_.has_value() && latest_board_->ts_ns > 0) {
            return latest_board_->ts_ns;
        }
        if (latest_signal_.has_value() && latest_signal_->ts_ns > 0) {
            return latest_signal_->ts_ns;
        }
        return last_trade_ts_ns_;
    }

    [[nodiscard]] SessionGateState current_session_gate(std::int64_t now_ns) const {
        if (!config_.enforce_session_gate) {
            return {SessionPhase::Trading, true, true, ""};
        }
        if (now_ns <= 0) {
            return {};
        }

        const int now_seconds = common::jst_seconds_of_day(now_ns);
        const auto [open_hour, open_minute] = config_.market_open_hm();
        const auto [close_hour, close_minute] = config_.market_close_hm();
        const auto [lunch_start_hour, lunch_start_minute] = config_.lunch_break_start_hm();
        const auto [lunch_end_hour, lunch_end_minute] = config_.lunch_break_end_hm();
        const int market_open_seconds = open_hour * 3600 + open_minute * 60;
        const int market_close_seconds = close_hour * 3600 + close_minute * 60;
        const int lunch_start_seconds = lunch_start_hour * 3600 + lunch_start_minute * 60;
        const int lunch_end_seconds = lunch_end_hour * 3600 + lunch_end_minute * 60;
        const int cutoff_seconds =
            std::max(market_open_seconds, market_close_seconds - config_.entry_cutoff_seconds_before_close);

        if (now_seconds < market_open_seconds) {
            return {SessionPhase::PreOpen, true, false, "session_pre_open"};
        }
        if (now_seconds >= market_close_seconds) {
            return {SessionPhase::Closed, true, false, "session_closed"};
        }
        if (!config_.allow_entries_during_lunch_break &&
            now_seconds >= lunch_start_seconds && now_seconds < lunch_end_seconds) {
            return {SessionPhase::LunchBreak, true, false, "session_lunch_break"};
        }
        if (config_.entry_cutoff_seconds_before_close > 0 && now_seconds >= cutoff_seconds) {
            return {SessionPhase::EntryCutoff, true, false, "session_entry_cutoff"};
        }
        return {SessionPhase::Trading, true, true, ""};
    }

    void publish_entry_decision(
        std::int64_t now_ns,
        std::string stage,
        bool allow,
        std::string reason,
        const std::optional<EntryDecision>& policy_decision = std::nullopt,
        bool market_ok = false,
        std::string market_reason = {},
        bool local_risk_ok = true,
        bool account_guard_ok = true,
        std::string account_guard_reason = {},
        int confirm_progress = 0
    ) {
        const auto session_gate = current_session_gate(now_ns);
        last_entry_decision_ = {
            now_ns,
            std::move(stage),
            to_string(session_gate.phase),
            allow,
            std::move(reason),
            policy_decision.has_value() ? policy_decision->entry_mode : std::string(),
            policy_decision.has_value() ? policy_decision->entry_score : latest_entry_score_,
            policy_decision.has_value() ? policy_decision->required_confirm : 0,
            market_ok,
            std::move(market_reason),
            local_risk_ok,
            account_guard_ok,
            std::move(account_guard_reason),
            confirm_progress,
            latest_board_.has_value() && latest_signal_.has_value()
                ? entry_layer_diagnostics(*latest_board_, *latest_signal_, config_)
                : EntryLayerDiagnostics{},
        };
        if (decision_trace_sink_) {
            decision_trace_sink_(last_entry_decision_);
        }
    }

    bool validate_market_quality(std::int64_t now_ns, std::string& reason) const {
        if (!latest_board_.has_value() || !latest_signal_.has_value()) { reason = "startup"; return false; }
        const auto& board = *latest_board_;
        const auto& signal = *latest_signal_;
        if (board.bid <= 0 || board.ask <= 0 || board.bid >= board.ask) { reason = "bidask"; return false; }
        if (board.bid_size < config_.min_best_volume || board.ask_size < config_.min_best_volume) { reason = "best_volume"; return false; }
        if (signal.mid_std_ticks >= config_.max_mid_std_ticks) { reason = "mid_std"; return false; }
        if (board.ts_ns > 0 && now_ns - board.ts_ns > static_cast<std::int64_t>(config_.max_tick_stale_seconds * 1e9)) { reason = "stale"; return false; }
        const double spread_ticks = board.spread() / std::max(execution_.get_tse_order_tick(board.mid()), 1e-9);
        if (spread_ticks < config_.min_spread_ticks || spread_ticks > config_.max_spread_ticks) { reason = "spread"; return false; }
        if (config_.enable_jump_filter && last_jump_detected_ns_ > 0) {
            const auto cooldown_ns = static_cast<std::int64_t>(config_.jump_cooldown_ms) * 1'000'000LL;
            if (now_ns - last_jump_detected_ns_ < cooldown_ns) { reason = "jump_cooldown"; return false; }
        }
        reason.clear(); return true;
    }

    void handle_entry(std::int64_t now_ns) {
        if (!latest_board_.has_value() || !latest_signal_.has_value()) {
            publish_entry_decision(now_ns, "startup", false, last_entry_block_reason_, std::nullopt, false, "startup");
            return;
        }
        if (kill_switch_active_) {
            last_entry_block_reason_ = "kill_switch";
            reset_confirm();
            publish_entry_decision(now_ns, "kill_switch", false, last_entry_block_reason_);
            return;
        }

        const auto session_gate = current_session_gate(now_ns);
        if (!session_gate.entry_allowed) {
            last_entry_block_reason_ = session_gate.block_reason;
            reset_confirm();
            publish_entry_decision(now_ns, "session_gate", false, last_entry_block_reason_);
            return;
        }

        std::string market_reason;
        if (!validate_market_quality(now_ns, market_reason)) {
            last_entry_block_reason_ = market_reason;
            reset_confirm();
            publish_entry_decision(now_ns, "market_quality", false, market_reason, std::nullopt, false, market_reason);
            return;
        }
        if (execution_.has_working_entry()) {
            last_entry_block_reason_ = "working_order";
            publish_entry_decision(now_ns, "working_order", false, last_entry_block_reason_, std::nullopt, true);
            return;
        }
        if (execution_.external_exit_replacement_in_progress()) {
            last_entry_block_reason_ = "external_exit_replacement_pending";
            publish_entry_decision(now_ns, "external_exit_replacement", false, last_entry_block_reason_, std::nullopt, true);
            return;
        }
        if (execution_.has_conflicting_opposite_order()) {
            last_entry_block_reason_ = "conflicting_opposite_order";
            publish_entry_decision(now_ns, "conflicting_order", false, last_entry_block_reason_, std::nullopt, true);
            return;
        }
        if (execution_.inventory.qty > 0 && !can_scale_in_with_live_tp()) {
            last_entry_block_reason_ = "scale_in_blocked";
            publish_entry_decision(now_ns, "scale_in", false, last_entry_block_reason_, std::nullopt, true);
            return;
        }

        const auto risk_gate = risk_.can_enter(now_ns);
        if (!risk_gate.first) {
            last_entry_block_reason_ = risk_gate.second;
            reset_confirm();
            publish_entry_decision(now_ns, "local_risk", false, last_entry_block_reason_, std::nullopt, true, "", false);
            return;
        }

        const auto decision = evaluate_long_signal(*latest_board_, *latest_signal_, config_);
        if (!decision.allow) {
            last_entry_block_reason_ = decision.reason;
            reset_confirm();
            publish_entry_decision(now_ns, "policy", false, last_entry_block_reason_, decision, true);
            return;
        }

        // Optional execution quality gate (pre-confirm signal filter, not in Python)
        if (config_.enable_exec_quality_gate) {
            const double tick = execution_.get_tse_order_tick(latest_board_->mid());
            const auto exec_quality = compute_execution_quality(*latest_board_, *latest_signal_, config_, tick);
            if (exec_quality.total() < config_.min_exec_quality_score) {
                last_entry_block_reason_ = "exec_quality:" + std::to_string(exec_quality.total());
                reset_confirm();
                record_near_miss_if_needed(decision, now_ns);
                publish_entry_decision(now_ns, "exec_quality", false, last_entry_block_reason_, decision, true);
                return;
            }
        }

        // Signal expiry: reset confirm counter if signal has been quiet too long
        if (last_confirm_ts_ns_ > 0 && config_.signal_expire_seconds > 0) {
            const auto expire_ns = static_cast<std::int64_t>(config_.signal_expire_seconds * 1e9);
            if (now_ns - last_confirm_ts_ns_ > expire_ns) {
                reset_confirm();
            }
        }

        // Accumulate confirmation ticks regardless of downstream rate limits (matches Python)
        const int next_confirm = long_confirm_ + 1;
        ++long_confirm_;
        last_confirm_ts_ns_ = now_ns;
        if (long_confirm_ < decision.required_confirm) {
            last_entry_block_reason_ = "confirming";
            publish_entry_decision(
                now_ns,
                "confirm",
                false,
                last_entry_block_reason_,
                decision,
                true,
                "",
                true,
                true,
                "",
                next_confirm
            );
            return;
        }

        if (now_ns - last_entry_order_action_ns_ <
            static_cast<std::int64_t>(config_.entry_order_interval_ms) * 1'000'000LL) {
            last_entry_block_reason_ = "entry_rate_limit";
            record_near_miss_if_needed(decision, now_ns);
            publish_entry_decision(now_ns, "entry_interval", false, last_entry_block_reason_, decision, true);
            return;
        }

        // Dynamic position sizing: scale up base volume on high-confidence signals
        int base_volume = config_.trade_volume;
        if (config_.scale_qty_by_score && decision.entry_score >= config_.scale_qty_score_threshold) {
            base_volume = static_cast<int>(config_.trade_volume * config_.scale_qty_multiplier);
            if (config_.scale_qty_max_volume > 0) {
                base_volume = std::min(base_volume, config_.scale_qty_max_volume);
            }
        }
        int order_qty = std::min(base_volume, std::max(config_.max_long_inventory - execution_.inventory.qty, 0));
        if (symbol_config_.max_notional > 0 && latest_board_->ask > 0) {
            order_qty = std::min(order_qty, static_cast<int>(symbol_config_.max_notional / latest_board_->ask));
        }
        if (order_qty <= 0) {
            last_entry_block_reason_ = "symbol_inventory_limit";
            reset_confirm();
            record_near_miss_if_needed(decision, now_ns);
            publish_entry_decision(now_ns, "inventory_limit", false, last_entry_block_reason_, decision, true);
            return;
        }
        order_qty = align_order_qty_to_lot_size(order_qty);
        if (order_qty <= 0) {
            last_entry_block_reason_ = "lot_size_rounddown";
            reset_confirm();
            record_near_miss_if_needed(decision, now_ns);
            publish_entry_decision(now_ns, "lot_size", false, last_entry_block_reason_, decision, true);
            return;
        }

        if (entry_guard_) {
            const auto guard = entry_guard_(order_qty, latest_board_->ask);
            if (!guard.first) {
                last_entry_block_reason_ = guard.second;
                reset_confirm();
                record_near_miss_if_needed(decision, now_ns);
                publish_entry_decision(
                    now_ns,
                    "account_guard",
                    false,
                    last_entry_block_reason_,
                    decision,
                    true,
                    "",
                    true,
                    false,
                    guard.second
                );
                return;
            }
        }

        reset_confirm();
        const double entry_price = decision.entry_mode == ENTRY_MODE_TAKER ? latest_board_->ask : latest_board_->bid;
        working_entry_mode_ = decision.entry_mode;
        working_entry_score_ = decision.entry_score;
        working_entry_scale_in_counted_ = false;
        if (execution_.open_explicit(
                1,
                order_qty,
                entry_price,
                *latest_board_,
                "long_entry",
                decision.entry_mode == ENTRY_MODE_TAKER,
                decision.entry_mode,
                decision.entry_score
            )) {
            working_entry_is_scale_in_ = execution_.inventory.qty > 0;
            last_entry_order_action_ns_ = now_ns;
            last_entry_block_reason_ = "entered";
            publish_entry_decision(now_ns, "entered", true, last_entry_block_reason_, decision, true);
            if (execution_.inventory.qty > 0) {
                post_execution_update(now_ns);
                handle_exit(now_ns);
                submit_delayed_tp(now_ns);
            }
            return;
        }
        last_entry_block_reason_ =
            now_ns < execution_.entry_blocked_until_ns ? "entry_api_backoff" : "entry_submit_rejected";
        publish_entry_decision(now_ns, "execution_submit", false, last_entry_block_reason_, decision, true);
    }

    void handle_exit(std::int64_t now_ns) {
        if (!latest_board_.has_value() || !latest_signal_.has_value() || execution_.inventory.qty <= 0) return;
        if (kill_switch_active_ && kill_switch_hard_close_) return enforce_kill_switch(now_ns);
        execution_.request_external_exit_replacement();
        if (execution_.external_exit_replacement_in_progress()) return;
        if (!execution_.can_manage_local_exit()) return;
        if (execution_.has_stranded_partial && allow_exit_order(now_ns)) { execution_.close(*latest_board_, -9.0, "stranded_partial_exit", true, latest_board_->bid); last_exit_order_action_ns_ = now_ns; return; }
        if (tp_retry_exit_required_ && allow_exit_order(now_ns)) { execution_.close(*latest_board_, -2.0, "tp_retry_exhausted_exit", true, latest_board_->bid); last_exit_order_action_ns_ = now_ns; return; }
        if (max_loss_emergency_due(*latest_board_) && allow_exit_order(now_ns)) {
            clear_pending_tp();
            execution_.close(*latest_board_, -9.0, "max_loss_emergency", true, emergency_exit_price(*latest_board_));
            last_exit_order_action_ns_ = now_ns;
            return;
        }
        if (!config_.tp_only_mode && position_timeout_due(now_ns)) {
            position_timeout_exit_required_ = true;
            clear_pending_tp();
            if (execution_.exit_order.has_value() && execution_.exit_order->reason.starts_with("limit_tp")) {
                execution_.cancel_exit_order("refresh_position_timeout");
                return;
            }
            if (allow_exit_order(now_ns)) { execution_.close(*latest_board_, -1.0, "position_timeout", true, latest_board_->bid); last_exit_order_action_ns_ = now_ns; return; }
        }
        const bool flow_flip = latest_signal_->tape_ofi_raw <= -config_.flow_flip_threshold || latest_signal_->lob_ofi_raw <= -config_.flow_flip_threshold;
        if ((!config_.tp_only_mode || config_.allow_flow_flip_in_tp_only) && config_.flow_flip_threshold > 0 && flow_flip && allow_exit_order(now_ns)) {
            execution_.close(*latest_board_, -1.0, "flow_flip", !config_.tp_only_mode, latest_board_->bid);
            last_exit_order_action_ns_ = now_ns;
            return;
        }
        const double stop_trigger = execution_.inventory.avg_price - config_.loss_ticks * execution_.get_tse_order_tick(execution_.inventory.avg_price);
        if ((!config_.tp_only_mode || config_.allow_stop_loss_in_tp_only) && latest_board_->bid <= stop_trigger && allow_exit_order(now_ns)) {
            execution_.close(*latest_board_, -1.0, "stop_loss", !config_.tp_only_mode, latest_board_->bid);
            last_exit_order_action_ns_ = now_ns;
            return;
        }
        schedule_limit_tp(now_ns);
    }

    void schedule_limit_tp(std::int64_t now_ns) {
        if (!config_.enable_limit_tp_order || execution_.inventory.qty <= 0 || kill_switch_hard_close_ || position_timeout_exit_required_) return;
        pending_tp_price_ = build_tp_price();
        pending_tp_volume_ = execution_.inventory.qty;
        need_submit_limit_tp_ = pending_tp_price_ > 0 && pending_tp_volume_ > 0;
        if (need_submit_limit_tp_ && pending_tp_submit_after_ns_ <= 0) pending_tp_submit_after_ns_ = now_ns + static_cast<std::int64_t>(config_.limit_tp_delay_seconds * 1e9);
    }

    void submit_delayed_tp(std::int64_t now_ns) {
        if (!need_submit_limit_tp_ || !latest_board_.has_value() || now_ns < pending_tp_submit_after_ns_) return;
        if (execution_.exit_order.has_value() && execution_.exit_order->reason.starts_with("limit_tp") &&
            execution_.exit_order->qty == pending_tp_volume_ && std::abs(execution_.exit_order->price - pending_tp_price_) <= 1e-9) { clear_pending_tp(); return; }
        if (!allow_limit_tp_order(now_ns)) return;
        const bool submitted = execution_.close(*latest_board_, 0.0, "limit_tp_quote", false, pending_tp_price_);
        if (submitted) {
            last_limit_tp_order_action_ns_ = now_ns;
            if (execution_.exit_order.has_value() && execution_.exit_order->reason == "limit_tp_quote") clear_pending_tp();
            return;
        }
        if (!execution_.exit_order.has_value()) {
            ++pending_tp_retry_count_;
            pending_tp_submit_after_ns_ = now_ns + static_cast<std::int64_t>(config_.limit_tp_delay_seconds * 1e9);
            if (pending_tp_retry_count_ >= config_.max_limit_tp_retries) {
                tp_retry_exit_required_ = true;
                pending_tp_retry_count_ = 0;
                need_submit_limit_tp_ = false;
            }
        }
    }

    void enforce_kill_switch(std::int64_t now_ns) {
        if (!kill_switch_active_) return;
        if (execution_.has_working_entry()) execution_.cancel_working("kill_switch_entry");
        if (kill_switch_hard_close_ && latest_board_.has_value() && execution_.inventory.qty > 0 && allow_exit_order(now_ns)) {
            clear_pending_tp();
            execution_.close(*latest_board_, -9.0, "kill_switch_emergency", true, latest_board_->bid);
            last_exit_order_action_ns_ = now_ns;
        }
    }

    void post_execution_update(std::int64_t now_ns) {
        const int current_qty = execution_.inventory.qty;
        const double current_avg_price = execution_.inventory.avg_price;
        if (current_qty > observed_inventory_qty_) {
            const int fill_qty = current_qty - observed_inventory_qty_;
            const double fill_price = execution::ExecutionController::incremental_fill_price(
                observed_inventory_qty_,
                observed_inventory_avg_price_,
                current_qty,
                current_avg_price
            );
            const auto fill_ts_ns = execution_.inventory.last_entry_fill_ts_ns > 0
                                        ? execution_.inventory.last_entry_fill_ts_ns
                                        : execution_.inventory.opened_ts_ns;
            if (observed_inventory_qty_ == 0 && latest_signal_.has_value()) round_trip_entry_signal_ = latest_signal_;
            last_entry_mode_ = execution_.inventory.entry_mode.empty() ? working_entry_mode_ : execution_.inventory.entry_mode;
            last_entry_score_ = std::max(execution_.inventory.entry_score, working_entry_score_);
            auto bucket_it = entry_fill_counts_.find(last_entry_mode_);
            if (bucket_it == entry_fill_counts_.end()) {
                bucket_it = entry_fill_counts_.find("unknown");
            }
            if (bucket_it != entry_fill_counts_.end()) {
                ++bucket_it->second;
            }
            if (journal_ != nullptr && fill_qty > 0 && fill_price > 0 && fill_ts_ns > 0) {
                int queue_ahead_qty = execution_.inventory.entry_queue_ahead_qty;
                if (execution_.working_order.has_value()) {
                    queue_ahead_qty = std::max(queue_ahead_qty, execution_.working_order->initial_queue_ahead_qty);
                }
                journal_->schedule_entry_markout(
                    symbol_config_.symbol,
                    execution_.inventory.side,
                    fill_qty,
                    fill_price,
                    fill_ts_ns,
                    !working_entry_mode_.empty() ? working_entry_mode_
                                                 : (!execution_.inventory.entry_mode.empty() ? execution_.inventory.entry_mode : last_entry_mode_),
                    working_entry_score_ > 0 ? working_entry_score_
                                             : (execution_.inventory.entry_score > 0 ? execution_.inventory.entry_score : last_entry_score_),
                    !execution_.paper_last_fill_reason.empty() ? execution_.paper_last_fill_reason : execution_.inventory.entry_fill_reason,
                    queue_ahead_qty,
                    mid_ref_
                );
            }
        }
        if (working_entry_is_scale_in_ && current_qty > observed_inventory_qty_ && !working_entry_scale_in_counted_) {
            ++round_scale_in_count_;
            working_entry_scale_in_counted_ = true;
        }
        observed_inventory_qty_ = current_qty;
        observed_inventory_avg_price_ = current_avg_price;
        for (const auto& trade : execution_.drain_round_trips()) {
            risk_.on_round_trip(trade, now_ns);
            if (trade.exit_reason.starts_with("limit_tp")) ++tp_fill_count_;
            if (journal_ != nullptr) { journal_->log_trade(trade, round_trip_entry_signal_); journal_->schedule_markout(trade, mid_ref_); }
        }
        if (current_qty == 0 && observed_inventory_qty_before_close_ > 0) {
            round_scale_in_count_ = 0;
            round_trip_entry_signal_.reset();
            touched_tp_order_id_.clear();
        }
        if (current_qty == 0) {
            clear_pending_tp();
            tp_retry_exit_required_ = false;
            position_timeout_exit_required_ = false;
        }
        if (!execution_.working_order.has_value()) {
            working_entry_is_scale_in_ = false;
            working_entry_scale_in_counted_ = false;
            working_entry_mode_.clear();
            working_entry_score_ = 0;
        }
        observed_inventory_qty_before_close_ = current_qty;
    }

    void record_tp_touch(const gateway::BoardSnapshot& snapshot) {
        if (!execution_.exit_order.has_value() || !execution_.exit_order->reason.starts_with("limit_tp")) return;
        if (snapshot.bid >= execution_.exit_order->price && touched_tp_order_id_ != execution_.exit_order->order_id) {
            ++tp_touch_count_;
            touched_tp_order_id_ = execution_.exit_order->order_id;
        }
    }

    [[nodiscard]] bool allow_exit_order(std::int64_t now_ns) const {
        return now_ns - last_exit_order_action_ns_ >= static_cast<std::int64_t>(config_.exit_order_interval_ms) * 1'000'000LL;
    }

    [[nodiscard]] bool allow_limit_tp_order(std::int64_t now_ns) const {
        return now_ns - last_limit_tp_order_action_ns_ >= static_cast<std::int64_t>(config_.limit_tp_order_interval_ms) * 1'000'000LL;
    }

    [[nodiscard]] bool position_timeout_due(std::int64_t now_ns) const {
        return config_.max_position_hold_seconds > 0 && execution_.inventory.opened_ts_ns > 0 &&
               now_ns - execution_.inventory.opened_ts_ns >= static_cast<std::int64_t>(config_.max_position_hold_seconds * 1e9);
    }

    [[nodiscard]] bool max_loss_emergency_due(const gateway::BoardSnapshot& snapshot) const {
        if (config_.max_loss_per_trade >= 0 || execution_.inventory.qty <= 0 || execution_.inventory.avg_price <= 0) {
            return false;
        }
        const double mark_price = execution_.inventory.side >= 0 ? snapshot.bid : snapshot.ask;
        if (mark_price <= 0) {
            return false;
        }
        const double gross_pnl =
            execution_.inventory.side * (mark_price - execution_.inventory.avg_price) * execution_.inventory.qty;
        const double round_trip_commission =
            config_.commission_per_share * execution_.inventory.qty * 2.0;
        return gross_pnl - round_trip_commission < config_.max_loss_per_trade;
    }

    [[nodiscard]] double emergency_exit_price(const gateway::BoardSnapshot& snapshot) const {
        const double tick = execution_.get_tse_order_tick(snapshot.bid);
        return std::max(snapshot.bid - config_.exit_slip_ticks * tick, tick);
    }

    [[nodiscard]] double build_tp_price() const {
        if (execution_.inventory.qty <= 0 || execution_.inventory.avg_price <= 0 || !latest_board_.has_value()) return 0.0;
        const double tick = execution_.get_tse_order_tick(execution_.inventory.avg_price);
        const double tp_ticks = config_.aggressive_taker_mode && execution_.inventory.entry_mode == ENTRY_MODE_TAKER
                                    ? config_.aggressive_taker_profit_ticks
                                    : config_.profit_ticks;
        return execution_.align_close_price_to_tse_tick(execution_.inventory.avg_price + tp_ticks * tick, execution_.inventory.side, execution_.inventory.avg_price, *latest_board_);
    }

    void reset_confirm() {
        long_confirm_ = 0;
        last_confirm_ts_ns_ = 0;
    }

    void record_near_miss_if_needed(const EntryDecision& decision, std::int64_t now_ns) {
        if (!config_.track_near_misses || decision.entry_score < config_.near_miss_min_score) {
            return;
        }
        ++near_miss_count_;
        last_near_miss_ts_ns_ = now_ns;
        last_near_miss_reason_ = last_entry_block_reason_;
    }

    [[nodiscard]] int align_order_qty_to_lot_size(int qty) const {
        if (qty <= 0) {
            return 0;
        }
        return (qty / std::max(symbol_config_.lot_size, 1)) * std::max(symbol_config_.lot_size, 1);
    }

    void clear_pending_tp() {
        need_submit_limit_tp_ = false;
        pending_tp_price_ = 0.0;
        pending_tp_volume_ = 0;
        pending_tp_submit_after_ns_ = 0;
        pending_tp_retry_count_ = 0;
    }

    config::SymbolConfig symbol_config_;
    config::StrategyConfig config_;
    config::OrderProfile order_profile_;
    bool dry_run_{true};
    std::shared_ptr<TradeJournal> journal_{nullptr};
    EntryGuard entry_guard_;
    DecisionTraceSink decision_trace_sink_;
    execution::ExecutionController execution_;
    signals::MicroEdgeSignalEngine signal_engine_;
    risk::RiskController risk_;
    bool started_{false};
    std::optional<gateway::BoardSnapshot> latest_board_;
    std::optional<signals::SignalPacket> latest_signal_;
    std::optional<signals::SignalPacket> round_trip_entry_signal_;
    std::int64_t last_trade_ts_ns_{0};
    std::vector<double> mid_ref_{0.0};
    int latest_entry_score_{0};
    bool latest_taker_breakout_ready_{false};
    std::map<std::string, int> entry_fill_counts_{{ENTRY_MODE_MAKER, 0}, {ENTRY_MODE_TAKER, 0}, {"mixed", 0}, {"unknown", 0}};
    int tp_touch_count_{0};
    int tp_fill_count_{0};
    int round_scale_in_count_{0};
    std::string last_entry_mode_;
    int last_entry_score_{0};
    std::string working_entry_mode_;
    int working_entry_score_{0};
    bool working_entry_is_scale_in_{false};
    bool working_entry_scale_in_counted_{false};
    int observed_inventory_qty_{0};
    double observed_inventory_avg_price_{0.0};
    int observed_inventory_qty_before_close_{0};
    std::string last_entry_block_reason_{"startup"};
    DecisionTrace last_entry_decision_{};
    bool need_submit_limit_tp_{false};
    double pending_tp_price_{0.0};
    int pending_tp_volume_{0};
    std::int64_t pending_tp_submit_after_ns_{0};
    int pending_tp_retry_count_{0};
    bool tp_retry_exit_required_{false};
    bool position_timeout_exit_required_{false};
    std::int64_t last_entry_order_action_ns_{0};
    std::int64_t last_exit_order_action_ns_{0};
    std::int64_t last_limit_tp_order_action_ns_{0};
    int long_confirm_{0};
    std::int64_t last_confirm_ts_ns_{0};
    std::int64_t last_jump_detected_ns_{0};
    double       pre_gap_mid_{0.0};
    int          near_miss_count_{0};
    std::int64_t last_near_miss_ts_ns_{0};
    std::string  last_near_miss_reason_;
    bool kill_switch_active_{false};
    std::string kill_switch_reason_;
    bool kill_switch_hard_close_{false};
    std::string touched_tp_order_id_;
    int event_queue_maxsize_{512};
    int events_enqueued_{0};
    int events_processed_{0};
    int max_queue_depth_{0};
    std::string last_event_kind_;
};

}  // namespace kabu::strategy
