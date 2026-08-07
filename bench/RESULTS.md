# Benchmark results

All four master-plan targets are met. Two were missed on the first measured run;
what follows records both the misses and what fixed them, because the
before/after is the part worth reading.

## Hardware and build

| Field | Value |
|---|---|
| CPU | 12th Gen Intel Core i7-12650H |
| Cores / threads | 10 physical / 16 logical |
| Base clock | 2.30 GHz |
| Caches | L1d 48 KiB ×8, L1i 32 KiB ×8, L2 1280 KiB ×8, L3 24576 KiB |
| RAM | 16 GB |
| OS | Windows 11 Home 10.0.26200 |
| Compiler | GCC 16.1.0 (MinGW-w64, UCRT, POSIX threads) |
| Build | RelWithDebInfo, `-O2 -Werror`, `LOB_DUAL_BOOK=OFF`, `LOB_SANITIZE=OFF` |
| Date | 2026-08-02 |
| Input | synthetic 10-minute session, 168,701 events |
| Raw output | `bench/raw/20260802_i7-12650H.json` |

**This is a laptop and it was not verified idle.** Run-to-run CV is 3% on the
tight micro-benchmarks and 12–15% on the pipeline benchmarks. Every figure below
is a mean over 8 repetitions unless stated. Treat them as lower bounds and
re-measure on a quiet machine before quoting any of them in a paper.

## Targets (master plan §4.10)

| Metric | Target | First run | Now | |
|---|---|---|---|---|
| Converter throughput | ≥ 1M msg/s | 538 k | **~1.00 M** (mean 1.05 M, median 997 k) | ✓ |
| Replay (book + strategy) | ≥ 300–500 k events/s | 166 k | **6.2 M** (median, S1) | ✓ |
| `DenseBook::ApplyDepth` p50 | < 100 ns | 8.9 ns | **10.1 ns** | ✓ |
| `DenseBook::ApplyDepth` p99 | < 1 µs | ≤ 100 ns (unresolvable) | **43 ns** | ✓ |

---

## The two misses, and what they were

### 1. Replay: 166 k → 6.2 M events/s (37×)

`OrderGateway::orders_` is a `std::map` that keeps every order ever placed,
because the analytics need a filled order's placement details long after it went
terminal. `LiveOrders()` and `LiveQty()` scanned that whole map.

`AtInventoryCap()` calls `LiveQty()` twice per strategy timer tick, and the
touch-rule fill model calls `LiveOrders()` on every trade. So the cost of a
query was O(orders placed so far), making replay **quadratic in the length of
the run**. On a 600 s session at a 100 ms requote cadence that is ~12,000 dead
entries being walked thousands of times.

The fix is a live-order index — a flat `std::vector<OrderId>` per side,
maintained on the state transitions that make an order terminal. Live orders
number a handful, so a vector beats any node structure, and `orders_` keeps its
full history for the analytics.

The tell was in the first run's own table: **S0_touch was the slowest strategy
at 89 k/s**, which makes no sense for the simplest strategy until you notice it
is the one that scans on every trade.

Two things this is worth saying out loud:

- It is a *complexity* bug, not a tuning problem. No amount of micro-optimising
  the scan would have found it; the shape of the table did.
- The unit tests never caught it, and could not have — they are correct, and
  they run short scenarios where n stays small. Quadratic behaviour is only
  visible at length.

### 2. Converter: 538 k → ~1.00 M msg/s (1.9×)

Two independent causes, found by reading the parse path rather than by guessing:

**Every message was JSON-parsed three times.** Once by `SkipValue()` to find the
extent of the recorder envelope's `"d"` payload, once by a probe that walked the
payload looking for the combined-stream `{"stream":…,"data":…}` wrapper, and
once for real. Fixes:

- `SkipValueSpan()` finds a container's extent by **bracket matching** — one
  tight loop tracking string state and escapes — instead of a recursive parse.
  It needs the payload's *extent*, never its contents.
- The wrapper probe now compares **only the first key**. Binance puts `stream`
  first, and this project's own recorder never emits the wrapper at all, so the
  common path costs one string comparison.

**Prices were parsed to double and then divided back to integers.** Prices and
quantities live on a decimal grid, so with a tick of 0.01 the string
`"99999.99"` *is* 9999999 ticks — the integer the book wants is already sitting
in the digits. The double route spent a `from_chars`, a multiply and an `llround`
per field to recover it. `ReadFixedPoint()` reads the digits straight into
fixed point, rounding half-away-from-zero so it agrees with
`llround(value / size)` exactly.

Instruments whose tick is *not* a power of ten (0.5, 2.5, 5) still take the
double path — `Instrument` detects the case once at construction. Both paths
exist because correctness for the general case is not negotiable for a 2× win on
the common one.

After the fix, `BM_ParseDepthPayloadOnly` (~959 k msg/s) and the full pipeline
(~1.00 M msg/s) are within noise of each other, which says the remaining cost is
essentially all parsing. Going further means a different parsing strategy
(SIMD digit scanning), not more of the same.

---

## Book core

| Benchmark | ns/op | items/s |
|---|---|---|
| `DenseBook/ApplyDepth/throughput` | **10.1** (CV 2.9%) | 102 M/s |
| `MapBook/ApplyDepth/throughput` | 65.5 | 15.7 M/s |
| `DenseBook/Reanchor` | 40,797 | 24.7 k/s |
| `DenseBook/BestBidAsk` | 1.24 | 1.70 G/s |
| `MapBook/BestBidAsk` | 1.32 | 1.53 G/s |
| `DenseBook/DepthWithin` n=1 / 5 / 20 | 9.18 / 23.7 / 106 | — |
| `MapBook/DepthWithin` n=1 / 5 / 20 | 6.95 / 15.7 / 80.5 | — |

**The dense path is 6.5× faster than the map on `ApplyDepth`**, which is the
whole justification for carrying two implementations.

**`DepthWithin` is *slower* on the dense path** (106 ns vs 80.5 ns at n=20). The
bitmap scan pays a fixed word-load per level while `std::map`'s iterator follows
already-hot pointers. It is called at strategy cadence, not per event, so it was
never the thing being optimised — but "the dense book is faster" is not true
without qualification, and it should not be said that way.

### Tail latency

Measured with an **lfence-serialised `rdtsc`** and a calibrated cycle→ns
conversion (`bench/cycle_timer.hpp`), not `std::chrono::steady_clock`.

The first run reported `p50 = 0 ns`. That was the instrument, not the code:
`steady_clock` on Windows is backed by QueryPerformanceCounter at ~100 ns
granularity, so an operation taking 10 ns quantises to 0 or 100. For a benchmark
whose entire claim is "under 100 ns", the timer was useless.

| Benchmark | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| `DenseBook/ApplyDepth` | 14.9 ns | 28.3 ns | **43.2 ns** | 105 ns | 291 µs |
| `MapBook/ApplyDepth` | 52.1 ns | 124 ns | 206 ns | 483 ns | 461 µs |

Timer overhead (the fenced read pair itself) measures **14.9 ns** and is
reported alongside the percentiles and subtracted from them. The median book
update is close enough to that floor that the throughput row above — 10.1 ns,
with no per-call instrumentation — is the more trustworthy figure for the median.
The percentiles are the trustworthy figures for the tail.

The ~300 µs maxima are OS scheduling, not steady-state cost.

## Converter

| Benchmark | Rate |
|---|---|
| `BM_ConvertDepthLines` (JSONL → events) | **~1.00 M msg/s** |
| `BM_ParseDepthPayloadOnly` | 959 k msg/s |
| `BM_ReadBinaryEvents` | 80.2 M events/s |
| `BM_WriteBinaryEvents` | 22.1 M events/s |

**The parse-once ratio: 80.2 M / 1.00 M ≈ 80×.** That is the measured version of
Part 11 pitfall #2 ("JSON parsing costs 100× the book update") and the
justification for the two-stage pipeline. Quote it as a ratio, and note it fell
from 133× only because the JSON side got faster.

## Replay

168,701 events per run, single-threaded, no CSV writing.

| Benchmark | events/s | vs first run |
|---|---|---|
| `BM_ReplayS0Queue` | 7.24 M | 308 k → 24× |
| `BM_ReplayS0Touch` | 4.39 M | 89 k → 49× |
| `BM_ReplayS1` | 6.2 M (median, CV 12%) | 169 k → 37× |
| `BM_ReplayS2AS` | 9.09 M | 166 k → 55× |
| `BM_ReplayS3` | 9.83 M | 183 k → 54× |
| `BM_ReplayProportionalQueue` | 7.68 M | 161 k → 48× |
| `BM_ReplayDualBook` | 6.58 M | 154 k → 43× |
| `BM_ReplayWithProbes` | 4.39 M | 130 k → 34× |

Reading the table:

- **S0_touch is still the slowest**, and now for the right reason: the touch rule
  fills 2.7× as often, and every fill costs a ledger update, a markout
  registration and a strategy callback. The naive model is more expensive to
  simulate than the honest one because it invents work.
- **Dual-book mode costs only ~10%**, cheap enough to leave on in CI.
- Probes cost ~40%. That is what RQ2 costs.
- S2/S3 are *faster* than S1 because they quote less often when σ is not yet
  estimated, not because the A–S closed form is cheap. Do not read that row as
  "the model is free".

## Reproducing

```powershell
. tools\mingw_env.ps1
cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOB_BUILD_BENCH=ON
cmake --build build/mingw --parallel
.\build\mingw\bin\lob_bench.exe --benchmark_min_time=1s --benchmark_repetitions=8 `
    --benchmark_report_aggregates_only=true `
    --benchmark_out=bench\raw\<date>_<cpu>.json --benchmark_out_format=json
```

Never benchmark with `LOB_DUAL_BOOK` or `LOB_SANITIZE` on — `BM_ReplayDualBook`
exists to quantify that distortion, not to be the default.

## Caveat on the input

These run on the synthetic session from `tests/test_support.cpp`, not on
recorded data. Synthetic input is fine for comparing implementations against each
other and for finding complexity bugs, but real depth messages carry more levels
per update and the touch moves differently, so absolute throughput on real data
will differ. **Re-run against a real `.lobbin` day before quoting an absolute
number anywhere, and say which input produced it.**
