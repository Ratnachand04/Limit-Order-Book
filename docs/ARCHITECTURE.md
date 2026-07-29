# Architecture

Implements master plan Part 4. This document records the decisions that are not
obvious from the code, and the reasons behind them.

## Dependency graph

```
common  ←  bookcore
        ←  analytics
        ←  converter
              ↑
             sim  ←  strategy  ←  apps
```

Strictly acyclic, and it costs one deliberate placement to keep it that way:
`Order`, `Fill`, `QueueState`, `FillCause` and `QueueTrackerStats` live in
`common/include/lob/execution.hpp` rather than in `sim/`. The analytics layer
must be able to describe a fill without depending on the engine that produced
one, and the engine must be able to book a fill into the ledger. Putting the
plain data in the middle makes that a straight line instead of a cycle.

| Module | Owns |
|---|---|
| `common` | integer types, the 32-byte `Event`, `Instrument`, JSON pull-parser, YAML subset, `RunConfig`, `CsvWriter`, `Rng` |
| `bookcore` | `MapBook`, `DenseBook`, `DualBook` |
| `converter` | Binance decoding, the §2.4 sync protocol, binary event I/O |
| `analytics` | `Ledger`, `MarkoutSampler`, `RunRecorders` |
| `sim` | `Clock`, `EventQueue`, `LatencyModel`, `OrderGateway`, `QueueTracker`, `ProbeEngine`, `Simulator`, the `Strategy` interface |
| `strategy` | S0–S3, A–S / GLFT closed forms, σ and flow estimators, the factory |

---

## The integer discipline

Past the parser, nothing is a floating-point number that could be a container
key or a cash amount.

```
price_ticks = llround(price / tick_size)
qty_lots    = llround(qty   / lot_size)
cash        = integer count of 1e-8 units
```

`Instrument` is the only place the conversion happens. It refuses to construct
if `tick_size * lot_size * 1e8` is not a whole number, because that product is
the exact cash value of one tick-lot:

```
notional = price_ticks * qty_lots * cash_per_tick_lot     (all integers)
```

Fees are held in **tenths of a basis point**, so the whole master-plan grid
`{-0.5, 0, 1, 2, 5, 10}` bp is exactly representable.

The mid can be a half tick, so every quantity in the PnL decomposition is
carried at **twice** its natural value ("x2 units", counts of 0.5e-8). Doubling
removes the only remaining source of fractions, and turns the §3.9 identity from
an approximate check into an exact one:

```
ΔE − (spread capture + inventory PnL − fees) == 0     exactly, always
```

A tolerance would hide small bugs. Zero does not.

---

## The binary event schema

Fixed 32-byte little-endian records.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `exch_ts_us` | exchange timestamp |
| 8 | 8 | `seq` | update id, for continuity checks |
| 16 | 4 | `price_ticks` | |
| 20 | 4 | `qty_lots` | DEPTH: the new **absolute** qty; 0 deletes |
| 24 | 1 | `type` | DEPTH=0, TRADE=1, SNAPSHOT_BEGIN=2, SNAPSHOT_LEVEL=3, SNAPSHOT_END=4 |
| 25 | 1 | `side` | BID=0, ASK=1; for TRADE, the **aggressor** |
| 26 | 1 | `symbol_id` | |
| 27 | 1 | `flags` | bit0 dirty, bit1 resync |
| 28 | 4 | `reserved` | zero-filled |

**Deviation from the master plan, recorded deliberately.** §4.3 lists the
trailing field as `int64_t _pad`, which would make the struct 36 bytes (40
padded). Both CLAUDE.md and §4.3 also state a *fixed 32-byte record*. The stated
size governs, so the reserved field is `int32_t` and the record is exactly 32
bytes with natural alignment. `static_assert`s in `common/include/lob/types.hpp`
pin every offset.

`side` for a TRADE is the **aggressor**: `kAsk` means a sell-aggressor, which
consumes the **bid** queue. This inversion — Binance's `m` flag names the
*maker*, the schema stores the *aggressor* — is the single most common place to
introduce a sign error, and it decides which resting orders a trade can fill.

---

## Book core

Two implementations that must agree exactly.

**`MapBook`** — one `std::map` per side, bids ordered descending. Node-based and
allocating, therefore slow. Its job is to be obviously correct.

**`DenseBook`** — the hot path:

- a dense `std::vector<Lots64>` over ±32768 ticks around an anchor;
- a **1-bit-per-tick occupancy bitmap** over the same range, so "find the next
  non-empty level below this one" is a word scan with `countl_zero` rather than
  a walk over 65k `int64`s;
- a `std::map` fallback for levels outside the window;
- cached best-price cursors that slide on update.

At a $0.01 tick, ±32768 ticks is only ±$327, so on BTC the window re-anchors
several times a day. That is a normal operation, not an error path, and it is
benchmarked as one.

**`DualBook`** runs both and compares. Reads are answered by the *reference*, so
if the fast path has drifted the simulation still behaves correctly and the
mismatch is reported rather than silently propagated into results.

A snapshot **rebuilds** a side rather than patching it. A book patched across a
gap stays subtly wrong forever, and the corruption surfaces as crossed levels
much later, far from the cause.

---

## Event loop and latency

One priority queue of timestamped events, three kinds:

| Kind | When it fires |
|---|---|
| `kMarket` | `exch_ts + δ_in` — when *we* see it |
| `kAction` | `decision_ts + δ_out` — when the *exchange* sees ours |
| `kTimer` | the strategy's periodic wake-up |

`δ = constant + Exp(jitter)`, drawn from the single seeded `std::mt19937_64`.

**Tie-break rule, documented because it is a modelling choice:** at equal
timestamps, market events are processed **before** our actions. The market moves
first, so a cancel racing an adverse trade loses the race — the conservative
direction to be wrong in. Ties within a kind break by insertion order, which
makes the ordering total and therefore reproducible.

**Monotonic visibility.** Independent jitter draws could make event *i+1* visible
before event *i*, which would let the book apply diffs out of sequence. A
market-data feed is a serial stream and cannot reorder, so visible timestamps
are clamped to be monotone.

**The clock separates local from exchange time.** A strategy can reach only local
time. There is no path from a `Strategy` to the raw event stream or to an
exchange timestamp, which stops lookahead structurally rather than by discipline.

---

## Queue tracker

State per resting order: `(A, B, remaining)` — quantity ahead, behind, and our
own unfilled size. Rules, in application order:

| Event | Effect |
|---|---|
| Placement | `A ← Q` (the visible quantity), `B ← 0` |
| Level increase `+ΔN` | `B ← B + ΔN` — the one unambiguous rule |
| Trade of size `v` | `fill = clamp(v − A, 0, remaining)`; `A ← max(0, A − v)`; overflow past us eats `B` |
| Unexplained decrease `ΔC` | a cancellation — PESS / OPT / PROP |
| Cross or trade-through | the remainder fills at our limit |

**Trade-vs-cancel attribution.** The tracker banks traded quantity as *credit*
against the level. When the level decreases, the credit explains as much as it
can and only the remainder is a cancellation. Credit that never finds a matching
decrease inside the attribution window expires and is **counted** — that residual
is a measurable property of the data and it is reported in `queue_stats.csv`,
not assumed away.

**Proportional splitting stays integral.** `A * (1 − ΔC/(A+B))` is fractional.
Rather than make queue state floating point (which would cost determinism across
standard libraries), the split is computed as `ΔC * A / total` in integers with
the remainder decided by one RNG draw: unbiased in expectation, exactly
reproducible, and — unlike a floating-point share — it gets an exactly-divisible
case such as `15 × 30/45` right instead of leaving it to a coin flip.

**Shadow orders do not queue behind each other.** Two of our orders at one level
each see the full trade quantity. That is the stated shadow-order limitation, not
an oversight.

---

## Analytics

`Ledger` marks to the mid on every change, books each fill, and exposes
`IdentityResidualX2()`, which must be exactly zero. `lob_replay` and `lob_sweep`
both **fail the run** if it is not — an identity that is exact by construction
makes any non-zero residual a defect, not accumulated error.

`MarkoutSampler` schedules one sample per fill per horizon and resolves each at
the mid **in force at that horizon**. Samples that the run ends before reaching
are reported as *unresolved* and excluded from means. They are never back-filled
with the last known mid: doing so would silently bias long horizons toward zero
adverse selection.

`ProbeEngine` spawns shadow orders on a fixed schedule at several depths on both
sides and tracks each to fill-or-expiry. Probes never touch the ledger. Their
monotonicities (deeper → less likely; more ahead → less likely) are the
simulator's own validation.

---

## Determinism

The contract: same data + same config + same seed ⇒ byte-identical outputs.

What it costs, and what it buys:

- **One** `std::mt19937_64`, passed by reference. Never a second engine.
- Distributions are written out by hand — `std::exponential_distribution` is not
  portable across standard libraries, so using it would break cross-platform
  reproducibility.
- No `std::unordered_*` anywhere its iteration order could reach an output.
- No wall-clock time in the simulation core.
- CSV doubles go through `std::to_chars` with fixed precision, not `operator<<`,
  which honours the stream locale. Negative zero is normalised.
- Files are opened in binary mode so line endings do not differ by platform.

`tests/test_determinism.cpp` asserts byte equality of every CSV, *and* that both
runs made the same number of RNG draws — which is what proves no code path
branched on anything non-deterministic. CI additionally runs the binary twice as
two separate processes and diffs the outputs.
