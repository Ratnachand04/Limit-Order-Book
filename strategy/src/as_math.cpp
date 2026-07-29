#include <lob/strategy/as_math.hpp>

#include <cmath>

namespace lob {
namespace {

bool Degenerate(const AsInputs& in) {
  return !(in.gamma > 0.0) || !(in.k > 0.0) || !std::isfinite(in.fair_value) ||
         !std::isfinite(in.sigma) || in.sigma < 0.0 || !std::isfinite(in.gamma) ||
         !std::isfinite(in.k);
}

}  // namespace

double MonopolistMarkup(double gamma, double k) {
  if (!(gamma > 0.0) || !(k > 0.0)) {
    return 0.0;
  }
  // As gamma -> 0 this tends to 2/k.  log1p keeps that limit accurate: at
  // gamma/k ~ 3e-4, log(1 + x) computed naively would lose about four digits.
  return (2.0 / gamma) * std::log1p(gamma / k);
}

double InventoryRiskPremium(double gamma, double sigma, double time_remaining_s) {
  if (!(gamma > 0.0) || sigma < 0.0 || time_remaining_s < 0.0) {
    return 0.0;
  }
  return gamma * sigma * sigma * time_remaining_s;
}

AsQuotes AvellanedaStoikov(const AsInputs& in) {
  AsQuotes out;
  if (Degenerate(in)) {
    return out;
  }
  const double premium = InventoryRiskPremium(in.gamma, in.sigma, in.time_remaining_s);

  // r = s - q * gamma * sigma^2 * (T - t).  Long inventory pushes our personal
  // fair value BELOW the market mid: we shade down to attract buyers of the
  // inventory we are carrying and to repel more buying.
  out.inventory_skew = in.inventory * premium;
  out.reservation_price = in.fair_value - out.inventory_skew;

  out.total_spread = premium + MonopolistMarkup(in.gamma, in.k);
  if (out.total_spread < 0.0) {
    out.total_spread = 0.0;
  }
  out.half_spread = 0.5 * out.total_spread;

  // The quotes are centred on r, NOT on s.  That is the whole mechanism by
  // which inventory is managed.
  out.bid_price = out.reservation_price - out.half_spread;
  out.ask_price = out.reservation_price + out.half_spread;
  out.delta_bid = in.fair_value - out.bid_price;
  out.delta_ask = out.ask_price - in.fair_value;
  out.valid = std::isfinite(out.bid_price) && std::isfinite(out.ask_price);
  return out;
}

double GlftSkewCoefficient(const AsInputs& in) {
  if (Degenerate(in) || !(in.a > 0.0)) {
    return 0.0;
  }
  const double ratio = in.gamma / in.k;
  // (1 + gamma/k)^(1 + k/gamma), computed in log space.  The exponent 1 + k/gamma
  // is enormous for small gamma (k/gamma ~ 3500 in the worked example), so the
  // direct pow() would overflow intermediate precision; exp(x * log1p(y)) does
  // not, and tends to e as gamma -> 0 exactly as the algebra says it should.
  const double log_term = (1.0 + in.k / in.gamma) * std::log1p(ratio);
  const double factor = std::exp(log_term);
  const double inner = (in.sigma * in.sigma * in.gamma) / (2.0 * in.k * in.a) * factor;
  if (!(inner > 0.0) || !std::isfinite(inner)) {
    return 0.0;
  }
  return std::sqrt(inner);
}

AsQuotes Glft(const AsInputs& in) {
  AsQuotes out;
  if (Degenerate(in)) {
    return out;
  }
  const double base = (1.0 / in.gamma) * std::log1p(in.gamma / in.k);
  const double c = GlftSkewCoefficient(in);
  const double q = in.inventory;

  // delta_b = base + ((2q + 1)/2) * c   -- long inventory pushes the bid away
  // delta_a = base - ((2q - 1)/2) * c   -- long inventory pulls the ask closer
  out.delta_bid = base + ((2.0 * q + 1.0) / 2.0) * c;
  out.delta_ask = base - ((2.0 * q - 1.0) / 2.0) * c;

  // A negative distance would mean quoting through the fair value, which is an
  // aggressive order, not a quote.  Floor at zero and let the tick-floor rule
  // at the quoting layer decide where it actually lands.
  if (out.delta_bid < 0.0) {
    out.delta_bid = 0.0;
  }
  if (out.delta_ask < 0.0) {
    out.delta_ask = 0.0;
  }

  out.bid_price = in.fair_value - out.delta_bid;
  out.ask_price = in.fair_value + out.delta_ask;
  out.total_spread = out.delta_bid + out.delta_ask;
  out.half_spread = 0.5 * out.total_spread;
  out.reservation_price = 0.5 * (out.bid_price + out.ask_price);
  out.inventory_skew = in.fair_value - out.reservation_price;
  out.valid = std::isfinite(out.bid_price) && std::isfinite(out.ask_price);
  return out;
}

}  // namespace lob
