#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kabu_micro_edge/journal.hpp"

namespace {

std::filesystem::path make_temp_dir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kabu_micro_edge_c_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

kabu::execution::RoundTrip make_round_trip() {
    return {"9984", 1, 100, 9980.0, 10030.0, 1'700'000'000'000'000'000LL, 1'700'000'015'000'000'000LL, 5000.0, "signal_reverse", "maker", 4, "trade_through", 1200};
}

kabu::signals::SignalPacket make_signal() {
    kabu::signals::SignalPacket signal;
    signal.obi_z = 1.2;
    signal.lob_ofi_z = 0.8;
    signal.tape_ofi_z = 0.5;
    signal.micro_momentum_z = 0.7;
    signal.microprice_tilt_z = 0.3;
    signal.composite = 0.75;
    return signal;
}

}  // namespace

TEST(JournalTest, LogTradePersistsImmediately) {
    const auto dir = make_temp_dir("journal_trade");
    kabu::TradeJournal journal(dir / "trades.csv", 0);
    journal.open();
    journal.log_trade(make_round_trip(), make_signal());
    journal.close();

    std::ifstream handle(dir / "trades.csv");
    std::string content((std::istreambuf_iterator<char>(handle)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("9984"), std::string::npos);
    EXPECT_NE(content.find("0.7500"), std::string::npos);
}

TEST(JournalTest, CloseFlushesPendingMarkout) {
    const auto dir = make_temp_dir("journal_markout");
    kabu::TradeJournal journal(dir / "trades.csv", 30);
    journal.open();
    journal.schedule_markout(make_round_trip(), {10050.0});
    journal.schedule_entry_markout("9984", 1, 100, 9980.0, 1'700'000'000'000'000'000LL, "taker", 5, "quote_cross", 0, {10010.0});
    journal.close();

    EXPECT_TRUE(std::filesystem::exists(dir / "trades.markout.csv"));
    EXPECT_TRUE(std::filesystem::exists(dir / "trades.entry_markout.csv"));
    const auto snapshot = journal.snapshot();
    EXPECT_TRUE(snapshot.contains("post_exit_horizons"));
    EXPECT_TRUE(snapshot.contains("entry_horizons"));
}

TEST(JournalTest, SnapshotTracksPendingMarkoutsBeforeClose) {
    const auto dir = make_temp_dir("journal_pending");
    kabu::TradeJournal journal(
        dir / "trades.csv",
        30,
        std::vector<double>{0.5, 1.0},
        std::vector<double>{0.2, 0.5}
    );
    journal.open();
    journal.schedule_markout(make_round_trip(), {10050.0});
    journal.schedule_entry_markout("9984", 1, 100, 9980.0, 1'700'000'000'000'000'000LL, "taker", 5, "quote_cross", 0, {10010.0});

    const auto snapshot = journal.snapshot();
    EXPECT_EQ(snapshot["pending_post_exit_markouts"].get<int>(), 2);
    EXPECT_EQ(snapshot["pending_entry_markouts"].get<int>(), 2);
    EXPECT_EQ(snapshot["pending_markouts"].get<int>(), 4);

    journal.close();
}
