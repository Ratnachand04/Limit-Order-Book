// Book-core micro-benchmarks (master plan §4.10).
//
// The headline figure the CV line quotes is `apply_depth` p50 < 100 ns and
// p99 < 1 us on the dense path.  Google Benchmark reports means and medians but
// not tail percentiles, so the percentile benchmarks below time each call
// individually and report p50/p90/p99/p999 as counters.
//
// Timing individual calls costs more than the call itself, so those numbers are
// an UPPER BOUND on the true latency.  The throughput benchmarks alongside them
// have no per-call timing overhead and are the honest measure of rate.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <lob/book/dense_book.hpp>
#include <lob/book/map_book.hpp>
#include <lob/rng.hpp>

#include "test_support.hpp"

namespace {

using namespace lob;

struct Update {
  Side side;
  Ticks price;
  Lots64 qty;
};

// A realistic update mix: mostly quantity churn within a few ticks of the
// touch, some deletions, an occasional level far from the mid.
std::vector<Update> MakeUpdates(std::size_t n, std::uint64_t seed) {
  Rng rng(seed);
  std::vector<Update> out;
  out.reserve(n);
  Ticks bid = 10'000'000;  // BTC-like: $100,000 at a $0.01 tick
  for (std::size_t i = 0; i < n; ++i) {
    const double u = rng.Uniform01();
    if (u < 0.08) {
      bid += (rng.Uniform01() < 0.5) ? 1 : -1;
    }
    const bool is_bid = rng.Uniform01() < 0.5;
    const Ticks offset = static_cast<Ticks>(rng.UniformInt(0, u < 0.9 ? 20 : 2000));
    const Ticks px = is_bid ? bid - offset : bid + 1 + offset;
    const Lots64 qty = rng.Uniform01() < 0.2 ? 0 : rng.UniformInt(1, 5000);
    out.push_back(Update{is_bid ? Side::kBid : Side::kAsk, px, qty});
  }
  return out;
}

template <typename BookT>
void Prime(BookT& book, Ticks centre) {
  for (Ticks d = 0; d < 50; ++d) {
    book.ApplyDepth(Side::kBid, centre - d, 100 + d);
    book.ApplyDepth(Side::kAsk, centre + 1 + d, 100 + d);
  }
}

// --- throughput -------------------------------------------------------------
template <typename BookT>
void BM_ApplyDepthThroughput(benchmark::State& state) {
  const std::vector<Update> updates = MakeUpdates(1 << 16, 12345);
  BookT book;
  Prime(book, 10'000'000);

  std::size_t i = 0;
  for (auto _ : state) {
    const Update& u = updates[i & (updates.size() - 1)];
    book.ApplyDepth(u.side, u.price, u.qty);
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
  benchmark::DoNotOptimize(book.BestBid());
}
BENCHMARK(BM_ApplyDepthThroughput<DenseBook>)->Name("DenseBook/ApplyDepth/throughput");
BENCHMARK(BM_ApplyDepthThroughput<MapBook>)->Name("MapBook/ApplyDepth/throughput");

// --- tail latency -----------------------------------------------------------
template <typename BookT>
void BM_ApplyDepthPercentiles(benchmark::State& state) {
  const std::vector<Update> updates = MakeUpdates(1 << 16, 999);
  BookT book;
  Prime(book, 10'000'000);

  std::vector<double> samples;
  samples.reserve(1 << 20);

  std::size_t i = 0;
  for (auto _ : state) {
    const Update& u = updates[i & (updates.size() - 1)];
    const auto t0 = std::chrono::steady_clock::now();
    book.ApplyDepth(u.side, u.price, u.qty);
    const auto t1 = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    ++i;
  }
  if (samples.empty()) {
    return;
  }
  std::sort(samples.begin(), samples.end());
  auto pct = [&samples](double p) {
    const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
    return samples[idx];
  };
  state.counters["p50_ns"] = pct(0.50);
  state.counters["p90_ns"] = pct(0.90);
  state.counters["p99_ns"] = pct(0.99);
  state.counters["p999_ns"] = pct(0.999);
  state.counters["max_ns"] = samples.back();
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ApplyDepthPercentiles<DenseBook>)->Name("DenseBook/ApplyDepth/percentiles");
BENCHMARK(BM_ApplyDepthPercentiles<MapBook>)->Name("MapBook/ApplyDepth/percentiles");

// --- the re-anchor path -----------------------------------------------------
// At a $0.01 tick the +/-32768-tick window spans only +/-$327, so on BTC this
// runs several times a day.  It is a normal operation and its cost belongs in
// the numbers.
void BM_DenseReanchor(benchmark::State& state) {
  DenseBook book;
  Prime(book, 10'000'000);
  Ticks anchor = 10'000'000;
  for (auto _ : state) {
    anchor += DenseBook::kHalfWindow / 2 + 1;
    book.Reanchor(anchor);
    benchmark::DoNotOptimize(book.anchor());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DenseReanchor)->Name("DenseBook/Reanchor");

// --- queries ----------------------------------------------------------------
template <typename BookT>
void BM_BestBid(benchmark::State& state) {
  BookT book;
  Prime(book, 10'000'000);
  for (auto _ : state) {
    benchmark::DoNotOptimize(book.BestBid());
    benchmark::DoNotOptimize(book.BestAsk());
  }
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_BestBid<DenseBook>)->Name("DenseBook/BestBidAsk");
BENCHMARK(BM_BestBid<MapBook>)->Name("MapBook/BestBidAsk");

template <typename BookT>
void BM_DepthWithin(benchmark::State& state) {
  BookT book;
  Prime(book, 10'000'000);
  const Ticks n = static_cast<Ticks>(state.range(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(book.DepthWithin(Side::kBid, n));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DepthWithin<DenseBook>)->Name("DenseBook/DepthWithin")->Arg(1)->Arg(5)->Arg(20);
BENCHMARK(BM_DepthWithin<MapBook>)->Name("MapBook/DepthWithin")->Arg(1)->Arg(5)->Arg(20);

}  // namespace
