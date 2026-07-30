// Determinism (CLAUDE.md Phase 3 gate: "two full runs byte-identical", wired
// into CI).
//
// The contract is: same data + same config + same seed => byte-identical
// outputs.  This is not a nicety.  It is what makes a result in the paper
// checkable by anyone with the repo, and it is the property that breaks first
// when someone reaches for wall-clock time, an unordered container's iteration
// order, or a second RNG.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <lob/analytics/recorders.hpp>
#include <lob/sim/simulator.hpp>
#include <lob/strategy/factory.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

struct RunArtifacts {
  std::string fills;
  std::string markouts;
  std::string pnl_daily;
  std::string queue_stats;
  std::string manifest;
  std::uint64_t fill_count = 0;
  std::int64_t equity_x2 = 0;
  std::uint64_t rng_draws = 0;
};

RunArtifacts RunOnce(const std::string& tag, const RunConfig& base,
                     const std::vector<Event>& events) {
  RunConfig config = base;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "lob_sim_tests" / ("determinism_" + tag);
  std::filesystem::remove_all(dir);
  config.output_dir = dir.string();

  std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);

  RunTags tags;
  tags.run_id = "determinism";
  tags.strategy = config.strategy.name;
  tags.symbol = config.PrimaryInstrument().symbol();
  tags.queue_assumption = std::string(QueueModelName(config.queue_model));
  tags.maker_fee_bp = config.fees.maker_bp();
  tags.latency_in_ms = config.latency.in_us / kUsPerMilli;
  tags.latency_out_ms = config.latency.out_us / kUsPerMilli;
  tags.seed = config.seed;

  RunRecorders recorders(config.output_dir, tags, config.PrimaryInstrument());
  Simulator sim(config, *strategy);
  sim.set_recorders(&recorders);
  sim.Run(events);
  recorders.WriteMarkouts(sim.markouts());
  recorders.WriteDailyPnl(sim.markouts().Summarise());
  recorders.WriteQueueStats(sim.queue_tracker().stats());
  recorders.WriteManifest(config.Manifest());
  recorders.CloseAll();

  RunArtifacts out;
  out.fills = testing::ReadFileBytes((dir / "fills.csv").string());
  out.markouts = testing::ReadFileBytes((dir / "markouts.csv").string());
  out.pnl_daily = testing::ReadFileBytes((dir / "pnl_daily.csv").string());
  out.queue_stats = testing::ReadFileBytes((dir / "queue_stats.csv").string());
  out.manifest = testing::ReadFileBytes((dir / "manifest.yaml").string());
  out.fill_count = sim.stats().fills;
  out.equity_x2 = sim.ledger().EquityX2();
  out.rng_draws = sim.rng().draws();
  return out;
}

// Removes the single manifest line that names the output directory, so two runs
// writing to different places can still be compared line for line.
std::string StripOutputDir(const std::string& manifest) {
  std::string out;
  std::size_t pos = 0;
  while (pos < manifest.size()) {
    const std::size_t nl = manifest.find('\n', pos);
    const std::size_t end = (nl == std::string::npos) ? manifest.size() : nl + 1;
    const std::string line = manifest.substr(pos, end - pos);
    if (line.rfind("output_dir:", 0) != 0) {
      out += line;
    }
    pos = end;
  }
  return out;
}

RunConfig DeterminismConfig(const Instrument& inst) {
  RunConfig c = testing::MakeTestConfig(inst, QueueModel::kProp, 50 * kUsPerMilli);
  // Jitter ON: the whole point is that randomness is reproducible, not absent.
  c.latency.jitter_exp_us = 5000.0;
  c.fees.maker_tenth_bp = 20;
  c.strategy.name = "S1";
  c.strategy.fixed_spread_ticks = 1;
  c.strategy.requote_min_ticks = 1;
  c.strategy.enforce_min_edge = false;
  c.strategy.q_max_lots = 20;
  c.strategy.sigma_window_us = 30 * kUsPerSecond;
  c.probes.enabled = true;
  c.probes.interval_us = 2 * kUsPerSecond;
  c.probes.depths_ticks = {1, 3};
  c.probes.horizons_us = {1 * kUsPerSecond, 5 * kUsPerSecond};
  c.seed = 20260729;
  return c;
}

TEST(Determinism, TwoRunsProduceByteIdenticalOutputs) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, /*seed=*/11, 1'600'000'000'000'000LL,
                                /*duration_us=*/120 * kUsPerSecond, /*events_per_second=*/40);
  ASSERT_GT(events.size(), 1000u);

  const RunConfig config = DeterminismConfig(inst);
  const RunArtifacts a = RunOnce("a", config, events);
  const RunArtifacts b = RunOnce("b", config, events);

  EXPECT_FALSE(a.fills.empty()) << "the scenario must actually produce fills to be meaningful";
  EXPECT_EQ(a.fills, b.fills);
  EXPECT_EQ(a.markouts, b.markouts);
  EXPECT_EQ(a.pnl_daily, b.pnl_daily);
  EXPECT_EQ(a.queue_stats, b.queue_stats);
  // The manifest records where the run wrote its output, and the two runs must
  // write to different directories or they would overwrite each other. That one
  // line is the only thing allowed to differ.
  EXPECT_EQ(StripOutputDir(a.manifest), StripOutputDir(b.manifest));
  EXPECT_EQ(a.fill_count, b.fill_count);
  EXPECT_EQ(a.equity_x2, b.equity_x2);
  // Same number of RNG draws, in the same order: proof that no code path
  // branched on anything non-deterministic.
  EXPECT_EQ(a.rng_draws, b.rng_draws);
}

TEST(Determinism, ADifferentSeedChangesTheOutput) {
  // The converse check: if two seeds gave identical output, the seed would not
  // be reaching the model and the determinism test above would be vacuous.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events = testing::SyntheticSession(
      inst, 11, 1'600'000'000'000'000LL, 120 * kUsPerSecond, 40);

  RunConfig config = DeterminismConfig(inst);
  const RunArtifacts a = RunOnce("seed_a", config, events);
  config.seed = 999;
  const RunArtifacts b = RunOnce("seed_b", config, events);
  EXPECT_NE(a.fills, b.fills);
}

TEST(Determinism, RerunningTheSameSimulatorObjectGivesTheSameResult) {
  // Run() resets every piece of state, so a sweep that reuses one Simulator
  // cannot leak the previous cell's state into the next.
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events = testing::SyntheticSession(
      inst, 3, 1'600'000'000'000'000LL, 60 * kUsPerSecond, 30);

  const RunConfig config = DeterminismConfig(inst);
  std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
  Simulator sim(config, *strategy);

  sim.Run(events);
  const std::uint64_t fills_first = sim.stats().fills;
  const std::int64_t equity_first = sim.ledger().EquityX2();
  const std::uint64_t draws_first = sim.rng().draws();

  sim.Run(events);
  EXPECT_EQ(sim.stats().fills, fills_first);
  EXPECT_EQ(sim.ledger().EquityX2(), equity_first);
  EXPECT_EQ(sim.rng().draws(), draws_first);
}

TEST(Determinism, PnlIdentityHoldsOnEveryQueueAssumption) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events = testing::SyntheticSession(
      inst, 5, 1'600'000'000'000'000LL, 90 * kUsPerSecond, 40);

  for (const QueueModel model : {QueueModel::kPess, QueueModel::kOpt, QueueModel::kProp}) {
    RunConfig config = DeterminismConfig(inst);
    config.queue_model = model;
    std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
    Simulator sim(config, *strategy);
    sim.Run(events);
    EXPECT_EQ(sim.ledger().IdentityResidualX2(), 0)
        << QueueModelName(model) << ": " << sim.ledger().IdentityReport();
  }
}

TEST(Determinism, DualBookAgreesOverAFullReplay) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events = testing::SyntheticSession(
      inst, 7, 1'600'000'000'000'000LL, 90 * kUsPerSecond, 40);

  RunConfig config = DeterminismConfig(inst);
  config.dual_book_check = true;
  std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
  Simulator sim(config, *strategy);
  sim.set_integrity_check_every(500);
  sim.Run(events);

  EXPECT_EQ(sim.stats().dual_book_mismatches, 0u);
  EXPECT_EQ(sim.stats().book_integrity_failures, 0u);
}

}  // namespace
}  // namespace lob
