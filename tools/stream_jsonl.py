#!/usr/bin/env python3
"""Stream gzipped recorder JSONL to stdout for `lob_convert --in -`.

The converter deliberately has no compression dependency: linking zlib into the
C++ build to read a file that Python already wrote is a dependency for one
function.  Decompression happens in a pipe instead.

    python tools/stream_jsonl.py data/raw/BTCUSDT/*.jsonl.gz \\
        | build/bin/lob_convert --in - --symbol BTCUSDT --tick 0.01 --lot 0.001 \\
              --out data/binary/BTCUSDT_2026-09-01.lobbin

Files are concatenated in the order given, so glob them in sorted order -- the
converter sorts by timestamp anyway, but a monotone input keeps its reorder
buffer small and makes the sequence-continuity check meaningful.

On a POSIX shell `gzip -dc` is faster and does the same thing; this exists so
the documented pipeline works identically on Windows.
"""

from __future__ import annotations

import argparse
import gzip
import sys
from pathlib import Path


def stream(path: Path, out) -> int:
    opener = gzip.open if path.suffix == ".gz" else open
    lines = 0
    with opener(path, "rb") as fh:  # type: ignore[operator]
        for chunk in fh:
            out.write(chunk)
            lines += 1
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", help=".jsonl or .jsonl.gz files, in order")
    parser.add_argument("--verbose", action="store_true", help="report progress on stderr")
    args = parser.parse_args()

    out = sys.stdout.buffer
    total = 0
    for name in args.paths:
        path = Path(name)
        if not path.is_file():
            print(f"stream_jsonl: {path} is not a file", file=sys.stderr)
            return 1
        count = stream(path, out)
        total += count
        if args.verbose:
            print(f"{path}: {count} lines", file=sys.stderr)
    out.flush()
    if args.verbose:
        print(f"total: {total} lines", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
