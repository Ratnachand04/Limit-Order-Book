# Benchmark results

**Status: not yet measured.**

This file is the permanent home of the performance numbers the CV line and the
paper's reproducibility appendix quote. It is empty of numbers on purpose:
CLAUDE.md rule 4 forbids fabricated results, and a benchmark table filled with
plausible-looking placeholders is exactly the kind of thing that survives into a
README and then into an interview.

Numbers appear here only after `lob_bench` has actually run on named hardware.

## How to produce them

```bash
cmake --preset msvc-release        # or gcc-release
cmake --build --preset msvc-release
./build/msvc-release/bin/lob_bench \
    --benchmark_repetitions=10 \
    --benchmark_report_aggregates_only=true \
    --benchmark_out=bench/raw/$(date +%Y%m%d).json \
    --benchmark_out_format=json
```

Then fill in the tables below **from that output**, and record the hardware.

Run benchmarks on a machine that is otherwise idle, with the release preset
(`RelWithDebInfo`), and never with `LOB_DUAL_BOOK` or `LOB_SANITIZE` on —
`BM_ReplayDualBook` exists to show how large that distortion is.

## Hardware (fill in)

| Field | Value |
|---|---|
| CPU | |
| Cores / threads | |
| Base / boost clock | |
| RAM | |
| OS | |
| Compiler | |
| Build type | RelWithDebInfo |
| Date | |
| Commit | |

## Targets (master plan §4.10)

| Metric | Target | Measured |
|---|---|---|
| Converter throughput | ≥ 1M msg/s | |
| Replay throughput (book + strategy) | ≥ 300–500k events/s | |
| `DenseBook::ApplyDepth` p50 | < 100 ns | |
| `DenseBook::ApplyDepth` p99 | < 1 µs | |

## Book core

| Benchmark | Items/s | p50 ns | p90 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|---|
| `DenseBook/ApplyDepth` | | | | | |
| `MapBook/ApplyDepth` | | | | | |
| `DenseBook/Reanchor` | | | | | |
| `DenseBook/BestBidAsk` | | | | | |
| `MapBook/BestBidAsk` | | | | | |

Note on method: the percentile benchmarks time each call individually, and that
timing costs more than the call itself, so the reported latencies are an **upper
bound**. The throughput rows have no per-call timing overhead and are the honest
measure of rate. Say this if asked.

## Converter

| Benchmark | Messages/s |
|---|---|
| `BM_ConvertDepthLines` (JSONL → events) | |
| `BM_ParseDepthPayloadOnly` | |
| `BM_ReadBinaryEvents` | |
| `BM_WriteBinaryEvents` | |

The ratio of `BM_ReadBinaryEvents` to `BM_ConvertDepthLines` is the measured
version of Part 11 pitfall #2 ("JSON parsing costs 100× the book update"), and
it is the justification for the two-stage pipeline. Quote it as a ratio, not as
a claim.

## Replay

| Benchmark | Events/s |
|---|---|
| `BM_ReplayS0Queue` | |
| `BM_ReplayS0Touch` | |
| `BM_ReplayS1` | |
| `BM_ReplayS2AS` | |
| `BM_ReplayS3` | |
| `BM_ReplayProportionalQueue` | |
| `BM_ReplayDualBook` | |
| `BM_ReplayWithProbes` | |

## Caveat on the input

These benchmarks run on the synthetic session from `tests/test_support.cpp`, not
on recorded data — the recorder has not produced a dataset yet. Synthetic input
is fine for comparing implementations against each other, but the absolute
throughput on real data will differ (real depth messages carry more levels and
the touch moves differently). Re-run against a real `.lobbin` day before quoting
an absolute number anywhere, and note which input produced it.
