// Binance message decoding: recorder JSONL line -> Event records.
//
// The recorder (recorder/recorder.py) writes one JSON object per line:
//
//   {"t": <local recv time, microseconds>,
//    "c": "depth" | "aggTrade" | "bookTicker" | "snapshot",
//    "s": "BTCUSDT",
//    "d": { ...verbatim exchange payload... }}
//
// Keeping the exchange payload verbatim is deliberate: the raw stream is the
// single source of truth (master plan §4.1), so the recorder must never
// reinterpret it, and every downstream artefact stays a pure function of it.
//
// Field names below follow the Binance stream documentation; see §2.4 for the
// reconstruction protocol these feed.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lob/instrument.hpp>
#include <lob/types.hpp>

namespace lob::binance {

enum class Channel : std::uint8_t {
  kUnknown = 0,
  kDepth,       // <symbol>@depth@100ms  -- diff depth stream
  kAggTrade,    // <symbol>@aggTrade
  kBookTicker,  // <symbol>@bookTicker   -- BBO, used for cross-checks only
  kSnapshot,    // REST /depth response  -- the resync anchor
};

Channel ChannelFromName(std::string_view name);
std::string_view ChannelName(Channel c);

// Which sequencing rule applies (master plan §2.4).  Spot streams carry only
// (U, u); USD-M futures streams additionally carry `pu`, the previous final
// update id, which makes continuity checkable without arithmetic on ranges.
enum class Market : std::uint8_t { kAuto = 0, kSpot, kFutures };

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------
struct RecorderLine {
  Ts recv_ts_us = 0;
  Channel channel = Channel::kUnknown;
  std::string_view symbol;
  std::string_view payload;  // span of the raw "d" object inside the input line
};

// Parses the envelope only; `payload` points into `line` and stays valid for as
// long as the caller's buffer does.  Also transparently unwraps a combined
// stream payload of the form {"stream": "...", "data": {...}}.
bool ParseRecorderLine(std::string_view line, RecorderLine& out, std::string* error);

// ---------------------------------------------------------------------------
// depthUpdate
// ---------------------------------------------------------------------------
struct DepthUpdate {
  Ts exch_ts_us = 0;
  Seq first_id = 0;       // "U"
  Seq final_id = 0;       // "u"
  Seq prev_final_id = 0;  // "pu" (futures only)
  bool has_prev_id = false;
  std::size_t bid_levels = 0;
  std::size_t ask_levels = 0;
};

// Appends one kDepth Event per level to `out`.  Quantities are ABSOLUTE (the
// stream semantics), so a quantity of 0 is a level delete -- exactly what the
// binary schema means by qty_lots == 0.
bool ParseDepthUpdate(std::string_view payload, const Instrument& inst, DepthUpdate& out,
                      std::vector<Event>& events, std::string* error);

// ---------------------------------------------------------------------------
// aggTrade
// ---------------------------------------------------------------------------
struct AggTrade {
  Ts exch_ts_us = 0;
  Seq agg_id = 0;
  Ticks price_ticks = 0;
  Lots qty_lots = 0;
  Side aggressor = Side::kBid;
};

// Binance sends `m` = "is the buyer the market maker".
//   m == true  -> the resting order was a BID, so the AGGRESSOR was SELLING.
//   m == false -> the resting order was an ASK, so the AGGRESSOR was BUYING.
// The binary schema stores the aggressor side, so this mapping is inverted from
// the flag.  It is the single most common place to introduce a sign error, and
// it decides which of our resting orders a trade can fill (§3.7).
bool ParseAggTrade(std::string_view payload, const Instrument& inst, AggTrade& out,
                   std::string* error);

// ---------------------------------------------------------------------------
// bookTicker
// ---------------------------------------------------------------------------
struct BookTicker {
  Seq update_id = 0;
  Ticks bid_ticks = 0;
  Ticks ask_ticks = 0;
  Lots bid_qty_lots = 0;
  Lots ask_qty_lots = 0;
};

bool ParseBookTicker(std::string_view payload, const Instrument& inst, BookTicker& out,
                     std::string* error);

// ---------------------------------------------------------------------------
// REST depth snapshot
// ---------------------------------------------------------------------------
struct DepthSnapshot {
  Seq last_update_id = 0;
  Ts exch_ts_us = 0;  // futures only; 0 when the payload carries no timestamp
  std::size_t bid_levels = 0;
  std::size_t ask_levels = 0;
};

// Appends kSnapshotBegin, one kSnapshotLevel per level, then kSnapshotEnd.
// `fallback_ts_us` is used when the payload has no timestamp of its own (spot
// REST responses do not carry one) -- pass the recorder's receive time.
bool ParseDepthSnapshot(std::string_view payload, const Instrument& inst, Ts fallback_ts_us,
                        DepthSnapshot& out, std::vector<Event>& events, std::string* error);

}  // namespace lob::binance
