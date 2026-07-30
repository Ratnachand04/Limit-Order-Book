// Whole-replay throughput (master plan §4.10: >= 300-500k events/s,
// single-threaded, with book + strategy in the loop).
//
// The strategy ladder is benchmarked separately so the cost of the model is
// visible: S0 is a book lookup, S2 evaluates the A-S closed form on every
// timer.  If S2 is much slower than S0, that is the price of the model, and it
// belongs in the paper rather than in a footnote.
#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <vector>

#include <lob/sim/simulator.hpp>
#include <lob/strategy/factory.hpp>

#include "test_support.hpp"

namespace {

using namespace lob;

const std::vector<Event>& Session() {
  // 10 minutes at 200 events/s -- large enough that per-run setup disappears.
  static const std::vector<Event> kEvents = testing::SyntheticSession(
      testing::TestInstrument(), /*seed=*/2026, 1'700'000'000'000'000LL,
      /*duration_us=*/600 * kUsPerSecond, /*events_per_second=*/200);
  return kEvents;
}

RunConfig BenchConfig(const std::string& strategy_name, QueueModel queue) {
  RunConfig c = testing::MakeTestConfig(testing::TestInstrument(), queue, 50 * kUsPerMilli);
  c.latency.jitter_exp_us = 5000.0;
  c.fees.maker_tenth_bp = 20;
  c.strategy.name = strategy_name;
  c.strategy.glft_mode = (strategy_name == "S2_GLFT");
  c.strategy.use_weighted_mid = (strategy_name == "S3");
  c.fill_model = (strategy_name == "S0_touch") ? "TOUCH" : "QUEUE";
  c.strategy.enforce_min_edge = false;
  c.strategy.q_max_lots = 20;
  c.strategy.sigma_window_us = 60 * kUsPerSecond;
  c.strategy.k_fallback = 0.5;
  return c;
}

void RunStrategyBenchmark(benchmark::State& state, const std::string& name, QueueModel queue) {
  const std::vector<Event>& events = Session();
  const RunConfig config = BenchConfig(name, queue);

  std::uint64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
    Simulator sim(config, *strategy);
    state.ResumeTiming();

    const SimulatorStats& stats = sim.Run(events);
    // Copied into a local first: the const-ref overload of DoNotOptimize is
    // deprecated precisely because it can still permit the optimisation it is
    // meant to prevent.
    std::uint64_t fills = stats.fills;
    benchmark::DoNotOptimize(fills);
    processed += stats.market_events;
  }
  // Items are MARKET EVENTS: the unit the 300-500k events/s target is in.
  state.SetItemsProcessed(static_cast<std::int64_t>(processed));
  state.counters["events"] = static_cast<double>(events.size());
}

void BM_ReplayS0Queue(benchmark::State& state) {
  RunStrategyBenchmark(state, "S0_queue", QueueModel::kPess);
}
BENCHMARK(BM_ReplayS0Queue)->Unit(benchmark::kMillisecond);

void BM_ReplayS0Touch(benchmark::State& state) {
  RunStrategyBenchmark(state, "S0_touch", QueueModel::kPess);
}
BENCHMARK(BM_ReplayS0Touch)->Unit(benchmark::kMillisecond);

void BM_ReplayS1(benchmark::State& state) {
  RunStrategyBenchmark(state, "S1", QueueModel::kPess);
}
BENCHMARK(BM_ReplayS1)->Unit(benchmark::kMillisecond);

void BM_ReplayS2AS(benchmark::State& state) {
  RunStrategyBenchmark(state, "S2_AS", QueueModel::kPess);
}
BENCHMARK(BM_ReplayS2AS)->Unit(benchmark::kMillisecond);

void BM_ReplayS3(benchmark::State& state) {
  RunStrategyBenchmark(state, "S3", QueueModel::kPess);
}
BENCHMARK(BM_ReplayS3)->Unit(benchmark::kMillisecond);

// The proportional queue model draws from the RNG on every unexplained level
// decrease; PESS and OPT do not.  This measures what that costs.
void BM_ReplayProportionalQueue(benchmark::State& state) {
  RunStrategyBenchmark(state, "S1", QueueModel::kProp);
}
BENCHMARK(BM_ReplayProportionalQueue)->Unit(benchmark::kMillisecond);

// The dual-book mode is a debug/CI configuration, not a production one; this
// quantifies the factor so nobody accidentally benchmarks with it on.
void BM_ReplayDualBook(benchmark::State& state) {
  const std::vector<Event>& events = Session();
  RunConfig config = BenchConfig("S1", QueueModel::kPess);
  config.dual_book_check = true;

  std::uint64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
    Simulator sim(config, *strategy);
    state.ResumeTiming();
    const SimulatorStats& stats = sim.Run(events);
    processed += stats.market_events;
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(processed));
}
BENCHMARK(BM_ReplayDualBook)->Unit(benchmark::kMillisecond);

// Probes multiply the number of tracked orders; this is what RQ2 costs.
void BM_ReplayWithProbes(benchmark::State& state) {
  const std::vector<Event>& events = Session();
  RunConfig config = BenchConfig("S1", QueueModel::kPess);
  config.probes.enabled = true;
  config.probes.interval_us = 5 * kUsPerSecond;
  config.probes.depths_ticks = {1, 2, 3, 5, 10};
  config.probes.horizons_us = {1 * kUsPerSecond, 5 * kUsPerSecond, 30 * kUsPerSecond};

  std::uint64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    std::unique_ptr<Strategy> strategy = MakeStrategy(config.strategy);
    Simulator sim(config, *strategy);
    state.ResumeTiming();
    const SimulatorStats& stats = sim.Run(events);
    processed += stats.market_events;
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(processed));
}
BENCHMARK(BM_ReplayWithProbes)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
