#include <lob/sim/gateway.hpp>

#include <algorithm>

namespace lob {

void OrderGateway::AddLive(OrderId id, Side side) { live(side).push_back(id); }

void OrderGateway::RemoveLive(OrderId id, Side side) {
  std::vector<OrderId>& v = live(side);
  const auto it = std::find(v.begin(), v.end(), id);
  if (it != v.end()) {
    v.erase(it);
  }
}

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
  AddLive(o.id, side);
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
  // Snapshotted because Cancel() can retire an order out of live_.
  std::vector<OrderId> ids;
  ids.reserve(live(Side::kBid).size() + live(Side::kAsk).size());
  ids.insert(ids.end(), live(Side::kBid).begin(), live(Side::kBid).end());
  ids.insert(ids.end(), live(Side::kAsk).begin(), live(Side::kAsk).end());
  for (const OrderId oid : ids) {
    Cancel(oid);
  }
}

void OrderGateway::CancelSide(Side side) {
  const std::vector<OrderId> ids = live(side);
  for (const OrderId oid : ids) {
    const Order* o = Find(oid);
    if (o != nullptr && !o->is_probe) {
      Cancel(oid);
    }
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
  const std::vector<OrderId>& index = live(side);
  out.reserve(index.size());
  for (const OrderId oid : index) {
    const auto it = orders_.find(oid);
    if (it == orders_.end() || it->second.terminal()) {
      continue;
    }
    if (it->second.is_probe && !include_probes) {
      continue;
    }
    out.push_back(oid);
  }
  return out;
}

Lots64 OrderGateway::LiveQty(Side side) const {
  Lots64 total = 0;
  for (const OrderId oid : live(side)) {
    const auto it = orders_.find(oid);
    if (it != orders_.end() && !it->second.terminal() && !it->second.is_probe) {
      total += it->second.remaining_qty;
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
  RemoveLive(id, o->side);
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
    // Terminal orders stay in `orders_` for the analytics, but must leave the
    // live index or every subsequent query pays for them forever.
    RemoveLive(id, o->side);
  } else if (o->state == OrderState::kResting) {
    o->state = OrderState::kPartiallyFilled;
  }
}

void OrderGateway::Retire(OrderId id) {
  const auto it = orders_.find(id);
  if (it != orders_.end()) {
    RemoveLive(id, it->second.side);
    orders_.erase(it);
  }
}

void OrderGateway::Clear() {
  orders_.clear();
  live_[0].clear();
  live_[1].clear();
  next_id_ = 0;
  placements_ = 0;
  cancels_ = 0;
}

}  // namespace lob
