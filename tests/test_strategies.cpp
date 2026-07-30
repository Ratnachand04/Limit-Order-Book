// The strategy ladder end to end (CLAUDE.md Phase 6 gate: "every strategy runs
// a full recorded day end-to-end"), plus the sigma and flow estimators.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <lob/sim/simulator.hpp>
#include <lob/strategy/factory.hpp>
#include <lob/strategy/strategies.hpp>
#include <lob/strategy/volatility.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

// ---------------------------------------------------------------------------
// sigma (§3.2)
// ---------------------------------------------------------------------------
TEST(Volatility, IsZeroUntilTwoSamplesExist) {
  VolatilityEstimator v(1 * kUsPerSecond, 600 * kUsPerSecond);
  EXPECT_FALSE(v.ready());
  v.Observe(0, 100.0);
  EXPECT_FALSE(v.ready());
  EXPECT_DOUBLE_EQ(v.sigma(), 0.0);
  v.Observe(1 * kUsPerSecond, 100.0);
  EXPECT_TRUE(v.ready());
}

TEST(Volatility, RecoversAKnownConstantStepSize) {
  // A mid that moves exactly +/-0.5 every second has sigma = 0.5 per sqrt(s).
  VolatilityEstimator v(1 * kUsPerSecond, 600 * kUsPerSecond);
  double mid = 100.0;
  v.Observe(0, mid);
  for (int i = 1; i <= 300; ++i) {
    mid += (i % 2 == 0) ? 0.5 : -0.5;
    v.Observe(static_cast<Ts>(i) * kUsPerSecond, mid);
  }
  EXPECT_NEAR(v.sigma(), 0.5, 1e-9);
}

TEST(Volatility, SamplesNoFasterThanTheConfiguredPeriod) {
  // "Sampling much faster than 1 s contaminates the estimate with
  //  microstructure noise (bid-ask bounce)."  The estimator enforces it.
  VolatilityEstimator v(1 * kUsPerSecond, 600 * kUsPerSecond);
  v.Observe(0, 100.0);
  for (int i = 1; i < 100; ++i) {  // 100 observations inside one second
    v.Observe(static_cast<Ts>(i) * 1000, 100.0 + (i % 2 ? 0.02 : -0.02));
  }
  EXPECT_EQ(v.sample_count(), 0u);
  v.Observe(1 * kUsPerSecond, 100.0);
  EXPECT_EQ(v.sample_count(), 1u);
}

TEST(Volatility, DropsSamplesOlderThanTheWindow) {
  VolatilityEstimator v(1 * kUsPerSecond, 10 * kUsPerSecond);
  double mid = 100.0;
  for (int i = 0; i <= 60; ++i) {
    mid += 0.1;
    v.Observe(static_cast<Ts>(i) * kUsPerSecond, mid);
  }
  EXPECT_LE(v.sample_count(), 11u);
}

TEST(FlowImbalance, SignsAggressionAndDecaysOutOfTheWindow) {
  FlowImbalance f(2 * kUsPerSecond);
  EXPECT_DOUBLE_EQ(f.SignedFraction(0), 0.0);

  // Buy-aggressive flow lifts offers: positive.
  f.Observe(0, Side::kBid, 100);
  EXPECT_DOUBLE_EQ(f.SignedFraction(0), 1.0);
  f.Observe(100'000, Side::kAsk, 100);
  EXPECT_DOUBLE_EQ(f.SignedFraction(100'000), 0.0);

  // Everything ages out of a 2 s window.
  EXPECT_DOUBLE_EQ(f.SignedFraction(10 * kUsPerSecond), 0.0);
  EXPECT_EQ(f.total_volume(), 0);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
TEST(StrategyFactory, BuildsEveryNameInTheLadder) {
  for (const std::string& name : StrategyNames()) {
    StrategyConfig config;
    config.name = name;
    std::unique_ptr<Strategy> s = MakeStrategy(config);
    ASSERT_NE(s, nullptr) << name;
    // S0_touch and S0_queue are deliberately the SAME code under two fill
    // models -- that is what makes RQ1 a controlled comparison.
    EXPECT_FALSE(s->name().empty());
  }
}

TEST(StrategyFactory, RejectsUnknownNamesLoudly) {
  StrategyConfig config;
  config.name = "S9_nonsense";
  EXPECT_THROW(MakeStrategy(config), std::invalid_argument);
}

TEST(StrategyFactory, FallsBackWithAnErrorWhenCalibrationIsMissing) {
  StrategyConfig config;
  config.name = "S2_AS";
  config.k_a_calibration_path = "definitely/not/here.json";
  config.k_fallback = 0.05;
  config.a_fallback = 2.0;
  const IntensityCalibration calib = LoadIntensityCalibration(config);
  // A run on fallback parameters is valid, but it must never be mistaken for a
  // calibrated one, so the failure is reported rather than swallowed.
  EXPECT_FALSE(calib.from_file);
  EXPECT_FALSE(calib.error.empty());
  EXPECT_DOUBLE_EQ(calib.k, 0.05);
  EXPECT_DOUBLE_EQ(calib.a, 2.0);
}

// ---------------------------------------------------------------------------
// End-to-end
// ---------------------------------------------------------------------------
struct LadderResult {
  std::uint64_t fills = 0;
  std::uint64_t quotes = 0;
  std::int64_t identity_residual = 1;
  Lots64 max_abs_inventory = 0;
};

LadderResult RunLadder(const std::string& name, const Instrument& inst,
                       const std::vector<Event>& events, QueueModel queue = QueueModel::kPess) {
  RunConfig config = testing::MakeTestConfig(inst, queue, 50 * kUsPerMilli);
  config.strategy.name = name;
  config.strategy.glft_mode = (name == "S2_GLFT");
  config.strategy.use_weighted_mid = (name == "S3");
  config.fill_model = (name == "S0_touch") ? "TOUCH" : "QUEUE";
  config.strategy.q_max_lots = 5;
  config.strategy.fixed_spread_ticks = 1;
  config.strategy.requote_min_ticks = 1;
  config.strategy.enforce_min_edge = false;
  config.strategy.sigma_window_us = 20 * kUsPerSecond;
  config.strategy.sigma_sample_us = 1 * kUsPerSecond;
  config.strategy.gamma = 1.0e-4;
  config.strategy.k_fallback = 0.5;
  config.strategy.a_fallback = 1.0;
  config.strategy.horizon_s = 60.0;
  config.fees.maker_tenth_bp = 0;

  std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
  Simulator sim(config, *strategy);
  sim.Run(events);

  LadderResult r;
  r.fills = sim.stats().fills;
  r.quotes = strategy->diagnostics().quotes_placed;
  r.identity_residual = sim.ledger().IdentityResidualX2();
  r.max_abs_inventory = std::abs(sim.ledger().inventory_lots());
  return r;
}

TEST(StrategyLadder, EveryStrategyRunsAFullSessionWithAnExactLedger) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events = testing::SyntheticSession(
      inst, /*seed=*/21, 1'600'000'000'000'000LL, 180 * kUsPerSecond, 40);
  ASSERT_GT(events.size(), 2000u);

  for (const std::string& name : StrategyNames()) {
    SCOPED_TRACE(name);
    const LadderResult r = RunLadder(name, inst, events);
    EXPECT_EQ(r.identity_residual, 0) << name << " broke the PnL identity";
    EXPECT_GT(r.quotes, 0u) << name << " never quoted";
  }
}

TEST(StrategyLadder, TouchRuleFillsMoreThanTheQueueAwareModelOnTheSameData) {
  // RQ1's headline mechanism, on synthetic data: the touch rule overstates how
  // often you fill because it ignores everyone who was in the queue first.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 33, 1'600'000'000'000'000LL, 180 * kUsPerSecond, 40);

  const LadderResult touch = RunLadder("S0_touch", inst, events);
  const LadderResult queue = RunLadder("S0_queue", inst, events);
  EXPECT_GT(touch.fills, queue.fills)
      << "touch " << touch.fills << " vs queue-aware " << queue.fills;
}

TEST(StrategyLadder, OptimisticQueueFillsAtLeastAsOftenAsPessimistic) {
  // The bracket must be ordered on real replays too, not only in unit tests.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 44, 1'600'000'000'000'000LL, 180 * kUsPerSecond, 40);

  const LadderResult pess = RunLadder("S0_queue", inst, events, QueueModel::kPess);
  const LadderResult opt = RunLadder("S0_queue", inst, events, QueueModel::kOpt);
  EXPECT_LE(pess.fills, opt.fills);
}

TEST(StrategyLadder, InventoryStaysInsideTheHardCap) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 55, 1'600'000'000'000'000LL, 180 * kUsPerSecond, 40);
  for (const char* name : {"S1", "S2_AS", "S3"}) {
    SCOPED_TRACE(name);
    const LadderResult r = RunLadder(name, inst, events);
    // The cap is 5 lots; forced liquidation flattens anything beyond it.
    EXPECT_LE(r.max_abs_inventory, 5);
  }
}

TEST(StrategyLadder, MinMoveFilterSuppressesChurn) {
  // Part 11 pitfall #6: naive re-quoting destroys queue position.  With a wide
  // min-move filter the strategy must requote strictly less often.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 66, 1'600'000'000'000'000LL, 120 * kUsPerSecond, 40);

  auto run_with = [&](Ticks min_move) {
    RunConfig config = testing::MakeTestConfig(inst, QueueModel::kPess, 50 * kUsPerMilli);
    config.strategy.name = "S1";
    config.strategy.fixed_spread_ticks = 2;
    config.strategy.requote_min_ticks = min_move;
    config.strategy.enforce_min_edge = false;
    std::unique_ptr<Strategy> s = MakeStrategy(config.strategy);
    Simulator sim(config, *s);
    sim.Run(events);
    return s->diagnostics();
  };

  const StrategyDiagnostics tight = run_with(1);
  const StrategyDiagnostics loose = run_with(10);
  EXPECT_GT(loose.requotes_suppressed_by_min_move, tight.requotes_suppressed_by_min_move);
  EXPECT_LE(loose.quotes_placed, tight.quotes_placed);
}

TEST(StrategyLadder, MinEdgeCheckBindsAtRetailFees) {
  // "at 10 bp fees it will bind almost always, which is finding RQ4 announcing
  //  itself" (Part 6).  On a one-tick spread near 100.00 a 10 bp maker fee is
  //  three orders of magnitude larger than the edge.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 77, 1'600'000'000'000'000LL, 120 * kUsPerSecond, 40);

  RunConfig config = testing::MakeTestConfig(inst, QueueModel::kPess, 50 * kUsPerMilli);
  config.strategy.name = "S1";
  config.strategy.enforce_min_edge = true;
  config.fees.maker_tenth_bp = 100;  // 10 bp
  std::unique_ptr<Strategy> s = MakeStrategy(config.strategy);
  Simulator sim(config, *s);
  sim.Run(events);

  EXPECT_GT(s->diagnostics().quotes_blocked_by_min_edge, 0u);
  EXPECT_EQ(s->diagnostics().quotes_placed, 0u)
      << "no quote at a one-tick spread can beat a 10 bp fee";
}

TEST(StrategyLadder, ProbeOrdersAreTrackedButNeverTouchTheLedger) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 88, 1'600'000'000'000'000LL, 120 * kUsPerSecond, 40);

  RunConfig config = testing::MakeTestConfig(inst, QueueModel::kPess, 50 * kUsPerMilli);
  config.strategy.name = "S0_queue";
  config.strategy.enforce_min_edge = false;
  config.probes.enabled = true;
  config.probes.interval_us = 5 * kUsPerSecond;
  config.probes.depths_ticks = {1, 2, 5};
  config.probes.horizons_us = {1 * kUsPerSecond, 5 * kUsPerSecond};
  config.probes.size_lots = 1;

  std::unique_ptr<Strategy> s = MakeStrategy(config.strategy);
  Simulator sim(config, *s);
  sim.Run(events);

  EXPECT_GT(sim.probes().placed(), 0u);
  EXPECT_EQ(sim.ledger().IdentityResidualX2(), 0);
  // Probe fills are counted separately and never enter the strategy's PnL.
  EXPECT_EQ(sim.stats().fills, static_cast<std::uint64_t>(sim.fills().size()));
}

TEST(StrategyLadder, ProbeFillProbabilityFallsWithDepth) {
  // §3.8's validation monotonicity: "deeper -> less likely to fill".  If this
  // fails, something in the queue model is wrong.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 101, 1'600'000'000'000'000LL, 600 * kUsPerSecond, 60);

  RunConfig config = testing::MakeTestConfig(inst, QueueModel::kOpt, 10 * kUsPerMilli);
  config.strategy.name = "S0_queue";
  config.strategy.enforce_min_edge = false;
  config.probes.enabled = true;
  config.probes.interval_us = 1 * kUsPerSecond;
  config.probes.depths_ticks = {1, 6};
  config.probes.horizons_us = {5 * kUsPerSecond};
  config.probes.size_lots = 1;

  std::unique_ptr<Strategy> s = MakeStrategy(config.strategy);
  Simulator sim(config, *s);
  sim.Run(events);
  ASSERT_GT(sim.probes().placed(), 20u);

  std::uint64_t shallow_fills = 0;
  std::uint64_t shallow_total = 0;
  std::uint64_t deep_fills = 0;
  std::uint64_t deep_total = 0;
  for (const ProbeRecord& r : sim.probes().records()) {
    if (r.depth_ticks == 1) {
      ++shallow_total;
      shallow_fills += r.filled ? 1u : 0u;
    } else {
      ++deep_total;
      deep_fills += r.filled ? 1u : 0u;
    }
  }
  ASSERT_GT(shallow_total, 0u);
  ASSERT_GT(deep_total, 0u);
  const double shallow_rate =
      static_cast<double>(shallow_fills) / static_cast<double>(shallow_total);
  const double deep_rate = static_cast<double>(deep_fills) / static_cast<double>(deep_total);
  EXPECT_GE(shallow_rate, deep_rate)
      << "shallow " << shallow_rate << " vs deep " << deep_rate;
}

}  // namespace
}  // namespace lob
