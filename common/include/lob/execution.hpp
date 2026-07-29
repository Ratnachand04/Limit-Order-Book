// Execution value types: orders, fills and queue state.
//
// These live in `common` rather than in `sim` for a structural reason: the
// analytics layer must be able to describe a fill without depending on the
// engine that produced it, and the engine must be able to book a fill into the
// ledger.  Putting the plain data in the middle keeps that a straight line
//
//     common  <-  analytics  <-  sim  <-  strategy
//
// instead of a cycle.
//
// Order lifecycle (master plan §4.6):
//
//   PENDING_NEW -> RESTING -> PARTIALLY_FILLED -> FILLED
//                          \-> PENDING_CANCEL   -> CANCELED
//
// The state machine exists because latency makes intent and reality diverge:
// between deciding to cancel and the cancel arriving, the order is still live
// and can still fill.  A backtest that treats cancels as instantaneous quietly
// removes exactly the fills that hurt most.
#pragma once

#include <cstdint>
#include <string_view>

#include <lob/types.hpp>

namespace lob {

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------
enum class OrderState : std::uint8_t {
  kPendingNew = 0,
  kResting = 1,
  kPartiallyFilled = 2,
  kFilled = 3,
  kPendingCancel = 4,
  kCanceled = 5,
  kRejected = 6,
};

std::string_view OrderStateName(OrderState s);

// True while the order can still be hit by an incoming trade.  A PENDING_CANCEL
// order is deliberately included: the cancel has not arrived yet.
inline constexpr bool IsLive(OrderState s) {
  return s == OrderState::kResting || s == OrderState::kPartiallyFilled ||
         s == OrderState::kPendingCancel;
}

inline constexpr bool IsTerminal(OrderState s) {
  return s == OrderState::kFilled || s == OrderState::kCanceled || s == OrderState::kRejected;
}

using OrderId = std::uint64_t;
inline constexpr OrderId kNoOrder = 0;

struct Order {
  OrderId id = kNoOrder;
  Side side = Side::kBid;
  Ticks price_ticks = 0;
  Lots64 original_qty = 0;
  Lots64 remaining_qty = 0;
  Lots64 filled_qty = 0;
  OrderState state = OrderState::kPendingNew;

  Ts decided_ts_us = 0;    // when the strategy asked for it (local clock)
  Ts effective_ts_us = 0;  // when it reaches / reached the exchange
  Ts resting_ts_us = 0;    // when it joined the queue
  Ts terminal_ts_us = 0;

  // Queue state at placement, retained so fill probability can be regressed on
  // initial queue position (§3.8).
  Lots64 ahead_at_placement = 0;
  Lots64 behind_at_placement = 0;

  // Set when a cancel has been decided but has not yet reached the exchange.
  Ts cancel_decided_ts_us = 0;
  Ts cancel_effective_ts_us = 0;

  // Probe orders are shadow orders used only to estimate fill probability
  // (§3.8); they never touch the ledger.
  bool is_probe = false;
  Ticks probe_depth_ticks = 0;
  Ts probe_expiry_ts_us = 0;

  [[nodiscard]] bool live() const { return IsLive(state); }
  [[nodiscard]] bool terminal() const { return IsTerminal(state); }
};

// ---------------------------------------------------------------------------
// Queue state (master plan §3.7)
// ---------------------------------------------------------------------------
struct QueueState {
  Lots64 ahead = 0;      // A -- visible quantity in front of us
  Lots64 behind = 0;     // B -- visible quantity behind us
  Lots64 remaining = 0;  // our own unfilled quantity
  Side side = Side::kBid;
  Ticks price_ticks = 0;

  // A / (A + remaining + B).  Its value at placement is the "queue fraction"
  // feature of the fill-probability model (§3.8).
  [[nodiscard]] double QueueFraction() const {
    const Lots64 total = ahead + remaining + behind;
    if (total <= 0) {
      return 0.0;
    }
    return static_cast<double>(ahead) / static_cast<double>(total);
  }
};

// Why an order filled.  Reported so the analytics can separate ordinary queue
// consumption from the "level swept" path, which is far more toxic (§2.3).
enum class FillCause : std::uint8_t {
  kQueueConsumed = 0,  // a trade ate through A and reached us
  kTradedThrough = 1,  // the print carried on past us into the queue behind
  kCrossed = 2,        // the opposite best reached our price
};

std::string_view FillCauseName(FillCause c);

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------
struct Fill {
  OrderId order_id = kNoOrder;
  Ts ts_us = 0;            // local time at which the fill is known to us
  Ts exch_ts_us = 0;       // exchange time of the trade that caused it
  Side side = Side::kBid;  // OUR side: kBid means we bought
  Ticks price_ticks = 0;
  Lots64 qty_lots = 0;

  // Book state at the fill, kept for analytics (§4.8 fills.csv).
  std::int64_t mid_x2_ticks_at_fill = 0;
  Lots64 ahead_at_placement = 0;
  Lots64 queue_ahead_at_fill = 0;
  FillCause cause = FillCause::kQueueConsumed;
  bool is_probe = false;
};

// Emitted by the queue tracker when an order is (partially) executed.
struct QueueFill {
  OrderId order_id = kNoOrder;
  Side side = Side::kBid;
  Ticks price_ticks = 0;
  Lots64 qty_lots = 0;
  FillCause cause = FillCause::kQueueConsumed;
  QueueState state_after;
};

// Counters that quantify how much of the queue model is assumption rather than
// data.  Part 11 pitfall #5 asks for exactly this: "measure the residual rate
// and report it".
struct QueueTrackerStats {
  std::uint64_t placements = 0;
  std::uint64_t level_increases = 0;
  std::uint64_t level_decreases = 0;
  std::uint64_t trades_seen = 0;
  Lots64 lots_explained_by_trades = 0;
  Lots64 lots_attributed_to_cancels = 0;
  // Trade quantity that never showed up as a level decrease inside the
  // attribution window -- the misattribution residual.
  Lots64 lots_trade_credit_expired = 0;
  std::uint64_t fills_queue_consumed = 0;
  std::uint64_t fills_traded_through = 0;
  std::uint64_t fills_crossed = 0;
};

}  // namespace lob
