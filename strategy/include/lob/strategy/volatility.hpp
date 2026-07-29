// Rolling volatility and trade-flow toxicity estimators.
//
// VOLATILITY (master plan §3.2).  sigma is estimated as the standard deviation
// of mid changes sampled every Delta = 1 s over a rolling 10-minute window,
// reported in price units per sqrt(second):
//
//     sigma^2 = (1 / (n * Delta)) * SUM (m_{t+Delta} - m_t)^2
//
// Why exactly Delta = 1 s, in one sentence you should be able to say on demand:
// sampling much faster contaminates the estimate with microstructure noise
// (bid-ask bounce), and sampling much slower loses reactivity.
//
// FLOW TOXICITY (master plan §3.6, Part 6, strategy S3).  One-sided aggression
// over a short window is the observable signature of informed flow arriving.
// S3 pulls the endangered quote when it crosses a threshold.
#pragma once

#include <cstdint>
#include <deque>

#include <lob/types.hpp>

namespace lob {

class VolatilityEstimator {
 public:
  VolatilityEstimator(Ts sample_us, Ts window_us) : sample_us_(sample_us), window_us_(window_us) {}

  // Feed the current mid, in PRICE units, as often as it changes.  Only one
  // sample per `sample_us` is retained.
  void Observe(Ts now_us, double mid_price);

  // sigma in price units per sqrt(second).  Zero until `ready()`.
  [[nodiscard]] double sigma() const;
  [[nodiscard]] double variance_per_second() const;
  [[nodiscard]] bool ready() const { return samples_ >= 2; }
  [[nodiscard]] std::size_t sample_count() const { return diffs_.size(); }

  void Reset();

 private:
  struct Diff {
    Ts ts_us = 0;         // time of the LATER of the two samples
    double squared = 0.0; // (m_t - m_{t-Delta})^2
    double dt_s = 0.0;    // actual elapsed time, which may exceed Delta after a
                          // quiet stretch or a dirty interval
  };

  void Trim(Ts now_us);

  Ts sample_us_;
  Ts window_us_;
  std::deque<Diff> diffs_;
  double sum_squared_ = 0.0;
  double sum_dt_s_ = 0.0;
  Ts last_sample_ts_ = 0;
  double last_sample_mid_ = 0.0;
  std::uint64_t samples_ = 0;
};

// Signed aggressive volume over a rolling window.
class FlowImbalance {
 public:
  explicit FlowImbalance(Ts window_us) : window_us_(window_us) {}

  // `aggressor` is the side that crossed the spread: kBid for a buy-aggressor.
  void Observe(Ts now_us, Side aggressor, Lots64 qty_lots);
  void Trim(Ts now_us);

  // Net signed volume / total volume over the window, in [-1, +1].
  // Positive means buy-aggressive flow dominates.
  [[nodiscard]] double SignedFraction(Ts now_us);
  [[nodiscard]] Lots64 total_volume() const { return total_; }

  void Reset();

 private:
  struct Print {
    Ts ts_us = 0;
    Lots64 signed_qty = 0;
    Lots64 qty = 0;
  };

  Ts window_us_;
  std::deque<Print> prints_;
  Lots64 signed_total_ = 0;
  Lots64 total_ = 0;
};

}  // namespace lob
