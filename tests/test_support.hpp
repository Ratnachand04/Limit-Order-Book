// Shared helpers for the test suite.
//
// Everything here builds SYNTHETIC data.  That is a deliberate and bounded use:
// synthetic input is how you test a simulator.  It is never used to produce a
// result, a figure, or a number that appears in the README or the paper --
// CLAUDE.md rule 4 ("never fabricate results") governs outputs, not fixtures.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lob/config.hpp>
#include <lob/instrument.hpp>
#include <lob/types.hpp>

namespace lob::testing {

// A convenient instrument for hand-worked scenarios: tick 0.01, lot 0.001.
// tick * lot * 1e8 = 1000, an exact integer, so the ledger stays exact.
inline Instrument TestInstrument(std::uint8_t id = 0) {
  return Instrument("TESTUSDT", id, 0.01, 0.001);
}

// The instrument used by master plan Part 5's worked trace.  Prices there are
// around 100.00 and quantities are whole units of 0.1, so tick 0.01 and lot 0.1
// put both on exact integer grids: 100.00 -> 10000 ticks, 5.0 -> 50 lots.
inline Instrument WorkedTraceInstrument() { return Instrument("TRACE", 0, 0.01, 0.1); }

// Builds a config with everything deterministic and analytics off by default.
RunConfig MakeTestConfig(const Instrument& instrument, QueueModel queue = QueueModel::kPess,
                         Ts latency_us = 0);

// --- event construction ----------------------------------------------------
class EventBuilder {
 public:
  explicit EventBuilder(const Instrument& instrument) : inst_(instrument) {}

  EventBuilder& SnapshotBegin(Ts ts, Seq seq);
  EventBuilder& SnapshotLevel(Ts ts, Seq seq, Side side, double price, double qty);
  EventBuilder& SnapshotEnd(Ts ts, Seq seq);
  EventBuilder& Depth(Ts ts, Seq seq, Side side, double price, double new_qty);
  // `aggressor` is the side that crossed: kAsk means a SELL aggressor.
  EventBuilder& Trade(Ts ts, Seq seq, Side aggressor, double price, double qty);
  EventBuilder& MarkLastDirty();

  [[nodiscard]] const std::vector<Event>& events() const { return events_; }
  [[nodiscard]] std::vector<Event> take() { return std::move(events_); }

 private:
  Instrument inst_;
  std::vector<Event> events_;
};

// A short two-sided book with a handful of levels, used as the starting state
// for most scenarios.
std::vector<Event> SimpleOpeningBook(const Instrument& inst, Ts ts = 1'000'000, Seq seq = 100);

// A deterministic pseudo-market: a random walk with depth updates and trades.
// `seed` fully determines the output, so a fixture generated from it is
// reproducible on any machine.
std::vector<Event> SyntheticSession(const Instrument& inst, std::uint64_t seed, Ts start_ts_us,
                                    Ts duration_us, int events_per_second);

// Writes `events` to a temporary file under the build tree and returns the path.
std::string WriteTempEvents(const std::string& name, const std::vector<Event>& events);

// Reads a whole file as bytes; used by the determinism and golden tests.
std::string ReadFileBytes(const std::string& path);

// Directory that holds committed golden fixtures.  Set by the test CMakeLists.
std::string GoldenDir();

}  // namespace lob::testing
