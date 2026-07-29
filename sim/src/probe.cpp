#include <lob/sim/probe.hpp>

#include <algorithm>

namespace lob {

Ts ProbeEngine::MaxHorizonUs() const {
  if (config_.horizons_us.empty()) {
    return 0;
  }
  return *std::max_element(config_.horizons_us.begin(), config_.horizons_us.end());
}

std::uint64_t ProbeEngine::Register(OrderId order_id, Ts placed_ts_us, Side side,
                                    Ticks depth_ticks, Ticks price_ticks, Lots64 size_lots,
                                    double initial_queue_fraction, double imbalance,
                                    double sigma_ticks) {
  ProbeRecord r;
  r.probe_id = placed_;
  r.order_id = order_id;
  r.placed_ts_us = placed_ts_us;
  r.side = side;
  r.depth_ticks = depth_ticks;
  r.price_ticks = price_ticks;
  r.size_lots = size_lots;
  r.initial_queue_fraction = initial_queue_fraction;
  r.imbalance = imbalance;
  r.sigma_ticks = sigma_ticks;

  by_order_[order_id] = records_.size();
  records_.push_back(r);
  ++placed_;
  return r.probe_id;
}

void ProbeEngine::NoteFill(OrderId order_id, Ts fill_ts_us) {
  ProbeRecord* r = Find(order_id);
  if (r == nullptr || r->filled) {
    return;
  }
  r->filled = true;
  r->fill_ts_us = fill_ts_us;
  ++filled_;
}

ProbeRecord* ProbeEngine::Find(OrderId order_id) {
  const auto it = by_order_.find(order_id);
  if (it == by_order_.end()) {
    return nullptr;
  }
  return &records_[it->second];
}

void ProbeEngine::Retire(OrderId order_id) {
  ProbeRecord* r = Find(order_id);
  if (r != nullptr) {
    r->retired = true;
  }
  by_order_.erase(order_id);
}

double ProbeEngine::FillRate(Ts horizon_us, Ts run_end_ts_us) const {
  std::uint64_t eligible = 0;
  std::uint64_t hits = 0;
  for (const ProbeRecord& r : records_) {
    // Only probes that had the full horizon available are counted.
    if (r.placed_ts_us + horizon_us > run_end_ts_us) {
      continue;
    }
    ++eligible;
    if (r.filled && (r.fill_ts_us - r.placed_ts_us) <= horizon_us) {
      ++hits;
    }
  }
  if (eligible == 0) {
    return 0.0;
  }
  return static_cast<double>(hits) / static_cast<double>(eligible);
}

void ProbeEngine::Clear() {
  records_.clear();
  by_order_.clear();
  placed_ = 0;
  filled_ = 0;
}

}  // namespace lob
