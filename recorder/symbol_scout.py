#!/usr/bin/env python3
"""Symbol selection by observed spread (master plan Phase 0 / week 0 deliverable).

Master plan §2.4 is blunt about why this matters: quoting the touch on BTC earns
about 0.001 bp of notional, while a retail maker fee is 2-10 bp. Passive market
making on ultra-tight majors is structurally unprofitable on spread capture
alone, so the recording list must also include instruments whose typical spread
is >= 3-5 ticks / >= 2 bp, where the economics are non-degenerate.

This script watches the bookTicker stream for a while and reports the observed
spread distribution per symbol, so the choice is made from data.

Part 11 pitfall #12: choose the symbols in week 0, COMMIT the choice, and keep
even the boring ones. Picking symbols after seeing results is survivorship bias
dressed up as judgement. Run this once, write the answer into
recorder/symbols.yaml and docs/EXPERIMENTS.md, and do not revisit it.

Usage::

    python symbol_scout.py --symbols BTCUSDT ETHUSDT SOLUSDT ARBUSDT --minutes 20
"""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import sys
import time
from collections import defaultdict
from typing import Any

try:
    import websockets
except ImportError:  # pragma: no cover
    print("symbol_scout: pip install -r recorder/requirements.txt", file=sys.stderr)
    raise

WS = {
    "spot": "wss://stream.binance.com:9443/stream?streams=",
    "futures": "wss://fstream.binance.com/stream?streams=",
}


async def scout(symbols: list[str], market: str, seconds: float) -> dict[str, Any]:
    url = WS[market] + "/".join(f"{s.lower()}@bookTicker" for s in symbols)
    spreads_ticks: dict[str, list[float]] = defaultdict(list)
    spreads_bp: dict[str, list[float]] = defaultdict(list)
    mids: dict[str, list[float]] = defaultdict(list)
    top_qty: dict[str, list[float]] = defaultdict(list)
    tick_guess: dict[str, float] = {}

    deadline = time.time() + seconds
    print(f"observing {len(symbols)} symbols for {seconds:.0f}s on {market} ...")
    async with websockets.connect(url, ping_interval=20, ping_timeout=20) as socket:
        while time.time() < deadline:
            try:
                raw = await asyncio.wait_for(socket.recv(), timeout=30.0)
            except asyncio.TimeoutError:
                print("no data for 30 s; stopping early", file=sys.stderr)
                break
            payload = json.loads(raw).get("data", {})
            symbol = payload.get("s")
            if symbol is None:
                continue
            try:
                bid = float(payload["b"])
                ask = float(payload["a"])
                bid_qty = float(payload["B"])
                ask_qty = float(payload["A"])
            except (KeyError, ValueError):
                continue
            if bid <= 0 or ask <= bid:
                continue

            mid = 0.5 * (bid + ask)
            spread = ask - bid
            # The tick size is not published on this stream; the smallest
            # non-zero spread seen is a serviceable lower bound for a scouting
            # run. Real tick sizes come from exchangeInfo before recording.
            prev = tick_guess.get(symbol)
            tick_guess[symbol] = spread if prev is None else min(prev, spread)

            mids[symbol].append(mid)
            spreads_bp[symbol].append(1e4 * spread / mid)
            top_qty[symbol].append(min(bid_qty, ask_qty) * mid)
            spreads_ticks[symbol].append(spread)

    report: dict[str, Any] = {}
    for symbol in symbols:
        raw_spreads = spreads_ticks.get(symbol, [])
        if not raw_spreads:
            report[symbol] = {"observations": 0}
            continue
        tick = tick_guess.get(symbol) or min(raw_spreads)
        in_ticks = [s / tick for s in raw_spreads] if tick > 0 else []
        report[symbol] = {
            "observations": len(raw_spreads),
            "mid_median": statistics.median(mids[symbol]),
            "spread_bp_median": statistics.median(spreads_bp[symbol]),
            "spread_bp_p90": sorted(spreads_bp[symbol])[int(0.9 * (len(spreads_bp[symbol]) - 1))],
            "spread_ticks_median": statistics.median(in_ticks) if in_ticks else None,
            "inferred_tick": tick,
            "top_notional_median": statistics.median(top_qty[symbol]),
        }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--symbols", nargs="+", required=True)
    parser.add_argument("--market", default="futures", choices=["spot", "futures"])
    parser.add_argument("--minutes", type=float, default=15.0)
    parser.add_argument("--json", default=None)
    args = parser.parse_args()

    report = asyncio.run(scout([s.upper() for s in args.symbols], args.market, args.minutes * 60))

    print(f"\n{'symbol':<12}{'obs':>8}{'mid':>14}{'spread bp':>12}{'bp p90':>10}"
          f"{'spread ticks':>14}{'top $':>14}")
    print("-" * 84)
    for symbol, info in report.items():
        if not info.get("observations"):
            print(f"{symbol:<12}{0:>8}   no data")
            continue
        print(
            f"{symbol:<12}{info['observations']:>8}{info['mid_median']:>14.4f}"
            f"{info['spread_bp_median']:>12.3f}{info['spread_bp_p90']:>10.3f}"
            f"{info['spread_ticks_median']:>14.2f}{info['top_notional_median']:>14.0f}"
        )

    print("\nSelection guidance (master plan §2.4):")
    print("  * keep 2 tight majors (BTC, ETH) -- the degenerate-economics case;")
    print("  * add 2 instruments with a median spread >= 3-5 ticks AND >= 2 bp,")
    print("    where passive market making is not automatically fee-dominated.")
    wide = [s for s, i in report.items()
            if i.get("observations") and i.get("spread_bp_median", 0) >= 2.0
            and (i.get("spread_ticks_median") or 0) >= 3.0]
    if wide:
        print(f"  candidates meeting both bars: {', '.join(wide)}")
    else:
        print("  no candidate met both bars in this window -- widen the symbol list and re-run.")
    print("\nCommit the choice to recorder/symbols.yaml and docs/EXPERIMENTS.md now,")
    print("before any results exist (Part 11 pitfall #12).")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
        print(f"\nJSON written to {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
