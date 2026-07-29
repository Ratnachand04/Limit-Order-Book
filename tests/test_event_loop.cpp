// Event ordering, the virtual clock, latency and the order state machine
// (master plan §4.5, §4.6 -- CLAUDE.md Phase 3 gate).
#include <gtest/gtest.h>

#include <vector>

#include <lob/config.hpp>
#include <lob/rng.hpp>
#include <lob/sim/clock.hpp>
#include <lob/sim/event_queue.hpp>
#include <lob/sim/gateway.hpp>
#include <lob/sim/latency.hpp>

namespace lob {
namespace {

// ---------------------------------------------------------------------------
// Event queue ordering
// ---------------------------------------------------------------------------
TEST(EventQueue, PopsInTimestampOrder) {
  EventQueue q;
  q.PushTimer(300);
  q.PushTimer(100);
  q.PushTimer(200);
  EXPECT_EQ(q.Pop().ts_us, 100);
  EXPECT_EQ(q.Pop().ts_us, 200);
  EXPECT_EQ(q.Pop().ts_us, 300);
  EXPECT_TRUE(q.empty());
}

TEST(EventQueue, MarketEventsBeatOurActionsAtEqualTimestamps) {
  // The documented tie-break of §4.5, and a MODELLING CHOICE, not a detail:
  // the market moves first, so a cancel racing an adverse trade loses the race.
  // It is the conservative direction to be wrong in.
  EventQueue q;
  q.PushTimer(1000);
  q.PushAction(1000, ActionKind::kCancel, 7);
  q.PushMarket(1000, 42);

  EXPECT_EQ(q.Pop().kind, SimEventKind::kMarket);
  EXPECT_EQ(q.Pop().kind, SimEventKind::kAction);
  EXPECT_EQ(q.Pop().kind, SimEventKind::kTimer);
}

TEST(EventQueue, TiesWithinAKindKeepInsertionOrder) {
  // Without this the ordering would not be total and two runs could differ.
  EventQueue q;
  q.PushAction(500, ActionKind::kPlace, 1);
  q.PushAction(500, ActionKind::kPlace, 2);
  q.PushAction(500, ActionKind::kPlace, 3);
  EXPECT_EQ(q.Pop().order_id, 1u);
  EXPECT_EQ(q.Pop().order_id, 2u);
  EXPECT_EQ(q.Pop().order_id, 3u);
}

TEST(EventQueue, OrderingIsTotalOverAMixedLoad) {
  EventQueue a;
  EventQueue b;
  // The same events inserted in a different order must come out identically.
  a.PushMarket(10, 1);
  a.PushAction(10, ActionKind::kPlace, 5);
  a.PushTimer(10);
  a.PushMarket(5, 0);

  b.PushMarket(5, 0);
  b.PushMarket(10, 1);
  b.PushAction(10, ActionKind::kPlace, 5);
  b.PushTimer(10);

  while (!a.empty()) {
    ASSERT_FALSE(b.empty());
    const SimEvent ea = a.Pop();
    const SimEvent eb = b.Pop();
    EXPECT_EQ(ea.ts_us, eb.ts_us);
    EXPECT_EQ(ea.kind, eb.kind);
  }
  EXPECT_TRUE(b.empty());
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
TEST(Clock, NeverRunsBackwards) {
  Clock c;
  c.AdvanceTo(1000);
  EXPECT_EQ(c.now_us(), 1000);
  c.AdvanceTo(500);  // ignored
  EXPECT_EQ(c.now_us(), 1000);
  c.AdvanceTo(2000);
  EXPECT_EQ(c.now_us(), 2000);
}

TEST(Clock, KeepsLocalAndExchangeTimeSeparate) {
  // The separation exists so a strategy structurally cannot read exchange time
  // (Part 11 pitfall #1: it would silently make it psychic).
  Clock c;
  c.SetExchangeTs(1'000'000);
  c.AdvanceTo(1'050'000);
  EXPECT_EQ(c.exchange_ts_us(), 1'000'000);
  EXPECT_EQ(c.now_us(), 1'050'000);
  EXPECT_DOUBLE_EQ(c.SecondsSince(1'000'000), 0.05);
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------
TEST(LatencyModel, ConstantLegWithNoJitterIsExact) {
  LatencyConfig cfg;
  cfg.in_us = 50'000;
  cfg.out_us = 50'000;
  cfg.jitter_exp_us = 0.0;
  Rng rng(1);
  LatencyModel latency(cfg, rng);

  EXPECT_EQ(latency.VisibleAt(1'000'000), 1'050'000);
  EXPECT_EQ(latency.EffectiveAt(0), 50'000);
}

TEST(LatencyModel, JitterIsNonNegativeAndMeanTracksTheConfiguredValue) {
  LatencyConfig cfg;
  cfg.in_us = 5'000;
  cfg.out_us = 5'000;
  cfg.jitter_exp_us = 5'000.0;  // mean 5 ms
  Rng rng(99);
  LatencyModel latency(cfg, rng);

  double total = 0.0;
  constexpr int kN = 20000;
  for (int i = 0; i < kN; ++i) {
    const Ts d = latency.DrawIn();
    ASSERT_GE(d, cfg.in_us);  // delta = constant + Exp(jitter), never below the constant
    total += static_cast<double>(d - cfg.in_us);
  }
  EXPECT_NEAR(total / kN, 5000.0, 250.0);
}

TEST(LatencyModel, IsFullyDeterminedByTheSeed) {
  LatencyConfig cfg;
  cfg.in_us = 50'000;
  cfg.jitter_exp_us = 5'000.0;

  auto draw = [&cfg](std::uint64_t seed) {
    Rng rng(seed);
    LatencyModel latency(cfg, rng);
    std::vector<Ts> out;
    for (int i = 0; i < 200; ++i) {
      out.push_back(latency.DrawIn());
    }
    return out;
  };
  EXPECT_EQ(draw(7), draw(7));
  EXPECT_NE(draw(7), draw(8));
}

TEST(Rng, ResetReproducesTheSameStream) {
  // A sweep runs many configurations over the same data; each must see the same
  // random stream, or the cells are not comparable.
  Rng rng(2024);
  std::vector<std::uint64_t> first;
  for (int i = 0; i < 50; ++i) {
    first.push_back(rng.NextU64());
  }
  rng.Reset();
  std::vector<std::uint64_t> second;
  for (int i = 0; i < 50; ++i) {
    second.push_back(rng.NextU64());
  }
  EXPECT_EQ(first, second);
  EXPECT_EQ(rng.draws(), 50u);
}

TEST(Rng, UniformIntCoversItsRangeInclusively) {
  Rng rng(5);
  bool saw_lo = false;
  bool saw_hi = false;
  for (int i = 0; i < 5000; ++i) {
    const std::int64_t v = rng.UniformInt(3, 7);
    ASSERT_GE(v, 3);
    ASSERT_LE(v, 7);
    saw_lo |= (v == 3);
    saw_hi |= (v == 7);
  }
  EXPECT_TRUE(saw_lo);
  EXPECT_TRUE(saw_hi);
  EXPECT_EQ(rng.UniformInt(4, 4), 4);
}

// ---------------------------------------------------------------------------
// Order state machine
// ---------------------------------------------------------------------------
class GatewayTest : public ::testing::Test {
 protected:
  GatewayTest() : rng_(1), latency_(MakeConfig(), rng_), gateway_(queue_, latency_, clock_) {}

  static LatencyConfig MakeConfig() {
    LatencyConfig c;
    c.in_us = 50'000;
    c.out_us = 50'000;
    c.jitter_exp_us = 0.0;
    return c;
  }

  EventQueue queue_;
  Clock clock_;
  Rng rng_;
  LatencyModel latency_;
  OrderGateway gateway_;
};

TEST_F(GatewayTest, PlacementIsPendingUntilDeltaOutElapses) {
  clock_.AdvanceTo(0);
  const OrderId id = gateway_.Place(Side::kBid, 10000, 10);
  ASSERT_NE(id, kNoOrder);

  const Order* o = gateway_.Find(id);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->state, OrderState::kPendingNew);
  EXPECT_EQ(o->decided_ts_us, 0);
  EXPECT_EQ(o->effective_ts_us, 50'000);

  // The action is queued for the moment the exchange sees it.
  ASSERT_FALSE(queue_.empty());
  const SimEvent se = queue_.Pop();
  EXPECT_EQ(se.ts_us, 50'000);
  EXPECT_EQ(se.action, ActionKind::kPlace);

  clock_.AdvanceTo(50'000);
  gateway_.ActivatePlace(id);
  EXPECT_EQ(gateway_.Find(id)->state, OrderState::kResting);
  EXPECT_EQ(gateway_.Find(id)->resting_ts_us, 50'000);
}

TEST_F(GatewayTest, OrderStaysLiveWhileACancelIsInFlight) {
  clock_.AdvanceTo(0);
  const OrderId id = gateway_.Place(Side::kBid, 10000, 10);
  clock_.AdvanceTo(50'000);
  gateway_.ActivatePlace(id);

  clock_.AdvanceTo(900'000);
  ASSERT_TRUE(gateway_.Cancel(id));
  const Order* o = gateway_.Find(id);
  // §4.6: the order is PENDING_CANCEL, which is still LIVE.  This window is
  // exactly where a naive backtest cheats by cancelling instantly.
  EXPECT_EQ(o->state, OrderState::kPendingCancel);
  EXPECT_TRUE(o->live());
  EXPECT_EQ(o->cancel_effective_ts_us, 950'000);

  clock_.AdvanceTo(950'000);
  gateway_.ActivateCancel(id);
  EXPECT_EQ(gateway_.Find(id)->state, OrderState::kCanceled);
  EXPECT_TRUE(gateway_.Find(id)->terminal());
}

TEST_F(GatewayTest, ACancelCannotLandBeforeThePlacementItCancels) {
  clock_.AdvanceTo(0);
  const OrderId id = gateway_.Place(Side::kBid, 10000, 10);  // effective at 50 ms
  // Cancelled almost immediately, while still PENDING_NEW.
  clock_.AdvanceTo(1'000);
  ASSERT_TRUE(gateway_.Cancel(id));
  const Order* o = gateway_.Find(id);
  // Naively it would land at 51 ms, which is after the placement -- fine here,
  // but the clamp guarantees it can never be earlier than the placement.
  EXPECT_GE(o->cancel_effective_ts_us, o->effective_ts_us);
  EXPECT_EQ(o->state, OrderState::kPendingNew);
}

TEST_F(GatewayTest, PartialThenFullFillWalksTheStateMachine) {
  clock_.AdvanceTo(0);
  const OrderId id = gateway_.Place(Side::kBid, 10000, 10);
  clock_.AdvanceTo(50'000);
  gateway_.ActivatePlace(id);

  gateway_.MarkFilled(id, 4, 60'000);
  EXPECT_EQ(gateway_.Find(id)->state, OrderState::kPartiallyFilled);
  EXPECT_EQ(gateway_.Find(id)->remaining_qty, 6);
  EXPECT_EQ(gateway_.Find(id)->filled_qty, 4);

  gateway_.MarkFilled(id, 6, 70'000);
  EXPECT_EQ(gateway_.Find(id)->state, OrderState::kFilled);
  EXPECT_EQ(gateway_.Find(id)->remaining_qty, 0);
  EXPECT_TRUE(gateway_.Find(id)->terminal());
  EXPECT_EQ(gateway_.Find(id)->terminal_ts_us, 70'000);
}

TEST_F(GatewayTest, ReplaceCancelsTheOldOrderAndCreatesANewOne) {
  clock_.AdvanceTo(0);
  const OrderId first = gateway_.Place(Side::kBid, 10000, 10);
  clock_.AdvanceTo(50'000);
  gateway_.ActivatePlace(first);

  clock_.AdvanceTo(100'000);
  const OrderId second = gateway_.Replace(first, Side::kBid, 9999, 10);
  EXPECT_NE(second, first);
  EXPECT_EQ(gateway_.Find(first)->state, OrderState::kPendingCancel);
  EXPECT_EQ(gateway_.Find(second)->state, OrderState::kPendingNew);
  EXPECT_EQ(gateway_.Find(second)->price_ticks, 9999);
  EXPECT_EQ(gateway_.placements(), 2u);
  EXPECT_EQ(gateway_.cancels(), 1u);
}

TEST_F(GatewayTest, CancellingAnUnknownOrTerminalOrderIsARejectedNoOp) {
  EXPECT_FALSE(gateway_.Cancel(12345));
  clock_.AdvanceTo(0);
  const OrderId id = gateway_.Place(Side::kBid, 10000, 10);
  clock_.AdvanceTo(50'000);
  gateway_.ActivatePlace(id);
  gateway_.MarkFilled(id, 10, 60'000);
  EXPECT_FALSE(gateway_.Cancel(id));  // already FILLED
}

TEST_F(GatewayTest, LiveQtyIgnoresProbesAndTerminalOrders) {
  clock_.AdvanceTo(0);
  const OrderId real = gateway_.Place(Side::kBid, 10000, 10);
  gateway_.PlaceProbe(Side::kBid, 9990, 7, 10, 1'000'000);
  EXPECT_EQ(gateway_.LiveQty(Side::kBid), 10);
  EXPECT_EQ(gateway_.LiveOrders(Side::kBid).size(), 1u);
  EXPECT_EQ(gateway_.LiveOrders(Side::kBid, /*include_probes=*/true).size(), 2u);

  clock_.AdvanceTo(50'000);
  gateway_.ActivatePlace(real);
  gateway_.MarkFilled(real, 10, 60'000);
  EXPECT_EQ(gateway_.LiveQty(Side::kBid), 0);
}

TEST_F(GatewayTest, ProbeOrdersScheduleTheirOwnExpiry) {
  clock_.AdvanceTo(0);
  const OrderId id = gateway_.PlaceProbe(Side::kAsk, 10005, 1, 5, 60'000'000);
  const Order* o = gateway_.Find(id);
  ASSERT_NE(o, nullptr);
  EXPECT_TRUE(o->is_probe);
  EXPECT_EQ(o->probe_depth_ticks, 5);

  bool saw_expiry = false;
  while (!queue_.empty()) {
    const SimEvent se = queue_.Pop();
    if (se.action == ActionKind::kProbeExpiry) {
      saw_expiry = true;
      EXPECT_EQ(se.ts_us, 60'000'000);
    }
  }
  EXPECT_TRUE(saw_expiry);
}

}  // namespace
}  // namespace lob
