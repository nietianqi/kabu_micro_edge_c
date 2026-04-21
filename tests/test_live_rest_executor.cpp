#include <gtest/gtest.h>

#include "kabu_micro_edge/live_rest_executor.hpp"

TEST(LiveRestExecutorTest, BuildQueryStringEncodesScalarsAndArrays) {
    const nlohmann::json params = {
        {"product", 2},
        {"symbol", "277A"},
        {"flags", nlohmann::json::array({"cash margin", true})},
    };

    EXPECT_EQ(
        kabu::gateway::build_query_string(params),
        "flags=cash%20margin&flags=true&product=2&symbol=277A"
    );
}

TEST(LiveRestExecutorTest, ParseHttpUrlHandlesExplicitPortAndPath) {
    const auto parsed = kabu::gateway::parse_http_url("http://localhost:18080/kabusapi/orders?id=1");

    EXPECT_FALSE(parsed.secure);
    EXPECT_EQ(parsed.host, "localhost");
    EXPECT_EQ(parsed.port, 18080);
    EXPECT_EQ(parsed.target, "/kabusapi/orders?id=1");
}

TEST(LiveRestExecutorTest, ParseHttpUrlDefaultsHttpsPort) {
    const auto parsed = kabu::gateway::parse_http_url("https://example.com/kabusapi/token");

    EXPECT_TRUE(parsed.secure);
    EXPECT_EQ(parsed.host, "example.com");
    EXPECT_EQ(parsed.port, 443);
    EXPECT_EQ(parsed.target, "/kabusapi/token");
}
