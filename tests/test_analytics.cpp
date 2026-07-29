// Markout sampling, date bucketing and CSV determinism (§3.9, §4.8).
#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <lob/analytics/markout.hpp>
#include <lob/analytics/recorders.hpp>
#include <lob/csv_writer.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

MarkoutSampler MakeSampler(std::vector<Ts> horizons) {
  return MarkoutSampler(testing::WorkedTraceInstrument(), std::move(horizons));
}

TEST(Markout, EdgeAndAdverseSelectionDecomposeTheMarkout) {
  MarkoutSampler s = MakeSampler({1 * kUsPerSecond});
  s.Advance(0, 20001);  // mid 100.005

  // Buy 0.5 at 100.00 with the mid at 100.005: edge = 0.5 bp on a $50 notional.
  s.AddFill(0, Side::kBid, 10000, 5, 20001);
  // One second later the mid has fallen to 100.00 -- classic maker outcome.
  s.Advance(1 * kUsPerSecond, 20000);

  ASSERT_EQ(s.samples().size(), 1u);
  const MarkoutSample& m = s.samples().front();
  ASSERT_TRUE(m.resolved);
  EXPECT_NEAR(m.edge_bp, 0.5, 1e-9);
  EXPECT_NEAR(m.adverse_selection_bp, -0.5, 1e-9);
  EXPECT_NEAR(m.markout_bp, m.edge_bp + m.adverse_selection_bp, 1e-12);
}

TEST(Markout, SignFlipsWithOurSide) {
  MarkoutSampler s = MakeSampler({1 * kUsPerSecond});
  s.Advance(0, 20000);
  s.AddFill(0, Side::kAsk, 10001, 10, 20000);  // sell above the mid
  s.Advance(1 * kUsPerSecond, 20020);          // mid rises: bad for a seller

  const MarkoutSample& m = s.samples().front();
  ASSERT_TRUE(m.resolved);
  EXPECT_GT(m.edge_bp, 0.0);               // sold above the mid
  EXPECT_LT(m.adverse_selection_bp, 0.0);  // and the market ran away
}

TEST(Markout, ResolvesAtTheMidInForceAtTheHorizonNotTheNextOne) {
  MarkoutSampler s = MakeSampler({1 * kUsPerSecond});
  s.Advance(0, 20000);
  s.AddFill(0, Side::kBid, 10000, 10, 20000);
  // The mid in force from t=0 until t=2 s is 20000; the sample due at t=1 s
  // must use it, not the 20100 that only arrives later.
  s.Advance(2 * kUsPerSecond, 20100);
  const MarkoutSample& m = s.samples().front();
  ASSERT_TRUE(m.resolved);
  EXPECT_EQ(m.mid_x2_at_horizon, 20000);
  EXPECT_NEAR(m.adverse_selection_bp, 0.0, 1e-12);
}

TEST(Markout, UnresolvedSamplesAreReportedNotBackFilled) {
  // A fill near the end of a run has no mid 60 s later.  Back-filling with the
  // last known mid would silently bias long horizons toward zero adverse
  // selection -- exactly the kind of quiet lie CLAUDE.md rule 4 forbids.
  MarkoutSampler s = MakeSampler({1 * kUsPerSecond, 60 * kUsPerSecond});
  s.Advance(0, 20000);
  s.AddFill(0, Side::kBid, 10000, 10, 20000);
  s.Advance(2 * kUsPerSecond, 19990);

  ASSERT_EQ(s.samples().size(), 2u);
  EXPECT_TRUE(s.samples()[0].resolved);   // h = 1 s
  EXPECT_FALSE(s.samples()[1].resolved);  // h = 60 s never came
  EXPECT_EQ(s.pending(), 1u);

  const std::vector<MarkoutSummary> summary = s.Summarise();
  ASSERT_EQ(summary.size(), 2u);
  EXPECT_EQ(summary[0].resolved, 1u);
  EXPECT_EQ(summary[1].resolved, 0u);
  EXPECT_EQ(summary[1].unresolved, 1u);
  // An unresolved horizon contributes nothing to the mean, rather than a zero.
  EXPECT_DOUBLE_EQ(summary[1].mean_markout_bp, 0.0);
}

TEST(Markout, HorizonsAreSortedAndDeduplicated) {
  MarkoutSampler s = MakeSampler({5 * kUsPerSecond, 1 * kUsPerSecond, 5 * kUsPerSecond});
  ASSERT_EQ(s.horizons_us().size(), 2u);
  EXPECT_EQ(s.horizons_us()[0], 1 * kUsPerSecond);
  EXPECT_EQ(s.horizons_us()[1], 5 * kUsPerSecond);
}

TEST(Markout, ManyFillsEachGetOneSamplePerHorizon) {
  MarkoutSampler s = MakeSampler({1 * kUsPerSecond, 2 * kUsPerSecond});
  s.Advance(0, 20000);
  for (int i = 0; i < 10; ++i) {
    s.AddFill(static_cast<Ts>(i) * 100'000, Side::kBid, 10000, 1, 20000);
  }
  s.Advance(20 * kUsPerSecond, 20000);
  EXPECT_EQ(s.samples().size(), 20u);
  for (const MarkoutSample& m : s.samples()) {
    EXPECT_TRUE(m.resolved);
  }
}

// ---------------------------------------------------------------------------
// Date bucketing
// ---------------------------------------------------------------------------
TEST(UtcDate, MatchesKnownEpochValues) {
  EXPECT_EQ(UtcDateString(0), "1970-01-01");
  EXPECT_EQ(UtcDateString(86'399 * kUsPerSecond), "1970-01-01");
  EXPECT_EQ(UtcDateString(86'400 * kUsPerSecond), "1970-01-02");
  // 2026-07-29T00:00:00Z = 1785283200
  EXPECT_EQ(UtcDateString(1'785'283'200LL * kUsPerSecond), "2026-07-29");
  // A leap day.
  EXPECT_EQ(UtcDateString(1'709'164'800LL * kUsPerSecond), "2024-02-29");
}

TEST(UtcDate, DayNumbersAreContiguousAcrossBoundaries) {
  const Ts start = 1'785'283'200LL * kUsPerSecond;
  EXPECT_EQ(UtcDayNumber(start + 86'400 * kUsPerSecond) - UtcDayNumber(start), 1);
  EXPECT_EQ(UtcDayNumber(start), UtcDayNumber(start + 86'399 * kUsPerSecond));
}

// ---------------------------------------------------------------------------
// CSV determinism
// ---------------------------------------------------------------------------
TEST(CsvWriter, FormatsDoublesIdenticallyEveryTime) {
  // operator<< honours the stream locale and default precision; to_chars does
  // not.  Byte-comparable output is what the determinism test rests on.
  EXPECT_EQ(CsvWriter::FormatDouble(1.5, 4), "1.5000");
  EXPECT_EQ(CsvWriter::FormatDouble(-0.000000001, 6), "0.000000");  // negative zero normalised
  EXPECT_EQ(CsvWriter::FormatDouble(0.0, 2), "0.00");
  EXPECT_EQ(CsvWriter::FormatDouble(-2.5, 1), "-2.5");
  EXPECT_EQ(CsvWriter::FormatDouble(1.0 / 3.0, 10), "0.3333333333");
}

TEST(CsvWriter, SpellsOutNonFiniteValues) {
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(CsvWriter::FormatDouble(inf, 4), "inf");
  EXPECT_EQ(CsvWriter::FormatDouble(-inf, 4), "-inf");
  EXPECT_EQ(CsvWriter::FormatDouble(std::numeric_limits<double>::quiet_NaN(), 4), "nan");
}

TEST(CsvWriter, WritesHeaderAndRowsAndQuotesWhereNeeded) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "lob_sim_tests" / "csv_test.csv").string();
  {
    CsvWriter w(path, {"a", "b", "c"});
    w.Row(std::int64_t{1}, std::string("plain"), 2.5);
    w.Row(std::int64_t{2}, std::string("has,comma"), 0.0);
    w.Row(std::int64_t{3}, std::string("has\"quote"), -1.0);
    w.Close();
  }
  const std::string text = testing::ReadFileBytes(path);
  EXPECT_NE(text.find("a,b,c\n"), std::string::npos);
  EXPECT_NE(text.find("\"has,comma\""), std::string::npos);
  EXPECT_NE(text.find("\"has\"\"quote\""), std::string::npos);
  // Binary mode: LF only, so Windows and Linux runs compare equal.
  EXPECT_EQ(text.find('\r'), std::string::npos);
}

TEST(CsvWriter, RejectsRowsWithTheWrongColumnCount) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "lob_sim_tests" / "csv_bad.csv").string();
  CsvWriter w(path, {"a", "b"});
  w.Field(std::int64_t{1});
  EXPECT_THROW(w.EndRow(), std::logic_error);
}

}  // namespace
}  // namespace lob
