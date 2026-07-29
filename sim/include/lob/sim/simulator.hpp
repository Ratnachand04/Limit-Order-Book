// The replay engine (master plan §4.5, §4.2).
//
// One pass over a binary event file drives, in this order:
//
//   1. the virtual clock            (never wall clock, never backwards)
//   2. the book core                (map / dense / dual)
//   3. the queue tracker            (§3.7, our shadow order's A and B)
//   4. the ledger and markouts      (§3.9)
//   5. the strategy callbacks       (§4.7)
//
// Determinism contract: given the same event file, the same RunConfig and the
// same seed, this class produces byte-identical CSVs on every platform.  The
// determinism test in tests/ enforces it and CI runs it on every push.
//
// The two fill models exist to answer RQ1 -- how wrong a naive backtest is:
//
//   kQueueAware  the honest model: an order fills only when trades actually
//                consume the queue in front of it.
//   kTouchRule   the fiction almost every retail backtest uses: any trade
//                printed at your price fills you in full, instantly.
//
// Running the same strategy on the same data under both is the controlled
// comparison the paper is built on.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lob/analytics/ledger.hpp>
#include <lob/analytics/markout.hpp>
#include <lob/analytics/recorders.hpp>
#include <lob/book/book_view.hpp>
#include <lob/config.hpp>
#include <lob/rng.hpp>
#include <lob/sim/clock.hpp>
#include <lob/sim/event_queue.hpp>
#include <lob/sim/gateway.hpp>
#include <lob/sim/latency.hpp>
#include <lob/sim/probe.hpp>
#include <lob/sim/queue_tracker.hpp>
#include <lob/sim/strategy.hpp>
#include <lob/types.hpp>

namespace lob {

enum class FillModel : std::uint8_t {
  kQueueAware = 0,
  kTouchRule = 1,
};

FillModel ParseFillModel(std::string_view s);
std::string_view FillModelName(FillModel m);

enum class BookKind : std::uint8_t { kDense = 0, kMap = 1, kDual = 2 };

struct SimulatorStats {
  std::uint64_t market_events = 0;
  std::uint64_t dirty_events = 0;
  std::uint64_t depth_events = 0;
  std::uint64_t trade_events = 0;
  std::uint64_t snapshot_events = 0;
  std::uint64_t timer_ticks = 0;
  std::uint64_t actions = 0;
  std::uint64_t fills = 0;
  std::uint64_t probe_fills = 0;
  std::uint64_t book_integrity_failures = 0;
  std::uint64_t dual_book_mismatches = 0;
  std::uint64_t forced_liquidations = 0;
  Ts first_local_ts_us = 0;
  Ts last_local_ts_us = 0;

  [[nodiscard]] std::string Report() const;
};

class Simulator {
 public:
  Simulator(RunConfig config, Strategy& strategy);
  ~Simulator();

  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  // Optional: attach CSV recorders.  Without them the run is still complete,
  // it just leaves no artefacts -- which is what the benchmarks want.
  void set_recorders(RunRecorders* recorders) { recorders_ = recorders; }
  void set_fill_model(FillModel model) { fill_model_ = model; }
  void set_book_kind(BookKind kind);
  // How often to run the O(levels) integrity check (0 disables it).
  void set_integrity_check_every(std::uint64_t n) { integrity_every_ = n; }

  // Replays a whole event array.  Returns the stats.
  const SimulatorStats& Run(const std::vector<Event>& events);
  const SimulatorStats& RunFile(const std::string& binary_path);

  // --- accessors, for tests and the runner apps ---------------------------
  [[nodiscard]] const BookView& book() const { return *book_; }
  [[nodiscard]] const Ledger& ledger() const { return ledger_; }
  [[nodiscard]] const MarkoutSampler& markouts() const { return markouts_; }
  [[nodiscard]] const QueueTracker& queue_tracker() const { return queue_; }
  [[nodiscard]] const OrderGateway& gateway() const { return gateway_; }
  [[nodiscard]] const ProbeEngine& probes() const { return probes_; }
  [[nodiscard]] const SimulatorStats& stats() const { return stats_; }
  [[nodiscard]] const RunConfig& config() const { return config_; }
  [[nodiscard]] const Rng& rng() const { return rng_; }
  [[nodiscard]] const std::vector<Fill>& fills() const { return fills_; }

 private:
  void HandleMarketEvent(const Event& e);
  void HandleAction(const SimEvent& se);
  void HandleTimer();
  void OnQueueFill(const QueueFill& qf);
  void ApplyTouchRule(const Event& trade);
  void RemarkAndNotify();
  void EnforceInventoryCap();
  [[nodiscard]] Lots64 VisibleQtyAt(Side side, Ticks price_ticks) const;
  void BookFill(OrderId id, Side side, Ticks price_ticks, Lots64 qty, FillCause cause);

  RunConfig config_;
  Strategy* strategy_;

  Rng rng_;
  Clock clock_;
  EventQueue events_;
  LatencyModel latency_;
  std::unique_ptr<BookView> book_;
  BookKind book_kind_ = BookKind::kDense;
  QueueTracker queue_;
  OrderGateway gateway_;
  Ledger ledger_;
  MarkoutSampler markouts_;
  ProbeEngine probes_;
  RunRecorders* recorders_ = nullptr;
  FillModel fill_model_ = FillModel::kQueueAware;

  const std::vector<Event>* market_ = nullptr;
  std::uint64_t next_market_index_ = 0;
  Ts last_visible_ts_us_ = 0;
  bool current_event_dirty_ = false;
  std::int64_t last_marked_mid_x2_ = 0;
  bool have_mid_ = false;
  bool have_first_ts_ = false;
  // Probes are spawned once per interval bucket; -1 means "none yet".
  std::int64_t last_probe_bucket_ = -1;

  std::uint64_t integrity_every_ = 0;
  std::uint64_t since_integrity_check_ = 0;
  std::uint64_t fill_counter_ = 0;
  std::vector<Fill> fills_;

  SimulatorStats stats_;
};

}  // namespace lob
