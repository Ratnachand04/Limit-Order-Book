# Socratic — Phase 6: the strategy ladder

**1. Write the A–S reservation price and total spread from memory. For each of
the two spread terms, say in one sentence what it charges for and which
direction it moves when its input grows.**
Look at: `strategy/include/lob/strategy/as_math.hpp`, master plan §3.4.
Probe: which term survives as γ → 0, and what does that limit *mean*
economically?

**2. Take the worked example — BTC ≈ $100,000, σ = 8 $/√s, T−t = 60 s, γ = 1e−5,
k = 0.035. Compute the inventory skew per unit and the total spread. Then explain
why the answer means A–S "can tell you to quote far wider than the touch".**
Look at: `tests/test_as_math.cpp::TotalSpreadMatchesTheWorkedExample`.
Probe: what does the tick-floor rule do about that, and what does it keep from
the model?

**3. In GLFT, at q = +1 the bid moves *away* and the ask moves *closer*. Say why
that is the correct direction, and how you would catch it if the sign were
flipped.**
Look at: `Glft` in `strategy/src/as_math.cpp`,
`tests/test_as_math.cpp::LongInventoryMovesTheBidAwayAndTheAskCloser`.
Probe: what would a sign error look like in the F6 inventory-path figure?

**4. `GlftSkewCoefficient` computes `(1 + γ/k)^(1 + k/γ)` in log space. Show what
the exponent is for the worked example, and say what a direct `pow()` would do.**
Look at: `GlftSkewCoefficient`, `MonopolistMarkup`'s use of `log1p`.

**5. The min-edge check blocks a quote whose *benign* fill would still lose to
fees. At a 10 bp maker fee on a one-tick BTC spread, how often does it bind, and
why is the counter for that a *result* rather than a diagnostic?**
Look at: `QuotingStrategy::PassesMinEdge`,
`StrategyDiagnostics::quotes_blocked_by_min_edge`, master plan §2.4 and RQ4.
Probe: do the arithmetic — 1 tick of $0.01 on $100,000 is how many bp?

---

**Bonus:** S0_touch and S0_queue are the *same class*. Why does that matter for
RQ1, and what would be wrong with writing two separate strategies that happen to
behave similarly?
