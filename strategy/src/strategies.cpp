#include <lob/strategy/strategies.hpp>

#include <cmath>

namespace lob {

// ---------------------------------------------------------------------------
// S0 -- the strawman joiner
// ---------------------------------------------------------------------------
DesiredQuotes S0Joiner::ComputeQuotes(const BookView& book, const Clock& /*clock*/) {
  DesiredQuotes d;
  if (!book.HasBothSides()) {
    return d;
  }
  // Always one unit at each touch, no skew, no view.  Deliberately naive: this
  // is the controlled comparison for RQ1, not a strategy anyone should run.
  d.want_bid = true;
  d.bid_ticks = book.BestBid();
  d.want_ask = ctx_.config->quote_both_sides;
  d.ask_ticks = book.BestAsk();
  last_reservation_ticks_ = book.MidTicks();
  last_half_spread_ticks_ = 0.5 * static_cast<double>(book.SpreadTicks());
  return d;
}

// ---------------------------------------------------------------------------
// S1 -- fixed spread, inventory-capped
// ---------------------------------------------------------------------------
DesiredQuotes S1FixedSpread::ComputeQuotes(const BookView& book, const Clock& /*clock*/) {
  DesiredQuotes d;
  if (!book.HasBothSides()) {
    return d;
  }
  const double mid_ticks = book.MidTicks();
  const double half = static_cast<double>(ctx_.config->fixed_spread_ticks);

  const Ticks bid = static_cast<Ticks>(std::floor(mid_ticks - half));
  const Ticks ask = static_cast<Ticks>(std::ceil(mid_ticks + half));

  d.want_bid = true;
  d.bid_ticks = ClampToSide(Side::kBid, bid, book);
  d.want_ask = ctx_.config->quote_both_sides;
  d.ask_ticks = ClampToSide(Side::kAsk, ask, book);

  // Pull the side that would push inventory through the cap.  Reconcile()
  // enforces this too; doing it here makes the intent explicit and keeps the
  // diagnostics attributable to the strategy rather than to the plumbing.
  if (AtInventoryCap(Side::kBid)) {
    d.want_bid = false;
  }
  if (AtInventoryCap(Side::kAsk)) {
    d.want_ask = false;
  }

  last_reservation_ticks_ = mid_ticks;
  last_half_spread_ticks_ = half;
  return d;
}

// ---------------------------------------------------------------------------
// S2 -- Avellaneda-Stoikov / GLFT
// ---------------------------------------------------------------------------
AsQuotes S2AvellanedaStoikov::BuildInputsAndSolve() const {
  AsInputs in;
  in.fair_value = FairValue();
  // Inventory in units of the base asset, which is what gamma * sigma^2 * (T-t)
  // is denominated against.
  in.inventory = ctx_.instrument->ToQty(inventory());
  in.sigma = sigma_price();
  in.gamma = ctx_.config->gamma;
  in.k = k_;
  in.a = a_;
  // (T - t) is treated as a fixed rolling horizon rather than a real terminal
  // time (§3.4 practical adaptation): there is no end of the world at 16:00 in
  // a 24/7 crypto market.
  in.time_remaining_s = ctx_.config->horizon_s;
  return glft_ ? Glft(in) : AvellanedaStoikov(in);
}

DesiredQuotes S2AvellanedaStoikov::ComputeQuotes(const BookView& book, const Clock& clock) {
  DesiredQuotes d;
  if (!book.HasBothSides()) {
    return d;
  }
  if (!sigma_.ready()) {
    // Quoting off an unestimated sigma would be quoting off zero risk, which
    // A-S answers with a zero inventory skew.  Wait for the window to fill.
    return d;
  }

  const AsQuotes q = BuildInputsAndSolve();
  if (!q.valid) {
    return d;
  }

  const double tick = ctx_.instrument->tick_size();
  last_reservation_ticks_ = q.reservation_price / tick;
  last_half_spread_ticks_ = q.half_spread / tick;

  // Round outward: a bid rounds DOWN and an ask rounds UP, so tick rounding
  // never makes a quote more aggressive than the model asked for.
  const Ticks raw_bid = static_cast<Ticks>(std::floor(q.bid_price / tick));
  const Ticks raw_ask = static_cast<Ticks>(std::ceil(q.ask_price / tick));

  d.want_bid = true;
  d.bid_ticks = ApplyTickFloor(Side::kBid, raw_bid, book);
  d.want_ask = ctx_.config->quote_both_sides;
  d.ask_ticks = ApplyTickFloor(Side::kAsk, raw_ask, book);

  if (AtInventoryCap(Side::kBid)) {
    d.want_bid = false;
  }
  if (AtInventoryCap(Side::kAsk)) {
    d.want_ask = false;
  }

  ApplyPull(d, clock);
  return d;
}

// ---------------------------------------------------------------------------
// S3 -- weighted mid + toxicity pull
// ---------------------------------------------------------------------------
double S3MicroToxicity::FairValue() const {
  // §3.6: m_w = I * P_ask + (1 - I) * P_bid.  A heavy bid queue means the ask
  // side is likelier to be consumed first, so fair value shifts toward the thin
  // side.  RQ5 asks whether this measurably improves per-fill markouts.
  return ctx_.config->use_weighted_mid ? weighted_mid_price() : mid_price();
}

void S3MicroToxicity::OnTradeExtra(const TradeInfo& /*trade*/, const BookView& /*book*/,
                                   const Clock& clock) {
  const double signed_fraction = flow().SignedFraction(clock.now_us());
  const double threshold = ctx_.config->toxicity_threshold;
  if (std::fabs(signed_fraction) < threshold) {
    return;
  }
  // Sustained one-sided aggression is incoming toxicity.  Buy-aggressive flow
  // (positive) is lifting offers, so it is our ASK that is about to be picked
  // off; sell-aggressive flow endangers our BID.
  const bool endanger_ask = signed_fraction > 0.0;
  const Ts pull_us =
      static_cast<Ts>(ctx_.config->toxicity_pull_s * static_cast<double>(kUsPerSecond));
  pull_until_us_ = clock.now_us() + pull_us;
  pull_ask_ = endanger_ask;
  pull_bid_ = !endanger_ask;
  ++diag_.toxicity_pulls;
}

void S3MicroToxicity::ApplyPull(DesiredQuotes& quotes, const Clock& clock) {
  if (clock.now_us() >= pull_until_us_) {
    pull_bid_ = false;
    pull_ask_ = false;
    return;
  }
  if (pull_bid_) {
    quotes.want_bid = false;
  }
  if (pull_ask_) {
    quotes.want_ask = false;
  }
}

}  // namespace lob
