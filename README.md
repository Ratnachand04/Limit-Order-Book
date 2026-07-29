# Queue-Aware Limit Order Book Simulator & Market-Making Study

A deterministic, queue-position-aware limit order book simulator and
market-making backtester in C++20, built to answer one question honestly:

> **How wrong is a naive backtest of a passive strategy?**

Nearly every retail and student backtest of a limit-order strategy uses the
*touch rule*: if the market price touches your limit price, assume a fill. This
is fiction. On a real exchange with price-time priority, when price touches your
level, everyone who arrived there before you fills first. If 40 BTC sits ahead
of your 0.1 BTC bid and only 5 BTC trades before the market moves away, you
filled nothing — while the touch rule credited you a fill at a great price.
Worse, the fills you *do* get are biased toward the moments the market is about
to move against you.

This repository builds the honest simulator, and measures the gap.

---

## Status

| Phase | What | State |
|---|---|---|
| 0 | Python recorder, watchdog, disk report | **code complete, not yet run** |
| 1 | Converter: JSONL → binary events, Binance sync protocol | **code complete** |
| 2 | Book core: `std::map` reference + dense fast path + dual-run | **code complete** |
| 3 | Event loop, virtual clock, latency, order state machine | **code complete** |
| 4 | Queue tracker: A/B/remaining, PESS/OPT/PROP | **code complete** |
| 5 | Ledger, exact PnL decomposition, markouts, probes | **code complete** |
| 6 | Strategy ladder S0–S3, A–S and GLFT | **code complete** |
| 7 | Experiment matrix runner, figure generation | **code complete** |

**No results exist yet.** No data has been recorded, so no run has been
performed, so there are no numbers in this README, in `bench/RESULTS.md`, or
anywhere else. That is deliberate: this project's governing rule is that every
number it produces is measured, and a README with plausible-looking placeholder
figures is exactly how a fake number ends up in an interview.

The results section below is a template. It gets filled in from committed CSVs,
by script, once the recorder has run.

---

## Research questions

| # | Question | Output |
|---|---|---|
| RQ1 | How large is the gap in fill rate and PnL between a naive touch-rule backtest and a queue-position-aware simulation, for the same strategy on the same data? | headline table, F4 |
| RQ2 | What is the empirical probability a passive order fills within horizon *h*, as a function of depth, queue fraction, imbalance and volatility? | fill-probability surfaces, F2 |
| RQ3 | How much maker PnL is lost to adverse selection, and over what horizon after the fill? | markout curves, F1 |
| RQ4 | At what maker-fee level does passive market making become viable, per instrument and strategy? | fee-viability frontier, F3 |
| RQ5 | Does an imbalance-weighted fair value measurably improve per-fill markouts? | S3 vs S2 markout delta |

---

## How it works

```
[Binance WS]  →  recorder (Python)  →  raw JSONL.gz, hourly
                                              ↓
                             converter (C++)  →  binary Event files (32 B records)
                                              ↓
   ┌──────────────────── replayer / event loop (C++) ────────────────────┐
   │  book core  ←  events  →  latency model  →  strategy (S0..S3)       │
   │      ↓                        ↓                    ↓                 │
   │  integrity checks      queue tracker        →   fills               │
   └──────────────┬────────────────────────────────────┬─────────────────┘
                  ↓                                    ↓
          benchmarks (msg/s, ns)              analytics CSVs
                                                       ↓
                                        Python  →  figures & tables
```

Three ideas carry the whole design:

**Queue position is the game.** A resting order is a *shadow*: it sits at a
level whose visible L2 quantity `Q = A + B` excludes it, `A` ahead and `B`
behind. Placement joins the back (`A ← Q, B ← 0`). Arrivals go behind. Trades
consume the front, and fill you with `clamp(v − A, 0, remaining)`.

**Cancels are unobservable, so they are bracketed.** L2 data shows that a level
shrank, not *which* orders left. Three assumptions are run and all three
reported — pessimistic (cancels from behind first, a lower bound on fills),
optimistic (from ahead first, an upper bound), and proportional (uniform, the
central estimate). Headline numbers always use pessimistic, because a
conservative bound is defensible forever.

**Latency is not a detail.** Market events become visible at `exch_ts + δ_in`;
our actions reach the exchange at `decision_ts + δ_out`. A cancel decided at *t*
is not a cancel at *t*, and during that window the order still fills — which is
precisely where a naive backtest dodges the fills that hurt.

---

## Build

Prerequisites: a C++20 compiler, CMake ≥ 3.22, Python 3.11+.
See [docs/BUILDING.md](docs/BUILDING.md) — including one-command Windows
toolchain setup via `tools/install_cpp_toolchain.cmd`.

```bash
cmake --preset msvc-debug        # or gcc-debug-sanitize on Linux
cmake --build --preset msvc-debug
ctest --preset msvc-debug --output-on-failure
```

Verify the toolchain first (it compiles and *runs* a probe program, because a
Visual Studio install can carry `cl.exe` while missing the Windows SDK):

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_toolchain.ps1
```

---

## Use

```bash
# 0. Record.  Do this first; every un-recorded day is unrecoverable.
python recorder/recorder.py --config recorder/symbols.yaml

# 1. Convert one hour of raw JSONL to binary events.
build/bin/lob_convert --symbol BTCUSDT --tick 0.01 --lot 0.001 \
    --in  data/raw/BTCUSDT/BTCUSDT_2026-09-01T00.jsonl \
    --out data/binary/BTCUSDT_2026-09-01T00.lobbin \
    --gaps data/binary/BTCUSDT_2026-09-01T00.gaps.csv
# gzipped input goes through a pipe, so the build stays dependency-free:
#   python -m gzip -d < raw.jsonl.gz | lob_convert --in - ...

# 2. Calibrate lambda(delta) = A exp(-k delta) on the calibration window.
build/bin/lob_calibrate --input data/binary/BTCUSDT_2026-09-0*.lobbin \
    --symbol BTCUSDT --tick 0.01 --lot 0.001 \
    --out data/calib/kA_BTCUSDT.json --points data/calib/lambda_fit.csv

# 3. Run one configuration.
build/bin/lob_replay --config configs/s2_as.yaml \
    --input data/binary/BTCUSDT_2026-09-01.lobbin \
    --output data/results/s2_as

# 4. Run the whole Part 7 matrix (queue x strategy x fee x latency).
build/bin/lob_sweep --config configs/base.yaml --matrix configs/matrix.yaml \
    --input data/binary/BTCUSDT_2026-09-01.lobbin \
    --output data/results/matrix

# 5. Every figure and table, from the CSVs alone.
python analysis/make_figures.py --results data/results/matrix --out data/figures
```

---

## Results

*(Template. Populated from committed CSVs by `analysis/make_figures.py` once
real runs exist. Every cell must be traceable to a run ID in
`data/results/matrix/index.csv`.)*

### RQ1 — the naive-backtest gap

| Metric | Touch rule | Queue-aware (PESS) | Ratio |
|---|---|---|---|
| Fills per quote-hour | — | — | — |
| Mean edge at fill (bp) | — | — | — |
| Daily PnL (mean, 95% CI) | — | — | — |

### RQ3 — markout curve (PESS, 2 bp maker)

| Horizon | 0.1 s | 1 s | 10 s | 60 s |
|---|---|---|---|---|
| Mean markout (bp) | — | — | — | — |
| of which adverse selection (bp) | — | — | — | — |

### RQ4 — break-even maker fee

| Strategy | BTCUSDT | ETHUSDT | mid-cap 1 | mid-cap 2 |
|---|---|---|---|---|
| S1 | — | — | — | — |
| S2_AS | — | — | — | — |
| S3 | — | — | — | — |

### Assumptions & limitations

Read this before any number above.

- **Shadow order.** Our order never depletes real liquidity and no competitor
  reacts to it. Results are conditional statements about recorded history, not
  forecasts of live PnL.
- **L2, not L3.** Binance does not publish order-by-order data, so queue
  position is *modelled*, not observed. The PESS/OPT/PROP bracket is the honest
  response; validating it against true L3 data is stated future work.
- **Trade-to-level attribution.** Aggregated trades and timing windows
  misclassify some cancels as trades and vice versa. The residual rate is
  measured and reported in `queue_stats.csv`, not assumed away.
- **Latency.** 5 ms results are decorative for a retail setup; 50–200 ms is the
  honest band and is what the headline numbers use.
- **Fees dominate.** At retail maker tiers on tight majors, spread capture is
  three orders of magnitude smaller than the fee. Results at 0 bp mean nothing;
  the frontier is the result.
- **Sample size.** Any Sharpe is computed from *daily* PnL, annualised with
  √365, reported with a stationary block-bootstrap interval, and bounded by how
  few days the sample has.
- **No live trading.** Public market-data websockets and REST depth only. No API
  keys, no signed requests, no order-sending code — ever.

---

## Repository layout

| Directory | Contents |
|---|---|
| `recorder/` | Python: websocket recorder, watchdog, disk report, symbol scout |
| `converter/` | C++: Binance decoding, book-sync protocol, binary event I/O |
| `bookcore/` | C++: `MapBook` (reference), `DenseBook` (fast), `DualBook` (both) |
| `sim/` | C++: event loop, clock, latency, gateway, queue tracker, probes |
| `strategy/` | C++: `Strategy` interface, S0–S3, A–S and GLFT closed forms |
| `analytics/` | C++: ledger, PnL decomposition, markouts, CSV writers |
| `apps/` | C++: `lob_replay`, `lob_sweep`, `lob_calibrate` |
| `analysis/` | Python: block bootstrap, figure and table generation |
| `configs/` | YAML experiment configurations and the sweep matrix |
| `tests/` | GoogleTest: unit, property/fuzz, worked-trace, determinism, golden |
| `bench/` | Google Benchmark targets and `RESULTS.md` |
| `docs/` | Master plan, architecture, experiment pre-registration, study guides |
| `tools/` | Toolchain setup and verification |
| `data/` | gitignored: `raw/`, `binary/`, `results/`, `figures/` |

---

## Reading

- **[docs/LOB_MarketMaking_Master_Plan.md](docs/LOB_MarketMaking_Master_Plan.md)** —
  the full design document. Everything here implements it.
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — how the pieces fit, and why.
- **[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md)** — pre-registration. Committed
  *before* the evaluation runs.
- **[docs/BUILDING.md](docs/BUILDING.md)** — toolchain setup.
- `docs/SOCRATIC_phase*.md` — questions to answer about each phase's code.

Core references: Avellaneda & Stoikov (2008); Guéant, Lehalle &
Fernandez-Tapia (2013); Stoikov (2017) micro-price; Cont, Stoikov & Talreja
(2010); Huang, Lehalle & Rosenbaum (2015) queue-reactive model; Moallemi & Yuan,
*The value of queue position*; Gould et al. (2013); Cartea, Jaimungal & Penalva
(2015); Harris (2003).

## License

MIT — see [LICENSE](LICENSE).
