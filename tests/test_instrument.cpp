// The integer grid and the exact cash ledger (master plan §4.1, CLAUDE.md).
#include <gtest/gtest.h>

#include <stdexcept>

#include <lob/instrument.hpp>

namespace lob {
namespace {

TEST(Instrument, ConvertsPricesAndQuantitiesToTheIntegerGrid) {
  const Instrument btc("BTCUSDT", 0, 0.01, 0.00001);
  EXPECT_EQ(btc.ToTicks(100000.00), 10'000'000);
  EXPECT_EQ(btc.ToTicks(99999.99), 9'999'999);
  EXPECT_EQ(btc.ToLots(0.1), 10000);
  // llround, not truncation: half-ticks round away from zero, not toward it.
  EXPECT_EQ(btc.ToTicks(0.015), 2);
  EXPECT_DOUBLE_EQ(btc.ToPrice(10'000'000), 100000.00);
}

TEST(Instrument, FloatingPointNoiseDoesNotShiftALevel) {
  // The reason prices are integers at all (Part 11 pitfall #3): 0.1 + 0.2 is not
  // 0.3, and a book keyed on doubles quietly grows a second level at "0.3".
  const Instrument inst("X", 0, 0.01, 0.001);
  EXPECT_EQ(inst.ToTicks(0.1 + 0.2), inst.ToTicks(0.3));
}

TEST(Instrument, NotionalIsExactIntegerArithmetic) {
  const Instrument inst("X", 0, 0.01, 0.001);
  // tick * lot * 1e8 = 0.01 * 0.001 * 1e8 = 1000 units of 1e-8 per tick-lot.
  EXPECT_EQ(inst.cash_per_tick_lot(), 1000);

  // 100.00 x 2.0 = 200.00 -> 200 * 1e8 = 2e10.
  const Ticks px = inst.ToTicks(100.00);   // 10000
  const Lots lots = inst.ToLots(2.0);      // 2000
  EXPECT_EQ(inst.Notional(px, lots), 20'000'000'000LL);
  EXPECT_DOUBLE_EQ(Instrument::CashToDouble(inst.Notional(px, lots)), 200.0);
}

TEST(Instrument, RejectsInstrumentsThatWouldMakeCashInexact) {
  // tick * lot * 1e8 must be a whole number, otherwise the ledger would round
  // on every fill and the §3.9 identity could not be asserted exactly.
  EXPECT_THROW(Instrument("bad", 0, 0.01, 1e-9), std::invalid_argument);
  EXPECT_THROW(Instrument("bad", 0, 0.0, 0.001), std::invalid_argument);
  EXPECT_THROW(Instrument("bad", 0, 0.01, -1.0), std::invalid_argument);
}

TEST(Instrument, FeeGridIsExactInTenthsOfABasisPoint) {
  // The master plan §2.4 fee grid is {-0.5, 0, 1, 2, 5, 10} bp.  Held as tenths
  // of a bp, every one of them is an exact integer.
  const Cash notional = 20'000'000'000LL;  // $200

  EXPECT_EQ(Instrument::Fee(notional, 0), 0);
  // 2 bp of $200 = $0.04 = 4e6 units of 1e-8.
  EXPECT_EQ(Instrument::Fee(notional, 20), 4'000'000);
  // 10 bp of $200 = $0.20.
  EXPECT_EQ(Instrument::Fee(notional, 100), 20'000'000);
  // A rebate is a negative fee and must come back negative, not clamped.
  EXPECT_EQ(Instrument::Fee(notional, -5), -1'000'000);
}

TEST(Instrument, FeeRoundsHalfAwayFromZeroSymmetrically) {
  // 1 tenth-bp of 50000 units is 0.5 -> rounds to 1, and -1 on the rebate side.
  EXPECT_EQ(Instrument::Fee(50'000, 1), 1);
  EXPECT_EQ(Instrument::Fee(50'000, -1), -1);
}

TEST(Instrument, RejectsPricesOutsideTheTickGrid) {
  const Instrument inst("X", 0, 0.01, 0.001);
  // The results are [[nodiscard]], so they are consumed rather than dropped.
  EXPECT_THROW({ volatile Ticks t = inst.ToTicks(1e12); (void)t; }, std::out_of_range);
  EXPECT_THROW({ volatile Lots l = inst.ToLots(1e12); (void)l; }, std::out_of_range);
}

TEST(InstrumentTable, ResolvesSymbolIdsAndRejectsDuplicates) {
  InstrumentTable table;
  table.Add(Instrument("AAA", 0, 0.01, 0.001));
  table.Add(Instrument("BBB", 1, 0.1, 0.01));

  EXPECT_EQ(table.size(), 2u);
  EXPECT_EQ(table.ById(0).symbol(), "AAA");
  EXPECT_EQ(table.ById(1).symbol(), "BBB");
  EXPECT_NE(table.Find("BBB"), nullptr);
  EXPECT_EQ(table.Find("CCC"), nullptr);
  EXPECT_THROW({ const Instrument& i = table.ById(2); (void)i.symbol(); }, std::out_of_range);
  EXPECT_THROW(table.Add(Instrument("CCC", 0, 0.01, 0.001)), std::invalid_argument);
}

}  // namespace
}  // namespace lob
