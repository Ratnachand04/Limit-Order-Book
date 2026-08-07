// The order gateway (master plan §4.5, §4.6).
//
// Every action a strategy takes goes through here, and every action is delayed
// by delta_out before it reaches the exchange.  That delay is the entire point:
// a cancel decided at t is not a cancel at t, and during the gap the order can
// still fill.  Backtests that skip this step get to dodge exactly the fills
// that lose the most money.
//
// A cancel or replace issued while an order is still PENDING_NEW is queued
// until the ack lands, rather than being applied to an order the exchange has
// not seen yet (§4.6).
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <lob/execution.hpp>
#include <lob/sim/clock.hpp>
#include <lob/sim/event_queue.hpp>
#include <lob/sim/latency.hpp>
#include <lob/types.hpp>

namespace lob {

class OrderGateway {
 public:
  OrderGateway(EventQueue& queue, LatencyModel& latency, Clock& clock)
      : queue_(&queue), latency_(&latency), clock_(&clock) {}

  // --- strategy-facing API -------------------------------------------------
  // Returns the id immediately; the order becomes RESTING at decision + delta_out.
  OrderId Place(Side side, Ticks price_ticks, Lots64 qty);

  // Probe orders (§3.8) never touch the ledger and expire on their own.
  OrderId PlaceProbe(Side side, Ticks price_ticks, Lots64 qty, Ticks depth_ticks, Ts expiry_us);

  // Requests a cancel.  Returns false if the order is unknown or already
  // terminal.  The order stays live until the cancel lands.
  bool Cancel(OrderId id);

  // Cancel + place.  The replacement joins the BACK of the new queue, which is
  // the cost of repricing that S1's min-move filter exists to control (§4.6,
  // Part 11 pitfall #6).
  OrderId Replace(OrderId id, Side side, Ticks price_ticks, Lots64 qty);

  void CancelAll();
  void CancelSide(Side side);

  // --- queries -------------------------------------------------------------
  [[nodiscard]] const Order* Find(OrderId id) const;
  [[nodiscard]] Order* FindMutable(OrderId id);
  [[nodiscard]] const std::map<OrderId, Order>& orders() const { return orders_; }
  // Live, non-probe orders on a side.  Strategies use this to decide whether
  // they already have the quote they want.
  [[nodiscard]] std::vector<OrderId> LiveOrders(Side side, bool include_probes = false) const;
  [[nodiscard]] Lots64 LiveQty(Side side) const;

  // --- engine-facing API ---------------------------------------------------
  // Called by the Simulator when a kPlace / kCancel action event fires.
  Order* ActivatePlace(OrderId id);
  Order* ActivateCancel(OrderId id);
  void MarkFilled(OrderId id, Lots64 qty, Ts ts_us);
  void Retire(OrderId id);
  void Clear();

  [[nodiscard]] std::uint64_t placements() const { return placements_; }
  [[nodiscard]] std::uint64_t cancels() const { return cancels_; }

 private:
  OrderId NewId() { return ++next_id_; }

  // `orders_` keeps every order ever placed, because the analytics and the
  // tests need to look up a filled order's placement details long after it went
  // terminal.  That makes it a bad thing to iterate: over a day of quoting it
  // grows to tens of thousands of entries.
  //
  // `live_` is the index that keeps the hot queries honest.  AtInventoryCap()
  // calls LiveQty() twice on every strategy timer tick, and the touch-rule fill
  // model calls LiveOrders() on every trade; scanning `orders_` there made
  // replay cost O(orders placed so far) per query, i.e. quadratic in the length
  // of the run.  Live orders number a handful, so a flat vector beats any node
  // structure here.
  void AddLive(OrderId id, Side side);
  void RemoveLive(OrderId id, Side side);
  [[nodiscard]] std::vector<OrderId>& live(Side side) {
    return live_[side == Side::kBid ? 0 : 1];
  }
  [[nodiscard]] const std::vector<OrderId>& live(Side side) const {
    return live_[side == Side::kBid ? 0 : 1];
  }

  EventQueue* queue_;
  LatencyModel* latency_;
  Clock* clock_;
  std::map<OrderId, Order> orders_;
  std::vector<OrderId> live_[2];
  OrderId next_id_ = 0;
  std::uint64_t placements_ = 0;
  std::uint64_t cancels_ = 0;
};

}  // namespace lob
