// A cycle-accurate timer for the latency percentile benchmarks.
//
// WHY THIS EXISTS.  The first run of `DenseBook/ApplyDepth/percentiles`
// reported p50 = 0 ns.  That was not a zero-cost operation -- it was the timer:
// std::chrono::steady_clock on Windows is backed by QueryPerformanceCounter at
// roughly 100 ns granularity, so anything faster than 100 ns quantises to 0 or
// 100.  For an operation whose whole point is being under 100 ns, the
// instrument was useless.
//
// The fix is the one a low-latency desk uses: read the CPU's timestamp counter
// directly, and convert to nanoseconds with a calibration measured at start-up.
//
// Correctness notes that matter:
//
//   * ORDERING.  rdtsc is not a serialising instruction; the CPU may execute it
//     out of order with the code being measured.  `lfence` before the read
//     stops earlier work being moved after it, and rdtscp (which has an
//     implicit load-fence on the preceding stream) is used for the closing
//     read.  Without this the timer measures nothing in particular.
//
//   * INVARIANT TSC.  On any CPU since roughly Nehalem/Bulldozer the TSC ticks
//     at a constant rate independent of the core's current frequency, so it is
//     a valid clock but NOT a valid cycle counter -- the conversion below is to
//     wall time, not to core cycles.  That is what we want here.
//
//   * The calibration is a wall-clock measurement and inherits the host clock's
//     accuracy.  It is good to well under a percent over a 50 ms window, which
//     is far tighter than anything these percentiles are used to claim.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace lob::bench {

// Serialised timestamp-counter reads.  Use Begin() before the operation and
// End() after it; the pairing is what makes the fences correct.
inline std::uint64_t TscBegin() {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

inline std::uint64_t TscEnd() {
  unsigned aux = 0;
  const std::uint64_t t = __rdtscp(&aux);  // ordered after everything before it
  _mm_lfence();                            // and nothing after may move before it
  return t;
}

// Nanoseconds per TSC tick, measured once against the steady clock.
//
// The busy-wait is deliberate: sleeping would let the thread migrate to another
// core, and on a machine whose TSC is not synchronised across cores that would
// corrupt the calibration.
inline double TscNanosPerTick() {
  static const double kNanosPerTick = [] {
    using clock = std::chrono::steady_clock;
    // Warm up so the first read is not paying for a cold path.
    (void)TscBegin();
    const auto wall_start = clock::now();
    const std::uint64_t tsc_start = TscBegin();
    const auto target = wall_start + std::chrono::milliseconds(50);
    while (clock::now() < target) {
      // spin
    }
    const std::uint64_t tsc_end = TscEnd();
    const auto wall_end = clock::now();

    const double elapsed_ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count());
    const double ticks = static_cast<double>(tsc_end - tsc_start);
    if (ticks <= 0.0 || elapsed_ns <= 0.0) {
      return 1.0;  // degenerate host; report ticks as if they were nanoseconds
    }
    return elapsed_ns / ticks;
  }();
  return kNanosPerTick;
}

// The floor this instrument can resolve: the cost of the fenced read pair
// itself, measured the same way it will be used.  Reporting a latency below
// this number would be reporting the instrument, not the code.
inline double TscOverheadNanos() {
  static const double kOverhead = [] {
    constexpr int kSamples = 4096;
    std::uint64_t best = ~std::uint64_t{0};
    for (int i = 0; i < kSamples; ++i) {
      const std::uint64_t a = TscBegin();
      const std::uint64_t b = TscEnd();
      const std::uint64_t d = b - a;
      if (d < best) {
        best = d;
      }
    }
    return static_cast<double>(best) * TscNanosPerTick();
  }();
  return kOverhead;
}

}  // namespace lob::bench
