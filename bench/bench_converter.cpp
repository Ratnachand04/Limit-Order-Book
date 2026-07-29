// Converter throughput (master plan §4.10, CLAUDE.md Phase 1 gate: >= 1M msg/s).
//
// Part 11 pitfall #2 is the reason this benchmark exists: "JSON parsing costs
// 100x the book update; convert once".  The comparison between the JSON path
// and the binary path below is the measurement that justifies the whole
// two-stage pipeline, and it is the number the CV line quotes.
#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include <lob/converter/binance.hpp>
#include <lob/converter/converter.hpp>
#include <lob/converter/event_io.hpp>
#include <lob/rng.hpp>

#include "test_support.hpp"

namespace {

using namespace lob;

Instrument BenchInstrument() { return Instrument("BTCUSDT", 0, 0.01, 0.001); }

// Recorder lines in exactly the shape recorder.py writes.
std::vector<std::string> MakeDepthLines(std::size_t n, std::uint64_t seed) {
  Rng rng(seed);
  std::vector<std::string> lines;
  lines.reserve(n);
  long long update_id = 1000;
  double mid = 100000.0;
  for (std::size_t i = 0; i < n; ++i) {
    mid += (rng.Uniform01() - 0.5) * 0.02;
    const long long first = update_id + 1;
    const int levels = 1 + static_cast<int>(rng.UniformInt(0, 4));
    std::string bids;
    std::string asks;
    for (int l = 0; l < levels; ++l) {
      const double bpx = mid - 0.01 * (l + 1);
      const double apx = mid + 0.01 * (l + 1);
      if (l != 0) {
        bids += ",";
        asks += ",";
      }
      bids += "[\"" + std::to_string(bpx) + "\",\"" + std::to_string(rng.Uniform01() * 5.0) + "\"]";
      asks += "[\"" + std::to_string(apx) + "\",\"" + std::to_string(rng.Uniform01() * 5.0) + "\"]";
    }
    update_id += levels;
    lines.push_back(R"({"t":)" + std::to_string(1'700'000'000'000'000LL + static_cast<long long>(i) * 1000) +
                    R"(,"c":"depth","s":"BTCUSDT","d":{"e":"depthUpdate","E":)" +
                    std::to_string(1'700'000'000'000LL + static_cast<long long>(i)) +
                    R"(,"s":"BTCUSDT","U":)" + std::to_string(first) + R"(,"u":)" +
                    std::to_string(update_id) + R"(,"b":[)" + bids + R"(],"a":[)" + asks +
                    R"(]}})");
  }
  return lines;
}

std::string SnapshotLine() {
  return R"({"t":1700000000000000,"c":"snapshot","s":"BTCUSDT","d":{"lastUpdateId":1000,)"
         R"("bids":[["99999.99","5.0"]],"asks":[["100000.01","4.0"]]}})";
}

// --- end-to-end conversion --------------------------------------------------
void BM_ConvertDepthLines(benchmark::State& state) {
  const std::vector<std::string> lines = MakeDepthLines(20'000, 7);
  ConverterOptions options;
  options.instrument = BenchInstrument();

  std::uint64_t messages = 0;
  for (auto _ : state) {
    Converter converter(options);
    converter.ProcessLine(SnapshotLine(), 0);
    std::uint64_t n = 1;
    for (const std::string& line : lines) {
      converter.ProcessLine(line, n++);
    }
    benchmark::DoNotOptimize(converter.stats().events_emitted);
    messages += lines.size();
  }
  // Items are MESSAGES, which is what the >= 1M msg/s target is quoted in.
  state.SetItemsProcessed(static_cast<std::int64_t>(messages));
}
BENCHMARK(BM_ConvertDepthLines)->Unit(benchmark::kMillisecond);

// --- the parse step alone ---------------------------------------------------
void BM_ParseDepthPayloadOnly(benchmark::State& state) {
  const std::vector<std::string> lines = MakeDepthLines(4096, 11);
  const Instrument inst = BenchInstrument();
  std::vector<Event> events;
  events.reserve(64);

  std::size_t i = 0;
  for (auto _ : state) {
    binance::RecorderLine env;
    binance::ParseRecorderLine(lines[i & 4095], env, nullptr);
    binance::DepthUpdate update;
    events.clear();
    binance::ParseDepthUpdate(env.payload, inst, update, events, nullptr);
    benchmark::DoNotOptimize(events.size());
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseDepthPayloadOnly);

// --- the payoff -------------------------------------------------------------
// Reading the SAME information back from the binary format.  The ratio between
// this and BM_ConvertDepthLines is the "parse once" argument, measured.
void BM_ReadBinaryEvents(benchmark::State& state) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 5, 1'700'000'000'000'000LL, 300 * kUsPerSecond, 200);
  const std::string path = testing::WriteTempEvents("bench_read", events);

  std::uint64_t records = 0;
  for (auto _ : state) {
    EventReader reader(path);
    const Event* block = nullptr;
    std::size_t n = 0;
    std::int64_t checksum = 0;
    while ((n = reader.NextBlock(block)) > 0) {
      for (std::size_t i = 0; i < n; ++i) {
        checksum += block[i].price_ticks;
      }
      records += n;
    }
    benchmark::DoNotOptimize(checksum);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(records));
}
BENCHMARK(BM_ReadBinaryEvents)->Unit(benchmark::kMillisecond);

void BM_WriteBinaryEvents(benchmark::State& state) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 6, 1'700'000'000'000'000LL, 120 * kUsPerSecond, 200);
  std::uint64_t records = 0;
  for (auto _ : state) {
    testing::WriteTempEvents("bench_write", events);
    records += events.size();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(records));
}
BENCHMARK(BM_WriteBinaryEvents)->Unit(benchmark::kMillisecond);

}  // namespace
