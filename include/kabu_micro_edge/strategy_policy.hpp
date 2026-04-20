#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/gateway.hpp"
#include "kabu_micro_edge/signals.hpp"

namespace kabu::strategy {

inline constexpr char ENTRY_MODE_MAKER[] = "maker";
inline constexpr char ENTRY_MODE_TAKER[] = "taker";

struct EntryDecision {
    bool allow{false};
    std::string reason;
    std::string entry_mode;
    int entry_score{0};
    int required_confirm{0};
};

inline void to_json(nlohmann::json& json, const EntryDecision& value) {
    json = {
        {"allow", value.allow},
        {"reason", value.reason},
        {"entry_mode", value.entry_mode},
        {"entry_score", value.entry_score},
        {"required_confirm", value.required_confirm},
    };
}

inline void from_json(const nlohmann::json& json, EntryDecision& value) {
    value.allow = json.value("allow", false);
    value.reason = json.value("reason", std::string{});
    value.entry_mode = json.value("entry_mode", std::string{});
    value.entry_score = json.value("entry_score", 0);
    value.required_confirm = json.value("required_confirm", 0);
}

struct EntryLayerDiagnostics {
    int direction_score{0};
    int confirmation_score{0};
    int trigger_score{0};
    int filter_score{0};
    bool book{false};
    bool microprice_tilt{false};
    bool lob_ofi{false};
    bool tape{false};
    bool micro_momentum{false};
    bool ask_light{false};
    bool integrated_ofi_positive{false};

    [[nodiscard]] int entry_score() const {
        return direction_score + confirmation_score + trigger_score + filter_score;
    }

    [[nodiscard]] nlohmann::json to_json() const {
        return {
            {"direction_score", direction_score},
            {"confirmation_score", confirmation_score},
            {"trigger_score", trigger_score},
            {"filter_score", filter_score},
            {"book", book},
            {"microprice_tilt", microprice_tilt},
            {"lob_ofi", lob_ofi},
            {"tape", tape},
            {"micro_momentum", micro_momentum},
            {"ask_light", ask_light},
            {"integrated_ofi_positive", integrated_ofi_positive},
        };
    }
};

inline void from_json(const nlohmann::json& json, EntryLayerDiagnostics& value) {
    value.direction_score = json.value("direction_score", 0);
    value.confirmation_score = json.value("confirmation_score", 0);
    value.trigger_score = json.value("trigger_score", 0);
    value.filter_score = json.value("filter_score", 0);
    value.book = json.value("book", false);
    value.microprice_tilt = json.value("microprice_tilt", false);
    value.lob_ofi = json.value("lob_ofi", false);
    value.tape = json.value("tape", false);
    value.micro_momentum = json.value("micro_momentum", false);
    value.ask_light = json.value("ask_light", false);
    value.integrated_ofi_positive = json.value("integrated_ofi_positive", false);
}

inline bool meets_threshold(double value, double threshold) {
    return value >= threshold || std::abs(value - threshold) <= 1e-12;
}

inline int top_depth_size(const std::vector<gateway::Level>& levels, int count) {
    int total = 0;
    for (int index = 0; index < std::min(static_cast<int>(levels.size()), std::max(count, 0)); ++index) {
        total += std::max(levels[index].size, 0);
    }
    return total;
}

inline EntryLayerDiagnostics entry_layer_diagnostics(
    const gateway::BoardSnapshot& snapshot,
    const signals::SignalPacket& signal,
    const config::StrategyConfig& strategy_cfg
) {
    EntryLayerDiagnostics diagnostics;
    diagnostics.book = meets_threshold(signal.obi_raw, strategy_cfg.book_imbalance_long);
    diagnostics.microprice_tilt = meets_threshold(signal.microprice_tilt_raw, strategy_cfg.microprice_tilt_long);
    diagnostics.lob_ofi = meets_threshold(signal.lob_ofi_raw, strategy_cfg.of_imbalance_long);
    diagnostics.tape = meets_threshold(signal.tape_ofi_raw, strategy_cfg.tape_imbalance_long);
    diagnostics.micro_momentum = meets_threshold(signal.micro_momentum_raw, strategy_cfg.mom_long_threshold);
    diagnostics.ask_light = top_depth_size(snapshot.bids, 2) > 0 &&
                            top_depth_size(snapshot.asks, 2) <= top_depth_size(snapshot.bids, 2);
    diagnostics.integrated_ofi_positive = signal.integrated_ofi > 0.0;
    diagnostics.direction_score = (diagnostics.book ? 2 : 0) + (diagnostics.microprice_tilt ? 2 : 0);
    diagnostics.confirmation_score = (diagnostics.lob_ofi ? 2 : 0) + (diagnostics.tape ? 3 : 0);
    diagnostics.trigger_score = diagnostics.micro_momentum ? 2 : 0;
    diagnostics.filter_score = (diagnostics.ask_light ? 1 : 0) + (diagnostics.integrated_ofi_positive ? 1 : 0);
    return diagnostics;
}

inline std::vector<std::pair<std::string, bool>> primary_long_checks(
    const signals::SignalPacket& signal,
    const config::StrategyConfig& strategy_cfg
) {
    return {
        {"book", meets_threshold(signal.obi_raw, strategy_cfg.book_imbalance_long)},
        {"microprice_tilt", meets_threshold(signal.microprice_tilt_raw, strategy_cfg.microprice_tilt_long)},
        {"lob_ofi", meets_threshold(signal.lob_ofi_raw, strategy_cfg.of_imbalance_long)},
        {"tape", meets_threshold(signal.tape_ofi_raw, strategy_cfg.tape_imbalance_long)},
        {"micro_momentum", meets_threshold(signal.micro_momentum_raw, strategy_cfg.mom_long_threshold)},
    };
}

inline bool passes_primary_long_checks(const std::vector<std::pair<std::string, bool>>& checks) {
    bool book = false;
    bool tilt = false;
    bool lob = false;
    bool tape = false;
    bool mom = false;
    for (const auto& [name, passed] : checks) {
        if (name == "book") book = passed;
        else if (name == "microprice_tilt") tilt = passed;
        else if (name == "lob_ofi") lob = passed;
        else if (name == "tape") tape = passed;
        else if (name == "micro_momentum") mom = passed;
    }
    return (book || tilt) && (lob || tape) && mom;
}

inline std::string reason_from_checks(const std::string& prefix, const std::vector<std::pair<std::string, bool>>& checks) {
    std::string failures;
    for (const auto& [name, passed] : checks) {
        if (passed) {
            continue;
        }
        if (!failures.empty()) {
            failures += ",";
        }
        failures += name;
    }
    return failures.empty() ? prefix : prefix + ":" + failures;
}

inline std::vector<std::pair<std::string, bool>> taker_breakout_checks(
    const gateway::BoardSnapshot& snapshot,
    const signals::SignalPacket& signal,
    const config::StrategyConfig& strategy_cfg
) {
    return {
        {"ask1_thin", snapshot.bid_size > 0 && snapshot.ask_size <= 0.5 * snapshot.bid_size},
        {"strong_tape", meets_threshold(signal.tape_ofi_raw, strategy_cfg.tape_imbalance_long * std::max(strategy_cfg.strong_signal_multiplier, 1.0))},
        {"tilt_up", meets_threshold(signal.microprice_tilt_raw, strategy_cfg.microprice_tilt_long)},
        {"integrated_ofi_positive", signal.integrated_ofi > 0.0},
        {"trade_burst_positive", signal.trade_burst_score > 0.0},
    };
}

inline bool has_taker_breakout_signal(
    const gateway::BoardSnapshot& snapshot,
    const signals::SignalPacket& signal,
    const config::StrategyConfig& strategy_cfg
) {
    for (const auto& [_, passed] : taker_breakout_checks(snapshot, signal, strategy_cfg)) {
        if (!passed) {
            return false;
        }
    }
    return true;
}

inline EntryDecision evaluate_long_signal(
    const gateway::BoardSnapshot& snapshot,
    const signals::SignalPacket& signal,
    const config::StrategyConfig& strategy_cfg
) {
    const auto diagnostics = entry_layer_diagnostics(snapshot, signal, strategy_cfg);
    const auto primary = primary_long_checks(signal, strategy_cfg);
    if (!passes_primary_long_checks(primary)) {
        return {false, reason_from_checks("primary", primary)};
    }

    const int entry_score = diagnostics.entry_score();
    if (entry_score < strategy_cfg.maker_score_threshold) {
        return {false, "score:" + std::to_string(entry_score) + "/" + std::to_string(strategy_cfg.maker_score_threshold)};
    }

    const auto breakout = taker_breakout_checks(snapshot, signal, strategy_cfg);
    bool taker_ready = true;
    for (const auto& [_, passed] : breakout) {
        taker_ready = taker_ready && passed;
    }

    std::string entry_mode = entry_score >= strategy_cfg.taker_score_threshold && taker_ready ? ENTRY_MODE_TAKER : ENTRY_MODE_MAKER;
    int required_confirm = strategy_cfg.confirm_ticks;
    if (strategy_cfg.use_adaptive_confirm) {
        bool strong_primary = true;
        for (const auto& [_, passed] : primary) {
            strong_primary = strong_primary && passed;
        }
        if (strong_primary) {
            required_confirm = std::max(required_confirm, strategy_cfg.strong_signal_confirm);
        }
    }
    if (strategy_cfg.aggressive_taker_mode && entry_score >= strategy_cfg.aggressive_taker_entry_score && taker_ready) {
        entry_mode = ENTRY_MODE_TAKER;
        required_confirm = std::max(required_confirm, 1);
    }
    return {true, "", entry_mode, entry_score, std::max(required_confirm, 1)};
}

}  // namespace kabu::strategy
