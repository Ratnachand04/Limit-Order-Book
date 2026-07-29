# Limit Order Book Simulator & Market-Making Study
### The complete master plan: idea → finance → mathematics → architecture → experiments → paper → schedule

**Who this is for:** you, building this Aug–Oct 2026 as your lead quant project and the basis of a research paper.
**How to use it:** read Parts 0–2 this week while your data recorder runs. Study Part 3 alongside the Avellaneda–Stoikov paper. Build against Parts 4–6 using the companion `CLAUDE.md`. Run Part 7's experiments in late September. Write the paper from Part 8 in October.

**One rule above all others:** every number this project produces goes on your CV and into your paper *exactly as measured*. If the strategy loses money net of fees — and at retail fee tiers it probably will — that finding, properly measured and explained, IS the paper. An honest negative result with a rigorous method beats a fake Sharpe every single time you are interviewed.

---

## Part 0 — The one-paragraph version

You will record live tick data (order-book changes and trades) from a crypto exchange for six-plus weeks, reconstruct the full limit order book from it in C++, and build an event-driven simulator that replays history while tracking a *virtual* passive order's position in the queue at its price level — because whether a resting order fills depends almost entirely on how much volume sits ahead of it. On top of the simulator you run a ladder of market-making strategies, from a naive "join the best bid" bot up to the Avellaneda–Stoikov optimal quoting model with inventory skew. You measure fill rates, adverse selection via markout curves, and PnL decomposed into spread capture versus inventory risk, net of fees — under three different queue assumptions and a grid of fee and latency scenarios. The headline research result is the *gap* between what a naive backtest claims and what a queue-aware simulation shows, which is a genuine, publishable, empirical contribution that almost no student project delivers.

Why interviewers care: this project demonstrates, in one artifact, C++ systems engineering, market microstructure understanding, stochastic-control-based strategy design, and — rarest of all — scientific honesty about backtesting. Those are the four things a quant desk actually screens for.

---

## Part 1 — Idea proposal: the research pitch

**The problem.** Nearly every retail and student backtest of a passive (limit-order) strategy uses the "touch rule": if the market price touches your limit price, you assume a fill. This is fiction. On a real exchange with price-time priority, when price touches your level, everyone who arrived at that price *before you* fills first. If 40 BTC of orders sit ahead of your 0.1 BTC bid and only 5 BTC trades at that price before the market moves away, you filled nothing — while the touch rule credited you a fill at a great price. Worse, the fills you *do* get are biased toward the moments the market is about to move against you (adverse selection). The result: naive backtests of passive strategies systematically overstate fill rates and PnL, sometimes by an order of magnitude. Professionals know this; almost nobody outside the industry quantifies it.

**The proposal.** Build the honest simulator, calibrate it on real data, and quantify exactly how wrong the naive approach is — then use the honest simulator to evaluate a classical optimal market-making model under realistic fees and latency.

**Research questions.**

| # | Question | Output |
|---|---|---|
| RQ1 | How large is the gap in fill rate and PnL between a naive touch-rule backtest and a queue-position-aware simulation, for the same strategy on the same data? | The headline table and figure |
| RQ2 | What is the empirical probability a passive order fills within horizon *h*, as a function of its depth level, queue fraction, book imbalance, and recent volatility? | Fill-probability surfaces |
| RQ3 | How much maker PnL is lost to adverse selection, and over what horizon after the fill does it materialize? | Markout curves |
| RQ4 | At what maker-fee level does passive market making on each instrument become viable, per strategy? | The fee-viability frontier |
| RQ5 | Does skewing quotes with an imbalance-weighted fair value (microprice) measurably improve per-fill markouts? | An alpha-value estimate |

**Contributions you can honestly claim.** An open-source, deterministic, queue-aware C++ LOB simulator; a reproducible calibration and evaluation pipeline on public crypto data; empirical answers to RQ1–RQ5 with confidence intervals; and a pre-registered experiment design (you commit the experiment plan to the repo *before* running the out-of-sample evaluation — a scientific practice that will make a reviewer, or an interviewer, sit up).

**Scope discipline (what this project is not).** No live trading, no exchange API keys, no order routing. No claim of modeling your own market impact (your virtual order is a "shadow" that doesn't change history — state this limitation prominently). One venue class (crypto), which you justify by data accessibility, and whose lessons you frame carefully.

---

## Part 2 — Model finance: the microstructure you must own cold

### 2.1 The limit order book

An electronic exchange for an instrument maintains two sorted queues of resting limit orders: bids (buy orders, sorted by price descending) and asks (sell orders, ascending). The highest bid and lowest ask are the *best bid and offer* (BBO); their difference is the *spread*; their average is the *mid price*. Prices live on a grid of *ticks* (BTCUSDT tick = $0.01). Within one price level, orders fill in strict arrival order: *price-time priority*. A *market* (or aggressive limit) order consumes resting liquidity from the front of the opposite queue — the sender is the *taker*, the resting orders are *makers*. Data comes in three fidelities: L1 (BBO only), L2 (aggregate quantity at each price level — what Binance gives you), L3 (every individual order — what NASDAQ's LOBSTER or Coinbase's full feed give). With L2 you can see *that* the queue at a price shrank by 3 BTC, but not *which* orders left — this single fact drives your entire queue-modeling problem.

### 2.2 The economics of a market maker

A market maker continuously posts a bid below and an ask above fair value, hoping to buy at the bid and sell at the ask, pocketing the spread. Its profit-and-loss over any period obeys an identity you should be able to write from memory:

```
Maker PnL  =  Spread capture  −  Adverse selection  +  Inventory PnL  −  Fees
```

*Spread capture* is the edge at the moment of each fill: you bought 0.1 BTC at 99,990 when mid was 100,000 → you captured $1.00 of instantaneous edge. *Adverse selection* is the tendency of the mid to move *against* you right after you fill — your bid gets hit precisely when sellers are aggressive, so the mid tends to fall after you buy. Measured as the *markout*: the change in mid over the seconds after your fill, signed by your side. *Inventory PnL* is mark-to-market gain or loss on the position you're warehousing between fills — this is where the trending-market risk lives. *Fees* are what the exchange charges per fill (makers sometimes pay less than takers, or even receive a rebate at high volume tiers).

Flow is *benign* when takers are uninformed (their trades don't predict price) and *toxic* when informed (their trades do). A market maker is structurally short an option to informed traders; everything in strategy design — spread width, inventory skew, quote pulling — is about managing that exposure.

### 2.3 Why queue position is the whole game

Consider two identical 0.1 BTC bids at the same price, one at the queue front, one at the back behind 40 BTC. The front order fills on the next modest sell trade — often while the price *stays* at that level, i.e., a benign fill that captures spread. The back order only fills when sellers chew through the entire 40 BTC — which usually happens exactly when the price is about to tick down through the level, i.e., a toxic fill. Front-of-queue fills are frequent and benign; back-of-queue fills are rare and adversely selected. (Moallemi & Yuan formalized the dollar "value of queue position"; your project measures its analogue empirically.) This is why the touch rule fails twice over: it overstates *how often* you fill and misstates *which* fills you get.

### 2.4 Venue reality: crypto specifics you must know

**Data feeds (Binance, your primary venue).** Three WebSocket streams per symbol: the *diff-depth* stream (`<symbol>@depth@100ms`) sending batched L2 level changes with sequence numbers; the *aggTrade* stream sending every trade with price, quantity, and an aggressor-side flag; and *bookTicker* sending BBO updates. Reconstruction protocol: buffer diff messages, fetch a REST depth snapshot with its `lastUpdateId`, discard buffered diffs entirely below that id, verify the first applied diff's sequence range brackets `lastUpdateId+1`, then apply diffs in order forever, checking sequence continuity (futures streams carry a previous-update-id field for exactly this). Any gap → resnapshot and mark the interval dirty. (Verify field names against current Binance docs when you build; the protocol shape is stable.)

**The L2 limitation.** Binance does not publish order-by-order data, so your queue position must be *modeled*, not observed. You will bracket reality with three assumptions (§3.7). Coinbase's exchange feed historically offered a full L3 channel, and LOBSTER provides free academic L3 samples for NASDAQ — either is your "future work: validate the queue model against true L3" paragraph.

**The fee arithmetic that shapes everything.** Do this arithmetic in your head in interviews. BTC at ~$100,000, tick $0.01: quoting best-bid/best-ask and capturing the full 1-tick spread earns $0.01 per BTC ≈ **0.001 bp** of notional. A typical retail *maker* fee on spot is ~10 bp; on futures ~2 bp at base tier. Fees exceed 1-tick spread capture by a factor of a thousand. Conclusion: passive MM on ultra-tight majors at retail fees is structurally unprofitable on spread capture alone — real firms survive via fee rebates at volume tiers, short-horizon alpha, or wider-spread instruments. Two design consequences: (1) your recording list must include at least two instruments whose typical spread is ≥ 3–5 ticks / ≥ 2 bp (mid-cap perpetuals — pick by observing spreads for a day), where the economics are non-degenerate; (2) the fee-viability frontier (RQ4) is a core experiment: rerun everything at maker fees ∈ {−0.5, 0, 1, 2, 5, 10} bp and find each strategy's break-even. That chart is one of your paper's best figures and one of your best interview stories.

**Historical backfill.** You cannot record the past, which is why the recorder starts *this week*. Binance's public data dumps can backfill trades and klines; full-depth history generally requires third-party vendors (some offer free samples). Plan around your own recording as the primary dataset.

### 2.5 What you may and may not claim

You may claim: measured fill rates, markouts, and PnL *within a shadow-order simulation calibrated on real data*, with stated queue and latency assumptions. You may not claim: that these are achievable live returns (your order would have changed the book, competed with real MMs, and faced production latency). Write this limitation in the paper's own words before an interviewer makes you say it.

---

## Part 3 — Model mathematics

### 3.1 Notation

| Symbol | Meaning |
|---|---|
| `S_t`, `m_t` | mid price at time t |
| `σ` | mid-price volatility (price units per √second) |
| `q_t` | your inventory (signed, in coins) |
| `X_t` | your cash |
| `δ^b, δ^a` | distances of your bid/ask quotes from mid |
| `λ(δ)` | intensity (arrivals/sec) of market orders that reach a quote at distance δ |
| `A, k` | parameters of `λ(δ) = A·e^{−kδ}` |
| `γ` | risk-aversion (units 1/price — it converts price-variance into a penalty) |
| `T − t` | remaining horizon in the finite-horizon model |
| `A_t, B_t` | quantity ahead of / behind your order in its queue |
| `f` | maker fee, as a fraction of notional |

### 3.2 Mid price and volatility

Use the mid `m = (best_bid + best_ask)/2` (later, the weighted mid of §3.6 as an alternative fair value). Estimate `σ` as the standard deviation of mid changes sampled every Δ = 1 s over a rolling window (e.g., 10 min), reported in $/√s: `σ̂² = (1/nΔ)·Σ (m_{t+Δ} − m_t)²`. Sampling much faster than 1 s contaminates the estimate with microstructure noise (bid-ask bounce); sampling much slower loses reactivity. Say exactly that sentence when asked why Δ = 1 s.

### 3.3 The fill-intensity model and its estimation

Assume a quote resting at distance δ from mid is executed by arriving market orders as a Poisson process with intensity `λ(δ) = A·e^{−kδ}`. `A` is the base arrival rate at the mid; `k` is how fast execution probability decays as you quote deeper — the market's "reach". The exponential form is both empirically reasonable and what makes Avellaneda–Stoikov solvable.

**Estimation (the empirical-hazard method).** Over a calibration window, for each side and each distance δ on a grid (1…D ticks): each second, imagine a fresh infinitesimal quote at distance δ from the current mid; count the second as an "execution" if aggressive flow traded through that price (for a bid at `m−δ`: some sell-aggressor trade printed at ≤ `m−δ`). Then `λ̂(δ) = executions / total seconds`, and fitting `ln λ̂(δ) = ln A − k·δ` by least squares over the linear region gives `Â, k̂`. Report the fit plot in the paper; if the tail bends (it will), fit the near region you actually quote in and say so.

### 3.4 Avellaneda–Stoikov: the optimal quoting model

**Setup (A–S 2008).** Mid follows `dS_t = σ dW_t`. The MM continuously posts a bid at `S−δ^b` and ask at `S+δ^a`; each side is lifted as a Poisson event with intensity `λ(δ)` as above, changing inventory by ∓1 unit and cash by the traded price. The MM maximizes expected exponential (CARA) utility of terminal wealth: `E[−exp(−γ(X_T + q_T·S_T))]`.

**Solution shape (via the HJB equation — derivation sketch).** Write the value function `u(t,x,s,q)`, apply dynamic programming: `u` earns the diffusion term in `s` plus, for each side, the jump improvement rate `λ(δ)·[u(after fill) − u(before)]`, maximized over `δ^b, δ^a`. The ansatz `u = −exp(−γx)·exp(−γθ(t,s,q))` separates cash out; with the exponential intensity, the first-order conditions solve in closed form. You do not need to reproduce the full derivation in interviews — you need the two results and their meaning:

**Result 1 — the reservation price** (the price at which you're indifferent to holding one more unit):
```
r(s, q, t) = s − q · γ · σ² · (T − t)
```
Long inventory (q>0) pushes your personal fair value *below* the market mid: you shade quotes downward to attract buyers of your inventory and repel more buying. The shift per unit of inventory is `γσ²(T−t)` — risk aversion times the variance of the mark-to-market risk you'd carry over the remaining horizon.

**Result 2 — the optimal total spread** (centered on r, not on s):
```
δ^a + δ^b = γ·σ²·(T − t) + (2/γ)·ln(1 + γ/k)
```
Term one: charge for the volatility risk of the inventory a fill creates. Term two: the monopolist-dealer markup — it *shrinks* as `k` grows (an elastic market punishes wide quotes with zero fills) and grows with `γ`. Quotes: `ask = r + spread/2`, `bid = r − spread/2`, then rounded to the tick grid.

**Worked numeric example (memorize the mechanics, not the numbers).** BTC ≈ $100,000. Suppose `σ = 8 $/√s` (≈ 2.4% daily), horizon `T−t = 60 s`, `γ = 10⁻⁵ $⁻¹`, and calibration gives `k = 0.035 $⁻¹` (intensity halves every ~$20 of depth). Then the inventory skew is `γσ²(T−t) = 10⁻⁵·64·60 ≈ $0.038` per unit — tiny per coin. The spread's second term: `γ/k ≈ 2.9·10⁻⁴`, so `(2/γ)ln(1+γ/k) ≈ 2/k ≈ $57` ≈ 5.7 bp. Two lessons you'll repeat in interviews: for small γ, **optimal spread ≈ 2/k — the market's elasticity, not your risk aversion, sets the spread**; and A–S can tell you to quote *far* wider than the touch on a tight instrument, i.e., its literal quotes sit deep in the book.

**Practical adaptation (what real desks do and what your S2/S3 do).** On tick-constrained instruments, use A–S for what it's genuinely good at — the *inventory skew* and the *no-quote/wide-quote decision* — while flooring quote placement at the touch or one tick behind: e.g., quote `bid = min(best_bid, round_to_tick(r − spread/2))`. Cap `q` at hard limits; treat `T−t` as a fixed rolling horizon (60 s) rather than a real terminal time, or use the infinite-horizon variant below.

### 3.5 The steady-state (Guéant–Lehalle–Fernandez-Tapia) form

With inventory hard-capped at ±Q and an infinite horizon, GLFT (2013) derive asymptotic optimal distances that practitioners actually use, because they don't depend on a fake terminal time:
```
δ^b*(q) ≈ (1/γ)·ln(1 + γ/k)  +  (2q+1)/2 · sqrt( (σ²γ)/(2kA) · (1 + γ/k)^{1+k/γ} )
δ^a*(q) ≈ (1/γ)·ln(1 + γ/k)  −  (2q−1)/2 · sqrt( (σ²γ)/(2kA) · (1 + γ/k)^{1+k/γ} )
```
Check the signs make sense: at q=0 both distances equal base + half-skew (symmetric); at q=+1 (long), the bid moves *away* (buy less) and the ask moves *closer* (sell more). Implement this as strategy S2's alternative mode; cite the paper, present the formula as their asymptotic result, and verify numerically that your implementation's q-dependence is monotone as above.

### 3.6 Short-horizon fair value: imbalance and the weighted mid

Order-book imbalance `I = Q^b/(Q^b + Q^a)` (top-level bid and ask quantities) predicts the direction of the next mid move: a heavy bid queue means the ask side is likelier to be consumed first. The *weighted mid* `m_w = I·P^a + (1−I)·P^b` shifts fair value toward the thin side and is the simple ancestor of Stoikov's micro-price (which estimates `E[future mid | imbalance, spread]` via a Markov chain — cite it; implement the weighted mid and, optionally, a regression-adjusted version). **Measure before you use:** regress `m_{t+h} − m_t` on `(m_w − m)_t` for h ∈ {1, 5, 10} s on calibration data and report R² and the correlation (information coefficient). Strategy S3 replaces `s` with `m_w` in the A–S formulas; RQ5 asks whether that measurably improves per-fill markouts out of sample.

### 3.7 Queue-position models — the heart of the simulator

Your virtual order rests at a price level whose *visible* L2 quantity (excluding you — you're a shadow) is `Q = A + B`: `A` ahead of you, `B` behind. On placement, price-time priority means you join the back: `A ← Q`, `B ← 0`. Then per event at your level:

**New quantity arrives (+ΔN):** always behind you → `B ← B + ΔN`. (The one unambiguous rule.)

**A trade of size v executes at your level** (identified by matching the trade stream to the level within a small time window, aggressor side opposite yours): trades consume the queue front:
```
fill_amount = clamp(v − A, 0, your_remaining_qty)
A ← max(0, A − v);   if v > A_prev + your_qty: B ← B − (v − A_prev − your_qty)
```

**The level shrinks by ΔC not explained by trades — a cancellation.** L2 can't tell you *where* in the queue it happened, so run three assumptions and report all three:
```
Pessimistic (cancels behind first):  B ← max(0, B − ΔC); overflow reduces A
Optimistic  (cancels ahead first):   A ← max(0, A − ΔC); overflow reduces B
Proportional (uniform location):     A ← A·(1 − ΔC/(A+B));  B ← B·(1 − ΔC/(A+B))
```
Pessimistic gives lower-bound fills, optimistic upper-bound, proportional your central estimate. **Headline CV/paper numbers always use pessimistic** — you can defend a conservative bound forever.

**Other fill triggers:** if the opposite best crosses your price, or your entire level is traded through and deleted, you fill (at your limit price). If price moves away, you simply keep resting. Partial fills reduce `your_remaining_qty` and continue.

**Latency interacts with all of this:** your cancel or replace decided at t only takes effect at `t + δ_out`; every event you react to you actually saw at `event_time + δ_in`. Sequence events accordingly (§4.5) — this is where naive backtests silently cheat.

### 3.8 Empirical fill probability (RQ2)

Definition: `P(fill within h | placement features)`, features = depth level of placement (ticks from mid), initial queue fraction `A/(A+B+you)`, imbalance `I`, and rolling `σ`. Estimation: during replay, spawn *virtual probe orders* on a schedule (e.g., every 5 s, both sides, several depth levels), track each to fill-or-expiry at horizon h ∈ {1, 5, 30, 60} s under each queue assumption, and log outcomes. Report (a) empirical surfaces (heatmaps of fill probability over depth × queue-fraction), and (b) a logistic regression `P(fill) = sigmoid(β·features)` with out-of-sample calibration plots. This doubles as the *validation* of your simulator: the monotonicities (deeper → less likely, more ahead → less likely, imbalance toward you → more likely) must hold or something is wrong.

### 3.9 Exact PnL accounting and decomposition

Define equity `E_t = X_t + q_t·m_t` (cash plus inventory marked at mid). Over any interval, algebra gives the exact decomposition — no approximation:
```
ΔE = Σ_fills  s_i·(m_{t_i} − p_i)·v_i        [instantaneous spread capture]
   + ∫ q_{t−} dm                              [inventory PnL: position × mid moves]
   − Σ_fills  f·p_i·v_i                       [fees]
```
where `s_i = +1` for your buys, `−1` for sells, `p_i, v_i` the fill price and size. Implement it as a running ledger and *assert* each day that the three components sum to the change in equity to numerical tolerance — that assertion has caught a bug in every backtester ever written.

**Markout (adverse selection made visible).** For each fill, `MO_i(h) = s_i·(m_{t_i+h} − p_i)·v_i` for h from 100 ms to 60 s. Decompose: `MO_i(h) = spread_capture_i + s_i·(m_{t_i+h} − m_{t_i})·v_i`; the second term is the **adverse-selection cost at horizon h** (negative on average for makers). Average per fill, normalize by notional into bp, plot against h: the markout curve — typically starts positive (you got edge) and decays or goes negative as the market drifts against your fills. Report, per strategy and queue assumption: average edge at fill (bp), adverse selection at 10 s (bp), net markout (bp). *These are the CV bracket numbers.* Related standard vocabulary to know: effective spread `2·s·(p−m_t)` and realized spread `2·s·(p−m_{t+h})` are the taker-convention cousins; be able to translate.

### 3.10 Statistics: making the numbers defensible

Aggregate to daily PnL. Report mean daily PnL with a **stationary block bootstrap** confidence interval (resample days in blocks of ~3 to respect serial dependence; 10,000 resamples). If you quote a Sharpe, compute it from *daily* PnL and annualize with √365, and say in the same breath that a 60-day sample bounds how seriously to take it. Split results by regime (Asian/EU/US session; top-vs-bottom volatility terciles). **Pre-register**: before touching the out-of-sample window, commit `EXPERIMENTS.md` — the exact strategy configs, parameter values (frozen from calibration), metrics, and the full experiment matrix — so nobody, including you, can accuse you of tuning on the test set. Calibrate on weeks 1–3 of data; evaluate on weeks 4–6+; never iterate strategy parameters on the evaluation window.

---

## Part 4 — System architecture (model design)

### 4.1 Design principles

Event-sourced: the recorded stream is the single source of truth; every downstream artifact is a pure function of it. Deterministic: single-threaded simulation core, virtual clock driven only by event timestamps, all randomness (latency jitter, proportional-cancel draws) from one seeded RNG; same data + same seed ⇒ byte-identical outputs (and a CI test enforces it). Integer arithmetic for prices and sizes: `price_ticks = llround(price/tick_size)`, `qty_lots = llround(qty/lot_size)` — floating-point keys corrupt books.

### 4.2 Components and data flow

```
[Binance WS] → recorder (Python, asyncio) → raw JSONL.gz, daily files
                                   ↓
                      converter (C++) → binary event files (schema §4.3)
                                   ↓
   ┌──────────────────────── replayer / event loop (C++) ───────────────────────┐
   │  book core  ←  events  →  latency model  →  strategy (S0..S3)              │
   │      ↓                        ↓                    ↓                        │
   │  integrity checks      order/queue tracker  →  fills                       │
   └──────────────┬─────────────────────────────────────┬───────────────────────┘
                  ↓                                     ↓
          benchmarks (msg/s, ns)                analytics logs (CSV)
                                                        ↓
                                        Python notebooks → figures & tables
```

### 4.3 Data layer

**Recorder in Python, deliberately.** Data collection must start *now* and run unattended for weeks; Python asyncio websockets with auto-reconnect, sequence-gap logging, hourly-rotated gzip JSONL, and a cron watchdog is a one-day build with your existing skills. Recording reliability > language purity; the C++ story of the project is the book core and simulator, and you'll say exactly that if asked. Record per symbol: diff-depth, aggTrade, bookTicker, plus a REST depth snapshot every 30 min (resync anchors). Symbols: BTCUSDT + ETHUSDT perpetuals (tight-spread regime) + two mid-caps chosen after one day of spread observation (target typical spread ≥ 3–5 ticks). Disk budget: rough order of 1–3 GB/day/symbol compressed — check after day one.

**Binary event schema (converter output), fixed 32-byte records:**
```cpp
struct Event {              // packed, little-endian
  int64_t  exch_ts_us;      // exchange timestamp
  int64_t  seq;             // update id (continuity checks)
  int32_t  price_ticks;
  int32_t  qty_lots;        // for DEPTH: new absolute qty at level; 0 = delete
  uint8_t  type;            // DEPTH=0, TRADE=1, SNAPSHOT_BEGIN=2, SNAPSHOT_LEVEL=3, SNAPSHOT_END=4
  uint8_t  side;            // BID=0, ASK=1; for TRADE: aggressor side
  uint8_t  symbol_id;
  uint8_t  flags;           // gap markers, dirty-interval markers
  int64_t  _pad;
};
```

### 4.4 Book core

Two containers per side. Cold path and correctness reference: `std::map<int32_t, int64_t>` (bids with `std::greater`). Hot path: a dense array of quantities indexed by `price_ticks − anchor` over a ±32k-tick window around the mid, with best-price cursors that slide on updates, falling back to the map outside the window. API: `apply_depth(side, price_ticks, new_qty)`, `apply_snapshot(...)`, `best_bid()/best_ask()/mid_ticks()`, `qty_at(side, price_ticks)`, `depth_within(side, n_ticks)`. Invariants asserted every N events in debug: best_bid < best_ask (never crossed), all quantities ≥ 0, array and map agree in dual-run mode. On sequence gap: mark interval dirty (excluded from analytics), resnapshot, continue.

### 4.5 Event loop and latency model

A priority queue of timestamped events: market events at `exch_ts + δ_in` (that's when *you* see them), your order actions at `decision_ts + δ_out` (that's when the *exchange* sees them), timers. `δ = constant + Exp(jitter)` from the seeded RNG; grid over {5 ms, 50 ms, 200 ms} in experiments. Tie-break rule (document it): market events before your actions at equal timestamps — the conservative choice.

### 4.6 Order and queue tracker

Your order's lifecycle: `PENDING_NEW → RESTING → PARTIALLY_FILLED → FILLED | PENDING_CANCEL → CANCELED`. While `RESTING`, maintain `(A, B, remaining)` per §3.7, with the queue-assumption enum {PESS, OPT, PROP} chosen by config. A cancel/replace intent while `PENDING_*` is queued until the ack time. Every transition and every `(A,B)` update is loggable in debug for the worked-trace test.

### 4.7 Strategy interface

```cpp
struct Strategy {
  virtual void on_book(const BookView&, const Clock&) = 0;
  virtual void on_trade(const Trade&, const Clock&) = 0;
  virtual void on_fill(const Fill&, const Clock&) = 0;
  virtual void on_timer(const Clock&) = 0;      // e.g., every 100 ms
  // emits: place / cancel / replace via an OrderGateway (which applies δ_out)
};
```
Implementations S0–S3 per Part 6; all parameters from one YAML config (Appendix A) so experiment runs are pure config sweeps.

### 4.8 Analytics outputs (CSV, consumed by Python notebooks)

`fills.csv`: ts, side, price, qty, mid_at_fill, A_at_placement, queue_assumption, strategy, symbol. `markouts.csv`: fill_id × horizon → signed markout bp. `pnl_daily.csv`: date, strategy, assumption, fee_bp, latency_ms → spread_capture, adverse_selection_10s, inventory_pnl, fees, total (and the identity check). `probes.csv` for §3.8. Notebooks produce every figure in Part 7 from these files only — full separation of simulation and analysis.

### 4.9 Testing

Unit tests (GoogleTest): book operations against hand-built scenarios; queue updates against §3.7 worked by hand; decomposition identity on synthetic fills. Golden replay: a captured 10-minute slice with committed expected outputs; any diff fails CI. Determinism test: two runs, byte-compare. Property test: random event fuzzing must never cross the book or go negative. The Part 5 trace, automated.

### 4.10 Performance engineering (your CV numbers live here)

Targets: converter ≥ 1M msg/s; replay with book + strategy ≥ 300–500k events/s single-threaded; book `apply_depth` p50 < 100 ns, p99 < 1 µs on the dense path. Measure with Google Benchmark plus a whole-day wall-clock run; report exact hardware. Techniques you'll learn by hitting the wall: parse once to binary (JSON parsing is 100× the cost of everything else), dense arrays over node-based maps, `reserve()`, avoiding allocation in the hot loop, and measuring p99 rather than means. Resulting CV line: *"replays N million events/day at X k events/s; book update p99 Y ns (i7-xxxx)"* — every number measured.

---

## Part 5 — How the simulator works, end to end (a worked trace)

Fix ideas with a concrete run; your test suite automates this exact scenario. Book: bids 100.00×5.0, 99.99×8.0; asks 100.01×4.0. Config: pessimistic queue, δ_in = δ_out = 50 ms, strategy joins best bid.

1. **t=0** — strategy (on_timer) decides: place bid 1.0 @ 100.00. Gateway stamps arrival t=50 ms.
2. **t=50 ms** — order becomes RESTING at 100.00. Level shows 5.0 (you're a shadow): `A=5.0, B=0, remaining=1.0`.
3. **t=120 ms** — DEPTH: 100.00 → 6.5. Increase of 1.5 ⇒ arrivals behind: `B=1.5`. (A=5.0.)
4. **t=300 ms** — TRADE: sell-aggressor 2.0 @ 100.00. Front consumed: `A = 5.0−2.0 = 3.0`. No fill. Level (from feed) → 4.5.
5. **t=420 ms** — DEPTH: 100.00 → 3.0. Drop of 1.5 with no matching trade ⇒ cancel. Pessimistic: behind first → `B = 0`; A stays 3.0. (Optimistic would have given A=1.5 — log both in debug and you can *see* the assumptions diverge.)
6. **t=600 ms** — TRADE: sell-aggressor 3.5 @ 100.00. `fill = clamp(3.5 − 3.0, 0, 1.0) = 0.5`. You buy 0.5 @ 100.00; `A=0, remaining=0.5`. Mid just before: 100.005 ⇒ instant edge = +0.005×0.5 = $0.0025 (0.5 bp). Fee at 2 bp: −$0.0100. Ledger updates cash, inventory, and the decomposition simultaneously.
7. **t=600 ms + h** — markout job samples mid at +1 s (100.00 ⇒ AS = −0.005×0.5), +5 s, … building this fill's markout curve. Net at 1 s: +0.0025 − 0.0025 − 0.0100 = **−$0.0100** — a fill that *looks* fine and loses money after adverse selection and fees. Multiply by thousands of fills: that's the paper.
8. **t=900 ms** — strategy replaces the order (inventory skew moved its target price); cancel effective t=950 ms; a new placement rejoins a queue at the *back*, resetting A — the cost of repricing, which your results will quantify.

Naive touch-rule comparison on the same data: it fills the full 1.0 at step 4's first touch-trade, books the instant edge, and ignores everything after — precisely the fiction RQ1 measures.

---

## Part 6 — The strategy ladder (how each strategy works)

**S0 — Naive joiner (the strawman).** Always quote 1 unit at best bid and best ask; never skew; evaluate under BOTH the touch rule and the queue-aware sim. Purpose: RQ1's controlled comparison. Expected finding: touch-rule fill counts several times higher, and markedly *better-looking* per-fill prices, than queue-aware — the headline gap.

**S1 — Fixed-spread, inventory-capped.** Quote at fixed distances d ticks each side of mid; pull the side that would breach |q| ≤ Q_max; re-quote on a 100 ms timer with a minimum-move filter (don't churn queue position for sub-tick reasons — repricing resets you to the back, a cost S1 will make visible).

**S2 — Avellaneda–Stoikov.** Compute r and the optimal spread from §3.4 with rolling σ̂ (10 min) and calibrated k̂ (weekly refresh); alternative mode: GLFT distances (§3.5). Tick-floor rule from §3.4's practical adaptation. Parameters γ ∈ {1e−6, 1e−5, 1e−4} on calibration data only.

**S3 — A–S + microprice skew + toxicity pull.** Replace mid with weighted mid m_w in r; add a pull rule: if trade-flow imbalance over the last 2 s exceeds a threshold (one-sided aggression = incoming toxicity), cancel the endangered side for 1 s. RQ5 = S3 vs S2 per-fill markouts, out of sample.

Risk controls on all strategies: hard |q| cap with liquidation-at-touch beyond it, max order size, kill on book-desync (dirty intervals excluded from analytics), fee-aware minimum-edge check (never quote where even a benign fill loses to fees — log how often this binds; at 10 bp fees it will bind almost always, which is finding RQ4 announcing itself).

---

## Part 7 — Experiment design (the paper's engine)

**Data plan.** Record from this week; calibration = weeks 1–3, evaluation = weeks 4–6+ (more is better; the recorder never stops). Symbols: 2 tight (BTC, ETH perps) + 2 wider mid-caps. All parameter fitting (k, A, γ selection, imbalance regressions, logistic fill model) on calibration only; freeze in `EXPERIMENTS.md`; commit; then run evaluation once per the matrix.

**The matrix.** {queue: PESS, OPT, PROP} × {S0_touch, S0_queue, S1, S2, S3} × {maker fee bp: −0.5, 0, 1, 2, 5, 10} × {latency ms: 5, 50, 200} × {4 symbols}. Cheap to run (config sweeps over the same replay), and the fee/latency axes turn one strategy into a *map* of viability — far more scientific than a single cherry-picked configuration.

**Primary metrics.** Fill rate (fills per quote-hour and per placement); avg edge at fill (bp); adverse selection at 1 s/10 s (bp); net markout (bp); daily PnL decomposition; break-even fee per strategy/symbol; touch-vs-queue gap ratios (RQ1).

**Figures.** F1 markout curves by strategy (the money plot). F2 fill-probability heatmap: depth × queue-fraction (RQ2). F3 PnL decomposition stacked bars across fee grid (RQ4 frontier). F4 touch vs queue-aware fill counts and PnL, same strategy same data (RQ1). F5 λ(δ) calibration fit. F6 inventory paths under S1 vs S2 (the skew visibly taming q). Tables: matrix summaries with bootstrap CIs; parameter estimates with dates.

**Honesty policy (write it in the repo).** All configs pre-registered; no metric deleted after the fact; negative results reported with the same prominence as positive; every figure regenerable by one script from the committed CSVs.

---

## Part 8 — The research paper

**Title options.** "How Wrong Is a Naive Backtest? Queue-Aware Simulation of Passive Market Making in Cryptocurrency Limit Order Books" · "Queue Position, Adverse Selection, and the Fee Frontier: An Empirical Study of Passive Market Making".

**Skeleton (target 12–18 pages).** *Abstract* (150 words: problem, method, the RQ1 gap number, the RQ4 break-even, one-sentence honesty). *1 Introduction:* why passive backtests lie; contributions list. *2 Related work:* one paragraph each — A–S 2008 (optimal quoting under exponential intensities); Guéant–Lehalle–Fernandez-Tapia 2013 (inventory-bounded asymptotics you implement); Stoikov 2017 micro-price (fair value you test); Cont–Stoikov–Talreja 2010 (stochastic LOB dynamics); Huang–Lehalle–Rosenbaum 2015 queue-reactive model (the sophisticated cousin of your queue assumptions); Moallemi–Yuan (value of queue position — your §2.3 in theory form); Gould et al. 2013 LOB survey; Cartea–Jaimungal–Penalva 2015 textbook (framework); Harris 2003 (institutions). *3 Data:* feeds, sync protocol, volumes, dirty-interval handling. *4 Simulator:* §§4–5 condensed; the three queue assumptions as the key methodological device; latency model; limitations (L2 not L3, shadow order, single venue) stated *here*, not buried. *5 Strategies:* Part 6 with equations. *6 Results:* RQ1→RQ5 in order, figures F1–F6. *7 Discussion:* why the gap exists (queue + adverse selection), what fee tiers imply about who can make markets, what L3 would add. *8 Conclusion.* Reproducibility appendix: commit hash, config, seed, hardware.

**Where it goes.** arXiv (q-fin.TR) as a preprint — free, citable, permanent; mirror on SSRN; submit to your university's research day and undergraduate research journals. Set expectations correctly: a rigorous, reproducible preprint plus the repo is the asset that moves interviews; peer-reviewed acceptance is upside, not the goal. The line "author of an arXiv preprint measuring backtest bias in market making, with open-source C++ simulator" is a category above every project bullet you currently have.

---

## Part 9 — The 10-week build schedule (overlay on your master tracker)

⚠️ **Non-negotiable this week, before anything else: the recorder goes live.** Every day un-recorded is a day of evaluation data you can never recover. Recorder first, C++ after.

| Wk | Dates | Engineering deliverable | Research deliverable | Learning focus |
|---|---|---|---|---|
| 0 | now–Aug 2 | Python recorder live on 4 symbols, watchdog, disk checks | Symbol selection note (observed spreads) | Harris ch. 1–6; Binance stream docs |
| 1 | Aug 3–9 | Repo, CMake, CI, binary schema + converter | — | learncpp core; Part 2 here, cold |
| 2 | Aug 10–16 | Book core (map path) + snapshot sync + integrity checks | λ(δ) first fit on week-0 data (Python ok) | §3.3; A–S paper §§1–2 |
| 3 | Aug 17–23 | Dense-array fast path; golden replay test; first benchmarks | σ̂ estimator; imbalance regression (IC/R²) | §§3.2, 3.6; A–S results |
| 4 | Aug 24–30 | Event loop + latency model + order state machine | — | §3.7 by hand on paper |
| 5 | Aug 31–Sep 6 | Queue tracker, all 3 assumptions; Part-5 trace as a test | Probe-order fill logs begin | §3.8 |
| 6 | Sep 7–13 | S0 + S1; analytics CSVs; decomposition identity asserted | First markout curves (they will be ugly; good) | §3.9 |
| 7 | Sep 14–20 | S2 (A–S + GLFT mode); config-driven sweeps | Calibration frozen; **EXPERIMENTS.md committed** | §§3.4–3.5 until you can whiteboard them |
| 8 | Sep 21–27 | S3; performance pass to targets; determinism test in CI | — | §3.10 |
| 9 | Sep 28–Oct 4 | Full matrix runs on evaluation window | All figures F1–F6, tables, CIs | — |
| 10 | Oct 5–11 | README with results; repo public; **CV brackets filled with real numbers** | Paper first draft from Part 8 | — |
| — | Oct 12–25 | — | Paper v2, arXiv/SSRN submission; 2-page interview defense doc | Part 12 drill |

---

## Part 10 — Learning path

**Reading order (finance/math).** Harris, *Trading and Exchanges*, ch. 1–8 first — vocabulary and institutions (week 0–1). Then Avellaneda & Stoikov 2008 with §3.4 as your companion; don't fight the HJB derivation, own the two results and the worked example. Then GLFT 2013 (read for the formulas and the setup, skim proofs). Then Cartea–Jaimungal–Penalva ch. on market making — the textbook consolidation. Then skim Stoikov's micro-price and Huang–Lehalle–Rosenbaum for related-work fluency. Your Sem-VII stochastic mathematics course is perfectly timed: Poisson processes and Brownian motion are exactly the machinery of §3.4 — treat the course as project support, and tell interviewers that story.

**C++ path, mapped to modules.** learncpp through classes/RAII → converter; move semantics + templates → book core and strategy interface; chrono, PRNG, priority_queue → event loop; then the tooling that makes you employable: CMake, GoogleTest, Google Benchmark, clang-format, sanitizers (`-fsanitize=address,undefined` in debug CI — put "sanitizer-clean" in the README). You will learn more C++ from making `apply_depth` hit 100 ns than from any tutorial.

**Prompts for your own understanding (answer in writing, week by week):** Why does repricing cost queue position, exactly? Why is the reservation price linear in q? Why does small γ make spread ≈ 2/k? Why must headline numbers use the pessimistic queue model? Why is the decomposition identity exact and not approximate?

---

## Part 11 — The pitfalls catalogue (your "what didn't work" bank, pre-stocked)

Document each of these as you hit it — the README section they feed is, for interviewers, the most credible thing in the repo. (1) **Lookahead via timestamps:** using exchange time instead of exchange-time-plus-δ_in for decisions — silently makes you psychic. (2) **JSON in the hot loop:** parsing costs 100× the book update; convert once. (3) **Float price keys:** 0.1+0.2 problems corrupt levels; integers only. (4) **Crossed books after resync:** always rebuild from snapshot, never patch. (5) **Trade-to-level attribution:** aggTrade aggregation and timing windows misclassify some cancels as trades; measure the residual rate and report it. (6) **Repricing churn:** naive re-quoting every tick destroys queue position; the min-move filter is worth measuring as its own experiment. (7) **Fee illusion:** results at 0 bp mean nothing; the frontier is the result. (8) **Sharpe inflation:** intraday autocorrelation makes per-minute Sharpe fantasy; daily aggregation + bootstrap only. (9) **Calibration leakage:** tuning γ by peeking at evaluation weeks — pre-registration exists to stop you. (10) **Latency fantasy:** 5 ms results are decorative; 50–200 ms is your realistic band, present it as such. (11) **Shadow-order fiction:** your fills don't deplete real liquidity; say so, prominently. (12) **Survivorship in symbol choice:** picking symbols *after* seeing results — choose in week 0, commit, keep even the boring ones.

---

## Part 12 — Interview drill: twenty questions you must own

| Question | The shape of a strong answer |
|---|---|
| Why do naive backtests overstate passive PnL? | Touch rule ignores price-time priority (overstates fill count) and fills are adversely selected (misstates fill quality); quote your measured RQ1 gap. |
| What is adverse selection and how did you measure it? | Post-fill signed mid drift; markout curves by horizon; your bp numbers at 1 s/10 s. |
| Why exponential fill intensity? | Empirically decent near the touch, and it makes the HJB solvable in closed form; show your λ(δ) fit and where it bends. |
| Derive the reservation price's intuition. | Indifference price under CARA: holding q adds γσ²(T−t)·q of variance-penalty per unit — fair value shifts linearly against inventory. |
| Why can optimal spread ≈ 2/k? | Small-γ limit: (2/γ)ln(1+γ/k) → 2/k; elasticity of demand, not risk aversion, sets the monopolist-dealer spread. |
| Your Sharpe is X — do you believe it? | It's daily-aggregated, block-bootstrapped, 60-day sample, shadow-order sim; here are the CI and the reasons it's an upper bound. |
| Which queue assumption is right? | None observable under L2; pessimistic for headlines, three-way bracket as method; L3 validation is the stated future work. |
| How do you attribute cancels vs trades at a level? | Trade-stream matching in a time window; residual decreases = cancels; measured misattribution rate; its effect bounded by the PESS/OPT bracket. |
| What broke first when you scaled replay? | JSON parsing; then map allocations; binary format + dense array → your measured msg/s and p99. |
| Why C++ single-threaded core? | Determinism and cache behavior; parallelism belongs across runs (the matrix), not inside one. |
| What does latency do to a maker? | Stale quotes get picked off: adverse selection grows with δ; show your 5/50/200 ms comparison. |
| Why is BTC MM unviable at retail fees? | 1-tick spread ≈ 0.001 bp vs 2–10 bp fees — three orders of magnitude; rebates/alpha/wider instruments are the only outs; show the frontier. |
| What would you change with L3 data? | Observe true queue position → replace assumptions with ground truth; validate PESS/OPT bracket; per-order cancel behavior models. |
| What's the microprice and did it help? | Imbalance-weighted fair value; your IC/R²; the S3-vs-S2 markout delta, out of sample. |
| How do you know the simulator is right? | Invariants, golden replay, determinism CI, hand-worked trace test, decomposition identity asserted daily, probe-order monotonicity checks. |
| Biggest limitation? | Shadow order: no own-impact, no competitor reaction; results are conditional statements about history, not live PnL forecasts. |
| Inventory risk vs spread capture — which dominated? | Your decomposition bars; typically inventory swamps capture without skew — S1 vs S2 inventory paths. |
| Why pre-register experiments? | Multiple-testing discipline; frozen configs before OOS; commit hash as proof. |
| What did you find that surprised you? | (Fill with your real answer — have one; "the size of the touch-rule gap" or "how fast markouts go negative" are typical.) |
| If a desk gave you a month, what next? | L3 venue, queue-reactive model (HLR), maker-rebate tier simulation, cross-venue quoting — chosen because each attacks a named limitation. |

---

## Appendix A — Default parameter file (config.yaml)

```yaml
symbols: [BTCUSDT, ETHUSDT, MIDCAP1, MIDCAP2]   # fixed in week 0
tick_size: {BTCUSDT: 0.01, ...}
latency_ms: {in: 50, out: 50, jitter_exp_ms: 5}
queue_model: PESS            # PESS | OPT | PROP
fees_bp: {maker: 2.0}
strategy:
  name: S2_AS                # S0_touch|S0_queue|S1|S2_AS|S2_GLFT|S3
  gamma: 1.0e-5
  horizon_s: 60
  sigma_window_s: 600
  k_A_calibration: calib/kA_2026wk34.json
  requote_min_ticks: 2
  q_max: 5
  timer_ms: 100
seed: 42
eval_window: {start: 2026-09-01, end: 2026-10-10}
```

## Appendix B — Core references

Avellaneda & Stoikov (2008), *High-frequency trading in a limit order book*, Quantitative Finance. · Guéant, Lehalle & Fernandez-Tapia (2013), *Dealing with the inventory risk*, Math. Financ. Econ. · Stoikov (2017), *The micro-price*. · Cont, Stoikov & Talreja (2010), *A stochastic model for order book dynamics*, Oper. Res. · Huang, Lehalle & Rosenbaum (2015), *The queue-reactive model*, JASA. · Moallemi & Yuan, *The value of queue position in a limit order book*. · Gould et al. (2013), *Limit order books*, Quantitative Finance (survey). · Cartea, Jaimungal & Penalva (2015), *Algorithmic and High-Frequency Trading*, CUP. · Harris (2003), *Trading and Exchanges*, OUP. (All papers are freely findable as preprints; read in the Part 10 order.)
