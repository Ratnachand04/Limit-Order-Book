// Hand-built book scenarios against the reference implementation (§4.4).
#include <gtest/gtest.h>

#include <lob/book/map_book.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

class MapBookTest : public ::testing::Test {
 protected:
  void Open() {
    for (const Event& e : testing::SimpleOpeningBook(inst_)) {
      book_.Apply(e);
    }
  }
  Instrument inst_ = testing::WorkedTraceInstrument();  // tick 0.01, lot 0.1
  MapBook book_;
};

TEST_F(MapBookTest, EmptyBookHasNoTouchAndIsNotCrossed) {
  EXPECT_FALSE(HasBid(book_.BestBid()));
  EXPECT_FALSE(HasAsk(book_.BestAsk()));
  EXPECT_FALSE(book_.HasBothSides());
  EXPECT_TRUE(book_.CheckInvariants());
  EXPECT_LT(book_.BestBid(), book_.BestAsk());
}

TEST_F(MapBookTest, SnapshotBuildsBothSides) {
  Open();
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(100.00));
  EXPECT_EQ(book_.BestAsk(), inst_.ToTicks(100.01));
  EXPECT_EQ(book_.QtyAt(Side::kBid, inst_.ToTicks(100.00)), inst_.ToLots(5.0));
  EXPECT_EQ(book_.QtyAt(Side::kAsk, inst_.ToTicks(100.03)), inst_.ToLots(15.0));
  EXPECT_EQ(book_.LevelCount(Side::kBid), 3u);
  EXPECT_EQ(book_.LevelCount(Side::kAsk), 3u);
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(MapBookTest, MidAndSpreadAreDerivedFromTheTouch) {
  Open();
  // Mid is carried as a doubled integer so a half-tick mid stays exact.
  EXPECT_EQ(book_.MidX2Ticks(), 20001);
  EXPECT_DOUBLE_EQ(book_.MidTicks(), 10000.5);
  EXPECT_EQ(book_.SpreadTicks(), 1);
}

TEST_F(MapBookTest, DepthQuantityIsAbsoluteAndZeroDeletes) {
  Open();
  const Ticks px = inst_.ToTicks(100.00);
  // The diff-depth stream sends the NEW total at the level, not a delta.
  book_.ApplyDepth(Side::kBid, px, inst_.ToLots(6.5));
  EXPECT_EQ(book_.QtyAt(Side::kBid, px), inst_.ToLots(6.5));

  book_.ApplyDepth(Side::kBid, px, 0);
  EXPECT_EQ(book_.QtyAt(Side::kBid, px), 0);
  EXPECT_EQ(book_.LevelCount(Side::kBid), 2u);
  // The touch slides down to the next surviving level.
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(99.99));
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(MapBookTest, NewBestOnEitherSideMovesTheTouch) {
  Open();
  // The opening book is one tick wide, so a new best has to make room first.
  book_.ApplyDepth(Side::kAsk, inst_.ToTicks(100.01), 0);
  EXPECT_EQ(book_.BestAsk(), inst_.ToTicks(100.02));
  book_.ApplyDepth(Side::kAsk, inst_.ToTicks(100.01), inst_.ToLots(2.0));
  EXPECT_EQ(book_.BestAsk(), inst_.ToTicks(100.01));

  book_.ApplyDepth(Side::kBid, inst_.ToTicks(100.00), 0);
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(99.99));
  book_.ApplyDepth(Side::kBid, inst_.ToTicks(100.00), inst_.ToLots(2.0));
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(100.00));
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(MapBookTest, DepthWithinSumsInwardFromTheBest) {
  Open();
  // n_ticks == 0 means the best level alone.
  EXPECT_EQ(book_.DepthWithin(Side::kBid, 0), inst_.ToLots(5.0));
  EXPECT_EQ(book_.DepthWithin(Side::kBid, 1), inst_.ToLots(13.0));   // 5.0 + 8.0
  EXPECT_EQ(book_.DepthWithin(Side::kBid, 2), inst_.ToLots(25.0));   // + 12.0
  EXPECT_EQ(book_.DepthWithin(Side::kAsk, 2), inst_.ToLots(28.0));   // 4 + 9 + 15
  EXPECT_EQ(book_.DepthWithin(Side::kBid, 50), inst_.ToLots(25.0));  // no further levels
}

TEST_F(MapBookTest, SnapshotRebuildsRatherThanPatches) {
  Open();
  book_.ApplyDepth(Side::kBid, inst_.ToTicks(99.50), inst_.ToLots(99.0));
  ASSERT_EQ(book_.LevelCount(Side::kBid), 4u);

  // Part 11 pitfall #4: a resync must REPLACE the side, never merge into it.
  book_.ApplySnapshotBegin();
  book_.ApplySnapshotLevel(Side::kBid, inst_.ToTicks(101.00), inst_.ToLots(2.0));
  book_.ApplySnapshotLevel(Side::kAsk, inst_.ToTicks(101.01), inst_.ToLots(3.0));
  book_.ApplySnapshotEnd();

  EXPECT_EQ(book_.LevelCount(Side::kBid), 1u);
  EXPECT_EQ(book_.LevelCount(Side::kAsk), 1u);
  EXPECT_EQ(book_.QtyAt(Side::kBid, inst_.ToTicks(99.50)), 0);
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(101.00));
  EXPECT_TRUE(book_.CheckInvariants());
}

TEST_F(MapBookTest, PartialSnapshotIsNeverObservable) {
  Open();
  const Ticks old_best = book_.BestBid();
  book_.ApplySnapshotBegin();
  book_.ApplySnapshotLevel(Side::kBid, inst_.ToTicks(200.00), inst_.ToLots(1.0));
  // Mid-snapshot the old book is still the visible one.
  EXPECT_EQ(book_.BestBid(), old_best);
  book_.ApplySnapshotEnd();
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(200.00));
}

TEST_F(MapBookTest, ZeroQuantitySnapshotLevelsAreIgnored) {
  book_.ApplySnapshotBegin();
  book_.ApplySnapshotLevel(Side::kBid, inst_.ToTicks(100.00), 0);
  book_.ApplySnapshotLevel(Side::kBid, inst_.ToTicks(99.99), inst_.ToLots(1.0));
  book_.ApplySnapshotEnd();
  EXPECT_EQ(book_.LevelCount(Side::kBid), 1u);
  EXPECT_EQ(book_.BestBid(), inst_.ToTicks(99.99));
}

TEST_F(MapBookTest, TradesDoNotMoveTheBookOnTheirOwn) {
  Open();
  const Lots64 before = book_.QtyAt(Side::kBid, inst_.ToTicks(100.00));
  // The exchange publishes the resulting level change separately; applying both
  // would double-count the consumed quantity.
  book_.Apply(MakeTrade(1, 1, Side::kAsk, inst_.ToTicks(100.00), inst_.ToLots(2.0)));
  EXPECT_EQ(book_.QtyAt(Side::kBid, inst_.ToTicks(100.00)), before);
}

TEST_F(MapBookTest, TopImbalanceAndWeightedMid) {
  book_.ApplySnapshotBegin();
  book_.ApplySnapshotLevel(Side::kBid, 100, 30);
  book_.ApplySnapshotLevel(Side::kAsk, 102, 10);
  book_.ApplySnapshotEnd();

  // I = Qb / (Qb + Qa) = 30 / 40 = 0.75
  EXPECT_DOUBLE_EQ(book_.TopImbalance(), 0.75);
  // m_w = I * P_ask + (1 - I) * P_bid = 0.75*102 + 0.25*100 = 101.5
  // A heavy bid queue pushes fair value toward the thin (ask) side.
  EXPECT_DOUBLE_EQ(book_.WeightedMidTicks(), 101.5);
  EXPECT_GT(book_.WeightedMidTicks(), book_.MidTicks());
}

TEST_F(MapBookTest, ImbalanceIsNeutralOnAnEmptyBook) {
  EXPECT_DOUBLE_EQ(book_.TopImbalance(), 0.5);
}

TEST_F(MapBookTest, CrossedBookIsReportedNotThrown) {
  book_.ApplyDepth(Side::kBid, 105, 10);
  book_.ApplyDepth(Side::kAsk, 100, 10);
  const BookIntegrity result = book_.CheckInvariants();
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("crossed"), std::string::npos);
}

TEST_F(MapBookTest, ClearResetsEverything) {
  Open();
  book_.Clear();
  EXPECT_EQ(book_.LevelCount(Side::kBid), 0u);
  EXPECT_EQ(book_.LevelCount(Side::kAsk), 0u);
  EXPECT_FALSE(book_.HasBothSides());
}

}  // namespace
}  // namespace lob
