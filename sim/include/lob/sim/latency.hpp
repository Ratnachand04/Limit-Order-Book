// Latency model (master plan §4.5).
//
//   delta = constant + Exp(jitter)
//
// applied on two legs:
//   delta_in  -- market events become visible at exch_ts + delta_in
//   delta_out -- our actions reach the exchange at decision_ts + delta_out
//
// Both draws come from the single seeded RNG, so the sequence of latencies is
// part of the reproducible state.  The experiment grid sweeps the constant over
// {5, 50, 200} ms; master plan Part 11 pitfall #10 is the reminder that 5 ms is
// decorative for a retail setup and 50-200 ms is the honest band.
#pragma once

#include <cstdint>

#include <lob/config.hpp>
#include <lob/rng.hpp>
#include <lob/types.hpp>

namespace lob {

class LatencyModel {
 public:
  LatencyModel(const LatencyConfig& config, Rng& rng) : config_(config), rng_(&rng) {}

  // Inbound leg: when we see a market event stamped `exch_ts_us`.
  [[nodiscard]] Ts VisibleAt(Ts exch_ts_us) { return exch_ts_us + DrawIn(); }

  // Outbound leg: when an action decided now reaches the matching engine.
  [[nodiscard]] Ts EffectiveAt(Ts decision_ts_us) { return decision_ts_us + DrawOut(); }

  [[nodiscard]] Ts DrawIn() { return config_.in_us + DrawJitter(); }
  [[nodiscard]] Ts DrawOut() { return config_.out_us + DrawJitter(); }

  [[nodiscard]] const LatencyConfig& config() const { return config_; }
  [[nodiscard]] std::uint64_t draw_count() const { return draws_; }

 private:
  [[nodiscard]] Ts DrawJitter() {
    ++draws_;
    if (!(config_.jitter_exp_us > 0.0)) {
      return 0;
    }
    const double j = rng_->ExponentialMean(config_.jitter_exp_us);
    return static_cast<Ts>(j);  // truncate: latency is measured in whole us
  }

  LatencyConfig config_;
  Rng* rng_;
  std::uint64_t draws_ = 0;
};

}  // namespace lob
