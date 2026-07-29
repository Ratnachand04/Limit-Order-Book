// The YAML subset reader and the typed experiment configuration
// (master plan Appendix A).
#include <gtest/gtest.h>

#include <string>

#include <lob/config.hpp>
#include <lob/yaml.hpp>

namespace lob {
namespace {

// The Appendix A default parameter file, verbatim.
constexpr const char* kAppendixA = R"(
symbols: [BTCUSDT, ETHUSDT, MIDCAP1, MIDCAP2]   # fixed in week 0
tick_size: {BTCUSDT: 0.01, ETHUSDT: 0.01, MIDCAP1: 0.001, MIDCAP2: 0.0001}
lot_size:  {BTCUSDT: 0.001, ETHUSDT: 0.001, MIDCAP1: 0.01, MIDCAP2: 0.1}
latency_ms: {in: 50, out: 50, jitter_exp_ms: 5}
queue_model: PESS            # PESS | OPT | PROP
fees_bp: {maker: 2.0}
strategy:
  name: S2_AS                # S0_touch|S0_queue|S1|S2_AS|S2_GLFT|S3
  gamma: 1.0e-5
  horizon_s: 60
  sigma_window_s: 600
  k_A_calibration: calib/kA_2026wk34.json
  requote_min_ticks: 2
  q_max: 5
  timer_ms: 100
seed: 42
eval_window: {start: 2026-09-01, end: 2026-10-10}
)";

// ---------------------------------------------------------------------------
// YAML subset
// ---------------------------------------------------------------------------
TEST(Yaml, ParsesBlockMappingsAndScalars) {
  const yaml::Node n = yaml::Parse("a: 1\nb: hello\nc: true\nd: 2.5\n");
  EXPECT_TRUE(n.IsMap());
  EXPECT_EQ(n["a"].AsInt("a"), 1);
  EXPECT_EQ(n["b"].AsString("b"), "hello");
  EXPECT_TRUE(n["c"].AsBool("c"));
  EXPECT_DOUBLE_EQ(n["d"].AsDouble("d"), 2.5);
}

TEST(Yaml, ParsesNestedBlockMappings) {
  const yaml::Node n = yaml::Parse("outer:\n  inner:\n    leaf: 7\n  sibling: 8\ntop: 9\n");
  EXPECT_EQ(n["outer"]["inner"]["leaf"].AsInt("leaf"), 7);
  EXPECT_EQ(n["outer"]["sibling"].AsInt("sibling"), 8);
  EXPECT_EQ(n["top"].AsInt("top"), 9);
}

TEST(Yaml, ParsesFlowSequencesAndMappings) {
  const yaml::Node n = yaml::Parse("s: [a, b, c]\nm: {x: 1, y: 2}\n");
  ASSERT_TRUE(n["s"].IsSequence());
  EXPECT_EQ(n["s"].size(), 3u);
  EXPECT_EQ(n["s"][0].AsString(""), "a");
  EXPECT_EQ(n["s"][2].AsString(""), "c");
  ASSERT_TRUE(n["m"].IsMap());
  EXPECT_EQ(n["m"]["y"].AsInt("y"), 2);
}

TEST(Yaml, ParsesBlockSequences) {
  const yaml::Node n = yaml::Parse("items:\n  - one\n  - two\n  - three\n");
  ASSERT_TRUE(n["items"].IsSequence());
  EXPECT_EQ(n["items"].size(), 3u);
  EXPECT_EQ(n["items"][1].AsString(""), "two");
}

TEST(Yaml, ParsesSequencesOfMappings) {
  const yaml::Node n = yaml::Parse("runs:\n  - name: a\n    fee: 1\n  - name: b\n    fee: 2\n");
  ASSERT_EQ(n["runs"].size(), 2u);
  EXPECT_EQ(n["runs"][0]["name"].AsString(""), "a");
  EXPECT_EQ(n["runs"][1]["fee"].AsInt(""), 2);
}

TEST(Yaml, StripsCommentsButNotInsideQuotes) {
  const yaml::Node n = yaml::Parse("a: 1  # trailing\n# whole line\nb: \"has # hash\"\n");
  EXPECT_EQ(n["a"].AsInt("a"), 1);
  EXPECT_EQ(n["b"].AsString("b"), "has # hash");
}

TEST(Yaml, HandlesScientificNotationAndNegatives) {
  const yaml::Node n = yaml::Parse("g: 1.0e-5\nn: -0.5\ni: 1e3\n");
  EXPECT_DOUBLE_EQ(n["g"].AsDouble("g"), 1.0e-5);
  EXPECT_DOUBLE_EQ(n["n"].AsDouble("n"), -0.5);
  // A float that happens to be integral is accepted as an integer.
  EXPECT_EQ(n["i"].AsInt("i"), 1000);
}

TEST(Yaml, MissingKeysYieldNullAndChainSafely) {
  const yaml::Node n = yaml::Parse("a: 1\n");
  EXPECT_TRUE(n["nope"].IsNull());
  EXPECT_TRUE(n["nope"]["deeper"].IsNull());  // must not dereference nothing
  EXPECT_EQ(n["nope"].AsIntOr(99), 99);
  EXPECT_EQ(n["nope"].AsStringOr("fallback"), "fallback");
  EXPECT_THROW(n["nope"].AsInt("nope"), yaml::ParseError);
}

TEST(Yaml, RejectsUnsupportedConstructsLoudly) {
  // Failing loudly beats silently mis-parsing a config that decides results.
  EXPECT_THROW(yaml::Parse("a: |\n  block scalar\n"), yaml::ParseError);
  EXPECT_THROW(yaml::Parse("a: &anchor 1\n"), yaml::ParseError);
  EXPECT_THROW(yaml::Parse("\ta: 1\n"), yaml::ParseError);
  EXPECT_THROW(yaml::Parse("a: [1, 2\n"), yaml::ParseError);
}

// ---------------------------------------------------------------------------
// RunConfig
// ---------------------------------------------------------------------------
TEST(RunConfig, LoadsTheAppendixADefaults) {
  const RunConfig c = RunConfig::FromYaml(yaml::Parse(kAppendixA));

  ASSERT_EQ(c.instruments.size(), 4u);
  EXPECT_EQ(c.instruments[0].symbol(), "BTCUSDT");
  EXPECT_EQ(c.instruments[0].symbol_id(), 0);
  EXPECT_DOUBLE_EQ(c.instruments[0].tick_size(), 0.01);
  EXPECT_EQ(c.instruments[3].symbol(), "MIDCAP2");
  EXPECT_EQ(c.instruments[3].symbol_id(), 3);

  EXPECT_EQ(c.latency.in_us, 50'000);
  EXPECT_EQ(c.latency.out_us, 50'000);
  EXPECT_DOUBLE_EQ(c.latency.jitter_exp_us, 5000.0);

  EXPECT_EQ(c.queue_model, QueueModel::kPess);
  EXPECT_EQ(c.fees.maker_tenth_bp, 20);
  EXPECT_DOUBLE_EQ(c.fees.maker_bp(), 2.0);

  EXPECT_EQ(c.strategy.name, "S2_AS");
  EXPECT_DOUBLE_EQ(c.strategy.gamma, 1.0e-5);
  EXPECT_DOUBLE_EQ(c.strategy.horizon_s, 60.0);
  EXPECT_EQ(c.strategy.sigma_window_us, 600 * kUsPerSecond);
  EXPECT_EQ(c.strategy.k_a_calibration_path, "calib/kA_2026wk34.json");
  EXPECT_EQ(c.strategy.requote_min_ticks, 2);
  EXPECT_EQ(c.strategy.q_max_lots, 5);
  EXPECT_EQ(c.strategy.timer_us, 100'000);

  EXPECT_EQ(c.seed, 42u);
  EXPECT_EQ(c.eval_start, "2026-09-01");
  EXPECT_EQ(c.eval_end, "2026-10-10");
  // S2_AS is not the strawman, so the honest fill model is the default.
  EXPECT_EQ(c.fill_model, "QUEUE");
}

TEST(RunConfig, TouchFillModelIsTheDefaultOnlyForTheStrawman) {
  const RunConfig s0 = RunConfig::FromYaml(yaml::Parse(
      "symbols: [X]\ntick_size: {X: 0.01}\nlot_size: {X: 0.001}\nstrategy:\n  name: S0_touch\n"));
  EXPECT_EQ(s0.fill_model, "TOUCH");

  const RunConfig s1 = RunConfig::FromYaml(yaml::Parse(
      "symbols: [X]\ntick_size: {X: 0.01}\nlot_size: {X: 0.001}\nstrategy:\n  name: S1\n"));
  EXPECT_EQ(s1.fill_model, "QUEUE");
}

TEST(RunConfig, GeneratesATraceableRunIdWhenNoneIsGiven) {
  const RunConfig c = RunConfig::FromYaml(yaml::Parse(kAppendixA));
  EXPECT_NE(c.run_id.find("S2_AS"), std::string::npos);
  EXPECT_NE(c.run_id.find("PESS"), std::string::npos);
  EXPECT_NE(c.run_id.find("seed42"), std::string::npos);
}

TEST(RunConfig, ManifestRecordsEveryDecidingParameter) {
  const RunConfig c = RunConfig::FromYaml(yaml::Parse(kAppendixA));
  const std::string m = c.Manifest();
  for (const char* needle : {"run_id:", "seed: 42", "queue_model: PESS", "fill_model: QUEUE",
                             "name: S2_AS", "gamma:", "BTCUSDT"}) {
    EXPECT_NE(m.find(needle), std::string::npos) << "manifest is missing " << needle;
  }
}

TEST(RunConfig, RejectsFeesFinerThanTheLedgerResolution) {
  // The ledger holds fees in tenths of a basis point.  A 2.05 bp fee cannot be
  // represented, and silently truncating it would corrupt the fee frontier.
  EXPECT_THROW(RunConfig::FromYaml(yaml::Parse("symbols: [X]\ntick_size: {X: 0.01}\n"
                                               "lot_size: {X: 0.001}\nfees_bp: {maker: 2.05}\n")),
               std::invalid_argument);
}

TEST(RunConfig, RejectsMissingOrContradictoryFields) {
  EXPECT_THROW(RunConfig::FromYaml(yaml::Parse("tick_size: {X: 0.01}\n")), std::invalid_argument);
  EXPECT_THROW(RunConfig::FromYaml(yaml::Parse("symbols: [X]\nlot_size: {X: 0.001}\n")),
               std::invalid_argument);
  // A symbol with no tick size defined.
  EXPECT_THROW(RunConfig::FromYaml(yaml::Parse(
                   "symbols: [X, Y]\ntick_size: {X: 0.01}\nlot_size: {X: 0.001}\n")),
               std::invalid_argument);
  // gamma <= 0 has no finite A-S solution.
  EXPECT_THROW(RunConfig::FromYaml(yaml::Parse("symbols: [X]\ntick_size: {X: 0.01}\n"
                                               "lot_size: {X: 0.001}\nstrategy:\n  gamma: 0\n")),
               std::invalid_argument);
}

TEST(RunConfig, ParsesEveryQueueModelSpelling) {
  EXPECT_EQ(ParseQueueModel("PESS"), QueueModel::kPess);
  EXPECT_EQ(ParseQueueModel("opt"), QueueModel::kOpt);
  EXPECT_EQ(ParseQueueModel("proportional"), QueueModel::kProp);
  EXPECT_THROW(ParseQueueModel("MAYBE"), std::invalid_argument);
  EXPECT_EQ(QueueModelName(QueueModel::kPess), "PESS");
}

TEST(RunConfig, MarkoutHorizonsMustBeStrictlyIncreasing) {
  EXPECT_THROW(
      RunConfig::FromYaml(yaml::Parse("symbols: [X]\ntick_size: {X: 0.01}\nlot_size: {X: 0.001}\n"
                                      "markouts:\n  horizons_s: [1, 1, 5]\n")),
      std::invalid_argument);
}

TEST(RunConfig, DefaultMarkoutGridMatchesThePlan) {
  // CLAUDE.md Phase 5: h in {0.1, 0.5, 1, 2, 5, 10, 30, 60} s.
  const RunConfig c = RunConfig::FromYaml(yaml::Parse(kAppendixA));
  const std::vector<Ts> expected = {100'000,    500'000,    1'000'000,  2'000'000,
                                    5'000'000,  10'000'000, 30'000'000, 60'000'000};
  EXPECT_EQ(c.markouts.horizons_us, expected);
}

}  // namespace
}  // namespace lob
