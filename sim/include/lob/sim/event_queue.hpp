// The simulator's timestamped priority queue (master plan §4.5).
//
// Three kinds of thing happen in a run and they must interleave in a defined,
// reproducible order:
//
//   kMarket -- a recorded Event, visible to us at exch_ts + delta_in
//   kAction -- one of our order actions, effective at decision_ts + delta_out
//   kTimer  -- the strategy's periodic wake-up
//
// TIE-BREAK RULE (documented because it is a modelling choice, not a detail):
// at equal timestamps, MARKET events are processed BEFORE our actions.  That is
// the conservative choice -- the market gets to move against us first, so a
// cancel racing an adverse trade loses the race.  Ties within a kind are broken
// by insertion order, which makes the whole ordering total and deterministic.
#pragma once

#include <cstdint>
#include <queue>
#include <vector>

#include <lob/execution.hpp>
#include <lob/types.hpp>

namespace lob {

enum class SimEventKind : std::uint8_t {
  kMarket = 0,  // must sort first at equal timestamps
  kAction = 1,
  kTimer = 2,
};

enum class ActionKind : std::uint8_t {
  kNone = 0,
  kPlace,
  kCancel,
  kProbeExpiry,
};

struct SimEvent {
  Ts ts_us = 0;
  SimEventKind kind = SimEventKind::kMarket;
  std::uint64_t sequence = 0;  // insertion order; makes the ordering total

  // kMarket payload: index into the replay's event array.
  std::uint64_t market_index = 0;

  // kAction payload.
  ActionKind action = ActionKind::kNone;
  OrderId order_id = kNoOrder;
};

// Strict weak ordering for a MIN-heap: `Later` returns true when `a` should be
// popped after `b`.
struct SimEventLater {
  bool operator()(const SimEvent& a, const SimEvent& b) const {
    if (a.ts_us != b.ts_us) {
      return a.ts_us > b.ts_us;
    }
    const auto ka = static_cast<std::uint8_t>(a.kind);
    const auto kb = static_cast<std::uint8_t>(b.kind);
    if (ka != kb) {
      return ka > kb;  // kMarket(0) before kAction(1) before kTimer(2)
    }
    return a.sequence > b.sequence;
  }
};

class EventQueue {
 public:
  EventQueue() = default;

  void Push(SimEvent e) {
    e.sequence = next_sequence_++;
    heap_.push(e);
  }

  void PushMarket(Ts ts_us, std::uint64_t market_index) {
    SimEvent e;
    e.ts_us = ts_us;
    e.kind = SimEventKind::kMarket;
    e.market_index = market_index;
    Push(e);
  }

  void PushAction(Ts ts_us, ActionKind action, OrderId order_id) {
    SimEvent e;
    e.ts_us = ts_us;
    e.kind = SimEventKind::kAction;
    e.action = action;
    e.order_id = order_id;
    Push(e);
  }

  void PushTimer(Ts ts_us) {
    SimEvent e;
    e.ts_us = ts_us;
    e.kind = SimEventKind::kTimer;
    Push(e);
  }

  [[nodiscard]] bool empty() const { return heap_.empty(); }
  [[nodiscard]] std::size_t size() const { return heap_.size(); }
  [[nodiscard]] const SimEvent& top() const { return heap_.top(); }

  SimEvent Pop() {
    SimEvent e = heap_.top();
    heap_.pop();
    return e;
  }

  void Clear() {
    heap_ = Heap();
    next_sequence_ = 0;
  }

 private:
  using Heap = std::priority_queue<SimEvent, std::vector<SimEvent>, SimEventLater>;
  Heap heap_;
  std::uint64_t next_sequence_ = 0;
};

}  // namespace lob
