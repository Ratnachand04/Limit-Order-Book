#!/usr/bin/env python3
"""Binance market-data recorder (master plan Phase 0, §4.3).

Records, per symbol:
  * ``<symbol>@depth@100ms``  diff-depth stream with sequence numbers
  * ``<symbol>@aggTrade``     every trade with an aggressor-side flag
  * ``<symbol>@bookTicker``   BBO updates, kept for cross-checks
  * a REST ``/depth`` snapshot every 30 minutes as a resync anchor

Output is one gzipped JSONL file per symbol per hour::

    {"t": <local receive time, microseconds>,
     "c": "depth" | "aggTrade" | "bookTicker" | "snapshot",
     "s": "BTCUSDT",
     "d": { ...verbatim exchange payload... }}

The exchange payload is stored **verbatim**. The recorded stream is the single
source of truth (§4.1) and every downstream artefact is a pure function of it,
so the recorder must never reinterpret, reorder or drop a field. Sequence
checking happens here only to *log* gaps; the converter is what acts on them.

Why Python (§4.3, and say this if asked): data collection had to start
immediately and run unattended for weeks. asyncio websockets with reconnect,
rotation and a watchdog is a one-day build. Recording reliability beats language
purity, and the C++ story of this project is the book core and the simulator.

NO TRADING. This module opens public market-data endpoints only. There is no
API key handling, no signed request, no order placement, and there never will
be.

Usage::

    python recorder.py --config recorder/symbols.yaml
    python recorder.py --symbols BTCUSDT ETHUSDT --market futures --out data/raw
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import gzip
import json
import logging
import os
import signal
import sys
import time
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

try:
    import websockets
except ImportError:  # pragma: no cover - dependency check
    print(
        "recorder: the 'websockets' package is required.\n"
        "    python -m pip install -r recorder/requirements.txt",
        file=sys.stderr,
    )
    raise

LOG = logging.getLogger("recorder")

# --- venue endpoints (public market data only) ------------------------------
ENDPOINTS = {
    "spot": {
        "ws": "wss://stream.binance.com:9443/stream?streams=",
        "rest": "https://api.binance.com/api/v3/depth",
        "snapshot_limit": 5000,
    },
    "futures": {
        "ws": "wss://fstream.binance.com/stream?streams=",
        "rest": "https://fapi.binance.com/fapi/v1/depth",
        "snapshot_limit": 1000,
    },
}

SNAPSHOT_INTERVAL_S = 30 * 60
ROTATE_SECONDS = 3600
# Binance closes a websocket after 24 h; reconnect well before that.
RECONNECT_AFTER_S = 12 * 3600
PING_INTERVAL_S = 20
PING_TIMEOUT_S = 20


def now_us() -> int:
    """Local receive time in microseconds. Never used as an exchange timestamp."""
    return time.time_ns() // 1000


def hour_stamp(ts_us: int) -> str:
    return datetime.fromtimestamp(ts_us / 1e6, tz=timezone.utc).strftime("%Y-%m-%dT%H")


# ---------------------------------------------------------------------------
# Hourly-rotated gzip writer
# ---------------------------------------------------------------------------
class RotatingJsonlWriter:
    """One gzip JSONL file per symbol per UTC hour.

    Rotation is by the *event's* hour, not by elapsed time, so a file named
    ``...T14.jsonl.gz`` contains exactly the lines received during hour 14 and a
    restart mid-hour appends to the right file instead of starting a new one.
    """

    def __init__(self, out_dir: Path, symbol: str, flush_every: int = 200) -> None:
        self.out_dir = out_dir
        self.symbol = symbol
        self.flush_every = flush_every
        self._handle: Optional[gzip.GzipFile] = None
        self._hour: Optional[str] = None
        self._since_flush = 0
        self.lines_written = 0
        self.bytes_written = 0

    def _path_for(self, hour: str) -> Path:
        return self.out_dir / self.symbol / f"{self.symbol}_{hour}.jsonl.gz"

    def _open(self, hour: str) -> None:
        path = self._path_for(hour)
        path.parent.mkdir(parents=True, exist_ok=True)
        # Append mode: a restart inside the hour must not truncate what is
        # already recorded. Un-recorded time is unrecoverable; a slightly larger
        # file is not a problem.
        self._handle = gzip.open(path, "at", encoding="utf-8")
        self._hour = hour
        LOG.info("%s: writing %s", self.symbol, path)

    def write(self, record: dict[str, Any]) -> None:
        hour = hour_stamp(record["t"])
        if hour != self._hour:
            self.close()
            self._open(hour)
        assert self._handle is not None
        line = json.dumps(record, separators=(",", ":"), ensure_ascii=False)
        self._handle.write(line)
        self._handle.write("\n")
        self.lines_written += 1
        self.bytes_written += len(line) + 1
        self._since_flush += 1
        if self._since_flush >= self.flush_every:
            self._handle.flush()
            self._since_flush = 0

    def close(self) -> None:
        if self._handle is not None:
            with contextlib.suppress(Exception):
                self._handle.flush()
                self._handle.close()
            self._handle = None
            self._hour = None


# ---------------------------------------------------------------------------
# Gap tracking
# ---------------------------------------------------------------------------
@dataclass
class GapLog:
    """Records sequence discontinuities to a sidecar JSONL file.

    The recorder does not try to repair anything -- it records what happened and
    moves on. Repair is the converter's job (§2.4), and keeping the two separate
    is what keeps the raw stream a faithful record.
    """

    path: Path
    count: int = 0

    def __post_init__(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def record(self, symbol: str, reason: str, expected: Any, got: Any) -> None:
        self.count += 1
        entry = {
            "t": now_us(),
            "iso": datetime.now(tz=timezone.utc).isoformat(),
            "symbol": symbol,
            "reason": reason,
            "expected": expected,
            "got": got,
        }
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(entry, separators=(",", ":")) + "\n")
        LOG.warning("%s: gap (%s) expected=%s got=%s", symbol, reason, expected, got)


@dataclass
class SymbolState:
    symbol: str
    writer: RotatingJsonlWriter
    last_final_id: Optional[int] = None
    messages: int = 0
    trades: int = 0
    snapshots: int = 0
    gaps: int = 0
    last_snapshot_at: float = field(default_factory=lambda: 0.0)


# ---------------------------------------------------------------------------
# REST snapshots
# ---------------------------------------------------------------------------
def fetch_snapshot_blocking(market: str, symbol: str, timeout: float = 10.0) -> dict[str, Any]:
    """Fetch a REST depth snapshot. Public endpoint, no authentication."""
    cfg = ENDPOINTS[market]
    url = f"{cfg['rest']}?symbol={symbol}&limit={cfg['snapshot_limit']}"
    request = urllib.request.Request(url, headers={"User-Agent": "lob-sim-recorder/1.0"})
    with urllib.request.urlopen(request, timeout=timeout) as response:  # noqa: S310
        return json.loads(response.read().decode("utf-8"))


async def snapshot_loop(
    market: str,
    states: dict[str, SymbolState],
    stop: asyncio.Event,
    interval_s: float = SNAPSHOT_INTERVAL_S,
) -> None:
    """Take a REST snapshot per symbol every `interval_s`.

    Snapshots are the resync anchors the converter needs (§2.4). One is taken
    immediately at start-up, because a recording that begins with no anchor has
    no usable book until the first periodic snapshot arrives.
    """
    while not stop.is_set():
        for symbol, state in states.items():
            try:
                payload = await asyncio.to_thread(fetch_snapshot_blocking, market, symbol)
            except Exception as exc:  # noqa: BLE001 - the loop must survive anything
                LOG.error("%s: snapshot failed: %s", symbol, exc)
                continue
            state.writer.write({"t": now_us(), "c": "snapshot", "s": symbol, "d": payload})
            state.snapshots += 1
            state.last_snapshot_at = time.time()
            LOG.info("%s: snapshot lastUpdateId=%s", symbol, payload.get("lastUpdateId"))
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(stop.wait(), timeout=interval_s)


# ---------------------------------------------------------------------------
# Websocket stream
# ---------------------------------------------------------------------------
def build_stream_url(market: str, symbols: list[str]) -> str:
    streams = []
    for symbol in symbols:
        lower = symbol.lower()
        streams.extend([f"{lower}@depth@100ms", f"{lower}@aggTrade", f"{lower}@bookTicker"])
    return ENDPOINTS[market]["ws"] + "/".join(streams)


CHANNEL_BY_EVENT = {"depthUpdate": "depth", "aggTrade": "aggTrade"}


def classify(payload: dict[str, Any]) -> str:
    event = payload.get("e")
    if event in CHANNEL_BY_EVENT:
        return CHANNEL_BY_EVENT[event]
    # bookTicker carries no "e" field on the spot combined stream.
    if "b" in payload and "B" in payload and "a" in payload and "A" in payload:
        return "bookTicker"
    return "unknown"


def check_sequence(state: SymbolState, payload: dict[str, Any], gaps: GapLog) -> None:
    """Log discontinuities. Does not attempt repair -- see the GapLog docstring."""
    first_id = payload.get("U")
    final_id = payload.get("u")
    prev_id = payload.get("pu")
    if final_id is None:
        return
    if state.last_final_id is not None:
        if prev_id is not None:
            # USD-M futures: pu must equal the previous message's u.
            if prev_id != state.last_final_id:
                state.gaps += 1
                gaps.record(state.symbol, "futures pu != previous u", state.last_final_id, prev_id)
        elif first_id is not None and first_id != state.last_final_id + 1:
            state.gaps += 1
            gaps.record(state.symbol, "spot U != previous u + 1", state.last_final_id + 1, first_id)
    state.last_final_id = final_id


async def stream_loop(
    market: str,
    symbols: list[str],
    states: dict[str, SymbolState],
    gaps: GapLog,
    stop: asyncio.Event,
) -> None:
    """Consume the combined stream forever, reconnecting on any failure."""
    url = build_stream_url(market, symbols)
    backoff = 1.0
    while not stop.is_set():
        try:
            LOG.info("connecting to %s", url[:120] + ("..." if len(url) > 120 else ""))
            async with websockets.connect(
                url,
                ping_interval=PING_INTERVAL_S,
                ping_timeout=PING_TIMEOUT_S,
                max_queue=None,
                close_timeout=5,
            ) as socket:
                LOG.info("connected; recording %d symbols", len(symbols))
                backoff = 1.0
                connected_at = time.time()

                while not stop.is_set():
                    if time.time() - connected_at > RECONNECT_AFTER_S:
                        # Binance drops a connection after 24 h; pre-empt it at a
                        # moment of our choosing rather than losing messages at
                        # one of its choosing.
                        LOG.info("scheduled reconnect after %.1f h", RECONNECT_AFTER_S / 3600)
                        break
                    try:
                        raw = await asyncio.wait_for(socket.recv(), timeout=60.0)
                    except asyncio.TimeoutError:
                        LOG.warning("no message for 60 s; reconnecting")
                        break

                    received_us = now_us()
                    try:
                        envelope = json.loads(raw)
                    except json.JSONDecodeError:
                        LOG.error("undecodable frame, skipped")
                        continue

                    payload = envelope.get("data", envelope)
                    symbol = payload.get("s")
                    if symbol is None:
                        continue
                    state = states.get(symbol.upper())
                    if state is None:
                        continue

                    channel = classify(payload)
                    if channel == "depth":
                        check_sequence(state, payload, gaps)
                    elif channel == "aggTrade":
                        state.trades += 1

                    state.writer.write(
                        {"t": received_us, "c": channel, "s": state.symbol, "d": payload}
                    )
                    state.messages += 1

        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 - the recorder must never die
            LOG.error("stream error: %s", exc)

        if stop.is_set():
            break
        # A reconnect is a discontinuity by definition: whatever the venue sent
        # while we were away is lost, so mark it for the converter to see.
        for state in states.values():
            if state.last_final_id is not None:
                gaps.record(state.symbol, "reconnect", state.last_final_id, None)
                state.last_final_id = None
        LOG.info("reconnecting in %.1f s", backoff)
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(stop.wait(), timeout=backoff)
        backoff = min(backoff * 2.0, 60.0)


async def heartbeat_loop(
    states: dict[str, SymbolState], gaps: GapLog, stop: asyncio.Event, period_s: float = 300.0
) -> None:
    """Periodic one-line status, so a silent recorder is visibly silent."""
    while not stop.is_set():
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(stop.wait(), timeout=period_s)
        if stop.is_set():
            break
        for state in states.values():
            LOG.info(
                "%s: msgs=%d trades=%d snapshots=%d gaps=%d bytes=%.1fMB",
                state.symbol,
                state.messages,
                state.trades,
                state.snapshots,
                state.gaps,
                state.writer.bytes_written / 1e6,
            )
        LOG.info("total gaps logged: %d", gaps.count)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def load_symbols_from_config(path: Path) -> tuple[list[str], str]:
    """Read symbols and market from the tiny YAML subset the project uses."""
    symbols: list[str] = []
    market = "futures"
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("symbols:"):
            value = line.split(":", 1)[1].strip()
            if value.startswith("[") and value.endswith("]"):
                symbols = [s.strip().strip("\"'") for s in value[1:-1].split(",") if s.strip()]
        elif line.startswith("- ") and not symbols:
            symbols.append(line[2:].strip().strip("\"'"))
        elif line.startswith("market:"):
            market = line.split(":", 1)[1].strip().strip("\"'")
    return symbols, market


async def amain(args: argparse.Namespace) -> int:
    symbols = [s.upper() for s in args.symbols]
    market = args.market
    if args.config:
        config_symbols, config_market = load_symbols_from_config(Path(args.config))
        symbols = symbols or [s.upper() for s in config_symbols]
        market = args.market or config_market
    if not symbols:
        LOG.error("no symbols given (use --symbols or --config)")
        return 2
    if market not in ENDPOINTS:
        LOG.error("--market must be one of %s", ", ".join(ENDPOINTS))
        return 2

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    gaps = GapLog(out_dir / "gaps.jsonl")
    states = {s: SymbolState(s, RotatingJsonlWriter(out_dir, s)) for s in symbols}

    LOG.info("recording %s on %s -> %s", ", ".join(symbols), market, out_dir)

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signame in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, signame, None)
        if sig is None:
            continue
        with contextlib.suppress(NotImplementedError):
            loop.add_signal_handler(sig, stop.set)

    tasks = [
        asyncio.create_task(stream_loop(market, symbols, states, gaps, stop)),
        asyncio.create_task(snapshot_loop(market, states, stop, args.snapshot_interval)),
        asyncio.create_task(heartbeat_loop(states, gaps, stop)),
    ]
    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass
    except KeyboardInterrupt:
        stop.set()
    finally:
        stop.set()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for state in states.values():
            state.writer.close()
            LOG.info(
                "%s: closed after %d messages, %d gaps", state.symbol, state.messages, state.gaps
            )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--symbols", nargs="*", default=[], help="e.g. BTCUSDT ETHUSDT")
    parser.add_argument("--market", default=None, choices=[None, "spot", "futures"])
    parser.add_argument("--out", default="data/raw", help="output directory")
    parser.add_argument("--config", default=None, help="recorder/symbols.yaml")
    parser.add_argument(
        "--snapshot-interval",
        type=float,
        default=SNAPSHOT_INTERVAL_S,
        help="seconds between REST depth snapshots (default 1800)",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if args.market is None and not args.config:
        args.market = "futures"

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S",
        stream=sys.stdout,
    )
    # Line-buffered stdout so `tail -f` on the log shows progress live.
    with contextlib.suppress(Exception):
        sys.stdout.reconfigure(line_buffering=True)

    try:
        return asyncio.run(amain(args))
    except KeyboardInterrupt:
        LOG.info("interrupted")
        return 0


if __name__ == "__main__":
    sys.exit(main())
