// The binary schema is a contract with every file the recorder will ever
// produce.  These tests exist so that changing it is loud.
#include <gtest/gtest.h>

#include <cstring>

#include <lob/execution.hpp>
#include <lob/types.hpp>

namespace lob {
namespace {

TEST(EventSchema, IsExactlyThirtyTwoBytes) {
  // master plan §4.3 / CLAUDE.md: "fixed 32-byte little-endian records".
  EXPECT_EQ(sizeof(Event), 32u);
  EXPECT_EQ(kEventSize, 32u);
  EXPECT_EQ(alignof(Event), 8u);
}

TEST(EventSchema, FieldOffsetsAreStable) {
  EXPECT_EQ(offsetof(Event, exch_ts_us), 0u);
  EXPECT_EQ(offsetof(Event, seq), 8u);
  EXPECT_EQ(offsetof(Event, price_ticks), 16u);
  EXPECT_EQ(offsetof(Event, qty_lots), 20u);
  EXPECT_EQ(offsetof(Event, type), 24u);
  EXPECT_EQ(offsetof(Event, side), 25u);
  EXPECT_EQ(offsetof(Event, symbol_id), 26u);
  EXPECT_EQ(offsetof(Event, flags), 27u);
  EXPECT_EQ(offsetof(Event, reserved), 28u);
}

TEST(EventSchema, RoundTripsThroughRawBytes) {
  const Event original = MakeDepth(1'700'000'000'123'456LL, 987654321, Side::kAsk, -12345, 6789,
                                   7, flags::kDirty);
  unsigned char raw[kEventSize];
  std::memcpy(raw, &original, kEventSize);
  Event restored{};
  std::memcpy(&restored, raw, kEventSize);
  EXPECT_EQ(original, restored);
  EXPECT_EQ(restored.Type(), EventType::kDepth);
  EXPECT_EQ(restored.SideOf(), Side::kAsk);
  EXPECT_TRUE(restored.IsDirty());
}

TEST(EventSchema, ReservedFieldIsZeroed) {
  const Event e = MakeTrade(1, 2, Side::kBid, 3, 4);
  EXPECT_EQ(e.reserved, 0);
}

TEST(Side, OppositeAndSign) {
  EXPECT_EQ(Opposite(Side::kBid), Side::kAsk);
  EXPECT_EQ(Opposite(Side::kAsk), Side::kBid);
  // s_i of the §3.9 decomposition: +1 when we buy.
  EXPECT_EQ(SignOf(Side::kBid), 1);
  EXPECT_EQ(SignOf(Side::kAsk), -1);
}

TEST(Trade, AggressorSideDeterminesWhichQueueIsConsumed) {
  // A SELL aggressor (kAsk) hits the bid, so it is the event that can fill a
  // resting BID.  Getting this inversion wrong flips every fill in the project.
  const Event sell_aggressor = MakeTrade(1, 1, Side::kAsk, 10000, 5);
  EXPECT_EQ(sell_aggressor.SideOf(), Side::kAsk);
  EXPECT_EQ(sell_aggressor.ConsumedSide(), Side::kBid);

  const Event buy_aggressor = MakeTrade(1, 1, Side::kBid, 10001, 5);
  EXPECT_EQ(buy_aggressor.ConsumedSide(), Side::kAsk);
}

TEST(Sentinels, EmptyBookIsNeverCrossed) {
  // best_bid < best_ask must hold even with nothing resting, so the invariant
  // check does not have to special-case an empty book.
  EXPECT_LT(kNoBid, kNoAsk);
  EXPECT_FALSE(HasBid(kNoBid));
  EXPECT_FALSE(HasAsk(kNoAsk));
  EXPECT_TRUE(HasBid(0));
  EXPECT_TRUE(HasAsk(0));
}

TEST(OrderState, LivenessMatchesTheStateMachine) {
  // PENDING_CANCEL is deliberately LIVE: the cancel has not landed yet, and the
  // order can still fill in the meantime.  That is the whole reason latency is
  // modelled at all (§4.6).
  EXPECT_TRUE(IsLive(OrderState::kResting));
  EXPECT_TRUE(IsLive(OrderState::kPartiallyFilled));
  EXPECT_TRUE(IsLive(OrderState::kPendingCancel));
  EXPECT_FALSE(IsLive(OrderState::kPendingNew));
  EXPECT_FALSE(IsLive(OrderState::kFilled));
  EXPECT_FALSE(IsLive(OrderState::kCanceled));

  EXPECT_TRUE(IsTerminal(OrderState::kFilled));
  EXPECT_TRUE(IsTerminal(OrderState::kCanceled));
  EXPECT_TRUE(IsTerminal(OrderState::kRejected));
  EXPECT_FALSE(IsTerminal(OrderState::kPendingCancel));
}

TEST(QueueState, QueueFractionIsAOverTotal) {
  QueueState q;
  q.ahead = 30;
  q.remaining = 10;
  q.behind = 60;
  EXPECT_DOUBLE_EQ(q.QueueFraction(), 0.3);

  QueueState empty;
  EXPECT_DOUBLE_EQ(empty.QueueFraction(), 0.0);
}

}  // namespace
}  // namespace lob
