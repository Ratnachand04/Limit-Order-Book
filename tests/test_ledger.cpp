// The PnL decomposition identity (master plan §3.9).
//
//   dE = spread capture + inventory PnL - fees
//
// The master plan asks for this to hold "within 1e-9 of notional".  Because
// every term is an integer here, it is asserted EXACTLY -- and "that assertion
// has caught a bug in every backtester ever written".
#include <gtest/gtest.h>

#include <lob/analytics/ledger.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

FeeConfig ZeroFees() {
  FeeConfig f;
  f.maker_tenth_bp = 0;
  f.taker_tenth_bp = 0;
  return f;
}

FeeConfig MakerBp(double bp) {
  FeeConfig f;
  f.maker_tenth_bp = static_cast<std::int64_t>(bp * 10.0);
  f.taker_tenth_bp = static_cast<std::int64_t>(bp * 10.0);
  return f;
}

TEST(Ledger, StartsFlatAndFlatIsZeroEquity) {
  Ledger ledger(testing::WorkedTraceInstrument(), ZeroFees());
  ledger.MarkTo(20001);
  EXPECT_EQ(ledger.cash_1e8(), 0);
  EXPECT_EQ(ledger.inventory_lots(), 0);
  EXPECT_EQ(ledger.EquityX2(), 0);
  EXPECT_TRUE(ledger.IdentityHolds());
}

TEST(Ledger, SpreadCaptureMatchesTheWorkedTraceNumber) {
  // master plan Part 5 step 6: buy 0.5 at 100.00 with the mid at 100.005.
  //   instantaneous edge = 0.005 * 0.5 = $0.0025
  const Instrument inst = testing::WorkedTraceInstrument();  // tick 0.01, lot 0.1
  Ledger ledger(inst, ZeroFees());

  const std::int64_t mid_x2 = 20001;  // (10000 + 10001) ticks
  ledger.MarkTo(mid_x2);
  ledger.ApplyFill(Side::kBid, /*price_ticks=*/10000, /*qty_lots=*/5, mid_x2);

  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.spread_capture_x2()), 0.0025);
  EXPECT_EQ(ledger.inventory_lots(), 5);
  // Cash paid out: 100.00 * 0.5 = $50.
  EXPECT_DOUBLE_EQ(Instrument::CashToDouble(ledger.cash_1e8()), -50.0);
  EXPECT_TRUE(ledger.IdentityHolds()) << ledger.IdentityReport();
}

TEST(Ledger, FeeMatchesTheWorkedTraceNumber) {
  // Same fill at a 2 bp maker fee: 2 bp of $50 = $0.01.
  const Instrument inst = testing::WorkedTraceInstrument();
  Ledger ledger(inst, MakerBp(2.0));
  const std::int64_t mid_x2 = 20001;
  ledger.MarkTo(mid_x2);
  ledger.ApplyFill(Side::kBid, 10000, 5, mid_x2);

  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.fees_x2()), 0.01);
  // Net at the moment of the fill: +0.0025 edge - 0.0100 fee.
  const double net = Ledger::X2ToCurrency(ledger.spread_capture_x2() - ledger.fees_x2());
  EXPECT_DOUBLE_EQ(net, -0.0075);
  EXPECT_TRUE(ledger.IdentityHolds()) << ledger.IdentityReport();
}

TEST(Ledger, InventoryPnlIntegratesPositionAgainstTheMidPath) {
  const Instrument inst = testing::WorkedTraceInstrument();
  Ledger ledger(inst, ZeroFees());
  ledger.MarkTo(20000);                       // mid 100.00
  ledger.ApplyFill(Side::kBid, 10000, 10, 20000);  // buy 1.0 at 100.00
  ledger.MarkTo(20020);                       // mid moves to 100.10

  // Long 1.0 across a +$0.10 move.
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.inventory_pnl_x2()), 0.10);
  EXPECT_TRUE(ledger.IdentityHolds()) << ledger.IdentityReport();
}

TEST(Ledger, ShortInventoryLosesWhenTheMidRises) {
  const Instrument inst = testing::WorkedTraceInstrument();
  Ledger ledger(inst, ZeroFees());
  ledger.MarkTo(20000);
  ledger.ApplyFill(Side::kAsk, 10000, 10, 20000);  // sell 1.0
  EXPECT_EQ(ledger.inventory_lots(), -10);
  ledger.MarkTo(20020);
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.inventory_pnl_x2()), -0.10);
  EXPECT_TRUE(ledger.IdentityHolds()) << ledger.IdentityReport();
}

TEST(Ledger, IdentityHoldsExactlyOverALongMixedSequence) {
  const Instrument inst = testing::TestInstrument();
  Ledger ledger(inst, MakerBp(2.0));

  std::int64_t mid_x2 = 2 * inst.ToTicks(100.0);
  ledger.MarkTo(mid_x2);

  // A deterministic but irregular walk of marks and fills on both sides.
  std::int64_t state = 12345;
  for (int i = 0; i < 5000; ++i) {
    state = state * 6364136223846793005LL + 1442695040888963407LL;
    const int move = static_cast<int>((state >> 33) % 7) - 3;
    mid_x2 += move;
    ledger.MarkTo(mid_x2);

    if ((state >> 21) % 3 == 0) {
      const bool buy = ((state >> 17) & 1) != 0;
      const Ticks px = static_cast<Ticks>(mid_x2 / 2 + (buy ? -1 : 1));
      const Lots64 qty = 1 + ((state >> 11) % 5);
      ledger.ApplyFill(buy ? Side::kBid : Side::kAsk, px, qty, mid_x2);
    }
  }
  // Exactly zero.  Not "small": every term is an integer, so any residual at
  // all is a defect.
  EXPECT_EQ(ledger.IdentityResidualX2(), 0) << ledger.IdentityReport();
  EXPECT_GT(ledger.fill_count(), 1000u);
}

TEST(Ledger, RebatesAreCreditsNotCosts) {
  const Instrument inst = testing::WorkedTraceInstrument();
  FeeConfig rebate;
  rebate.maker_tenth_bp = -5;  // -0.5 bp
  rebate.taker_tenth_bp = 50;
  Ledger ledger(inst, rebate);
  ledger.MarkTo(20000);
  const Cash before = ledger.cash_1e8();
  ledger.ApplyFill(Side::kBid, 10000, 10, 20000);  // $100 notional
  // -0.5 bp of $100 = -$0.005, i.e. we are PAID.
  EXPECT_LT(ledger.fees_x2(), 0);
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.fees_x2()), -0.005);
  // Cash out is the notional minus the rebate.
  EXPECT_EQ(before - ledger.cash_1e8(), 100'00000000LL - 500000LL);
  EXPECT_TRUE(ledger.IdentityHolds()) << ledger.IdentityReport();
}

TEST(Ledger, RoundTripAtTheSamePriceLosesExactlyTwoFees) {
  const Instrument inst = testing::WorkedTraceInstrument();
  Ledger ledger(inst, MakerBp(2.0));
  ledger.MarkTo(20000);
  ledger.ApplyFill(Side::kBid, 10000, 10, 20000);
  ledger.ApplyFill(Side::kAsk, 10000, 10, 20000);
  EXPECT_EQ(ledger.inventory_lots(), 0);
  // Bought and sold at the mid: zero edge, zero inventory PnL, two fees.
  EXPECT_EQ(ledger.spread_capture_x2(), 0);
  EXPECT_EQ(ledger.inventory_pnl_x2(), 0);
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.fees_x2()), 0.04);  // 2 x 2bp x $100
  EXPECT_TRUE(ledger.IdentityHolds()) << ledger.IdentityReport();
}

TEST(Ledger, TakerFeeAppliesToForcedLiquidations) {
  const Instrument inst = testing::WorkedTraceInstrument();
  FeeConfig fees;
  fees.maker_tenth_bp = 0;
  fees.taker_tenth_bp = 50;  // 5 bp
  Ledger ledger(inst, fees);
  ledger.MarkTo(20000);
  ledger.ApplyFill(Side::kBid, 10000, 10, 20000, /*maker=*/false);
  EXPECT_DOUBLE_EQ(Ledger::X2ToCurrency(ledger.fees_x2()), 0.05);  // 5 bp of $100
  EXPECT_TRUE(ledger.IdentityHolds());
}

}  // namespace
}  // namespace lob
