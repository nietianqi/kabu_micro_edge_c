#include <gtest/gtest.h>

#include "kabu_micro_edge/oms.hpp"

TEST(OmsTest, OrderLedgerPartialToFilled) {
    kabu::oms::OrderLedger ledger;
    ledger.add({"A1", "9984", 1, 100, 100.0});
    ledger.mark_working("A1");
    ledger.apply_fill("A1", 40, 100.0);
    ASSERT_NE(ledger.get("A1"), nullptr);
    EXPECT_EQ(ledger.get("A1")->status, kabu::oms::OrderStatus::PartiallyFilled);
    ledger.apply_fill("A1", 60, 101.0);
    EXPECT_EQ(ledger.get("A1")->status, kabu::oms::OrderStatus::Filled);
}

TEST(OmsTest, PositionLedgerFlipAndRealized) {
    kabu::oms::PositionLedger ledger;
    auto& state_a = ledger.apply_fill("9984", 1, 100, 100.0);
    EXPECT_EQ(state_a.qty, 100);
    auto& state_b = ledger.apply_fill("9984", -1, 150, 101.0);
    EXPECT_EQ(state_b.side, -1);
    EXPECT_EQ(state_b.qty, 50);
    EXPECT_GT(state_b.realized_pnl, 0.0);
}

TEST(OmsTest, ReconcileOrderStateDetectsInconsistency) {
    kabu::oms::WorkingOrderRecord local{"A1", "9984", 1, 100, 100.0, kabu::oms::OrderStatus::PartiallyFilled, 60};
    kabu::gateway::OrderSnapshot broker;
    broker.order_id = "A1";
    broker.side = 1;
    broker.order_qty = 100;
    broker.cum_qty = 20;
    broker.leaves_qty = 80;
    broker.price = 100.0;
    broker.avg_fill_price = 100.0;
    const auto [reconciled, issue] = kabu::oms::reconcile_order_state(local, broker);
    EXPECT_EQ(reconciled.order_id, "A1");
    EXPECT_TRUE(issue.has_value());
}
