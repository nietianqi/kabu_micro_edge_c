#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "kabu_micro_edge/gateway_types.hpp"

namespace kabu::gateway {

inline bool is_fill_detail(const nlohmann::json& detail) {
    const int rec_type = parse_int(detail.value("RecType", nlohmann::json()));
    if (rec_type == 3 || rec_type == 8) {
        return true;
    }
    const std::string execution_id = parse_string(detail.value("ExecutionID", nlohmann::json()), std::string());
    return !execution_id.empty() && execution_id.rfind("E", 0) == 0;
}

class KabuAdapter {
  public:
    static std::vector<Level> parse_levels(const nlohmann::json& raw, const std::string& prefix, bool descending) {
        std::vector<Level> levels;
        for (int index = 1; index <= 10; ++index) {
            const std::string key = prefix + std::to_string(index);
            if (!raw.contains(key) || !raw.at(key).is_object()) {
                continue;
            }
            const auto& entry = raw.at(key);
            const double price = parse_float(entry.value("Price", nlohmann::json()));
            const int qty = parse_int(entry.value("Qty", nlohmann::json()));
            if (price > 0 && qty > 0) {
                levels.push_back(Level{price, qty});
            }
        }
        std::sort(levels.begin(), levels.end(), [descending](const Level& lhs, const Level& rhs) {
            return descending ? lhs.price > rhs.price : lhs.price < rhs.price;
        });
        return levels;
    }

    static std::optional<BoardSnapshot> board(const nlohmann::json& raw, const std::optional<BoardSnapshot>& prev) {
        BoardSnapshot snapshot;
        snapshot.symbol = parse_string(raw.value("Symbol", nlohmann::json()), std::string());
        snapshot.exchange = parse_int(raw.value("Exchange", nlohmann::json()), 1);
        snapshot.bids = parse_levels(raw, "Buy", true);
        snapshot.asks = parse_levels(raw, "Sell", false);

        const double bid_raw = parse_float(raw.value("AskPrice", nlohmann::json()));
        snapshot.bid = bid_raw > 0 ? bid_raw : (!snapshot.bids.empty() ? snapshot.bids.front().price : 0.0);
        const double ask_raw = parse_float(raw.value("BidPrice", nlohmann::json()));
        snapshot.ask = ask_raw > 0 ? ask_raw : (!snapshot.asks.empty() ? snapshot.asks.front().price : 0.0);
        const int bid_qty_raw = parse_int(raw.value("AskQty", nlohmann::json()));
        snapshot.bid_size = bid_qty_raw > 0 ? bid_qty_raw : (!snapshot.bids.empty() ? snapshot.bids.front().size : 0);
        const int ask_qty_raw = parse_int(raw.value("BidQty", nlohmann::json()));
        snapshot.ask_size = ask_qty_raw > 0 ? ask_qty_raw : (!snapshot.asks.empty() ? snapshot.asks.front().size : 0);
        snapshot.bid_sign = parse_string(raw.value("AskSign", nlohmann::json()), std::string());
        snapshot.ask_sign = parse_string(raw.value("BidSign", nlohmann::json()), std::string());
        snapshot.current_ts_ns = to_ns(raw.value("CurrentPriceTime", nlohmann::json()));
        snapshot.bid_ts_ns = to_ns(raw.value("AskTime", nlohmann::json()));
        snapshot.ask_ts_ns = to_ns(raw.value("BidTime", nlohmann::json()));
        if (snapshot.bid_ts_ns >= snapshot.ask_ts_ns && snapshot.bid_ts_ns >= snapshot.current_ts_ns && snapshot.bid_ts_ns > 0) {
            snapshot.ts_ns = snapshot.bid_ts_ns;
            snapshot.ts_source = "bid_time";
        } else if (snapshot.ask_ts_ns >= snapshot.bid_ts_ns && snapshot.ask_ts_ns >= snapshot.current_ts_ns && snapshot.ask_ts_ns > 0) {
            snapshot.ts_ns = snapshot.ask_ts_ns;
            snapshot.ts_source = "ask_time";
        } else if (snapshot.current_ts_ns > 0) {
            snapshot.ts_ns = snapshot.current_ts_ns;
            snapshot.ts_source = "current_price_time";
        } else {
            snapshot.ts_ns = 0;
            snapshot.ts_source = "no_exchange_time";
        }
        snapshot.last = parse_float(raw.value("CurrentPrice", nlohmann::json()));
        snapshot.last_size = parse_int(raw.value("CurrentPriceQty", nlohmann::json()));
        snapshot.volume = parse_int(raw.value("TradingVolume", nlohmann::json()));
        snapshot.vwap = parse_float(raw.value("VWAP", nlohmann::json()), snapshot.last);
        snapshot.out_of_order = prev.has_value() && snapshot.ts_ns > 0 && prev->ts_ns > 0 && snapshot.ts_ns < prev->ts_ns;
        snapshot.duplicate =
            prev.has_value() && !snapshot.out_of_order && snapshot.ts_ns > 0 && snapshot.ts_ns == prev->ts_ns &&
            std::abs(snapshot.bid - prev->bid) <= 1e-9 && std::abs(snapshot.ask - prev->ask) <= 1e-9 &&
            snapshot.bid_size == prev->bid_size && snapshot.ask_size == prev->ask_size && snapshot.volume == prev->volume;
        return snapshot;
    }

    static std::optional<TradePrint> trade(
        const nlohmann::json& raw,
        const std::optional<BoardSnapshot>& prev_board,
        int prev_volume,
        std::optional<double> last_trade_price
    ) {
        const int cumulative_volume = parse_int(raw.value("TradingVolume", nlohmann::json()));
        const int size = std::max(0, cumulative_volume - std::max(prev_volume, 0));
        if (size <= 0) {
            return std::nullopt;
        }

        const double price = parse_float(raw.value("CurrentPrice", nlohmann::json()));
        if (price <= 0) {
            return std::nullopt;
        }

        int side = 0;
        if (prev_board.has_value() && prev_board->valid()) {
            if (price >= prev_board->ask) {
                side = 1;
            } else if (price <= prev_board->bid) {
                side = -1;
            } else if (price > prev_board->mid()) {
                side = 1;
            } else if (price < prev_board->mid()) {
                side = -1;
            } else if (last_trade_price.has_value()) {
                if (price > *last_trade_price) {
                    side = 1;
                } else if (price < *last_trade_price) {
                    side = -1;
                }
            }
        }

        TradePrint trade;
        trade.symbol = parse_string(raw.value("Symbol", nlohmann::json()), std::string());
        trade.exchange = parse_int(raw.value("Exchange", nlohmann::json()), 1);
        trade.ts_ns = to_ns(raw.contains("TradingVolumeTime") ? raw.at("TradingVolumeTime") : raw.value("CurrentPriceTime", nlohmann::json()));
        trade.price = price;
        trade.size = size;
        trade.side = side;
        trade.cumulative_volume = cumulative_volume;
        return trade;
    }

    static std::optional<OrderSnapshot> order_snapshot(const nlohmann::json& raw) {
        OrderSnapshot snapshot;
        snapshot.order_id = parse_string(raw.contains("ID") ? raw.at("ID") : raw.value("OrderId", nlohmann::json()), std::string());
        if (snapshot.order_id.empty()) {
            return std::nullopt;
        }

        snapshot.symbol = parse_string(raw.value("Symbol", nlohmann::json()), std::string());
        snapshot.exchange = parse_int(raw.value("Exchange", nlohmann::json()), 1);
        snapshot.order_qty = parse_int(raw.contains("OrderQty") ? raw.at("OrderQty") : raw.value("Qty", nlohmann::json()));
        snapshot.cum_qty = parse_int(raw.value("CumQty", nlohmann::json()));
        snapshot.price = parse_float(raw.value("Price", nlohmann::json()));
        snapshot.state_code = parse_int(raw.value("State", nlohmann::json()));
        snapshot.order_state_code = parse_int(raw.value("OrderState", nlohmann::json()));
        snapshot.is_final = snapshot.state_code == 5 || snapshot.order_state_code == 5;

        double fill_weighted_value = 0.0;
        int fill_weighted_qty = 0;
        std::int64_t latest_fill_ts_ns = 0;
        const auto details = raw.contains("Details") && raw.at("Details").is_array() ? raw.at("Details") : nlohmann::json::array();
        for (const auto& detail : details) {
            if (!detail.is_object()) {
                continue;
            }
            const std::int64_t detail_ts = to_ns(
                detail.contains("ExecutionDay")   ? detail.at("ExecutionDay")
                : detail.contains("TransactTime") ? detail.at("TransactTime")
                : detail.contains("RecvTime")     ? detail.at("RecvTime")
                                                  : detail.value("Time", nlohmann::json())
            );
            if (is_fill_detail(detail)) {
                const int detail_qty = parse_int(detail.value("Qty", nlohmann::json()));
                const double detail_price = parse_float(detail.value("Price", nlohmann::json()));
                if (detail_qty > 0 && detail_price > 0) {
                    fill_weighted_value += detail_qty * detail_price;
                    fill_weighted_qty += detail_qty;
                    latest_fill_ts_ns = std::max(latest_fill_ts_ns, detail_ts);
                }
            }
        }

        if (snapshot.cum_qty > 0 && fill_weighted_qty > 0) {
            snapshot.avg_fill_price = fill_weighted_value / fill_weighted_qty;
        } else if (snapshot.cum_qty > 0 && snapshot.price > 0) {
            snapshot.avg_fill_price = snapshot.price;
        }
        snapshot.leaves_qty = std::max(snapshot.order_qty - snapshot.cum_qty, 0);
        snapshot.side = internal_side(raw.value("Side", nlohmann::json()));
        snapshot.fill_ts_ns = latest_fill_ts_ns;
        snapshot.raw = raw;
        return snapshot;
    }

    static std::optional<PositionLot> position_lot(const nlohmann::json& raw) {
        PositionLot lot;
        lot.hold_id = parse_string(raw.contains("HoldID") ? raw.at("HoldID") : raw.value("ExecutionID", nlohmann::json()), std::string());
        lot.symbol = parse_string(raw.value("Symbol", nlohmann::json()), std::string());
        const auto leaves_qty = raw.contains("LeavesQty") ? raw.at("LeavesQty") : nlohmann::json();
        const auto hold_qty = raw.contains("HoldQty") ? raw.at("HoldQty") : nlohmann::json();
        lot.qty = parse_int(!leaves_qty.is_null() ? leaves_qty : raw.value("Qty", nlohmann::json()));
        if (lot.hold_id.empty() || lot.symbol.empty() || lot.qty <= 0) {
            return std::nullopt;
        }

        const auto closable_qty = raw.contains("ClosableQty") ? raw.at("ClosableQty") : nlohmann::json();
        const int locked_qty = parse_int(hold_qty);
        lot.closable_qty = !closable_qty.is_null() ? parse_int(closable_qty) : std::max(lot.qty - locked_qty, 0);
        lot.exchange = parse_int(raw.value("Exchange", nlohmann::json()), 1);
        lot.side = internal_side(raw.value("Side", nlohmann::json()));
        lot.price = parse_float(raw.contains("Price") ? raw.at("Price") : raw.value("ExecutionPrice", nlohmann::json()));
        lot.margin_trade_type = parse_int(raw.value("MarginTradeType", nlohmann::json()), 0);
        return lot;
    }
};

}  // namespace kabu::gateway
