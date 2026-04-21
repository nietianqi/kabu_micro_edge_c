#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "kabu_micro_edge/gateway.hpp"

namespace kabu::oms {

enum class OrderStatus {
    NewPending,
    Working,
    PartiallyFilled,
    Filled,
    CancelPending,
    Canceled,
    Rejected,
    Unknown,
};

inline std::string to_string(OrderStatus status) {
    switch (status) {
    case OrderStatus::NewPending:
        return "NEW_PENDING";
    case OrderStatus::Working:
        return "WORKING";
    case OrderStatus::PartiallyFilled:
        return "PARTIALLY_FILLED";
    case OrderStatus::Filled:
        return "FILLED";
    case OrderStatus::CancelPending:
        return "CANCEL_PENDING";
    case OrderStatus::Canceled:
        return "CANCELED";
    case OrderStatus::Rejected:
        return "REJECTED";
    case OrderStatus::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

struct WorkingOrderRecord {
    std::string order_id;
    std::string symbol;
    int side{0};
    int qty{0};
    double price{0.0};
    OrderStatus status{OrderStatus::NewPending};
    int cum_qty{0};
    double avg_fill_price{0.0};
    OrderStatus broker_status{OrderStatus::Unknown};
    int broker_cum_qty{0};
    double broker_avg_fill_price{0.0};
    bool broker_final{false};
    std::string cancel_reason;
    std::map<std::string, std::string> tags;

    [[nodiscard]] int leaves_qty() const { return std::max(qty - cum_qty, 0); }
    [[nodiscard]] bool is_final() const {
        return status == OrderStatus::Filled || status == OrderStatus::Canceled || status == OrderStatus::Rejected;
    }
};

enum class BrokerSnapshotDisposition {
    Applied,
    Duplicate,
    Stale,
    FinalRepeat,
    UnknownOrder,
};

struct BrokerSnapshotUpdate {
    BrokerSnapshotDisposition disposition{BrokerSnapshotDisposition::UnknownOrder};
    OrderStatus broker_status{OrderStatus::Unknown};
    int broker_cum_qty{0};
    bool broker_final{false};
};

inline OrderStatus broker_order_status(const kabu::gateway::OrderSnapshot& snapshot) {
    const std::string status = snapshot.status();
    if (status == "filled") {
        return OrderStatus::Filled;
    }
    if (status == "cancelled") {
        return OrderStatus::Canceled;
    }
    if (status == "partial") {
        return OrderStatus::PartiallyFilled;
    }
    if (status == "working") {
        return OrderStatus::Working;
    }
    return OrderStatus::Unknown;
}

class OrderLedger {
  public:
    void add(const WorkingOrderRecord& record) {
        if (records_.contains(record.order_id)) {
            spdlog::warn("duplicate order_id={} overwriting existing record", record.order_id);
        }
        records_[record.order_id] = record;
    }

    [[nodiscard]] WorkingOrderRecord* get(const std::string& order_id) {
        auto it = records_.find(order_id);
        return it == records_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const WorkingOrderRecord* get(const std::string& order_id) const {
        auto it = records_.find(order_id);
        return it == records_.end() ? nullptr : &it->second;
    }

    void mark_working(const std::string& order_id) {
        if (auto* record = get(order_id); record != nullptr && !record->is_final()) {
            record->status = OrderStatus::Working;
        }
    }

    void mark_cancel_pending(const std::string& order_id, const std::string& reason = {}) {
        if (auto* record = get(order_id); record != nullptr && !record->is_final()) {
            record->status = OrderStatus::CancelPending;
            record->cancel_reason = reason;
        }
    }

    void apply_fill(const std::string& order_id, int fill_qty, double fill_price) {
        auto* record = get(order_id);
        if (record == nullptr || record->is_final() || fill_qty <= 0) {
            return;
        }
        const int old_cum_qty = record->cum_qty;
        const int new_cum_qty = std::min(record->qty, old_cum_qty + fill_qty);
        const int applied_qty = new_cum_qty - old_cum_qty;
        if (applied_qty <= 0) {
            return;
        }
        if (old_cum_qty == 0) {
            record->avg_fill_price = fill_price;
        } else {
            const double prev_value = old_cum_qty * record->avg_fill_price;
            const double fill_value = applied_qty * fill_price;
            record->avg_fill_price = (prev_value + fill_value) / new_cum_qty;
        }
        record->cum_qty = new_cum_qty;
        record->status = record->cum_qty >= record->qty ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
    }

    void mark_canceled(const std::string& order_id) {
        if (auto* record = get(order_id); record != nullptr && !record->is_final()) {
            record->status = OrderStatus::Canceled;
        }
    }

    void mark_rejected(const std::string& order_id) {
        if (auto* record = get(order_id); record != nullptr && !record->is_final()) {
            record->status = OrderStatus::Rejected;
        }
    }

    void mark_filled(const std::string& order_id) {
        if (auto* record = get(order_id); record != nullptr && !record->is_final()) {
            if (record->cum_qty < record->qty) {
                record->cum_qty = record->qty;
            }
            if (record->avg_fill_price <= 0) {
                record->avg_fill_price = record->price;
            }
            record->status = OrderStatus::Filled;
        }
    }

    [[nodiscard]] BrokerSnapshotUpdate observe_broker_snapshot(
        const std::string& order_id,
        const kabu::gateway::OrderSnapshot& snapshot
    ) {
        auto* record = get(order_id);
        if (record == nullptr) {
            return {};
        }

        const auto next_status = broker_order_status(snapshot);
        if (snapshot.cum_qty < record->broker_cum_qty) {
            return {BrokerSnapshotDisposition::Stale, record->broker_status, record->broker_cum_qty, record->broker_final};
        }

        const bool same_fill_price = std::abs(record->broker_avg_fill_price - snapshot.avg_fill_price) <= 1e-9;
        const bool same_progress = snapshot.cum_qty == record->broker_cum_qty && next_status == record->broker_status &&
                                   snapshot.is_final == record->broker_final && same_fill_price;
        if (same_progress) {
            return {
                record->broker_final ? BrokerSnapshotDisposition::FinalRepeat : BrokerSnapshotDisposition::Duplicate,
                record->broker_status,
                record->broker_cum_qty,
                record->broker_final,
            };
        }

        record->broker_status = next_status;
        record->broker_cum_qty = snapshot.cum_qty;
        record->broker_avg_fill_price = snapshot.avg_fill_price;
        record->broker_final = snapshot.is_final;
        return {BrokerSnapshotDisposition::Applied, record->broker_status, record->broker_cum_qty, record->broker_final};
    }

    [[nodiscard]] std::vector<WorkingOrderRecord> records() const {
        std::vector<WorkingOrderRecord> values;
        values.reserve(records_.size());
        for (const auto& [_, record] : records_) {
            values.push_back(record);
        }
        return values;
    }

    [[nodiscard]] nlohmann::json snapshot() const {
        nlohmann::json json = nlohmann::json::object();
        for (const auto& [order_id, record] : records_) {
            json[order_id] = {
                {"symbol", record.symbol},
                {"side", record.side},
                {"qty", record.qty},
                {"price", record.price},
                {"status", to_string(record.status)},
                {"cum_qty", record.cum_qty},
                {"avg_fill_price", record.avg_fill_price},
                {"broker_status", to_string(record.broker_status)},
                {"broker_cum_qty", record.broker_cum_qty},
                {"broker_avg_fill_price", record.broker_avg_fill_price},
                {"broker_final", record.broker_final},
                {"cancel_reason", record.cancel_reason},
                {"tags", record.tags},
            };
        }
        return json;
    }

  private:
    std::map<std::string, WorkingOrderRecord> records_;
};

struct PositionState {
    std::string symbol;
    int side{0};
    int qty{0};
    double avg_price{0.0};
    double realized_pnl{0.0};
};

class PositionLedger {
  public:
    PositionState& apply_fill(const std::string& symbol, int side, int qty, double price) {
        if (side != -1 && side != 1) {
            throw std::runtime_error("invalid side=" + std::to_string(side) + ": must be -1 (short) or +1 (long)");
        }
        auto& position = positions_[symbol];
        position.symbol = symbol;
        if (qty <= 0) {
            return position;
        }
        if (position.qty == 0) {
            position.side = side;
            position.qty = qty;
            position.avg_price = price;
            return position;
        }
        if (position.side == side) {
            const int total_qty = position.qty + qty;
            const double total_value = position.qty * position.avg_price + qty * price;
            position.qty = total_qty;
            position.avg_price = total_value / std::max(total_qty, 1);
            return position;
        }
        const int close_qty = std::min(position.qty, qty);
        position.realized_pnl += position.side * (price - position.avg_price) * close_qty;
        const int remaining_open_qty = position.qty - close_qty;
        const int residual_qty = qty - close_qty;
        if (remaining_open_qty > 0) {
            position.qty = remaining_open_qty;
            return position;
        }
        if (residual_qty > 0) {
            position.side = side;
            position.qty = residual_qty;
            position.avg_price = price;
            return position;
        }
        position.side = 0;
        position.qty = 0;
        position.avg_price = 0.0;
        return position;
    }

  private:
    std::map<std::string, PositionState> positions_;
};

struct ReconciliationIssue {
    std::string order_id;
    std::string local_status;
    std::string broker_status;
    std::string severity;
    std::string message;
};

inline std::pair<WorkingOrderRecord, std::optional<ReconciliationIssue>> reconcile_order_state(
    WorkingOrderRecord local,
    const kabu::gateway::OrderSnapshot& broker
) {
    std::optional<ReconciliationIssue> issue;
    const std::string broker_status = broker.status();
    local.broker_status = broker_order_status(broker);
    local.broker_cum_qty = broker.cum_qty;
    local.broker_avg_fill_price = broker.avg_fill_price;
    local.broker_final = broker.is_final;
    if (!local.is_final()) {
        if (broker_status == "filled") {
            local.status = OrderStatus::Filled;
        } else if (broker_status == "cancelled") {
            local.status = OrderStatus::Canceled;
        } else if (broker_status == "partial") {
            local.status = OrderStatus::PartiallyFilled;
        } else if (local.status == OrderStatus::NewPending) {
            local.status = OrderStatus::Working;
        }
    }
    if (broker.cum_qty < local.cum_qty) {
        issue = ReconciliationIssue{
            local.order_id,
            to_string(local.status),
            broker_status,
            "high",
            "broker cum_qty is behind local cum_qty",
        };
    }
    if (local.status == OrderStatus::Canceled && local.broker_status == OrderStatus::Filled) {
        issue = ReconciliationIssue{
            local.order_id,
            to_string(local.status),
            broker_status,
            "high",
            "broker reports a filled order that local state marked as canceled",
        };
    }
    if (local.status == OrderStatus::Filled && local.broker_status == OrderStatus::Canceled) {
        issue = ReconciliationIssue{
            local.order_id,
            to_string(local.status),
            broker_status,
            "high",
            "broker reports a canceled order that local state marked as filled",
        };
    }
    if (broker.cum_qty > local.cum_qty) {
        local.cum_qty = broker.cum_qty;
    }
    if (broker.avg_fill_price > 0) {
        local.avg_fill_price = broker.avg_fill_price;
    }
    return {local, issue};
}

}  // namespace kabu::oms
