#include <lob/sim/queue_tracker.hpp>

#include <algorithm>
#include <cmath>

namespace lob {

QueueTracker::QueueTracker(QueueModel model, Rng& rng, Ts attribution_window_us)
    : model_(model), rng_(&rng), attribution_window_us_(attribution_window_us) {}

void QueueTracker::Clear() {
  orders_.clear();
  levels_.clear();
  stats_ = QueueTrackerStats{};
}

// ---------------------------------------------------------------------------
// Placement / removal
// ---------------------------------------------------------------------------
void QueueTracker::Place(OrderId id, Side side, Ticks price_ticks, Lots64 qty,
                         Lots64 visible_level_qty, Ts ts_us) {
  QueueState q;
  q.side = side;
  q.price_ticks = price_ticks;
  q.remaining = qty;
  // §3.7: price-time priority puts a new order at the BACK of the queue, so
  // everything currently visible at the level is ahead of it.
  q.ahead = visible_level_qty > 0 ? visible_level_qty : 0;
  q.behind = 0;
  orders_[id] = q;

  LevelInfo& level = levels_[LevelKey(side, price_ticks)];
  ExpireCredit(level, ts_us);
  level.last_visible_qty = q.ahead;
  level.orders.push_back(id);
  ++stats_.placements;
}

void QueueTracker::DetachOrder(OrderId id, Side side, Ticks price_ticks) {
  const auto it = levels_.find(LevelKey(side, price_ticks));
  if (it == levels_.end()) {
    return;
  }
  std::vector<OrderId>& v = it->second.orders;
  v.erase(std::remove(v.begin(), v.end(), id), v.end());
  if (v.empty()) {
    levels_.erase(it);
  }
}

void QueueTracker::Remove(OrderId id) {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return;
  }
  DetachOrder(id, it->second.side, it->second.price_ticks);
  orders_.erase(it);
}

const QueueState* QueueTracker::State(OrderId id) const {
  const auto it = orders_.find(id);
  return it == orders_.end() ? nullptr : &it->second;
}

void QueueTracker::Emit(OrderId id, const QueueState& q, Lots64 qty, FillCause cause) {
  switch (cause) {
    case FillCause::kQueueConsumed:
      ++stats_.fills_queue_consumed;
      break;
    case FillCause::kTradedThrough:
      ++stats_.fills_traded_through;
      break;
    case FillCause::kCrossed:
      ++stats_.fills_crossed;
      break;
  }
  if (!sink_) {
    return;
  }
  QueueFill f;
  f.order_id = id;
  f.side = q.side;
  f.price_ticks = q.price_ticks;
  f.qty_lots = qty;
  f.cause = cause;
  f.state_after = q;
  sink_(f);
}

// ---------------------------------------------------------------------------
// Queue arithmetic
// ---------------------------------------------------------------------------
void QueueTracker::ApplyIncrease(QueueState& q, Lots64 added) {
  // The one unambiguous rule of §3.7: arrivals are behind us.
  q.behind += added;
}

void QueueTracker::ApplyCancel(QueueState& q, Lots64 cancelled) {
  if (cancelled <= 0) {
    return;
  }
  switch (model_) {
    case QueueModel::kPess: {
      // Cancels come from behind first, so our position in the queue improves
      // as slowly as possible.  This is the lower bound on fills and the model
      // every headline number in the paper uses (§3.7).
      const Lots64 from_behind = std::min(cancelled, q.behind);
      q.behind -= from_behind;
      const Lots64 overflow = cancelled - from_behind;
      q.ahead = std::max<Lots64>(0, q.ahead - overflow);
      break;
    }
    case QueueModel::kOpt: {
      // Cancels come from ahead first: our position improves as fast as
      // possible.  Upper bound on fills.
      const Lots64 from_ahead = std::min(cancelled, q.ahead);
      q.ahead -= from_ahead;
      const Lots64 overflow = cancelled - from_ahead;
      q.behind = std::max<Lots64>(0, q.behind - overflow);
      break;
    }
    case QueueModel::kProp: {
      // Uniform over the queue.  The exact split A * (1 - dC/(A+B)) is
      // fractional; rather than make queue state floating point (which would
      // cost determinism across standard libraries, §4.1), the integer split is
      // randomly rounded from the seeded RNG.  Unbiased in expectation, exactly
      // reproducible in practice.
      const Lots64 total = q.ahead + q.behind;
      if (total <= 0) {
        break;
      }
      const Lots64 effective = std::min(cancelled, total);
      // Done entirely in integers: `effective * A / total` with the remainder
      // decided by one RNG draw.  Computing the share in floating point first
      // would make an exactly-divisible split (say 15 * 30/45) land on
      // 9.999999999999998 and depend on a coin flip for the right answer.
      const Lots64 numerator = effective * q.ahead;
      Lots64 from_ahead = numerator / total;
      const Lots64 remainder = numerator % total;
      if (remainder > 0 && rng_->UniformInt(0, total - 1) < remainder) {
        ++from_ahead;
      }
      from_ahead = std::clamp<Lots64>(from_ahead, 0, std::min(q.ahead, effective));
      Lots64 from_behind = effective - from_ahead;
      if (from_behind > q.behind) {
        // Behind cannot absorb its share; the excess must come from ahead.
        from_ahead = std::min<Lots64>(q.ahead, from_ahead + (from_behind - q.behind));
        from_behind = q.behind;
      }
      q.ahead -= from_ahead;
      q.behind -= from_behind;
      break;
    }
  }
  if (q.ahead < 0) {
    q.ahead = 0;
  }
  if (q.behind < 0) {
    q.behind = 0;
  }
}

// ---------------------------------------------------------------------------
// Trades
// ---------------------------------------------------------------------------
void QueueTracker::OnTrade(Side aggressor, Ticks price_ticks, Lots64 qty, Ts ts_us) {
  if (qty <= 0) {
    return;
  }
  // A sell-aggressor (kAsk) consumes the BID queue and vice versa.  Getting
  // this inversion wrong is the single most common sign error in the project.
  const Side consumed_side = Opposite(aggressor);
  ++stats_.trades_seen;

  const auto lit = levels_.find(LevelKey(consumed_side, price_ticks));
  if (lit == levels_.end()) {
    return;  // we have nothing resting there
  }
  LevelInfo& level = lit->second;
  ExpireCredit(level, ts_us);
  // Bank the traded quantity so the level decrease this trade will cause is not
  // later mistaken for a cancellation.
  level.trade_credit += qty;
  level.credit_ts_us = ts_us;

  // Each of our orders is an independent shadow that does not deplete real
  // liquidity, so every one of them sees the full trade quantity.  This is the
  // shadow-order limitation stated prominently in §2.5 / Part 11 #11.
  //
  // The ids are snapshotted because Emit() calls the fill sink, which can erase
  // from orders_ and levels_ before we get back here.  The buffer is a reused
  // member so the snapshot costs no allocation.
  scratch_ids_.assign(level.orders.begin(), level.orders.end());
  for (const OrderId id : scratch_ids_) {
    const auto oit = orders_.find(id);
    if (oit == orders_.end()) {
      continue;
    }
    QueueState& q = oit->second;
    if (q.remaining <= 0) {
      continue;
    }
    const Lots64 ahead_before = q.ahead;
    const Lots64 remaining_before = q.remaining;

    // fill = clamp(v - A, 0, remaining)
    const Lots64 reaches_us = qty - ahead_before;
    const Lots64 fill = std::clamp<Lots64>(reaches_us, 0, remaining_before);

    q.ahead = std::max<Lots64>(0, ahead_before - qty);
    if (fill > 0) {
      q.remaining -= fill;
      // Anything left after eating A and all of us comes out of B.
      const Lots64 past_us = qty - ahead_before - remaining_before;
      if (past_us > 0) {
        q.behind = std::max<Lots64>(0, q.behind - past_us);
      }
      // "Traded through" means the print did not stop at us -- it carried on
      // into the queue behind.  That is the aggressive, sweeping flow whose
      // fills are the most adversely selected (§2.3), so it is worth telling
      // apart from an ordinary fill that merely happened to complete us.
      Emit(id, q, fill, past_us > 0 ? FillCause::kTradedThrough : FillCause::kQueueConsumed);
    }
  }
}

// ---------------------------------------------------------------------------
// Level updates
// ---------------------------------------------------------------------------
void QueueTracker::ExpireCredit(LevelInfo& level, Ts ts_us) {
  if (level.trade_credit > 0 && ts_us - level.credit_ts_us > attribution_window_us_) {
    // The trade never showed up as a level decrease.  Part 11 pitfall #5: this
    // residual is a measurable property of the data, so it is counted rather
    // than quietly dropped.
    stats_.lots_trade_credit_expired += level.trade_credit;
    level.trade_credit = 0;
  }
}

void QueueTracker::OnLevelUpdate(Side side, Ticks price_ticks, Lots64 new_visible_qty, Ts ts_us) {
  const auto lit = levels_.find(LevelKey(side, price_ticks));
  if (lit == levels_.end()) {
    return;
  }
  LevelInfo& level = lit->second;
  ExpireCredit(level, ts_us);

  const Lots64 old_qty = level.last_visible_qty;
  const Lots64 fresh = std::max<Lots64>(0, new_visible_qty);
  level.last_visible_qty = fresh;
  if (fresh == old_qty) {
    return;
  }

  if (fresh > old_qty) {
    const Lots64 added = fresh - old_qty;
    ++stats_.level_increases;
    for (const OrderId id : level.orders) {
      const auto oit = orders_.find(id);
      if (oit != orders_.end()) {
        ApplyIncrease(oit->second, added);
      }
    }
    return;
  }

  // Decrease: trades first, then whatever is left is a cancellation.
  const Lots64 decrease = old_qty - fresh;
  ++stats_.level_decreases;
  const Lots64 explained = std::min(decrease, level.trade_credit);
  level.trade_credit -= explained;
  stats_.lots_explained_by_trades += explained;
  const Lots64 cancelled = decrease - explained;
  if (cancelled <= 0) {
    return;
  }
  stats_.lots_attributed_to_cancels += cancelled;
  for (const OrderId id : level.orders) {
    const auto oit = orders_.find(id);
    if (oit != orders_.end()) {
      ApplyCancel(oit->second, cancelled);
    }
  }
}

// ---------------------------------------------------------------------------
// Cross / trade-through
// ---------------------------------------------------------------------------
void QueueTracker::OnBook(Ticks best_bid, Ticks best_ask, Ts /*ts_us*/) {
  // This runs on every market event of the replay, and in the overwhelming
  // majority of them nothing is crossed, so the no-op path must cost nothing.
  if (orders_.empty()) {
    return;
  }
  scratch_ids_.clear();
  for (const auto& [id, q] : orders_) {
    if (q.remaining <= 0) {
      continue;
    }
    // §3.7: "if the opposite best crosses your price ... you fill (at your
    // limit price)".  A resting bid at or above the best ask would have been
    // matched by the exchange; a shadow order that survives this is a fiction
    // that flatters fill rates.
    if (q.side == Side::kBid && HasAsk(best_ask) && best_ask <= q.price_ticks) {
      scratch_ids_.push_back(id);
    } else if (q.side == Side::kAsk && HasBid(best_bid) && best_bid >= q.price_ticks) {
      scratch_ids_.push_back(id);
    }
  }
  for (const OrderId id : scratch_ids_) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
      continue;
    }
    QueueState& q = it->second;
    const Lots64 fill = q.remaining;
    q.remaining = 0;
    q.ahead = 0;
    Emit(id, q, fill, FillCause::kCrossed);
  }
}

}  // namespace lob
