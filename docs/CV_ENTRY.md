# CV entry — copy-paste ready

Companion to `CV_AND_INTERVIEW.md`, which covers how to *defend* this in an
interview. This file is just the text and the numbers.

Every figure here is reproducible from the repository. Nothing is estimated.

---

## The rule that governs this file

Three things a market-making CV bullet usually states — **fill rate**,
**adverse-selection cost in bp**, and **messages/day** — do not exist yet,
because no market data has been recorded.

Do not fill them in with plausible values. On a quant CV, the first question
about an adverse-selection number is "how did you compute that?", and the people
asking compute it for a living. One unsupportable figure discredits the whole
project, including the parts that are real.

Version A below is what can be defended today. Version B is what to swap in once
the recorder has produced data.

---

## Version A — usable today

```
Queue-Aware Limit Order Book Simulator & Market-Making Study            C++20 / Python
github.com/Ratnachand04/Limit-Order-Book                                   Aug–Oct 2026

• Built a deterministic, event-driven L2 order-book simulator that models a passive
  order's queue position (price-time priority) instead of the naive touch-rule fill
  assumption; brackets the unobservable cancel location under three assumptions
  (pessimistic / optimistic / proportional) and reports all three.

• Implemented Avellaneda–Stoikov optimal quoting (reservation price r = s − qγσ²(T−t);
  spread γσ²(T−t) + (2/γ)ln(1+γ/k)) and the Guéant–Lehalle–Fernandez-Tapia steady-state
  form, with λ(δ)=Ae^(−kδ) fill intensity calibrated by empirical hazard estimation and
  rolling 10-min volatility.

• Diagnosed an O(n²) in the replay hot path from benchmark shape alone (the simplest
  strategy benchmarked slowest); replaced a full-map scan with a live-order index —
  replay 166k → 6.2M events/s (37×). Cut JSONL→binary conversion 538k → 1.0M msg/s by
  eliminating a triple-parse and reading decimal prices straight to fixed-point integers.

• Order book on a dense array with a bit-per-tick occupancy index: apply_depth 10 ns
  median / 43 ns p99, cross-validated against a std::map reference on every update.
  Exact PnL attribution — integer cash in 1e-8 units makes ΔE = spread capture +
  inventory − fees hold at residual exactly 0, asserted per trading day.

• Pre-registered 324-cell experiment matrix (queue model × strategy × maker fee
  −0.5→10 bp × latency 5/50/200 ms) with stationary block-bootstrap CIs; 209 tests
  incl. property/fuzz, golden replay and byte-level determinism enforced in CI.
```

Cut to three bullets: keep 1, 3, 4 — domain, the O(n²), the exact identity.

## Version B — after the recorder has run

Replace the second half of bullet 3, or bullet 5, with the finding:

```
• Quantified the naive-backtest bias on N months of recorded Binance tick data: the
  touch rule overstates fill count by [X.X]× and daily PnL by [Y] bp versus the
  queue-aware model on identical data; adverse selection at 10 s measured [Z] bp per
  fill, and no strategy breaks even above a [W] bp maker fee.
```

Sources: `data/results/matrix/T3_rq1_gap.csv` (X, Y),
`data/figures/T1_markout_summary.csv` (Z), `T2_break_even_fee.csv` (W).

---

## Defensible numbers

| Claim | Value | Source |
|---|---|---|
| `apply_depth` median / p99 | 10 ns / 43 ns | `bench/RESULTS.md` |
| Dense vs `std::map` on `apply_depth` | 6.5× | `bench/RESULTS.md` |
| Replay throughput | 6.2M events/s (from 166k, 37×) | `bench/RESULTS.md` |
| Converter throughput | ~1.0M msg/s (from 538k) | `bench/RESULTS.md` |
| Parse-once ratio (binary read vs JSON) | ~80× | `bench/RESULTS.md` |
| PnL identity residual | exactly 0 | `analytics/include/lob/analytics/ledger.hpp` |
| Tests | 209 | `ctest --test-dir build/mingw` |
| C++ core / tests+bench / Python | ~10.5k / ~4.6k / ~1.8k lines | `git ls-files` |
| Experiment matrix | 324 cells | `configs/matrix.yaml` |
| Strategies | 6 | `strategy/include/lob/strategy/strategies.hpp` |
| Queue assumptions | 3 (PESS / OPT / PROP) | `common/include/lob/config.hpp` |
| Markout horizons | 8, from 0.1 s to 60 s | `configs/base.yaml` |
| Fee grid | −0.5 to 10 bp | `configs/matrix.yaml` |
| Latency grid | 5 / 50 / 200 ms | `configs/matrix.yaml` |

**Caveat to volunteer if asked:** benchmarks ran on a laptop that was not
verified idle, run-to-run CV 12–15% on the pipeline benchmarks, and on synthetic
input. Say it before you are asked.

---

## Venue choice

Use **Binance USD-M perpetual futures**, not spot. The futures diff-depth stream
carries `pu` (previous update ID), which makes sequence-continuity checking exact
rather than inferred from range arithmetic. That is a real reason and it is the
kind of detail that reads as someone who actually handled the data.

---

## Keyword bank

**Microstructure** — limit order book · L2/L3 market data · price-time priority ·
queue position · order book imbalance · microprice / weighted mid · bid-ask
spread · adverse selection · markout · effective and realized spread · toxic
flow · maker-taker fees · tick data · order flow

**Quant methods** — Avellaneda–Stoikov · Guéant–Lehalle–Fernandez-Tapia ·
optimal market making · stochastic optimal control · HJB equation · CARA
utility · reservation price · inventory risk · Poisson arrival intensity ·
exponential fill intensity λ(δ)=Ae^(−kδ) · realized volatility estimation · PnL
attribution / decomposition · stationary block bootstrap · confidence intervals ·
Sharpe ratio · pre-registration

**Systems / C++** — C++20 · low-latency · event-driven simulation · cache-aware
data structures · branch-free bit manipulation · integer / fixed-point
arithmetic · deterministic replay · CMake · GoogleTest · Google Benchmark ·
AddressSanitizer / UBSan · property-based and fuzz testing · CI/CD · Git ·
profiling · algorithmic complexity analysis

**Data** — Python · asyncio · WebSockets · pandas · NumPy · Matplotlib · binary
serialization · time-series · gap detection and resynchronization

**Do not claim** — lock-free, multithreading, SIMD, FPGA, kdb+/q. None of them
are in this project.

---

## Re-weighting by role

| Role | Lead with |
|---|---|
| Quant researcher | bullets 1, 2, 5 — method, math, statistics |
| Quant developer / algo dev | bullets 3, 4 — the O(n²) and the exact identity |
| Quant trader | bullet 1 and the fee-viability frontier |

---

## The line to keep

The template's *"documented what didn't work and why"* is the most credible
sentence on the page — keep it.

At retail fee tiers, capturing a one-tick spread on BTC earns ~0.001 bp of
notional against a 2–10 bp maker fee: three orders of magnitude under water.
Passive market making on tight majors is structurally unprofitable, and the data
will very likely show exactly that. Report it as the finding, with the fee
frontier showing where it stops being true.

An honest negative result with a rigorous method beats a suspicious Sharpe in
every interview room that matters, and everyone experienced in that room knows
it.
