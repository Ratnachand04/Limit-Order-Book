// The strategy ladder (master plan Part 6).
//
//   S0_touch  Naive joiner, evaluated under the TOUCH-rule fill model.  The
//             strawman.  Its only purpose is to be the controlled comparison
//             for RQ1: same strategy, same data, dishonest fill model.
//   S0_queue  The same joiner under the queue-aware fill model.  The gap
//             between these two IS the headline result.
//   S1        Fixed spread, inventory-capped, min-move filtered.  The first
//             strategy that makes the cost of repricing visible.
//   S2_AS     Avellaneda-Stoikov (§3.4) with rolling sigma and calibrated k.
//   S2_GLFT   The same, using the GLFT steady-state distances (§3.5).
//   S3        S2 plus a weighted-mid fair value and a toxicity pull (§3.6).
#pragma once

#include <string>

#include <lob/strategy/as_math.hpp>
#include <lob/strategy/quoting_strategy.hpp>

namespace lob {

// ---------------------------------------------------------------------------
// S0 -- always quote one unit at the touch, never skew.
// ---------------------------------------------------------------------------
class S0Joiner final : public QuotingStrategy {
 public:
  explicit S0Joiner(std::string label) : label_(std::move(label)) {}
  [[nodiscard]] std::string name() const override { return label_; }

 protected:
  DesiredQuotes ComputeQuotes(const BookView& book, const Clock& clock) override;

 private:
  std::string label_;
};

// ---------------------------------------------------------------------------
// S1 -- fixed distance from mid, inventory-capped.
// ---------------------------------------------------------------------------
class S1FixedSpread final : public QuotingStrategy {
 public:
  [[nodiscard]] std::string name() const override { return "S1"; }

 protected:
  DesiredQuotes ComputeQuotes(const BookView& book, const Clock& clock) override;
};

// ---------------------------------------------------------------------------
// S2 -- Avellaneda-Stoikov, or its GLFT steady-state form.
// ---------------------------------------------------------------------------
class S2AvellanedaStoikov : public QuotingStrategy {
 public:
  explicit S2AvellanedaStoikov(bool glft_mode) : glft_(glft_mode) {}
  [[nodiscard]] std::string name() const override { return glft_ ? "S2_GLFT" : "S2_AS"; }

  // Calibration inputs (§3.3).  Loaded from the k/A JSON when one is
  // configured; otherwise the configured fallbacks are used and the run
  // manifest records that they were fallbacks.
  void SetIntensity(double k, double a) {
    k_ = k;
    a_ = a;
  }
  [[nodiscard]] double k() const { return k_; }
  [[nodiscard]] double a() const { return a_; }

 protected:
  DesiredQuotes ComputeQuotes(const BookView& book, const Clock& clock) override;

  // The fair value the model is centred on.  S2 uses the mid; S3 overrides.
  [[nodiscard]] virtual double FairValue() const { return mid_price(); }
  // Hook for S3's toxicity pull; returns which side (if any) to suppress.
  virtual void ApplyPull(DesiredQuotes& /*quotes*/, const Clock& /*clock*/) {}

  AsQuotes BuildInputsAndSolve() const;

  bool glft_ = false;
  double k_ = 0.035;
  double a_ = 1.0;
};

// ---------------------------------------------------------------------------
// S3 -- S2 + weighted-mid fair value + toxicity pull.
// ---------------------------------------------------------------------------
class S3MicroToxicity final : public S2AvellanedaStoikov {
 public:
  explicit S3MicroToxicity(bool glft_mode) : S2AvellanedaStoikov(glft_mode) {}
  [[nodiscard]] std::string name() const override { return "S3"; }

 protected:
  [[nodiscard]] double FairValue() const override;
  void ApplyPull(DesiredQuotes& quotes, const Clock& clock) override;
  void OnTradeExtra(const TradeInfo& trade, const BookView& book, const Clock& clock) override;

 private:
  // While `pull_until_us_` is in the future, the named side stays out of the
  // market.  kNoPull means nothing is currently pulled.
  Ts pull_until_us_ = 0;
  bool pull_bid_ = false;
  bool pull_ask_ = false;
};

}  // namespace lob
