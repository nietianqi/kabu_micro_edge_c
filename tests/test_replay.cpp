#include <gtest/gtest.h>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/replay.hpp"

namespace {

kabu::strategy::MicroEdgeStrategy make_strategy() {
    auto config = kabu::config::load_config();
    config.symbol().symbol = "7269";
    config.symbol().exchange = 9;
    config.symbol().tick_size = 0.5;
    config.symbol().max_notional = 1'000'000;
    config.strategy.trade_volume = 100;
    config.strategy.book_imbalance_long = 0.35;
    config.strategy.of_imbalance_long = 0.05;
    config.strategy.tape_imbalance_long = 0.20;
    config.strategy.mom_long_threshold = 0.15;
    config.strategy.microprice_tilt_long = 0.20;
    config.strategy.confirm_ticks = 1;
    config.strategy.strong_signal_confirm = 1;
    config.strategy.entry_order_interval_ms = 0;
    config.strategy.exit_order_interval_ms = 0;
    config.strategy.limit_tp_order_interval_ms = 0;
    config.strategy.limit_tp_delay_seconds = 0.0;
    config.strategy.aggressive_taker_mode = false;
    return {config.symbol(), config.strategy, config.order_profile, true};
}

kabu::gateway::BoardSnapshot make_board(std::int64_t ts_ns, double bid, double ask, int bid_size, int ask_size) {
    kabu::gateway::BoardSnapshot snapshot;
    snapshot.symbol = "7269";
    snapshot.exchange = 9;
    snapshot.ts_ns = ts_ns;
    snapshot.bid = bid;
    snapshot.ask = ask;
    snapshot.bid_size = bid_size;
    snapshot.ask_size = ask_size;
    snapshot.last = (bid + ask) / 2.0;
    snapshot.vwap = snapshot.last;
    snapshot.volume = 1000;
    for (int i = 0; i < 5; ++i) {
        snapshot.bids.push_back({bid - i * 0.5, bid_size + i * 50});
        snapshot.asks.push_back({ask + i * 0.5, ask_size + i * 25});
    }
    return snapshot;
}

}  // namespace

TEST(ReplayTest, DrivesEntryAndTpLifecycle) {
    auto strategy = make_strategy();
    strategy.start();

    const auto t1 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00");
    const auto t2 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.200000+09:00");
    const auto t3 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00");
    const auto t4 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.700000+09:00");
    const auto t5 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00");

    const std::vector<kabu::replay::ReplayEvent> events = {
        {"board", make_board(t1, 1734.0, 1734.5, 300, 600)},
        {"trade", kabu::gateway::TradePrint{"7269", 9, t2, 1734.5, 1500, 1, 1500}},
        {"board", make_board(t3, 1734.5, 1735.0, 1500, 100)},
        {"timer", t4},
        {"board", make_board(t5, 1736.0, 1736.5, 100, 1200)},
    };

    kabu::replay::ReplayRunner replay(strategy);
    const auto snapshots = replay.run(events);

    ASSERT_EQ(snapshots.size(), 5U);
    EXPECT_EQ(snapshots.back()["execution"]["inventory_qty"].get<int>(), 0);
    EXPECT_GE(snapshots.back()["risk"]["daily_pnl"].get<double>(), 0.0);
}
