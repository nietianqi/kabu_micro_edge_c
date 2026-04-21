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
    EXPECT_FALSE(plan.poll_positions);
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
    EXPECT_FALSE(order_plan.poll_positions);
    ASSERT_EQ(order_plan.order_ids.size(), 1U);
    EXPECT_EQ(order_plan.order_ids.front(), "OID-1");

    strategy->execution().working_order.reset();
    strategy->execution().inventory.qty = 100;
    strategy->execution().inventory.side = 1;
    auto inventory_plan = app.build_reconcile_plan(*strategy);
    EXPECT_EQ(inventory_plan.mode, "inventory");
    EXPECT_TRUE(inventory_plan.poll_positions);
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

TEST(RuntimeTest, AccountEntryGuardBlocksWhileRecoveryIsInProgress) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config, "7269", 9);

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);
    app.set_recovery_state(true, "startup_recovery");

    const auto guard = app.make_account_entry_guard();
    const auto [allowed, reason] = guard(100, 1735.0);

    EXPECT_FALSE(allowed);
    EXPECT_EQ(reason, "startup_recovery");
    const auto status = app.status_snapshot();
    EXPECT_TRUE(status.at("recovery_in_progress").get<bool>());
    EXPECT_EQ(status.at("recovery_reason").get<std::string>(), "startup_recovery");
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

TEST(RuntimeTest, CollectReconcileInputsSkipsPollingWhenIdle) {
    auto config = kabu::config::load_config();
    kabu::app::MicroEdgeApp app(config);

    int positions_calls = 0;
    int order_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string&,
            const std::string& url,
            const nlohmann::json&,
            const nlohmann::json&,
            bool,
            kabu::gateway::RequestLane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/positions")) {
                ++positions_calls;
                return {200, nlohmann::json::array()};
            }
            if (url.ends_with("/kabusapi/orders")) {
                ++order_calls;
                return {200, nlohmann::json::array()};
            }
            return {404, nlohmann::json::object()};
        }
    );

    const auto snapshot = app.collect_reconcile_inputs({kabu::app::ReconcilePlan{"idle", 2.0, false, {}}});

    EXPECT_FALSE(snapshot.positions.has_value());
    EXPECT_FALSE(snapshot.order_snapshots.has_value());
    EXPECT_EQ(positions_calls, 0);
    EXPECT_EQ(order_calls, 0);
}

TEST(RuntimeTest, CollectReconcileInputsFetchesTargetedOrdersWithoutPositions) {
    auto config = kabu::config::load_config();
    kabu::app::MicroEdgeApp app(config);

    int positions_calls = 0;
    int order_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json&,
            const nlohmann::json& params,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/positions")) {
                ++positions_calls;
                return {200, nlohmann::json::array()};
            }
            if (url.ends_with("/kabusapi/orders")) {
                ++order_calls;
                EXPECT_EQ(method, "GET");
                EXPECT_TRUE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
                EXPECT_EQ(params.at("id").get<std::string>(), "OID-1");
                return {200,
                        nlohmann::json::array(
                            {nlohmann::json{
                                {"ID", "OID-1"},
                                {"Symbol", "7269"},
                                {"Exchange", 9},
                                {"State", 3},
                                {"OrderState", 3},
                                {"Side", "2"},
                                {"OrderQty", 100},
                                {"CumQty", 10},
                                {"Price", 1735.0},
                            }})};
            }
            return {404, nlohmann::json::object()};
        }
    );

    const auto snapshot = app.collect_reconcile_inputs({kabu::app::ReconcilePlan{"orders", 0.5, false, {"OID-1"}}});

    EXPECT_FALSE(snapshot.positions.has_value());
    ASSERT_TRUE(snapshot.order_snapshots.has_value());
    ASSERT_TRUE(snapshot.order_snapshots->contains("OID-1"));
    EXPECT_EQ(snapshot.order_snapshots->at("OID-1").cum_qty, 10);
    EXPECT_EQ(positions_calls, 0);
    EXPECT_EQ(order_calls, 1);
}

TEST(RuntimeTest, CollectStartupRecoveryInputsFetchesFullBrokerState) {
    auto config = kabu::config::load_config();
    kabu::app::MicroEdgeApp app(config);

    int positions_calls = 0;
    int order_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json&,
            const nlohmann::json& params,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/positions")) {
                ++positions_calls;
                EXPECT_EQ(method, "GET");
                EXPECT_TRUE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
                EXPECT_EQ(params.at("product").get<int>(), 0);
                return {200,
                        nlohmann::json::array(
                            {nlohmann::json{
                                {"HoldID", "HOLD-1"},
                                {"Symbol", "7269"},
                                {"Exchange", 9},
                                {"Side", "2"},
                                {"LeavesQty", 100},
                                {"ClosableQty", 100},
                                {"Price", 1734.5},
                                {"MarginTradeType", 1},
                            }})};
            }
            if (url.ends_with("/kabusapi/orders")) {
                ++order_calls;
                EXPECT_EQ(method, "GET");
                EXPECT_TRUE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
                EXPECT_EQ(params.at("product").get<int>(), 0);
                EXPECT_FALSE(params.contains("id"));
                return {200,
                        nlohmann::json::array(
                            {nlohmann::json{
                                {"ID", "OID-1"},
                                {"Symbol", "7269"},
                                {"Exchange", 9},
                                {"State", 3},
                                {"OrderState", 3},
                                {"Side", "2"},
                                {"OrderQty", 100},
                                {"CumQty", 50},
                                {"Price", 1735.0},
                            }})};
            }
            return {404, nlohmann::json::object()};
        }
    );

    const auto snapshot = app.collect_startup_recovery_inputs();

    ASSERT_TRUE(snapshot.positions.has_value());
    ASSERT_EQ(snapshot.positions->size(), 1U);
    ASSERT_TRUE(snapshot.order_snapshots.has_value());
    ASSERT_TRUE(snapshot.order_snapshots->contains("OID-1"));
    EXPECT_EQ(positions_calls, 1);
    EXPECT_EQ(order_calls, 1);
}

TEST(RuntimeTest, AuthorizationRetryRefreshesTokenForUnauthorizedPositionsPoll) {
    auto config = kabu::config::load_config();
    config.api_password = "secret";
    kabu::app::MicroEdgeApp app(config);

    int token_calls = 0;
    int register_calls = 0;
    int position_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json& params,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/token")) {
                ++token_calls;
                EXPECT_EQ(method, "POST");
                EXPECT_FALSE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-POSITIONS"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_calls;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(include_token);
                return {200, nlohmann::json{{"Result", 0}}};
            }
            if (url.ends_with("/kabusapi/positions")) {
                ++position_calls;
                EXPECT_EQ(method, "GET");
                EXPECT_TRUE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
                EXPECT_EQ(params.at("product").get<int>(), 0);
                if (position_calls == 1) {
                    return {403, nlohmann::json{{"Code", 403}, {"Message", "expired"}}};
                }
                return {200,
                        nlohmann::json::array(
                            {nlohmann::json{
                                {"HoldID", "HOLD-1"},
                                {"Symbol", "7269"},
                                {"Exchange", 9},
                                {"Side", "2"},
                                {"LeavesQty", 100},
                                {"ClosableQty", 100},
                                {"Price", 1734.5},
                                {"MarginTradeType", 1},
                            }})};
            }
            return {404, nlohmann::json::object()};
        }
    );

    const auto positions = app.with_authorization_retry(
        [&]() { return app.rest().get_positions(std::nullopt, 0, kabu::gateway::RequestLane::Poll); }
    );

    ASSERT_EQ(positions.size(), 1U);
    EXPECT_EQ(position_calls, 2);
    EXPECT_EQ(token_calls, 1);
    EXPECT_EQ(register_calls, 1);
    EXPECT_EQ(app.rest().token(), "TOKEN-POSITIONS");
    EXPECT_EQ(app.status_snapshot().at("token_refresh_count").get<int>(), 1);
}

TEST(RuntimeTest, CollectActiveOrderSnapshotsRefreshesTokenAfterUnauthorizedPoll) {
    auto config = kabu::config::load_config();
    config.api_password = "secret";
    kabu::app::MicroEdgeApp app(config);

    int token_calls = 0;
    int register_calls = 0;
    int order_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json& params,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/token")) {
                ++token_calls;
                EXPECT_EQ(method, "POST");
                EXPECT_FALSE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-ORDERS"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_calls;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(include_token);
                return {200, nlohmann::json{{"Result", 0}}};
            }
            if (url.ends_with("/kabusapi/orders")) {
                ++order_calls;
                EXPECT_EQ(method, "GET");
                EXPECT_TRUE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
                EXPECT_EQ(params.at("id").get<std::string>(), "OID-1");
                if (order_calls == 1) {
                    return {401, nlohmann::json{{"Code", 401}, {"Message", "expired"}}};
                }
                return {200,
                        nlohmann::json::array(
                            {nlohmann::json{
                                {"ID", "OID-1"},
                                {"State", 3},
                                {"OrderState", 3},
                                {"Symbol", "7269"},
                                {"Exchange", 9},
                                {"Side", "2"},
                                {"OrderQty", 100},
                                {"CumQty", 100},
                                {"Price", 1734.5},
                                {"Details",
                                 {nlohmann::json{
                                     {"RecType", 8},
                                     {"Qty", 100},
                                     {"Price", 1734.5},
                                     {"ExecutionID", "E-1"},
                                     {"ExecutionDay", "2026-04-07T09:00:01+09:00"},
                                 }}},
                            }})};
            }
            return {404, nlohmann::json::object()};
        }
    );

    const auto snapshots = app.collect_active_order_snapshots({"OID-1"});

    ASSERT_TRUE(snapshots.has_value());
    ASSERT_TRUE(snapshots->contains("OID-1"));
    EXPECT_EQ(order_calls, 2);
    EXPECT_EQ(token_calls, 1);
    EXPECT_EQ(register_calls, 1);
    EXPECT_EQ(app.rest().token(), "TOKEN-ORDERS");
    EXPECT_EQ(app.status_snapshot().at("token_refresh_count").get<int>(), 1);
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

TEST(RuntimeTest, ReregisterSymbolsRefreshesTokenAfterUnauthorized) {
    auto config = kabu::config::load_config();
    config.api_password = "secret";
    kabu::app::MicroEdgeApp app(config);

    int token_calls = 0;
    int register_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json&,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/token")) {
                ++token_calls;
                EXPECT_EQ(method, "POST");
                EXPECT_FALSE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-REFRESHED"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_calls;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(include_token);
                if (register_calls == 1) {
                    return {401, nlohmann::json{{"Code", 401}, {"Message", "expired"}}};
                }
                return {200, nlohmann::json{{"Result", 0}}};
            }
            return {404, nlohmann::json::object()};
        }
    );

    app.reregister_symbols();

    EXPECT_EQ(token_calls, 1);
    EXPECT_EQ(register_calls, 2);
    EXPECT_EQ(app.rest().token(), "TOKEN-REFRESHED");
    EXPECT_EQ(app.status_snapshot().at("token_refresh_count").get<int>(), 1);
}

TEST(RuntimeTest, WebSocketReconnectCallbackCanReregisterSymbols) {
    auto config = kabu::config::load_config();
    config.api_password = "secret";
    kabu::app::MicroEdgeApp app(config);

    int token_calls = 0;
    int register_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json&,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/token")) {
                ++token_calls;
                EXPECT_EQ(method, "POST");
                EXPECT_FALSE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-2"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_calls;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(include_token);
                if (register_calls == 1) {
                    return {403, nlohmann::json{{"Code", 403}, {"Message", "expired"}}};
                }
                return {200, nlohmann::json{{"Result", 0}}};
            }
            return {404, nlohmann::json::object()};
        }
    );

    kabu::gateway::KabuWebSocket websocket(
        "ws://localhost:18080/kabusapi/websocket",
        [](const kabu::gateway::BoardSnapshot&) {},
        [](const kabu::gateway::TradePrint&) {},
        [&]() { app.reregister_symbols(); },
        "TOKEN-1"
    );

    websocket.simulate_reconnect();

    EXPECT_EQ(token_calls, 1);
    EXPECT_EQ(register_calls, 2);
    EXPECT_EQ(app.rest().token(), "TOKEN-2");
}

TEST(RuntimeTest, WebSocketReconnectCallbackRefreshesWebSocketToken) {
    auto config = kabu::config::load_config();
    config.api_password = "secret";
    kabu::app::MicroEdgeApp app(config);

    int token_calls = 0;
    int register_calls = 0;
    app.set_rest_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json&,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            if (url.ends_with("/kabusapi/token")) {
                ++token_calls;
                EXPECT_EQ(method, "POST");
                EXPECT_FALSE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-2"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_calls;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(include_token);
                if (register_calls == 1) {
                    return {401, nlohmann::json{{"Code", 401}, {"Message", "expired"}}};
                }
                return {200, nlohmann::json{{"Result", 0}}};
            }
            return {404, nlohmann::json::object()};
        }
    );

    std::unique_ptr<kabu::gateway::KabuWebSocket> websocket;
    kabu::gateway::KabuWebSocket* websocket_handle = nullptr;
    websocket = std::make_unique<kabu::gateway::KabuWebSocket>(
        "ws://localhost:18080/kabusapi/websocket",
        [](const kabu::gateway::BoardSnapshot&) {},
        [](const kabu::gateway::TradePrint&) {},
        [&]() {
            app.reregister_symbols();
            ASSERT_NE(websocket_handle, nullptr);
            websocket_handle->set_api_token(app.rest().token());
        },
        "TOKEN-1"
    );
    websocket_handle = websocket.get();

    websocket->simulate_reconnect();

    EXPECT_EQ(token_calls, 1);
    EXPECT_EQ(register_calls, 2);
    EXPECT_EQ(app.rest().token(), "TOKEN-2");
    EXPECT_EQ(websocket->api_token(), "TOKEN-2");
}
