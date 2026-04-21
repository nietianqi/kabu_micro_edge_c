#include <gtest/gtest.h>

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/execution.hpp"

namespace {

kabu::gateway::BoardSnapshot make_board(
    double bid,
    double ask,
    int bid_size = 1200,
    int ask_size = 300,
    std::int64_t ts_ns = 0
) {
    kabu::gateway::BoardSnapshot snapshot;
    snapshot.symbol = "7269";
    snapshot.exchange = 9;
    snapshot.ts_ns = ts_ns == 0 ? kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00+09:00") : ts_ns;
    snapshot.bid = bid;
    snapshot.ask = ask;
    snapshot.bid_size = bid_size;
    snapshot.ask_size = ask_size;
    snapshot.last = (bid + ask) / 2.0;
    snapshot.vwap = snapshot.last;
    snapshot.volume = 1000;
    for (int i = 0; i < 5; ++i) {
        snapshot.bids.push_back({bid - i * 0.5, bid_size + i * 100});
        snapshot.asks.push_back({ask + i * 0.5, ask_size + i * 50});
    }
    return snapshot;
}

}  // namespace

TEST(ExecutionTest, OpenExplicitQuoteCrossPreservesEntryMetadata) {
    const auto config = kabu::config::load_config();
    kabu::execution::ExecutionController controller(
        "7269",
        9,
        config.order_profile,
        true,
        0.5,
        0.25,
        0.0,
        1000,
        0,
        30,
        false
    );

    ASSERT_TRUE(controller.open_explicit(1, 100, 1734.5, make_board(1734.0, 1734.5), "test", false, "maker", 5));
    EXPECT_EQ(controller.inventory.qty, 100);
    EXPECT_EQ(controller.inventory.entry_fill_reason, "quote_cross");
    EXPECT_EQ(controller.inventory.entry_mode, "maker");
    EXPECT_EQ(controller.inventory.entry_score, 5);
    EXPECT_FALSE(controller.working_order.has_value());
}

TEST(ExecutionTest, RestingOrderKeepsQueueAheadQty) {
    const auto config = kabu::config::load_config();
    kabu::execution::ExecutionController controller(
        "7269",
        9,
        config.order_profile,
        true,
        0.5,
        0.25,
        0.0,
        1000,
        0,
        30,
        false
    );

    ASSERT_TRUE(controller.open_explicit(1, 100, 1734.0, make_board(1734.0, 1734.5, 1400, 400), "test", false, "maker", 3));
    ASSERT_TRUE(controller.working_order.has_value());
    EXPECT_EQ(controller.inventory.qty, 0);
    EXPECT_EQ(controller.working_order->mode, "maker");
    EXPECT_EQ(controller.working_order->entry_score, 3);
    EXPECT_EQ(controller.working_order->queue_ahead_qty, 1400);
    EXPECT_EQ(controller.working_order->initial_queue_ahead_qty, 1400);
}

TEST(ExecutionTest, LiveOpenExplicitDoesNotResendWhileWorkingOrderActive) {
    const auto config = kabu::config::load_config();
    kabu::execution::ExecutionController controller(
        "7269",
        9,
        config.order_profile,
        false,
        0.5,
        0.25,
        0.0,
        1000,
        0,
        30,
        false
    );

    int entry_sends = 0;
    controller.set_live_order_callbacks(
        [&](int, int, double, bool) {
            ++entry_sends;
            return std::string("OID-ENTRY-1");
        },
        [&](int, int, double, bool) { return std::string("OID-EXIT-1"); },
        [&](const std::string&) {}
    );

    ASSERT_TRUE(controller.open_explicit(1, 100, 1734.0, make_board(1734.0, 1734.5), "test", false, "maker", 3));
    ASSERT_TRUE(controller.working_order.has_value());
    EXPECT_EQ(entry_sends, 1);

    EXPECT_FALSE(controller.open_explicit(1, 100, 1734.0, make_board(1734.0, 1734.5), "test", false, "maker", 3));
    EXPECT_EQ(entry_sends, 1);
    EXPECT_EQ(controller.working_order->order_id, "OID-ENTRY-1");
}

TEST(ExecutionTest, CloseDoesNotResendWhileCancelPending) {
    const auto config = kabu::config::load_config();
    kabu::execution::ExecutionController controller(
        "7269",
        9,
        config.order_profile,
        false,
        0.5,
        0.25,
        0.0,
        1000,
        0,
        30,
        false
    );

    controller.inventory.side = 1;
    controller.inventory.qty = 100;
    controller.inventory.avg_price = 1734.0;
    controller.inventory.opened_ts_ns = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00+09:00");

    int exit_sends = 0;
    int cancel_sends = 0;
    controller.set_live_order_callbacks(
        [&](int, int, double, bool) { return std::string("OID-ENTRY-1"); },
        [&](int, int, double, bool) {
            ++exit_sends;
            return std::string("OID-EXIT-1");
        },
        [&](const std::string&) { ++cancel_sends; }
    );

    ASSERT_TRUE(controller.close(make_board(1734.0, 1734.5), 0.0, "limit_tp_quote", false, 1735.0));
    ASSERT_TRUE(controller.exit_order.has_value());
    EXPECT_EQ(exit_sends, 1);

    ASSERT_TRUE(controller.cancel_exit_order("refresh_limit_tp"));
    ASSERT_TRUE(controller.exit_order.has_value());
    EXPECT_TRUE(controller.exit_order->cancel_requested);
    EXPECT_EQ(cancel_sends, 1);
    EXPECT_EQ(
        controller.snapshot().at("ledger").at("OID-EXIT-1").at("status").get<std::string>(),
        "CANCEL_PENDING"
    );

    EXPECT_FALSE(controller.close(make_board(1734.0, 1734.5), -9.0, "kill_switch_emergency", true, 1734.0));
    EXPECT_EQ(exit_sends, 1);
    EXPECT_EQ(cancel_sends, 1);
}

TEST(ExecutionTest, BrokerSnapshotIsIdempotentForPartialAndFinalFills) {
    const auto config = kabu::config::load_config();
    kabu::execution::ExecutionController controller(
        "7269",
        9,
        config.order_profile,
        false,
        0.5,
        0.25,
        0.0,
        1000,
        0,
        30,
        false
    );

    int entry_sends = 0;
    controller.set_live_order_callbacks(
        [&](int, int, double, bool) {
            ++entry_sends;
            return std::string("OID-ENTRY-1");
        },
        [&](int, int, double, bool) { return std::string("OID-EXIT-1"); },
        [&](const std::string&) {}
    );

    ASSERT_TRUE(controller.open_explicit(1, 100, 1734.0, make_board(1734.0, 1734.5), "test", false, "maker", 3));
    ASSERT_TRUE(controller.working_order.has_value());
    EXPECT_EQ(entry_sends, 1);

    kabu::gateway::OrderSnapshot partial;
    partial.order_id = "OID-ENTRY-1";
    partial.side = 1;
    partial.order_qty = 100;
    partial.cum_qty = 40;
    partial.price = 1734.0;
    partial.avg_fill_price = 1734.0;
    partial.is_final = false;

    controller.apply_broker_snapshot(partial);
    EXPECT_EQ(controller.inventory.qty, 40);
    ASSERT_TRUE(controller.working_order.has_value());
    EXPECT_EQ(controller.working_order->cum_qty, 40);

    controller.apply_broker_snapshot(partial);
    EXPECT_EQ(controller.inventory.qty, 40);
    ASSERT_TRUE(controller.working_order.has_value());
    EXPECT_EQ(controller.working_order->cum_qty, 40);

    auto stale = partial;
    stale.cum_qty = 20;
    controller.apply_broker_snapshot(stale);
    EXPECT_EQ(controller.inventory.qty, 40);
    ASSERT_TRUE(controller.working_order.has_value());
    EXPECT_EQ(controller.working_order->cum_qty, 40);

    kabu::gateway::OrderSnapshot final_fill = partial;
    final_fill.cum_qty = 100;
    final_fill.avg_fill_price = 1734.5;
    final_fill.is_final = true;
    final_fill.state_code = 5;
    final_fill.order_state_code = 5;
    controller.apply_broker_snapshot(final_fill);
    EXPECT_EQ(controller.inventory.qty, 100);
    EXPECT_FALSE(controller.working_order.has_value());

    controller.apply_broker_snapshot(final_fill);
    EXPECT_EQ(controller.inventory.qty, 100);
    EXPECT_TRUE(controller.drain_round_trips().empty());
}
