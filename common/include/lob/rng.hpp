// The simulator's single source of randomness.
//
// CLAUDE.md tech constraint: "deterministic (one seeded std::mt19937_64;
// identical data+seed => byte-identical outputs)".  Everything stochastic --
// latency jitter (§4.5) and the proportional-cancel draws (§3.7) -- draws from
// one instance so the draw *order* is part of the reproducible state.
//
// Consequences worth knowing before you add a caller:
//   * Never construct a second engine.  Pass this by reference.
//   * Never draw conditionally on anything that is not itself deterministic.
//   * std::exponential_distribution and friends are NOT portable across
//     standard libraries, so the transforms below are written out by hand.
//     That is what keeps Windows/MSVC and Linux/libstdc++ runs byte-identical.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace lob {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : seed_(seed), engine_(seed) {}

  [[nodiscard]] std::uint64_t seed() const { return seed_; }
  [[nodiscard]] std::uint64_t draws() const { return draws_; }

  // Restores the engine to its initial state.  Used between sweep runs so each
  // configuration in the experiment matrix sees the same random stream.
  void Reset() {
    engine_.seed(seed_);
    draws_ = 0;
  }

  [[nodiscard]] std::uint64_t NextU64() {
    ++draws_;
    return engine_();
  }

  // Uniform on [0, 1).  Built from the top 53 bits so the result is exactly
  // representable and identical on every conforming implementation.
  [[nodiscard]] double Uniform01() {
    constexpr double kInv53 = 1.0 / 9007199254740992.0;  // 2^-53
    return static_cast<double>(NextU64() >> 11) * kInv53;
  }

  // Uniform on (0, 1] -- never returns 0, so log() below is always finite.
  [[nodiscard]] double UniformPositive() {
    constexpr double kInv53 = 1.0 / 9007199254740992.0;
    return (static_cast<double>(NextU64() >> 11) + 1.0) * kInv53;
  }

  // Exp(rate): mean 1/rate.  Inverse-transform sampling, written out rather
  // than delegated to std::exponential_distribution for portability.
  [[nodiscard]] double Exponential(double rate) {
    if (!(rate > 0.0)) {
      return 0.0;
    }
    return -std::log(UniformPositive()) / rate;
  }

  // Exp with the mean given directly, which is how the latency config reads
  // ("jitter_exp_ms: 5" means a mean of 5 ms).
  [[nodiscard]] double ExponentialMean(double mean) {
    if (!(mean > 0.0)) {
      return 0.0;
    }
    return -std::log(UniformPositive()) * mean;
  }

  // Uniform integer in [lo, hi], inclusive.  Rejection-sampled so the
  // distribution is exact and independent of the standard library.
  [[nodiscard]] std::int64_t UniformInt(std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) {
      return lo;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1U;
    if (span == 0U) {  // full 64-bit range
      return static_cast<std::int64_t>(NextU64());
    }
    const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() -
                                (std::numeric_limits<std::uint64_t>::max() % span) - 1U;
    std::uint64_t v = NextU64();
    while (v > limit) {
      v = NextU64();
    }
    return lo + static_cast<std::int64_t>(v % span);
  }

  [[nodiscard]] bool Bernoulli(double p) { return Uniform01() < p; }

 private:
  std::uint64_t seed_;
  std::mt19937_64 engine_;
  std::uint64_t draws_ = 0;
};

}  // namespace lob
