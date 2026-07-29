// Markout sampling -- adverse selection made visible (master plan §3.9).
//
//   MO_i(h) = s_i * (m_{t_i + h} - p_i) * v_i
//           = s_i * (m_{t_i} - p_i) * v_i            <- edge at the fill
//           + s_i * (m_{t_i + h} - m_{t_i}) * v_i    <- adverse selection at h
//
// The second term is negative on average for a maker: your bid gets hit
// precisely when sellers are aggressive, so the mid tends to fall after you
// buy.  Plotting the average against h gives the markout curve, which is the
// paper's money plot (F1) and the source of the CV bracket numbers.
//
// Unresolved samples: a fill near the end of the run has no mid 60 s later.
// Those samples are reported as UNRESOLVED and excluded from averages.  They
// are never back-filled with the last known mid -- that would silently bias
// long horizons toward zero adverse selection (CLAUDE.md rule 4: never
// fabricate results).
#pragma once

#include <cstdint>
#include <queue>
#include <vector>

#include <lob/instrument.hpp>
#include <lob/types.hpp>

namespace lob {

struct MarkoutSample {
  std::uint64_t fill_id = 0;
  Ts horizon_us = 0;
  bool resolved = false;

  // All three are signed from OUR point of view and normalised to basis points
  // of the fill's own notional, so fills of different sizes are comparable.
  double edge_bp = 0.0;               // s * (m_t   - p) * v / notional
  double adverse_selection_bp = 0.0;  // s * (m_t+h - m_t) * v / notional
  double markout_bp = 0.0;            // edge + adverse selection

  std::int64_t mid_x2_at_fill = 0;
  std::int64_t mid_x2_at_horizon = 0;
};

struct MarkoutSummary {
  Ts horizon_us = 0;
  std::uint64_t resolved = 0;
  std::uint64_t unresolved = 0;
  double mean_edge_bp = 0.0;
  double mean_adverse_selection_bp = 0.0;
  double mean_markout_bp = 0.0;
};

class MarkoutSampler {
 public:
  MarkoutSampler(Instrument instrument, std::vector<Ts> horizons_us);

  // Registers a fill.  `ts_us` is local time; `mid_x2_ticks` the mid at the
  // fill.  Returns the fill id assigned.
  std::uint64_t AddFill(Ts ts_us, Side our_side, Ticks price_ticks, Lots64 qty_lots,
                        std::int64_t mid_x2_ticks);

  // Advances the sampler's notion of the mid path.  Call on every mid change
  // AND on the final event of the run.
  void Advance(Ts now_us, std::int64_t mid_x2_ticks);

  [[nodiscard]] const std::vector<MarkoutSample>& samples() const { return samples_; }
  [[nodiscard]] const std::vector<Ts>& horizons_us() const { return horizons_us_; }
  [[nodiscard]] std::uint64_t pending() const { return static_cast<std::uint64_t>(due_.size()); }
  [[nodiscard]] std::vector<MarkoutSummary> Summarise() const;

  void Reset();

 private:
  struct FillRecord {
    Ts ts_us = 0;
    Side side = Side::kBid;
    Ticks price_ticks = 0;
    Lots64 qty_lots = 0;
    std::int64_t mid_x2_at_fill = 0;
    std::int64_t notional_x2 = 0;
  };

  struct Due {
    Ts ts_us = 0;
    std::size_t sample_index = 0;
    bool operator>(const Due& other) const {
      if (ts_us != other.ts_us) {
        return ts_us > other.ts_us;
      }
      return sample_index > other.sample_index;
    }
  };

  void ResolveUpTo(Ts limit_us, bool inclusive, std::int64_t mid_x2);
  void Resolve(std::size_t sample_index, std::int64_t mid_x2);

  Instrument instrument_;
  std::vector<Ts> horizons_us_;
  std::vector<FillRecord> fills_;
  std::vector<MarkoutSample> samples_;
  std::priority_queue<Due, std::vector<Due>, std::greater<Due>> due_;

  std::int64_t last_mid_x2_ = 0;
  bool have_mid_ = false;
};

}  // namespace lob
