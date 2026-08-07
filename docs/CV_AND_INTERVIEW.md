# Using this project to get a callback

Written for a specific situation: applying to C++ / algo-dev roles at firms like
HRT, Citadel Securities, Jane Street, Optiver, IMC and Jump, with an academic
record that will not carry the application on its own.

The honest premise: **a project cannot fix a transcript, but it can make you the
candidate who obviously knows things a transcript does not measure.** Those firms
hire people who can be shown, in twenty minutes, to reason about correctness and
performance at a level most graduates cannot. This project can demonstrate that.
Not yet, entirely — see "What is still missing", which you should read first.

---

## 1. What is genuinely strong here

Be able to defend each of these cold. They are the reason this is worth putting
on a CV at all.

**A quadratic bug found by reading a benchmark table.** The first measured run
showed S0_touch — the *simplest* strategy — as the slowest at 89 k events/s. That
makes no sense, and the reason it made no sense was the bug: the order gateway's
live-order query scanned every order ever placed, and the touch model calls it on
every trade. Replay was quadratic in the length of the run. Fixing it with a
live-order index took replay from 166 k to 6.2 M events/s.

This is the single best story in the project. It is not "I optimised a loop". It
is *measure, notice the shape is wrong, form a hypothesis, confirm, fix,
re-measure* — which is the loop those firms actually screen for. And the honest
coda matters too: the unit tests never caught it and could not have, because
quadratic behaviour is only visible at length.

**An exact PnL identity, not an approximate one.** The design document asked for
`ΔE = spread capture + inventory − fees` to hold "within 1e-9 of notional". It
holds at **exactly zero**, because every quantity is an integer: cash in 1e-8
units, fees in integer tenths of a basis point, and every value carried at twice
its natural size so a half-tick mid never introduces a fraction. `Instrument`
refuses to construct if `tick_size × lot_size × 1e8` is not a whole number.

Why that is worth saying: a tolerance hides small bugs. Exact zero means any
non-zero residual is a defect, and the runner fails the run on it. That is a
design-taste signal, and it is rare.

**A measurement that was wrong, caught and replaced.** The first latency run
reported `p50 = 0 ns`. That was not a free operation — it was `steady_clock` on
Windows quantising at ~100 ns. Replacing it with an lfence-serialised `rdtsc`,
calibrated to nanoseconds, with the instrument's own 14.9 ns overhead measured
and reported alongside the percentiles, is the correct response. Knowing that
your instrument can be the thing that is wrong is a senior instinct.

**Determinism as an enforced contract.** Same data + same config + same seed ⇒
byte-identical CSVs. The test asserts byte equality *and* that both runs made the
same number of RNG draws — which is what proves no code path branched on
anything non-deterministic. Distributions are hand-written because
`std::exponential_distribution` is not portable across standard libraries. CI
runs the binary twice as separate processes and diffs the output.

**Two book implementations that must agree.** A `std::map` reference and a dense
array with a one-bit-per-tick occupancy bitmap (so "next non-empty level below"
is a `countl_zero` word scan, not a 65 k-element walk), cross-checked by a
`DualBook` that answers reads from the *reference* so a drifting fast path
degrades safely instead of silently corrupting results. 6.5× faster on
`ApplyDepth`; **and honestly slower on `DepthWithin`**, which is in the results
file because it is true.

**The domain content.** Queue-position modelling with three bracketing cancel
assumptions, adverse-selection markouts, and the touch-rule-versus-queue-aware
comparison. This is what stops you reading as a generic C++ developer.

---

## 2. What is still missing — read this before applying

**No real market data has been recorded, so the project has no findings.**

Every number in the repository today is either a performance measurement or a
result computed on a *synthetic* session. If an interviewer asks "so what did you
find?", the current honest answer is "nothing yet, I have not recorded data" —
and that is a bad answer after you have described a research simulator.

This is the highest-value thing you can do and it costs you almost nothing but
calendar time:

```bash
python -m pip install -r recorder/requirements.txt
python recorder/recorder.py --config recorder/symbols.yaml
```

**Start it today.** Six weeks of data cannot be acquired in the week before an
interview. Every day not recorded is a day of evaluation window you can never
get back. Until it has run, the project is a well-engineered simulator with
nothing to simulate, and that is a much weaker story than it needs to be.

Two smaller gaps, in order:

- **Benchmarks are on a noisy laptop.** CV is 12–15% on the pipeline benchmarks.
  If you get access to a quiet machine, re-run and update `bench/RESULTS.md`. If
  asked, say the numbers are lower bounds from an unverified-idle laptop — do not
  pretend otherwise.
- **The golden fixture is synthetic.** Fine as a regression anchor, and it is
  labelled as such. Cut a second fixture from real data once you have some.

---

## 3. CV lines

Use the numbers exactly. Every one is reproducible from the repository.

> **Queue-Aware Limit Order Book Simulator & Market-Making Study** — C++20, 15k LOC · [github.com/Ratnachand04/Limit-Order-Book](https://github.com/Ratnachand04/Limit-Order-Book)
>
> - Built a deterministic, event-driven LOB simulator that models a passive
>   order's **queue position** rather than assuming the naive touch-rule fill;
>   measured the resulting gap in fill rate on identical data.
> - Diagnosed an **O(n²)** hot path from benchmark shape alone (the simplest
>   strategy was the slowest); replaced a full-map scan with a live-order index —
>   **replay 166 k → 6.2 M events/s (37×)**.
> - Cut JSON→binary conversion **538 k → 1.0 M msg/s** by removing a triple-parse
>   of every message (bracket-matched extent scan) and parsing decimal prices
>   straight to fixed-point integers instead of via `double`.
> - Order book on a dense array with a bit-per-tick occupancy index:
>   **`apply_depth` 10 ns median, 43 ns p99**, cross-validated against a
>   `std::map` reference on every update in CI.
> - **Exact** PnL decomposition — integer cash in 1e-8 units makes the identity
>   `ΔE = spread + inventory − fees` hold at residual **exactly 0**, asserted
>   per-day; the runner fails the run otherwise.
> - Byte-level determinism (same data + seed ⇒ identical output) enforced by CI
>   across two processes; 209 tests incl. property/fuzz, golden replay, and a
>   hand-worked trace verified against values computed independently on paper.

If you only have room for two lines, keep the **O(n²)** one and the **exact PnL
identity** one. They are the two that make a reviewer stop skimming.

Once data exists, replace one performance bullet with the finding — a measured
RQ1 gap is worth more than any throughput number.

---

## 4. The 60-second version

Practise until it is fluent, then stop practising so it does not sound recited.

> Nearly every retail backtest of a limit-order strategy assumes that if the
> price touches your limit, you fill. That is fiction — on a real exchange with
> price-time priority, everyone who arrived at that price before you fills first.
> So I built a simulator that tracks how much size is ahead of my order in the
> queue and only fills when trades actually consume it.
>
> The hard part is that L2 data shows you a level shrank but not *which* orders
> left, so cancel position is unobservable. I run three assumptions —
> pessimistic, optimistic, proportional — and report the bracket, with
> pessimistic for headline numbers because a conservative bound is defensible.
>
> On the engineering side the two things I am most pleased with are that the PnL
> decomposition identity is exact rather than approximate — everything is
> integer, so any non-zero residual is a real bug and the run fails — and that I
> found a quadratic in my own hot path by noticing the simplest strategy
> benchmarked as the slowest. That one was a 37× speedup.

---

## 5. Questions you will be asked

**"What was the hardest bug?"** — The O(n²). Tell it as the *process*: the table
looked wrong before the code did. Say that the unit tests could not have caught
it and explain why.

**"Why is your PnL identity exact and not within a tolerance?"** — Because every
term is an integer. Walk through: cash in 1e-8 units; notional is
`price_ticks × qty_lots × (tick × lot × 1e8)` with the last factor validated to
be a whole number at construction; fees in integer tenths of a bp so the whole
{−0.5, 0, 1, 2, 5, 10} grid is representable; everything doubled so a half-tick
mid stays integral. Then the punchline: a tolerance would have hidden bugs.

**"How do you know the simulator is right?"** — Six independent mechanisms:
invariants asserted on the book, dual-run against a reference implementation,
byte-level determinism across processes, a hand-worked trace whose expected
values were computed on paper before any code, the decomposition identity
asserted per day, and probe-order monotonicities (deeper ⇒ less likely to fill)
which are a self-check on the queue model.

**"Which queue assumption is right?"** — Reject the premise. None is observable
under L2. That is why the bracket *is* the method, why headline numbers use
pessimistic, and why validating against true L3 data is the stated future work.

**"What is the biggest limitation?"** — The shadow order. It never depletes real
liquidity and no competitor reacts to it, so results are conditional statements
about recorded history, not live PnL forecasts. Say this before you are asked;
volunteering the limitation is worth more than defending against it.

**"Is your benchmark trustworthy?"** — Three caveats, offered unprompted: a
laptop that was not verified idle with 12–15% CV; synthetic input, so absolute
numbers will differ on real data; and the median latency is close to the
instrument's own 14.9 ns floor, which is why the throughput figure is the better
median and the rdtsc percentiles are the better tail.

**"Why C++ single-threaded?"** — Determinism and cache behaviour. Parallelism
belongs *across* runs — the 324-cell experiment matrix — not inside one.

**Expect a follow-up you cannot fully answer.** Say so, say what you would
measure to find out, and move on. Bluffing is the failure mode; "I don't know,
here's how I'd check" is not.

---

## 6. Practical notes on applying

- **The README is the interview.** A reviewer spends 60–90 seconds. It opens
  with the problem in plain language, states the status honestly including what
  has not been done, and puts the assumptions-and-limitations block where it
  cannot be missed. Do not let it drift into a feature list.
- **Referrals beat the portal**, especially with a weak transcript. Engineers at
  these firms respond to a short, specific message with a link and one concrete
  technical detail — the O(n²) find works well — far better than to a cover
  letter.
- **Do not oversell.** These interviewers detect inflation instantly, and one
  unsupportable claim costs you the whole project's credibility. "I measured
  this on a laptop and here are the caveats" reads as senior. "6.2 M events per
  second" with no context reads as a red flag.
- **The negative result is an asset.** At retail fee tiers, passive market
  making on tight majors is structurally unprofitable — 1-tick spread capture on
  BTC is ~0.001 bp against a 2–10 bp fee, three orders of magnitude under water.
  When the data shows that, report it as the finding. An honest negative result
  with a rigorous method is a better interview story than a suspicious Sharpe,
  and every experienced person in the room knows it.
- **Keep the interview-defence habit.** Anything you cannot derive at a
  whiteboard should not be a bullet.

---

## 7. What to do next, in order

1. **Start the recorder today.** Nothing else on this list matters as much.
2. Pick the four symbols with `recorder/symbol_scout.py`, commit the choice to
   `recorder/symbols.yaml` before any results exist, and keep even the boring
   ones.
3. Work through `docs/SOCRATIC_phase*.md` and write the answers down. If you
   cannot answer them, you cannot defend the project, and it is better to find
   that out now.
4. After three weeks of data: run `lob_calibrate`, freeze the parameters into
   `docs/EXPERIMENTS.md`, and commit it **before** touching the evaluation
   window. The commit hash predating the evaluation runs is the proof, and it is
   the kind of scientific discipline that makes a reviewer sit up.
5. Run the matrix, generate the figures, and put the real RQ1 number in the
   README and on the CV.
6. Re-run the benchmarks on the quietest machine you can find and update
   `bench/RESULTS.md`.
