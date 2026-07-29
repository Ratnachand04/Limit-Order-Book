# CLAUDE.md — Limit Order Book Simulator & Market-Making Study

You are helping me (a final-year CS student building my lead quant project and a research paper) implement a queue-position-aware limit order book simulator and market-making backtester in C++. The full design document is `docs/LOB_MarketMaking_Master_Plan.md` — read it before writing any code, and treat its Part 3 (mathematics), Part 4 (architecture), Part 5 (worked trace), and Appendix A (config) as the specification.

## How to work with me — non-negotiable rules

1. **Phase gates.** Work strictly phase by phase (below). At the end of each phase: all tests green, then STOP and produce (a) a summary of what was built and why, and (b) `docs/SOCRATIC_<phase>.md` with 5 questions I must be able to answer about this phase's code before we continue. Do not start the next phase until I say so.
2. **I write two modules myself.** The queue-position tracker (Phase 4) and the Avellaneda–Stoikov quote computation (Phase 6): for these, you write the tests and interfaces FIRST, then I implement, then you review my code and suggest fixes. Do not implement these two for me unless I explicitly ask after attempting them.
3. **Tests before implementation** for every module. A module without tests does not exist.
4. **Never fabricate results.** All analytics numbers come from real runs on real recorded data. If data is missing, say so; never generate placeholder "example results" into any results file or README.
5. **No live trading.** No exchange API keys, no order-sending code, no withdrawal/account endpoints — ever. Public market-data websockets and REST snapshots only. This is a research simulator.
6. **Explain as you go.** Every non-obvious design decision gets a short comment citing the master plan section (e.g., `// §3.7 pessimistic cancel rule`).

## Tech constraints

- C++20, CMake ≥ 3.22, single-threaded simulation core, deterministic (one seeded `std::mt19937_64`; identical data+seed ⇒ byte-identical outputs).
- GoogleTest (unit + golden tests), Google Benchmark, clang-format (Google style, enforced), `-Wall -Wextra -Werror`; debug CI runs `-fsanitize=address,undefined`.
- Integer arithmetic for prices/quantities: `price_ticks = llround(price / tick_size)`, `qty_lots = llround(qty / lot_size)`. Floating point never used as a container key or for cash accounting (cash in integer 1e-8 units).
- Recorder and analytics in Python 3.11 (`asyncio`+`websockets`; `pandas`+`matplotlib` notebooks). Everything else C++.
- GitHub Actions CI: build, tests, sanitizers, determinism check, clang-format check.

## Repository layout

```
recorder/        # Python: websocket recorder + watchdog + rotation
converter/       # C++: JSONL -> binary Event files (schema below)
bookcore/        # C++: order book (map reference path + dense fast path)
sim/             # C++: event loop, virtual clock, latency model, order gateway,
                 #      order state machine, queue tracker
strategy/        # C++: Strategy interface; S0_touch, S0_queue, S1, S2 (AS + GLFT), S3
analytics/       # C++ writers -> CSV; Python notebooks -> figures/tables
configs/         # YAML experiment configs (Appendix A of master plan)
tests/           # unit, golden, determinism, property tests + fixtures
bench/           # Google Benchmark targets
docs/            # master plan, EXPERIMENTS.md (pre-registration), SOCRATIC_*.md
data/            # gitignored; raw/, binary/, results/
```

## Binary event schema (fixed 32-byte little-endian records)

```cpp
struct Event {
  int64_t exch_ts_us; int64_t seq;
  int32_t price_ticks; int32_t qty_lots;   // DEPTH: new ABSOLUTE qty at level; 0 = delete
  uint8_t type;   // DEPTH=0, TRADE=1, SNAPSHOT_BEGIN=2, SNAPSHOT_LEVEL=3, SNAPSHOT_END=4
  uint8_t side;   // BID=0, ASK=1; for TRADE this is the AGGRESSOR side
  uint8_t symbol_id; uint8_t flags;        // bit0: after-gap/dirty
  int64_t _pad;
};
```

## Phases and acceptance gates

**Phase 0 — Recorder (Python; FIRST, it must run tonight).** Async recorder for configured symbols: diff-depth, aggTrade, bookTicker streams; REST depth snapshot every 30 min; hourly-rotated gzip JSONL with received-timestamp; auto-reconnect with sequence-gap logging to a gaps file; systemd/cron watchdog script; disk-usage report script. *Gate:* 24 h unattended run, gap report generated, documented restart procedure.

**Phase 1 — Converter.** Stream JSONL → binary Events; validates sequence continuity (per Binance spot/futures rules — check the current official docs and encode the sync rule from master plan §2.4); marks dirty flags after gaps; throughput benchmark target ≥ 1M msg/s. *Gate:* round-trip test (synthetic JSONL → binary → decoded equals expected), benchmark recorded in `bench/RESULTS.md`.

**Phase 2 — Book core.** Reference path: `std::map` per side; API `apply_depth / apply_snapshot / best_bid / best_ask / mid_ticks / qty_at / depth_within`. Fast path: dense array over ±32k ticks around an anchor with sliding best cursors, map fallback outside window, dual-run mode asserting both paths agree. Integrity checks: never crossed, never negative, snapshot resync on dirty flag. *Gate:* unit tests from hand-built scenarios; fuzz/property test (random events, invariants hold); golden replay of a committed 10-minute fixture; `apply_depth` benchmark p50 < 100 ns, p99 < 1 µs recorded.

**Phase 3 — Event loop, latency, orders.** Timestamped priority queue; virtual clock; market events visible at `exch_ts + δ_in`; strategy actions effective at `decision_ts + δ_out` with `δ = const + Exp(jitter)` from the seeded RNG; tie-break: market events before our actions at equal time. Order state machine: PENDING_NEW → RESTING → PARTIALLY_FILLED → FILLED | PENDING_CANCEL → CANCELED. *Gate:* unit tests for ordering/latency edge cases; determinism test (two full runs byte-identical) wired into CI.

**Phase 4 — Queue tracker (I implement; you write tests + interface first).** State (A, B, remaining) per master plan §3.7; placement joins back (A = visible level qty, B = 0); adds go behind; trades consume front with fill = clamp(v − A, 0, remaining); unexplained level decreases are cancels handled per configured assumption PESS (behind first) / OPT (ahead first) / PROP (proportional); fills also on cross/trade-through. Your tests must include the full worked trace from master plan Part 5, expected values computed independently, all three assumptions. *Gate:* my implementation passes your tests; your review notes addressed.

**Phase 5 — Analytics core.** Ledger: cash, inventory, equity = cash + q·mid; per-fill records; markout sampler at h ∈ {0.1,0.5,1,2,5,10,30,60} s; PnL decomposition (spread capture + inventory + fees) with a per-day assertion that components sum to Δequity within 1e-9 of notional; CSV writers for `fills.csv`, `markouts.csv`, `pnl_daily.csv`, `probes.csv` (schemas in master plan §4.8); probe-order engine for fill-probability estimation (§3.8). *Gate:* decomposition identity test on synthetic fill sequences; probe monotonicity sanity on a real day (deeper ⇒ lower fill probability).

**Phase 6 — Strategies.** `Strategy` interface (on_book/on_trade/on_fill/on_timer + gateway). S0_touch (naive touch-rule fill logic — deliberately, for the RQ1 comparison), S0_queue, S1 fixed-spread with |q| cap and requote-min-move filter, S2 Avellaneda–Stoikov (r = s − qγσ²(T−t); total spread = γσ²(T−t) + (2/γ)ln(1+γ/k); GLFT asymptotic mode per §3.5; tick-floor rule §3.4; rolling σ per §3.2; k,A loaded from calibration JSON) — **I implement the A–S math**, you test it (including sign/monotonicity in q). S3 = S2 with weighted-mid fair value + toxicity pull (§3.6, Part 6). All parameters from YAML config; a run = (config, data range) → CSVs. *Gate:* every strategy runs a full recorded day end-to-end; S2 monotonicity tests pass; config sweep runner works.

**Phase 7 — Experiments & paper artifacts.** Runner executing the full matrix from master plan Part 7 (queue × strategy × fee × latency × symbol) over the frozen evaluation window; notebooks generating figures F1–F6 and all tables with block-bootstrap CIs (§3.10) strictly from CSVs; README results section template (with a visible "assumptions & limitations" block); `docs/EXPERIMENTS.md` pre-registration checked in BEFORE evaluation runs. *Gate:* one command reproduces every figure from committed CSVs; README numbers traceable to run IDs.

## Definition of done (whole project)
CI green including determinism and sanitizers; benchmarks recorded with hardware noted; golden fixtures committed; all figures reproducible by script; no unexplained magic numbers (constants named and cited to master-plan sections); zero fabricated data anywhere.
