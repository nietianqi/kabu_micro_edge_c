#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "kabu_micro_edge/telemetry.hpp"

namespace {

std::filesystem::path make_temp_dir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kabu_micro_edge_c_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

}  // namespace

TEST(LatencyReportTest, AnalyzeLogExtractsLatencyAndStale) {
    const auto dir = make_temp_dir("latency");
    const auto log_path = dir / "run.log";
    std::ofstream out(log_path);
    out << "14:00:00.000 [kabu.gateway] INFO websocket connected\n";
    out << "14:00:01.000 [kabu.gateway] WARNING market data latency 1200.0ms for 7269 (source=bid_time)\n";
    out << "14:00:02.000 [kabu.gateway] INFO latency stats symbol=7269 samples=100 p50=300.0ms p90=1200.0ms p99=3200.0ms max=8000.0ms\n";
    out << "14:00:03.000 [kabu.execution] INFO entry order sent symbol=7269\n";
    out << "14:00:04.000 [kabu.execution] INFO cancel requested symbol=7269 order_id=PAPER-7269-1 reason=abnormal_stale_quote\n";
    out << "14:00:05.000 [kabu.gateway] WARNING websocket disconnected\n";
    out.close();

    auto run = kabu::telemetry::analyze_log(log_path, "local");
    EXPECT_EQ(run.connects, 1);
    EXPECT_EQ(run.disconnects, 1);
    EXPECT_EQ(run.total_entries(), 1);
    EXPECT_EQ(run.total_cancels(), 1);
    EXPECT_EQ(run.total_stale_cancels(), 1);
    EXPECT_EQ(run.symbols["7269"].est_latency()["method"], "weighted_window");
}

TEST(LatencyReportTest, AttachTradeStaleExitRate) {
    const auto dir = make_temp_dir("latency_trades");
    const auto log_path = dir / "run.log";
    std::ofstream(log_path) << "14:00:00.000 [kabu.gateway] INFO websocket connected\n";
    const auto trades_path = dir / "trades.csv";
    std::ofstream trades(trades_path);
    trades << "ts_jst,symbol,side,qty,entry_price,exit_price,realized_pnl,hold_ms,exit_reason\n";
    trades << "2026-03-12T11:00:00+09:00,7269,1,100,2000,2001,100,1000,signal_reverse\n";
    trades << "2026-03-12T11:01:00+09:00,7269,1,100,2002,2001,-100,1000,abnormal_stale_quote\n";
    trades.close();
    auto run = kabu::telemetry::analyze_log(log_path, "local");
    kabu::telemetry::attach_trade_stale_exit_rate(run, trades_path);
    EXPECT_EQ(run.total_trade_rows, 2);
    EXPECT_EQ(run.stale_trade_exits, 1);
    EXPECT_DOUBLE_EQ(run.stale_trade_exit_rate, 50.0);
}
