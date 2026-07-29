// JSONL -> binary Event conversion, including the Binance book-sync protocol.
//
// This is the only component that enforces master plan §2.4:
//
//   buffer diffs -> take a REST snapshot with its lastUpdateId -> discard
//   buffered diffs entirely below that id -> verify the first applied diff's
//   sequence range brackets lastUpdateId + 1 -> apply diffs in order forever,
//   checking continuity.  Any gap -> resnapshot and mark the interval dirty.
//
// Offline we cannot ask for a snapshot on demand, so a gap instead raises the
// dirty flag on every subsequent event until the recorder's next periodic
// snapshot arrives.  Dirty events are still replayed -- the book must stay live
// -- but analytics excludes them (§4.4).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <lob/converter/binance.hpp>
#include <lob/instrument.hpp>
#include <lob/types.hpp>

namespace lob {

struct ConverterStats {
  std::uint64_t lines_read = 0;
  std::uint64_t lines_malformed = 0;
  std::uint64_t lines_wrong_symbol = 0;

  std::uint64_t depth_messages = 0;
  std::uint64_t trade_messages = 0;
  std::uint64_t book_ticker_messages = 0;
  std::uint64_t snapshot_messages = 0;

  std::uint64_t depth_dropped_presync = 0;  // arrived before any snapshot
  std::uint64_t depth_dropped_stale = 0;    // u <= lastUpdateId of the snapshot
  std::uint64_t sequence_gaps = 0;
  std::uint64_t resyncs = 0;

  std::uint64_t events_emitted = 0;
  std::uint64_t events_dirty = 0;

  Ts first_ts_us = 0;
  Ts last_ts_us = 0;

  [[nodiscard]] std::string Report() const;
};

// One record per detected discontinuity, written to a sidecar report so the
// dirty intervals can be excluded from analytics and *counted* in the paper.
struct GapRecord {
  Ts ts_us = 0;
  Seq expected = 0;
  Seq got = 0;
  std::uint64_t line_number = 0;
  std::string reason;
};

struct ConverterOptions {
  Instrument instrument;
  binance::Market market = binance::Market::kAuto;
  // Events whose exchange timestamp falls outside [min_ts_us, max_ts_us] are
  // dropped.  0 means unbounded.  Used to cut the frozen evaluation window.
  Ts min_ts_us = 0;
  Ts max_ts_us = 0;
  // A trade may legitimately be timestamped a few ms before the depth update
  // that reflects it.  Emitting trades first at equal timestamps is the
  // conservative choice for queue tracking: a level decrease is only treated as
  // a cancel after the trades that explain it have been seen (§3.7).
  bool trades_before_depth_at_equal_ts = true;
};

class Converter {
 public:
  explicit Converter(ConverterOptions options);

  // Feeds one recorder JSONL line.  Returns false only for a malformed line
  // (which is counted, not fatal -- a truncated final line after a crash must
  // not lose the whole file).
  bool ProcessLine(std::string_view line, std::uint64_t line_number);

  // Sorts the accumulated events into replay order and hands them over.
  // Invalidates the internal buffer.
  std::vector<Event> Finish();

  [[nodiscard]] const ConverterStats& stats() const { return stats_; }
  [[nodiscard]] const std::vector<GapRecord>& gaps() const { return gaps_; }
  [[nodiscard]] bool synced() const { return synced_; }
  [[nodiscard]] bool dirty() const { return dirty_; }

  // Convenience: converts an entire stream to a binary file.  `reader` returns
  // false at end of input.  Returns the stats.
  static ConverterStats ConvertStream(const ConverterOptions& options,
                                      const std::function<bool(std::string&)>& read_line,
                                      const std::string& output_path,
                                      std::vector<GapRecord>* gaps_out);

 private:
  void EmitDepth(const binance::DepthUpdate& update, std::size_t first_event_index);
  bool CheckSequence(const binance::DepthUpdate& update, Ts ts_us, std::uint64_t line_number);
  void RaiseGap(Ts ts_us, Seq expected, Seq got, std::uint64_t line_number, std::string reason);
  void Keep(Event e);

  ConverterOptions options_;
  ConverterStats stats_;
  std::vector<GapRecord> gaps_;
  std::vector<Event> events_;
  std::vector<Event> scratch_;

  bool synced_ = false;
  bool dirty_ = false;
  bool first_diff_after_snapshot_ = false;
  Seq last_final_id_ = 0;
  binance::Market resolved_market_ = binance::Market::kAuto;
};

}  // namespace lob
