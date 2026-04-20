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
    EXPECT_EQ(config.order_profile.margin_trade_type, 1);
    EXPECT_EQ(config.strategy.maker_score_threshold, 6);
    EXPECT_EQ(config.strategy.taker_score_threshold, 9);
    EXPECT_TRUE(config.strategy.aggressive_taker_mode);
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
