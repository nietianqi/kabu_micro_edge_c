#include <filesystem>

#include <gtest/gtest.h>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/strategy.hpp"

namespace {

kabu::gateway::BoardSnapshot make_board(
    double bid,
    double ask,
    int bid_size,
    int ask_size,
    std::int64_t ts_ns
) {
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
        snapshot.bids.push_back({bid - i * 0.5, bid_size + i * 100});
        snapshot.asks.push_back({ask + i * 0.5, ask_size + i * 25});
    }
    return snapshot;
}

kabu::gateway::TradePrint make_trade(std::int64_t ts_ns, double price, int side, int size = 1500) {
    return {"7269", 9, ts_ns, price, size, side, size};
}

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

}  // namespace

TEST(StrategyTest, StatusTopLevelKeysAreFrozen) {
    auto strategy = make_strategy();
    strategy.start();
    const auto status = strategy.status();
    EXPECT_EQ(status.get<nlohmann::json::object_t>().size(), 7U);
    EXPECT_TRUE(status.contains("symbol"));
    EXPECT_TRUE(status.contains("state"));
    EXPECT_TRUE(status.contains("signal"));
    EXPECT_TRUE(status.contains("risk"));
    EXPECT_TRUE(status.contains("execution"));
    EXPECT_TRUE(status.contains("runtime"));
    EXPECT_TRUE(status.contains("analytics"));
    EXPECT_EQ(status["runtime"]["queue_maxsize"].get<int>(), 512);
    EXPECT_TRUE(status["runtime"]["event_worker_running"].get<bool>());
}

TEST(StrategyTest, ExecutionSnapshotKeysAreFrozen) {
    auto strategy = make_strategy();
    strategy.start();
    const auto snapshot = strategy.execution().snapshot();
    EXPECT_TRUE(snapshot.contains("state"));
    EXPECT_TRUE(snapshot.contains("inventory_side"));
    EXPECT_TRUE(snapshot.contains("inventory_qty"));
    EXPECT_TRUE(snapshot.contains("inventory_price"));
    EXPECT_TRUE(snapshot.contains("working_order_id"));
    EXPECT_TRUE(snapshot.contains("entry_order_id"));
    EXPECT_TRUE(snapshot.contains("exit_order_id"));
    EXPECT_TRUE(snapshot.contains("active_order_ids"));
    EXPECT_TRUE(snapshot.contains("stats"));
    EXPECT_TRUE(snapshot.contains("ledger"));
}

TEST(StrategyTest, DrivesEntryTpLifecycleInDryRun) {
    auto strategy = make_strategy();
    strategy.start();

    const auto first_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00");
    const auto second_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00");
    const auto tp_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00");

    strategy.process_board(make_board(1734.0, 1734.5, 300, 600, first_ts));
    strategy.process_trade(make_trade(first_ts + 50'000'000, 1734.5, 1));
    strategy.process_board(make_board(1734.5, 1735.0, 1500, 100, second_ts));

    EXPECT_EQ(strategy.execution().inventory.qty, 100);
    EXPECT_FALSE(strategy.execution().has_working_entry());
    ASSERT_TRUE(strategy.execution().exit_order.has_value());
    EXPECT_EQ(strategy.execution().exit_order->reason, "limit_tp_quote");
    EXPECT_EQ(strategy.status()["execution"]["scale_in_count"].get<int>(), 1);

    strategy.on_timer(second_ts + 200'000'000);
    ASSERT_TRUE(strategy.execution().exit_order.has_value());
    EXPECT_EQ(strategy.execution().exit_order->reason, "limit_tp_quote");

    strategy.process_board(make_board(1736.0, 1736.5, 100, 1200, tp_ts));
    EXPECT_EQ(strategy.execution().inventory.qty, 0);
    EXPECT_GE(strategy.risk().snapshot(tp_ts).daily_pnl, 0.0);
    EXPECT_EQ(strategy.status()["execution"]["scale_in_count"].get<int>(), 0);
}

TEST(StrategyTest, KillSwitchBlocksNewEntries) {
    auto strategy = make_strategy();
    strategy.start();
    strategy.activate_kill_switch("manual_test", false);

    const auto ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00");
    strategy.process_board(make_board(1734.0, 1734.5, 1500, 100, ts));

    EXPECT_EQ(strategy.execution().inventory.qty, 0);
    EXPECT_FALSE(strategy.execution().has_working_entry());
    const auto status = strategy.status();
    EXPECT_TRUE(status["risk"]["kill_switch_active"].get<bool>());
    EXPECT_EQ(status["risk"]["kill_switch_reason"].get<std::string>(), "manual_test");
    EXPECT_EQ(status["risk"]["last_entry_block_reason"].get<std::string>(), "kill_switch");
}
