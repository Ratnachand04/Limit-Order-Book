// The §3.7 queue model -- the heart of the simulator, and the module whose
// bugs would be least visible in the results.
//
// Every expected value below is worked out by hand from the master plan's
// rules, not read off an implementation.
#include <gtest/gtest.h>

#include <vector>

#include <lob/rng.hpp>
#include <lob/sim/queue_tracker.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

// Lot size 0.1 throughout, so a quantity of 1.5 is 15 lots.
constexpr Lots64 kL = 10;  // lots per 1.0 unit

class QueueTrackerTest : public ::testing::Test {
 protected:
  void Build(QueueModel model) {
    rng_ = std::make_unique<Rng>(1234);
    tracker_ = std::make_unique<QueueTracker>(model, *rng_);
    fills_.clear();
    tracker_->set_fill_sink([this](const QueueFill& f) { fills_.push_back(f); });
  }

  [[nodiscard]] const QueueState& State(OrderId id) const {
    const QueueState* s = tracker_->State(id);
    EXPECT_NE(s, nullptr);
    static const QueueState kEmpty;
    return s != nullptr ? *s : kEmpty;
  }

  std::unique_ptr<Rng> rng_;
  std::unique_ptr<QueueTracker> tracker_;
  std::vector<QueueFill> fills_;
};

// ---------------------------------------------------------------------------
// The unambiguous rules
// ---------------------------------------------------------------------------
TEST_F(QueueTrackerTest, PlacementJoinsTheBackOfTheQueue) {
  Build(QueueModel::kPess);
  // §3.7: "On placement, price-time priority means you join the back:
  //        A <- Q, B <- 0."
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 0);
  EXPECT_EQ(State(1).ahead, 5 * kL);
  EXPECT_EQ(State(1).behind, 0);
  EXPECT_EQ(State(1).remaining, 1 * kL);
}

TEST_F(QueueTrackerTest, ArrivalsGoBehindUs) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 0);
  // Level 5.0 -> 6.5: an increase of 1.5, all of it behind us.
  tracker_->OnLevelUpdate(Side::kBid, 10000, 65, 1000);
  EXPECT_EQ(State(1).ahead, 5 * kL);
  EXPECT_EQ(State(1).behind, 15);
}

TEST_F(QueueTrackerTest, TradesConsumeTheFrontWithoutFillingUsWhileAheadRemains) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 0);
  // A sell-aggressor (kAsk) is what consumes a BID queue.
  tracker_->OnTrade(Side::kAsk, 10000, 2 * kL, 1000);
  EXPECT_EQ(State(1).ahead, 3 * kL);
  EXPECT_EQ(State(1).remaining, 1 * kL);
  EXPECT_TRUE(fills_.empty());
}

TEST_F(QueueTrackerTest, ABuyAggressorDoesNotTouchOurBid) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 0);
  // A buy-aggressor lifts offers; it cannot consume the bid queue.
  tracker_->OnTrade(Side::kBid, 10000, 50 * kL, 1000);
  EXPECT_EQ(State(1).ahead, 5 * kL);
  EXPECT_TRUE(fills_.empty());
}

TEST_F(QueueTrackerTest, FillIsClampOfTradeMinusAhead) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 3 * kL, 0);
  // fill = clamp(v - A, 0, remaining) = clamp(3.5 - 3.0, 0, 1.0) = 0.5
  tracker_->OnTrade(Side::kAsk, 10000, 35, 1000);
  ASSERT_EQ(fills_.size(), 1u);
  EXPECT_EQ(fills_[0].qty_lots, 5);
  EXPECT_EQ(State(1).ahead, 0);
  EXPECT_EQ(State(1).remaining, 5);
  EXPECT_EQ(fills_[0].cause, FillCause::kQueueConsumed);
}

TEST_F(QueueTrackerTest, ATradeThroughUsEatsIntoTheQueueBehind) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 3 * kL, 0);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 3 * kL + 2 * kL, 500);  // 2.0 arrives behind
  ASSERT_EQ(State(1).behind, 2 * kL);

  // 5.0 trades: 3.0 eats A, 1.0 fills us, 1.0 carries on into B.
  tracker_->OnTrade(Side::kAsk, 10000, 5 * kL, 1000);
  ASSERT_EQ(fills_.size(), 1u);
  EXPECT_EQ(fills_[0].qty_lots, 1 * kL);
  EXPECT_EQ(fills_[0].cause, FillCause::kTradedThrough);
  EXPECT_EQ(State(1).ahead, 0);
  EXPECT_EQ(State(1).remaining, 0);
  EXPECT_EQ(State(1).behind, 1 * kL);
}

// ---------------------------------------------------------------------------
// Cancel attribution -- the three assumptions
// ---------------------------------------------------------------------------
TEST_F(QueueTrackerTest, PessimisticCancelsComeFromBehindFirst) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 3 * kL, 0);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 45, 500);  // +1.5 behind -> B = 1.5
  ASSERT_EQ(State(1).behind, 15);

  // A drop of 1.5 with no trade to explain it is a cancellation.
  tracker_->OnLevelUpdate(Side::kBid, 10000, 30, 1000);
  EXPECT_EQ(State(1).behind, 0);
  EXPECT_EQ(State(1).ahead, 3 * kL);  // unchanged: we never gain position
}

TEST_F(QueueTrackerTest, PessimisticOverflowEatsAhead) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 3 * kL, 0);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 40, 500);  // B = 1.0
  // Cancel 2.5: 1.0 from B, then 1.5 overflows into A.
  tracker_->OnLevelUpdate(Side::kBid, 10000, 15, 1000);
  EXPECT_EQ(State(1).behind, 0);
  EXPECT_EQ(State(1).ahead, 15);
}

TEST_F(QueueTrackerTest, OptimisticCancelsComeFromAheadFirst) {
  Build(QueueModel::kOpt);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 3 * kL, 0);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 45, 500);  // B = 1.5
  tracker_->OnLevelUpdate(Side::kBid, 10000, 30, 1000);  // cancel 1.5
  EXPECT_EQ(State(1).ahead, 15);      // 3.0 - 1.5
  EXPECT_EQ(State(1).behind, 15);     // untouched
}

TEST_F(QueueTrackerTest, ProportionalSplitsExactlyWhenItDivides) {
  Build(QueueModel::kProp);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 3 * kL, 0);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 45, 500);  // A = 30, B = 15, total 45
  tracker_->OnLevelUpdate(Side::kBid, 10000, 30, 1000);  // cancel 15
  // 15 * 30/45 = 10 exactly.  Computed in integers, so this is not a coin flip.
  EXPECT_EQ(State(1).ahead, 20);
  EXPECT_EQ(State(1).behind, 10);
}

TEST_F(QueueTrackerTest, ProportionalIsUnbiasedWhenItDoesNotDivide) {
  // A = 10, B = 10, cancel 3 -> exactly 1.5 should come from ahead.  Over many
  // independent draws the mean must sit on 1.5; each individual draw is 1 or 2.
  double total_from_ahead = 0.0;
  constexpr int kTrials = 4000;
  for (int i = 0; i < kTrials; ++i) {
    Rng rng(static_cast<std::uint64_t>(i) + 1);
    QueueTracker t(QueueModel::kProp, rng);
    t.Place(1, Side::kBid, 10000, 5, 10, 0);
    t.OnLevelUpdate(Side::kBid, 10000, 20, 100);  // B = 10
    t.OnLevelUpdate(Side::kBid, 10000, 17, 200);  // cancel 3
    const QueueState* s = t.State(1);
    ASSERT_NE(s, nullptr);
    const Lots64 from_ahead = 10 - s->ahead;
    EXPECT_GE(from_ahead, 1);
    EXPECT_LE(from_ahead, 2);
    EXPECT_EQ(from_ahead + (10 - s->behind), 3);
    total_from_ahead += static_cast<double>(from_ahead);
  }
  const double mean = total_from_ahead / kTrials;
  EXPECT_NEAR(mean, 1.5, 0.05);
}

TEST_F(QueueTrackerTest, TradesAreNotMistakenForCancels) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 0);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 65, 100);  // B = 1.5

  // 2.0 trades, then the feed reports the level at 4.5.  The decrease of 2.0 is
  // fully explained; nothing may be attributed to a cancellation.
  tracker_->OnTrade(Side::kAsk, 10000, 2 * kL, 200);
  tracker_->OnLevelUpdate(Side::kBid, 10000, 45, 210);
  EXPECT_EQ(State(1).ahead, 3 * kL);
  EXPECT_EQ(State(1).behind, 15);  // untouched
  EXPECT_EQ(tracker_->stats().lots_attributed_to_cancels, 0);
  EXPECT_EQ(tracker_->stats().lots_explained_by_trades, 2 * kL);
}

TEST_F(QueueTrackerTest, StaleTradeCreditExpiresAndIsCounted) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 0);
  tracker_->OnTrade(Side::kAsk, 10000, 2 * kL, 0);
  // No matching level decrease arrives inside the attribution window.  Part 11
  // pitfall #5 asks for this residual to be measured, not silently dropped.
  tracker_->OnLevelUpdate(Side::kBid, 10000, 4 * kL, 10 * kUsPerSecond);
  EXPECT_EQ(tracker_->stats().lots_trade_credit_expired, 2 * kL);
  // With the credit expired, the 1.0 decrease is a cancellation.
  EXPECT_EQ(tracker_->stats().lots_attributed_to_cancels, 1 * kL);
}

// ---------------------------------------------------------------------------
// Other fill triggers
// ---------------------------------------------------------------------------
TEST_F(QueueTrackerTest, CrossFillsTheWholeRemainderAtOurLimit) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 50 * kL, 0);
  // The opposite best has reached our price: the exchange would have matched us.
  tracker_->OnBook(/*best_bid=*/10000, /*best_ask=*/10000, 1000);
  ASSERT_EQ(fills_.size(), 1u);
  EXPECT_EQ(fills_[0].qty_lots, 1 * kL);
  EXPECT_EQ(fills_[0].price_ticks, 10000);
  EXPECT_EQ(fills_[0].cause, FillCause::kCrossed);
}

TEST_F(QueueTrackerTest, NoCrossFillWhenTheSpreadIsIntact) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 50 * kL, 0);
  tracker_->OnBook(10000, 10001, 1000);
  EXPECT_TRUE(fills_.empty());
}

TEST_F(QueueTrackerTest, RemovedOrdersStopTracking) {
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 0, 0);
  tracker_->Remove(1);
  EXPECT_EQ(tracker_->State(1), nullptr);
  tracker_->OnTrade(Side::kAsk, 10000, 50 * kL, 1000);
  EXPECT_TRUE(fills_.empty());
}

TEST_F(QueueTrackerTest, TwoShadowOrdersAtOneLevelEachSeeTheFullTrade) {
  // The shadow-order limitation, made explicit (§2.5, Part 11 #11): our orders
  // do not deplete real liquidity, and they do not queue behind each other.
  Build(QueueModel::kPess);
  tracker_->Place(1, Side::kBid, 10000, 1 * kL, 1 * kL, 0);
  tracker_->Place(2, Side::kBid, 10000, 1 * kL, 1 * kL, 0);
  tracker_->OnTrade(Side::kAsk, 10000, 3 * kL, 1000);
  ASSERT_EQ(fills_.size(), 2u);
  EXPECT_EQ(fills_[0].qty_lots, 1 * kL);
  EXPECT_EQ(fills_[1].qty_lots, 1 * kL);
}

// ---------------------------------------------------------------------------
// The Part 5 worked trace, at the tracker level, under all three assumptions
// ---------------------------------------------------------------------------
struct TraceOutcome {
  Lots64 ahead_after_cancel = 0;
  Lots64 behind_after_cancel = 0;
  Lots64 fill_qty = 0;
  Lots64 remaining_after = 0;
};

TraceOutcome RunPart5Trace(QueueModel model) {
  Rng rng(7);
  QueueTracker tracker(model, rng);
  std::vector<QueueFill> fills;
  tracker.set_fill_sink([&fills](const QueueFill& f) { fills.push_back(f); });

  // Book: bid 100.00 x 5.0.  We place 1.0 at 100.00; it rests at t = 50 ms.
  tracker.Place(1, Side::kBid, 10000, 1 * kL, 5 * kL, 50'000);
  // t = 120 ms: level 100.00 -> 6.5.  Increase of 1.5 goes behind.
  tracker.OnLevelUpdate(Side::kBid, 10000, 65, 120'000);
  // t = 300 ms: sell-aggressor 2.0 at 100.00.  A: 5.0 -> 3.0, no fill.
  tracker.OnTrade(Side::kAsk, 10000, 2 * kL, 300'000);
  // ...and the feed reports the level at 4.5, fully explained by that trade.
  tracker.OnLevelUpdate(Side::kBid, 10000, 45, 305'000);
  // t = 420 ms: level -> 3.0.  A drop of 1.5 with no trade behind it: a cancel.
  tracker.OnLevelUpdate(Side::kBid, 10000, 30, 420'000);

  TraceOutcome out;
  const QueueState* mid = tracker.State(1);
  out.ahead_after_cancel = mid != nullptr ? mid->ahead : -1;
  out.behind_after_cancel = mid != nullptr ? mid->behind : -1;

  // t = 600 ms: sell-aggressor 3.5 at 100.00.
  tracker.OnTrade(Side::kAsk, 10000, 35, 600'000);
  out.fill_qty = fills.empty() ? 0 : fills.front().qty_lots;
  const QueueState* end = tracker.State(1);
  out.remaining_after = end != nullptr ? end->remaining : -1;
  return out;
}

TEST(Part5WorkedTrace, Pessimistic) {
  const TraceOutcome o = RunPart5Trace(QueueModel::kPess);
  // Step 5: "Pessimistic: behind first -> B = 0; A stays 3.0."
  EXPECT_EQ(o.ahead_after_cancel, 30);
  EXPECT_EQ(o.behind_after_cancel, 0);
  // Step 6: "fill = clamp(3.5 - 3.0, 0, 1.0) = 0.5 ... remaining = 0.5".
  EXPECT_EQ(o.fill_qty, 5);
  EXPECT_EQ(o.remaining_after, 5);
}

TEST(Part5WorkedTrace, Optimistic) {
  const TraceOutcome o = RunPart5Trace(QueueModel::kOpt);
  // Step 5: "Optimistic would have given A = 1.5."
  EXPECT_EQ(o.ahead_after_cancel, 15);
  EXPECT_EQ(o.behind_after_cancel, 15);
  // With only 1.5 ahead, the 3.5 print fills the whole 1.0.
  EXPECT_EQ(o.fill_qty, 10);
  EXPECT_EQ(o.remaining_after, 0);
}

TEST(Part5WorkedTrace, Proportional) {
  const TraceOutcome o = RunPart5Trace(QueueModel::kProp);
  // A = 3.0, B = 1.5, cancel 1.5.  15 * 30/45 = 10 lots from ahead exactly.
  EXPECT_EQ(o.ahead_after_cancel, 20);
  EXPECT_EQ(o.behind_after_cancel, 10);
  // fill = clamp(3.5 - 2.0, 0, 1.0) = 1.0
  EXPECT_EQ(o.fill_qty, 10);
  EXPECT_EQ(o.remaining_after, 0);
}

TEST(Part5WorkedTrace, PessimisticIsALowerBoundAndOptimisticAnUpperBound) {
  // The whole point of running three assumptions: they bracket reality.
  const TraceOutcome pess = RunPart5Trace(QueueModel::kPess);
  const TraceOutcome prop = RunPart5Trace(QueueModel::kProp);
  const TraceOutcome opt = RunPart5Trace(QueueModel::kOpt);
  EXPECT_LE(pess.fill_qty, prop.fill_qty);
  EXPECT_LE(prop.fill_qty, opt.fill_qty);
  EXPECT_GE(pess.ahead_after_cancel, prop.ahead_after_cancel);
  EXPECT_GE(prop.ahead_after_cancel, opt.ahead_after_cancel);
}

}  // namespace
}  // namespace lob
