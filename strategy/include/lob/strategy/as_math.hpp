// Avellaneda-Stoikov and Guéant-Lehalle-Fernandez-Tapia closed forms.
//
// Kept as free functions over plain doubles, with no book, no clock and no
// state, so they can be tested directly against the master plan's worked
// example and against the monotonicity properties that must hold.
//
// ---------------------------------------------------------------------------
// A-S (2008), master plan §3.4
// ---------------------------------------------------------------------------
//   reservation price   r = s - q * gamma * sigma^2 * (T - t)
//   total spread        delta_a + delta_b = gamma * sigma^2 * (T - t)
//                                         + (2 / gamma) * ln(1 + gamma / k)
//   quotes              ask = r + spread/2,  bid = r - spread/2
//
// Term one of the spread charges for the volatility risk a fill creates.  Term
// two is the monopolist-dealer markup: it SHRINKS as k grows (an elastic market
// punishes wide quotes with no fills) and grows with gamma.
//
// The small-gamma limit is worth knowing cold: (2/gamma) * ln(1 + gamma/k)
// -> 2/k, i.e. the market's elasticity, not your risk aversion, sets the
// spread.
//
// ---------------------------------------------------------------------------
// GLFT (2013), master plan §3.5
// ---------------------------------------------------------------------------
//   delta_b*(q) = (1/gamma) ln(1 + gamma/k) + ((2q+1)/2) * sqrt(C)
//   delta_a*(q) = (1/gamma) ln(1 + gamma/k) - ((2q-1)/2) * sqrt(C)
//   C           = (sigma^2 * gamma) / (2 * k * A) * (1 + gamma/k)^(1 + k/gamma)
//
// These are the asymptotic, inventory-bounded distances practitioners actually
// use, because they do not depend on a fictitious terminal time.  Sanity check
// the signs: at q = 0 the two distances are equal; at q = +1 (long) the bid
// moves AWAY (buy less) and the ask moves CLOSER (sell more).
//
// UNITS.  Everything here is in PRICE units, matching the master plan's worked
// example (gamma = 1e-5 $^-1, k = 0.035 $^-1, sigma = 8 $/sqrt(s)).  Conversion
// to the tick grid happens at the quoting layer, once, at the end.
#pragma once

namespace lob {

struct AsInputs {
  double fair_value = 0.0;       // s -- the mid, or the weighted mid for S3
  double inventory = 0.0;        // q, signed, in units of the base asset
  double sigma = 0.0;            // price units per sqrt(second)
  double gamma = 1.0e-5;         // risk aversion, 1/price
  double k = 0.035;              // intensity decay, 1/price
  double a = 1.0;                // base arrival rate at the mid, per second
  double time_remaining_s = 60.0;  // (T - t)
};

struct AsQuotes {
  double reservation_price = 0.0;
  double total_spread = 0.0;
  double half_spread = 0.0;
  double bid_price = 0.0;
  double ask_price = 0.0;
  double delta_bid = 0.0;  // distance from fair value down to the bid
  double delta_ask = 0.0;  // distance from fair value up to the ask
  double inventory_skew = 0.0;  // fair_value - reservation_price
  bool valid = false;           // false when the inputs are degenerate
};

// (2/gamma) * ln(1 + gamma/k), evaluated with log1p so it stays accurate as
// gamma/k approaches zero (where the naive form loses every significant digit).
double MonopolistMarkup(double gamma, double k);

// gamma * sigma^2 * (T - t) -- the inventory risk premium, and also the shift
// of the reservation price per unit of inventory.
double InventoryRiskPremium(double gamma, double sigma, double time_remaining_s);

AsQuotes AvellanedaStoikov(const AsInputs& in);
AsQuotes Glft(const AsInputs& in);

// The GLFT skew coefficient sqrt(C) above.  Exposed for tests.
double GlftSkewCoefficient(const AsInputs& in);

}  // namespace lob
