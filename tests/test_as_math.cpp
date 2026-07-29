// Avellaneda-Stoikov (§3.4) and GLFT (§3.5).
//
// These tests check the two things that matter: that the formulas reproduce the
// master plan's worked numeric example, and that their behaviour in q has the
// right SIGN and MONOTONICITY.  A spread formula that is off by a constant is a
// calibration problem; one whose inventory skew has the wrong sign is a bot
// that accumulates risk instead of shedding it.
#include <gtest/gtest.h>

#include <cmath>

#include <lob/strategy/as_math.hpp>

namespace lob {
namespace {

// The master plan §3.4 worked example: BTC ~ $100,000, sigma = 8 $/sqrt(s),
// T - t = 60 s, gamma = 1e-5 $^-1, k = 0.035 $^-1.
AsInputs WorkedExample() {
  AsInputs in;
  in.fair_value = 100000.0;
  in.inventory = 0.0;
  in.sigma = 8.0;
  in.gamma = 1.0e-5;
  in.k = 0.035;
  in.a = 1.0;
  in.time_remaining_s = 60.0;
  return in;
}

TEST(AvellanedaStoikov, InventoryRiskPremiumMatchesTheWorkedExample) {
  // gamma * sigma^2 * (T - t) = 1e-5 * 64 * 60 ~ $0.038 per unit of inventory.
  const double premium = InventoryRiskPremium(1.0e-5, 8.0, 60.0);
  EXPECT_NEAR(premium, 0.0384, 1e-6);
}

TEST(AvellanedaStoikov, MonopolistMarkupApproachesTwoOverKForSmallGamma) {
  // "for small gamma, optimal spread ~ 2/k -- the market's elasticity, not your
  // risk aversion, sets the spread."  2 / 0.035 ~ $57.14.
  const double markup = MonopolistMarkup(1.0e-5, 0.035);
  EXPECT_NEAR(markup, 2.0 / 0.035, 0.02);
  EXPECT_NEAR(markup, 57.14, 0.05);
}

TEST(AvellanedaStoikov, MonopolistMarkupGrowsWithGammaAndShrinksWithK) {
  const double base = MonopolistMarkup(1.0e-5, 0.035);
  EXPECT_GT(MonopolistMarkup(1.0e-4, 0.035), base);  // more risk-averse -> wider
  EXPECT_LT(MonopolistMarkup(1.0e-5, 0.070), base);  // more elastic  -> tighter
}

TEST(AvellanedaStoikov, ReservationPriceEqualsTheMidWhenFlat) {
  const AsQuotes q = AvellanedaStoikov(WorkedExample());
  ASSERT_TRUE(q.valid);
  EXPECT_DOUBLE_EQ(q.reservation_price, 100000.0);
  EXPECT_DOUBLE_EQ(q.inventory_skew, 0.0);
  // Quotes are symmetric about the mid when there is nothing to skew for.
  EXPECT_NEAR(100000.0 - q.bid_price, q.ask_price - 100000.0, 1e-9);
}

TEST(AvellanedaStoikov, TotalSpreadMatchesTheWorkedExample) {
  const AsQuotes q = AvellanedaStoikov(WorkedExample());
  // gamma sigma^2 (T-t) + (2/gamma) ln(1 + gamma/k) ~ 0.0384 + 57.14
  EXPECT_NEAR(q.total_spread, 0.0384 + 2.0 / 0.035, 0.05);
  EXPECT_NEAR(q.half_spread, q.total_spread / 2.0, 1e-12);
  // "A-S can tell you to quote far wider than the touch on a tight instrument."
  EXPECT_GT(q.total_spread, 50.0);
}

TEST(AvellanedaStoikov, LongInventoryPushesTheReservationPriceBelowTheMid) {
  AsInputs in = WorkedExample();
  in.inventory = 1.0;
  const AsQuotes q = AvellanedaStoikov(in);
  ASSERT_TRUE(q.valid);
  // r = s - q * gamma * sigma^2 * (T - t): long inventory shades fair value DOWN
  // so we attract buyers of it and repel more buying.
  EXPECT_LT(q.reservation_price, in.fair_value);
  EXPECT_NEAR(in.fair_value - q.reservation_price, 0.0384, 1e-6);
}

TEST(AvellanedaStoikov, ShortInventoryPushesTheReservationPriceAboveTheMid) {
  AsInputs in = WorkedExample();
  in.inventory = -1.0;
  const AsQuotes q = AvellanedaStoikov(in);
  EXPECT_GT(q.reservation_price, in.fair_value);
}

TEST(AvellanedaStoikov, ReservationPriceIsLinearAndMonotoneInInventory) {
  double previous = 1e300;
  for (double q = -5.0; q <= 5.0; q += 1.0) {
    AsInputs in = WorkedExample();
    in.inventory = q;
    const double r = AvellanedaStoikov(in).reservation_price;
    EXPECT_LT(r, previous) << "reservation price must fall as inventory rises, q = " << q;
    previous = r;
  }
  // Linearity: the step is the same everywhere.
  AsInputs a = WorkedExample();
  a.inventory = 0.0;
  AsInputs b = WorkedExample();
  b.inventory = 1.0;
  AsInputs c = WorkedExample();
  c.inventory = 2.0;
  const double step1 =
      AvellanedaStoikov(a).reservation_price - AvellanedaStoikov(b).reservation_price;
  const double step2 =
      AvellanedaStoikov(b).reservation_price - AvellanedaStoikov(c).reservation_price;
  EXPECT_NEAR(step1, step2, 1e-12);
}

TEST(AvellanedaStoikov, TotalSpreadDoesNotDependOnInventory) {
  // A-S skews the CENTRE with inventory, not the width.  Confusing the two is a
  // classic implementation error.
  AsInputs flat = WorkedExample();
  AsInputs loaded = WorkedExample();
  loaded.inventory = 4.0;
  EXPECT_NEAR(AvellanedaStoikov(flat).total_spread, AvellanedaStoikov(loaded).total_spread,
              1e-12);
}

TEST(AvellanedaStoikov, SpreadWidensWithVolatilityAndHorizon) {
  const double base = AvellanedaStoikov(WorkedExample()).total_spread;
  AsInputs vol = WorkedExample();
  vol.sigma = 24.0;
  EXPECT_GT(AvellanedaStoikov(vol).total_spread, base);
  AsInputs horizon = WorkedExample();
  horizon.time_remaining_s = 600.0;
  EXPECT_GT(AvellanedaStoikov(horizon).total_spread, base);
}

TEST(AvellanedaStoikov, RejectsDegenerateInputs) {
  AsInputs bad = WorkedExample();
  bad.gamma = 0.0;
  EXPECT_FALSE(AvellanedaStoikov(bad).valid);
  bad = WorkedExample();
  bad.k = 0.0;
  EXPECT_FALSE(AvellanedaStoikov(bad).valid);
  bad = WorkedExample();
  bad.sigma = -1.0;
  EXPECT_FALSE(AvellanedaStoikov(bad).valid);
}

// ---------------------------------------------------------------------------
// GLFT
// ---------------------------------------------------------------------------
TEST(Glft, IsSymmetricWhenFlat) {
  AsInputs in = WorkedExample();
  in.inventory = 0.0;
  const AsQuotes q = Glft(in);
  ASSERT_TRUE(q.valid);
  // §3.5: "at q=0 both distances equal base + half-skew (symmetric)".
  EXPECT_NEAR(q.delta_bid, q.delta_ask, 1e-9);
  EXPECT_NEAR(q.reservation_price, in.fair_value, 1e-6);
}

TEST(Glft, LongInventoryMovesTheBidAwayAndTheAskCloser) {
  AsInputs flat = WorkedExample();
  AsInputs long_one = WorkedExample();
  long_one.inventory = 1.0;

  const AsQuotes f = Glft(flat);
  const AsQuotes l = Glft(long_one);
  ASSERT_TRUE(f.valid);
  ASSERT_TRUE(l.valid);

  // §3.5 sign check, verbatim: "at q=+1 (long), the bid moves *away* (buy less)
  // and the ask moves *closer* (sell more)."
  EXPECT_GT(l.delta_bid, f.delta_bid);
  EXPECT_LT(l.delta_ask, f.delta_ask);
}

TEST(Glft, DistancesAreMonotoneInInventory) {
  double previous_bid = -1e300;
  double previous_ask = 1e300;
  for (double q = -3.0; q <= 3.0; q += 1.0) {
    AsInputs in = WorkedExample();
    in.inventory = q;
    const AsQuotes quotes = Glft(in);
    ASSERT_TRUE(quotes.valid);
    EXPECT_GT(quotes.delta_bid, previous_bid) << "delta_bid must rise with q, q = " << q;
    EXPECT_LT(quotes.delta_ask, previous_ask) << "delta_ask must fall with q, q = " << q;
    previous_bid = quotes.delta_bid;
    previous_ask = quotes.delta_ask;
  }
}

TEST(Glft, SkewCoefficientIsFiniteForSmallGamma) {
  // (1 + gamma/k)^(1 + k/gamma) has an exponent around 3500 in the worked
  // example; a direct pow() would be hopeless.  In log space it converges to e.
  const double c = GlftSkewCoefficient(WorkedExample());
  EXPECT_TRUE(std::isfinite(c));
  EXPECT_GT(c, 0.0);
}

TEST(Glft, SkewGrowsWithVolatility) {
  AsInputs calm = WorkedExample();
  AsInputs wild = WorkedExample();
  wild.sigma = 32.0;
  EXPECT_GT(GlftSkewCoefficient(wild), GlftSkewCoefficient(calm));
}

TEST(Glft, SkewShrinksAsArrivalRateGrows) {
  // More flow means a position can be worked off faster, so less skew is needed.
  AsInputs quiet = WorkedExample();
  AsInputs busy = WorkedExample();
  busy.a = 100.0;
  EXPECT_LT(GlftSkewCoefficient(busy), GlftSkewCoefficient(quiet));
}

TEST(Glft, BaseDistanceApproachesOneOverKForSmallGamma) {
  // (1/gamma) ln(1 + gamma/k) -> 1/k, exactly half the A-S monopolist markup.
  AsInputs in = WorkedExample();
  in.inventory = 0.0;
  in.sigma = 0.0;  // kill the skew term so only the base survives
  const AsQuotes q = Glft(in);
  EXPECT_NEAR(q.delta_bid, 1.0 / in.k, 0.05);
  EXPECT_NEAR(q.delta_ask, 1.0 / in.k, 0.05);
}

TEST(Glft, NeverQuotesThroughFairValue) {
  AsInputs in = WorkedExample();
  in.inventory = 50.0;  // an extreme long: the raw formula wants a negative ask
  const AsQuotes q = Glft(in);
  ASSERT_TRUE(q.valid);
  EXPECT_GE(q.delta_ask, 0.0);
  EXPECT_GE(q.ask_price, in.fair_value);
}

}  // namespace
}  // namespace lob
