#include <gtest/gtest.h>

#include "kabu_micro_edge/signals.hpp"

namespace {

kabu::gateway::BoardSnapshot make_board(double bid, double ask, int bid_size = 800, int ask_size = 300, std::int64_t ts_ns = 0) {
    kabu::gateway::BoardSnapshot snapshot;
    snapshot.symbol = "7269";
    snapshot.exchange = 9;
    snapshot.ts_ns = ts_ns == 0 ? kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00+09:00") : ts_ns;
    snapshot.bid = bid;
    snapshot.ask = ask;
    snapshot.bid_size = bid_size;
    snapshot.ask_size = ask_size;
    snapshot.last = (bid + ask) / 2.0;
    snapshot.volume = 1000;
    snapshot.vwap = snapshot.last;
    for (int i = 0; i < 5; ++i) {
        snapshot.bids.push_back({bid - i * 0.5, bid_size + i * 100});
        snapshot.asks.push_back({ask + i * 0.5, ask_size + i * 50});
    }
    return snapshot;
}

kabu::gateway::TradePrint make_trade(double price, int side, int size = 200, std::int64_t ts_ns = 0) {
    return {"7269", 9, ts_ns == 0 ? kabu::common::parse_iso8601_to_ns("2026-04-07T09:00:00.100000+09:00") : ts_ns, price, size, side, size};
}

}  // namespace

TEST(SignalsTest, NormalizeBoardSnapshotSwapsNegativeSpread) {
    const auto raw = make_board(1735.0, 1734.5, 200, 500);
    const auto repaired = kabu::signals::normalize_board_snapshot(raw, false, true);
    EXPECT_DOUBLE_EQ(repaired.bid, 1734.5);
    EXPECT_DOUBLE_EQ(repaired.ask, 1735.0);
    EXPECT_EQ(repaired.bid_size, 500);
    EXPECT_EQ(repaired.ask_size, 200);
}

TEST(SignalsTest, RefreshFromLatestBoardUpdatesTradeDrivenFields) {
    kabu::signals::MicroEdgeSignalEngine engine(0.5, 5, 0.8, 10, 30, 100, false, true, true);
    const auto board = make_board(1734.0, 1734.5, 500, 200);
    const auto initial = engine.on_board(board);
    engine.on_trade(make_trade(1734.5, 1, 1000));
    const auto refreshed = engine.refresh_from_latest_board(board, board.ts_ns + 100'000'000);
    EXPECT_DOUBLE_EQ(refreshed.obi_raw, initial.obi_raw);
    EXPECT_DOUBLE_EQ(refreshed.lob_ofi_raw, initial.lob_ofi_raw);
    EXPECT_GT(refreshed.tape_ofi_raw, initial.tape_ofi_raw);
    EXPECT_GT(refreshed.trade_burst_score, 0.0);
    EXPECT_GT(refreshed.integrated_ofi, initial.integrated_ofi);
}

TEST(SignalsTest, TracksBookTapeAndLobOfi) {
    kabu::signals::MicroEdgeSignalEngine engine(0.5, 5, 0.8, 10, 30, 100, false, true, true);
    const auto first = make_board(1734.0, 1734.5, 300, 600);
    const auto second = make_board(1734.5, 1735.0, 900, 250, first.ts_ns + 500'000'000);
    engine.on_board(first);
    engine.on_trade(make_trade(1734.5, 1, 200, first.ts_ns + 100'000'000));
    const auto packet = engine.on_board(second);
    EXPECT_GT(packet.obi_raw, 0.0);
    EXPECT_GT(packet.tape_ofi_raw, 0.0);
    EXPECT_GT(packet.lob_ofi_raw, 0.0);
    EXPECT_GT(packet.microprice_tilt_raw, 0.0);
    EXPECT_NEAR(packet.microprice_gap_ticks, packet.microprice_tilt_raw, 1e-9);
    EXPECT_GT(packet.integrated_ofi, 0.0);
    EXPECT_GT(packet.trade_burst_score, 0.0);
}
