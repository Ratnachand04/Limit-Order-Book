// Property / fuzz testing of the book core (CLAUDE.md Phase 2 gate:
// "fuzz/property test (random events, invariants hold)").
//
// Two properties are asserted over randomly generated event streams:
//   1. the book's own invariants never break -- never crossed, never negative;
//   2. the dense fast path and the std::map reference agree at every step.
//
// The generator is seeded from the project RNG, so a failure is reproducible
// from the seed printed in the failure message.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <lob/book/dual_book.hpp>
#include <lob/rng.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

// Applies random depth updates around a wandering touch, keeping the generated
// stream self-consistent (a stream that crosses its own book would be testing
// the book against input the exchange can never send).
void FuzzOnce(std::uint64_t seed, int steps) {
  Rng rng(seed);
  std::vector<std::string> problems;
  DualBook book([&problems](const std::string& m) { problems.push_back(m); });
  book.set_check_every_update(true);

  Ticks bid = 10000;
  Ticks ask = 10001;
  book.ApplyDepth(Side::kBid, bid, 50);
  book.ApplyDepth(Side::kAsk, ask, 50);

  for (int i = 0; i < steps; ++i) {
    const double u = rng.Uniform01();

    if (u < 0.15) {
      // Move the touch by a tick, deleting the level left on the wrong side.
      const bool up = rng.Uniform01() < 0.5;
      if (up) {
        book.ApplyDepth(Side::kAsk, ask, 0);
        ++ask;
        ++bid;
        book.ApplyDepth(Side::kAsk, ask, 1 + rng.UniformInt(1, 99));
        book.ApplyDepth(Side::kBid, bid, 1 + rng.UniformInt(1, 99));
      } else {
        book.ApplyDepth(Side::kBid, bid, 0);
        --bid;
        --ask;
        book.ApplyDepth(Side::kBid, bid, 1 + rng.UniformInt(1, 99));
        book.ApplyDepth(Side::kAsk, ask, 1 + rng.UniformInt(1, 99));
      }
    } else if (u < 0.25) {
      // A big jump, which forces the dense window to re-anchor.
      const Ticks jump = static_cast<Ticks>(rng.UniformInt(-40000, 40000));
      book.ApplyDepth(Side::kBid, bid, 0);
      book.ApplyDepth(Side::kAsk, ask, 0);
      bid += jump;
      ask = bid + 1;
      book.ApplyDepth(Side::kBid, bid, 1 + rng.UniformInt(1, 99));
      book.ApplyDepth(Side::kAsk, ask, 1 + rng.UniformInt(1, 99));
    } else if (u < 0.35) {
      // Deep, out-of-window levels: exercises the map fallback.
      const bool is_bid = rng.Uniform01() < 0.5;
      const Ticks offset = static_cast<Ticks>(rng.UniformInt(33000, 60000));
      const Ticks px = is_bid ? bid - offset : ask + offset;
      const Lots64 qty = rng.Uniform01() < 0.3 ? 0 : rng.UniformInt(1, 500);
      book.ApplyDepth(is_bid ? Side::kBid : Side::kAsk, px, qty);
    } else if (u < 0.45) {
      // Occasional full resync from a snapshot.
      book.ApplySnapshotBegin();
      const int depth = static_cast<int>(rng.UniformInt(1, 12));
      for (int d = 0; d < depth; ++d) {
        book.ApplySnapshotLevel(Side::kBid, bid - d, rng.UniformInt(1, 200));
        book.ApplySnapshotLevel(Side::kAsk, ask + d, rng.UniformInt(1, 200));
      }
      book.ApplySnapshotEnd();
    } else {
      // Ordinary churn at a level near the touch, including deletions.
      const bool is_bid = rng.Uniform01() < 0.5;
      const Ticks offset = static_cast<Ticks>(rng.UniformInt(0, 40));
      const Ticks px = is_bid ? bid - offset : ask + offset;
      const Lots64 qty = rng.Uniform01() < 0.25 ? 0 : rng.UniformInt(1, 400);
      book.ApplyDepth(is_bid ? Side::kBid : Side::kAsk, px, qty);
    }

    const BookIntegrity ok = book.CheckInvariants();
    ASSERT_TRUE(ok.ok) << "seed=" << seed << " step=" << i << ": " << ok.message;
  }

  EXPECT_EQ(book.mismatches(), 0u) << "seed=" << seed << ": " << book.first_mismatch();
  EXPECT_TRUE(problems.empty()) << problems.front();
}

TEST(BookProperty, InvariantsHoldAndPathsAgreeAcrossManySeeds) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FuzzOnce(seed, 400);
  }
}

TEST(BookProperty, LongSingleSessionStaysConsistent) {
  FuzzOnce(987654321ULL, 5000);
}

TEST(BookProperty, DepthWithinAgreesBetweenPaths) {
  Rng rng(4242);
  DualBook book;
  Ticks bid = 50000;
  Ticks ask = 50003;
  for (int i = 0; i < 2000; ++i) {
    const bool is_bid = rng.Uniform01() < 0.5;
    const Ticks offset = static_cast<Ticks>(rng.UniformInt(0, 60));
    const Ticks px = is_bid ? bid - offset : ask + offset;
    book.ApplyDepth(is_bid ? Side::kBid : Side::kAsk, px,
                    rng.Uniform01() < 0.2 ? 0 : rng.UniformInt(1, 300));
  }
  // Compare through the concrete types: DualBook answers reads from the
  // reference, so asking it twice would prove nothing.
  for (Ticks n : {Ticks{0}, Ticks{1}, Ticks{5}, Ticks{20}, Ticks{100}}) {
    EXPECT_EQ(book.reference().DepthWithin(Side::kBid, n),
              book.fast().DepthWithin(Side::kBid, n))
        << "n=" << n;
    EXPECT_EQ(book.reference().DepthWithin(Side::kAsk, n),
              book.fast().DepthWithin(Side::kAsk, n))
        << "n=" << n;
  }
}

}  // namespace
}  // namespace lob
