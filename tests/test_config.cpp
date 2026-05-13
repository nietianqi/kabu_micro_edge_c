#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "kabu_micro_edge/config.hpp"

namespace {

std::filesystem::path make_temp_dir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kabu_micro_edge_c_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path write_config(const std::filesystem::path& dir, const nlohmann::json& payload) {
    const auto path = dir / "config.json";
    std::ofstream out(path);
    out << payload.dump(2);
    return path;
}

}  // namespace

TEST(ConfigTest, LoadsDefaultConfig) {
    const auto config = kabu::config::load_config();
    EXPECT_FALSE(config.debug_sendorder_log);
    EXPECT_EQ(config.order_profile.margin_trade_type, 1);
    EXPECT_DOUBLE_EQ(config.strategy.profit_ticks, 1.0);
    EXPECT_EQ(config.strategy.maker_score_threshold, 6);
    EXPECT_EQ(config.strategy.taker_score_threshold, 9);
    EXPECT_TRUE(config.strategy.aggressive_taker_mode);
    EXPECT_EQ(config.symbol().lot_size, 100);
}

TEST(ConfigTest, LoadsDebugSendorderLogOverride) {
    const auto dir = make_temp_dir("config_debug_sendorder_log");
    const auto path = write_config(dir, {{"debug_sendorder_log", true}});

    const auto config = kabu::config::load_config(path);
    EXPECT_TRUE(config.debug_sendorder_log);
}

TEST(ConfigTest, RejectsRetiredFields) {
    const auto dir = make_temp_dir("config_retired");
    const auto path = write_config(dir, {{"strategy", {{"entry_slip_ticks", 0.0}}}});
    EXPECT_THROW(kabu::config::load_config(path), std::runtime_error);
}

TEST(ConfigTest, SupportsSymbolsListAndJournalSplitting) {
    const auto dir = make_temp_dir("config_symbols");
    const auto path = write_config(
        dir,
        {
            {"journal_path", (dir / "logs" / "trades.csv").string()},
            {"symbols",
             {
                 {{"symbol", "7269"}, {"exchange", 9}, {"tick_size", 0.5}, {"max_notional", 500000}},
                 {{"symbol", "7203"}, {"exchange", 1}, {"tick_size", 1.0}, {"max_notional", 800000}},
             }},
        }
    );

    const auto config = kabu::config::load_config(path);
    ASSERT_EQ(config.symbols.size(), 2U);
    EXPECT_EQ(config.symbol().symbol, "7269");
    EXPECT_EQ(std::filesystem::path(config.journal_path_for(config.symbols[0])).filename().string(), "trades_7269.csv");
    EXPECT_EQ(std::filesystem::path(config.journal_path_for(config.symbols[1])).filename().string(), "trades_7203.csv");
}

TEST(ConfigTest, RejectsAmbiguousNormalizedRoutes) {
    const auto dir = make_temp_dir("config_ambiguous");
    const auto path = write_config(
        dir,
        {
            {"symbols",
             {
                 {{"symbol", "7269"}, {"exchange", 9}, {"tick_size", 0.5}, {"max_notional", 500000}},
                 {{"symbol", "7269"}, {"exchange", 1}, {"tick_size", 0.5}, {"max_notional", 500000}},
             }},
        }
    );
    EXPECT_THROW(kabu::config::load_config(path), std::runtime_error);
}

TEST(ConfigTest, LoadsCustomLotSizeFromSymbolConfig) {
    const auto dir = make_temp_dir("config_lot_size");
    const auto path = write_config(
        dir,
        {
            {"symbol",
             {
                 {"symbol", "7269"},
                 {"exchange", 9},
                 {"tick_size", 0.5},
                 {"max_notional", 500000},
                 {"lot_size", 1000},
             }},
        }
    );

    const auto config = kabu::config::load_config(path);
    EXPECT_EQ(config.symbol().lot_size, 1000);
}

TEST(ConfigTest, RejectsNonPositiveLotSize) {
    const auto dir = make_temp_dir("config_bad_lot_size");
    const auto path = write_config(
        dir,
        {
            {"symbol",
             {
                 {"symbol", "7269"},
                 {"exchange", 9},
                 {"tick_size", 0.5},
                 {"max_notional", 500000},
                 {"lot_size", 0},
             }},
        }
    );

    EXPECT_THROW(kabu::config::load_config(path), std::runtime_error);
}

TEST(ConfigTest, RejectsInvalidDiagnosticsHeartbeatInterval) {
    const auto dir = make_temp_dir("config_bad_diagnostics");
    const auto path = write_config(
        dir,
        {
            {"diagnostics",
             {
                 {"jsonl_path", (dir / "diagnostics.jsonl").string()},
                 {"heartbeat_interval_s", 0},
             }},
        }
    );

    EXPECT_THROW(kabu::config::load_config(path), std::runtime_error);
}

TEST(ConfigTest, RejectsNegativeLiveSafetyCooldown) {
    const auto dir = make_temp_dir("config_bad_live_safety");
    const auto path = write_config(
        dir,
        {
            {"live_safety",
             {
                 {"rest_error_cooldown_seconds", -1.0},
             }},
        }
    );

    EXPECT_THROW(kabu::config::load_config(path), std::runtime_error);
}

TEST(ConfigTest, LoadsAdvancedStrategyControls) {
    const auto dir = make_temp_dir("config_advanced_strategy_controls");
    const auto path = write_config(
        dir,
        {
            {"strategy",
             {
                 {"enable_jump_filter", false},
                 {"jump_gap_seconds", 0.75},
                 {"jump_mid_ticks", 4.5},
                 {"jump_cooldown_ms", 900},
                 {"enable_exec_quality_gate", true},
                 {"min_exec_quality_score", 7},
                 {"track_near_misses", false},
                 {"near_miss_min_score", 11},
                 {"scale_qty_by_score", true},
                 {"scale_qty_score_threshold", 12},
                 {"scale_qty_multiplier", 1.8},
                 {"scale_qty_max_volume", 300},
             }},
        }
    );

    const auto config = kabu::config::load_config(path);
    EXPECT_FALSE(config.strategy.enable_jump_filter);
    EXPECT_DOUBLE_EQ(config.strategy.jump_gap_seconds, 0.75);
    EXPECT_DOUBLE_EQ(config.strategy.jump_mid_ticks, 4.5);
    EXPECT_EQ(config.strategy.jump_cooldown_ms, 900);
    EXPECT_TRUE(config.strategy.enable_exec_quality_gate);
    EXPECT_EQ(config.strategy.min_exec_quality_score, 7);
    EXPECT_FALSE(config.strategy.track_near_misses);
    EXPECT_EQ(config.strategy.near_miss_min_score, 11);
    EXPECT_TRUE(config.strategy.scale_qty_by_score);
    EXPECT_EQ(config.strategy.scale_qty_score_threshold, 12);
    EXPECT_DOUBLE_EQ(config.strategy.scale_qty_multiplier, 1.8);
    EXPECT_EQ(config.strategy.scale_qty_max_volume, 300);
}

TEST(ConfigTest, RejectsInvalidAdvancedStrategyRanges) {
    const auto dir = make_temp_dir("config_bad_advanced_strategy_ranges");
    const auto path = write_config(
        dir,
        {
            {"strategy",
             {
                 {"jump_mid_ticks", 0.0},
                 {"scale_qty_multiplier", 0.9},
             }},
        }
    );

    EXPECT_THROW(kabu::config::load_config(path), std::runtime_error);
}
