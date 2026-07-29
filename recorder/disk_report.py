#!/usr/bin/env python3
"""Disk-usage and data-health report (master plan Phase 0 gate).

Answers the three questions that decide whether the recording plan survives:

  1. How much disk is a day of each symbol actually costing?
     (§4.3 guesses 1-3 GB/day/symbol compressed -- "check after day one".)
  2. How many hours are missing, and where?
  3. How many sequence gaps were logged, and when?

A gap report is part of the Phase 0 acceptance gate, and the missing-hours table
is what tells you whether the 24 h unattended run really was unattended.

Usage::

    python disk_report.py --out data/raw
    python disk_report.py --out data/raw --json report.json
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

HOUR_FORMAT = "%Y-%m-%dT%H"


def human(n_bytes: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n_bytes) < 1024.0:
            return f"{n_bytes:6.1f} {unit}"
        n_bytes /= 1024.0
    return f"{n_bytes:6.1f} PB"


def parse_hour(path: Path) -> datetime | None:
    stem = path.name.removesuffix(".jsonl.gz")
    _, _, hour = stem.rpartition("_")
    try:
        return datetime.strptime(hour, HOUR_FORMAT).replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def scan_symbol(symbol_dir: Path) -> dict[str, Any]:
    files = sorted(symbol_dir.glob("*.jsonl.gz"))
    hours: dict[datetime, int] = {}
    total = 0
    for path in files:
        hour = parse_hour(path)
        size = path.stat().st_size
        total += size
        if hour is not None:
            hours[hour] = hours.get(hour, 0) + size

    by_day: dict[str, int] = defaultdict(int)
    for hour, size in hours.items():
        by_day[hour.strftime("%Y-%m-%d")] += size

    missing: list[str] = []
    empty: list[str] = []
    if hours:
        start, end = min(hours), max(hours)
        cursor = start
        while cursor <= end:
            if cursor not in hours:
                missing.append(cursor.strftime(HOUR_FORMAT))
            elif hours[cursor] < 1024:
                # A gzip file this small holds no data; it means the recorder
                # opened the hour and then stopped.
                empty.append(cursor.strftime(HOUR_FORMAT))
            cursor += timedelta(hours=1)

    return {
        "files": len(files),
        "bytes": total,
        "hours_present": len(hours),
        "first_hour": min(hours).strftime(HOUR_FORMAT) if hours else None,
        "last_hour": max(hours).strftime(HOUR_FORMAT) if hours else None,
        "missing_hours": missing,
        "suspiciously_empty_hours": empty,
        "bytes_by_day": dict(sorted(by_day.items())),
    }


def read_gaps(out_dir: Path) -> dict[str, Any]:
    path = out_dir / "gaps.jsonl"
    if not path.exists():
        return {"total": 0, "by_symbol": {}, "by_reason": {}, "recent": []}
    by_symbol: Counter[str] = Counter()
    by_reason: Counter[str] = Counter()
    entries: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            entries.append(entry)
            by_symbol[entry.get("symbol", "?")] += 1
            by_reason[entry.get("reason", "?")] += 1
    return {
        "total": len(entries),
        "by_symbol": dict(by_symbol),
        "by_reason": dict(by_reason),
        "recent": entries[-10:],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="data/raw", help="recorder output directory")
    parser.add_argument("--json", default=None, help="also write the report as JSON here")
    args = parser.parse_args()

    out_dir = Path(args.out)
    if not out_dir.is_dir():
        print(f"disk_report: {out_dir} does not exist", file=sys.stderr)
        return 1

    symbol_dirs = sorted(p for p in out_dir.iterdir() if p.is_dir() and not p.name.startswith("."))
    report: dict[str, Any] = {
        "generated": datetime.now(tz=timezone.utc).isoformat(),
        "out_dir": str(out_dir),
        "symbols": {},
    }

    print(f"\n=== recorder data report: {out_dir} ===\n")
    grand_total = 0
    for symbol_dir in symbol_dirs:
        info = scan_symbol(symbol_dir)
        report["symbols"][symbol_dir.name] = info
        grand_total += info["bytes"]

        print(f"{symbol_dir.name}")
        print(f"  files            {info['files']}")
        print(f"  size             {human(info['bytes'])}")
        print(f"  hours present    {info['hours_present']}  ({info['first_hour']} .. {info['last_hour']})")
        if info["missing_hours"]:
            shown = ", ".join(info["missing_hours"][:8])
            more = "" if len(info["missing_hours"]) <= 8 else f" (+{len(info['missing_hours']) - 8} more)"
            print(f"  MISSING HOURS    {len(info['missing_hours'])}: {shown}{more}")
        if info["suspiciously_empty_hours"]:
            print(f"  EMPTY HOURS      {len(info['suspiciously_empty_hours'])}")
        for day, size in info["bytes_by_day"].items():
            print(f"    {day}   {human(size)}")
        print()

    gaps = read_gaps(out_dir)
    report["gaps"] = gaps
    print("sequence gaps")
    print(f"  total            {gaps['total']}")
    for symbol, count in sorted(gaps["by_symbol"].items()):
        print(f"    {symbol:<12} {count}")
    for reason, count in sorted(gaps["by_reason"].items()):
        print(f"    {reason:<28} {count}")
    print()

    usage = shutil.disk_usage(out_dir)
    report["disk"] = {"total": usage.total, "used": usage.used, "free": usage.free}
    print("disk")
    print(f"  total            {human(usage.total)}")
    print(f"  free             {human(usage.free)}")
    print(f"  recorded so far  {human(grand_total)}")

    # Days of runway at the observed rate -- the number that decides whether the
    # six-week recording plan actually fits (§4.3).
    days_seen = {
        day for info in report["symbols"].values() for day in info["bytes_by_day"]
    }
    if days_seen and grand_total > 0:
        per_day = grand_total / max(1, len(days_seen))
        runway = usage.free / per_day if per_day > 0 else float("inf")
        report["bytes_per_day"] = per_day
        report["days_of_runway"] = runway
        print(f"  rate             {human(per_day)}/day across {len(days_seen)} day(s)")
        print(f"  runway           {runway:.1f} days at the current rate")
        if runway < 45:
            print("  WARNING: less than the ~6 weeks the evaluation window needs.")
    print()

    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"JSON written to {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
