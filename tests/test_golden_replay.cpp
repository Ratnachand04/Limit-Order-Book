// Golden replay (CLAUDE.md Phase 2 gate: "golden replay of a committed
// 10-minute fixture"; §4.9 "any diff fails CI").
//
// The fixture is a SYNTHETIC ten-minute session generated deterministically
// from a fixed seed.  Synthetic input is the right thing here -- it is a
// regression anchor, not a result -- and a committed binary that can be
// regenerated bit-for-bit from a seed is auditable in a way a captured slice of
// private data is not.  When real recorded data exists, add a second fixture
// cut from it; the harness below takes any .lobbin.
//
// BOOTSTRAPPING.  If the expectation files are absent, the test writes them and
// SKIPS rather than passing vacuously.  Review the generated files, commit
// them, and the test becomes a hard gate from then on.  Re-baseline
// deliberately with LOB_REGENERATE_GOLDEN=1.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <lob/analytics/recorders.hpp>
#include <lob/converter/event_io.hpp>
#include <lob/sim/simulator.hpp>
#include <lob/strategy/factory.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

// Frozen generator parameters.  Changing any of these changes the fixture and
// invalidates every committed expectation, so they are constants, not knobs.
constexpr std::uint64_t kFixtureSeed = 20260729;
constexpr Ts kFixtureStartTs = 1'785'283'200LL * kUsPerSecond;  // 2026-07-29T00:00:00Z
constexpr Ts kFixtureDuration = 600 * kUsPerSecond;             // ten minutes
constexpr int kFixtureEventsPerSecond = 40;

bool RegenerateRequested() {
  const char* v = std::getenv("LOB_REGENERATE_GOLDEN");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

std::filesystem::path FixtureDir() { return std::filesystem::path(testing::GoldenDir()); }

std::vector<Event> BuildFixtureEvents() {
  return testing::SyntheticSession(testing::TestInstrument(), kFixtureSeed, kFixtureStartTs,
                                   kFixtureDuration, kFixtureEventsPerSecond);
}

RunConfig GoldenConfig() {
  RunConfig c = testing::MakeTestConfig(testing::TestInstrument(), QueueModel::kPess,
                                        50 * kUsPerMilli);
  c.run_id = "golden";
  c.latency.jitter_exp_us = 5000.0;
  c.fees.maker_tenth_bp = 20;
  c.strategy.name = "S1";
  c.strategy.fixed_spread_ticks = 1;
  c.strategy.requote_min_ticks = 2;
  c.strategy.q_max_lots = 10;
  c.strategy.enforce_min_edge = false;
  c.strategy.sigma_window_us = 60 * kUsPerSecond;
  c.markouts.horizons_us = {1 * kUsPerSecond, 10 * kUsPerSecond};
  c.seed = kFixtureSeed;
  return c;
}

// Compares against a committed file, or writes it and reports that it did.
// Returns true when a comparison actually happened.
bool CompareOrBootstrap(const std::filesystem::path& expected_path, const std::string& actual,
                        std::string* note) {
  if (RegenerateRequested() || !std::filesystem::exists(expected_path)) {
    std::filesystem::create_directories(expected_path.parent_path());
    std::ofstream out(expected_path.string(), std::ios::binary | std::ios::trunc);
    out.write(actual.data(), static_cast<std::streamsize>(actual.size()));
    *note = "wrote " + expected_path.string();
    return false;
  }
  const std::string expected = testing::ReadFileBytes(expected_path.string());
  EXPECT_EQ(expected, actual) << "golden mismatch in " << expected_path.string()
                              << "\nIf this change is intended, re-run with "
                                 "LOB_REGENERATE_GOLDEN=1 and commit the new expectation.";
  return true;
}

TEST(GoldenReplay, FixtureRegeneratesBitForBitFromItsSeed) {
  // The fixture's whole value is that it is reproducible.  If the generator
  // ever drifts, this fails before any downstream expectation does.
  const std::vector<Event> a = BuildFixtureEvents();
  const std::vector<Event> b = BuildFixtureEvents();
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(a[i], b[i]) << "record " << i;
  }
  EXPECT_GT(a.size(), 10'000u) << "a ten-minute fixture should be substantial";
}

TEST(GoldenReplay, FixtureFileMatchesTheCommittedBytes) {
  const std::vector<Event> events = BuildFixtureEvents();
  const std::filesystem::path fixture = FixtureDir() / "golden_10min.lobbin";

  const std::filesystem::path temp =
      std::filesystem::temp_directory_path() / "lob_sim_tests" / "golden_regen.lobbin";
  std::filesystem::create_directories(temp.parent_path());
  WriteAllEvents(temp.string(), events);
  const std::string actual = testing::ReadFileBytes(temp.string());

  std::string note;
  if (!CompareOrBootstrap(fixture, actual, &note)) {
    GTEST_SKIP() << "golden fixture bootstrapped (" << note
                 << "). Review it, commit it, and this test becomes a hard gate.";
  }
}

TEST(GoldenReplay, AnalyticsOutputMatchesTheCommittedExpectations) {
  const std::vector<Event> events = BuildFixtureEvents();
  const RunConfig config = GoldenConfig();

  const std::filesystem::path out_dir =
      std::filesystem::temp_directory_path() / "lob_sim_tests" / "golden_run";
  std::filesystem::remove_all(out_dir);

  RunConfig run = config;
  run.output_dir = out_dir.string();

  std::unique_ptr<Strategy> strategy = MakeStrategy(run.strategy);
  RunTags tags;
  tags.run_id = run.run_id;
  tags.strategy = run.strategy.name;
  tags.symbol = run.PrimaryInstrument().symbol();
  tags.queue_assumption = std::string(QueueModelName(run.queue_model));
  tags.maker_fee_bp = run.fees.maker_bp();
  tags.latency_in_ms = run.latency.in_us / kUsPerMilli;
  tags.latency_out_ms = run.latency.out_us / kUsPerMilli;
  tags.seed = run.seed;

  RunRecorders recorders(run.output_dir, tags, run.PrimaryInstrument());
  Simulator sim(run, *strategy);
  sim.set_recorders(&recorders);
  sim.set_integrity_check_every(1000);
  sim.Run(events);
  recorders.WriteMarkouts(sim.markouts());
  recorders.WriteDailyPnl(sim.markouts().Summarise());
  recorders.WriteQueueStats(sim.queue_tracker().stats());
  recorders.CloseAll();

  // Invariants that must hold regardless of what the golden bytes say.
  EXPECT_EQ(sim.ledger().IdentityResidualX2(), 0) << sim.ledger().IdentityReport();
  EXPECT_EQ(sim.stats().book_integrity_failures, 0u);
  EXPECT_GT(sim.stats().market_events, 10'000u);

  bool compared = false;
  std::vector<std::string> notes;
  for (const char* name : {"fills.csv", "markouts.csv", "pnl_daily.csv", "queue_stats.csv"}) {
    const std::string actual = testing::ReadFileBytes((out_dir / name).string());
    std::string note;
    if (CompareOrBootstrap(FixtureDir() / "golden" / name, actual, &note)) {
      compared = true;
    } else {
      notes.push_back(note);
    }
  }
  if (!compared) {
    std::string joined;
    for (const std::string& n : notes) {
      joined += n + "; ";
    }
    GTEST_SKIP() << "golden expectations bootstrapped (" << joined
                 << "). Review, commit, and this becomes a hard gate.";
  }
}

TEST(GoldenReplay, ReplayingTheFixtureTwiceGivesIdenticalLedgerState) {
  const std::vector<Event> events = BuildFixtureEvents();
  const RunConfig config = GoldenConfig();

  auto run = [&]() {
    std::unique_ptr<Strategy> s = MakeStrategy(config.strategy);
    Simulator sim(config, *s);
    sim.Run(events);
    return std::make_tuple(sim.ledger().EquityX2(), sim.ledger().spread_capture_x2(),
                           sim.ledger().inventory_pnl_x2(), sim.ledger().fees_x2(),
                           sim.stats().fills, sim.rng().draws());
  };
  EXPECT_EQ(run(), run());
}

}  // namespace
}  // namespace lob
