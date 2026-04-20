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
