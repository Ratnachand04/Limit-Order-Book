#include <lob/analytics/markout.hpp>

#include <algorithm>
#include <functional>

namespace lob {

MarkoutSampler::MarkoutSampler(Instrument instrument, std::vector<Ts> horizons_us)
    : instrument_(std::move(instrument)), horizons_us_(std::move(horizons_us)) {
  std::sort(horizons_us_.begin(), horizons_us_.end());
  horizons_us_.erase(std::unique(horizons_us_.begin(), horizons_us_.end()), horizons_us_.end());
}

void MarkoutSampler::Reset() {
  fills_.clear();
  samples_.clear();
  due_ = decltype(due_)();
  last_mid_x2_ = 0;
  have_mid_ = false;
}

std::uint64_t MarkoutSampler::AddFill(Ts ts_us, Side our_side, Ticks price_ticks, Lots64 qty_lots,
                                      std::int64_t mid_x2_ticks) {
  FillRecord rec;
  rec.ts_us = ts_us;
  rec.side = our_side;
  rec.price_ticks = price_ticks;
  rec.qty_lots = qty_lots;
  rec.mid_x2_at_fill = mid_x2_ticks;
  // Notional in x2 units so it lines up with the x2 cash quantities the ledger
  // produces; the bp ratios below are then pure integers over integers.
  rec.notional_x2 = 2 * instrument_.Notional(price_ticks, qty_lots);

  const std::uint64_t fill_id = static_cast<std::uint64_t>(fills_.size());
  fills_.push_back(rec);

  for (const Ts h : horizons_us_) {
    MarkoutSample s;
    s.fill_id = fill_id;
    s.horizon_us = h;
    s.mid_x2_at_fill = mid_x2_ticks;
    const std::size_t index = samples_.size();
    samples_.push_back(s);
    due_.push(Due{ts_us + h, index});
  }
  return fill_id;
}

void MarkoutSampler::Resolve(std::size_t sample_index, std::int64_t mid_x2) {
  MarkoutSample& s = samples_[sample_index];
  const FillRecord& f = fills_[static_cast<std::size_t>(s.fill_id)];
  if (f.notional_x2 == 0) {
    s.resolved = true;
    return;
  }
  const std::int64_t sign = SignOf(f.side);
  const std::int64_t cpt = instrument_.cash_per_tick_lot();

  // All three terms in x2 cash units, then normalised to bp of notional.
  const std::int64_t edge_x2 =
      sign * (f.mid_x2_at_fill - 2 * static_cast<std::int64_t>(f.price_ticks)) * f.qty_lots * cpt;
  const std::int64_t adverse_x2 = sign * (mid_x2 - f.mid_x2_at_fill) * f.qty_lots * cpt;

  const double denom = static_cast<double>(f.notional_x2);
  s.edge_bp = 10000.0 * static_cast<double>(edge_x2) / denom;
  s.adverse_selection_bp = 10000.0 * static_cast<double>(adverse_x2) / denom;
  s.markout_bp = s.edge_bp + s.adverse_selection_bp;
  s.mid_x2_at_horizon = mid_x2;
  s.resolved = true;
}

void MarkoutSampler::ResolveUpTo(Ts limit_us, bool inclusive, std::int64_t mid_x2) {
  while (!due_.empty()) {
    const Due& d = due_.top();
    const bool ready = inclusive ? (d.ts_us <= limit_us) : (d.ts_us < limit_us);
    if (!ready) {
      break;
    }
    const std::size_t index = d.sample_index;
    due_.pop();
    Resolve(index, mid_x2);
  }
}

void MarkoutSampler::Advance(Ts now_us, std::int64_t mid_x2_ticks) {
  if (have_mid_) {
    // Samples due strictly before `now` are resolved at the mid that was in
    // force over that interval -- the previous one, not the new one.
    ResolveUpTo(now_us, /*inclusive=*/false, last_mid_x2_);
  }
  last_mid_x2_ = mid_x2_ticks;
  have_mid_ = true;
  // Samples due exactly now see the new mid.
  ResolveUpTo(now_us, /*inclusive=*/true, mid_x2_ticks);
}

std::vector<MarkoutSummary> MarkoutSampler::Summarise() const {
  std::vector<MarkoutSummary> out;
  out.reserve(horizons_us_.size());
  for (const Ts h : horizons_us_) {
    MarkoutSummary sum;
    sum.horizon_us = h;
    double edge = 0.0;
    double adverse = 0.0;
    double markout = 0.0;
    for (const MarkoutSample& s : samples_) {
      if (s.horizon_us != h) {
        continue;
      }
      if (!s.resolved) {
        ++sum.unresolved;
        continue;
      }
      ++sum.resolved;
      edge += s.edge_bp;
      adverse += s.adverse_selection_bp;
      markout += s.markout_bp;
    }
    if (sum.resolved > 0) {
      const double n = static_cast<double>(sum.resolved);
      sum.mean_edge_bp = edge / n;
      sum.mean_adverse_selection_bp = adverse / n;
      sum.mean_markout_bp = markout / n;
    }
    out.push_back(sum);
  }
  return out;
}

}  // namespace lob
