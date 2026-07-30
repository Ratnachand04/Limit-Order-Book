// The master plan Part 5 worked trace, end to end through the Simulator.
//
// This is the test that proves the pieces fit together: latency, the queue
// tracker, the ledger, the fee model and the markout sampler all have to agree
// with a scenario that was worked out by hand on paper before any code existed.
//
// Timing.  The trace's clock is the STRATEGY's clock.  With delta_in = 50 ms,
// an event the trace places at t is stamped at exchange time t - 50 ms, so the
// exchange timestamps below are the trace times shifted back by one inbound
// leg (and by +1 s so nothing is negative).
//
//   trace t=0     -> local 1.000 s   place bid 1.0 @ 100.00
//   trace t=50ms  -> local 1.050 s   RESTING, A=5.0 B=0 remaining=1.0
//   trace t=120ms -> local 1.120 s   level -> 6.5, B=1.5
//   trace t=300ms -> local 1.300 s   sell-aggressor 2.0, A=3.0, no fill
//   trace t=420ms -> local 1.420 s   level -> 3.0, cancel 1.5, PESS: B=0
//   trace t=600ms -> local 1.600 s   sell-aggressor 3.5, fill 0.5 @ 100.00
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <lob/sim/simulator.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

constexpr Ts kBase = 1'000'000;      // 1 s, so no timestamp is negative
constexpr Ts kInboundLeg = 50'000;   // delta_in
constexpr Ts kOutboundLeg = 50'000;  // delta_out

// Exchange timestamp for an event the trace observes at local time `trace_us`.
constexpr Ts Exch(Ts trace_us) { return kBase + trace_us - kInboundLeg; }

// A scripted strategy: place one bid at 100.00 the first time the book is
// two-sided, then do nothing.  Deliberately not a QuotingStrategy -- the trace
// is about the engine, not about quoting logic.
class TraceStrategy final : public Strategy {
 public:
  explicit TraceStrategy(Ticks price_ticks, Lots64 qty) : price_(price_ticks), qty_(qty) {}

  void OnBook(const BookView& book, const Clock& clock) override { MaybePlace(book, clock); }
  void OnTrade(const TradeInfo&, const BookView& book, const Clock& clock) override {
    MaybePlace(book, clock);
  }
  void OnTimer(const BookView& book, const Clock& clock) override { MaybePlace(book, clock); }
  void OnFill(const Fill& fill, const Clock& clock) override {
    fills.push_back(fill);
    fill_local_ts.push_back(clock.now_us());
  }
  [[nodiscard]] std::string name() const override { return "trace"; }

  std::vector<Fill> fills;
  std::vector<Ts> fill_local_ts;
  Ts placed_at_us = -1;
  OrderId order = kNoOrder;

 private:
  void MaybePlace(const BookView& book, const Clock& clock) {
    if (order != kNoOrder || !book.HasBothSides()) {
      return;
    }
    order = ctx_.gateway->Place(Side::kBid, price_, qty_);
    placed_at_us = clock.now_us();
  }

  Ticks price_;
  Lots64 qty_;
};

std::vector<Event> BuildTraceEvents(const Instrument& inst) {
  testing::EventBuilder b(inst);
  // Opening book: bids 100.00 x 5.0, 99.99 x 8.0; ask 100.01 x 4.0.
  b.SnapshotBegin(Exch(0), 100);
  b.SnapshotLevel(Exch(0), 100, Side::kBid, 100.00, 5.0);
  b.SnapshotLevel(Exch(0), 100, Side::kBid, 99.99, 8.0);
  b.SnapshotLevel(Exch(0), 100, Side::kAsk, 100.01, 4.0);
  b.SnapshotEnd(Exch(0), 100);

  // Step 3: level 100.00 -> 6.5 (an increase of 1.5, all behind us).
  b.Depth(Exch(120'000), 101, Side::kBid, 100.00, 6.5);
  // Step 4: sell-aggressor 2.0 at 100.00, then the level reported at 4.5.
  b.Trade(Exch(300'000), 102, Side::kAsk, 100.00, 2.0);
  b.Depth(Exch(305'000), 103, Side::kBid, 100.00, 4.5);
  // Step 5: level -> 3.0 with no trade to explain it: a cancellation of 1.5.
  b.Depth(Exch(420'000), 104, Side::kBid, 100.00, 3.0);
  // Step 6: sell-aggressor 3.5 at 100.00 -> fill 0.5.
  b.Trade(Exch(600'000), 105, Side::kAsk, 100.00, 3.5);
  // Step 7: the level is emptied, so the mid settles at 100.00 for the markout.
  b.Depth(Exch(650'000), 106, Side::kBid, 100.00, 0);
  // Quiet tail so the 1 s markout horizon can resolve.
  b.Depth(Exch(3'000'000), 107, Side::kBid, 99.98, 1.0);
  return b.take();
}

RunConfig TraceConfig(const Instrument& inst) {
  RunConfig c = testing::MakeTestConfig(inst, QueueModel::kPess);
  c.latency.in_us = kInboundLeg;
  c.latency.out_us = kOutboundLeg;
  c.latency.jitter_exp_us = 0.0;
  c.fees.maker_tenth_bp = 20;  // 2 bp, as in the trace
  c.strategy.timer_us = 10 * kUsPerMilli;
  c.markouts.horizons_us = {1 * kUsPerSecond};
  return c;
}

class WorkedTraceTest : public ::testing::Test {
 protected:
  void Run(QueueModel model) {
    inst_ = testing::WorkedTraceInstrument();  // tick 0.01, lot 0.1
    config_ = TraceConfig(inst_);
    config_.queue_model = model;
    strategy_ = std::make_unique<TraceStrategy>(inst_.ToTicks(100.00), inst_.ToLots(1.0));
    sim_ = std::make_unique<Simulator>(config_, *strategy_);
    events_ = BuildTraceEvents(inst_);
    sim_->Run(events_);
  }

  Instrument inst_;
  RunConfig config_;
  std::vector<Event> events_;
  std::unique_ptr<TraceStrategy> strategy_;
  std::unique_ptr<Simulator> sim_;
};

TEST_F(WorkedTraceTest, Step1And2_OrderRestsOneOutboundLegAfterTheDecision) {
  Run(QueueModel::kPess);
  ASSERT_NE(strategy_->order, kNoOrder);
  EXPECT_EQ(strategy_->placed_at_us, kBase) << "the book becomes visible one inbound leg late";

  const Order* o = sim_->gateway().Find(strategy_->order);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->decided_ts_us, kBase);
  // Trace step 1: "Gateway stamps arrival t=50 ms."
  EXPECT_EQ(o->effective_ts_us, kBase + kOutboundLeg);
  EXPECT_EQ(o->resting_ts_us, kBase + kOutboundLeg);
  // Trace step 2: "Level shows 5.0 (you're a shadow): A=5.0, B=0, remaining=1.0."
  EXPECT_EQ(o->ahead_at_placement, inst_.ToLots(5.0));
}

TEST_F(WorkedTraceTest, Step6_FillsExactlyHalfAUnitAtTheLimitPrice) {
  Run(QueueModel::kPess);
  ASSERT_EQ(strategy_->fills.size(), 1u);
  const Fill& f = strategy_->fills.front();

  // "fill = clamp(3.5 - 3.0, 0, 1.0) = 0.5.  You buy 0.5 @ 100.00"
  EXPECT_EQ(f.qty_lots, inst_.ToLots(0.5));
  EXPECT_EQ(f.price_ticks, inst_.ToTicks(100.00));
  EXPECT_EQ(f.side, Side::kBid);
  // "Mid just before: 100.005"
  EXPECT_EQ(f.mid_x2_ticks_at_fill, 20001);
  EXPECT_EQ(strategy_->fill_local_ts.front(), kBase + 600'000);
  EXPECT_EQ(f.cause, FillCause::kQueueConsumed);
}

TEST_F(WorkedTraceTest, Step6_LedgerNumbersMatchThePaperArithmetic) {
  Run(QueueModel::kPess);
  ASSERT_EQ(sim_->stats().fills, 1u);

  // "instant edge = +0.005 x 0.5 = $0.0025 (0.5 bp)"
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(sim_->ledger().spread_capture_x2()), 0.0025);
  // "Fee at 2 bp: -$0.0100"  (2 bp of the $50 notional)
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(sim_->ledger().fees_x2()), 0.01);
  EXPECT_EQ(sim_->ledger().inventory_lots(), inst_.ToLots(0.5));
  EXPECT_DOUBLE_EQ(Instrument::CashToDouble(sim_->ledger().notional_traded_1e8()), 50.0);

  // The §3.9 identity is exact, always.
  EXPECT_EQ(sim_->ledger().IdentityResidualX2(), 0) << sim_->ledger().IdentityReport();
}

TEST_F(WorkedTraceTest, Step7_MarkoutShowsTheEdgeGivenBackAtOneSecond) {
  Run(QueueModel::kPess);
  const std::vector<MarkoutSample>& samples = sim_->markouts().samples();
  ASSERT_EQ(samples.size(), 1u);
  const MarkoutSample& s = samples.front();
  ASSERT_TRUE(s.resolved) << "the tail of the session must be long enough to resolve h = 1 s";

  // Edge at the fill: 0.0025 on a $50 notional = 0.5 bp -- the trace's number.
  EXPECT_NEAR(s.edge_bp, 0.5, 1e-9);
  // "markout job samples mid at +1 s (100.00 => AS = -0.005 x 0.5)"
  EXPECT_EQ(s.mid_x2_at_horizon, 20000);
  EXPECT_NEAR(s.adverse_selection_bp, -0.5, 1e-9);
  // Gross markout is zero: every basis point of edge was handed straight back.
  EXPECT_NEAR(s.markout_bp, 0.0, 1e-9);

  // "Net at 1 s: +0.0025 - 0.0025 - 0.0100 = -$0.0100 -- a fill that *looks*
  //  fine and loses money after adverse selection and fees.  Multiply by
  //  thousands of fills: that's the paper."
  const double notional = 50.0;
  const double net_currency = (s.markout_bp / 10000.0) * notional -
                              Ledger::X2ToCurrency(sim_->ledger().fees_x2());
  EXPECT_NEAR(net_currency, -0.01, 1e-12);
}

TEST_F(WorkedTraceTest, QueueAttributionSplitsTradesFromCancels) {
  Run(QueueModel::kPess);
  const QueueTrackerStats& q = sim_->queue_tracker().stats();

  // Two level decreases are explained by trades:
  //   step 4:  6.5 -> 4.5, a fall of 2.0, matched by the 2.0 print
  //   step 7:  3.0 -> 0,   a fall of 3.0, matched by the 3.5 print
  // The 3.5 print exceeds the 3.0 that was left at the level, so only 3.0 of it
  // can be credited against a decrease; the rest is simply the print running
  // past the level.
  EXPECT_EQ(q.lots_explained_by_trades, inst_.ToLots(5.0));

  // Step 5 is the only unexplained decrease: 4.5 -> 3.0 with no print behind it.
  EXPECT_EQ(q.lots_attributed_to_cancels, inst_.ToLots(1.5));
  EXPECT_EQ(q.trades_seen, 2u);
  EXPECT_EQ(q.placements, 1u);
}

TEST_F(WorkedTraceTest, OptimisticFillsTheWholeOrderAndPessimisticDoesNot) {
  Run(QueueModel::kPess);
  const Lots64 pess_qty = strategy_->fills.empty() ? 0 : strategy_->fills.front().qty_lots;

  Run(QueueModel::kOpt);
  const Lots64 opt_qty = strategy_->fills.empty() ? 0 : strategy_->fills.front().qty_lots;

  // Trace step 5: "Optimistic would have given A=1.5", which the 3.5 print then
  // clears entirely.  This gap between the two IS the methodological device.
  EXPECT_EQ(pess_qty, inst_.ToLots(0.5));
  EXPECT_EQ(opt_qty, inst_.ToLots(1.0));
  EXPECT_LT(pess_qty, opt_qty);
}

TEST_F(WorkedTraceTest, TouchRuleFillsTheFullSizeOnTheFirstTouch) {
  // "Naive touch-rule comparison on the same data: it fills the full 1.0 at
  //  step 4's first touch-trade ... precisely the fiction RQ1 measures."
  inst_ = testing::WorkedTraceInstrument();
  config_ = TraceConfig(inst_);
  config_.fill_model = "TOUCH";
  strategy_ = std::make_unique<TraceStrategy>(inst_.ToTicks(100.00), inst_.ToLots(1.0));
  Simulator sim(config_, *strategy_);
  events_ = BuildTraceEvents(inst_);
  sim.Run(events_);

  ASSERT_FALSE(strategy_->fills.empty());
  const Fill& first = strategy_->fills.front();
  // Filled at the FIRST trade (step 4), which the queue-aware model correctly
  // does not fill at all, and for the full order size.
  EXPECT_EQ(strategy_->fill_local_ts.front(), kBase + 300'000);
  EXPECT_EQ(first.qty_lots, inst_.ToLots(1.0));
  EXPECT_EQ(sim.ledger().IdentityResidualX2(), 0);
}

TEST_F(WorkedTraceTest, DirtyEventsAreReplayedButFlagged) {
  // Dirty intervals keep the book live and are excluded from analytics (§4.4).
  inst_ = testing::WorkedTraceInstrument();
  config_ = TraceConfig(inst_);
  strategy_ = std::make_unique<TraceStrategy>(inst_.ToTicks(100.00), inst_.ToLots(1.0));
  Simulator sim(config_, *strategy_);
  std::vector<Event> events = BuildTraceEvents(inst_);
  for (Event& e : events) {
    if (e.Type() == EventType::kTrade) {
      e.flags |= flags::kDirty;
    }
  }
  sim.Run(events);
  EXPECT_EQ(sim.stats().dirty_events, 2u);
  // Still replayed: the fill happens, it is simply not evidence.
  EXPECT_EQ(sim.stats().fills, 1u);
}

}  // namespace
}  // namespace lob
