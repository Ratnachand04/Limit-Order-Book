// The virtual clock (master plan §4.1, §4.5).
//
// There is no wall-clock time anywhere in the simulation core.  Time advances
// only when the event loop pops an event, and it never goes backwards.  That is
// what makes a run a pure function of (data, config, seed).
//
// The distinction this type exists to enforce:
//
//   exchange time  -- when a thing happened at the venue (Event::exch_ts_us)
//   local time     -- when WE could have known about it (exch_ts + delta_in)
//
// A strategy may only ever read local time.  Reading exchange time in a
// decision is Part 11 pitfall #1: it silently makes the strategy psychic.
#pragma once

#include <lob/types.hpp>

namespace lob {

class Clock {
 public:
  Clock() = default;

  // Local ("our") time: the timestamp of the event currently being delivered.
  [[nodiscard]] Ts now_us() const { return now_us_; }
  [[nodiscard]] double now_s() const {
    return static_cast<double>(now_us_) / static_cast<double>(kUsPerSecond);
  }

  // Exchange time of the market event currently being processed.  Provided for
  // ANALYTICS ONLY (CSV timestamps, markout alignment) -- never for decisions.
  [[nodiscard]] Ts exchange_ts_us() const { return exchange_ts_us_; }

  [[nodiscard]] double SecondsSince(Ts earlier_us) const {
    return static_cast<double>(now_us_ - earlier_us) / static_cast<double>(kUsPerSecond);
  }

  // Only the event loop may advance the clock.
  void AdvanceTo(Ts local_us) {
    if (local_us > now_us_) {
      now_us_ = local_us;
    }
  }
  void SetExchangeTs(Ts exch_us) { exchange_ts_us_ = exch_us; }
  void Reset() {
    now_us_ = 0;
    exchange_ts_us_ = 0;
  }

 private:
  Ts now_us_ = 0;
  Ts exchange_ts_us_ = 0;
};

}  // namespace lob
