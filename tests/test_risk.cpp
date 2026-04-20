#include <cstdio>

#include <gtest/gtest.h>

#include "kabu_micro_edge/risk.hpp"

namespace {

std::int64_t ts(int day, int hour, int minute = 0, int second = 0) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "2026-04-%02dT%02d:%02d:%02d+09:00", day, hour, minute, second);
    return kabu::common::parse_iso8601_to_ns(buffer);
}

}  // namespace

TEST(RiskTest, DailyPnlResetsOnNewDay) {
    kabu::risk::RiskController controller(-50.0, 60);
    kabu::execution::RoundTrip trade{"7269", 1, 100, 1734.0, 1733.4, ts(7, 14, 54), ts(7, 14, 55), -60.0, "stop_loss"};
    controller.on_round_trip(trade, ts(7, 14, 55));
    EXPECT_EQ(controller.can_enter(ts(7, 14, 56)).second, "max_daily_loss");
    EXPECT_TRUE(controller.can_enter(ts(8, 9, 0)).first);
    EXPECT_EQ(controller.snapshot(ts(8, 9, 0)).daily_pnl, 0.0);
}

TEST(RiskTest, ConsecutiveLossCooldownResetsCounter) {
    kabu::risk::RiskController controller(-10000.0, 600, 3);
    kabu::execution::RoundTrip trade{"7269", 1, 100, 1000.0, 999.0, ts(7, 10, 0), ts(7, 10, 1), -100.0, "stop_loss"};
    controller.on_round_trip(trade, ts(7, 10, 1));
    controller.on_round_trip(trade, ts(7, 10, 2));
    controller.on_round_trip(trade, ts(7, 10, 3));
    EXPECT_EQ(controller.consecutive_losses(), 0);
    EXPECT_EQ(controller.can_enter(ts(7, 10, 5)).second, "cooldown");
}

TEST(RiskTest, AccountRiskBlocksWhenExceeded) {
    kabu::risk::AccountRiskController controller({true, -20000.0, 500, 0.0});
    const auto snapshot = controller.evaluate({{"7269", 401, 1734.0, 0, 0.0}}, 0.0, 100, 1734.0);
    EXPECT_TRUE(snapshot.entry_blocked);
    EXPECT_EQ(snapshot.block_reason, "account_max_total_long_inventory");
}
