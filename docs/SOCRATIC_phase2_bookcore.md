# Socratic — Phase 2: the book core

**1. `DenseBook` keeps a dense quantity array *and* a one-bit-per-tick occupancy
bitmap over the same range. The array alone is enough to answer every query.
What is the bitmap for, and what is the worst-case cost it removes?**
Look at: `FindPrevSet` / `FindNextSet`, `RefreshWindowBestBid` in
`bookcore/src/dense_book.cpp`.
Probe: what happens when the best bid is deleted and the next level is 3000
ticks away?

**2. The window is ±32768 ticks. At BTCUSDT's $0.01 tick, how many dollars is
that, and how often would you expect a re-anchor on a normal day?**
Look at: `DenseBook::MaybeReanchor`, `docs/ARCHITECTURE.md`.
Probe: why does re-anchoring at *half* the window rather than at the edge stop
it from thrashing?

**3. A snapshot rebuilds a side instead of merging into it. Describe a concrete
sequence where merging leaves the book wrong in a way that is not detected for
hours.**
Look at: `MapBook::ApplySnapshotEnd`, master plan Part 11 pitfall #4.

**4. In `DualBook`, reads are answered by the `MapBook`, not the `DenseBook`.
Why that way round, given that `DenseBook` is the one being tested?**
Look at: `bookcore/include/lob/book/dual_book.hpp` header comment.
Probe: what would a run produce if the fast path drifted and reads came from it?

**5. `CheckInvariants` returns a `BookIntegrity` instead of throwing or
asserting. What does the caller do with a failure, and why is "crash" the wrong
answer for a book fed by a live exchange feed?**
Look at: `BookIntegrity`, `Simulator::HandleMarketEvent`.

---

**Bonus:** `apply_depth` p50 < 100 ns is the target. The percentile benchmark
times each call individually, which costs more than the call. What does that
make the reported number, and what would you say if an interviewer asked whether
you really hit 100 ns?
