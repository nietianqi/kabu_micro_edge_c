#include <optional>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "kabu_micro_edge/gateway.hpp"

TEST(GatewayTest, NormalizesReversedBidAskSemantics) {
    const nlohmann::json raw = {
        {"Symbol", "9984"},
        {"Exchange", 1},
        {"AskPrice", 9980},
        {"AskQty", 400},
        {"BidPrice", 9990},
        {"BidQty", 500},
        {"Buy1", {{"Price", 9980}, {"Qty", 400}}},
        {"Sell1", {{"Price", 9990}, {"Qty", 500}}},
        {"CurrentPrice", 9985},
        {"CurrentPriceTime", "2026-03-11T09:00:00+09:00"},
        {"TradingVolume", 1000},
    };
    const auto snapshot = kabu::gateway::KabuAdapter::board(raw, std::nullopt);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_DOUBLE_EQ(snapshot->bid, 9980.0);
    EXPECT_DOUBLE_EQ(snapshot->ask, 9990.0);
    EXPECT_EQ(snapshot->bid_size, 400);
    EXPECT_EQ(snapshot->ask_size, 500);
}

TEST(GatewayTest, TradeSizeUsesVolumeDeltaAndTradingVolumeTime) {
    const nlohmann::json raw = {
        {"Symbol", "9984"},
        {"Exchange", 1},
        {"CurrentPrice", 9990},
        {"CurrentPriceTime", "2026-03-11T09:00:01+09:00"},
        {"TradingVolumeTime", "2026-03-11T09:00:02+09:00"},
        {"TradingVolume", 1200},
    };
    const auto trade = kabu::gateway::KabuAdapter::trade(raw, std::nullopt, 1000, 9985.0);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->size, 200);
    EXPECT_DOUBLE_EQ(trade->price, 9990.0);
    EXPECT_EQ(
        trade->ts_ns,
        kabu::common::parse_iso8601_to_ns("2026-03-11T09:00:02+09:00")
    );
}

TEST(GatewayTest, BoardTimestampPrefersBidOrAskTime) {
    const nlohmann::json raw = {
        {"Symbol", "9984"},
        {"Exchange", 1},
        {"AskPrice", 9980},
        {"AskQty", 400},
        {"BidPrice", 9990},
        {"BidQty", 500},
        {"Buy1", {{"Price", 9980}, {"Qty", 400}}},
        {"Sell1", {{"Price", 9990}, {"Qty", 500}}},
        {"CurrentPrice", 9985},
        {"CurrentPriceTime", "2026-03-11T09:00:00+09:00"},
        {"BidTime", "2026-03-11T09:00:01+09:00"},
        {"AskTime", "2026-03-11T09:00:00.500000+09:00"},
        {"TradingVolume", 1000},
    };
    const auto snapshot = kabu::gateway::KabuAdapter::board(raw, std::nullopt);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->ts_source, "bid_time");
}

TEST(GatewayTest, OrderSnapshotUsesFillDetails) {
    const nlohmann::json raw = {
        {"ID", "ORDER-2"},
        {"State", 3},
        {"OrderState", 3},
        {"Side", "2"},
        {"OrderQty", 100},
        {"CumQty", 80},
        {"Price", 1000.0},
        {"Details",
         {
             {{"RecType", 8}, {"Qty", 50}, {"Price", 1001.0}, {"ExecutionID", "E1"}, {"ExecutionDay", "2026-03-13T10:00:00+09:00"}},
             {{"RecType", 8}, {"Qty", 30}, {"Price", 1002.0}, {"ExecutionID", "E2"}, {"ExecutionDay", "2026-03-13T10:00:01+09:00"}},
         }},
    };
    const auto snapshot = kabu::gateway::KabuAdapter::order_snapshot(raw);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->cum_qty, 80);
    EXPECT_NEAR(snapshot->avg_fill_price, (50 * 1001.0 + 30 * 1002.0) / 80.0, 1e-9);
    EXPECT_GT(snapshot->fill_ts_ns, 0);
}

TEST(GatewayTest, RestClientTokenAndRegisterFlowMatchPythonPaths) {
    kabu::gateway::KabuRestClient client("http://localhost:18080");
    int token_calls = 0;
    int register_calls = 0;
    client.set_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json& params,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            EXPECT_TRUE(params.empty());
            if (url.ends_with("/kabusapi/token")) {
                ++token_calls;
                EXPECT_EQ(method, "POST");
                EXPECT_FALSE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_EQ(json_body.at("APIPassword").get<std::string>(), "secret");
                return {200, nlohmann::json{{"Token", "TOKEN-1"}}};
            }
            if (url.ends_with("/kabusapi/register")) {
                ++register_calls;
                EXPECT_EQ(method, "PUT");
                EXPECT_TRUE(include_token);
                EXPECT_EQ(lane, kabu::gateway::RequestLane::Order);
                EXPECT_TRUE(json_body.contains("Symbols"));
                return {200, nlohmann::json{{"Result", 0}}};
            }
            return {404, nlohmann::json::object()};
        }
    );

    EXPECT_EQ(client.get_token("secret"), "TOKEN-1");
    EXPECT_EQ(client.token(), "TOKEN-1");
    EXPECT_EQ(client.password(), "secret");

    const auto response = client.register_symbols({nlohmann::json{{"Symbol", "7269"}, {"Exchange", 1}}});
    EXPECT_EQ(response.at("Result").get<int>(), 0);
    EXPECT_EQ(token_calls, 1);
    EXPECT_EQ(register_calls, 1);
}

TEST(GatewayTest, RestClientGetOrdersUsesPollLaneAndIdParam) {
    kabu::gateway::KabuRestClient client("http://localhost:18080");
    client.set_request_executor(
        [&](const std::string& method,
            const std::string& url,
            const nlohmann::json& json_body,
            const nlohmann::json& params,
            bool include_token,
            kabu::gateway::RequestLane lane) -> kabu::gateway::TransportResponse {
            EXPECT_EQ(method, "GET");
            EXPECT_TRUE(url.ends_with("/kabusapi/orders"));
            EXPECT_TRUE(json_body.empty());
            EXPECT_TRUE(include_token);
            EXPECT_EQ(lane, kabu::gateway::RequestLane::Poll);
            EXPECT_EQ(params.at("product").get<int>(), 0);
            EXPECT_EQ(params.at("id").get<std::string>(), "OID-1");
            return {200,
                    nlohmann::json::array(
                        {nlohmann::json{
                            {"ID", "OID-1"},
                            {"State", 2},
                            {"OrderState", 2},
                            {"Side", "2"},
                            {"OrderQty", 100},
                            {"CumQty", 0},
                            {"Price", 1735.0},
                        }})};
        }
    );

    const auto orders = client.get_orders("OID-1");
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(client.poll_bucket_acquires(), 1);
}

TEST(GatewayTest, WebSocketCanReplayCallbacksAndReconnect) {
    int board_events = 0;
    int trade_events = 0;
    int reconnects = 0;
    kabu::gateway::KabuWebSocket websocket(
        "ws://localhost:18080/kabusapi/websocket",
        [&](const kabu::gateway::BoardSnapshot&) { ++board_events; },
        [&](const kabu::gateway::TradePrint&) { ++trade_events; },
        [&]() { ++reconnects; },
        "TOKEN-1"
    );

    websocket.run();
    EXPECT_TRUE(websocket.running());
    EXPECT_EQ(websocket.status(), "running");
    EXPECT_EQ(websocket.api_token(), "TOKEN-1");

    websocket.simulate_board(kabu::gateway::BoardSnapshot{});
    websocket.simulate_trade(kabu::gateway::TradePrint{});
    websocket.simulate_reconnect();
    websocket.set_api_token("TOKEN-2");
    websocket.stop();

    EXPECT_EQ(board_events, 1);
    EXPECT_EQ(trade_events, 1);
    EXPECT_EQ(reconnects, 1);
    EXPECT_EQ(websocket.api_token(), "TOKEN-2");
    EXPECT_TRUE(websocket.stopped());
}
