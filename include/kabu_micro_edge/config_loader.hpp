#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config_types.hpp"

namespace kabu::config {

inline const nlohmann::json& default_config_json() {
    static const nlohmann::json value = {
        {"api_password", "YOUR_KABU_API_PASSWORD"},
        {"base_url", "http://localhost:18080"},
        {"ws_url", "ws://localhost:18080/kabusapi/websocket"},
        {"dry_run", true},
        {"status_interval_s", 15},
        {"reconcile_interval_ms", 500},
        {"timer_interval_ms", 50},
        {"event_queue_maxsize", 512},
        {"kill_switch_path", "logs/kill-switch.json"},
        {"kill_switch_poll_interval_ms", 250},
        {"alert_webhook_url", ""},
        {"alert_cooldown_seconds", 300},
        {"alert_timeout_s", 3.0},
        {"journal_path", "trades.csv"},
        {"markout_seconds", 30},
        {"rate_limit_per_second", 4.0},
        {"order_rate_limit_per_second", 4.0},
        {"poll_rate_limit_per_second", 4.0},
        {"shutdown_emergency_timeout_s", 5.0},
        {"startup_retry_count", 3},
        {"startup_retry_delay_s", 2.0},
        {"order_profile",
         {{"mode", "margin"},
          {"allow_short", false},
          {"account_type", 4},
          {"cash_buy_fund_type", "02"},
          {"cash_buy_deliv_type", 2},
          {"cash_sell_fund_type", ""},
          {"cash_sell_deliv_type", 0},
          {"margin_trade_type", 1},
          {"margin_open_fund_type", "11"},
          {"margin_open_deliv_type", 0},
          {"margin_close_deliv_type", 2},
          {"front_order_type_limit", 20},
          {"front_order_type_market", 10}}},
        {"account_risk",
         {{"enabled", true},
          {"max_daily_loss", -20000.0},
          {"max_total_long_inventory", 0},
          {"max_total_notional", 0.0}}},
        {"symbol",
         {{"symbol", "7269"},
          {"exchange", 9},
          {"tick_size", 0.5},
          {"max_notional", 500000},
          {"topix100", false}}},
        {"strategy",
         {{"trade_volume", 100},
          {"profit_ticks", 2.0},
          {"aggressive_taker_profit_ticks", 1.0},
          {"loss_ticks", 3.0},
          {"exit_slip_ticks", 1.0},
          {"enable_limit_tp_order", true},
          {"tp_only_mode", true},
          {"allow_flow_flip_in_tp_only", false},
          {"allow_stop_loss_in_tp_only", false},
          {"max_loss_per_trade", -500.0},
          {"book_imbalance_long", 0.35},
          {"of_imbalance_long", 0.25},
          {"tape_window_seconds", 10},
          {"tape_imbalance_long", 0.20},
          {"mom_long_threshold", 0.25},
          {"microprice_tilt_long", 0.25},
          {"confirm_ticks", 2},
          {"use_adaptive_confirm", true},
          {"strong_signal_confirm", 2},
          {"strong_signal_multiplier", 1.5},
          {"entry_order_timeout", 1.0},
          {"exit_order_timeout", 3.0},
          {"entry_order_interval_ms", 120},
          {"exit_order_interval_ms", 80},
          {"limit_tp_order_interval_ms", 150},
          {"max_daily_loss", -20000.0},
          {"max_long_inventory", 500},
          {"max_tick_stale_seconds", 2.5},
          {"kabu_bidask_reversed", false},
          {"auto_fix_negative_spread", true},
          {"book_depth_levels", 5},
          {"book_decay", 0.75},
          {"min_best_volume", 100},
          {"min_spread_ticks", 1.0},
          {"max_spread_ticks", 3.0},
          {"max_mid_std_ticks", 2.5},
          {"mid_std_window", 60},
          {"use_tape_ofi", true},
          {"use_microprice_tilt", true},
          {"aggressive_taker_mode", true},
          {"aggressive_taker_entry_score", 11},
          {"maker_score_threshold", 6},
          {"taker_score_threshold", 9},
          {"max_scale_in_per_round_trip", 1},
          {"max_position_hold_seconds", 120.0},
          {"max_limit_tp_retries", 5},
          {"flow_flip_threshold", 0.18},
          {"cooling_seconds", 60},
          {"cooling_consecutive_losses", 0},
          {"signal_expire_seconds", 3.0},
          {"limit_tp_delay_seconds", 0.1},
          {"min_order_lifetime_ms", 150},
          {"max_requotes_per_minute", 30},
          {"allow_aggressive_exit", false},
          {"market_open", "09:00"},
          {"market_close", "15:30"},
          {"lunch_break_start", "11:30"},
          {"lunch_break_end", "12:30"},
          {"commission_per_share", 0.0}}}
    };
    return value;
}

inline const nlohmann::json& DEFAULT_CONFIG = default_config_json();

inline nlohmann::json deep_merge(nlohmann::json base, const nlohmann::json& override_payload) {
    if (!base.is_object() || !override_payload.is_object()) {
        return override_payload;
    }
    for (auto it = override_payload.begin(); it != override_payload.end(); ++it) {
        if (base.contains(it.key()) && base.at(it.key()).is_object() && it.value().is_object()) {
            base[it.key()] = deep_merge(base.at(it.key()), it.value());
        } else {
            base[it.key()] = it.value();
        }
    }
    return base;
}

inline int minutes_from_hhmm(const std::string& value) {
    const auto pos = value.find(':');
    if (pos == std::string::npos) {
        throw std::runtime_error("invalid HH:MM value: " + value);
    }
    const int hour = std::stoi(value.substr(0, pos));
    const int minute = std::stoi(value.substr(pos + 1));
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        throw std::runtime_error("invalid HH:MM value: " + value);
    }
    return hour * 60 + minute;
}

inline SymbolConfig load_symbol_config(const nlohmann::json& payload, const std::string& section_name) {
    SymbolConfig symbol;
    symbol.symbol = payload.value("symbol", std::string());
    symbol.exchange = payload.value("exchange", 1);
    symbol.tick_size = payload.value("tick_size", 0.0);
    symbol.max_notional = payload.value("max_notional", 0.0);
    symbol.topix100 = payload.value("topix100", false);
    if (symbol.symbol.empty()) {
        throw std::runtime_error(section_name + ".symbol is required");
    }
    if (symbol.tick_size <= 0) {
        throw std::runtime_error(section_name + ".tick_size must be > 0");
    }
    return symbol;
}

inline void validate_symbols(const std::vector<SymbolConfig>& symbols) {
    if (symbols.empty()) {
        throw std::runtime_error("config.json must define at least one symbol");
    }
    std::map<std::pair<std::string, int>, std::string> seen_keys;
    std::map<std::pair<std::string, int>, std::string> seen_stream_keys;
    for (const auto& symbol : symbols) {
        const auto key = symbol.key();
        if (seen_keys.contains(key)) {
            throw std::runtime_error(
                "duplicate symbol config: symbol=" + symbol.symbol + " exchange=" + std::to_string(symbol.exchange)
            );
        }
        seen_keys[key] = symbol.symbol + "@" + std::to_string(symbol.exchange);
        for (const auto& stream_key : symbol.stream_keys()) {
            if (seen_stream_keys.contains(stream_key)) {
                throw std::runtime_error(
                    "symbols create ambiguous market-data routing after exchange normalization: " +
                    seen_stream_keys[stream_key] + " conflicts with " + symbol.symbol + "@" +
                    std::to_string(symbol.exchange)
                );
            }
            seen_stream_keys[stream_key] = symbol.symbol + "@" + std::to_string(symbol.exchange);
        }
    }
}

inline void validate_strategy_modes(const StrategyConfig& strategy) {
    if (!strategy.enable_limit_tp_order) {
        throw std::runtime_error("config.json strategy.enable_limit_tp_order must be true for this execution model.");
    }
    if (!strategy.use_tape_ofi) {
        throw std::runtime_error("config.json strategy.use_tape_ofi must be true for the 5-signal long-entry system.");
    }
    if (!strategy.use_microprice_tilt) {
        throw std::runtime_error("config.json strategy.use_microprice_tilt must be true for the 5-signal long-entry system.");
    }
    if (strategy.maker_score_threshold <= 0) {
        throw std::runtime_error("config.json strategy.maker_score_threshold must be > 0.");
    }
    if (strategy.aggressive_taker_entry_score <= 0) {
        throw std::runtime_error("config.json strategy.aggressive_taker_entry_score must be > 0.");
    }
    if (strategy.maker_score_threshold > 13) {
        throw std::runtime_error("config.json strategy.maker_score_threshold must be <= 13.");
    }
    if (strategy.taker_score_threshold > 13) {
        throw std::runtime_error("config.json strategy.taker_score_threshold must be <= 13.");
    }
    if (strategy.aggressive_taker_entry_score > 13) {
        throw std::runtime_error("config.json strategy.aggressive_taker_entry_score must be <= 13.");
    }
    if (strategy.taker_score_threshold < strategy.maker_score_threshold) {
        throw std::runtime_error("config.json strategy.taker_score_threshold must be >= maker_score_threshold.");
    }
    if (strategy.aggressive_taker_entry_score < strategy.taker_score_threshold) {
        throw std::runtime_error(
            "config.json strategy.aggressive_taker_entry_score must be >= taker_score_threshold."
        );
    }
}

inline void validate_account_risk(const AccountRiskConfig& account_risk) {
    if (account_risk.max_daily_loss > 0) {
        throw std::runtime_error("config.json account_risk.max_daily_loss must be <= 0.");
    }
    if (account_risk.max_total_long_inventory < 0) {
        throw std::runtime_error("config.json account_risk.max_total_long_inventory must be >= 0.");
    }
    if (account_risk.max_total_notional < 0) {
        throw std::runtime_error("config.json account_risk.max_total_notional must be >= 0.");
    }
}

inline void validate_strategy_ranges(const StrategyConfig& strategy) {
    auto require = [](bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };

    require(strategy.max_scale_in_per_round_trip >= 0, "config.json strategy.max_scale_in_per_round_trip must be >= 0.");
    require(strategy.max_position_hold_seconds >= 0, "config.json strategy.max_position_hold_seconds must be >= 0.");
    require(strategy.max_limit_tp_retries > 0, "config.json strategy.max_limit_tp_retries must be > 0.");
    require(strategy.trade_volume > 0, "config.json strategy.trade_volume must be > 0.");
    require(strategy.profit_ticks > 0, "config.json strategy.profit_ticks must be > 0.");
    require(strategy.aggressive_taker_profit_ticks > 0, "config.json strategy.aggressive_taker_profit_ticks must be > 0.");
    require(strategy.loss_ticks > 0, "config.json strategy.loss_ticks must be > 0.");
    require(strategy.exit_slip_ticks >= 0, "config.json strategy.exit_slip_ticks must be >= 0.");
    require(strategy.max_loss_per_trade <= 0, "config.json strategy.max_loss_per_trade must be <= 0.");
    require(strategy.tape_window_seconds > 0, "config.json strategy.tape_window_seconds must be > 0.");
    require(strategy.confirm_ticks > 0, "config.json strategy.confirm_ticks must be >= 1.");
    require(strategy.strong_signal_confirm > 0, "config.json strategy.strong_signal_confirm must be >= 1.");
    require(strategy.entry_order_timeout > 0, "config.json strategy.entry_order_timeout must be > 0.");
    require(strategy.exit_order_timeout >= 0, "config.json strategy.exit_order_timeout must be >= 0.");
    require(strategy.entry_order_interval_ms >= 0, "config.json strategy.entry_order_interval_ms must be >= 0.");
    require(strategy.exit_order_interval_ms >= 0, "config.json strategy.exit_order_interval_ms must be >= 0.");
    require(strategy.limit_tp_order_interval_ms >= 0, "config.json strategy.limit_tp_order_interval_ms must be >= 0.");
    require(strategy.max_daily_loss <= 0, "config.json strategy.max_daily_loss must be <= 0.");
    require(strategy.max_long_inventory >= 0, "config.json strategy.max_long_inventory must be >= 0.");
    require(strategy.max_tick_stale_seconds > 0, "config.json strategy.max_tick_stale_seconds must be > 0.");
    require(strategy.book_depth_levels > 0, "config.json strategy.book_depth_levels must be > 0.");
    require(strategy.book_decay > 0 && strategy.book_decay <= 1.0, "config.json strategy.book_decay must be within (0, 1].");
    require(strategy.min_best_volume >= 0, "config.json strategy.min_best_volume must be >= 0.");
    require(strategy.min_spread_ticks > 0, "config.json strategy.min_spread_ticks must be > 0.");
    require(strategy.max_spread_ticks >= strategy.min_spread_ticks, "config.json strategy.max_spread_ticks must be >= min_spread_ticks.");
    require(strategy.max_spread_ticks <= 100, "config.json strategy.max_spread_ticks must be <= 100.");
    require(strategy.max_mid_std_ticks > 0, "config.json strategy.max_mid_std_ticks must be > 0.");
    require(strategy.mid_std_window > 0, "config.json strategy.mid_std_window must be > 0.");
    require(strategy.strong_signal_multiplier > 1.0, "config.json strategy.strong_signal_multiplier must be > 1.0.");
    require(strategy.flow_flip_threshold >= 0, "config.json strategy.flow_flip_threshold must be >= 0.");
    require(strategy.cooling_seconds >= 0, "config.json strategy.cooling_seconds must be >= 0.");
    require(strategy.cooling_consecutive_losses >= 0, "config.json strategy.cooling_consecutive_losses must be >= 0.");
    require(
        !(strategy.cooling_consecutive_losses > 0 && strategy.cooling_seconds <= 0),
        "config.json strategy.cooling_seconds must be > 0 when cooling_consecutive_losses is enabled."
    );
    require(strategy.book_imbalance_long > 0, "config.json strategy.book_imbalance_long must be > 0.");
    require(strategy.of_imbalance_long > 0, "config.json strategy.of_imbalance_long must be > 0.");
    require(strategy.tape_imbalance_long > 0, "config.json strategy.tape_imbalance_long must be > 0.");
    require(strategy.mom_long_threshold > 0, "config.json strategy.mom_long_threshold must be > 0.");
    require(strategy.microprice_tilt_long > 0, "config.json strategy.microprice_tilt_long must be > 0.");
    require(strategy.signal_expire_seconds > 0, "config.json strategy.signal_expire_seconds must be > 0.");
    require(strategy.limit_tp_delay_seconds >= 0, "config.json strategy.limit_tp_delay_seconds must be >= 0.");
    require(strategy.min_order_lifetime_ms >= 0, "config.json strategy.min_order_lifetime_ms must be >= 0.");
    require(strategy.max_requotes_per_minute > 0, "config.json strategy.max_requotes_per_minute must be > 0.");
    require(strategy.commission_per_share >= 0, "config.json strategy.commission_per_share must be >= 0.");
}

inline void validate_market_session(const StrategyConfig& strategy) {
    const int market_open_minutes = minutes_from_hhmm(strategy.market_open);
    const int market_close_minutes = minutes_from_hhmm(strategy.market_close);
    const int lunch_start_minutes = minutes_from_hhmm(strategy.lunch_break_start);
    const int lunch_end_minutes = minutes_from_hhmm(strategy.lunch_break_end);
    if (market_open_minutes >= market_close_minutes) {
        throw std::runtime_error("config.json strategy.market_open must be earlier than market_close.");
    }
    if (lunch_start_minutes >= lunch_end_minutes) {
        throw std::runtime_error("config.json strategy.lunch_break_start must be earlier than lunch_break_end.");
    }
    if (lunch_start_minutes < market_open_minutes || lunch_end_minutes > market_close_minutes) {
        throw std::runtime_error("config.json strategy lunch break must stay within the trading session.");
    }
}

inline AppConfig load_config(const std::filesystem::path& path = {}) {
    nlohmann::json overrides = nlohmann::json::object();
    nlohmann::json payload = default_config_json();

    if (!path.empty() && std::filesystem::exists(path)) {
        std::ifstream handle(path);
        handle >> overrides;
        payload = deep_merge(payload, overrides);
    }

    if (overrides.contains("symbol") && overrides.contains("symbols")) {
        throw std::runtime_error("config.json must specify either 'symbol' or 'symbols', not both.");
    }

    const auto strategy_cfg = payload.at("strategy");
    const auto override_strategy_cfg =
        overrides.contains("strategy") ? overrides.at("strategy") : nlohmann::json::object();
    if (!override_strategy_cfg.is_object()) {
        throw std::runtime_error("config.json 'strategy' must be an object.");
    }

    const std::map<std::string, std::string> deprecated_fields{
        {"entry_slip_ticks", "use maker/taker execution rules instead of ask+slip entry pricing"},
        {"limit_tp_ticks", "use strategy.profit_ticks for passive TP distance"},
        {"limit_tp_timeout", "TP orders no longer use a timeout-based repost policy"},
        {"min_secondary_score", "the secondary-score gate was removed and is no longer used by the strategy"},
    };
    for (const auto& [field_name, guidance] : deprecated_fields) {
        if (override_strategy_cfg.contains(field_name)) {
            throw std::runtime_error(
                "config.json strategy." + field_name + " has been removed; " + guidance + "."
            );
        }
    }

    std::vector<SymbolConfig> symbols;
    if (overrides.contains("symbols")) {
        const auto& symbols_cfg = overrides.at("symbols");
        if (!symbols_cfg.is_array() || symbols_cfg.empty()) {
            throw std::runtime_error("config.json 'symbols' must be a non-empty list.");
        }
        for (std::size_t index = 0; index < symbols_cfg.size(); ++index) {
            if (!symbols_cfg.at(index).is_object()) {
                throw std::runtime_error("every entry in config.json 'symbols' must be an object.");
            }
            symbols.push_back(load_symbol_config(symbols_cfg.at(index), "symbols[" + std::to_string(index) + "]"));
        }
    } else {
        if (!payload.contains("symbol") || !payload.at("symbol").is_object()) {
            throw std::runtime_error("config.json must contain a 'symbol' section or a 'symbols' list.");
        }
        symbols.push_back(load_symbol_config(payload.at("symbol"), "symbol"));
    }
    validate_symbols(symbols);

    StrategyConfig strategy;
    strategy.trade_volume = strategy_cfg.value("trade_volume", 100);
    strategy.profit_ticks = strategy_cfg.value("profit_ticks", 2.0);
    strategy.aggressive_taker_profit_ticks = strategy_cfg.value("aggressive_taker_profit_ticks", 1.0);
    strategy.loss_ticks = strategy_cfg.value("loss_ticks", 3.0);
    strategy.exit_slip_ticks = strategy_cfg.value("exit_slip_ticks", 1.0);
    strategy.enable_limit_tp_order = strategy_cfg.value("enable_limit_tp_order", true);
    strategy.tp_only_mode = strategy_cfg.value("tp_only_mode", true);
    strategy.allow_flow_flip_in_tp_only = strategy_cfg.value("allow_flow_flip_in_tp_only", false);
    strategy.allow_stop_loss_in_tp_only = strategy_cfg.value("allow_stop_loss_in_tp_only", false);
    strategy.max_loss_per_trade = strategy_cfg.value("max_loss_per_trade", -500.0);
    strategy.book_imbalance_long = strategy_cfg.value("book_imbalance_long", 0.28);
    strategy.of_imbalance_long = strategy_cfg.value("of_imbalance_long", 0.12);
    strategy.tape_window_seconds = strategy_cfg.value("tape_window_seconds", 10);
    strategy.tape_imbalance_long = strategy_cfg.value("tape_imbalance_long", 0.15);
    strategy.mom_long_threshold = strategy_cfg.value("mom_long_threshold", 0.15);
    strategy.microprice_tilt_long = strategy_cfg.value("microprice_tilt_long", 0.10);
    strategy.confirm_ticks = strategy_cfg.value("confirm_ticks", 1);
    strategy.use_adaptive_confirm = strategy_cfg.value("use_adaptive_confirm", true);
    strategy.strong_signal_confirm = strategy_cfg.value("strong_signal_confirm", 1);
    strategy.entry_order_timeout = strategy_cfg.value("entry_order_timeout", 2.0);
    strategy.exit_order_timeout = strategy_cfg.value("exit_order_timeout", 3.0);
    strategy.entry_order_interval_ms = strategy_cfg.value("entry_order_interval_ms", 120);
    strategy.exit_order_interval_ms = strategy_cfg.value("exit_order_interval_ms", 80);
    strategy.limit_tp_order_interval_ms = strategy_cfg.value("limit_tp_order_interval_ms", 150);
    strategy.max_daily_loss = strategy_cfg.value("max_daily_loss", -20000.0);
    strategy.max_long_inventory = strategy_cfg.value("max_long_inventory", 500);
    strategy.max_tick_stale_seconds = strategy_cfg.value("max_tick_stale_seconds", 2.5);
    strategy.kabu_bidask_reversed = strategy_cfg.value("kabu_bidask_reversed", false);
    strategy.auto_fix_negative_spread = strategy_cfg.value("auto_fix_negative_spread", true);
    strategy.book_depth_levels = strategy_cfg.value("book_depth_levels", 5);
    strategy.book_decay = strategy_cfg.value("book_decay", 0.75);
    strategy.min_best_volume = strategy_cfg.value("min_best_volume", 100);
    strategy.min_spread_ticks = strategy_cfg.value("min_spread_ticks", 1.0);
    strategy.max_spread_ticks = strategy_cfg.value("max_spread_ticks", 4.0);
    strategy.max_mid_std_ticks = strategy_cfg.value("max_mid_std_ticks", 3.0);
    strategy.mid_std_window = strategy_cfg.value("mid_std_window", 60);
    strategy.use_tape_ofi = strategy_cfg.value("use_tape_ofi", true);
    strategy.use_microprice_tilt = strategy_cfg.value("use_microprice_tilt", true);
    strategy.aggressive_taker_mode = strategy_cfg.value("aggressive_taker_mode", false);
    strategy.aggressive_taker_entry_score = strategy_cfg.value("aggressive_taker_entry_score", 11);
    strategy.maker_score_threshold = strategy_cfg.value("maker_score_threshold", 6);
    strategy.taker_score_threshold = strategy_cfg.value("taker_score_threshold", 9);
    strategy.max_scale_in_per_round_trip = strategy_cfg.value("max_scale_in_per_round_trip", 1);
    strategy.max_position_hold_seconds = strategy_cfg.value("max_position_hold_seconds", 120.0);
    strategy.max_limit_tp_retries = strategy_cfg.value("max_limit_tp_retries", 5);
    strategy.strong_signal_multiplier = strategy_cfg.value("strong_signal_multiplier", 1.5);
    strategy.flow_flip_threshold = strategy_cfg.value("flow_flip_threshold", 0.18);
    strategy.cooling_seconds = strategy_cfg.value("cooling_seconds", 60);
    strategy.cooling_consecutive_losses = strategy_cfg.value("cooling_consecutive_losses", 0);
    strategy.signal_expire_seconds = strategy_cfg.value("signal_expire_seconds", 3.0);
    strategy.limit_tp_delay_seconds = strategy_cfg.value("limit_tp_delay_seconds", 0.5);
    strategy.min_order_lifetime_ms = strategy_cfg.value("min_order_lifetime_ms", 150);
    strategy.max_requotes_per_minute = strategy_cfg.value("max_requotes_per_minute", 30);
    strategy.allow_aggressive_exit = strategy_cfg.value("allow_aggressive_exit", false);
    strategy.market_open = strategy_cfg.value("market_open", std::string("09:00"));
    strategy.market_close = strategy_cfg.value("market_close", std::string("15:30"));
    strategy.lunch_break_start = strategy_cfg.value("lunch_break_start", std::string("11:30"));
    strategy.lunch_break_end = strategy_cfg.value("lunch_break_end", std::string("12:30"));
    strategy.commission_per_share = strategy_cfg.value("commission_per_share", 0.0);
    validate_strategy_modes(strategy);
    validate_strategy_ranges(strategy);
    validate_market_session(strategy);

    const int event_queue_maxsize = payload.value("event_queue_maxsize", 512);
    if (event_queue_maxsize <= 0) {
        throw std::runtime_error("config.json event_queue_maxsize must be > 0.");
    }
    const int kill_switch_poll_interval_ms = payload.value("kill_switch_poll_interval_ms", 250);
    if (kill_switch_poll_interval_ms < 0) {
        throw std::runtime_error("config.json kill_switch_poll_interval_ms must be >= 0.");
    }
    const int status_interval_s = payload.value("status_interval_s", 15);
    if (status_interval_s <= 0) {
        throw std::runtime_error("config.json status_interval_s must be > 0.");
    }
    const int reconcile_interval_ms = payload.value("reconcile_interval_ms", 500);
    if (reconcile_interval_ms <= 0) {
        throw std::runtime_error("config.json reconcile_interval_ms must be > 0.");
    }
    const int timer_interval_ms = payload.value("timer_interval_ms", 50);
    if (timer_interval_ms <= 0) {
        throw std::runtime_error("config.json timer_interval_ms must be > 0.");
    }
    const int markout_seconds = payload.value("markout_seconds", 30);
    if (markout_seconds <= 0) {
        throw std::runtime_error("config.json markout_seconds must be > 0.");
    }
    const double rate_limit_per_second = payload.value("rate_limit_per_second", 4.0);
    if (rate_limit_per_second <= 0) {
        throw std::runtime_error("config.json rate_limit_per_second must be > 0.");
    }
    const double order_rate_limit_per_second = payload.value("order_rate_limit_per_second", rate_limit_per_second);
    if (order_rate_limit_per_second <= 0) {
        throw std::runtime_error("config.json order_rate_limit_per_second must be > 0.");
    }
    const double poll_rate_limit_per_second = payload.value("poll_rate_limit_per_second", rate_limit_per_second);
    if (poll_rate_limit_per_second <= 0) {
        throw std::runtime_error("config.json poll_rate_limit_per_second must be > 0.");
    }
    const double shutdown_emergency_timeout_s = payload.value("shutdown_emergency_timeout_s", 5.0);
    if (shutdown_emergency_timeout_s <= 0) {
        throw std::runtime_error("config.json shutdown_emergency_timeout_s must be > 0.");
    }
    const std::string alert_webhook_url = payload.value("alert_webhook_url", std::string());
    if (!alert_webhook_url.empty() &&
        alert_webhook_url.rfind("http://", 0) != 0 &&
        alert_webhook_url.rfind("https://", 0) != 0) {
        throw std::runtime_error("config.json alert_webhook_url must start with http:// or https://.");
    }
    const int alert_cooldown_seconds = payload.value("alert_cooldown_seconds", 300);
    if (alert_cooldown_seconds < 0) {
        throw std::runtime_error("config.json alert_cooldown_seconds must be >= 0.");
    }
    const double alert_timeout_s = payload.value("alert_timeout_s", 3.0);
    if (alert_timeout_s <= 0) {
        throw std::runtime_error("config.json alert_timeout_s must be > 0.");
    }

    const auto account_risk_cfg =
        payload.contains("account_risk") && payload.at("account_risk").is_object()
            ? payload.at("account_risk")
            : nlohmann::json::object();
    AccountRiskConfig account_risk;
    account_risk.enabled = account_risk_cfg.value("enabled", true);
    account_risk.max_daily_loss = account_risk_cfg.value("max_daily_loss", strategy.max_daily_loss);
    account_risk.max_total_long_inventory = account_risk_cfg.value("max_total_long_inventory", 0);
    account_risk.max_total_notional = account_risk_cfg.value("max_total_notional", 0.0);
    validate_account_risk(account_risk);

    AppConfig config;
    config.api_password = payload.value("api_password", std::string());
    config.base_url = payload.value("base_url", std::string("http://localhost:18080"));
    config.ws_url = payload.value("ws_url", std::string("ws://localhost:18080/kabusapi/websocket"));
    config.dry_run = payload.value("dry_run", true);
    config.status_interval_s = status_interval_s;
    config.reconcile_interval_ms = reconcile_interval_ms;
    config.timer_interval_ms = timer_interval_ms;
    config.event_queue_maxsize = event_queue_maxsize;
    config.kill_switch_path = payload.value("kill_switch_path", std::string("logs/kill-switch.json"));
    config.kill_switch_poll_interval_ms = kill_switch_poll_interval_ms;
    config.alert_webhook_url = alert_webhook_url;
    config.alert_cooldown_seconds = alert_cooldown_seconds;
    config.alert_timeout_s = alert_timeout_s;
    config.journal_path = payload.value("journal_path", std::string("trades.csv"));
    config.markout_seconds = markout_seconds;
    config.rate_limit_per_second = rate_limit_per_second;
    config.order_rate_limit_per_second = order_rate_limit_per_second;
    config.poll_rate_limit_per_second = poll_rate_limit_per_second;
    config.shutdown_emergency_timeout_s = shutdown_emergency_timeout_s;
    config.startup_retry_count = payload.value("startup_retry_count", 3);
    config.startup_retry_delay_s = payload.value("startup_retry_delay_s", 2.0);
    config.order_profile =
        OrderProfile::from_json(payload.contains("order_profile") ? payload.at("order_profile") : nlohmann::json::object());
    config.account_risk = account_risk;
    config.symbols = symbols;
    config.strategy = strategy;
    return config;
}

}  // namespace kabu::config
