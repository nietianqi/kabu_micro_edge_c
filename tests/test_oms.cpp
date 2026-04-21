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

TEST(OmsTest, ObserveBrokerSnapshotClassifiesDuplicateStaleAndFinalRepeat) {
    kabu::oms::OrderLedger ledger;
    ledger.add({"A1", "9984", 1, 100, 100.0});
    ledger.mark_working("A1");

    kabu::gateway::OrderSnapshot partial;
    partial.order_id = "A1";
    partial.side = 1;
    partial.order_qty = 100;
    partial.cum_qty = 40;
    partial.price = 100.0;
    partial.avg_fill_price = 100.0;

    auto update = ledger.observe_broker_snapshot("A1", partial);
    EXPECT_EQ(update.disposition, kabu::oms::BrokerSnapshotDisposition::Applied);
    ASSERT_NE(ledger.get("A1"), nullptr);
    EXPECT_EQ(ledger.get("A1")->broker_status, kabu::oms::OrderStatus::PartiallyFilled);

    update = ledger.observe_broker_snapshot("A1", partial);
    EXPECT_EQ(update.disposition, kabu::oms::BrokerSnapshotDisposition::Duplicate);

    auto stale = partial;
    stale.cum_qty = 20;
    update = ledger.observe_broker_snapshot("A1", stale);
    EXPECT_EQ(update.disposition, kabu::oms::BrokerSnapshotDisposition::Stale);

    auto final_fill = partial;
    final_fill.cum_qty = 100;
    final_fill.avg_fill_price = 101.0;
    final_fill.is_final = true;
    final_fill.state_code = 5;
    final_fill.order_state_code = 5;
    update = ledger.observe_broker_snapshot("A1", final_fill);
    EXPECT_EQ(update.disposition, kabu::oms::BrokerSnapshotDisposition::Applied);

    update = ledger.observe_broker_snapshot("A1", final_fill);
    EXPECT_EQ(update.disposition, kabu::oms::BrokerSnapshotDisposition::FinalRepeat);
}

TEST(OmsTest, ReconcileOrderStateFlagsCanceledVsFilledMismatch) {
    kabu::oms::WorkingOrderRecord local{"A1", "9984", 1, 100, 100.0, kabu::oms::OrderStatus::Canceled, 0};
    kabu::gateway::OrderSnapshot broker;
    broker.order_id = "A1";
    broker.side = 1;
    broker.order_qty = 100;
    broker.cum_qty = 100;
    broker.leaves_qty = 0;
    broker.price = 100.0;
    broker.avg_fill_price = 100.5;
    broker.is_final = true;
    broker.state_code = 5;
    broker.order_state_code = 5;
    const auto [reconciled, issue] = kabu::oms::reconcile_order_state(local, broker);
    EXPECT_EQ(reconciled.broker_status, kabu::oms::OrderStatus::Filled);
    ASSERT_TRUE(issue.has_value());
    EXPECT_EQ(issue->severity, "high");
}
