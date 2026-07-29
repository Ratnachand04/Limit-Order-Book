#include <lob/sim/simulator.hpp>

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include <lob/book/dense_book.hpp>
#include <lob/book/dual_book.hpp>
#include <lob/book/map_book.hpp>
#include <lob/converter/event_io.hpp>

namespace lob {

FillModel ParseFillModel(std::string_view s) {
  if (s == "TOUCH" || s == "touch") {
    return FillModel::kTouchRule;
  }
  return FillModel::kQueueAware;
}

std::string_view FillModelName(FillModel m) {
  return m == FillModel::kTouchRule ? "TOUCH" : "QUEUE";
}

std::string SimulatorStats::Report() const {
  std::ostringstream os;
  os << "market_events            " << market_events << "\n"
     << "  depth                  " << depth_events << "\n"
     << "  trade                  " << trade_events << "\n"
     << "  snapshot               " << snapshot_events << "\n"
     << "  dirty (excluded)       " << dirty_events << "\n"
     << "timer_ticks              " << timer_ticks << "\n"
     << "actions                  " << actions << "\n"
     << "fills                    " << fills << "\n"
     << "probe_fills              " << probe_fills << "\n"
     << "forced_liquidations      " << forced_liquidations << "\n"
     << "book_integrity_failures  " << book_integrity_failures << "\n"
     << "dual_book_mismatches     " << dual_book_mismatches << "\n"
     << "first_local_ts_us        " << first_local_ts_us << "\n"
     << "last_local_ts_us         " << last_local_ts_us << "\n";
  return os.str();
}

// ---------------------------------------------------------------------------
Simulator::Simulator(RunConfig config, Strategy& strategy)
    : config_(std::move(config)),
      strategy_(&strategy),
      rng_(config_.seed),
      latency_(config_.latency, rng_),
      queue_(config_.queue_model, rng_),
      gateway_(events_, latency_, clock_),
      ledger_(config_.PrimaryInstrument(), config_.fees),
      markouts_(config_.PrimaryInstrument(), config_.markouts.horizons_us),
      probes_(config_.probes) {
  fill_model_ = ParseFillModel(config_.fill_model);
  set_book_kind(config_.dual_book_check ? BookKind::kDual : BookKind::kDense);
  queue_.set_fill_sink([this](const QueueFill& qf) { OnQueueFill(qf); });
}

Simulator::~Simulator() = default;

void Simulator::set_book_kind(BookKind kind) {
  book_kind_ = kind;
  switch (kind) {
    case BookKind::kMap:
      book_ = std::make_unique<MapBook>();
      break;
    case BookKind::kDual: {
      auto dual = std::make_unique<DualBook>(
          [this](const std::string&) { ++stats_.dual_book_mismatches; });
      book_ = std::move(dual);
      break;
    }
    case BookKind::kDense:
    default:
      book_ = std::make_unique<DenseBook>();
      break;
  }
}

Lots64 Simulator::VisibleQtyAt(Side side, Ticks price_ticks) const {
  return book_->QtyAt(side, price_ticks);
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------
const SimulatorStats& Simulator::RunFile(const std::string& binary_path) {
  const std::vector<Event> events = ReadAllEvents(binary_path);
  return Run(events);
}

const SimulatorStats& Simulator::Run(const std::vector<Event>& events) {
  stats_ = SimulatorStats{};
  fills_.clear();
  fill_counter_ = 0;
  since_integrity_check_ = 0;
  last_visible_ts_us_ = 0;
  have_mid_ = false;
  have_first_ts_ = false;
  last_probe_bucket_ = -1;
  last_marked_mid_x2_ = 0;
  next_market_index_ = 0;
  market_ = &events;

  rng_.Reset();
  clock_.Reset();
  events_.Clear();
  gateway_.Clear();
  queue_.Clear();
  ledger_.Reset();
  markouts_.Reset();
  probes_.Clear();
  book_->Clear();

  StrategyContext ctx;
  ctx.gateway = &gateway_;
  ctx.book = book_.get();
  ctx.clock = &clock_;
  ctx.instrument = &config_.PrimaryInstrument();
  ctx.config = &config_.strategy;
  ctx.fees = &config_.fees;
  ctx.ledger = &ledger_;
  ctx.rng = &rng_;
  strategy_->OnStart(ctx);

  if (events.empty()) {
    return stats_;
  }

  // Schedule the first market event; each subsequent one is scheduled as its
  // predecessor is popped, so the heap only ever holds a handful of entries.
  auto schedule_next_market = [this]() {
    if (market_ == nullptr || next_market_index_ >= market_->size()) {
      return;
    }
    const Event& e = (*market_)[next_market_index_];
    Ts visible = latency_.VisibleAt(e.exch_ts_us);
    // A market-data feed is a serial stream: it cannot reorder.  Independent
    // jitter draws could otherwise make event i+1 visible before event i, which
    // would let the book apply diffs out of sequence.  Clamping to be monotone
    // keeps the jitter model honest without breaking the stream.
    visible = std::max(visible, last_visible_ts_us_);
    last_visible_ts_us_ = visible;
    events_.PushMarket(visible, next_market_index_);
    ++next_market_index_;
  };
  schedule_next_market();

  bool first_timer_scheduled = false;

  while (!events_.empty()) {
    const SimEvent se = events_.Pop();
    clock_.AdvanceTo(se.ts_us);
    if (!have_first_ts_) {
      have_first_ts_ = true;
      stats_.first_local_ts_us = se.ts_us;
    }
    stats_.last_local_ts_us = se.ts_us;

    switch (se.kind) {
      case SimEventKind::kMarket: {
        HandleMarketEvent((*market_)[se.market_index]);
        schedule_next_market();
        if (!first_timer_scheduled) {
          events_.PushTimer(clock_.now_us() + config_.strategy.timer_us);
          first_timer_scheduled = true;
        }
        break;
      }
      case SimEventKind::kAction:
        HandleAction(se);
        break;
      case SimEventKind::kTimer:
        HandleTimer();
        // Stop the metronome once the data is exhausted, otherwise the loop
        // would tick forever on an empty book.
        if (next_market_index_ < market_->size()) {
          events_.PushTimer(clock_.now_us() + config_.strategy.timer_us);
        }
        break;
    }
  }

  // Final mark so late markout samples resolve at the last known mid.
  if (book_->HasBothSides()) {
    markouts_.Advance(clock_.now_us(), book_->MidX2Ticks());
  }
  strategy_->OnEnd();
  return stats_;
}

// ---------------------------------------------------------------------------
void Simulator::HandleMarketEvent(const Event& e) {
  clock_.SetExchangeTs(e.exch_ts_us);
  current_event_dirty_ = e.IsDirty();
  ++stats_.market_events;
  if (current_event_dirty_) {
    ++stats_.dirty_events;
  }

  switch (e.Type()) {
    case EventType::kTrade: {
      ++stats_.trade_events;
      // Trades are applied BEFORE the depth update that reflects them (the
      // converter guarantees that ordering), so a level decrease is only
      // treated as a cancellation once the trades explaining it are known.
      if (fill_model_ == FillModel::kQueueAware) {
        queue_.OnTrade(e.SideOf(), e.price_ticks, e.qty_lots, clock_.now_us());
      } else {
        ApplyTouchRule(e);
      }
      RemarkAndNotify();
      TradeInfo t;
      t.exch_ts_us = e.exch_ts_us;
      t.aggressor = e.SideOf();
      t.price_ticks = e.price_ticks;
      t.qty_lots = e.qty_lots;
      strategy_->OnTrade(t, *book_, clock_);
      return;
    }
    case EventType::kDepth: {
      ++stats_.depth_events;
      book_->ApplyDepth(e.SideOf(), e.price_ticks, e.qty_lots);
      queue_.OnLevelUpdate(e.SideOf(), e.price_ticks, e.qty_lots, clock_.now_us());
      break;
    }
    case EventType::kSnapshotBegin:
      ++stats_.snapshot_events;
      book_->ApplySnapshotBegin();
      break;
    case EventType::kSnapshotLevel:
      book_->ApplySnapshotLevel(e.SideOf(), e.price_ticks, e.qty_lots);
      break;
    case EventType::kSnapshotEnd:
      book_->ApplySnapshotEnd();
      break;
  }

  // §3.7 "other fill triggers": the opposite best reaching our price.
  if (fill_model_ == FillModel::kQueueAware) {
    queue_.OnBook(book_->BestBid(), book_->BestAsk(), clock_.now_us());
  }

  if (integrity_every_ > 0 && ++since_integrity_check_ >= integrity_every_) {
    since_integrity_check_ = 0;
    const BookIntegrity ok = book_->CheckInvariants();
    if (!ok) {
      ++stats_.book_integrity_failures;
    }
  }

  RemarkAndNotify();
  strategy_->OnBook(*book_, clock_);
}

void Simulator::RemarkAndNotify() {
  if (!book_->HasBothSides()) {
    return;
  }
  const std::int64_t mid_x2 = book_->MidX2Ticks();
  // Always advance the markout sampler, even when the mid is unchanged: a
  // sample can come due during a quiet stretch and must resolve at the mid that
  // was actually in force, not at whatever the next move happens to be.
  markouts_.Advance(clock_.now_us(), mid_x2);

  if (have_mid_ && mid_x2 == last_marked_mid_x2_) {
    return;
  }
  have_mid_ = true;
  last_marked_mid_x2_ = mid_x2;
  ledger_.MarkTo(mid_x2);
  if (recorders_ != nullptr) {
    recorders_->ObserveLedger(clock_.now_us(), ledger_, current_event_dirty_);
  }
  EnforceInventoryCap();
}

// ---------------------------------------------------------------------------
void Simulator::HandleAction(const SimEvent& se) {
  ++stats_.actions;
  switch (se.action) {
    case ActionKind::kPlace: {
      Order* o = gateway_.ActivatePlace(se.order_id);
      if (o == nullptr) {
        return;
      }
      const Lots64 visible = VisibleQtyAt(o->side, o->price_ticks);
      o->ahead_at_placement = visible;
      queue_.Place(o->id, o->side, o->price_ticks, o->remaining_qty, visible, clock_.now_us());
      if (o->is_probe) {
        const QueueState* qs = queue_.State(o->id);
        probes_.Register(o->id, clock_.now_us(), o->side, o->probe_depth_ticks, o->price_ticks,
                         o->remaining_qty, qs != nullptr ? qs->QueueFraction() : 0.0,
                         book_->TopImbalance(), strategy_->last_sigma_ticks_per_sqrt_s());
      } else if (recorders_ != nullptr) {
        recorders_->NoteQuotePlaced(clock_.now_us());
      }
      // An order that arrives already crossing the opposite side would have
      // been matched immediately by the exchange.
      queue_.OnBook(book_->BestBid(), book_->BestAsk(), clock_.now_us());
      return;
    }
    case ActionKind::kCancel: {
      const Order* before = gateway_.Find(se.order_id);
      if (before == nullptr || before->terminal()) {
        return;
      }
      queue_.Remove(se.order_id);
      Order* o = gateway_.ActivateCancel(se.order_id);
      if (o != nullptr && o->is_probe) {
        probes_.Retire(o->id);
      }
      return;
    }
    case ActionKind::kProbeExpiry: {
      ProbeRecord* r = probes_.Find(se.order_id);
      if (r == nullptr) {
        return;
      }
      const Order* o = gateway_.Find(se.order_id);
      if (o != nullptr && !o->terminal()) {
        queue_.Remove(se.order_id);
        gateway_.ActivateCancel(se.order_id);
      }
      if (recorders_ != nullptr) {
        // One row per horizon: "filled within h" is the RQ2 observation.
        for (const Ts h : probes_.config().horizons_us) {
          const bool filled_by_h = r->filled && (r->fill_ts_us - r->placed_ts_us) <= h;
          recorders_->RecordProbe(r->probe_id, r->placed_ts_us, r->side, r->depth_ticks,
                                  r->initial_queue_fraction, r->imbalance, r->sigma_ticks, h,
                                  filled_by_h, r->fill_ts_us);
        }
      }
      probes_.Retire(se.order_id);
      return;
    }
    case ActionKind::kNone:
    default:
      return;
  }
}

void Simulator::HandleTimer() {
  ++stats_.timer_ticks;
  strategy_->OnTimer(*book_, clock_);

  if (!probes_.enabled() || !book_->HasBothSides()) {
    return;
  }
  const Ts interval = probes_.config().interval_us;
  if (interval <= 0) {
    return;
  }
  // §3.8 spawns probes on their own cadence, independent of the strategy's
  // timer.  Bucketing by wall position rather than counting ticks keeps the
  // schedule stable when the timer period changes across the experiment grid.
  const std::int64_t bucket = clock_.now_us() / interval;
  if (bucket == last_probe_bucket_) {
    return;
  }
  last_probe_bucket_ = bucket;

  const Ts expiry = clock_.now_us() + probes_.MaxHorizonUs();
  for (const Ticks depth : probes_.config().depths_ticks) {
    // depth == 1 means "at the touch"; deeper probes step away from it.
    const Ticks bid_px = book_->BestBid() - (depth - 1);
    const Ticks ask_px = book_->BestAsk() + (depth - 1);
    gateway_.PlaceProbe(Side::kBid, bid_px, probes_.config().size_lots, depth, expiry);
    gateway_.PlaceProbe(Side::kAsk, ask_px, probes_.config().size_lots, depth, expiry);
  }
}

// ---------------------------------------------------------------------------
void Simulator::OnQueueFill(const QueueFill& qf) {
  Order* o = gateway_.FindMutable(qf.order_id);
  if (o == nullptr) {
    return;
  }
  gateway_.MarkFilled(qf.order_id, qf.qty_lots, clock_.now_us());

  if (o->is_probe) {
    // Probes are pure measurement: no cash, no inventory, no markouts.
    probes_.NoteFill(qf.order_id, clock_.now_us());
    ++stats_.probe_fills;
    if (o->terminal()) {
      queue_.Remove(qf.order_id);
    }
    return;
  }
  BookFill(qf.order_id, qf.side, qf.price_ticks, qf.qty_lots, qf.cause);
  if (o->terminal()) {
    queue_.Remove(qf.order_id);
  }
}

void Simulator::BookFill(OrderId id, Side side, Ticks price_ticks, Lots64 qty, FillCause cause) {
  const std::int64_t mid_x2 = book_->HasBothSides()
                                  ? book_->MidX2Ticks()
                                  : 2 * static_cast<std::int64_t>(price_ticks);

  Fill f;
  f.order_id = id;
  f.ts_us = clock_.now_us();
  f.exch_ts_us = clock_.exchange_ts_us();
  f.side = side;
  f.price_ticks = price_ticks;
  f.qty_lots = qty;
  f.mid_x2_ticks_at_fill = mid_x2;
  f.cause = cause;
  const Order* o = gateway_.Find(id);
  if (o != nullptr) {
    f.ahead_at_placement = o->ahead_at_placement;
  }
  const QueueState* qs = queue_.State(id);
  f.queue_ahead_at_fill = qs != nullptr ? qs->ahead : 0;

  ledger_.ApplyFill(side, price_ticks, qty, mid_x2, /*maker=*/true);
  const std::uint64_t fill_id = markouts_.AddFill(f.ts_us, side, price_ticks, qty, mid_x2);
  fills_.push_back(f);
  ++fill_counter_;
  ++stats_.fills;

  if (recorders_ != nullptr) {
    // Dirty intervals are replayed so the book stays live, but they are not
    // evidence: they are excluded from the analytics (§4.4).
    if (!current_event_dirty_) {
      QueueState state = qs != nullptr ? *qs : QueueState{};
      recorders_->RecordFill(fill_id, f, state, cause, book_->TopImbalance(),
                             strategy_->last_sigma_ticks_per_sqrt_s());
    }
    recorders_->ObserveLedger(clock_.now_us(), ledger_, current_event_dirty_);
  }
  // A fill is the only thing that can breach the inventory cap, so check it
  // here rather than waiting for the next mid move.
  EnforceInventoryCap();
  strategy_->OnFill(f, clock_);
}

// ---------------------------------------------------------------------------
void Simulator::ApplyTouchRule(const Event& trade) {
  // The fiction RQ1 measures (master plan Part 1, Part 5): any trade printed
  // at or through our limit price fills us, with no regard for how much
  // quantity was ahead of us in the queue.
  const Side our_side = Opposite(trade.SideOf());
  std::vector<OrderId> ids = gateway_.LiveOrders(our_side);
  for (const OrderId id : ids) {
    Order* o = gateway_.FindMutable(id);
    if (o == nullptr || !o->live() || o->state == OrderState::kPendingNew) {
      continue;
    }
    const bool touched = (our_side == Side::kBid) ? (trade.price_ticks <= o->price_ticks)
                                                  : (trade.price_ticks >= o->price_ticks);
    if (!touched) {
      continue;
    }
    const Lots64 qty = std::min<Lots64>(o->remaining_qty, trade.qty_lots);
    if (qty <= 0) {
      continue;
    }
    gateway_.MarkFilled(id, qty, clock_.now_us());
    BookFill(id, our_side, o->price_ticks, qty, FillCause::kQueueConsumed);
  }
}

// ---------------------------------------------------------------------------
void Simulator::EnforceInventoryCap() {
  const Lots64 cap = config_.strategy.q_max_lots;
  const Lots64 q = ledger_.inventory_lots();
  if (cap <= 0 || (q <= cap && q >= -cap)) {
    return;
  }
  if (!book_->HasBothSides()) {
    return;
  }
  // Part 6 risk control: beyond the hard cap the position is flattened back to
  // the cap by crossing the spread, paying the TAKER fee.  This is the cost of
  // breaching the limit and it must show up in the PnL, not be waved away.
  const Lots64 excess = q > cap ? (q - cap) : (q + cap);
  if (excess == 0) {
    return;
  }
  const bool selling = q > cap;
  const Side our_side = selling ? Side::kAsk : Side::kBid;
  const Ticks px = selling ? book_->BestBid() : book_->BestAsk();
  const Lots64 qty = selling ? excess : -excess;
  if (qty <= 0) {
    return;
  }
  const std::int64_t mid_x2 = book_->MidX2Ticks();
  ledger_.ApplyFill(our_side, px, qty, mid_x2, /*maker=*/false);
  ++stats_.forced_liquidations;
}

}  // namespace lob
