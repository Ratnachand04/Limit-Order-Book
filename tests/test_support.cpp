#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <lob/converter/event_io.hpp>
#include <lob/rng.hpp>

namespace lob::testing {

RunConfig MakeTestConfig(const Instrument& instrument, QueueModel queue, Ts latency_us) {
  RunConfig c;
  c.run_id = "test";
  c.instruments.push_back(instrument);
  c.queue_model = queue;
  c.latency.in_us = latency_us;
  c.latency.out_us = latency_us;
  c.latency.jitter_exp_us = 0.0;  // tests want the latency to be exactly as configured
  c.fees.maker_tenth_bp = 0;
  c.fees.taker_tenth_bp = 0;
  c.seed = 42;
  c.strategy.order_size_lots = 1;
  c.strategy.q_max_lots = 1000;
  c.strategy.timer_us = 100 * kUsPerMilli;
  c.strategy.requote_min_ticks = 1;
  c.strategy.enforce_min_edge = false;  // zero fees, so the check is vacuous
  c.probes.enabled = false;
  return c;
}

// ---------------------------------------------------------------------------
EventBuilder& EventBuilder::SnapshotBegin(Ts ts, Seq seq) {
  events_.push_back(MakeSnapshotMarker(EventType::kSnapshotBegin, ts, seq, inst_.symbol_id()));
  return *this;
}

EventBuilder& EventBuilder::SnapshotLevel(Ts ts, Seq seq, Side side, double price, double qty) {
  events_.push_back(MakeSnapshotLevel(ts, seq, side, inst_.ToTicks(price), inst_.ToLots(qty),
                                      inst_.symbol_id()));
  return *this;
}

EventBuilder& EventBuilder::SnapshotEnd(Ts ts, Seq seq) {
  events_.push_back(MakeSnapshotMarker(EventType::kSnapshotEnd, ts, seq, inst_.symbol_id()));
  return *this;
}

EventBuilder& EventBuilder::Depth(Ts ts, Seq seq, Side side, double price, double new_qty) {
  events_.push_back(MakeDepth(ts, seq, side, inst_.ToTicks(price), inst_.ToLots(new_qty),
                              inst_.symbol_id()));
  return *this;
}

EventBuilder& EventBuilder::Trade(Ts ts, Seq seq, Side aggressor, double price, double qty) {
  events_.push_back(
      MakeTrade(ts, seq, aggressor, inst_.ToTicks(price), inst_.ToLots(qty), inst_.symbol_id()));
  return *this;
}

EventBuilder& EventBuilder::MarkLastDirty() {
  if (!events_.empty()) {
    events_.back().flags |= flags::kDirty;
  }
  return *this;
}

// ---------------------------------------------------------------------------
std::vector<Event> SimpleOpeningBook(const Instrument& inst, Ts ts, Seq seq) {
  EventBuilder b(inst);
  b.SnapshotBegin(ts, seq);
  b.SnapshotLevel(ts, seq, Side::kBid, 100.00, 5.0);
  b.SnapshotLevel(ts, seq, Side::kBid, 99.99, 8.0);
  b.SnapshotLevel(ts, seq, Side::kBid, 99.98, 12.0);
  b.SnapshotLevel(ts, seq, Side::kAsk, 100.01, 4.0);
  b.SnapshotLevel(ts, seq, Side::kAsk, 100.02, 9.0);
  b.SnapshotLevel(ts, seq, Side::kAsk, 100.03, 15.0);
  b.SnapshotEnd(ts, seq);
  return b.take();
}

std::vector<Event> SyntheticSession(const Instrument& inst, std::uint64_t seed, Ts start_ts_us,
                                    Ts duration_us, int events_per_second) {
  Rng rng(seed);
  std::vector<Event> out = SimpleOpeningBook(inst, start_ts_us, 1000);

  // Track a simple two-sided book so the generated stream stays self-consistent
  // (never crossed, never negative) -- a fuzz stream that violates the market's
  // own invariants would test the book against input it can never see.
  Ticks bid = inst.ToTicks(100.00);
  Ticks ask = inst.ToTicks(100.01);
  std::vector<Lots64> bid_qty(8, 50);
  std::vector<Lots64> ask_qty(8, 40);

  Seq seq = 1001;
  const Ts step_us = (events_per_second > 0)
                         ? (kUsPerSecond / static_cast<Ts>(events_per_second))
                         : kUsPerSecond;
  for (Ts t = start_ts_us + step_us; t < start_ts_us + duration_us; t += step_us) {
    const double u = rng.Uniform01();
    if (u < 0.10) {
      // Mid moves one tick.
      const bool up = rng.Uniform01() < 0.5;
      if (up) {
        bid += 1;
        ask += 1;
      } else {
        bid -= 1;
        ask -= 1;
      }
      out.push_back(MakeDepth(t, seq++, Side::kBid, bid, bid_qty[0], inst.symbol_id()));
      out.push_back(MakeDepth(t, seq++, Side::kAsk, ask, ask_qty[0], inst.symbol_id()));
      // Delete the level that is now on the wrong side of the touch.
      out.push_back(MakeDepth(t, seq++, up ? Side::kBid : Side::kAsk,
                              up ? bid - 1 : ask + 1, 0, inst.symbol_id()));
    } else if (u < 0.30) {
      // A trade against one side.
      const bool sell_aggressor = rng.Uniform01() < 0.5;
      const Side aggressor = sell_aggressor ? Side::kAsk : Side::kBid;
      const Ticks px = sell_aggressor ? bid : ask;
      const Lots64 available = sell_aggressor ? bid_qty[0] : ask_qty[0];
      const Lots64 qty = std::max<Lots64>(1, rng.UniformInt(1, std::max<Lots64>(1, available / 2)));
      out.push_back(MakeTrade(t, seq++, aggressor, px, static_cast<Lots>(qty), inst.symbol_id()));
      // The level shrinks by the traded amount.
      if (sell_aggressor) {
        bid_qty[0] = std::max<Lots64>(1, bid_qty[0] - qty);
        out.push_back(MakeDepth(t, seq++, Side::kBid, bid, bid_qty[0], inst.symbol_id()));
      } else {
        ask_qty[0] = std::max<Lots64>(1, ask_qty[0] - qty);
        out.push_back(MakeDepth(t, seq++, Side::kAsk, ask, ask_qty[0], inst.symbol_id()));
      }
    } else {
      // Ordinary quantity churn at a random level: additions and cancels.
      const bool is_bid = rng.Uniform01() < 0.5;
      const int level = static_cast<int>(rng.UniformInt(0, 5));
      const Lots64 delta = rng.UniformInt(-15, 25);
      if (is_bid) {
        bid_qty[static_cast<std::size_t>(level)] =
            std::max<Lots64>(1, bid_qty[static_cast<std::size_t>(level)] + delta);
        out.push_back(MakeDepth(t, seq++, Side::kBid, bid - level,
                                static_cast<Lots>(bid_qty[static_cast<std::size_t>(level)]),
                                inst.symbol_id()));
      } else {
        ask_qty[static_cast<std::size_t>(level)] =
            std::max<Lots64>(1, ask_qty[static_cast<std::size_t>(level)] + delta);
        out.push_back(MakeDepth(t, seq++, Side::kAsk, ask + level,
                                static_cast<Lots>(ask_qty[static_cast<std::size_t>(level)]),
                                inst.symbol_id()));
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
std::string WriteTempEvents(const std::string& name, const std::vector<Event>& events) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lob_sim_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / (name + ".lobbin");
  WriteAllEvents(path.string(), events);
  return path.string();
}

std::string ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

std::string GoldenDir() {
#ifdef LOB_GOLDEN_DIR
  return std::string(LOB_GOLDEN_DIR);
#else
  return std::string("tests/fixtures");
#endif
}

}  // namespace lob::testing
