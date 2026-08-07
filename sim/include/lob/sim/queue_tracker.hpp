// Queue-position tracking (master plan §3.7) -- the heart of the simulator.
//
// Our order is a SHADOW: it rests at a price level whose visible L2 quantity
// Q = A + B excludes us.  `A` is the quantity ahead of us, `B` behind.  With L2
// data we can see that a level shrank but not WHICH orders left, so the cancel
// location has to be assumed, and the honest response is to run all three
// assumptions and report the bracket (§3.7, Part 12 "which queue assumption is
// right?").
//
// Rules, in the order they are applied:
//
//   Placement          A <- Q (the visible quantity), B <- 0.  Price-time
//                      priority puts us at the back of the queue.
//   Level increase +dN B <- B + dN.  The one unambiguous rule: anything that
//                      arrives after us is behind us.
//   Trade of size v    Trades consume the FRONT:
//                        fill = clamp(v - A, 0, remaining)
//                        A    <- max(0, A - v)
//                        B    <- B - max(0, v - A_prev - remaining_prev)
//   Level decrease dC  Whatever a trade does not explain is a cancellation:
//     not explained      PESS  B first, overflow eats A   -> lower bound fills
//     by trades          OPT   A first, overflow eats B   -> upper bound fills
//                        PROP  uniform over the queue     -> central estimate
//   Cross / through    If the opposite best reaches our price, or the level is
//                      consumed entirely, the remainder fills at our limit.
//
// PROP would need fractional quantities to be exact.  Rather than switch the
// queue state to floating point (which would break determinism across
// platforms and contradict §4.1), the split is randomly rounded from the single
// seeded RNG: unbiased in expectation and exactly reproducible.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include <lob/config.hpp>
#include <lob/execution.hpp>
#include <lob/rng.hpp>
#include <lob/types.hpp>

namespace lob {

// QueueState, FillCause, QueueFill and QueueTrackerStats are declared in
// <lob/execution.hpp> so the analytics layer can describe a fill without
// depending on the engine that produced it.
using QueueFillSink = std::function<void(const QueueFill&)>;

class QueueTracker {
 public:
  // `attribution_window_us` is the "small time window" of §3.7 within which a
  // trade print may still explain a level decrease.  Binance batches depth at
  // 100 ms, so the default is deliberately a little wider than one batch.
  QueueTracker(QueueModel model, Rng& rng, Ts attribution_window_us = 250 * kUsPerMilli);

  void set_fill_sink(QueueFillSink sink) { sink_ = std::move(sink); }

  // --- order lifecycle -----------------------------------------------------
  // `visible_level_qty` is Q at the instant the order reaches the exchange.
  void Place(OrderId id, Side side, Ticks price_ticks, Lots64 qty, Lots64 visible_level_qty,
             Ts ts_us);
  void Remove(OrderId id);
  [[nodiscard]] const QueueState* State(OrderId id) const;
  [[nodiscard]] std::size_t tracked_orders() const { return orders_.size(); }

  // --- market events -------------------------------------------------------
  // A trade printed at `price_ticks` with the given aggressor.  Only trades
  // whose aggressor is opposite our side can consume our queue.
  void OnTrade(Side aggressor, Ticks price_ticks, Lots64 qty, Ts ts_us);

  // The visible quantity at (side, price_ticks) is now `new_visible_qty`.
  // Must be called for EVERY depth update at a level we are resting on, and
  // before the book itself is updated is not required -- the tracker keeps its
  // own view of the level.
  void OnLevelUpdate(Side side, Ticks price_ticks, Lots64 new_visible_qty, Ts ts_us);

  // Called after the book has been updated.  Fills any order the opposite side
  // has reached (§3.7 "other fill triggers").
  void OnBook(Ticks best_bid, Ticks best_ask, Ts ts_us);

  [[nodiscard]] const QueueTrackerStats& stats() const { return stats_; }
  [[nodiscard]] QueueModel model() const { return model_; }
  void Clear();

 private:
  struct LevelInfo {
    Lots64 last_visible_qty = 0;
    // Trade quantity printed at this level that has not yet been matched to a
    // level decrease.  Expires after the attribution window.
    Lots64 trade_credit = 0;
    Ts credit_ts_us = 0;
    std::vector<OrderId> orders;
  };

  static std::int64_t LevelKey(Side side, Ticks price_ticks) {
    return (static_cast<std::int64_t>(side) << 32) |
           static_cast<std::int64_t>(static_cast<std::uint32_t>(price_ticks));
  }

  void ApplyCancel(QueueState& q, Lots64 cancelled);
  void ApplyIncrease(QueueState& q, Lots64 added);
  void Emit(OrderId id, const QueueState& q, Lots64 qty, FillCause cause);
  void ExpireCredit(LevelInfo& level, Ts ts_us);
  void DetachOrder(OrderId id, Side side, Ticks price_ticks);

  QueueModel model_;
  Rng* rng_;
  Ts attribution_window_us_;
  QueueFillSink sink_;
  std::map<OrderId, QueueState> orders_;
  std::map<std::int64_t, LevelInfo> levels_;
  QueueTrackerStats stats_;

  // Reused across calls so the hot paths never allocate (§4.10 "avoiding
  // allocation in the hot loop").  OnBook runs on EVERY market event, so a
  // vector constructed there was one malloc/free per event for the whole
  // replay.  Both uses need a snapshot of the ids, because the fill sink can
  // re-enter and erase from orders_ while we are iterating.
  std::vector<OrderId> scratch_ids_;
};

}  // namespace lob
