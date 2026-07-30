# Benchmark results

First measured run. **Two of the four master-plan targets are not met**; they are
recorded as measured rather than adjusted, and the likely causes are named below.

## Hardware and build

| Field | Value |
|---|---|
| CPU | 12th Gen Intel Core i7-12650H |
| Cores / threads | 10 physical / 16 logical |
| Base clock | 2.30 GHz (reported 2.688 GHz under load) |
| Caches | L1d 48 KiB ×8, L1i 32 KiB ×8, L2 1280 KiB ×8, L3 24576 KiB |
| RAM | 16 GB |
| OS | Windows 11 Home 10.0.26200 |
| Compiler | GCC 16.1.0 (MinGW-w64, UCRT, POSIX threads) |
| Build type | RelWithDebInfo, `-O2`, `-Werror`, `LOB_DUAL_BOOK=OFF`, `LOB_SANITIZE=OFF` |
| Date | 2026-07-30 |
| Input | synthetic 10-minute session (`tests/test_support.cpp`), 168,701 events |
| Command | `lob_bench --benchmark_min_time=1s` |
| Raw output | `bench/raw/20260730_i7-12650H.json` |

**This is a laptop, and it was not verified idle.** Treat every figure as a
lower bound on what the code can do, and re-measure before quoting any of them
in the paper.

## Targets (master plan §4.10)

| Metric | Target | Measured | Met? |
|---|---|---|---|
| Converter throughput | ≥ 1M msg/s | **538 k msg/s** | ✗ |
| Replay (book + strategy) | ≥ 300–500 k events/s | **308 k/s** (S0_queue), **166–183 k/s** (S1/S2/S3) | partly |
| `DenseBook::ApplyDepth` p50 | < 100 ns | **8.93 ns** | ✓ |
| `DenseBook::ApplyDepth` p99 | < 1 µs | **≤ 100 ns** | ✓ |

## Book core

| Benchmark | ns/op | items/s |
|---|---|---|
| `DenseBook/ApplyDepth/throughput` | **8.93** | 113.7 M/s |
| `MapBook/ApplyDepth/throughput` | 68.1 | 14.8 M/s |
| `DenseBook/Reanchor` | 36,187 | 28.7 k/s |
| `DenseBook/BestBidAsk` | 1.14 | 1.79 G/s |
| `MapBook/BestBidAsk` | 1.16 | 1.77 G/s |
| `DenseBook/DepthWithin` n=1 / 5 / 20 | 9.89 / 28.0 / 110 | — |
| `MapBook/DepthWithin` n=1 / 5 / 20 | 6.02 / 14.2 / 70.0 | — |

**The dense path is 7.6× faster than the map path on `ApplyDepth`** (8.93 ns vs
68.1 ns), which is the whole justification for carrying two implementations.

Two honest caveats:

1. **`DepthWithin` is *slower* on the dense path than on the map** (110 ns vs
   70 ns at n=20). The bitmap scan pays a fixed word-load cost per level while
   `std::map`'s iterator just follows already-hot pointers, and `DepthWithin` is
   called at strategy cadence rather than per event, so it was never the thing
   being optimised. Worth knowing before quoting "the dense book is faster"
   without qualification.
2. **The percentile numbers are quantised.** `std::chrono::steady_clock` on
   Windows has 100 ns granularity, so the reported `p50_ns = 0` means "below
   timer resolution", not zero. The table below is what the tool printed; the
   throughput row above is the trustworthy latency measure.

| Benchmark | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| `DenseBook/ApplyDepth` | 0 (< 100 ns) | 100 ns | 100 ns | 200 ns | 383 µs |
| `MapBook/ApplyDepth` | 100 ns | 200 ns | 300 ns | 500 ns | 232 µs |

The 383 µs maxima are re-anchors and OS scheduling noise, not steady-state cost.

## Converter

| Benchmark | Rate |
|---|---|
| `BM_ConvertDepthLines` (JSONL → events) | **538 k msg/s** |
| `BM_ParseDepthPayloadOnly` | 543 k msg/s |
| `BM_ReadBinaryEvents` | **71.4 M events/s** |
| `BM_WriteBinaryEvents` | 30.1 M events/s |

**Target missed: 538 k msg/s against ≥ 1M.** Two identified causes, neither
mysterious:

- The two rows are nearly identical (538 k vs 543 k), which says the cost is
  essentially *all* in JSON parsing — the sequence checking, event construction
  and sorting are free by comparison. Making the target means making the parser
  faster, not the pipeline leaner.
- `BM_ConvertDepthLines` constructs a fresh `Converter` per iteration, and its
  constructor `reserve()`s one million events — a ~32 MB allocation and
  first-touch per iteration, inside the timed region. That is a measurement
  artefact *and* a real inefficiency for short inputs.

**The parse-once ratio: 71.4 M / 538 k ≈ 133×.** That is the measured version of
Part 11 pitfall #2 ("JSON parsing costs 100× the book update"), and it is the
justification for the two-stage pipeline. Quote it as a ratio.

## Replay

168,701 events per run, single-threaded, no CSV writing.

| Benchmark | events/s | ms/run |
|---|---|---|
| `BM_ReplayS0Queue` | **308 k** | 570 |
| `BM_ReplayS0Touch` | 89.2 k | 1980 |
| `BM_ReplayS1` | 169 k | 1007 |
| `BM_ReplayS2AS` | 166 k | 1057 |
| `BM_ReplayS3` | 183 k | 959 |
| `BM_ReplayProportionalQueue` | 161 k | 1066 |
| `BM_ReplayDualBook` | 154 k | 1129 |
| `BM_ReplayWithProbes` | 130 k | 1320 |

**Target partly missed.** S0_queue reaches the bottom of the 300–500 k band; the
quoting strategies run at roughly half that. Reading the table:

- **S0_touch is the slowest at 89 k/s**, which is not a code problem — the touch
  rule fills 2.7× as often, and every fill costs a ledger update, a markout
  registration and a strategy callback. The naive model is *more* expensive to
  simulate than the honest one because it invents work.
- S1/S2/S3 at ~170 k/s pay for order reconciliation on every 100 ms timer plus
  the queue tracker's `std::map` lookups per level update. The A–S closed form
  itself is not the cost: S2_AS (166 k) and S1 (169 k) are within noise of each
  other, so the transcendental functions are free relative to the bookkeeping.
- **PROP costs 5% over PESS** (161 k vs 169 k) — the RNG draw on every
  unexplained level decrease.
- **Dual-book mode costs only 9%** (154 k vs 169 k), which is cheaper than
  expected and makes it reasonable to leave on in CI.
- Probes cost 23%, which is what RQ2 costs.

If the target matters, the first thing to profile is the per-level-update path
through `QueueTracker`, where a `std::map` lookup happens for every depth event
at a tracked level.

## Reproducing

```powershell
. tools\mingw_env.ps1
cmake --preset mingw-release
cmake --build --preset mingw-release --parallel
.\build\mingw-release\bin\lob_bench.exe --benchmark_min_time=1s `
    --benchmark_out=bench\raw\<date>_<cpu>.json --benchmark_out_format=json
```

Never benchmark with `LOB_DUAL_BOOK` or `LOB_SANITIZE` on — `BM_ReplayDualBook`
exists to quantify that distortion, not to be the default.

## Caveat on the input

These run on the synthetic session from `tests/test_support.cpp`, not on
recorded data — the recorder has not produced a dataset yet. Synthetic input is
fine for comparing implementations against each other, but real depth messages
carry more levels per update and the touch moves differently, so absolute
throughput on real data will differ. **Re-run against a real `.lobbin` day
before quoting an absolute number anywhere, and say which input produced it.**
