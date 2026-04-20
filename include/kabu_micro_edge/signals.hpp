#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <tuple>
#include <vector>

#include "kabu_micro_edge/gateway.hpp"

namespace kabu::signals {

struct SignalPacket {
    std::int64_t ts_ns{0};
    double obi_raw{0.0};
    double lob_ofi_raw{0.0};
    double tape_ofi_raw{0.0};
    double micro_momentum_raw{0.0};
    double microprice_tilt_raw{0.0};
    double microprice{0.0};
    double mid{0.0};
    double obi_z{0.0};
    double lob_ofi_z{0.0};
    double tape_ofi_z{0.0};
    double micro_momentum_z{0.0};
    double microprice_tilt_z{0.0};
    double composite{0.0};
    double mid_std_ticks{0.0};
    int secondary_score{0};
    double microprice_gap_ticks{0.0};
    double integrated_ofi{0.0};
    double trade_burst_score{0.0};
};

inline void to_json(nlohmann::json& json, const SignalPacket& value) {
    json = {
        {"ts_ns", value.ts_ns},
        {"obi_raw", value.obi_raw},
        {"lob_ofi_raw", value.lob_ofi_raw},
        {"tape_ofi_raw", value.tape_ofi_raw},
        {"micro_momentum_raw", value.micro_momentum_raw},
        {"microprice_tilt_raw", value.microprice_tilt_raw},
        {"microprice", value.microprice},
        {"mid", value.mid},
        {"obi_z", value.obi_z},
        {"lob_ofi_z", value.lob_ofi_z},
        {"tape_ofi_z", value.tape_ofi_z},
        {"micro_momentum_z", value.micro_momentum_z},
        {"microprice_tilt_z", value.microprice_tilt_z},
        {"composite", value.composite},
        {"mid_std_ticks", value.mid_std_ticks},
        {"secondary_score", value.secondary_score},
        {"microprice_gap_ticks", value.microprice_gap_ticks},
        {"integrated_ofi", value.integrated_ofi},
        {"trade_burst_score", value.trade_burst_score},
    };
}

inline void from_json(const nlohmann::json& json, SignalPacket& value) {
    value.ts_ns = json.value("ts_ns", static_cast<std::int64_t>(0));
    value.obi_raw = json.value("obi_raw", 0.0);
    value.lob_ofi_raw = json.value("lob_ofi_raw", 0.0);
    value.tape_ofi_raw = json.value("tape_ofi_raw", 0.0);
    value.micro_momentum_raw = json.value("micro_momentum_raw", 0.0);
    value.microprice_tilt_raw = json.value("microprice_tilt_raw", 0.0);
    value.microprice = json.value("microprice", 0.0);
    value.mid = json.value("mid", 0.0);
    value.obi_z = json.value("obi_z", 0.0);
    value.lob_ofi_z = json.value("lob_ofi_z", 0.0);
    value.tape_ofi_z = json.value("tape_ofi_z", 0.0);
    value.micro_momentum_z = json.value("micro_momentum_z", 0.0);
    value.microprice_tilt_z = json.value("microprice_tilt_z", 0.0);
    value.composite = json.value("composite", 0.0);
    value.mid_std_ticks = json.value("mid_std_ticks", 0.0);
    value.secondary_score = json.value("secondary_score", 0);
    value.microprice_gap_ticks = json.value("microprice_gap_ticks", 0.0);
    value.integrated_ofi = json.value("integrated_ofi", 0.0);
    value.trade_burst_score = json.value("trade_burst_score", 0.0);
}

struct TopOfBookRepair {
    double bid{0.0};
    double ask{0.0};
    int bid_size{0};
    int ask_size{0};
    bool was_repaired{false};
    bool was_swapped{false};
};

class RollingZScore {
  public:
    explicit RollingZScore(int window)
        : window_(std::max(window, 1)), min_samples_(std::min(window_, 20)) {}

    double update(double value) {
        values_.push_back(value);
        sum_x_ += value;
        sum_x2_ += value * value;
        if (static_cast<int>(values_.size()) > window_) {
            const double removed = values_.front();
            values_.pop_front();
            sum_x_ -= removed;
            sum_x2_ -= removed * removed;
        }
        return score(value);
    }

    [[nodiscard]] double score(double value) const {
        const int count = static_cast<int>(values_.size());
        if (count < min_samples_) {
            return 0.0;
        }
        const double mean = sum_x_ / count;
        const double variance = std::max(sum_x2_ / count - mean * mean, 0.0);
        if (variance <= 1e-12) {
            return 0.0;
        }
        return (value - mean) / std::sqrt(variance);
    }

  private:
    int window_{1};
    int min_samples_{1};
    std::deque<double> values_;
    double sum_x_{0.0};
    double sum_x2_{0.0};
};

class RollingStdTicks {
  public:
    RollingStdTicks(int window, double tick_size)
        : window_(std::max(window, 2)), tick_size_(std::max(tick_size, 1e-9)) {}

    double update(double price) {
        values_.push_back(price);
        sum_x_ += price;
        sum_x2_ += price * price;
        if (static_cast<int>(values_.size()) > window_) {
            const double removed = values_.front();
            values_.pop_front();
            sum_x_ -= removed;
            sum_x2_ -= removed * removed;
        }
        const int count = static_cast<int>(values_.size());
        if (count < 5) {
            return 0.0;
        }
        const double mean = sum_x_ / count;
        const double variance = std::max(sum_x2_ / count - mean * mean, 0.0);
        return std::sqrt(variance) / tick_size_;
    }

  private:
    int window_{2};
    double tick_size_{1e-9};
    std::deque<double> values_;
    double sum_x_{0.0};
    double sum_x2_{0.0};
};

class TapePressure {
  public:
    explicit TapePressure(int window_seconds) : window_ns_(std::max(window_seconds, 1) * 1'000'000'000LL) {}

    double on_trade(const kabu::gateway::TradePrint& trade) {
        if (trade.side > 0) {
            events_.push_back({trade.ts_ns, trade.size, 0});
            buy_qty_ += trade.size;
            burst_events_.push_back({trade.ts_ns, trade.size, 0});
            burst_buy_qty_ += trade.size;
        } else if (trade.side < 0) {
            events_.push_back({trade.ts_ns, 0, trade.size});
            sell_qty_ += trade.size;
            burst_events_.push_back({trade.ts_ns, 0, trade.size});
            burst_sell_qty_ += trade.size;
        }
        trim(trade.ts_ns);
        trim_burst(trade.ts_ns);
        return current();
    }

    [[nodiscard]] double current() const {
        const int total = buy_qty_ + sell_qty_;
        return total <= 0 ? 0.0 : static_cast<double>(buy_qty_ - sell_qty_) / total;
    }

    [[nodiscard]] double burst() const {
        const int total = burst_buy_qty_ + burst_sell_qty_;
        return total <= 0 ? 0.0 : static_cast<double>(burst_buy_qty_ - burst_sell_qty_) / total;
    }

  private:
    struct Event {
        std::int64_t ts_ns{0};
        int buy_qty{0};
        int sell_qty{0};
    };

    void trim(std::int64_t now_ns) {
        while (!events_.empty() && now_ns - events_.front().ts_ns > window_ns_) {
            buy_qty_ -= events_.front().buy_qty;
            sell_qty_ -= events_.front().sell_qty;
            events_.pop_front();
        }
    }

    void trim_burst(std::int64_t now_ns) {
        while (!burst_events_.empty() && now_ns - burst_events_.front().ts_ns > burst_window_ns_) {
            burst_buy_qty_ -= burst_events_.front().buy_qty;
            burst_sell_qty_ -= burst_events_.front().sell_qty;
            burst_events_.pop_front();
        }
    }

    std::int64_t window_ns_{1'000'000'000LL};
    std::int64_t burst_window_ns_{500'000'000LL};
    std::deque<Event> events_;
    std::deque<Event> burst_events_;
    int buy_qty_{0};
    int sell_qty_{0};
    int burst_buy_qty_{0};
    int burst_sell_qty_{0};
};

inline TopOfBookRepair normalize_top_of_book(
    double bid,
    double ask,
    int bid_size,
    int ask_size,
    bool kabu_bidask_reversed,
    bool auto_fix_negative_spread
) {
    const bool need_swap = bid > 0 && ask > 0 && bid > ask && (kabu_bidask_reversed || auto_fix_negative_spread);
    return need_swap ? TopOfBookRepair{ask, bid, ask_size, bid_size, true, true}
                     : TopOfBookRepair{bid, ask, bid_size, ask_size, false, false};
}

inline kabu::gateway::BoardSnapshot normalize_board_snapshot(
    const kabu::gateway::BoardSnapshot& snapshot,
    bool kabu_bidask_reversed,
    bool auto_fix_negative_spread
) {
    const auto repair = normalize_top_of_book(
        snapshot.bid,
        snapshot.ask,
        snapshot.bid_size,
        snapshot.ask_size,
        kabu_bidask_reversed,
        auto_fix_negative_spread
    );
    if (!repair.was_swapped) {
        return snapshot;
    }
    auto repaired = snapshot;
    repaired.bid = repair.bid;
    repaired.ask = repair.ask;
    repaired.bid_size = repair.bid_size;
    repaired.ask_size = repair.ask_size;
    repaired.bids = snapshot.asks;
    repaired.asks = snapshot.bids;
    repaired.bid_sign = snapshot.ask_sign;
    repaired.ask_sign = snapshot.bid_sign;
    return repaired;
}

class MicroEdgeSignalEngine {
  public:
    MicroEdgeSignalEngine(
        double tick_size,
        int book_depth_levels,
        double book_decay,
        int tape_window_seconds,
        int mid_std_window,
        int min_best_volume,
        bool kabu_bidask_reversed,
        bool auto_fix_negative_spread,
        bool use_microprice_tilt,
        int zscore_window = 120
    )
        : tick_size_(std::max(tick_size, 1e-9)),
          book_depth_levels_(std::max(book_depth_levels, 1)),
          book_decay_(book_decay),
          min_best_volume_(std::max(min_best_volume, 1)),
          kabu_bidask_reversed_(kabu_bidask_reversed),
          auto_fix_negative_spread_(auto_fix_negative_spread),
          use_microprice_tilt_(use_microprice_tilt),
          tape_(tape_window_seconds),
          mid_std_(mid_std_window, tick_size_),
          z_obi_(zscore_window),
          z_lob_(zscore_window),
          z_tape_(zscore_window),
          z_mom_(zscore_window),
          z_tilt_(zscore_window) {}

    [[nodiscard]] kabu::gateway::BoardSnapshot sanitize_snapshot(const kabu::gateway::BoardSnapshot& snapshot) const {
        return normalize_board_snapshot(snapshot, kabu_bidask_reversed_, auto_fix_negative_spread_);
    }

    double on_trade(const kabu::gateway::TradePrint& trade) { return tape_.on_trade(trade); }

    SignalPacket on_board(const kabu::gateway::BoardSnapshot& snapshot) {
        const double obi_raw = book_imbalance(snapshot);
        const double lob_ofi_raw = lob_ofi(snapshot);
        const double tape_ofi_raw = tape_.current();
        const auto [microprice, micro_momentum_raw, microprice_tilt_raw] = micro_signals(snapshot);
        const double microprice_gap_ticks = microprice_tilt_raw;
        const double integrated_ofi = 0.5 * lob_ofi_raw + 0.5 * tape_ofi_raw;
        const double trade_burst_score = tape_.burst();
        const double mid_std_ticks = mid_std_.update(snapshot.mid());

        SignalPacket packet;
        packet.ts_ns = snapshot.ts_ns;
        packet.obi_raw = obi_raw;
        packet.lob_ofi_raw = lob_ofi_raw;
        packet.tape_ofi_raw = tape_ofi_raw;
        packet.micro_momentum_raw = micro_momentum_raw;
        packet.microprice_tilt_raw = microprice_tilt_raw;
        packet.microprice = microprice;
        packet.mid = snapshot.mid();
        packet.obi_z = z_obi_.update(obi_raw);
        packet.lob_ofi_z = z_lob_.update(lob_ofi_raw);
        packet.tape_ofi_z = z_tape_.update(tape_ofi_raw);
        packet.micro_momentum_z = z_mom_.update(micro_momentum_raw);
        packet.microprice_tilt_z = z_tilt_.update(microprice_tilt_raw);
        packet.composite =
            0.30 * packet.obi_z + 0.25 * packet.lob_ofi_z + 0.25 * packet.tape_ofi_z +
            0.12 * packet.micro_momentum_z + 0.08 * packet.microprice_tilt_z;
        packet.mid_std_ticks = mid_std_ticks;
        packet.microprice_gap_ticks = microprice_gap_ticks;
        packet.integrated_ofi = integrated_ofi;
        packet.trade_burst_score = trade_burst_score;
        last_board_ = snapshot;
        last_signal_ = packet;
        return packet;
    }

    SignalPacket refresh_from_latest_board(const kabu::gateway::BoardSnapshot& snapshot, std::optional<std::int64_t> ts_ns = std::nullopt) {
        if (!last_signal_.has_value()) {
            auto packet = on_board(snapshot);
            if (ts_ns.has_value() && *ts_ns > 0) {
                packet.ts_ns = *ts_ns;
                last_signal_ = packet;
            }
            return packet;
        }
        auto packet = *last_signal_;
        const double tape_ofi_raw = tape_.current();
        const double tape_ofi_z = z_tape_.score(tape_ofi_raw);
        packet.ts_ns = ts_ns.has_value() && *ts_ns > 0 ? *ts_ns : snapshot.ts_ns;
        packet.tape_ofi_raw = tape_ofi_raw;
        packet.tape_ofi_z = tape_ofi_z;
        packet.integrated_ofi = 0.5 * packet.lob_ofi_raw + 0.5 * tape_ofi_raw;
        packet.trade_burst_score = tape_.burst();
        packet.composite =
            0.30 * packet.obi_z + 0.25 * packet.lob_ofi_z + 0.25 * tape_ofi_z +
            0.12 * packet.micro_momentum_z + 0.08 * packet.microprice_tilt_z;
        last_signal_ = packet;
        return packet;
    }

  private:
    [[nodiscard]] double book_imbalance(const kabu::gateway::BoardSnapshot& snapshot) const {
        double bid_weight = 0.0;
        double ask_weight = 0.0;
        for (int level = 0; level < book_depth_levels_; ++level) {
            const double weight = std::pow(book_decay_, static_cast<double>(level));
            if (level < static_cast<int>(snapshot.bids.size())) {
                bid_weight += weight * snapshot.bids[level].size;
            }
            if (level < static_cast<int>(snapshot.asks.size())) {
                ask_weight += weight * snapshot.asks[level].size;
            }
        }
        const double total = bid_weight + ask_weight;
        return total <= 0 ? 0.0 : (bid_weight - ask_weight) / total;
    }

    [[nodiscard]] double lob_ofi(const kabu::gateway::BoardSnapshot& snapshot) const {
        if (!last_board_.has_value()) {
            return 0.0;
        }
        auto py_is_close = [](double lhs, double rhs) {
            constexpr double abs_tol = 1e-12;
            constexpr double rel_tol = 1e-9;
            const double diff = std::fabs(lhs - rhs);
            if (diff <= abs_tol) {
                return true;
            }
            return diff <= rel_tol * std::max(std::fabs(lhs), std::fabs(rhs));
        };

        double buy_delta = 0.0;
        double sell_delta = 0.0;
        for (int level = 0; level < book_depth_levels_; ++level) {
            const auto* curr_bid = level < static_cast<int>(snapshot.bids.size()) ? &snapshot.bids[level] : nullptr;
            const auto* curr_ask = level < static_cast<int>(snapshot.asks.size()) ? &snapshot.asks[level] : nullptr;
            const auto* prev_bid = level < static_cast<int>(last_board_->bids.size()) ? &last_board_->bids[level] : nullptr;
            const auto* prev_ask = level < static_cast<int>(last_board_->asks.size()) ? &last_board_->asks[level] : nullptr;

            if (prev_bid && curr_bid) {
                if (curr_bid->price > prev_bid->price) {
                    buy_delta += std::max(curr_bid->size, min_best_volume_);
                } else if (py_is_close(curr_bid->price, prev_bid->price)) {
                    const int diff = curr_bid->size - prev_bid->size;
                    if (diff > 0) {
                        buy_delta += diff;
                    } else if (diff < 0) {
                        sell_delta += -diff;
                    }
                } else {
                    sell_delta += std::max(prev_bid->size, min_best_volume_);
                }
            } else if (curr_bid && !prev_bid) {
                buy_delta += std::max(curr_bid->size, min_best_volume_);
            } else if (prev_bid && !curr_bid) {
                sell_delta += std::max(prev_bid->size, min_best_volume_);
            }

            if (prev_ask && curr_ask) {
                if (curr_ask->price < prev_ask->price) {
                    buy_delta += std::max(curr_ask->size, min_best_volume_);
                } else if (py_is_close(curr_ask->price, prev_ask->price)) {
                    const int diff = curr_ask->size - prev_ask->size;
                    if (diff > 0) {
                        sell_delta += diff;
                    } else if (diff < 0) {
                        buy_delta += -diff;
                    }
                } else {
                    sell_delta += std::max(prev_ask->size, min_best_volume_);
                }
            } else if (curr_ask && !prev_ask) {
                sell_delta += std::max(curr_ask->size, min_best_volume_);
            } else if (prev_ask && !curr_ask) {
                buy_delta += std::max(prev_ask->size, min_best_volume_);
            }
        }
        const double total = buy_delta + sell_delta;
        return total <= 0 ? 0.0 : (buy_delta - sell_delta) / total;
    }

    [[nodiscard]] std::tuple<double, double, double> micro_signals(const kabu::gateway::BoardSnapshot& snapshot) {
        const int total_size = snapshot.bid_size + snapshot.ask_size;
        const double microprice =
            total_size <= 0 ? snapshot.mid() : (snapshot.ask * snapshot.bid_size + snapshot.bid * snapshot.ask_size) / total_size;
        double micro_momentum = 0.0;
        if (!micro_ema_.has_value()) {
            micro_ema_ = microprice;
        } else {
            micro_momentum = (microprice - *micro_ema_) / tick_size_;
            micro_ema_ = 0.2 * microprice + 0.8 * *micro_ema_;
        }
        const double microprice_tilt = use_microprice_tilt_ ? (microprice - snapshot.mid()) / tick_size_ : 0.0;
        return {microprice, micro_momentum, microprice_tilt};
    }

    double tick_size_{1e-9};
    int book_depth_levels_{1};
    double book_decay_{0.75};
    int min_best_volume_{1};
    bool kabu_bidask_reversed_{false};
    bool auto_fix_negative_spread_{true};
    bool use_microprice_tilt_{true};
    TapePressure tape_;
    RollingStdTicks mid_std_;
    RollingZScore z_obi_;
    RollingZScore z_lob_;
    RollingZScore z_tape_;
    RollingZScore z_mom_;
    RollingZScore z_tilt_;
    std::optional<kabu::gateway::BoardSnapshot> last_board_;
    std::optional<double> micro_ema_;
    std::optional<SignalPacket> last_signal_;
};

}  // namespace kabu::signals
