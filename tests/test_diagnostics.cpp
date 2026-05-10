#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kabu_micro_edge/app.hpp"
#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/diagnostics.hpp"
#include "kabu_micro_edge/strategy.hpp"

namespace {

std::filesystem::path make_temp_dir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kabu_micro_edge_c_diagnostics_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
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

std::shared_ptr<kabu::strategy::MicroEdgeStrategy> make_strategy(kabu::config::AppConfig& config) {
    config.symbol().symbol = "7269";
    config.symbol().exchange = 9;
    config.symbol().tick_size = 0.5;
    config.symbol().max_notional = 1'000'000;
    config.strategy.entry_order_interval_ms = 0;
    config.strategy.exit_order_interval_ms = 0;
    config.strategy.limit_tp_order_interval_ms = 0;
    config.strategy.limit_tp_delay_seconds = 0.0;
    config.strategy.confirm_ticks = 1;
    config.strategy.strong_signal_confirm = 1;
    auto strategy =
        std::make_shared<kabu::strategy::MicroEdgeStrategy>(config.symbol(), config.strategy, config.order_profile, true);
    strategy->start();
    return strategy;
}

}  // namespace

TEST(DiagnosticsTest, JsonlWriterPersistsDecisionAndHeartbeat) {
    auto config = kabu::config::load_config();
    auto strategy = make_strategy(config);

    const auto first_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T11:45:00.100000+09:00");
    const auto second_ts = kabu::common::parse_iso8601_to_ns("2026-04-07T11:45:00.300000+09:00");
    strategy->process_board(make_board(1734.0, 1734.5, 300, 600, first_ts));
    strategy->process_trade(make_trade(first_ts + 50'000'000, 1734.5, 1));
    strategy->process_board(make_board(1734.5, 1735.0, 1500, 100, second_ts));

    kabu::app::MicroEdgeApp app(config);
    app.set_strategy(strategy);

    const auto dir = make_temp_dir("jsonl");
    const auto path = dir / "diagnostics.jsonl";
    kabu::telemetry::JsonlDiagnosticsWriter writer(path);
    writer.open();
    writer.write_decision("7269@9", strategy->last_entry_decision());
    writer.write_heartbeat(app.status_snapshot(), second_ts);
    writer.close();

    std::ifstream in(path);
    ASSERT_TRUE(in.good());

    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            rows.push_back(nlohmann::json::parse(line));
        }
    }

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].at("type").get<std::string>(), "entry_decision");
    EXPECT_EQ(rows[0].at("decision").at("reason").get<std::string>(), "session_lunch_break");
    EXPECT_EQ(rows[1].at("type").get<std::string>(), "heartbeat");
    ASSERT_EQ(rows[1].at("status").at("strategies").size(), 1U);
    EXPECT_EQ(rows[1].at("status").at("strategies")[0].at("symbol").get<std::string>(), "7269");
}
