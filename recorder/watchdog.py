#!/usr/bin/env python3
"""Recorder watchdog (master plan Phase 0 gate: "systemd/cron watchdog script").

Checks that the recorder is alive and actually writing, and restarts it if not.
Designed to be run from cron or the Windows Task Scheduler every few minutes.

The failure this exists to catch is not "the process died" -- systemd handles
that. It is the quieter one: the process is up, the socket is open, and nothing
has been written for twenty minutes. Every un-recorded day is evaluation data
that can never be recovered, so silence has to be treated as failure.

Usage::

    python watchdog.py --out data/raw --symbols BTCUSDT ETHUSDT \\
        --restart-cmd "python recorder/recorder.py --config recorder/symbols.yaml"

    # cron, every 5 minutes:
    */5 * * * * cd /srv/lob && python recorder/watchdog.py --out data/raw \\
        --symbols BTCUSDT --restart-cmd "..." >> logs/watchdog.log 2>&1
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import shlex
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

LOG = logging.getLogger("watchdog")

DEFAULT_STALE_SECONDS = 600.0  # 10 min with no new bytes is a dead recorder


def newest_file(out_dir: Path, symbol: str) -> Optional[Path]:
    symbol_dir = out_dir / symbol
    if not symbol_dir.is_dir():
        return None
    candidates = sorted(symbol_dir.glob(f"{symbol}_*.jsonl.gz"))
    return candidates[-1] if candidates else None


def seconds_since_write(path: Path) -> float:
    return max(0.0, time.time() - path.stat().st_mtime)


def state_path(out_dir: Path) -> Path:
    return out_dir / ".watchdog_state.json"


def load_state(out_dir: Path) -> dict:
    path = state_path(out_dir)
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(out_dir: Path, state: dict) -> None:
    try:
        state_path(out_dir).write_text(json.dumps(state, indent=2), encoding="utf-8")
    except OSError as exc:
        LOG.error("cannot write watchdog state: %s", exc)


def recorder_is_running(pattern: str) -> bool:
    """Best-effort process check that works on both Windows and POSIX."""
    try:
        if os.name == "nt":
            output = subprocess.run(
                ["wmic", "process", "get", "commandline"],
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            ).stdout
        else:
            output = subprocess.run(
                ["ps", "-eo", "args"], capture_output=True, text=True, timeout=20, check=False
            ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        LOG.warning("process check failed (%s); relying on file freshness alone", exc)
        return True
    # Exclude the watchdog's own command line, which contains the pattern too.
    own = " ".join(sys.argv)
    for line in output.splitlines():
        if pattern in line and line.strip() != own.strip() and "watchdog" not in line:
            return True
    return False


def restart(command: str, log_path: Optional[Path]) -> bool:
    LOG.warning("restarting recorder: %s", command)
    try:
        stdout = None
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            stdout = log_path.open("a", encoding="utf-8")
        kwargs: dict = {"stdout": stdout, "stderr": subprocess.STDOUT}
        if os.name == "nt":
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
        else:
            kwargs["start_new_session"] = True
        subprocess.Popen(shlex.split(command), **kwargs)  # noqa: S603
        return True
    except (OSError, ValueError) as exc:
        LOG.error("restart failed: %s", exc)
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="data/raw", help="recorder output directory")
    parser.add_argument("--symbols", nargs="+", required=True)
    parser.add_argument("--restart-cmd", required=True, help="command to relaunch the recorder")
    parser.add_argument(
        "--stale-seconds",
        type=float,
        default=DEFAULT_STALE_SECONDS,
        help="a symbol with no new bytes for this long counts as stalled",
    )
    parser.add_argument("--log", default=None, help="append the restarted recorder's output here")
    parser.add_argument("--dry-run", action="store_true", help="report only, never restart")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s watchdog: %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S",
        stream=sys.stdout,
    )

    out_dir = Path(args.out)
    state = load_state(out_dir)
    now_iso = datetime.now(tz=timezone.utc).isoformat()

    problems: list[str] = []
    for symbol in args.symbols:
        newest = newest_file(out_dir, symbol)
        if newest is None:
            problems.append(f"{symbol}: no output file at all")
            continue
        age = seconds_since_write(newest)
        size_mb = newest.stat().st_size / 1e6
        LOG.info("%s: %s  %.1f MB, last write %.0f s ago", symbol, newest.name, size_mb, age)
        if age > args.stale_seconds:
            problems.append(f"{symbol}: stalled, last write {age:.0f} s ago ({newest.name})")

    running = recorder_is_running("recorder.py")
    if not running:
        problems.append("recorder process not found")

    state["last_check"] = now_iso
    state["problems"] = problems

    if not problems:
        LOG.info("healthy: process up and all %d symbols writing", len(args.symbols))
        state["last_healthy"] = now_iso
        save_state(out_dir, state)
        return 0

    for problem in problems:
        LOG.error("%s", problem)

    if args.dry_run:
        LOG.warning("--dry-run: not restarting")
        save_state(out_dir, state)
        return 1

    if restart(args.restart_cmd, Path(args.log) if args.log else None):
        state["last_restart"] = now_iso
        state["restart_count"] = int(state.get("restart_count", 0)) + 1
        LOG.warning("restart issued (total restarts: %d)", state["restart_count"])
        save_state(out_dir, state)
        return 1

    save_state(out_dir, state)
    return 2


if __name__ == "__main__":
    sys.exit(main())
