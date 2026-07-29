// Typed experiment configuration -- the C++ shape of master plan Appendix A.
//
// Rule from CLAUDE.md: "no unexplained magic numbers (constants named and cited
// to master-plan sections)".  Every tunable the simulator has lives here, is
// loaded from YAML, and is echoed into each run's manifest so a result can
// always be traced back to the exact inputs that produced it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lob/instrument.hpp>
#include <lob/types.hpp>
#include <lob/yaml.hpp>

namespace lob {

// --- §3.7 queue-position assumptions ---------------------------------------
enum class QueueModel : std::uint8_t {
  kPess = 0,  // cancels come from BEHIND first -> lower bound on fills
  kOpt = 1,   // cancels come from AHEAD first  -> upper bound on fills
  kProp = 2,  // cancels are uniform over the queue -> central estimate
};

std::string_view QueueModelName(QueueModel m);
QueueModel ParseQueueModel(std::string_view s);

// --- §4.5 latency model ----------------------------------------------------
struct LatencyConfig {
  // delta_in: market events become visible to the strategy at exch_ts + delta_in.
  Ts in_us = 50 * kUsPerMilli;
  // delta_out: strategy actions reach the exchange at decision_ts + delta_out.
  Ts out_us = 50 * kUsPerMilli;
  // Both legs add an independent Exp draw with this mean, from the seeded RNG.
  double jitter_exp_us = 5.0 * static_cast<double>(kUsPerMilli);

  static LatencyConfig FromYaml(const yaml::Node& n);
};

// --- fees ------------------------------------------------------------------
// Held in tenths of a basis point so the whole master plan §2.4 grid
// {-0.5, 0, 1, 2, 5, 10} bp is exactly representable in integers and the cash
// ledger never rounds.  Negative means a rebate.
struct FeeConfig {
  std::int64_t maker_tenth_bp = 20;  // 2.0 bp, the futures base tier
  std::int64_t taker_tenth_bp = 50;  // 5.0 bp; only used by forced liquidation

  [[nodiscard]] double maker_bp() const { return static_cast<double>(maker_tenth_bp) / 10.0; }
  [[nodiscard]] double taker_bp() const { return static_cast<double>(taker_tenth_bp) / 10.0; }

  static FeeConfig FromYaml(const yaml::Node& n);
};

// --- strategy --------------------------------------------------------------
struct StrategyConfig {
  // S0_touch | S0_queue | S1 | S2_AS | S2_GLFT | S3
  std::string name = "S0_queue";

  // Common quoting controls (Part 6).
  Lots order_size_lots = 1;
  Lots64 q_max_lots = 5;               // hard inventory cap
  Ticks requote_min_ticks = 2;         // min-move filter; repricing costs queue position
  Ts timer_us = 100 * kUsPerMilli;     // on_timer cadence
  Ticks fixed_spread_ticks = 1;        // S1: distance each side of mid
  bool quote_both_sides = true;

  // §3.2 volatility estimator.
  Ts sigma_sample_us = 1 * kUsPerSecond;   // Delta = 1 s: slower loses reactivity,
  Ts sigma_window_us = 600 * kUsPerSecond;  // faster picks up bid-ask bounce.

  // §3.4 Avellaneda-Stoikov.
  double gamma = 1.0e-5;                 // risk aversion, units 1/price
  double horizon_s = 60.0;               // rolling (T - t), not a real terminal time
  std::string k_a_calibration_path;      // JSON from the lambda(delta) fit, §3.3
  double k_fallback = 0.035;             // used only when no calibration file is given
  double a_fallback = 1.0;
  bool glft_mode = false;                // §3.5 steady-state distances instead of A-S

  // §3.4 practical adaptation: never quote strictly worse than the touch.
  bool tick_floor_at_touch = true;

  // §3.6 / Part 6 -- S3 only.
  bool use_weighted_mid = false;
  double toxicity_window_s = 2.0;
  double toxicity_threshold = 0.75;  // |signed flow| / total flow over the window
  double toxicity_pull_s = 1.0;      // how long the endangered side stays pulled

  // Risk controls (Part 6).  A quote whose best case still loses to fees is
  // never sent; the counter for how often this binds is itself finding RQ4.
  bool enforce_min_edge = true;

  static StrategyConfig FromYaml(const yaml::Node& n);
};

// --- §3.8 probe orders -----------------------------------------------------
struct ProbeConfig {
  bool enabled = false;
  Ts interval_us = 5 * kUsPerSecond;             // spawn cadence
  std::vector<Ticks> depths_ticks{1, 2, 3, 5, 10};
  std::vector<Ts> horizons_us{1 * kUsPerSecond, 5 * kUsPerSecond, 30 * kUsPerSecond,
                              60 * kUsPerSecond};
  Lots size_lots = 1;

  static ProbeConfig FromYaml(const yaml::Node& n);
};

// --- §3.9 markout sampling -------------------------------------------------
struct MarkoutConfig {
  // h in {0.1, 0.5, 1, 2, 5, 10, 30, 60} s -- CLAUDE.md Phase 5.
  std::vector<Ts> horizons_us{100'000,    500'000,    1'000'000,  2'000'000,
                              5'000'000,  10'000'000, 30'000'000, 60'000'000};

  static MarkoutConfig FromYaml(const yaml::Node& n);
};

// --- top level -------------------------------------------------------------
struct RunConfig {
  std::string run_id;                 // stamped into every output row
  std::vector<Instrument> instruments;
  LatencyConfig latency;
  QueueModel queue_model = QueueModel::kPess;
  FeeConfig fees;
  StrategyConfig strategy;
  ProbeConfig probes;
  MarkoutConfig markouts;
  std::uint64_t seed = 42;

  // Inclusive date bounds, "YYYY-MM-DD".  Empty means unbounded.
  std::string eval_start;
  std::string eval_end;

  std::string input_path;   // binary event file or directory
  std::string output_dir = "data/results";

  // "QUEUE" (the honest model) or "TOUCH" (the naive fiction of RQ1).
  // Defaults to TOUCH only for the strategy literally named S0_touch, whose
  // whole purpose is to be the strawman in that comparison.
  std::string fill_model = "QUEUE";

  // Runs the book core with both containers and asserts they agree (§4.4).
  bool dual_book_check = false;

  [[nodiscard]] const Instrument& PrimaryInstrument() const;
  [[nodiscard]] InstrumentTable BuildInstrumentTable() const;

  // Canonical text form of the resolved config, written next to every result
  // set so a number in the paper can be traced to its inputs.
  [[nodiscard]] std::string Manifest() const;

  static RunConfig FromYaml(const yaml::Node& root);
  static RunConfig FromFile(const std::string& path);
};

}  // namespace lob
