#include <lob/analytics/recorders.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace lob {
namespace {

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  const char last = dir.back();
  if (last == '/' || last == '\\') {
    return dir + name;
  }
  return dir + "/" + name;
}

std::string_view CauseName(FillCause c) {
  switch (c) {
    case FillCause::kQueueConsumed:
      return "queue_consumed";
    case FillCause::kTradedThrough:
      return "traded_through";
    case FillCause::kCrossed:
      return "crossed";
  }
  return "?";
}

}  // namespace

// ---------------------------------------------------------------------------
// Civil date arithmetic (Howard Hinnant's days-from-civil, inverted).
// Reimplemented here so the date strings in every CSV are byte-identical on
// every toolchain, which the determinism test requires.
// ---------------------------------------------------------------------------
std::int64_t UtcDayNumber(Ts ts_us) {
  const std::int64_t seconds = ts_us / kUsPerSecond;
  // Floor division: timestamps before the epoch must round down, not toward 0.
  std::int64_t days = seconds / 86400;
  if (seconds % 86400 < 0) {
    --days;
  }
  return days;
}

std::string UtcDateString(Ts ts_us) {
  std::int64_t z = UtcDayNumber(ts_us);
  z += 719468;  // shift the epoch to 0000-03-01
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;                                   // [0, 146096]
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            // [0, 365]
  const std::int64_t mp = (5 * doy + 2) / 153;                                 // [0, 11]
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;                         // [1, 31]
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;                            // [1, 12]
  const std::int64_t year = y + (m <= 2 ? 1 : 0);

  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld", static_cast<long long>(year),
                static_cast<long long>(m), static_cast<long long>(d));
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// RunRecorders
// ---------------------------------------------------------------------------
RunRecorders::RunRecorders(const std::string& output_dir, RunTags tags,
                           const Instrument& instrument)
    : output_dir_(output_dir), tags_(std::move(tags)), instrument_(instrument) {
  std::error_code ec;
  std::filesystem::create_directories(output_dir_, ec);

  fills_.Open(JoinPath(output_dir_, "fills.csv"),
              {"run_id", "strategy", "symbol", "queue_assumption", "maker_fee_bp", "latency_ms",
               "seed", "fill_id", "ts_us", "date", "side", "price_ticks", "price", "qty_lots",
               "qty", "mid_x2_ticks_at_fill", "mid_at_fill", "ahead_at_placement",
               "queue_fraction_at_placement", "ahead_at_fill", "behind_at_fill", "cause",
               "imbalance_at_fill", "sigma_at_fill", "is_probe"});

  markouts_.Open(JoinPath(output_dir_, "markouts.csv"),
                 {"run_id", "strategy", "symbol", "queue_assumption", "maker_fee_bp", "fill_id",
                  "horizon_us", "horizon_s", "resolved", "edge_bp", "adverse_selection_bp",
                  "markout_bp"});

  probes_.Open(JoinPath(output_dir_, "probes.csv"),
               {"run_id", "strategy", "symbol", "queue_assumption", "probe_id", "placed_ts_us",
                "date", "side", "depth_ticks", "initial_queue_fraction", "imbalance", "sigma",
                "horizon_us", "horizon_s", "filled", "time_to_fill_us"});
}

DailyBucket& RunRecorders::BucketFor(Ts ts_us) {
  const std::int64_t day = UtcDayNumber(ts_us);
  auto it = daily_.find(day);
  if (it == daily_.end()) {
    DailyBucket b;
    b.day = day;
    b.date = UtcDateString(ts_us);
    it = daily_.emplace(day, std::move(b)).first;
  }
  return it->second;
}

void RunRecorders::RecordFill(std::uint64_t fill_id, const Fill& fill,
                              const QueueState& queue_state, FillCause cause,
                              double imbalance_at_fill, double sigma_at_fill) {
  const double price = instrument_.ToPrice(fill.price_ticks);
  const double qty = instrument_.ToQty(fill.qty_lots);
  const double mid = static_cast<double>(fill.mid_x2_ticks_at_fill) * 0.5 *
                     instrument_.tick_size();
  const Lots64 total_at_placement = fill.ahead_at_placement + fill.qty_lots;
  const double q_frac =
      total_at_placement > 0
          ? static_cast<double>(fill.ahead_at_placement) / static_cast<double>(total_at_placement)
          : 0.0;

  fills_.Row(tags_.run_id, tags_.strategy, tags_.symbol, tags_.queue_assumption,
             tags_.maker_fee_bp, static_cast<std::int64_t>(tags_.latency_in_ms), tags_.seed,
             fill_id, fill.ts_us, UtcDateString(fill.ts_us),
             std::string(SideName(fill.side)), static_cast<std::int64_t>(fill.price_ticks), price,
             fill.qty_lots, qty, fill.mid_x2_ticks_at_fill, mid, fill.ahead_at_placement, q_frac,
             queue_state.ahead, queue_state.behind, std::string(CauseName(cause)),
             imbalance_at_fill, sigma_at_fill, fill.is_probe);
}

void RunRecorders::WriteMarkouts(const MarkoutSampler& sampler) {
  for (const MarkoutSample& s : sampler.samples()) {
    markouts_.Row(tags_.run_id, tags_.strategy, tags_.symbol, tags_.queue_assumption,
                  tags_.maker_fee_bp, s.fill_id, s.horizon_us,
                  static_cast<double>(s.horizon_us) / static_cast<double>(kUsPerSecond),
                  s.resolved, s.edge_bp, s.adverse_selection_bp, s.markout_bp);
  }
}

void RunRecorders::RecordProbe(std::uint64_t probe_id, Ts placed_ts_us, Side side,
                               Ticks depth_ticks, double initial_queue_fraction, double imbalance,
                               double sigma, Ts horizon_us, bool filled, Ts fill_ts_us) {
  const Ts time_to_fill = filled ? (fill_ts_us - placed_ts_us) : -1;
  probes_.Row(tags_.run_id, tags_.strategy, tags_.symbol, tags_.queue_assumption, probe_id,
              placed_ts_us, UtcDateString(placed_ts_us), std::string(SideName(side)),
              static_cast<std::int64_t>(depth_ticks), initial_queue_fraction, imbalance, sigma,
              horizon_us, static_cast<double>(horizon_us) / static_cast<double>(kUsPerSecond),
              filled, time_to_fill);
}

void RunRecorders::ObserveLedger(Ts ts_us, const Ledger& ledger, bool dirty_event) {
  DailyBucket& b = BucketFor(ts_us);
  if (!b.has_equity_start) {
    b.equity_start_x2 = ledger.EquityX2();
    b.has_equity_start = true;
  }
  b.spread_capture_x2 += ledger.spread_capture_x2() - last_spread_x2_;
  b.inventory_pnl_x2 += ledger.inventory_pnl_x2() - last_inventory_x2_;
  b.fees_x2 += ledger.fees_x2() - last_fees_x2_;
  b.notional_traded_1e8 += ledger.notional_traded_1e8() - last_notional_1e8_;
  b.fills += ledger.fill_count() - last_fills_;
  b.equity_end_x2 = ledger.EquityX2();
  if (dirty_event) {
    ++b.dirty_events;
  }

  last_spread_x2_ = ledger.spread_capture_x2();
  last_inventory_x2_ = ledger.inventory_pnl_x2();
  last_fees_x2_ = ledger.fees_x2();
  last_notional_1e8_ = ledger.notional_traded_1e8();
  last_fills_ = ledger.fill_count();
}

void RunRecorders::NoteQuotePlaced(Ts ts_us) { ++BucketFor(ts_us).quotes_placed; }

void RunRecorders::WriteDailyPnl(const std::vector<MarkoutSummary>& markout_summary) {
  // Adverse selection at 10 s is the headline number of §3.9; find it if the
  // horizon grid contains it, otherwise report the closest available.
  double adverse_10s_bp = 0.0;
  bool have_adverse = false;
  Ts best_delta = 0;
  for (const MarkoutSummary& m : markout_summary) {
    if (m.resolved == 0) {
      continue;
    }
    const Ts target = 10 * kUsPerSecond;
    const Ts delta = m.horizon_us > target ? m.horizon_us - target : target - m.horizon_us;
    if (!have_adverse || delta < best_delta) {
      have_adverse = true;
      best_delta = delta;
      adverse_10s_bp = m.mean_adverse_selection_bp;
    }
  }

  CsvWriter out(JoinPath(output_dir_, "pnl_daily.csv"),
                {"run_id", "strategy", "symbol", "queue_assumption", "maker_fee_bp",
                 "latency_in_ms", "latency_out_ms", "seed", "date", "spread_capture",
                 "inventory_pnl", "fees", "total", "equity_change", "identity_residual_x2",
                 "adverse_selection_10s_bp", "fills", "quotes_placed", "notional_traded",
                 "dirty_events"});

  for (const auto& [day, b] : daily_) {
    const std::int64_t total_x2 = b.spread_capture_x2 + b.inventory_pnl_x2 - b.fees_x2;
    const std::int64_t equity_change_x2 = b.equity_end_x2 - b.equity_start_x2;
    // The per-day identity assertion of §3.9.  Exact, because every term is an
    // integer -- see analytics/ledger.hpp.
    const std::int64_t residual = equity_change_x2 - total_x2;

    out.Row(tags_.run_id, tags_.strategy, tags_.symbol, tags_.queue_assumption,
            tags_.maker_fee_bp, tags_.latency_in_ms, tags_.latency_out_ms, tags_.seed, b.date,
            Ledger::X2ToCurrency(b.spread_capture_x2), Ledger::X2ToCurrency(b.inventory_pnl_x2),
            Ledger::X2ToCurrency(-b.fees_x2), Ledger::X2ToCurrency(total_x2),
            Ledger::X2ToCurrency(equity_change_x2), residual,
            have_adverse ? adverse_10s_bp : 0.0, b.fills, b.quotes_placed,
            Instrument::CashToDouble(b.notional_traded_1e8), b.dirty_events);
  }
  out.Close();
}

void RunRecorders::WriteManifest(const std::string& text) {
  std::ofstream out(JoinPath(output_dir_, "manifest.yaml"), std::ios::binary | std::ios::trunc);
  out << text;
}

void RunRecorders::WriteQueueStats(const QueueTrackerStats& s) {
  CsvWriter out(JoinPath(output_dir_, "queue_stats.csv"),
                {"run_id", "strategy", "symbol", "queue_assumption", "placements",
                 "level_increases", "level_decreases", "trades_seen", "lots_explained_by_trades",
                 "lots_attributed_to_cancels", "lots_trade_credit_expired",
                 "trade_attribution_residual_fraction", "fills_queue_consumed",
                 "fills_traded_through", "fills_crossed"});
  const Lots64 attributed = s.lots_explained_by_trades + s.lots_trade_credit_expired;
  const double residual_fraction =
      attributed > 0 ? static_cast<double>(s.lots_trade_credit_expired) /
                           static_cast<double>(attributed)
                     : 0.0;
  out.Row(tags_.run_id, tags_.strategy, tags_.symbol, tags_.queue_assumption, s.placements,
          s.level_increases, s.level_decreases, s.trades_seen, s.lots_explained_by_trades,
          s.lots_attributed_to_cancels, s.lots_trade_credit_expired, residual_fraction,
          s.fills_queue_consumed, s.fills_traded_through, s.fills_crossed);
  out.Close();
}

void RunRecorders::CloseAll() {
  fills_.Close();
  markouts_.Close();
  probes_.Close();
}

}  // namespace lob
