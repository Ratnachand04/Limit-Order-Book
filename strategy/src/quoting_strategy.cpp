#include <lob/strategy/quoting_strategy.hpp>

#include <cmath>
#include <cstdlib>

namespace lob {

void QuotingStrategy::OnStart(const StrategyContext& ctx) {
  Strategy::OnStart(ctx);
  sigma_ = VolatilityEstimator(ctx.config->sigma_sample_us, ctx.config->sigma_window_us);
  flow_ = FlowImbalance(
      static_cast<Ts>(ctx.config->toxicity_window_s * static_cast<double>(kUsPerSecond)));
  bid_order_ = kNoOrder;
  ask_order_ = kNoOrder;
  bid_order_px_ = 0;
  ask_order_px_ = 0;
}

// ---------------------------------------------------------------------------
double QuotingStrategy::mid_price() const {
  if (book_ == nullptr || !book_->HasBothSides()) {
    return 0.0;
  }
  return book_->MidTicks() * ctx_.instrument->tick_size();
}

double QuotingStrategy::weighted_mid_price() const {
  if (book_ == nullptr || !book_->HasBothSides()) {
    return 0.0;
  }
  return book_->WeightedMidTicks() * ctx_.instrument->tick_size();
}

Lots64 QuotingStrategy::inventory() const {
  return ctx_.ledger != nullptr ? ctx_.ledger->inventory_lots() : 0;
}

bool QuotingStrategy::AtInventoryCap(Side side) const {
  const Lots64 cap = ctx_.config->q_max_lots;
  const Lots64 q = inventory();
  // Include what is already working: two resting bids at the cap would breach
  // it if both filled.  Counting them is the difference between a cap and a
  // suggestion.
  const Lots64 working = ctx_.gateway != nullptr ? ctx_.gateway->LiveQty(side) : 0;
  if (side == Side::kBid) {
    return q + working >= cap;
  }
  return q - working <= -cap;
}

// ---------------------------------------------------------------------------
void QuotingStrategy::OnBook(const BookView& book, const Clock& clock) {
  book_ = &book;
  if (book.HasBothSides()) {
    sigma_.Observe(clock.now_us(), book.MidTicks() * ctx_.instrument->tick_size());
    last_sigma_ticks_ = sigma_.sigma() / ctx_.instrument->tick_size();
  }
}

void QuotingStrategy::OnTrade(const TradeInfo& trade, const BookView& book, const Clock& clock) {
  book_ = &book;
  flow_.Observe(clock.now_us(), trade.aggressor, trade.qty_lots);
  OnTradeExtra(trade, book, clock);
}

void QuotingStrategy::OnFill(const Fill& fill, const Clock& /*clock*/) {
  // The gateway has already retired the order if it is fully filled; drop our
  // handle so the next reconcile re-quotes instead of thinking it is still live.
  const Order* o = ctx_.gateway != nullptr ? ctx_.gateway->Find(fill.order_id) : nullptr;
  if (o == nullptr || o->terminal()) {
    if (bid_order_ == fill.order_id) {
      bid_order_ = kNoOrder;
    }
    if (ask_order_ == fill.order_id) {
      ask_order_ = kNoOrder;
    }
  }
}

void QuotingStrategy::OnTimer(const BookView& book, const Clock& clock) {
  book_ = &book;
  ++diag_.timer_ticks;
  if (!book.HasBothSides()) {
    return;
  }
  const DesiredQuotes desired = ComputeQuotes(book, clock);
  Reconcile(desired, book, clock);
}

// ---------------------------------------------------------------------------
Ticks QuotingStrategy::ClampToSide(Side side, Ticks price_ticks, const BookView& book) {
  // A "quote" that crosses is an aggressive order.  These strategies are
  // passive by construction, so a crossing price is clamped back to one tick
  // inside the opposite best.
  if (side == Side::kBid) {
    const Ticks ba = book.BestAsk();
    if (HasAsk(ba) && price_ticks >= ba) {
      return ba - 1;
    }
    return price_ticks;
  }
  const Ticks bb = book.BestBid();
  if (HasBid(bb) && price_ticks <= bb) {
    return bb + 1;
  }
  return price_ticks;
}

Ticks QuotingStrategy::ApplyTickFloor(Side side, Ticks desired_ticks,
                                      const BookView& book) const {
  if (!ctx_.config->tick_floor_at_touch) {
    return ClampToSide(side, desired_ticks, book);
  }
  // §3.4: "quote bid = min(best_bid, round_to_tick(r - spread/2))".  A-S on a
  // tick-constrained instrument can call for quotes far behind the touch; the
  // floor keeps the placement at or better than the touch so the strategy is
  // actually in the market, while the model still supplies the skew and the
  // pull decision.
  if (side == Side::kBid) {
    const Ticks bb = book.BestBid();
    const Ticks px = HasBid(bb) ? (desired_ticks > bb ? bb : desired_ticks) : desired_ticks;
    return ClampToSide(side, px, book);
  }
  const Ticks ba = book.BestAsk();
  const Ticks px = HasAsk(ba) ? (desired_ticks < ba ? ba : desired_ticks) : desired_ticks;
  return ClampToSide(side, px, book);
}

bool QuotingStrategy::PassesMinEdge(Side side, Ticks price_ticks, const BookView& book) const {
  if (!ctx_.config->enforce_min_edge || ctx_.fees == nullptr) {
    return true;
  }
  if (!book.HasBothSides()) {
    return false;
  }
  const double tick = ctx_.instrument->tick_size();
  const double price = static_cast<double>(price_ticks) * tick;
  const double mid = book.MidTicks() * tick;
  // Edge on a benign fill: buy below the mid, sell above it.
  const double edge = (side == Side::kBid) ? (mid - price) : (price - mid);
  // Fee rate as a fraction of notional; tenths of a bp -> 1e-5 each.
  const double fee_rate = static_cast<double>(ctx_.fees->maker_tenth_bp) * 1.0e-5;
  const double fee_cost = fee_rate * price;
  return edge > fee_cost;
}

// ---------------------------------------------------------------------------
void QuotingStrategy::Reconcile(const DesiredQuotes& desired, const BookView& book,
                                const Clock& /*clock*/) {
  ReconcileSide(Side::kBid, desired.want_bid, desired.bid_ticks, book);
  ReconcileSide(Side::kAsk, desired.want_ask, desired.ask_ticks, book);
}

void QuotingStrategy::ReconcileSide(Side side, bool want, Ticks price_ticks,
                                    const BookView& book) {
  OrderId& handle = OrderFor(side);
  Ticks& handle_px = PriceFor(side);

  // Drop a stale handle: the order may have filled or been cancelled since.
  if (handle != kNoOrder) {
    const Order* o = ctx_.gateway->Find(handle);
    if (o == nullptr || o->terminal() || o->state == OrderState::kPendingCancel) {
      handle = kNoOrder;
    }
  }

  if (!want) {
    if (handle != kNoOrder) {
      ctx_.gateway->Cancel(handle);
      ++diag_.quotes_cancelled;
      handle = kNoOrder;
    }
    return;
  }

  if (AtInventoryCap(side)) {
    ++diag_.quotes_blocked_by_inventory_cap;
    if (handle != kNoOrder) {
      ctx_.gateway->Cancel(handle);
      ++diag_.quotes_cancelled;
      handle = kNoOrder;
    }
    return;
  }

  if (!PassesMinEdge(side, price_ticks, book)) {
    ++diag_.quotes_blocked_by_min_edge;
    if (handle != kNoOrder) {
      ctx_.gateway->Cancel(handle);
      ++diag_.quotes_cancelled;
      handle = kNoOrder;
    }
    return;
  }

  if (handle == kNoOrder) {
    handle = ctx_.gateway->Place(side, price_ticks, ctx_.config->order_size_lots);
    handle_px = price_ticks;
    ++diag_.quotes_placed;
    return;
  }

  if (handle_px == price_ticks) {
    return;  // already exactly where we want to be
  }

  // The min-move filter.  Repricing sends us to the BACK of the new queue, so a
  // one-tick improvement is often a strict loss; only move when the target has
  // drifted far enough to be worth the queue position.
  const Ticks move = static_cast<Ticks>(std::abs(static_cast<long>(price_ticks) -
                                                 static_cast<long>(handle_px)));
  if (move < ctx_.config->requote_min_ticks) {
    ++diag_.requotes_suppressed_by_min_move;
    return;
  }

  handle = ctx_.gateway->Replace(handle, side, price_ticks, ctx_.config->order_size_lots);
  handle_px = price_ticks;
  ++diag_.quotes_cancelled;
  ++diag_.quotes_placed;
}

}  // namespace lob
