#include <lob/converter/binance.hpp>

#include <lob/json.hpp>

namespace lob::binance {
namespace {

void SetError(std::string* error, std::string_view what, std::string_view detail = {}) {
  if (error != nullptr) {
    error->assign(what);
    if (!detail.empty()) {
      error->append(": ");
      error->append(detail);
    }
  }
}

constexpr Ts kMsToUs = 1000;

// Reads a price/quantity pair of the form ["7403.89", "0.002"].
//
// This is the innermost loop of the whole conversion stage -- a day of one
// symbol is tens of millions of these -- so it takes the integer path whenever
// the instrument's grid allows it, and only falls back to double parsing for
// instruments whose tick or lot size is not a power of ten.
bool ReadLevel(json::Reader& r, const Instrument& inst, Ticks& px, Lots& qty) {
  if (!r.EnterArray()) {
    return false;
  }

  if (inst.tick_is_pow10()) {
    std::int64_t ticks = 0;
    if (!r.NextElement() || !r.ReadFixedPoint(inst.tick_decimals(), ticks)) {
      return false;
    }
    px = static_cast<Ticks>(ticks);
  } else {
    double price = 0.0;
    if (!r.NextElement() || !r.ReadNumberLoose(price)) {
      return false;
    }
    px = inst.ToTicks(price);
  }

  if (inst.lot_is_pow10()) {
    std::int64_t lots = 0;
    if (!r.NextElement() || !r.ReadFixedPoint(inst.lot_decimals(), lots)) {
      return false;
    }
    qty = static_cast<Lots>(lots);
  } else {
    double quantity = 0.0;
    if (!r.NextElement() || !r.ReadNumberLoose(quantity)) {
      return false;
    }
    qty = inst.ToLots(quantity);
  }

  // Drain any extra elements (spot REST snapshots historically carried a third
  // ignored field); tolerate them rather than reject the file.
  std::string_view ignored;
  while (r.NextElement()) {
    if (!r.SkipValueSpan(ignored)) {
      return false;
    }
  }
  return true;
}

// Reads a ["p","q"] array of levels and appends one event each.
bool ReadLevelArray(json::Reader& r, const Instrument& inst, Side side, EventType type, Ts ts,
                    Seq seq, std::vector<Event>& events, std::size_t& count) {
  if (!r.EnterArray()) {
    return false;
  }
  while (r.NextElement()) {
    Ticks px = 0;
    Lots qty = 0;
    if (!ReadLevel(r, inst, px, qty)) {
      return false;
    }
    Event e;
    e.exch_ts_us = ts;
    e.seq = seq;
    e.price_ticks = px;
    e.qty_lots = qty;
    e.type = static_cast<std::uint8_t>(type);
    e.side = static_cast<std::uint8_t>(side);
    e.symbol_id = inst.symbol_id();
    events.push_back(e);
    ++count;
  }
  return r.ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// Channel names
// ---------------------------------------------------------------------------
Channel ChannelFromName(std::string_view name) {
  if (name == "depth" || name == "depthUpdate") {
    return Channel::kDepth;
  }
  if (name == "aggTrade" || name == "trade") {
    return Channel::kAggTrade;
  }
  if (name == "bookTicker") {
    return Channel::kBookTicker;
  }
  if (name == "snapshot" || name == "depthSnapshot") {
    return Channel::kSnapshot;
  }
  return Channel::kUnknown;
}

std::string_view ChannelName(Channel c) {
  switch (c) {
    case Channel::kDepth:
      return "depth";
    case Channel::kAggTrade:
      return "aggTrade";
    case Channel::kBookTicker:
      return "bookTicker";
    case Channel::kSnapshot:
      return "snapshot";
    case Channel::kUnknown:
      break;
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------
bool ParseRecorderLine(std::string_view line, RecorderLine& out, std::string* error) {
  out = RecorderLine{};
  json::Reader r(line);
  if (!r.EnterObject()) {
    SetError(error, "line is not a JSON object");
    return false;
  }
  std::string_view key;
  std::string_view ignored;
  bool have_payload = false;
  while (r.NextMember(key)) {
    if (key == "t") {
      if (!r.ReadIntLoose(out.recv_ts_us)) {
        SetError(error, "field 't' is not an integer");
        return false;
      }
    } else if (key == "c") {
      std::string_view name;
      if (!r.ReadStringRaw(name)) {
        SetError(error, "field 'c' is not a string");
        return false;
      }
      out.channel = ChannelFromName(name);
    } else if (key == "s") {
      if (!r.ReadStringRaw(out.symbol)) {
        SetError(error, "field 's' is not a string");
        return false;
      }
    } else if (key == "d") {
      // Bracket-matched: this only needs the payload's extent, and recursively
      // parsing it here is what made every message cost three parses.
      if (!r.SkipValueSpan(out.payload)) {
        SetError(error, "field 'd' is not a well-formed value");
        return false;
      }
      have_payload = true;
    } else if (!r.SkipValueSpan(ignored)) {
      SetError(error, "malformed value for key", key);
      return false;
    }
  }
  if (!r.ok()) {
    SetError(error, "malformed JSON", r.error());
    return false;
  }
  if (!have_payload) {
    SetError(error, "line has no 'd' payload");
    return false;
  }

  // Combined-stream form: {"stream": "...", "data": {...}}.  Descend so the
  // payload parsers always see the bare exchange object.
  //
  // Only the FIRST key is examined.  Binance puts "stream" first in the
  // combined-stream envelope, and this project's own recorder never emits the
  // wrapper at all, so the common path costs one key comparison instead of a
  // full walk of the payload.
  json::Reader probe(out.payload);
  if (probe.EnterObject() && probe.PeekNextKeyIs("stream")) {
    std::string_view k;
    while (probe.NextMember(k)) {
      if (k == "data") {
        std::string_view inner;
        if (probe.SkipValueSpan(inner)) {
          out.payload = inner;
        }
        break;
      }
      if (!probe.SkipValueSpan(k)) {
        break;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// depthUpdate
// ---------------------------------------------------------------------------
bool ParseDepthUpdate(std::string_view payload, const Instrument& inst, DepthUpdate& out,
                      std::vector<Event>& events, std::string* error) {
  out = DepthUpdate{};
  const std::size_t events_before = events.size();

  // Ids and timestamps can appear after the level arrays, so the levels are
  // staged into the output vector first and their ts/seq patched at the end.
  json::Reader r(payload);
  if (!r.EnterObject()) {
    SetError(error, "depthUpdate payload is not an object");
    return false;
  }

  Ts event_ts_ms = 0;
  Ts txn_ts_ms = 0;
  std::string_view key;
  while (r.NextMember(key)) {
    if (key == "E") {
      if (!r.ReadIntLoose(event_ts_ms)) {
        SetError(error, "depthUpdate 'E' is not an integer");
        return false;
      }
    } else if (key == "T") {
      if (!r.ReadIntLoose(txn_ts_ms)) {
        SetError(error, "depthUpdate 'T' is not an integer");
        return false;
      }
    } else if (key == "U") {
      if (!r.ReadIntLoose(out.first_id)) {
        SetError(error, "depthUpdate 'U' is not an integer");
        return false;
      }
    } else if (key == "u") {
      if (!r.ReadIntLoose(out.final_id)) {
        SetError(error, "depthUpdate 'u' is not an integer");
        return false;
      }
    } else if (key == "pu") {
      if (!r.ReadIntLoose(out.prev_final_id)) {
        SetError(error, "depthUpdate 'pu' is not an integer");
        return false;
      }
      out.has_prev_id = true;
    } else if (key == "b") {
      if (!ReadLevelArray(r, inst, Side::kBid, EventType::kDepth, 0, 0, events, out.bid_levels)) {
        SetError(error, "depthUpdate bid levels are malformed");
        events.resize(events_before);
        return false;
      }
    } else if (key == "a") {
      if (!ReadLevelArray(r, inst, Side::kAsk, EventType::kDepth, 0, 0, events, out.ask_levels)) {
        SetError(error, "depthUpdate ask levels are malformed");
        events.resize(events_before);
        return false;
      }
    } else if (!r.SkipValue()) {
      SetError(error, "depthUpdate has a malformed field", key);
      events.resize(events_before);
      return false;
    }
  }
  if (!r.ok()) {
    SetError(error, "depthUpdate JSON is malformed", r.error());
    events.resize(events_before);
    return false;
  }

  // Prefer the transaction time when the venue supplies one: on USD-M futures
  // `T` is the match-engine time and `E` is when the event was pushed out.
  // Using the push time would attribute market moves later than they happened.
  const Ts ts_ms = (txn_ts_ms > 0) ? txn_ts_ms : event_ts_ms;
  if (ts_ms <= 0) {
    SetError(error, "depthUpdate has no usable timestamp (neither 'T' nor 'E')");
    events.resize(events_before);
    return false;
  }
  out.exch_ts_us = ts_ms * kMsToUs;

  for (std::size_t i = events_before; i < events.size(); ++i) {
    events[i].exch_ts_us = out.exch_ts_us;
    events[i].seq = out.final_id;
  }
  return true;
}

// ---------------------------------------------------------------------------
// aggTrade
// ---------------------------------------------------------------------------
bool ParseAggTrade(std::string_view payload, const Instrument& inst, AggTrade& out,
                   std::string* error) {
  out = AggTrade{};
  json::Reader r(payload);
  if (!r.EnterObject()) {
    SetError(error, "aggTrade payload is not an object");
    return false;
  }

  Ts event_ts_ms = 0;
  Ts trade_ts_ms = 0;
  double price = 0.0;
  double qty = 0.0;
  bool have_price = false;
  bool have_qty = false;
  bool buyer_is_maker = false;
  bool have_maker_flag = false;

  std::string_view key;
  while (r.NextMember(key)) {
    if (key == "E") {
      if (!r.ReadIntLoose(event_ts_ms)) {
        SetError(error, "aggTrade 'E' is not an integer");
        return false;
      }
    } else if (key == "T") {
      if (!r.ReadIntLoose(trade_ts_ms)) {
        SetError(error, "aggTrade 'T' is not an integer");
        return false;
      }
    } else if (key == "a") {
      if (!r.ReadIntLoose(out.agg_id)) {
        SetError(error, "aggTrade 'a' is not an integer");
        return false;
      }
    } else if (key == "p") {
      if (!r.ReadNumberLoose(price)) {
        SetError(error, "aggTrade 'p' is not a number");
        return false;
      }
      have_price = true;
    } else if (key == "q") {
      if (!r.ReadNumberLoose(qty)) {
        SetError(error, "aggTrade 'q' is not a number");
        return false;
      }
      have_qty = true;
    } else if (key == "m") {
      if (!r.ReadBool(buyer_is_maker)) {
        SetError(error, "aggTrade 'm' is not a boolean");
        return false;
      }
      have_maker_flag = true;
    } else if (!r.SkipValue()) {
      SetError(error, "aggTrade has a malformed field", key);
      return false;
    }
  }
  if (!r.ok()) {
    SetError(error, "aggTrade JSON is malformed", r.error());
    return false;
  }
  if (!have_price || !have_qty) {
    SetError(error, "aggTrade is missing 'p' or 'q'");
    return false;
  }
  if (!have_maker_flag) {
    // Without the maker flag there is no aggressor side, and a trade whose
    // aggressor we cannot identify cannot fill a queue-tracked order (§3.7).
    // Dropping it silently would bias fills, so this is a hard error.
    SetError(error, "aggTrade is missing the 'm' (buyer-is-maker) flag");
    return false;
  }

  const Ts ts_ms = (trade_ts_ms > 0) ? trade_ts_ms : event_ts_ms;
  if (ts_ms <= 0) {
    SetError(error, "aggTrade has no usable timestamp");
    return false;
  }
  out.exch_ts_us = ts_ms * kMsToUs;
  out.price_ticks = inst.ToTicks(price);
  out.qty_lots = inst.ToLots(qty);
  // See the header: the flag names the MAKER, the schema stores the AGGRESSOR.
  out.aggressor = buyer_is_maker ? Side::kAsk : Side::kBid;
  return true;
}

// ---------------------------------------------------------------------------
// bookTicker
// ---------------------------------------------------------------------------
bool ParseBookTicker(std::string_view payload, const Instrument& inst, BookTicker& out,
                     std::string* error) {
  out = BookTicker{};
  json::Reader r(payload);
  if (!r.EnterObject()) {
    SetError(error, "bookTicker payload is not an object");
    return false;
  }
  double bid = 0.0;
  double ask = 0.0;
  double bid_qty = 0.0;
  double ask_qty = 0.0;
  std::string_view key;
  while (r.NextMember(key)) {
    if (key == "u") {
      if (!r.ReadIntLoose(out.update_id)) {
        return false;
      }
    } else if (key == "b") {
      if (!r.ReadNumberLoose(bid)) {
        return false;
      }
    } else if (key == "B") {
      if (!r.ReadNumberLoose(bid_qty)) {
        return false;
      }
    } else if (key == "a") {
      if (!r.ReadNumberLoose(ask)) {
        return false;
      }
    } else if (key == "A") {
      if (!r.ReadNumberLoose(ask_qty)) {
        return false;
      }
    } else if (!r.SkipValue()) {
      SetError(error, "bookTicker has a malformed field", key);
      return false;
    }
  }
  if (!r.ok()) {
    SetError(error, "bookTicker JSON is malformed", r.error());
    return false;
  }
  out.bid_ticks = inst.ToTicks(bid);
  out.ask_ticks = inst.ToTicks(ask);
  out.bid_qty_lots = inst.ToLots(bid_qty);
  out.ask_qty_lots = inst.ToLots(ask_qty);
  return true;
}

// ---------------------------------------------------------------------------
// REST depth snapshot
// ---------------------------------------------------------------------------
bool ParseDepthSnapshot(std::string_view payload, const Instrument& inst, Ts fallback_ts_us,
                        DepthSnapshot& out, std::vector<Event>& events, std::string* error) {
  out = DepthSnapshot{};
  const std::size_t events_before = events.size();

  json::Reader r(payload);
  if (!r.EnterObject()) {
    SetError(error, "snapshot payload is not an object");
    return false;
  }

  Ts event_ts_ms = 0;
  Ts txn_ts_ms = 0;
  std::string_view key;
  // Levels are staged with a placeholder ts/seq and patched below, because
  // lastUpdateId may follow the arrays in the JSON.
  while (r.NextMember(key)) {
    if (key == "lastUpdateId") {
      if (!r.ReadIntLoose(out.last_update_id)) {
        SetError(error, "snapshot 'lastUpdateId' is not an integer");
        events.resize(events_before);
        return false;
      }
    } else if (key == "E") {
      if (!r.ReadIntLoose(event_ts_ms)) {
        events.resize(events_before);
        return false;
      }
    } else if (key == "T") {
      if (!r.ReadIntLoose(txn_ts_ms)) {
        events.resize(events_before);
        return false;
      }
    } else if (key == "bids") {
      if (!ReadLevelArray(r, inst, Side::kBid, EventType::kSnapshotLevel, 0, 0, events,
                          out.bid_levels)) {
        SetError(error, "snapshot bids are malformed");
        events.resize(events_before);
        return false;
      }
    } else if (key == "asks") {
      if (!ReadLevelArray(r, inst, Side::kAsk, EventType::kSnapshotLevel, 0, 0, events,
                          out.ask_levels)) {
        SetError(error, "snapshot asks are malformed");
        events.resize(events_before);
        return false;
      }
    } else if (!r.SkipValue()) {
      SetError(error, "snapshot has a malformed field", key);
      events.resize(events_before);
      return false;
    }
  }
  if (!r.ok()) {
    SetError(error, "snapshot JSON is malformed", r.error());
    events.resize(events_before);
    return false;
  }
  if (out.last_update_id <= 0) {
    SetError(error, "snapshot has no lastUpdateId -- it cannot anchor a resync");
    events.resize(events_before);
    return false;
  }

  const Ts ts_ms = (txn_ts_ms > 0) ? txn_ts_ms : event_ts_ms;
  out.exch_ts_us = (ts_ms > 0) ? ts_ms * kMsToUs : fallback_ts_us;

  for (std::size_t i = events_before; i < events.size(); ++i) {
    events[i].exch_ts_us = out.exch_ts_us;
    events[i].seq = out.last_update_id;
  }

  // Bracket the levels with the begin/end markers the book core keys off.
  events.insert(events.begin() + static_cast<std::ptrdiff_t>(events_before),
                MakeSnapshotMarker(EventType::kSnapshotBegin, out.exch_ts_us, out.last_update_id,
                                   inst.symbol_id()));
  events.push_back(MakeSnapshotMarker(EventType::kSnapshotEnd, out.exch_ts_us, out.last_update_id,
                                      inst.symbol_id()));
  return true;
}

}  // namespace lob::binance
