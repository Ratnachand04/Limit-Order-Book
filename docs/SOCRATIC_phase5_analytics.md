# Socratic — Phase 5: ledger, markouts, probes

**1. Write the PnL decomposition from memory and prove it is *exact*, not an
approximation. Then say why this implementation can assert a residual of exactly
zero when the master plan only asked for 1e-9 of notional.**
Look at: `analytics/include/lob/analytics/ledger.hpp` header comment,
`Ledger::IdentityResidualX2`.
Probe: what is an "x2 unit" and what fraction does doubling remove?

**2. `Instrument`'s constructor refuses to build if `tick_size * lot_size * 1e8`
is not a whole number. What breaks if that check is removed, and how long would
it take you to notice?**
Look at: `common/src/instrument.cpp`, `tests/test_instrument.cpp`.

**3. Fees are held in tenths of a basis point, and a fee of 2.05 bp is rejected
outright. Defend that as a design choice rather than a limitation.**
Look at: `FeeBpToTenths`, `Instrument::Fee`, master plan §2.4's fee grid.

**4. A fill 30 seconds before the end of the run gets a 60-second markout sample
that never resolves. The sampler reports it as unresolved and excludes it. What
would back-filling with the last known mid do to the markout curve, and in which
direction?**
Look at: `MarkoutSampler::Advance`, `Summarise`,
`tests/test_analytics.cpp::UnresolvedSamplesAreReportedNotBackFilled`.

**5. Probe orders are tracked by the queue tracker exactly like real orders but
never touch the ledger. What are they measuring, and what are the three
monotonicities that must hold or the queue model is wrong?**
Look at: `sim/include/lob/sim/probe.hpp`, master plan §3.8.
Probe: how would you *use* a violated monotonicity — as a bug report, or as a
finding?

---

**Bonus:** the markout decomposes into edge at fill plus adverse selection at
horizon h. For a maker, which term is reliably negative, and what does its
*shape* over h tell you about who you are trading against?
