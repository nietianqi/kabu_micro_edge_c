#pragma once

#include <cmath>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "kabu_micro_edge/common/time.hpp"
#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/execution.hpp"

namespace kabu::risk {

struct RiskSnapshot {
    double daily_pnl{0.0};
    int consecutive_losses{0};
    std::int64_t cooldown_until_ns{0};
    bool entry_blocked{false};
};

inline void to_json(nlohmann::json& json, const RiskSnapshot& value) {
    json = {
        {"daily_pnl", value.daily_pnl},
        {"consecutive_losses", value.consecutive_losses},
        {"cooldown_until_ns", value.cooldown_until_ns},
        {"entry_blocked", value.entry_blocked},
    };
}

inline void from_json(const nlohmann::json& json, RiskSnapshot& value) {
    value.daily_pnl = json.value("daily_pnl", 0.0);
    value.consecutive_losses = json.value("consecutive_losses", 0);
    value.cooldown_until_ns = json.value("cooldown_until_ns", static_cast<std::int64_t>(0));
    value.entry_blocked = json.value("entry_blocked", false);
}

class RiskController {
  public:
    RiskController(double max_daily_loss, int cooling_seconds, int cooling_consecutive_losses = 0)
        : max_daily_loss_(max_daily_loss),
          cooldown_ns_(std::max(cooling_seconds, 0) * 1'000'000'000LL),
          cooling_consecutive_losses_(std::max(cooling_consecutive_losses, 0)) {}

    [[nodiscard]] std::pair<bool, std::string> can_enter(std::int64_t now_ns) {
        const auto reason = entry_block_reason(now_ns);
        return {reason.empty(), reason};
    }

    void on_round_trip(const kabu::execution::RoundTrip& trade, std::optional<std::int64_t> now_ns = std::nullopt) {
        if (now_ns.has_value()) {
            roll_daily_state(*now_ns);
        }
        daily_pnl_ += trade.realized_pnl;
        if (trade.realized_pnl < 0) {
            ++consecutive_losses_;
            if (now_ns.has_value() && cooldown_ns_ > 0) {
                cooldown_until_ns_ = std::max(cooldown_until_ns_, *now_ns + cooldown_ns_);
            }
            if (cooling_consecutive_losses_ > 0 && consecutive_losses_ >= cooling_consecutive_losses_) {
                consecutive_losses_ = 0;
            }
        } else {
            consecutive_losses_ = 0;
        }
    }

    [[nodiscard]] RiskSnapshot snapshot(std::int64_t now_ns) {
        const auto reason = entry_block_reason(now_ns);
        return RiskSnapshot{daily_pnl_, consecutive_losses_, cooldown_until_ns_, !reason.empty()};
    }

    [[nodiscard]] int consecutive_losses() const { return consecutive_losses_; }

  private:
    [[nodiscard]] std::string entry_block_reason(std::int64_t now_ns) {
        roll_daily_state(now_ns);
        if (now_ns < cooldown_until_ns_) {
            return "cooldown";
        }
        if (max_daily_loss_ < 0 && daily_pnl_ <= max_daily_loss_) {
            return "max_daily_loss";
        }
        return "";
    }

    void roll_daily_state(std::int64_t now_ns) {
        const auto session_key = kabu::common::jst_day_key(now_ns);
        if (!daily_session_key_.has_value()) {
            daily_session_key_ = session_key;
            return;
        }
        if (*daily_session_key_ == session_key) {
            return;
        }
        daily_session_key_ = session_key;
        daily_pnl_ = 0.0;
        consecutive_losses_ = 0;
        if (cooldown_until_ns_ < now_ns) {
            cooldown_until_ns_ = 0;
        }
    }

    double max_daily_loss_{0.0};
    std::int64_t cooldown_ns_{0};
    int cooling_consecutive_losses_{0};
    double daily_pnl_{0.0};
    int consecutive_losses_{0};
    std::int64_t cooldown_until_ns_{0};
    std::optional<std::tuple<int, int, int>> daily_session_key_;
};

struct AccountExposure {
    std::string symbol;
    int inventory_qty{0};
    double inventory_price{0.0};
    int pending_entry_qty{0};
    double pending_entry_price{0.0};
};

struct AccountRiskSnapshot {
    double realized_pnl{0.0};
    int total_inventory_qty{0};
    int total_pending_entry_qty{0};
    int total_projected_qty{0};
    double total_notional{0.0};
    double projected_notional{0.0};
    bool entry_blocked{false};
    std::string block_reason;
};

inline void to_json(nlohmann::json& json, const AccountRiskSnapshot& value) {
    json = {
        {"realized_pnl", value.realized_pnl},
        {"total_inventory_qty", value.total_inventory_qty},
        {"total_pending_entry_qty", value.total_pending_entry_qty},
        {"total_projected_qty", value.total_projected_qty},
        {"total_notional", value.total_notional},
        {"projected_notional", value.projected_notional},
        {"entry_blocked", value.entry_blocked},
        {"block_reason", value.block_reason},
    };
}

inline void from_json(const nlohmann::json& json, AccountRiskSnapshot& value) {
    value.realized_pnl = json.value("realized_pnl", 0.0);
    value.total_inventory_qty = json.value("total_inventory_qty", 0);
    value.total_pending_entry_qty = json.value("total_pending_entry_qty", 0);
    value.total_projected_qty = json.value("total_projected_qty", 0);
    value.total_notional = json.value("total_notional", 0.0);
    value.projected_notional = json.value("projected_notional", 0.0);
    value.entry_blocked = json.value("entry_blocked", false);
    value.block_reason = json.value("block_reason", std::string{});
}

class AccountRiskController {
  public:
    explicit AccountRiskController(kabu::config::AccountRiskConfig config) : config_(std::move(config)) {}

    [[nodiscard]] AccountRiskSnapshot evaluate(
        const std::vector<AccountExposure>& exposures,
        double realized_pnl,
        int additional_qty = 0,
        double additional_price = 0.0
    ) const {
        int total_inventory_qty = 0;
        int total_pending_entry_qty = 0;
        double total_notional = 0.0;
        for (const auto& exposure : exposures) {
            const int inventory_qty = std::max(exposure.inventory_qty, 0);
            const int pending_entry_qty = std::max(exposure.pending_entry_qty, 0);
            const double inventory_price = std::max(exposure.inventory_price, 0.0);
            const double pending_entry_price = std::max(exposure.pending_entry_price, 0.0);
            total_inventory_qty += inventory_qty;
            total_pending_entry_qty += pending_entry_qty;
            total_notional += inventory_qty * inventory_price;
            total_notional += pending_entry_qty * pending_entry_price;
        }

        const int total_qty = total_inventory_qty + total_pending_entry_qty;
        const int total_projected_qty = total_qty + std::max(additional_qty, 0);
        const double projected_notional = total_notional + std::max(additional_qty, 0) * std::max(additional_price, 0.0);

        std::string block_reason;
        if (config_.enabled) {
            if (config_.max_daily_loss < 0 && realized_pnl <= config_.max_daily_loss) {
                block_reason = "account_max_daily_loss";
            } else if (config_.max_total_long_inventory > 0 &&
                       (total_qty > config_.max_total_long_inventory || total_projected_qty > config_.max_total_long_inventory)) {
                block_reason = "account_max_total_long_inventory";
            } else if (config_.max_total_notional > 0 &&
                       (total_notional > config_.max_total_notional || projected_notional > config_.max_total_notional)) {
                block_reason = "account_max_total_notional";
            }
        }

        return AccountRiskSnapshot{
            realized_pnl,
            total_inventory_qty,
            total_pending_entry_qty,
            total_projected_qty,
            std::round(total_notional * 1000.0) / 1000.0,
            std::round(projected_notional * 1000.0) / 1000.0,
            !block_reason.empty(),
            block_reason,
        };
    }

  private:
    kabu::config::AccountRiskConfig config_;
};

}  // namespace kabu::risk
