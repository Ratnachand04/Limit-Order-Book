// Shared quoting machinery for S0-S3 (master plan Part 6).
//
// Every strategy in the ladder computes a desired pair of quotes and then hands
// it to Reconcile(), which owns the parts that are the same everywhere and easy
// to get subtly wrong:
//
//   * the min-move filter -- repricing resets queue position, so a sub-tick
//     improvement is usually a loss (Part 11 pitfall #6).  How often it
//     suppresses a requote is itself an experiment.
//   * the hard inventory cap -- never add to a position at the cap.
//   * the fee-aware minimum edge -- never quote where even a BENIGN fill loses
//     to fees.  At 10 bp retail maker fees this binds almost always, which is
//     finding RQ4 announcing itself, so the counter is reported, not hidden.
//   * the tick-floor rule (§3.4 practical adaptation) -- A-S on a
//     tick-constrained instrument will happily tell you to quote $57 wide;
//     clamping the placement to the touch is what makes it usable.
#pragma once

#include <cstdint>
#include <string>

#include <lob/sim/strategy.hpp>
#include <lob/strategy/volatility.hpp>

namespace lob {

// What the strategy wants to have resting right now.
struct DesiredQuotes {
  bool want_bid = false;
  Ticks bid_ticks = 0;
  bool want_ask = false;
  Ticks ask_ticks = 0;
};

class QuotingStrategy : public Strategy {
 public:
  void OnStart(const StrategyContext& ctx) override;
  void OnBook(const BookView& book, const Clock& clock) override;
  void OnTrade(const TradeInfo& trade, const BookView& book, const Clock& clock) override;
  void OnFill(const Fill& fill, const Clock& clock) override;
  void OnTimer(const BookView& book, const Clock& clock) override;

  [[nodiscard]] double last_sigma_ticks_per_sqrt_s() const override { return last_sigma_ticks_; }
  [[nodiscard]] double last_reservation_price_ticks() const override {
    return last_reservation_ticks_;
  }
  [[nodiscard]] double last_half_spread_ticks() const override { return last_half_spread_ticks_; }

 protected:
  // Subclasses implement this.  Returning an empty DesiredQuotes means "no
  // quotes right now", which is a legitimate and often correct answer.
  virtual DesiredQuotes ComputeQuotes(const BookView& book, const Clock& clock) = 0;

  // Called on every trade before the flow tracker is consulted; S3 overrides.
  virtual void OnTradeExtra(const TradeInfo&, const BookView&, const Clock&) {}

  void Reconcile(const DesiredQuotes& desired, const BookView& book, const Clock& clock);

  // Would a benign fill at this price still beat the maker fee?
  [[nodiscard]] bool PassesMinEdge(Side side, Ticks price_ticks, const BookView& book) const;

  // §3.4 practical adaptation: never place strictly worse than the touch.
  [[nodiscard]] Ticks ApplyTickFloor(Side side, Ticks desired_ticks, const BookView& book) const;

  // Clamps to a valid, non-crossing price on the correct side of the book.
  [[nodiscard]] static Ticks ClampToSide(Side side, Ticks price_ticks, const BookView& book);

  [[nodiscard]] Lots64 inventory() const;
  [[nodiscard]] bool AtInventoryCap(Side side) const;

  [[nodiscard]] double mid_price() const;
  [[nodiscard]] double weighted_mid_price() const;
  [[nodiscard]] double sigma_price() const { return sigma_.sigma(); }
  [[nodiscard]] const VolatilityEstimator& sigma_estimator() const { return sigma_; }
  [[nodiscard]] FlowImbalance& flow() { return flow_; }

  const BookView* book_ = nullptr;
  VolatilityEstimator sigma_{1 * kUsPerSecond, 600 * kUsPerSecond};
  FlowImbalance flow_{2 * kUsPerSecond};

  double last_sigma_ticks_ = 0.0;
  double last_reservation_ticks_ = 0.0;
  double last_half_spread_ticks_ = 0.0;

  OrderId bid_order_ = kNoOrder;
  OrderId ask_order_ = kNoOrder;
  Ticks bid_order_px_ = 0;
  Ticks ask_order_px_ = 0;

 private:
  void ReconcileSide(Side side, bool want, Ticks price_ticks, const BookView& book);
  [[nodiscard]] OrderId& OrderFor(Side side) {
    return side == Side::kBid ? bid_order_ : ask_order_;
  }
  [[nodiscard]] Ticks& PriceFor(Side side) {
    return side == Side::kBid ? bid_order_px_ : ask_order_px_;
  }
};

}  // namespace lob
