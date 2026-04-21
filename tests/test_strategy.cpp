#include <filesystem>
#include <memory>
#include <vector>

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

kabu::strategy::MicroEdgeStrategy make_strategy(std::shared_ptr<kabu::TradeJournal> journal = nullptr) {
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
    return {config.symbol(), config.strategy, config.order_profile, true, std::move(journal)};
}

kabu::strategy::MicroEdgeStrategy make_live_strategy(std::shared_ptr<kabu::TradeJournal> journal = nullptr) {
    auto config = kabu::config::load_config();
    config.symbol().symbol = "7269";
    config.symbol().exchange = 9;
    config.symbol().tick_size = 0.5;
    config.symbol().max_notional = 1'000'000;
    config.strategy.trade_volume = 100;
    config.strategy.entry_order_interval_ms = 0;
    config.strategy.exit_order_interval_ms = 0;
    config.strategy.limit_tp_order_interval_ms = 0;
    config.strategy.limit_tp_delay_seconds = 0.0;
    return {config.symbol(), config.strategy, config.order_profile, false, std::move(journal)};
}

std::filesystem::path make_temp_dir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kabu_micro_edge_c_strategy_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
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

TEST(StrategyTest, ScaleInRequiresLiveTpWorkflow) {
    auto strategy = make_strategy();
    strategy.start();

    const auto first_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00");
    const auto second_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00");
    strategy.execution().inventory.side = 1;
    strategy.execution().inventory.qty = 100;
    strategy.execution().inventory.avg_price = 1734.0;
    strategy.execution().inventory.opened_ts_ns = first_ts - 1'000'000;
    strategy.execution().exit_order = kabu::execution::WorkingOrder{
        "EXIT-1",
        "exit",
        -1,
        100,
        1735.0,
        false,
        first_ts - 1'000'000,
        "limit_tp_quote"
    };
    strategy.execution().exit_order->cancel_requested = true;

    strategy.process_board(make_board(1734.0, 1734.5, 300, 600, first_ts));
    strategy.process_trade(make_trade(first_ts + 50'000'000, 1734.5, 1));
    strategy.process_board(make_board(1734.5, 1735.0, 1500, 100, second_ts));

    EXPECT_FALSE(strategy.execution().has_working_entry());
    EXPECT_EQ(strategy.status()["risk"]["last_entry_block_reason"].get<std::string>(), "scale_in_blocked");
}

TEST(StrategyTest, EntryFillSchedulesEntryMarkoutInAnalytics) {
    const auto dir = make_temp_dir("entry_markout");
    auto journal = std::make_shared<kabu::TradeJournal>(
        dir / "trades.csv",
        30,
        std::vector<double>{},
        std::vector<double>{0.2, 0.5}
    );
    journal->open();

    auto strategy = make_strategy(journal);
    strategy.start();

    const auto first_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00");
    const auto second_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00");
    strategy.process_board(make_board(1734.0, 1734.5, 300, 600, first_ts));
    strategy.process_trade(make_trade(first_ts + 50'000'000, 1734.5, 1));
    strategy.process_board(make_board(1734.5, 1735.0, 1500, 100, second_ts));

    const auto markout = strategy.status()["analytics"]["markout"];
    EXPECT_EQ(markout["pending_post_exit_markouts"].get<int>(), 0);
    EXPECT_EQ(markout["pending_entry_markouts"].get<int>(), 2);
    EXPECT_EQ(markout["pending_markouts"].get<int>(), 2);

    journal->close();
}

TEST(StrategyTest, LiveReconcileAppliesBrokerOrderAndPositionSnapshots) {
    auto strategy = make_live_strategy();
    strategy.start();

    const auto fill_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00");
    strategy.execution().working_order = kabu::execution::WorkingOrder{
        "OID-ENTRY",
        "entry",
        1,
        100,
        1734.5,
        false,
        fill_ts - 1'000'000,
        "long_entry"
    };

    kabu::gateway::OrderSnapshot snapshot;
    snapshot.order_id = "OID-ENTRY";
    snapshot.symbol = "7269";
    snapshot.exchange = 9;
    snapshot.side = 1;
    snapshot.order_qty = 100;
    snapshot.cum_qty = 100;
    snapshot.price = 1734.5;
    snapshot.avg_fill_price = 1734.5;
    snapshot.is_final = true;
    snapshot.fill_ts_ns = fill_ts;

    const std::vector<nlohmann::json> positions{
        nlohmann::json{
            {"HoldID", "HOLD-1"},
            {"Symbol", "7269"},
            {"Exchange", 9},
            {"Side", "2"},
            {"LeavesQty", 100},
            {"ClosableQty", 100},
            {"Price", 1734.5},
            {"MarginTradeType", 1},
        }
    };

    strategy.reconcile_with_prefetched(positions, std::map<std::string, kabu::gateway::OrderSnapshot>{{"OID-ENTRY", snapshot}}, fill_ts);

    EXPECT_FALSE(strategy.execution().has_working_entry());
    EXPECT_EQ(strategy.execution().inventory.qty, 100);
    EXPECT_EQ(strategy.execution().inventory.side, 1);
    EXPECT_DOUBLE_EQ(strategy.execution().inventory.avg_price, 1734.5);
    EXPECT_EQ(strategy.execution().broker_hold_qty, 100);
    EXPECT_EQ(strategy.execution().broker_closable_qty, 100);
    EXPECT_FALSE(strategy.execution().has_external_inventory);
}

TEST(StrategyTest, LiveReconcileTracksExternalActiveOrders) {
    auto strategy = make_live_strategy();
    strategy.start();

    const std::vector<nlohmann::json> positions{};
    kabu::gateway::OrderSnapshot external_order;
    external_order.order_id = "OID-EXTERNAL";
    external_order.symbol = "7269";
    external_order.exchange = 9;
    external_order.side = 1;
    external_order.order_qty = 100;
    external_order.cum_qty = 0;
    external_order.price = 1734.5;
    external_order.is_final = false;

    strategy.reconcile_with_prefetched(
        positions,
        std::map<std::string, kabu::gateway::OrderSnapshot>{{"OID-EXTERNAL", external_order}},
        kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00")
    );

    EXPECT_TRUE(strategy.execution().has_external_active_orders);
    ASSERT_EQ(strategy.execution().broker_active_order_ids.size(), 1U);
    EXPECT_EQ(strategy.execution().broker_active_order_ids.front(), "OID-EXTERNAL");
}

TEST(StrategyTest, LiveReconcileRecoversExternalInventoryFromBrokerPositions) {
    auto strategy = make_live_strategy();
    strategy.start();

    const std::vector<nlohmann::json> positions{
        nlohmann::json{
            {"HoldID", "HOLD-1"},
            {"Symbol", "7269"},
            {"Exchange", 9},
            {"Side", "2"},
            {"LeavesQty", 200},
            {"ClosableQty", 200},
            {"Price", 1734.5},
            {"MarginTradeType", 1},
        }
    };

    strategy.reconcile_with_prefetched(
        positions,
        std::map<std::string, kabu::gateway::OrderSnapshot>{},
        kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00")
    );

    EXPECT_EQ(strategy.execution().inventory.qty, 0);
    EXPECT_EQ(strategy.execution().broker_hold_qty, 200);
    EXPECT_EQ(strategy.execution().broker_closable_qty, 200);
    EXPECT_TRUE(strategy.execution().has_external_inventory);
    EXPECT_TRUE(strategy.execution().has_external_inventory_conflict());
    EXPECT_FALSE(strategy.execution().manual_close_lock);
}

TEST(StrategyTest, LiveReconcileClearsRecoveredExternalInventoryWhenBrokerFlattens) {
    auto strategy = make_live_strategy();
    strategy.start();

    const auto ts_ns = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00");
    const std::vector<nlohmann::json> positions{
        nlohmann::json{
            {"HoldID", "HOLD-1"},
            {"Symbol", "7269"},
            {"Exchange", 9},
            {"Side", "2"},
            {"LeavesQty", 100},
            {"ClosableQty", 100},
            {"Price", 1734.5},
            {"MarginTradeType", 1},
        }
    };

    strategy.reconcile_with_prefetched(positions, std::map<std::string, kabu::gateway::OrderSnapshot>{}, ts_ns);
    ASSERT_TRUE(strategy.execution().has_external_inventory);

    strategy.reconcile_with_prefetched(
        std::vector<nlohmann::json>{},
        std::map<std::string, kabu::gateway::OrderSnapshot>{},
        ts_ns + 1'000'000
    );

    EXPECT_FALSE(strategy.execution().has_external_inventory);
    EXPECT_EQ(strategy.execution().broker_hold_qty, 0);
    EXPECT_EQ(strategy.execution().broker_closable_qty, 0);
    EXPECT_FALSE(strategy.execution().manual_close_lock);
}

TEST(StrategyTest, LiveReconcileClearsRecoveredExternalOrdersAfterBrokerFinalizesThem) {
    auto strategy = make_live_strategy();
    strategy.start();

    kabu::gateway::OrderSnapshot external_order;
    external_order.order_id = "OID-EXTERNAL";
    external_order.symbol = "7269";
    external_order.exchange = 9;
    external_order.side = 1;
    external_order.order_qty = 100;
    external_order.cum_qty = 0;
    external_order.price = 1734.5;
    external_order.is_final = false;

    const auto ts_ns = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00");
    strategy.reconcile_with_prefetched(
        std::vector<nlohmann::json>{},
        std::map<std::string, kabu::gateway::OrderSnapshot>{{"OID-EXTERNAL", external_order}},
        ts_ns
    );

    ASSERT_TRUE(strategy.execution().has_external_active_orders);

    external_order.is_final = true;
    external_order.state_code = 5;
    external_order.order_state_code = 5;
    strategy.reconcile_with_prefetched(
        std::vector<nlohmann::json>{},
        std::map<std::string, kabu::gateway::OrderSnapshot>{{"OID-EXTERNAL", external_order}},
        ts_ns + 1'000'000
    );

    EXPECT_FALSE(strategy.execution().has_external_active_orders);
    EXPECT_TRUE(strategy.execution().broker_active_order_ids.empty());
}
