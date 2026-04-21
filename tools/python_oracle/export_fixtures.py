from __future__ import annotations

import argparse
import asyncio
import dataclasses
import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


JST = timezone(timedelta(hours=9))


def _ts(hour: int, minute: int, second: int = 0, millis: int = 0) -> int:
    dt = datetime(2026, 4, 7, hour, minute, second, millis * 1000, tzinfo=JST)
    return int(dt.timestamp() * 1e9)


def _to_jsonable(value: Any) -> Any:
    if dataclasses.is_dataclass(value):
        return dataclasses.asdict(value)
    if isinstance(value, tuple):
        return [_to_jsonable(item) for item in value]
    if isinstance(value, list):
        return [_to_jsonable(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _to_jsonable(item) for key, item in value.items()}
    return value


def _board(BoardSnapshot, Level, *, bid: float, ask: float, bid_size: int, ask_size: int, ts_ns: int):
    tick = 0.5
    return BoardSnapshot(
        symbol="7269",
        exchange=9,
        ts_ns=ts_ns,
        bid=bid,
        ask=ask,
        bid_size=bid_size,
        ask_size=ask_size,
        last=(bid + ask) / 2,
        last_size=0,
        volume=1000,
        vwap=(bid + ask) / 2,
        bids=tuple(Level(price=bid - i * tick, size=bid_size + i * 100) for i in range(5)),
        asks=tuple(Level(price=ask + i * tick, size=ask_size + i * 25) for i in range(5)),
    )


def export_fixtures(source_root: Path, output_dir: Path) -> None:
    sys.path.insert(0, str(source_root))

    from kabu_micro_edge.app import MicroEdgeApp
    from kabu_micro_edge.config import load_config
    from kabu_micro_edge.gateway import BoardSnapshot, Level, TradePrint
    from kabu_micro_edge.replay import ReplayEvent, ReplayRunner
    from kabu_micro_edge.signals import MicroEdgeSignalEngine
    from kabu_micro_edge.strategy import MicroEdgeStrategy, entry_policy

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "config").mkdir(exist_ok=True)
    (output_dir / "signals").mkdir(exist_ok=True)
    (output_dir / "strategy").mkdir(exist_ok=True)
    (output_dir / "runtime").mkdir(exist_ok=True)

    app_config = load_config(None)
    app_config.symbol.symbol = "7269"
    app_config.symbol.exchange = 9
    app_config.symbol.tick_size = 0.5
    app_config.symbol.max_notional = 1_000_000
    app_config.strategy.confirm_ticks = 1
    app_config.strategy.strong_signal_confirm = 1
    app_config.strategy.entry_order_interval_ms = 0
    app_config.strategy.exit_order_interval_ms = 0
    app_config.strategy.limit_tp_order_interval_ms = 0
    app_config.strategy.limit_tp_delay_seconds = 0.0
    app_config.strategy.aggressive_taker_mode = False

    (output_dir / "config" / "default.json").write_text(
        json.dumps(_to_jsonable(app_config), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    engine = MicroEdgeSignalEngine(
        tick_size=app_config.symbol.tick_size,
        book_depth_levels=app_config.strategy.book_depth_levels,
        book_decay=app_config.strategy.book_decay,
        tape_window_seconds=app_config.strategy.tape_window_seconds,
        mid_std_window=app_config.strategy.mid_std_window,
        min_best_volume=app_config.strategy.min_best_volume,
        kabu_bidask_reversed=app_config.strategy.kabu_bidask_reversed,
        auto_fix_negative_spread=app_config.strategy.auto_fix_negative_spread,
        use_microprice_tilt=app_config.strategy.use_microprice_tilt,
    )

    baseline_board = _board(BoardSnapshot, Level, bid=1734.0, ask=1734.5, bid_size=300, ask_size=600, ts_ns=_ts(9, 0, 0, 100))
    breakout_board = _board(BoardSnapshot, Level, bid=1734.5, ask=1735.0, bid_size=1500, ask_size=100, ts_ns=_ts(9, 0, 0, 300))
    engine.on_board(baseline_board)
    engine.on_trade(
        TradePrint(
            symbol="7269",
            exchange=9,
            ts_ns=_ts(9, 0, 0, 200),
            price=1734.5,
            size=1500,
            side=1,
            cumulative_volume=1500,
        )
    )
    breakout_signal = engine.on_board(breakout_board)

    (output_dir / "signals" / "breakout_packet.json").write_text(
        json.dumps(_to_jsonable(breakout_signal), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    strategy = MicroEdgeStrategy(
        symbol=app_config.symbol,
        config=app_config.strategy,
        order_profile=app_config.order_profile,
        rest_client=None,
        dry_run=True,
        journal=None,
    )
    decision = entry_policy.evaluate_long_signal(strategy, breakout_board, breakout_signal)
    (output_dir / "strategy" / "entry_decision.json").write_text(
        json.dumps(_to_jsonable(decision), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    async def _collect_replay() -> tuple[list[dict[str, Any]], dict[str, Any]]:
        replay_strategy = MicroEdgeStrategy(
            symbol=app_config.symbol,
            config=app_config.strategy,
            order_profile=app_config.order_profile,
            rest_client=None,
            dry_run=True,
            journal=None,
        )
        await replay_strategy.start()
        try:
            replay_events = [
                ReplayEvent(kind="board", payload=baseline_board),
                ReplayEvent(
                    kind="trade",
                    payload=TradePrint(
                        symbol="7269",
                        exchange=9,
                        ts_ns=_ts(9, 0, 0, 200),
                        price=1734.5,
                        size=1500,
                        side=1,
                        cumulative_volume=1500,
                    ),
                ),
                ReplayEvent(kind="board", payload=breakout_board),
                ReplayEvent(kind="timer", payload=_ts(9, 0, 0, 700)),
                ReplayEvent(
                    kind="board",
                    payload=_board(
                        BoardSnapshot,
                        Level,
                        bid=1736.0,
                        ask=1736.5,
                        bid_size=100,
                        ask_size=1200,
                        ts_ns=_ts(9, 0, 1, 0),
                    ),
                ),
            ]
            replay_runner = ReplayRunner(replay_strategy)
            return await replay_runner.run(replay_events), replay_strategy.status()
        finally:
            await replay_strategy.stop()

    replay_snapshots, final_status = asyncio.run(_collect_replay())
    (output_dir / "strategy" / "replay_snapshots.json").write_text(
        json.dumps(_to_jsonable(replay_snapshots), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (output_dir / "strategy" / "final_status.json").write_text(
        json.dumps(_to_jsonable(final_status), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    app = MicroEdgeApp(app_config)
    strategy.execution.inventory.qty = 300
    strategy.execution.inventory.avg_price = 1734.5
    app.strategy = strategy
    account_snapshot = app._account_risk_snapshot(additional_qty=100, additional_price=1735.0)
    (output_dir / "runtime" / "account_risk_snapshot.json").write_text(
        json.dumps(_to_jsonable(account_snapshot), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    reconcile_plan = app._build_reconcile_plan(strategy)
    (output_dir / "runtime" / "reconcile_plan.json").write_text(
        json.dumps(_to_jsonable(reconcile_plan), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Export kabu_micro_edge Python oracle fixtures")
    parser.add_argument("--source-root", default="D:/kabu_micro_edge")
    parser.add_argument("--output-dir", default="fixtures/python_oracle")
    args = parser.parse_args()
    export_fixtures(Path(args.source_root), Path(args.output_dir))


if __name__ == "__main__":
    main()
