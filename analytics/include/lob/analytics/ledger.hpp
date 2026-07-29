// Cash, inventory and the exact PnL decomposition (master plan §3.9).
//
//   E_t = X_t + q_t * m_t                                  (equity)
//
//   dE  = SUM_fills s_i * (m_ti - p_i) * v_i               (spread capture)
//       + INTEGRAL q_{t-} dm                               (inventory PnL)
//       - SUM_fills f * p_i * v_i                          (fees)
//
// The master plan asks for the identity to be asserted "within 1e-9 of
// notional".  It is asserted EXACTLY here, because every term is an integer:
//
//   * cash is an integer count of 1e-8 units (CLAUDE.md);
//   * a notional is price_ticks * qty_lots * (tick_size * lot_size * 1e8),
//     and Instrument's constructor rejects any instrument whose last factor is
//     not a whole number, so no rounding ever enters;
//   * the mid can be a half tick, so every quantity below is carried at TWICE
//     its natural value ("x2 units", i.e. counts of 0.5e-8).  Doubling removes
//     the only source of fractions in the whole ledger.
//
// An exact identity is a much sharper bug detector than a tolerance: any
// residual other than zero is a real defect, not accumulated float error.
#pragma once

#include <cstdint>
#include <string>

#include <lob/config.hpp>
#include <lob/instrument.hpp>
#include <lob/types.hpp>

namespace lob {

struct LedgerSnapshot {
  Cash cash_1e8 = 0;
  Lots64 inventory_lots = 0;
  std::int64_t equity_x2 = 0;
  std::int64_t spread_capture_x2 = 0;
  std::int64_t inventory_pnl_x2 = 0;
  std::int64_t fees_x2 = 0;
  std::uint64_t fill_count = 0;
  Lots64 volume_lots = 0;
  std::int64_t notional_traded_1e8 = 0;
};

class Ledger {
 public:
  Ledger(Instrument instrument, FeeConfig fees);

  // --- marking -------------------------------------------------------------
  // Must be called on every mid change, before any fill at the new mid, so the
  // inventory term integrates q against the mid path exactly.
  void MarkTo(std::int64_t mid_x2_ticks);

  // --- fills ---------------------------------------------------------------
  // `our_side` is OUR side: kBid means we bought.  `maker` selects the fee
  // tier; passive fills are maker, forced liquidations at the touch are taker.
  void ApplyFill(Side our_side, Ticks price_ticks, Lots64 qty_lots,
                 std::int64_t mid_x2_ticks_at_fill, bool maker = true);

  // --- state ---------------------------------------------------------------
  [[nodiscard]] Cash cash_1e8() const { return cash_1e8_; }
  [[nodiscard]] Lots64 inventory_lots() const { return inventory_lots_; }
  [[nodiscard]] std::int64_t mid_x2_ticks() const { return mid_x2_ticks_; }
  [[nodiscard]] bool marked() const { return marked_; }

  // Equity in x2 units: 2 * (cash + q * mid).
  [[nodiscard]] std::int64_t EquityX2() const;
  [[nodiscard]] double EquityCurrency() const;

  [[nodiscard]] std::int64_t spread_capture_x2() const { return spread_capture_x2_; }
  [[nodiscard]] std::int64_t inventory_pnl_x2() const { return inventory_pnl_x2_; }
  [[nodiscard]] std::int64_t fees_x2() const { return fees_x2_; }
  [[nodiscard]] std::uint64_t fill_count() const { return fill_count_; }
  [[nodiscard]] Lots64 volume_lots() const { return volume_lots_; }
  [[nodiscard]] std::int64_t notional_traded_1e8() const { return notional_traded_1e8_; }

  [[nodiscard]] LedgerSnapshot Snapshot() const;

  // --- the assertion that catches every backtester bug ---------------------
  // Returns (equity change) - (spread capture + inventory - fees).  Must be
  // exactly 0.  Anything else is a defect in the ledger or its callers.
  [[nodiscard]] std::int64_t IdentityResidualX2() const;
  [[nodiscard]] bool IdentityHolds() const { return IdentityResidualX2() == 0; }
  [[nodiscard]] std::string IdentityReport() const;

  // Converts an x2 cash amount to currency units for reporting.
  [[nodiscard]] static double X2ToCurrency(std::int64_t x2) {
    return static_cast<double>(x2) / (2.0 * static_cast<double>(kCashScale));
  }

  // Normalises an x2 cash amount to basis points of an x2 notional.
  [[nodiscard]] static double ToBasisPoints(std::int64_t value_x2, std::int64_t notional_x2) {
    if (notional_x2 == 0) {
      return 0.0;
    }
    return 10000.0 * static_cast<double>(value_x2) / static_cast<double>(notional_x2);
  }

  [[nodiscard]] const Instrument& instrument() const { return instrument_; }
  [[nodiscard]] const FeeConfig& fees() const { return fees_; }

  void Reset();

 private:
  Instrument instrument_;
  FeeConfig fees_;
  std::int64_t cash_per_tick_lot_ = 0;

  Cash cash_1e8_ = 0;
  Lots64 inventory_lots_ = 0;
  std::int64_t mid_x2_ticks_ = 0;
  bool marked_ = false;

  std::int64_t initial_equity_x2_ = 0;
  std::int64_t spread_capture_x2_ = 0;
  std::int64_t inventory_pnl_x2_ = 0;
  std::int64_t fees_x2_ = 0;

  std::uint64_t fill_count_ = 0;
  Lots64 volume_lots_ = 0;
  std::int64_t notional_traded_1e8_ = 0;
};

}  // namespace lob
