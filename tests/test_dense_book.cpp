// The dense fast path (§4.4) must be indistinguishable from the reference.
#include <gtest/gtest.h>

#include <lob/book/dense_book.hpp>
#include <lob/book/dual_book.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

class DenseBookTest : public ::testing::Test {
 protected:
  void Open() {
    for (const Event& e : testing::SimpleOpeningBook(inst_)) {
      book_.Apply(e);
    }
  }
  Instrument inst_ = testing::WorkedTraceInstrument();
  DenseBook book_;
};

TEST_F(DenseBookTest, SnapshotAnchorsTheWindowOnTheMid) {
  Open();
  EXPECT_TRUE(book_.anchored());
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(100.00));
  EXPECT_EQ(book_.BestAsk(), inst_.ToTicks(100.01));
  // A snapshot is the best possible moment to centre the window.
  EXPECT_NEAR(static_cast<double>(book_.anchor()), book_.MidTicks(), 1.0);
  EXPECT_EQ(book_.far_level_count(Side::kBid), 0u);
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, BestCursorsSlideOnDeleteAndInsert) {
  Open();
  book_.ApplyDepth(Side::kBid, inst_.ToTicks(100.00), 0);
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(99.99));
  book_.ApplyDepth(Side::kBid, inst_.ToTicks(99.99), 0);
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(99.98));
  book_.ApplyDepth(Side::kBid, inst_.ToTicks(100.00), inst_.ToLots(1.0));
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(100.00));
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, EmptyingASideReportsNoTouch) {
  Open();
  for (const double px : {100.00, 99.99, 99.98}) {
    book_.ApplyDepth(Side::kBid, inst_.ToTicks(px), 0);
  }
  EXPECT_FALSE(HasBid(book_.BestBid()));
  EXPECT_EQ(book_.LevelCount(Side::kBid), 0u);
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, ResizingALevelDoesNotDisturbTheCursors) {
  Open();
  const Ticks best = book_.BestBid();
  book_.ApplyDepth(Side::kBid, best, inst_.ToLots(500.0));
  EXPECT_EQ(book_.BestBid(), best);
  EXPECT_EQ(book_.QtyAt(Side::kBid, best), inst_.ToLots(500.0));
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, LevelsOutsideTheWindowFallBackToTheMap) {
  Open();
  // +/-32768 ticks at a $0.01 tick is only +/-$327, so a level $1000 away is
  // genuinely outside the window and must still be tracked.
  const Ticks far_px = book_.anchor() + DenseBook::kHalfWindow + 500;
  book_.ApplyDepth(Side::kAsk, far_px, inst_.ToLots(3.0));
  EXPECT_EQ(book_.far_level_count(Side::kAsk), 1u);
  EXPECT_EQ(book_.QtyAt(Side::kAsk, far_px), inst_.ToLots(3.0));
  EXPECT_EQ(book_.LevelCount(Side::kAsk), 4u);
  // It is far worse than the touch, so the touch is unmoved.
  EXPECT_EQ(book_.BestAsk(), inst_.ToTicks(100.01));
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, FarLevelBecomesTheTouchWhenTheNearOnesGo) {
  Open();
  const Ticks far_px = book_.anchor() + DenseBook::kHalfWindow + 500;
  book_.ApplyDepth(Side::kAsk, far_px, inst_.ToLots(3.0));
  for (const double px : {100.01, 100.02, 100.03}) {
    book_.ApplyDepth(Side::kAsk, inst_.ToTicks(px), 0);
  }
  EXPECT_EQ(book_.BestAsk(), far_px);
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, ReanchoringPreservesEveryLevel) {
  Open();
  const Ticks far_px = book_.anchor() + DenseBook::kHalfWindow + 500;
  book_.ApplyDepth(Side::kAsk, far_px, inst_.ToLots(3.0));

  const Ticks best_bid = book_.BestBid();
  const Ticks best_ask = book_.BestAsk();
  const Lots64 bid_qty = book_.QtyAt(Side::kBid, best_bid);

  book_.Reanchor(far_px - 10);
  EXPECT_EQ(book_.reanchor_count(), 1u);
  EXPECT_EQ(book_.BestBid(), best_bid);
  EXPECT_EQ(book_.BestAsk(), best_ask);
  EXPECT_EQ(book_.QtyAt(Side::kBid, best_bid), bid_qty);
  EXPECT_EQ(book_.QtyAt(Side::kAsk, far_px), inst_.ToLots(3.0));
  EXPECT_EQ(book_.LevelCount(Side::kBid), 3u);
  EXPECT_EQ(book_.LevelCount(Side::kAsk), 4u);
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, DriftingPriceReanchorsAutomatically) {
  Open();
  const Ticks start_anchor = book_.anchor();
  // Walk the whole book upward past half the window.
  Ticks bid = book_.BestBid();
  Ticks ask = book_.BestAsk();
  for (int i = 0; i < DenseBook::kHalfWindow / 2 + 100; ++i) {
    book_.ApplyDepth(Side::kBid, bid, 0);
    book_.ApplyDepth(Side::kAsk, ask, 0);
    ++bid;
    ++ask;
    book_.ApplyDepth(Side::kBid, bid, inst_.ToLots(1.0));
    book_.ApplyDepth(Side::kAsk, ask, inst_.ToLots(1.0));
  }
  EXPECT_GT(book_.reanchor_count(), 0u);
  EXPECT_NE(book_.anchor(), start_anchor);
  EXPECT_EQ(book_.BestBid(), bid);
  EXPECT_EQ(book_.BestAsk(), ask);
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(DenseBookTest, DepthWithinMatchesTheReference) {
  Open();
  MapBook reference;
  for (const Event& e : testing::SimpleOpeningBook(inst_)) {
    reference.Apply(e);
  }
  for (Ticks n = 0; n <= 5; ++n) {
    EXPECT_EQ(book_.DepthWithin(Side::kBid, n), reference.DepthWithin(Side::kBid, n)) << "n=" << n;
    EXPECT_EQ(book_.DepthWithin(Side::kAsk, n), reference.DepthWithin(Side::kAsk, n)) << "n=" << n;
  }
}

// ---------------------------------------------------------------------------
// Dual-run mode
// ---------------------------------------------------------------------------
TEST(DualBook, AgreesWithTheReferenceOverAScriptedSession) {
  const Instrument inst = testing::WorkedTraceInstrument();
  std::vector<std::string> problems;
  DualBook book([&problems](const std::string& m) { problems.push_back(m); });
  book.set_check_every_update(true);

  for (const Event& e : testing::SimpleOpeningBook(inst)) {
    book.Apply(e);
  }
  book.ApplyDepth(Side::kBid, inst.ToTicks(100.00), inst.ToLots(6.5));
  book.ApplyDepth(Side::kBid, inst.ToTicks(100.00), inst.ToLots(3.0));
  book.ApplyDepth(Side::kBid, inst.ToTicks(100.00), 0);
  book.ApplyDepth(Side::kAsk, inst.ToTicks(100.01), 0);
  // 100.01 is now free, so re-quoting the bid there does not cross.
  book.ApplyDepth(Side::kBid, inst.ToTicks(100.01), inst.ToLots(2.0));

  EXPECT_EQ(book.mismatches(), 0u) << book.first_mismatch();
  EXPECT_TRUE(book.CheckInvariants()) << book.CheckInvariants().message;
  EXPECT_TRUE(problems.empty());
}

TEST(DualBook, ReadsAreAnsweredByTheReference) {
  const Instrument inst = testing::WorkedTraceInstrument();
  DualBook book;
  for (const Event& e : testing::SimpleOpeningBook(inst)) {
    book.Apply(e);
  }
  EXPECT_EQ(book.BestBid(), book.reference().BestBid());
  EXPECT_EQ(book.BestAsk(), book.reference().BestAsk());
  EXPECT_EQ(book.BestBid(), book.fast().BestBid());
}

}  // namespace
}  // namespace lob
