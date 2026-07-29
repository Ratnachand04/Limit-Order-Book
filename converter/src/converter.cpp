#include <lob/converter/converter.hpp>

#include <algorithm>
#include <sstream>

#include <lob/converter/event_io.hpp>

namespace lob {
namespace {

// Order in which event types are replayed when their exchange timestamps tie.
// A snapshot re-anchors the book, so its markers and levels must land before
// anything that reads the book.  Trades precede depth so that a level decrease
// is only classified as a cancel after the trade that explains it has been
// applied (§3.7 -- misclassifying trades as cancels biases queue position).
int TypeRank(std::uint8_t type, bool trades_first) {
  switch (static_cast<EventType>(type)) {
    case EventType::kSnapshotBegin:
      return 0;
    case EventType::kSnapshotLevel:
      return 1;
    case EventType::kSnapshotEnd:
      return 2;
    case EventType::kTrade:
      return trades_first ? 3 : 4;
    case EventType::kDepth:
      return trades_first ? 4 : 3;
  }
  return 5;
}

}  // namespace

std::string ConverterStats::Report() const {
  std::ostringstream os;
  os << "lines_read            " << lines_read << "\n"
     << "lines_malformed       " << lines_malformed << "\n"
     << "lines_wrong_symbol    " << lines_wrong_symbol << "\n"
     << "depth_messages        " << depth_messages << "\n"
     << "trade_messages        " << trade_messages << "\n"
     << "book_ticker_messages  " << book_ticker_messages << "\n"
     << "snapshot_messages     " << snapshot_messages << "\n"
     << "depth_dropped_presync " << depth_dropped_presync << "\n"
     << "depth_dropped_stale   " << depth_dropped_stale << "\n"
     << "sequence_gaps         " << sequence_gaps << "\n"
     << "resyncs               " << resyncs << "\n"
     << "events_emitted        " << events_emitted << "\n"
     << "events_dirty          " << events_dirty << "\n"
     << "first_ts_us           " << first_ts_us << "\n"
     << "last_ts_us            " << last_ts_us << "\n";
  if (events_emitted > 0) {
    const double dirty_pct =
        100.0 * static_cast<double>(events_dirty) / static_cast<double>(events_emitted);
    os << "dirty_fraction_pct    " << dirty_pct << "\n";
  }
  return os.str();
}

Converter::Converter(ConverterOptions options)
    : options_(std::move(options)), resolved_market_(options_.market) {
  events_.reserve(1U << 20);
  scratch_.reserve(1024);
}

void Converter::Keep(Event e) {
  if (options_.min_ts_us != 0 && e.exch_ts_us < options_.min_ts_us) {
    return;
  }
  if (options_.max_ts_us != 0 && e.exch_ts_us > options_.max_ts_us) {
    return;
  }
  if (dirty_) {
    e.flags |= flags::kDirty;
    ++stats_.events_dirty;
  }
  if (stats_.events_emitted == 0 || e.exch_ts_us < stats_.first_ts_us) {
    stats_.first_ts_us = e.exch_ts_us;
  }
  if (e.exch_ts_us > stats_.last_ts_us) {
    stats_.last_ts_us = e.exch_ts_us;
  }
  ++stats_.events_emitted;
  events_.push_back(e);
}

void Converter::RaiseGap(Ts ts_us, Seq expected, Seq got, std::uint64_t line_number,
                         std::string reason) {
  ++stats_.sequence_gaps;
  dirty_ = true;
  GapRecord g;
  g.ts_us = ts_us;
  g.expected = expected;
  g.got = got;
  g.line_number = line_number;
  g.reason = std::move(reason);
  gaps_.push_back(std::move(g));
}

bool Converter::CheckSequence(const binance::DepthUpdate& u, Ts ts_us,
                              std::uint64_t line_number) {
  // Resolve the venue's sequencing dialect from the first message that shows
  // its hand: only USD-M futures carries `pu`.
  if (resolved_market_ == binance::Market::kAuto) {
    resolved_market_ = u.has_prev_id ? binance::Market::kFutures : binance::Market::kSpot;
  }

  if (first_diff_after_snapshot_) {
    // §2.4: the first applied diff's range must bracket lastUpdateId + 1.
    // Anything else means the snapshot and the stream do not join up.
    if (!(u.first_id <= last_final_id_ + 1 && u.final_id >= last_final_id_ + 1)) {
      RaiseGap(ts_us, last_final_id_ + 1, u.first_id, line_number,
               "first diff after snapshot does not bracket lastUpdateId+1");
    }
    first_diff_after_snapshot_ = false;
    last_final_id_ = u.final_id;
    return true;
  }

  if (resolved_market_ == binance::Market::kFutures && u.has_prev_id) {
    if (u.prev_final_id != last_final_id_) {
      RaiseGap(ts_us, last_final_id_, u.prev_final_id, line_number,
               "futures continuity broken: pu != previous u");
    }
  } else {
    if (u.first_id != last_final_id_ + 1) {
      RaiseGap(ts_us, last_final_id_ + 1, u.first_id, line_number,
               "spot continuity broken: U != previous u + 1");
    }
  }
  last_final_id_ = u.final_id;
  return true;
}

bool Converter::ProcessLine(std::string_view line, std::uint64_t line_number) {
  ++stats_.lines_read;
  if (line.empty()) {
    return true;
  }

  binance::RecorderLine env;
  std::string error;
  if (!binance::ParseRecorderLine(line, env, &error)) {
    ++stats_.lines_malformed;
    return false;
  }
  if (!env.symbol.empty() && env.symbol != options_.instrument.symbol()) {
    ++stats_.lines_wrong_symbol;
    return true;
  }

  switch (env.channel) {
    case binance::Channel::kSnapshot: {
      ++stats_.snapshot_messages;
      scratch_.clear();
      binance::DepthSnapshot snap;
      if (!binance::ParseDepthSnapshot(env.payload, options_.instrument, env.recv_ts_us, snap,
                                       scratch_, &error)) {
        ++stats_.lines_malformed;
        return false;
      }
      // A snapshot re-anchors the book: the dirty interval ends here.
      const bool was_dirty = dirty_;
      dirty_ = false;
      synced_ = true;
      first_diff_after_snapshot_ = true;
      last_final_id_ = snap.last_update_id;
      if (was_dirty) {
        ++stats_.resyncs;
      }
      for (Event& e : scratch_) {
        e.flags |= flags::kResync;
        Keep(e);
      }
      return true;
    }

    case binance::Channel::kDepth: {
      ++stats_.depth_messages;
      scratch_.clear();
      binance::DepthUpdate update;
      if (!binance::ParseDepthUpdate(env.payload, options_.instrument, update, scratch_,
                                     &error)) {
        ++stats_.lines_malformed;
        return false;
      }
      if (!synced_) {
        // No base book yet: these diffs describe changes to a state we do not
        // have.  Applying them would silently corrupt the book (Part 11 #4).
        ++stats_.depth_dropped_presync;
        return true;
      }
      if (update.final_id <= last_final_id_ && !first_diff_after_snapshot_) {
        ++stats_.depth_dropped_stale;
        return true;
      }
      if (first_diff_after_snapshot_ && update.final_id <= last_final_id_) {
        // Entirely below the snapshot: discard per §2.4.
        ++stats_.depth_dropped_stale;
        return true;
      }
      CheckSequence(update, update.exch_ts_us, line_number);
      for (const Event& e : scratch_) {
        Keep(e);
      }
      return true;
    }

    case binance::Channel::kAggTrade: {
      ++stats_.trade_messages;
      binance::AggTrade trade;
      if (!binance::ParseAggTrade(env.payload, options_.instrument, trade, &error)) {
        ++stats_.lines_malformed;
        return false;
      }
      Keep(MakeTrade(trade.exch_ts_us, trade.agg_id, trade.aggressor, trade.price_ticks,
                     trade.qty_lots, options_.instrument.symbol_id()));
      return true;
    }

    case binance::Channel::kBookTicker: {
      // Counted but not converted: the binary schema reconstructs the BBO from
      // the depth stream, and a second source of truth for it would be a second
      // thing to keep consistent.  Kept in the raw data for cross-checks.
      ++stats_.book_ticker_messages;
      return true;
    }

    case binance::Channel::kUnknown:
    default:
      ++stats_.lines_malformed;
      return false;
  }
}

std::vector<Event> Converter::Finish() {
  // Stable sort on (timestamp, type rank).  Stability preserves recorder
  // arrival order within a tie, which is the only extra information we have.
  const bool trades_first = options_.trades_before_depth_at_equal_ts;
  std::stable_sort(events_.begin(), events_.end(),
                   [trades_first](const Event& a, const Event& b) {
                     if (a.exch_ts_us != b.exch_ts_us) {
                       return a.exch_ts_us < b.exch_ts_us;
                     }
                     return TypeRank(a.type, trades_first) < TypeRank(b.type, trades_first);
                   });
  if (!events_.empty()) {
    stats_.first_ts_us = events_.front().exch_ts_us;
    stats_.last_ts_us = events_.back().exch_ts_us;
  }
  return std::move(events_);
}

ConverterStats Converter::ConvertStream(const ConverterOptions& options,
                                        const std::function<bool(std::string&)>& read_line,
                                        const std::string& output_path,
                                        std::vector<GapRecord>* gaps_out) {
  Converter converter(options);
  std::string line;
  std::uint64_t n = 0;
  while (read_line(line)) {
    ++n;
    // Tolerate CRLF input even though the recorder writes LF.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    if (!line.empty()) {
      converter.ProcessLine(line, n);
    }
  }
  const std::vector<Event> events = converter.Finish();
  WriteAllEvents(output_path, events);
  if (gaps_out != nullptr) {
    *gaps_out = converter.gaps();
  }
  return converter.stats();
}

}  // namespace lob
