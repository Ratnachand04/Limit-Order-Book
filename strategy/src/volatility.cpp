#include <lob/strategy/volatility.hpp>

#include <cmath>

namespace lob {

void VolatilityEstimator::Reset() {
  diffs_.clear();
  sum_squared_ = 0.0;
  sum_dt_s_ = 0.0;
  last_sample_ts_ = 0;
  last_sample_mid_ = 0.0;
  samples_ = 0;
}

void VolatilityEstimator::Observe(Ts now_us, double mid_price) {
  if (samples_ == 0) {
    last_sample_ts_ = now_us;
    last_sample_mid_ = mid_price;
    samples_ = 1;
    return;
  }
  if (now_us - last_sample_ts_ < sample_us_) {
    return;
  }
  const double dt_s = static_cast<double>(now_us - last_sample_ts_) /
                      static_cast<double>(kUsPerSecond);
  const double d = mid_price - last_sample_mid_;

  Diff diff;
  diff.ts_us = now_us;
  diff.squared = d * d;
  diff.dt_s = dt_s;
  diffs_.push_back(diff);
  sum_squared_ += diff.squared;
  sum_dt_s_ += diff.dt_s;

  last_sample_ts_ = now_us;
  last_sample_mid_ = mid_price;
  ++samples_;
  Trim(now_us);
}

void VolatilityEstimator::Trim(Ts now_us) {
  while (!diffs_.empty() && now_us - diffs_.front().ts_us > window_us_) {
    sum_squared_ -= diffs_.front().squared;
    sum_dt_s_ -= diffs_.front().dt_s;
    diffs_.pop_front();
  }
  // Guard against the accumulated subtraction drifting below zero.
  if (diffs_.empty() || sum_squared_ < 0.0) {
    sum_squared_ = 0.0;
    for (const Diff& d : diffs_) {
      sum_squared_ += d.squared;
    }
  }
  if (diffs_.empty() || sum_dt_s_ < 0.0) {
    sum_dt_s_ = 0.0;
    for (const Diff& d : diffs_) {
      sum_dt_s_ += d.dt_s;
    }
  }
}

double VolatilityEstimator::variance_per_second() const {
  // Dividing by the ACTUAL elapsed time rather than n * Delta keeps the
  // estimate unbiased when samples are missing -- which happens across dirty
  // intervals and quiet stretches, and would otherwise inflate sigma.
  if (diffs_.empty() || sum_dt_s_ <= 0.0) {
    return 0.0;
  }
  return sum_squared_ / sum_dt_s_;
}

double VolatilityEstimator::sigma() const {
  const double v = variance_per_second();
  return v > 0.0 ? std::sqrt(v) : 0.0;
}

// ---------------------------------------------------------------------------
void FlowImbalance::Reset() {
  prints_.clear();
  signed_total_ = 0;
  total_ = 0;
}

void FlowImbalance::Observe(Ts now_us, Side aggressor, Lots64 qty_lots) {
  if (qty_lots <= 0) {
    return;
  }
  Print p;
  p.ts_us = now_us;
  p.qty = qty_lots;
  // A buy-aggressor lifts offers: positive signed flow.
  p.signed_qty = (aggressor == Side::kBid) ? qty_lots : -qty_lots;
  prints_.push_back(p);
  signed_total_ += p.signed_qty;
  total_ += p.qty;
  Trim(now_us);
}

void FlowImbalance::Trim(Ts now_us) {
  while (!prints_.empty() && now_us - prints_.front().ts_us > window_us_) {
    signed_total_ -= prints_.front().signed_qty;
    total_ -= prints_.front().qty;
    prints_.pop_front();
  }
  if (prints_.empty()) {
    signed_total_ = 0;
    total_ = 0;
  }
}

double FlowImbalance::SignedFraction(Ts now_us) {
  Trim(now_us);
  if (total_ <= 0) {
    return 0.0;
  }
  return static_cast<double>(signed_total_) / static_cast<double>(total_);
}

}  // namespace lob
