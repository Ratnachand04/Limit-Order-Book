# Socratic — Phase 1: the converter

**1. Write the Binance sync protocol from memory, in five steps, and say what
each step protects against.**
Look at: `converter/include/lob/converter/converter.hpp` header comment,
`Converter::CheckSequence`, master plan §2.4.
Probe: why must the first applied diff *bracket* `lastUpdateId + 1` rather than
simply start after it?

**2. Binance's `aggTrade` sends `m` = "is the buyer the market maker". Our schema
stores the aggressor. Write the mapping, then say which of our resting orders a
trade with `m == true` can fill.**
Look at: `binance::ParseAggTrade`, `Event::ConsumedSide`,
`tests/test_converter.cpp::InvertsTheMakerFlagToGetTheAggressor`.
Probe: if you got this backwards, would any test in the project fail, or would
the fills just be quietly wrong?

**3. A depth message says a level's quantity is now 3.0. It does *not* say the
level fell by 1.5. Why does the schema store the absolute quantity, and what
would go wrong with deltas after a dropped message?**
Look at: the `qty_lots` comment in `common/include/lob/types.hpp`.

**4. On a gap, the converter marks events dirty and keeps going rather than
stopping. Justify both halves of that: why keep replaying, and why exclude the
interval from analytics?**
Look at: `Converter::RaiseGap`, `flags::kDirty`, master plan §4.4.
Probe: what would happen to the book if dirty events were dropped instead?

**5. At equal exchange timestamps the converter emits TRADE before DEPTH. What
breaks if you reverse that, and where exactly?**
Look at: `TypeRank` in `converter/src/converter.cpp`,
`QueueTracker::OnLevelUpdate`.
Probe: trace a level going 5.0 → 3.0 with a 2.0 trade printed at the same
microsecond, in both orders.

---

**Bonus:** the converter buffers a whole file and stable-sorts it rather than
streaming. What does that cost in memory for one hour of BTCUSDT, and what would
you change if it were a whole day?
