#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace kabu::execution {

enum class ExecutionState { Flat, Opening, Open, Closing };
enum class QuoteMode { PassiveFairValue, QueueDefense, CloseOnly };

inline std::string to_string(ExecutionState state) {
    switch (state) {
    case ExecutionState::Flat:
        return "FLAT";
    case ExecutionState::Opening:
        return "OPENING";
    case ExecutionState::Open:
        return "OPEN";
    case ExecutionState::Closing:
        return "CLOSING";
    }
    return "FLAT";
}

inline std::string to_string(QuoteMode mode) {
    switch (mode) {
    case QuoteMode::PassiveFairValue:
        return "PASSIVE_FAIR_VALUE";
    case QuoteMode::QueueDefense:
        return "QUEUE_DEFENSE";
    case QuoteMode::CloseOnly:
        return "CLOSE_ONLY";
    }
    return "PASSIVE_FAIR_VALUE";
}

struct PriceDecision {
    double price{0.0};
    bool is_market{false};
    double edge_ticks{0.0};
};

struct WorkingOrder {
    std::string order_id;
    std::string purpose;
    int side{0};
    int qty{0};
    double price{0.0};
    bool is_market{false};
    std::int64_t sent_ts_ns{0};
    std::string reason;
    std::string mode{to_string(QuoteMode::PassiveFairValue)};
    int cum_qty{0};
    double avg_fill_price{0.0};
    bool cancel_requested{false};
    int queue_ahead_qty{0};
    int initial_queue_ahead_qty{0};
    int entry_score{0};
    std::string fill_reason;
};

struct Inventory {
    int side{0};
    int qty{0};
    double avg_price{0.0};
    std::int64_t opened_ts_ns{0};
    std::int64_t last_entry_fill_ts_ns{0};
    int entry_qty{0};
    int exit_qty{0};
    double exit_value{0.0};
    std::string entry_mode;
    int entry_score{0};
    std::string entry_fill_reason;
    int entry_queue_ahead_qty{0};
};

struct RoundTrip {
    std::string symbol;
    int side{0};
    int qty{0};
    double entry_price{0.0};
    double exit_price{0.0};
    std::int64_t entry_ts_ns{0};
    std::int64_t exit_ts_ns{0};
    double realized_pnl{0.0};
    std::string exit_reason;
    std::string entry_mode;
    int entry_score{0};
    std::string fill_reason;
    int queue_ahead_qty{0};
};

class RequoteBudget {
  public:
    explicit RequoteBudget(int max_requotes_per_minute)
        : max_requotes_per_minute_(std::max(max_requotes_per_minute, 0)) {}

    bool allow(std::int64_t now_ns) {
        trim(now_ns);
        return static_cast<int>(timestamps_.size()) < max_requotes_per_minute_;
    }

    void consume(std::int64_t now_ns) { timestamps_.push_back(now_ns); }

  private:
    void trim(std::int64_t now_ns) {
        constexpr std::int64_t window_ns = 60LL * 1'000'000'000LL;
        while (!timestamps_.empty() && now_ns - timestamps_.front() > window_ns) {
            timestamps_.pop_front();
        }
    }

    int max_requotes_per_minute_{0};
    std::deque<std::int64_t> timestamps_;
};

inline std::optional<int> extract_error_code(const nlohmann::json& payload) {
    auto try_object = [](const nlohmann::json& object) -> std::optional<int> {
        if (!object.is_object()) {
            return std::nullopt;
        }
        for (const char* key : {"Code", "ResultCode", "code", "result_code"}) {
            if (!object.contains(key) || object.at(key).is_null()) {
                continue;
            }
            try {
                if (object.at(key).is_number_integer()) {
                    return object.at(key).get<int>();
                }
                if (object.at(key).is_number()) {
                    return static_cast<int>(object.at(key).get<double>());
                }
                if (object.at(key).is_string()) {
                    return std::stoi(object.at(key).get<std::string>());
                }
            } catch (...) {
            }
        }
        return std::nullopt;
    };

    if (auto code = try_object(payload)) {
        return code;
    }
    if (payload.is_array()) {
        for (const auto& item : payload) {
            if (auto code = try_object(item)) {
                return code;
            }
        }
    }
    return std::nullopt;
}

}  // namespace kabu::execution
