#include <lob/sim/gateway.hpp>

namespace lob {

OrderId OrderGateway::Place(Side side, Ticks price_ticks, Lots64 qty) {
  if (qty <= 0) {
    return kNoOrder;
  }
  Order o;
  o.id = NewId();
  o.side = side;
  o.price_ticks = price_ticks;
  o.original_qty = qty;
  o.remaining_qty = qty;
  o.state = OrderState::kPendingNew;
  o.decided_ts_us = clock_->now_us();
  o.effective_ts_us = latency_->EffectiveAt(o.decided_ts_us);
  orders_[o.id] = o;
  queue_->PushAction(o.effective_ts_us, ActionKind::kPlace, o.id);
  ++placements_;
  return o.id;
}

OrderId OrderGateway::PlaceProbe(Side side, Ticks price_ticks, Lots64 qty, Ticks depth_ticks,
                                 Ts expiry_us) {
  const OrderId id = Place(side, price_ticks, qty);
  if (id == kNoOrder) {
    return kNoOrder;
  }
  Order& o = orders_[id];
  o.is_probe = true;
  o.probe_depth_ticks = depth_ticks;
  o.probe_expiry_ts_us = expiry_us;
  queue_->PushAction(expiry_us, ActionKind::kProbeExpiry, id);
  return id;
}

bool OrderGateway::Cancel(OrderId id) {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return false;
  }
  Order& o = it->second;
  if (o.terminal() || o.state == OrderState::kPendingCancel) {
    return false;
  }
  o.cancel_decided_ts_us = clock_->now_us();
  o.cancel_effective_ts_us = latency_->EffectiveAt(o.cancel_decided_ts_us);
  // A cancel issued while the order is still PENDING_NEW cannot land before the
  // placement it is cancelling; the exchange would reject it (§4.6).
  if (o.state == OrderState::kPendingNew && o.cancel_effective_ts_us < o.effective_ts_us) {
    o.cancel_effective_ts_us = o.effective_ts_us;
  }
  // The order is not marked PENDING_CANCEL until it is actually RESTING;
  // otherwise a fill arriving before the ack would look like a fill on a
  // cancelled order.
  if (o.state == OrderState::kResting || o.state == OrderState::kPartiallyFilled) {
    o.state = OrderState::kPendingCancel;
  }
  queue_->PushAction(o.cancel_effective_ts_us, ActionKind::kCancel, id);
  ++cancels_;
  return true;
}

OrderId OrderGateway::Replace(OrderId id, Side side, Ticks price_ticks, Lots64 qty) {
  Cancel(id);
  return Place(side, price_ticks, qty);
}

void OrderGateway::CancelAll() {
  std::vector<OrderId> ids;
  ids.reserve(orders_.size());
  for (const auto& [oid, o] : orders_) {
    if (!o.terminal()) {
      ids.push_back(oid);
    }
  }
  for (const OrderId oid : ids) {
    Cancel(oid);
  }
}

void OrderGateway::CancelSide(Side side) {
  std::vector<OrderId> ids;
  for (const auto& [oid, o] : orders_) {
    if (!o.terminal() && o.side == side && !o.is_probe) {
      ids.push_back(oid);
    }
  }
  for (const OrderId oid : ids) {
    Cancel(oid);
  }
}

const Order* OrderGateway::Find(OrderId id) const {
  const auto it = orders_.find(id);
  return it == orders_.end() ? nullptr : &it->second;
}

Order* OrderGateway::FindMutable(OrderId id) {
  const auto it = orders_.find(id);
  return it == orders_.end() ? nullptr : &it->second;
}

std::vector<OrderId> OrderGateway::LiveOrders(Side side, bool include_probes) const {
  std::vector<OrderId> out;
  for (const auto& [oid, o] : orders_) {
    if (o.side != side || o.terminal()) {
      continue;
    }
    if (o.is_probe && !include_probes) {
      continue;
    }
    out.push_back(oid);
  }
  return out;
}

Lots64 OrderGateway::LiveQty(Side side) const {
  Lots64 total = 0;
  for (const auto& [oid, o] : orders_) {
    if (o.side == side && !o.terminal() && !o.is_probe) {
      total += o.remaining_qty;
    }
  }
  return total;
}

Order* OrderGateway::ActivatePlace(OrderId id) {
  Order* o = FindMutable(id);
  if (o == nullptr || o->state != OrderState::kPendingNew) {
    return nullptr;
  }
  o->state = OrderState::kResting;
  o->resting_ts_us = clock_->now_us();
  return o;
}

Order* OrderGateway::ActivateCancel(OrderId id) {
  Order* o = FindMutable(id);
  if (o == nullptr || o->terminal()) {
    return nullptr;
  }
  o->state = OrderState::kCanceled;
  o->terminal_ts_us = clock_->now_us();
  return o;
}

void OrderGateway::MarkFilled(OrderId id, Lots64 qty, Ts ts_us) {
  Order* o = FindMutable(id);
  if (o == nullptr) {
    return;
  }
  o->filled_qty += qty;
  o->remaining_qty -= qty;
  if (o->remaining_qty <= 0) {
    o->remaining_qty = 0;
    o->state = OrderState::kFilled;
    o->terminal_ts_us = ts_us;
  } else if (o->state == OrderState::kResting) {
    o->state = OrderState::kPartiallyFilled;
  }
}

void OrderGateway::Retire(OrderId id) { orders_.erase(id); }

void OrderGateway::Clear() {
  orders_.clear();
  next_id_ = 0;
  placements_ = 0;
  cancels_ = 0;
}

}  // namespace lob
