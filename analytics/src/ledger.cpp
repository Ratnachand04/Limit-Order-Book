#include <lob/analytics/ledger.hpp>

#include <sstream>

namespace lob {

Ledger::Ledger(Instrument instrument, FeeConfig fees)
    : instrument_(std::move(instrument)),
      fees_(fees),
      cash_per_tick_lot_(instrument_.cash_per_tick_lot()) {}

void Ledger::Reset() {
  cash_1e8_ = 0;
  inventory_lots_ = 0;
  mid_x2_ticks_ = 0;
  marked_ = false;
  initial_equity_x2_ = 0;
  spread_capture_x2_ = 0;
  inventory_pnl_x2_ = 0;
  fees_x2_ = 0;
  fill_count_ = 0;
  volume_lots_ = 0;
  notional_traded_1e8_ = 0;
}

void Ledger::MarkTo(std::int64_t mid_x2_ticks) {
  if (!marked_) {
    // The first mark defines the origin of the equity path.  Booking the
    // inventory term from a zero mid would credit a spurious q * m_0.
    mid_x2_ticks_ = mid_x2_ticks;
    marked_ = true;
    initial_equity_x2_ = EquityX2();
    return;
  }
  if (mid_x2_ticks == mid_x2_ticks_) {
    return;
  }
  // Inventory PnL over this step: q_{t-} * dm.  In x2 units the mid difference
  // is already doubled, so no further scaling is needed.
  const std::int64_t dm_x2 = mid_x2_ticks - mid_x2_ticks_;
  inventory_pnl_x2_ += inventory_lots_ * dm_x2 * cash_per_tick_lot_;
  mid_x2_ticks_ = mid_x2_ticks;
}

void Ledger::ApplyFill(Side our_side, Ticks price_ticks, Lots64 qty_lots,
                       std::int64_t mid_x2_ticks_at_fill, bool maker) {
  if (qty_lots <= 0) {
    return;
  }
  const std::int64_t sign = SignOf(our_side);  // +1 when we buy, -1 when we sell
  const Cash notional = instrument_.Notional(price_ticks, qty_lots);
  const std::int64_t fee_tenth_bp = maker ? fees_.maker_tenth_bp : fees_.taker_tenth_bp;
  const Cash fee = Instrument::Fee(notional, fee_tenth_bp);

  // Cash: buying pays out, selling takes in; the fee is always a cost (or a
  // credit when the tier is a rebate, i.e. fee_tenth_bp < 0).
  cash_1e8_ -= sign * notional;
  cash_1e8_ -= fee;
  inventory_lots_ += sign * qty_lots;

  // Spread capture at the moment of the fill: s * (m - p) * v, doubled.
  const std::int64_t edge_x2 =
      sign * (mid_x2_ticks_at_fill - 2 * static_cast<std::int64_t>(price_ticks)) * qty_lots *
      cash_per_tick_lot_;
  spread_capture_x2_ += edge_x2;
  fees_x2_ += 2 * fee;

  ++fill_count_;
  volume_lots_ += qty_lots;
  notional_traded_1e8_ += notional;

  // A fill does not move the mid, but the caller may be marking at a mid that
  // differs from the last MarkTo (for instance the mid observed at the trade).
  // Booking the difference here keeps the identity exact.
  if (marked_ && mid_x2_ticks_at_fill != mid_x2_ticks_) {
    const std::int64_t dm_x2 = mid_x2_ticks_at_fill - mid_x2_ticks_;
    // Use the inventory BEFORE this fill: the position that was actually
    // carried across the mid move.
    const Lots64 prior_inventory = inventory_lots_ - sign * qty_lots;
    inventory_pnl_x2_ += prior_inventory * dm_x2 * cash_per_tick_lot_;
    mid_x2_ticks_ = mid_x2_ticks_at_fill;
  } else if (!marked_) {
    mid_x2_ticks_ = mid_x2_ticks_at_fill;
    marked_ = true;
    // Equity origin must exclude everything this fill just booked.
    initial_equity_x2_ = EquityX2() - (spread_capture_x2_ - fees_x2_);
  }
}

std::int64_t Ledger::EquityX2() const {
  return 2 * cash_1e8_ + inventory_lots_ * mid_x2_ticks_ * cash_per_tick_lot_;
}

double Ledger::EquityCurrency() const { return X2ToCurrency(EquityX2()); }

std::int64_t Ledger::IdentityResidualX2() const {
  const std::int64_t delta_equity = EquityX2() - initial_equity_x2_;
  const std::int64_t components = spread_capture_x2_ + inventory_pnl_x2_ - fees_x2_;
  return delta_equity - components;
}

std::string Ledger::IdentityReport() const {
  std::ostringstream os;
  os << "PnL decomposition (" << instrument_.symbol() << ")\n"
     << "  equity change    " << X2ToCurrency(EquityX2() - initial_equity_x2_) << "\n"
     << "  spread capture   " << X2ToCurrency(spread_capture_x2_) << "\n"
     << "  inventory PnL    " << X2ToCurrency(inventory_pnl_x2_) << "\n"
     << "  fees             " << X2ToCurrency(-fees_x2_) << "\n"
     << "  residual         " << IdentityResidualX2() << " (x2 units, must be exactly 0)\n"
     << "  fills            " << fill_count_ << "\n"
     << "  volume (lots)    " << volume_lots_ << "\n"
     << "  inventory (lots) " << inventory_lots_ << "\n";
  return os.str();
}

LedgerSnapshot Ledger::Snapshot() const {
  LedgerSnapshot s;
  s.cash_1e8 = cash_1e8_;
  s.inventory_lots = inventory_lots_;
  s.equity_x2 = EquityX2();
  s.spread_capture_x2 = spread_capture_x2_;
  s.inventory_pnl_x2 = inventory_pnl_x2_;
  s.fees_x2 = fees_x2_;
  s.fill_count = fill_count_;
  s.volume_lots = volume_lots_;
  s.notional_traded_1e8 = notional_traded_1e8_;
  return s;
}

}  // namespace lob
