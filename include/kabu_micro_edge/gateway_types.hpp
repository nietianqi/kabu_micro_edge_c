#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/common/time.hpp"

namespace kabu::gateway {

inline double parse_float(const nlohmann::json& value, double default_value = 0.0) {
    if (value.is_null()) {
        return default_value;
    }
    try {
        if (value.is_number()) {
            return value.get<double>();
        }
        if (value.is_string()) {
            return std::stod(value.get<std::string>());
        }
    } catch (...) {
    }
    return default_value;
}

inline int parse_int(const nlohmann::json& value, int default_value = 0) {
    if (value.is_null()) {
        return default_value;
    }
    try {
        if (value.is_number_integer()) {
            return value.get<int>();
        }
        if (value.is_number()) {
            return static_cast<int>(value.get<double>());
        }
        if (value.is_string()) {
            return static_cast<int>(std::stod(value.get<std::string>()));
        }
    } catch (...) {
    }
    return default_value;
}

inline std::int64_t to_ns(const nlohmann::json& value) {
    if (!value.is_string()) {
        return 0;
    }
    return kabu::common::parse_iso8601_to_ns(value.get<std::string>());
}

inline std::string kabu_side(int internal_side) {
    if (internal_side > 0) {
        return "2";
    }
    if (internal_side < 0) {
        return "1";
    }
    throw std::runtime_error("internal side must be +1 or -1");
}

inline int internal_side(const nlohmann::json& raw_side) {
    const std::string side = raw_side.is_string() ? raw_side.get<std::string>() : raw_side.dump();
    if (side == "2" || side == "BUY" || side == "Buy") {
        return 1;
    }
    if (side == "1" || side == "SELL" || side == "Sell") {
        return -1;
    }
    return 0;
}

struct Level {
    double price{0.0};
    int size{0};
};

struct BoardSnapshot {
    std::string symbol;
    int exchange{1};
    std::int64_t ts_ns{0};
    double bid{0.0};
    double ask{0.0};
    int bid_size{0};
    int ask_size{0};
    double last{0.0};
    int last_size{0};
    int volume{0};
    double vwap{0.0};
    std::vector<Level> bids;
    std::vector<Level> asks;
    bool duplicate{false};
    bool out_of_order{false};
    std::string bid_sign;
    std::string ask_sign;
    std::int64_t bid_ts_ns{0};
    std::int64_t ask_ts_ns{0};
    std::int64_t current_ts_ns{0};
    std::string ts_source;

    [[nodiscard]] double mid() const { return (bid + ask) / 2.0; }
    [[nodiscard]] double spread() const { return ask - bid; }
    [[nodiscard]] bool valid() const { return bid > 0 && ask > 0 && bid < ask; }
};

struct TradePrint {
    std::string symbol;
    int exchange{1};
    std::int64_t ts_ns{0};
    double price{0.0};
    int size{0};
    int side{0};
    int cumulative_volume{0};
};

struct OrderSnapshot {
    std::string order_id;
    int side{0};
    int order_qty{0};
    int cum_qty{0};
    int leaves_qty{0};
    double price{0.0};
    double avg_fill_price{0.0};
    int state_code{0};
    int order_state_code{0};
    bool is_final{false};
    std::int64_t fill_ts_ns{0};
    nlohmann::json raw{nlohmann::json::object()};

    [[nodiscard]] std::string status() const {
        if (is_final && cum_qty >= order_qty && order_qty > 0) {
            return "filled";
        }
        if (is_final) {
            return "cancelled";
        }
        if (cum_qty > 0) {
            return "partial";
        }
        return "working";
    }
};

struct PositionLot {
    std::string hold_id;
    std::string symbol;
    int exchange{1};
    int side{0};
    int qty{0};
    int closable_qty{0};
    double price{0.0};
    int margin_trade_type{0};
};

class KabuApiError : public std::runtime_error {
  public:
    KabuApiError(std::string message, int status = 0, nlohmann::json payload = {})
        : std::runtime_error(std::move(message)), status_(status), payload_(std::move(payload)) {}

    [[nodiscard]] int status() const { return status_; }
    [[nodiscard]] const nlohmann::json& payload() const { return payload_; }

  private:
    int status_{0};
    nlohmann::json payload_;
};

inline std::optional<int> extract_error_code(const nlohmann::json& payload) {
    auto try_object = [](const nlohmann::json& object) -> std::optional<int> {
        if (!object.is_object()) {
            return std::nullopt;
        }
        for (const char* key : {"Code", "ResultCode", "code", "result_code"}) {
            if (object.contains(key) && !object.at(key).is_null()) {
                return parse_int(object.at(key));
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

inline std::optional<std::string> extract_error_message(const nlohmann::json& payload) {
    auto try_object = [](const nlohmann::json& object) -> std::optional<std::string> {
        if (!object.is_object()) {
            return std::nullopt;
        }
        for (const char* key : {"Message", "Result", "message", "result"}) {
            if (object.contains(key) && !object.at(key).is_null()) {
                return object.at(key).is_string() ? std::optional<std::string>(object.at(key).get<std::string>())
                                                  : std::optional<std::string>(object.at(key).dump());
            }
        }
        return std::nullopt;
    };

    if (auto message = try_object(payload)) {
        return message;
    }
    if (payload.is_array()) {
        for (const auto& item : payload) {
            if (auto message = try_object(item)) {
                return message;
            }
        }
    }
    return std::nullopt;
}

inline bool is_tse_family_exchange(int exchange) { return exchange == 1 || exchange == 9 || exchange == 27; }
inline int normalize_margin_equity_exchange(int exchange) { return is_tse_family_exchange(exchange) ? 9 : exchange; }

inline void to_json(nlohmann::json& json, const Level& value) {
    json = {{"price", value.price}, {"size", value.size}};
}

inline void from_json(const nlohmann::json& json, Level& value) {
    value.price = json.at("price").get<double>();
    value.size = json.at("size").get<int>();
}

inline void to_json(nlohmann::json& json, const BoardSnapshot& value) {
    json = {
        {"symbol", value.symbol},
        {"exchange", value.exchange},
        {"ts_ns", value.ts_ns},
        {"bid", value.bid},
        {"ask", value.ask},
        {"bid_size", value.bid_size},
        {"ask_size", value.ask_size},
        {"last", value.last},
        {"last_size", value.last_size},
        {"volume", value.volume},
        {"vwap", value.vwap},
        {"bids", value.bids},
        {"asks", value.asks},
    };
}

inline void from_json(const nlohmann::json& json, BoardSnapshot& value) {
    value.symbol = json.at("symbol").get<std::string>();
    value.exchange = json.at("exchange").get<int>();
    value.ts_ns = json.at("ts_ns").get<std::int64_t>();
    value.bid = json.at("bid").get<double>();
    value.ask = json.at("ask").get<double>();
    value.bid_size = json.at("bid_size").get<int>();
    value.ask_size = json.at("ask_size").get<int>();
    value.last = json.value("last", (value.bid + value.ask) / 2.0);
    value.last_size = json.value("last_size", 0);
    value.volume = json.value("volume", 0);
    value.vwap = json.value("vwap", value.last);
    value.bids = json.at("bids").get<std::vector<Level>>();
    value.asks = json.at("asks").get<std::vector<Level>>();
}

inline void to_json(nlohmann::json& json, const TradePrint& value) {
    json = {
        {"symbol", value.symbol},
        {"exchange", value.exchange},
        {"ts_ns", value.ts_ns},
        {"price", value.price},
        {"size", value.size},
        {"side", value.side},
        {"cumulative_volume", value.cumulative_volume},
    };
}

inline void from_json(const nlohmann::json& json, TradePrint& value) {
    value.symbol = json.at("symbol").get<std::string>();
    value.exchange = json.at("exchange").get<int>();
    value.ts_ns = json.at("ts_ns").get<std::int64_t>();
    value.price = json.at("price").get<double>();
    value.size = json.at("size").get<int>();
    value.side = json.at("side").get<int>();
    value.cumulative_volume = json.at("cumulative_volume").get<int>();
}

}  // namespace kabu::gateway
