#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config_types.hpp"

namespace kabu::app {

struct LiveSafetySnapshot {
    bool enabled{false};
    bool entry_blocked{false};
    std::string block_reason;
    std::int64_t entry_blocked_until_ns{0};
    int consecutive_rest_errors{0};
    std::string last_rest_error;
};

inline void to_json(nlohmann::json& json, const LiveSafetySnapshot& value) {
    json = {
        {"enabled", value.enabled},
        {"entry_blocked", value.entry_blocked},
        {"block_reason", value.block_reason},
        {"entry_blocked_until_ns", value.entry_blocked_until_ns},
        {"consecutive_rest_errors", value.consecutive_rest_errors},
        {"last_rest_error", value.last_rest_error},
    };
}

class LiveSafetyController {
  public:
    LiveSafetyController(config::LiveSafetyConfig config, bool active_live_mode)
        : config_(std::move(config)), active_live_mode_(active_live_mode && config_.enabled) {}

    void note_rest_error(std::int64_t now_ns, const std::string& message) {
        if (!active_live_mode_) {
            return;
        }
        last_rest_error_ = message;
        ++consecutive_rest_errors_;
        if (config_.rest_error_cooldown_seconds > 0) {
            const auto cooldown_ns =
                static_cast<std::int64_t>(config_.rest_error_cooldown_seconds * 1'000'000'000.0);
            entry_blocked_until_ns_ = std::max(entry_blocked_until_ns_, now_ns + cooldown_ns);
        }
        if (config_.max_consecutive_rest_errors > 0 &&
            consecutive_rest_errors_ >= config_.max_consecutive_rest_errors) {
            kill_switch_requested_ = config_.hard_kill_on_max_consecutive_rest_errors;
        }
    }

    void note_rest_success() {
        if (!active_live_mode_) {
            return;
        }
        consecutive_rest_errors_ = 0;
    }

    [[nodiscard]] std::pair<bool, std::string> can_enter(std::int64_t now_ns) const {
        if (!active_live_mode_) {
            return {true, ""};
        }
        if (now_ns < entry_blocked_until_ns_) {
            return {false, "live_safety_api_backoff"};
        }
        return {true, ""};
    }

    [[nodiscard]] LiveSafetySnapshot snapshot(std::int64_t now_ns) const {
        const auto gate = can_enter(now_ns);
        return {
            active_live_mode_,
            !gate.first,
            gate.second,
            entry_blocked_until_ns_,
            consecutive_rest_errors_,
            last_rest_error_,
        };
    }

    [[nodiscard]] bool should_activate_kill_switch() const { return kill_switch_requested_; }
    void clear_kill_switch_request() { kill_switch_requested_ = false; }

  private:
    config::LiveSafetyConfig config_;
    bool active_live_mode_{false};
    std::int64_t entry_blocked_until_ns_{0};
    int consecutive_rest_errors_{0};
    std::string last_rest_error_;
    bool kill_switch_requested_{false};
};

}  // namespace kabu::app
