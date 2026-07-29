// Probe orders -- empirical fill probability (master plan §3.8, RQ2).
//
// On a fixed schedule the engine spawns virtual orders at several depths on
// both sides and tracks each to fill-or-expiry over a grid of horizons.  The
// result is P(fill within h | depth, queue fraction, imbalance, sigma), which
// is both a headline result (the F2 heatmap) and the simulator's own
// validation: the monotonicities MUST hold --
//
//   deeper                     -> less likely to fill
//   more quantity ahead of you -> less likely to fill
//   imbalance in your favour   -> more likely to fill
//
// If they do not, something in the queue model is wrong.
//
// Probes are pure shadows: they are tracked by the queue tracker exactly like a
// real order, but they never touch the ledger, never affect inventory and never
// count as strategy fills.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <lob/config.hpp>
#include <lob/execution.hpp>
#include <lob/types.hpp>

namespace lob {

struct ProbeRecord {
  std::uint64_t probe_id = 0;
  OrderId order_id = kNoOrder;
  Ts placed_ts_us = 0;
  Side side = Side::kBid;
  Ticks depth_ticks = 0;
  Ticks price_ticks = 0;
  Lots64 size_lots = 0;

  // Features at placement, for the logistic model of §3.8.
  double initial_queue_fraction = 0.0;
  double imbalance = 0.5;
  double sigma_ticks = 0.0;

  bool filled = false;
  Ts fill_ts_us = 0;
  bool retired = false;
};

class ProbeEngine {
 public:
  explicit ProbeEngine(ProbeConfig config) : config_(std::move(config)) {}

  [[nodiscard]] bool enabled() const { return config_.enabled; }
  [[nodiscard]] const ProbeConfig& config() const { return config_; }

  // Longest horizon: how long a probe must be tracked before it can be retired.
  [[nodiscard]] Ts MaxHorizonUs() const;

  std::uint64_t Register(OrderId order_id, Ts placed_ts_us, Side side, Ticks depth_ticks,
                         Ticks price_ticks, Lots64 size_lots, double initial_queue_fraction,
                         double imbalance, double sigma_ticks);

  void NoteFill(OrderId order_id, Ts fill_ts_us);
  ProbeRecord* Find(OrderId order_id);
  void Retire(OrderId order_id);

  [[nodiscard]] const std::vector<ProbeRecord>& records() const { return records_; }
  [[nodiscard]] std::uint64_t placed() const { return placed_; }
  [[nodiscard]] std::uint64_t filled() const { return filled_; }

  // Empirical fill rate at a horizon, computed only over probes that were
  // tracked for at least that long.  Probes cut short by the end of the run are
  // excluded rather than counted as "did not fill" -- counting them would bias
  // long horizons downward.
  [[nodiscard]] double FillRate(Ts horizon_us, Ts run_end_ts_us) const;

  void Clear();

 private:
  ProbeConfig config_;
  std::vector<ProbeRecord> records_;
  std::map<OrderId, std::size_t> by_order_;
  std::uint64_t placed_ = 0;
  std::uint64_t filled_ = 0;
};

}  // namespace lob
