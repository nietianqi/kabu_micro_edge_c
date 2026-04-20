#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kabu_micro_edge/app/runtime.hpp"
#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/replay.hpp"
#include "kabu_micro_edge/signals.hpp"
#include "kabu_micro_edge/strategy.hpp"
#include "kabu_micro_edge/strategy_policy.hpp"

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::path(KABU_MICRO_EDGE_SOURCE_DIR);
}

nlohmann::json load_fixture(const std::filesystem::path& relative_path) {
    const auto path = repo_root() / relative_path;
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(in);
}

kabu::config::AppConfig make_config() {
    auto config = kabu::config::load_config();
    config.symbol().symbol = "7269";
    config.symbol().exchange = 9;
    config.symbol().tick_size = 0.5;
    config.symbol().max_notional = 1'000'000;
    config.strategy.confirm_ticks = 1;
    config.strategy.strong_signal_confirm = 1;
    config.strategy.entry_order_interval_ms = 0;
    config.strategy.exit_order_interval_ms = 0;
    config.strategy.limit_tp_order_interval_ms = 0;
    config.strategy.limit_tp_delay_seconds = 0.0;
    config.strategy.aggressive_taker_mode = false;
    return config;
}

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
    auto config = make_config();
    return {config.symbol(), config.strategy, config.order_profile, true};
}

std::vector<kabu::replay::ReplayEvent> make_replay_events() {
    const auto t1 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00");
    const auto t2 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.200000+09:00");
    const auto t3 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00");
    const auto t4 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.700000+09:00");
    const auto t5 = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:01+09:00");
    return {
        {"board", make_board(1734.0, 1734.5, 300, 600, t1)},
        {"trade", make_trade(t2, 1734.5, 1)},
        {"board", make_board(1734.5, 1735.0, 1500, 100, t3)},
        {"timer", t4},
        {"board", make_board(1736.0, 1736.5, 100, 1200, t5)},
    };
}

void expect_near(double actual, double expected, double tolerance = 1e-9) {
    EXPECT_NEAR(actual, expected, tolerance);
}

}  // namespace

TEST(PythonOracleTest, BreakoutSignalMatchesFixtureCoreFields) {
    const auto config = make_config();
    kabu::signals::MicroEdgeSignalEngine engine(
        config.symbol().tick_size,
        config.strategy.book_depth_levels,
        config.strategy.book_decay,
        config.strategy.tape_window_seconds,
        config.strategy.mid_std_window,
        config.strategy.min_best_volume,
        config.strategy.kabu_bidask_reversed,
        config.strategy.auto_fix_negative_spread,
        config.strategy.use_microprice_tilt
    );

    const auto baseline = make_board(
        1734.0,
        1734.5,
        300,
        600,
        kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00")
    );
    const auto breakout = make_board(
        1734.5,
        1735.0,
        1500,
        100,
        kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00")
    );

    engine.on_board(baseline);
    engine.on_trade(make_trade(kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.200000+09:00"), 1734.5, 1));
    const auto packet = engine.on_board(breakout);
    const auto fixture =
        load_fixture("fixtures/python_oracle/signals/breakout_packet.json").get<kabu::signals::SignalPacket>();

    EXPECT_EQ(packet.ts_ns, fixture.ts_ns);
    expect_near(packet.obi_raw, fixture.obi_raw);
    expect_near(packet.lob_ofi_raw, fixture.lob_ofi_raw);
    expect_near(packet.tape_ofi_raw, fixture.tape_ofi_raw);
    expect_near(packet.micro_momentum_raw, fixture.micro_momentum_raw);
    expect_near(packet.microprice_tilt_raw, fixture.microprice_tilt_raw);
    expect_near(packet.integrated_ofi, fixture.integrated_ofi);
    expect_near(packet.trade_burst_score, fixture.trade_burst_score);
}

TEST(PythonOracleTest, EntryDecisionMatchesFixture) {
    const auto config = make_config();
    const auto breakout_signal =
        load_fixture("fixtures/python_oracle/signals/breakout_packet.json").get<kabu::signals::SignalPacket>();
    const auto breakout_board = make_board(
        1734.5,
        1735.0,
        1500,
        100,
        kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.300000+09:00")
    );

    const auto decision = kabu::strategy::evaluate_long_signal(breakout_board, breakout_signal, config.strategy);
    const auto fixture =
        load_fixture("fixtures/python_oracle/strategy/entry_decision.json").get<kabu::strategy::EntryDecision>();

    EXPECT_EQ(decision.allow, fixture.allow);
    EXPECT_EQ(decision.reason, fixture.reason);
    EXPECT_EQ(decision.entry_mode, fixture.entry_mode);
    EXPECT_EQ(decision.entry_score, fixture.entry_score);
    EXPECT_EQ(decision.required_confirm, fixture.required_confirm);
}

TEST(PythonOracleTest, ReplaySnapshotsMatchPythonLifecycle) {
    auto strategy = make_strategy();
    strategy.start();

    kabu::replay::ReplayRunner replay(strategy);
    const auto snapshots = replay.run(make_replay_events());
    const auto fixture = load_fixture("fixtures/python_oracle/strategy/replay_snapshots.json");

    ASSERT_EQ(snapshots.size(), fixture.size());

    EXPECT_EQ(snapshots[0]["state"].get<std::string>(), fixture[0]["state"].get<std::string>());
    EXPECT_EQ(snapshots[0]["risk"]["last_entry_block_reason"].get<std::string>(), fixture[0]["risk"]["last_entry_block_reason"].get<std::string>());

    EXPECT_EQ(snapshots[2]["state"].get<std::string>(), fixture[2]["state"].get<std::string>());
    EXPECT_EQ(snapshots[2]["execution"]["working_order_id"].get<std::string>(), fixture[2]["execution"]["working_order_id"].get<std::string>());
    EXPECT_EQ(snapshots[2]["execution"]["last_entry_mode"].get<std::string>(), fixture[2]["execution"]["last_entry_mode"].get<std::string>());
    EXPECT_EQ(snapshots[2]["execution"]["last_entry_score"].get<int>(), fixture[2]["execution"]["last_entry_score"].get<int>());
    EXPECT_EQ(snapshots[2]["execution"]["scale_in_count"].get<int>(), fixture[2]["execution"]["scale_in_count"].get<int>());
    EXPECT_EQ(snapshots[2]["analytics"]["entry_modes"]["taker_fills"].get<int>(), fixture[2]["analytics"]["entry_modes"]["taker_fills"].get<int>());

    EXPECT_EQ(snapshots[4]["state"].get<std::string>(), fixture[4]["state"].get<std::string>());
    EXPECT_EQ(snapshots[4]["execution"]["inventory_qty"].get<int>(), fixture[4]["execution"]["inventory_qty"].get<int>());
    EXPECT_EQ(snapshots[4]["execution"]["paper_last_fill_reason"].get<std::string>(), fixture[4]["execution"]["paper_last_fill_reason"].get<std::string>());
    EXPECT_EQ(snapshots[4]["analytics"]["tp"]["touch_count"].get<int>(), fixture[4]["analytics"]["tp"]["touch_count"].get<int>());
    EXPECT_EQ(snapshots[4]["analytics"]["tp"]["fill_count"].get<int>(), fixture[4]["analytics"]["tp"]["fill_count"].get<int>());
}

TEST(PythonOracleTest, FinalStatusMatchesPythonFixtureCoreFields) {
    auto strategy = make_strategy();
    strategy.start();

    kabu::replay::ReplayRunner replay(strategy);
    const auto snapshots = replay.run(make_replay_events());
    (void)snapshots;
    const auto status = strategy.status();
    const auto fixture = load_fixture("fixtures/python_oracle/strategy/final_status.json");

    EXPECT_EQ(status.get<nlohmann::json::object_t>().size(), fixture.get<nlohmann::json::object_t>().size());
    EXPECT_EQ(status["symbol"].get<std::string>(), fixture["symbol"].get<std::string>());
    EXPECT_EQ(status["state"].get<std::string>(), fixture["state"].get<std::string>());
    EXPECT_EQ(status["risk"]["last_entry_block_reason"].get<std::string>(), fixture["risk"]["last_entry_block_reason"].get<std::string>());
    EXPECT_EQ(status["execution"]["paper_last_fill_reason"].get<std::string>(), fixture["execution"]["paper_last_fill_reason"].get<std::string>());
    EXPECT_EQ(status["execution"]["last_entry_mode"].get<std::string>(), fixture["execution"]["last_entry_mode"].get<std::string>());
    EXPECT_EQ(status["execution"]["last_entry_score"].get<int>(), fixture["execution"]["last_entry_score"].get<int>());
    EXPECT_EQ(status["execution"]["scale_in_count"].get<int>(), fixture["execution"]["scale_in_count"].get<int>());
    EXPECT_EQ(status["execution"]["stats"]["sent_orders"].get<int>(), fixture["execution"]["stats"]["sent_orders"].get<int>());
    EXPECT_EQ(status["execution"]["stats"]["fills"].get<int>(), fixture["execution"]["stats"]["fills"].get<int>());
    EXPECT_EQ(status["runtime"]["events_enqueued"].get<int>(), fixture["runtime"]["events_enqueued"].get<int>());
    EXPECT_EQ(status["runtime"]["events_processed"].get<int>(), fixture["runtime"]["events_processed"].get<int>());
    EXPECT_EQ(status["runtime"]["last_event_kind"].get<std::string>(), fixture["runtime"]["last_event_kind"].get<std::string>());
    EXPECT_EQ(status["analytics"]["tp"]["touch_count"].get<int>(), fixture["analytics"]["tp"]["touch_count"].get<int>());
    EXPECT_EQ(status["analytics"]["tp"]["fill_count"].get<int>(), fixture["analytics"]["tp"]["fill_count"].get<int>());
}

TEST(PythonOracleTest, RuntimeFixturesMatchPythonOracle) {
    auto config = make_config();
    auto strategy = std::make_shared<kabu::strategy::MicroEdgeStrategy>(
        config.symbol(),
        config.strategy,
        config.order_profile,
        true
    );
    strategy->start();
    strategy->execution().inventory.qty = 300;
    strategy->execution().inventory.avg_price = 1734.5;

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);

    const auto risk_snapshot = app.account_risk_snapshot(100, 1735.0);
    const auto risk_fixture =
        load_fixture("fixtures/python_oracle/runtime/account_risk_snapshot.json").get<kabu::risk::AccountRiskSnapshot>();
    EXPECT_EQ(risk_snapshot.total_inventory_qty, risk_fixture.total_inventory_qty);
    EXPECT_EQ(risk_snapshot.total_pending_entry_qty, risk_fixture.total_pending_entry_qty);
    EXPECT_EQ(risk_snapshot.total_projected_qty, risk_fixture.total_projected_qty);
    expect_near(risk_snapshot.total_notional, risk_fixture.total_notional);
    expect_near(risk_snapshot.projected_notional, risk_fixture.projected_notional);
    EXPECT_EQ(risk_snapshot.entry_blocked, risk_fixture.entry_blocked);
    EXPECT_EQ(risk_snapshot.block_reason, risk_fixture.block_reason);

    const auto reconcile_plan = app.build_reconcile_plan(*strategy);
    const auto reconcile_fixture =
        load_fixture("fixtures/python_oracle/runtime/reconcile_plan.json").get<kabu::app::ReconcilePlan>();
    EXPECT_EQ(reconcile_plan.mode, reconcile_fixture.mode);
    expect_near(reconcile_plan.sleep_s, reconcile_fixture.sleep_s);
    EXPECT_EQ(reconcile_plan.poll_positions, reconcile_fixture.poll_positions);
    EXPECT_EQ(reconcile_plan.order_ids, reconcile_fixture.order_ids);
}
