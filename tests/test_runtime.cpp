#include <filesystem>
#include <fstream>
#include <memory>

#include <gtest/gtest.h>

#include "kabu_micro_edge/app/runtime.hpp"

namespace {

std::shared_ptr<kabu::strategy::MicroEdgeStrategy> make_strategy(kabu::config::AppConfig& config, const std::string& symbol, int exchange) {
    config.symbol().symbol = symbol;
    config.symbol().exchange = exchange;
    config.symbol().tick_size = exchange == 9 ? 0.5 : 1.0;
    config.strategy.entry_order_interval_ms = 0;
    config.strategy.exit_order_interval_ms = 0;
    config.strategy.limit_tp_order_interval_ms = 0;
    config.strategy.limit_tp_delay_seconds = 0.0;
    auto strategy = std::make_shared<kabu::strategy::MicroEdgeStrategy>(config.symbol(), config.strategy, config.order_profile, true);
    strategy->start();
    return strategy;
}

kabu::gateway::BoardSnapshot make_board(const std::string& symbol, int exchange) {
    kabu::gateway::BoardSnapshot snapshot;
    snapshot.symbol = symbol;
    snapshot.exchange = exchange;
    snapshot.ts_ns = kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00+09:00");
    snapshot.bid = 1734.0;
    snapshot.ask = 1734.5;
    snapshot.bid_size = 1200;
    snapshot.ask_size = 100;
    snapshot.last = 1734.25;
    snapshot.vwap = 1734.25;
    snapshot.volume = 1000;
    for (int i = 0; i < 5; ++i) {
        snapshot.bids.push_back({snapshot.bid - i * 0.5, 1200 + i * 50});
        snapshot.asks.push_back({snapshot.ask + i * 0.5, 100 + i * 25});
    }
    return snapshot;
}

}  // namespace

TEST(RuntimeTest, ReconcilePlanUsesIdleBackoffWhenFlat) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config, "7269", 9);
    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);

    const auto plan = app.build_reconcile_plan(*strategy);
    EXPECT_EQ(plan.mode, "idle");
    EXPECT_TRUE(plan.order_ids.empty());
    EXPECT_GT(plan.sleep_s, config.reconcile_interval_ms / 1000.0);
}

TEST(RuntimeTest, ReconcilePlanStaysFastForOrdersAndInventory) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config, "7269", 9);
    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);

    strategy->execution().working_order = kabu::execution::WorkingOrder{"OID-1", "entry", 1, 100, 1734.0, false};
    auto order_plan = app.build_reconcile_plan(*strategy);
    EXPECT_EQ(order_plan.mode, "orders");

    strategy->execution().working_order.reset();
    strategy->execution().inventory.qty = 100;
    strategy->execution().inventory.side = 1;
    auto inventory_plan = app.build_reconcile_plan(*strategy);
    EXPECT_EQ(inventory_plan.mode, "inventory");
}

TEST(RuntimeTest, RoutesBoardBySymbolAndExchange) {
    auto config = kabu::config::load_config();
    auto strategy_a = make_strategy(config, "7269", 9);
    auto second_symbol = config.symbol();
    second_symbol.symbol = "7203";
    second_symbol.exchange = 1;
    second_symbol.tick_size = 1.0;
    auto strategy_b = std::make_shared<kabu::strategy::MicroEdgeStrategy>(second_symbol, config.strategy, config.order_profile, true);
    strategy_b->start();

    kabu::app::MicroEdgeApp app(config);
    app.register_strategy(config.symbol(), strategy_a);
    app.register_strategy(second_symbol, strategy_b);
    app.on_board(make_board("7203", 1));

    ASSERT_TRUE(strategy_b->latest_board().has_value());
    EXPECT_EQ(strategy_b->latest_board()->symbol, "7203");
    EXPECT_FALSE(strategy_a->latest_board().has_value());
}

TEST(RuntimeTest, AccountRiskSnapshotAggregatesInventoryAndPending) {
    auto config = kabu::config::load_config();
    config.account_risk.max_total_long_inventory = 499;
    auto strategy = make_strategy(config, "7269", 9);
    strategy->execution().inventory.qty = 300;
    strategy->execution().inventory.avg_price = 1734.5;
    strategy->execution().working_order = kabu::execution::WorkingOrder{"OID-1", "entry", 1, 100, 1735.0, false};

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);
    const auto snapshot = app.account_risk_snapshot(100, 1735.0);

    EXPECT_EQ(snapshot.total_inventory_qty, 300);
    EXPECT_EQ(snapshot.total_pending_entry_qty, 100);
    EXPECT_EQ(snapshot.total_projected_qty, 500);
    EXPECT_TRUE(snapshot.entry_blocked);
    EXPECT_EQ(snapshot.block_reason, "account_max_total_long_inventory");
}

TEST(RuntimeTest, AccountEntryGuardBlocksProjectedEntryThatExceedsLimit) {
    auto config = kabu::config::load_config();
    config.account_risk.max_total_long_inventory = 500;
    auto strategy = make_strategy(config, "7269", 9);
    strategy->execution().inventory.qty = 400;
    strategy->execution().inventory.avg_price = 1734.5;

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);
    const auto guard = app.make_account_entry_guard();
    const auto [allowed, reason] = guard(101, 1735.0);

    EXPECT_FALSE(allowed);
    EXPECT_EQ(reason, "account_max_total_long_inventory");
}

TEST(RuntimeTest, FileKillSwitchRequestActivatesSoftStop) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config, "7269", 9);

    const auto dir = std::filesystem::temp_directory_path() / "kabu_micro_edge_c_runtime";
    std::filesystem::create_directories(dir);
    const auto kill_switch_path = dir / "kill-switch.json";
    std::ofstream out(kill_switch_path);
    out << R"({"active": true, "mode": "soft", "reason": "ops_soft_stop"})";
    out.close();

    config.kill_switch_path = kill_switch_path.string();
    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);

    EXPECT_TRUE(app.poll_kill_switch());
    EXPECT_TRUE(app.kill_switch_active());
    EXPECT_EQ(app.kill_switch_reason(), "ops_soft_stop");
    EXPECT_FALSE(app.kill_switch_hard_close());
}

TEST(RuntimeTest, BuildRegisterPayloadUsesRegisterExchangeNormalization) {
    auto config = kabu::config::load_config();
    config.symbols.clear();
    config.symbols.push_back({"7269", 9, 0.5, 500000.0, false});
    config.symbols.push_back({"7203", 1, 1.0, 500000.0, false});

    kabu::app::MicroEdgeApp app(config);
    const auto payload = app.build_register_payload();

    ASSERT_EQ(payload.size(), 2U);
    EXPECT_EQ(payload[0].at("Symbol").get<std::string>(), "7269");
    EXPECT_EQ(payload[0].at("Exchange").get<int>(), 1);
    EXPECT_EQ(payload[1].at("Symbol").get<std::string>(), "7203");
    EXPECT_EQ(payload[1].at("Exchange").get<int>(), 1);
}

TEST(RuntimeTest, StartupWithRetryRequestsTokenThenRegistersSymbols) {
    auto config = kabu::config::load_config();
    config.api_password = "secret";
    config.startup_retry_count = 1;
    auto strategy = make_strategy(config, "7269", 9);

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);

    int token_attempts = 0;
    int register_attempts = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json&,
            bool,
            kabu::gateway::RequestLane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/token")) {
                ++token_attempts;
                if (token_attempts == 1) {
                    return {503, nlohmann::json{{"Message", "retry"}}};
                }
                EXPECT_EQ(method, "POST");
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-OK"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_attempts;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(json_body.contains("Symbols"));
                return {200, nlohmann::json{{"Result", 0}}};
            }
            return {404, nlohmann::json::object()};
        }
    );

    int sleep_calls = 0;
    app.startup_with_retry([&](double) { ++sleep_calls; });

    EXPECT_EQ(token_attempts, 2);
    EXPECT_EQ(register_attempts, 1);
    EXPECT_EQ(sleep_calls, 1);
    EXPECT_EQ(app.rest().token(), "TOKEN-OK");
}

TEST(RuntimeTest, CollectActiveOrderSnapshotsUsesOrdersEndpoint) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config, "7269", 9);

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json&,
            const nlohmann::json& params,
            bool,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            EXPECT_EQ(method, "GET");
            EXPECT_TRUE(url.ends_with("/kabusapi/orders"));
            EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
            EXPECT_EQ(params.at("id").get<std::string>(), "OID-1");
            return {200,
                    nlohmann::json::array(
                        {nlohmann::json{
                            {"ID", "OID-1"},
                            {"State", 3},
                            {"OrderState", 3},
                            {"Side", "2"},
                            {"OrderQty", 100},
                            {"CumQty", 80},
                            {"Price", 1735.0},
                            {"Details",
                             {nlohmann::json{
                                 {"RecType", 8},
                                 {"Qty", 80},
                                 {"Price", 1735.5},
                                 {"ExecutionID", "E-1"},
                                 {"ExecutionDay", "2026-04-07T09:00:01+09:00"},
                             }}},
                        }})};
        }
    );

    const auto snapshots = app.collect_active_order_snapshots({"OID-1"});
    ASSERT_TRUE(snapshots.has_value());
    ASSERT_TRUE(snapshots->contains("OID-1"));
    EXPECT_EQ(snapshots->at("OID-1").cum_qty, 80);
    EXPECT_NEAR(snapshots->at("OID-1").avg_fill_price, 1735.5, 1e-9);
}

TEST(RuntimeTest, StatusSnapshotIncludesAccountAndStrategies) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config, "7269", 9);
    strategy->execution().inventory.qty = 100;
    strategy->execution().inventory.avg_price = 1734.5;

    kabu::app::MicroEdgeApp app(config);
    app.set_running(true);
    app.set_strategy(strategy);
    const auto status = app.status_snapshot();

    EXPECT_TRUE(status.at("running").get<bool>());
    EXPECT_TRUE(status.contains("account_risk"));
    EXPECT_TRUE(status.contains("strategies"));
    ASSERT_EQ(status.at("strategies").size(), 1U);
    EXPECT_EQ(status.at("strategies")[0].at("symbol").get<std::string>(), "7269");
    EXPECT_EQ(status.at("account_risk").at("total_inventory_qty").get<int>(), 100);
}
