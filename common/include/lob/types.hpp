// Core value types for the simulator.
//
// Design rule (master plan §4.1): prices and quantities are INTEGERS everywhere
// past the parser.  Floating point is never a container key and never touches
// cash accounting -- 0.1 + 0.2 problems corrupt price levels (pitfall #3,
// master plan Part 11).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace lob {

// ---------------------------------------------------------------------------
// Scalar aliases
// ---------------------------------------------------------------------------
using Ts = std::int64_t;      // microseconds since the Unix epoch (exchange clock)
using Seq = std::int64_t;     // exchange update id, used for continuity checks
using Ticks = std::int32_t;   // price, in integer ticks: llround(price / tick_size)
using Lots = std::int32_t;    // quantity, in integer lots: llround(qty / lot_size)
using Lots64 = std::int64_t;  // accumulators over Lots (level totals, queue state)

// Cash is an integer count of 1e-8 currency units (CLAUDE.md tech constraints).
// At 1e-8 granularity an int64 spans +/- 9.2e10 units of quote currency, which
// is many orders of magnitude beyond anything this simulator books.
using Cash = std::int64_t;
inline constexpr std::int64_t kCashScale = 100'000'000;  // 1e-8 units per unit

inline constexpr Ts kUsPerSecond = 1'000'000;
inline constexpr Ts kUsPerMilli = 1'000;

// ---------------------------------------------------------------------------
// Side
// ---------------------------------------------------------------------------
enum class Side : std::uint8_t { kBid = 0, kAsk = 1 };

inline constexpr Side Opposite(Side s) { return s == Side::kBid ? Side::kAsk : Side::kBid; }

// +1 for the bid side (buying), -1 for the ask side (selling).  This is the
// `s_i` of the PnL decomposition in master plan §3.9.
inline constexpr int SignOf(Side s) { return s == Side::kBid ? +1 : -1; }

inline constexpr std::string_view SideName(Side s) { return s == Side::kBid ? "BID" : "ASK"; }

// ---------------------------------------------------------------------------
// Event types (binary schema, master plan §4.3)
// ---------------------------------------------------------------------------
enum class EventType : std::uint8_t {
  kDepth = 0,          // new ABSOLUTE qty at a level; qty_lots == 0 means delete
  kTrade = 1,          // `side` carries the AGGRESSOR side
  kSnapshotBegin = 2,  // start of a REST depth snapshot; seq == lastUpdateId
  kSnapshotLevel = 3,  // one level of the snapshot in progress
  kSnapshotEnd = 4,    // end of snapshot; the book is now authoritative
};

inline constexpr std::string_view EventTypeName(EventType t) {
  switch (t) {
    case EventType::kDepth:
      return "DEPTH";
    case EventType::kTrade:
      return "TRADE";
    case EventType::kSnapshotBegin:
      return "SNAPSHOT_BEGIN";
    case EventType::kSnapshotLevel:
      return "SNAPSHOT_LEVEL";
    case EventType::kSnapshotEnd:
      return "SNAPSHOT_END";
  }
  return "?";
}

// Bit flags for Event::flags.
namespace flags {
inline constexpr std::uint8_t kNone = 0;
// bit0 -- this event falls in an interval following a detected sequence gap.
// Dirty intervals are replayed (the book must stay live) but are excluded from
// analytics (master plan §4.4).
inline constexpr std::uint8_t kDirty = 1u << 0;
// bit1 -- the converter re-anchored the book from a REST snapshot here.
inline constexpr std::uint8_t kResync = 1u << 1;
}  // namespace flags

// ---------------------------------------------------------------------------
// Event -- the on-disk record.  Fixed 32 bytes, little-endian.
// ---------------------------------------------------------------------------
//
// NOTE on the schema: both CLAUDE.md and master plan §4.3 state a *fixed
// 32-byte record*, but the field list in §4.3 ends with `int64_t _pad`, which
// would make the struct 36 bytes (40 padded).  The stated record size governs,
// so the trailing reserved field is int32_t here and the struct is exactly 32
// bytes with natural alignment.  See docs/ARCHITECTURE.md for the layout table.
struct Event {
  Ts exch_ts_us = 0;    // offset  0  exchange timestamp
  Seq seq = 0;          // offset  8  update id (continuity checks)
  Ticks price_ticks = 0;  // offset 16
  Lots qty_lots = 0;      // offset 20  DEPTH: new absolute qty at level, 0 = delete
  std::uint8_t type = 0;       // offset 24  EventType
  std::uint8_t side = 0;       // offset 25  Side; for TRADE the aggressor side
  std::uint8_t symbol_id = 0;  // offset 26
  std::uint8_t flags = 0;      // offset 27  see namespace flags
  std::int32_t reserved = 0;   // offset 28  zero-filled; keeps the record at 32 bytes

  [[nodiscard]] constexpr EventType Type() const { return static_cast<EventType>(type); }
  [[nodiscard]] constexpr Side SideOf() const { return static_cast<Side>(side); }
  [[nodiscard]] constexpr bool IsDirty() const { return (flags & flags::kDirty) != 0; }

  // For a TRADE, `side` is the aggressor.  A sell-aggressor (kAsk) consumes the
  // BID queue, so it is the event that can fill our resting bid (§3.7).
  [[nodiscard]] constexpr Side ConsumedSide() const { return Opposite(SideOf()); }

  friend constexpr bool operator==(const Event& a, const Event& b) {
    return a.exch_ts_us == b.exch_ts_us && a.seq == b.seq && a.price_ticks == b.price_ticks &&
           a.qty_lots == b.qty_lots && a.type == b.type && a.side == b.side &&
           a.symbol_id == b.symbol_id && a.flags == b.flags && a.reserved == b.reserved;
  }
};

inline constexpr std::size_t kEventSize = 32;
static_assert(sizeof(Event) == kEventSize, "Event must be exactly 32 bytes (master plan §4.3)");
static_assert(alignof(Event) == 8, "Event alignment must be 8");
static_assert(offsetof(Event, exch_ts_us) == 0);
static_assert(offsetof(Event, seq) == 8);
static_assert(offsetof(Event, price_ticks) == 16);
static_assert(offsetof(Event, qty_lots) == 20);
static_assert(offsetof(Event, type) == 24);
static_assert(offsetof(Event, side) == 25);
static_assert(offsetof(Event, symbol_id) == 26);
static_assert(offsetof(Event, flags) == 27);
static_assert(offsetof(Event, reserved) == 28);

// The reader and writer memcpy whole records, which is only valid on a
// little-endian host.  Every target this project supports is little-endian; the
// assert exists so a big-endian port fails loudly instead of silently.
static_assert(std::endian::native == std::endian::little,
              "Event I/O assumes a little-endian host");

// ---------------------------------------------------------------------------
// Convenience constructors
// ---------------------------------------------------------------------------
inline Event MakeDepth(Ts ts, Seq seq, Side side, Ticks px, Lots new_qty,
                       std::uint8_t symbol_id = 0, std::uint8_t f = flags::kNone) {
  Event e;
  e.exch_ts_us = ts;
  e.seq = seq;
  e.price_ticks = px;
  e.qty_lots = new_qty;
  e.type = static_cast<std::uint8_t>(EventType::kDepth);
  e.side = static_cast<std::uint8_t>(side);
  e.symbol_id = symbol_id;
  e.flags = f;
  return e;
}

// `aggressor` is the side that crossed the spread.  kAsk means a sell-aggressor
// (it hit the bid); kBid means a buy-aggressor (it lifted the offer).
inline Event MakeTrade(Ts ts, Seq seq, Side aggressor, Ticks px, Lots qty,
                       std::uint8_t symbol_id = 0, std::uint8_t f = flags::kNone) {
  Event e;
  e.exch_ts_us = ts;
  e.seq = seq;
  e.price_ticks = px;
  e.qty_lots = qty;
  e.type = static_cast<std::uint8_t>(EventType::kTrade);
  e.side = static_cast<std::uint8_t>(aggressor);
  e.symbol_id = symbol_id;
  e.flags = f;
  return e;
}

inline Event MakeSnapshotMarker(EventType t, Ts ts, Seq last_update_id,
                                std::uint8_t symbol_id = 0, std::uint8_t f = flags::kNone) {
  Event e;
  e.exch_ts_us = ts;
  e.seq = last_update_id;
  e.type = static_cast<std::uint8_t>(t);
  e.symbol_id = symbol_id;
  e.flags = f;
  return e;
}

inline Event MakeSnapshotLevel(Ts ts, Seq last_update_id, Side side, Ticks px, Lots qty,
                               std::uint8_t symbol_id = 0, std::uint8_t f = flags::kNone) {
  Event e;
  e.exch_ts_us = ts;
  e.seq = last_update_id;
  e.price_ticks = px;
  e.qty_lots = qty;
  e.type = static_cast<std::uint8_t>(EventType::kSnapshotLevel);
  e.side = static_cast<std::uint8_t>(side);
  e.symbol_id = symbol_id;
  e.flags = f;
  return e;
}

// ---------------------------------------------------------------------------
// Sentinels
// ---------------------------------------------------------------------------
// An empty side reports these, chosen so that `best_bid < best_ask` stays true
// on an empty book and so arithmetic on them saturates rather than wraps.
inline constexpr Ticks kNoBid = std::numeric_limits<Ticks>::min() / 2;
inline constexpr Ticks kNoAsk = std::numeric_limits<Ticks>::max() / 2;

inline constexpr bool HasBid(Ticks b) { return b != kNoBid; }
inline constexpr bool HasAsk(Ticks a) { return a != kNoAsk; }

}  // namespace lob
