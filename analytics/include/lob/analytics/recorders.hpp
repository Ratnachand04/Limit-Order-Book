// CSV output for a run (master plan §4.8).
//
// Simulation and analysis are fully separated: the C++ side writes these four
// files and nothing else, and every figure and table in the paper is produced
// by the Python notebooks from these files alone.  That separation is what
// makes "one command reproduces every figure from committed CSVs" achievable.
//
// Schemas are fixed here.  Changing a column means regenerating every result,
// so the header strings are the contract.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <lob/analytics/ledger.hpp>
#include <lob/analytics/markout.hpp>
#include <lob/config.hpp>
#include <lob/csv_writer.hpp>
#include <lob/execution.hpp>

namespace lob {

// Converts a microsecond epoch timestamp to a UTC "YYYY-MM-DD" date string.
// Implemented locally rather than via std::chrono's calendar types so the
// result is identical on every toolchain the project builds on.
std::string UtcDateString(Ts ts_us);
std::int64_t UtcDayNumber(Ts ts_us);

// One row of pnl_daily.csv, accumulated as the run proceeds.
struct DailyBucket {
  std::int64_t day = 0;
  std::string date;
  std::int64_t spread_capture_x2 = 0;
  std::int64_t inventory_pnl_x2 = 0;
  std::int64_t fees_x2 = 0;
  std::int64_t equity_start_x2 = 0;
  std::int64_t equity_end_x2 = 0;
  std::int64_t notional_traded_1e8 = 0;
  std::uint64_t fills = 0;
  std::uint64_t quotes_placed = 0;
  std::uint64_t dirty_events = 0;
  bool has_equity_start = false;
};

// Metadata stamped onto every row so a number in a table can always be traced
// back to the configuration that produced it.
struct RunTags {
  std::string run_id;
  std::string strategy;
  std::string symbol;
  std::string queue_assumption;
  double maker_fee_bp = 0.0;
  std::int64_t latency_in_ms = 0;
  std::int64_t latency_out_ms = 0;
  std::uint64_t seed = 0;
};

class RunRecorders {
 public:
  RunRecorders(const std::string& output_dir, RunTags tags, const Instrument& instrument);

  // --- fills.csv -----------------------------------------------------------
  void RecordFill(std::uint64_t fill_id, const Fill& fill, const QueueState& queue_state,
                  FillCause cause, double imbalance_at_fill, double sigma_at_fill);

  // --- markouts.csv --------------------------------------------------------
  // Written once at the end, when every resolvable sample has been resolved.
  void WriteMarkouts(const MarkoutSampler& sampler);

  // --- probes.csv (§3.8) ---------------------------------------------------
  void RecordProbe(std::uint64_t probe_id, Ts placed_ts_us, Side side, Ticks depth_ticks,
                   double initial_queue_fraction, double imbalance, double sigma,
                   Ts horizon_us, bool filled, Ts fill_ts_us);

  // --- pnl_daily.csv -------------------------------------------------------
  // Call after every ledger update so the daily buckets track the equity path.
  void ObserveLedger(Ts ts_us, const Ledger& ledger, bool dirty_event);
  void NoteQuotePlaced(Ts ts_us);
  void WriteDailyPnl(const std::vector<MarkoutSummary>& markout_summary);

  // --- manifest ------------------------------------------------------------
  void WriteManifest(const std::string& text);
  void WriteQueueStats(const QueueTrackerStats& stats);

  void CloseAll();

  [[nodiscard]] const std::string& output_dir() const { return output_dir_; }
  [[nodiscard]] const std::map<std::int64_t, DailyBucket>& daily() const { return daily_; }

 private:
  DailyBucket& BucketFor(Ts ts_us);

  std::string output_dir_;
  RunTags tags_;
  Instrument instrument_;

  CsvWriter fills_;
  CsvWriter markouts_;
  CsvWriter probes_;
  std::map<std::int64_t, DailyBucket> daily_;

  // Running totals at the end of the previous observation, so each day's
  // contribution is a difference rather than a level.
  std::int64_t last_spread_x2_ = 0;
  std::int64_t last_inventory_x2_ = 0;
  std::int64_t last_fees_x2_ = 0;
  std::int64_t last_notional_1e8_ = 0;
  std::uint64_t last_fills_ = 0;
};

}  // namespace lob
