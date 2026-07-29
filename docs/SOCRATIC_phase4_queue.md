# Socratic — Phase 4: the queue tracker

This is the module the whole project rests on. If any answer here is shaky, the
results are not defensible.

**1. Write the four §3.7 rules from memory: placement, arrival, trade,
unexplained decrease. Then say which one is the *only* unambiguous one, and why
the other three are modelling choices.**
Look at: `sim/include/lob/sim/queue_tracker.hpp` header comment.

**2. Work the Part 5 trace by hand for all three assumptions, and state the fill
quantity each gives at step 6. Then say why PESS ≤ PROP ≤ OPT must hold for
*any* input, not just this one.**
Look at: `tests/test_queue_tracker.cpp::Part5WorkedTrace`.
Probe: what would it mean if you found an input where PESS filled more than OPT?

**3. The tracker banks traded quantity as *credit* against a level, and the
credit expires after an attribution window. What is the credit for, what does
the expiry protect against, and what is the counter it feeds?**
Look at: `QueueTracker::ExpireCredit`, `QueueTrackerStats::lots_trade_credit_expired`,
master plan Part 11 pitfall #5.
Probe: why is the residual reported rather than being tuned to zero?

**4. The proportional split is computed as `ΔC * A / total` in integers with the
remainder decided by one RNG draw, not as `ΔC * (A / total)` in doubles. Give the
concrete case from the Part 5 trace where the floating-point version gets the
answer wrong.**
Look at: `ApplyCancel`, `QueueModel::kProp` branch.
Probe: 15 × 30/45 — what does `floor` see in double arithmetic?

**5. Two of our orders resting at the same price each see the full trade
quantity; they do not queue behind each other. Name this assumption, say why it
is unavoidable here, and say where in the paper it must appear.**
Look at: `QueueTracker::OnTrade` comment, master plan §2.5, Part 11 pitfall #11.

---

**Bonus, and the interview question:** *"Which queue assumption is right?"* Give
the full answer — the one that starts by rejecting the premise.
