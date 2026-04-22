#pragma once

#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace kabu::config {

struct OrderProfile {
    std::string mode{"margin"};
    bool allow_short{false};
    int account_type{4};
    std::string cash_buy_fund_type{"02"};
    int cash_buy_deliv_type{2};
    std::string cash_sell_fund_type;
    int cash_sell_deliv_type{0};
    int margin_trade_type{1};
    std::string margin_open_fund_type{"11"};
    int margin_open_deliv_type{0};
    int margin_close_deliv_type{2};
    int front_order_type_limit{20};
    int front_order_type_market{10};

    static OrderProfile from_json(const nlohmann::json& payload) {
        OrderProfile profile;
        const auto& object = payload.is_object() ? payload : nlohmann::json::object();
        profile.mode = object.value("mode", std::string("margin"));
        profile.allow_short = object.value("allow_short", false);
        profile.account_type = object.value("account_type", 4);
        profile.cash_buy_fund_type = object.value("cash_buy_fund_type", std::string("02"));
        profile.cash_buy_deliv_type = object.value("cash_buy_deliv_type", 2);
        profile.cash_sell_fund_type = object.value("cash_sell_fund_type", std::string(""));
        profile.cash_sell_deliv_type = object.value("cash_sell_deliv_type", 0);
        profile.margin_trade_type = object.value("margin_trade_type", 1);
        profile.margin_open_fund_type = object.value("margin_open_fund_type", std::string("11"));
        profile.margin_open_deliv_type = object.value("margin_open_deliv_type", 0);
        profile.margin_close_deliv_type = object.value("margin_close_deliv_type", 2);
        profile.front_order_type_limit = object.value("front_order_type_limit", 20);
        profile.front_order_type_market = object.value("front_order_type_market", 10);
        return profile;
    }
};

struct AccountRiskConfig {
    bool enabled{true};
    double max_daily_loss{-20000.0};
    int max_total_long_inventory{0};
    double max_total_notional{0.0};
};

struct SymbolConfig {
    std::string symbol;
    int exchange{1};
    double tick_size{0.0};
    double max_notional{0.0};
    int lot_size{100};
    bool topix100{false};

    [[nodiscard]] int register_exchange() const {
        return (exchange == 9 || exchange == 27) ? 1 : exchange;
    }

    [[nodiscard]] std::pair<std::string, int> key() const { return {symbol, exchange}; }

    [[nodiscard]] std::vector<std::pair<std::string, int>> stream_keys() const {
        std::vector<std::pair<std::string, int>> keys{key()};
        const auto register_key = std::make_pair(symbol, register_exchange());
        if (register_key != keys.front()) {
            keys.push_back(register_key);
        }
        return keys;
    }
};

struct StrategyConfig {
    int trade_volume{100};
    double profit_ticks{2.0};
    double aggressive_taker_profit_ticks{1.0};
    double loss_ticks{3.0};
    double exit_slip_ticks{1.0};
    bool enable_limit_tp_order{true};
    bool tp_only_mode{true};
    bool allow_flow_flip_in_tp_only{false};
    bool allow_stop_loss_in_tp_only{false};
    double max_loss_per_trade{-500.0};
    double book_imbalance_long{0.35};
    double of_imbalance_long{0.25};
    int tape_window_seconds{10};
    double tape_imbalance_long{0.20};
    double mom_long_threshold{0.25};
    double microprice_tilt_long{0.25};
    int confirm_ticks{2};
    bool use_adaptive_confirm{true};
    int strong_signal_confirm{2};
    double entry_order_timeout{1.0};
    double exit_order_timeout{3.0};
    int entry_order_interval_ms{120};
    int exit_order_interval_ms{80};
    int limit_tp_order_interval_ms{150};
    double max_daily_loss{-20000.0};
    int max_long_inventory{500};
    double max_tick_stale_seconds{2.5};
    bool kabu_bidask_reversed{false};
    bool auto_fix_negative_spread{true};
    int book_depth_levels{5};
    double book_decay{0.75};
    int min_best_volume{100};
    double min_spread_ticks{1.0};
    double max_spread_ticks{3.0};
    double max_mid_std_ticks{2.5};
    int mid_std_window{60};
    bool use_tape_ofi{true};
    bool use_microprice_tilt{true};
    bool aggressive_taker_mode{true};
    int aggressive_taker_entry_score{11};
    int maker_score_threshold{6};
    int taker_score_threshold{9};
    int max_scale_in_per_round_trip{1};
    double max_position_hold_seconds{120.0};
    int max_limit_tp_retries{5};
    double strong_signal_multiplier{1.5};
    double flow_flip_threshold{0.18};
    int cooling_seconds{60};
    int cooling_consecutive_losses{0};
    double signal_expire_seconds{3.0};
    double limit_tp_delay_seconds{0.1};
    int min_order_lifetime_ms{150};
    int max_requotes_per_minute{30};
    bool allow_aggressive_exit{false};
    std::string market_open{"09:00"};
    std::string market_close{"15:30"};
    std::string lunch_break_start{"11:30"};
    std::string lunch_break_end{"12:30"};
    double commission_per_share{0.0};

    [[nodiscard]] std::pair<int, int> market_open_hm() const { return parse_hhmm(market_open, "market_open"); }
    [[nodiscard]] std::pair<int, int> market_close_hm() const { return parse_hhmm(market_close, "market_close"); }
    [[nodiscard]] std::pair<int, int> lunch_break_start_hm() const {
        return parse_hhmm(lunch_break_start, "lunch_break_start");
    }
    [[nodiscard]] std::pair<int, int> lunch_break_end_hm() const { return parse_hhmm(lunch_break_end, "lunch_break_end"); }

  private:
    [[nodiscard]] static std::pair<int, int> parse_hhmm(const std::string& value, const char* field_name) {
        const auto colon = value.find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error(std::string(field_name) + " must use HH:MM format");
        }
        const int hour = std::stoi(value.substr(0, colon));
        const int minute = std::stoi(value.substr(colon + 1));
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            throw std::runtime_error(std::string(field_name) + " must be a valid JST time");
        }
        return {hour, minute};
    }
};

struct AppConfig {
    std::string api_password;
    std::string base_url{"http://localhost:18080"};
    std::string ws_url{"ws://localhost:18080/kabusapi/websocket"};
    bool dry_run{true};
    bool debug_sendorder_log{false};
    int status_interval_s{15};
    int reconcile_interval_ms{500};
    int timer_interval_ms{50};
    int event_queue_maxsize{512};
    std::string kill_switch_path{"logs/kill-switch.json"};
    int kill_switch_poll_interval_ms{250};
    std::string alert_webhook_url;
    int alert_cooldown_seconds{300};
    double alert_timeout_s{3.0};
    std::string journal_path{"trades.csv"};
    int markout_seconds{30};
    double rate_limit_per_second{4.0};
    double order_rate_limit_per_second{4.0};
    double poll_rate_limit_per_second{4.0};
    double shutdown_emergency_timeout_s{5.0};
    int startup_retry_count{3};
    double startup_retry_delay_s{2.0};
    OrderProfile order_profile;
    AccountRiskConfig account_risk;
    std::vector<SymbolConfig> symbols;
    StrategyConfig strategy;

    SymbolConfig& symbol() {
        if (symbols.empty()) {
            throw std::runtime_error("config.json must define at least one symbol");
        }
        return symbols.front();
    }

    const SymbolConfig& symbol() const {
        if (symbols.empty()) {
            throw std::runtime_error("config.json must define at least one symbol");
        }
        return symbols.front();
    }

    [[nodiscard]] bool is_multi_symbol() const { return symbols.size() > 1; }

    [[nodiscard]] std::string journal_path_for(const SymbolConfig& symbol_cfg) const {
        std::string raw_path = journal_path;
        const std::map<std::string, std::string> values{
            {"symbol", symbol_cfg.symbol},
            {"exchange", std::to_string(symbol_cfg.exchange)},
            {"register_exchange", std::to_string(symbol_cfg.register_exchange())},
        };

        for (const auto& [name, value] : values) {
            const std::string token = "{" + name + "}";
            std::size_t pos = 0;
            while ((pos = raw_path.find(token, pos)) != std::string::npos) {
                raw_path.replace(pos, token.size(), value);
                pos += value.size();
            }
        }

        if (raw_path.find('{') != std::string::npos || raw_path.find('}') != std::string::npos) {
            throw std::runtime_error("journal_path contains unsupported placeholder");
        }

        if (symbols.size() <= 1 || journal_path.find("{symbol}") != std::string::npos ||
            journal_path.find("{exchange}") != std::string::npos ||
            journal_path.find("{register_exchange}") != std::string::npos) {
            return raw_path;
        }

        std::filesystem::path base_path(raw_path);
        int duplicate_symbol_count = 0;
        for (const auto& item : symbols) {
            if (item.symbol == symbol_cfg.symbol) {
                ++duplicate_symbol_count;
            }
        }
        const std::string suffix_symbol =
            duplicate_symbol_count == 1 ? symbol_cfg.symbol : symbol_cfg.symbol + "_" + std::to_string(symbol_cfg.exchange);
        const std::string suffix = base_path.has_extension() ? base_path.extension().string() : ".csv";
        const std::string stem = base_path.has_extension() ? base_path.stem().string() : base_path.filename().string();
        return (base_path.parent_path() / (stem + "_" + suffix_symbol + suffix)).string();
    }
};

}  // namespace kabu::config
