# Socratic — Phase 3: event loop, latency, orders

**1. At equal timestamps, market events are processed before our actions. State
the rule, then say which side of the trade it is conservative *for*, and give the
concrete scenario it decides.**
Look at: `SimEventLater` in `sim/include/lob/sim/event_queue.hpp`, master plan
§4.5.
Probe: a cancel and an adverse trade land in the same microsecond — who wins,
and why is that the honest choice?

**2. `PENDING_CANCEL` is classified as LIVE. Explain why, and name the class of
backtest bug that classifying it as terminal would create.**
Look at: `IsLive` in `common/include/lob/execution.hpp`,
`OrderGateway::Cancel`.
Probe: which fills specifically would disappear, and would they be the good ones
or the bad ones?

**3. Market events are made visible at `exch_ts + δ_in`, but the visible
timestamps are then clamped to be monotone. Why is the unclamped version wrong,
given that the jitter draws are genuinely independent?**
Look at: `schedule_next_market` in `sim/src/simulator.cpp`.
Probe: what does a feed physically do that independent jitter does not model?

**4. A strategy can reach `Clock::now_us()` but there is no path from a
`Strategy` to an event's `exch_ts_us` for decision-making. Why was that made a
structural property rather than a coding rule?**
Look at: `sim/include/lob/sim/clock.hpp`, `StrategyContext`.
Probe: master plan Part 11 pitfall #1 — what does the bug look like in the
results when it happens?

**5. The determinism test asserts byte-identical CSVs *and* an identical count of
RNG draws. What does the second assertion catch that the first would miss?**
Look at: `tests/test_determinism.cpp`, `Rng::draws()`.
Probe: think of a change that produces the same output today but is not actually
deterministic.

---

**Bonus:** distributions are hand-written (`Rng::ExponentialMean`) rather than
using `std::exponential_distribution`. Why does that matter for a project whose
CI runs on Linux and whose author develops on Windows?
