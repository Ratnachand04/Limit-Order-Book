// The Strategy interface (master plan §4.7).
//
//   struct Strategy {
//     virtual void on_book(const BookView&, const Clock&)  = 0;
//     virtual void on_trade(const Trade&, const Clock&)    = 0;
//     virtual void on_fill(const Fill&, const Clock&)      = 0;
//     virtual void on_timer(const Clock&)                  = 0;
//     // emits place / cancel / replace via an OrderGateway (which applies delta_out)
//   };
//
// Everything a strategy is allowed to know is reachable from StrategyContext.
// In particular there is no way to reach the raw event stream or an exchange
// timestamp from here: a strategy sees the book as of LOCAL time only, which is
// what stops Part 11 pitfall #1 ("lookahead via timestamps") structurally
// rather than by discipline.
#pragma once

#include <cstdint>
#include <string>

#include <lob/analytics/ledger.hpp>
#include <lob/book/book_view.hpp>
#include <lob/config.hpp>
#include <lob/execution.hpp>
#include <lob/rng.hpp>
#include <lob/sim/clock.hpp>
#include <lob/sim/gateway.hpp>

namespace lob {

// A trade as the strategy sees it.
struct TradeInfo {
  Ts exch_ts_us = 0;
  Side aggressor = Side::kBid;  // kAsk means a SELL aggressor (it hit the bid)
  Ticks price_ticks = 0;
  Lots64 qty_lots = 0;
};

struct StrategyContext {
  OrderGateway* gateway = nullptr;
  const BookView* book = nullptr;
  const Clock* clock = nullptr;
  const Instrument* instrument = nullptr;
  const StrategyConfig* config = nullptr;
  const FeeConfig* fees = nullptr;
  const Ledger* ledger = nullptr;  // read-only: inventory and equity
  Rng* rng = nullptr;
};

// Counters every strategy maintains, written to the run summary.  The
// min-edge counter is not bookkeeping for its own sake: "log how often this
// binds; at 10 bp fees it will bind almost always, which is finding RQ4
// announcing itself" (Part 6).
struct StrategyDiagnostics {
  std::uint64_t quotes_placed = 0;
  std::uint64_t quotes_cancelled = 0;
  std::uint64_t requotes_suppressed_by_min_move = 0;
  std::uint64_t quotes_blocked_by_min_edge = 0;
  std::uint64_t quotes_blocked_by_inventory_cap = 0;
  std::uint64_t toxicity_pulls = 0;
  std::uint64_t timer_ticks = 0;
};

class Strategy {
 public:
  virtual ~Strategy() = default;

  // Called once before the first event.  The context outlives the strategy.
  virtual void OnStart(const StrategyContext& ctx) { ctx_ = ctx; }

  virtual void OnBook(const BookView& book, const Clock& clock) = 0;
  virtual void OnTrade(const TradeInfo& trade, const BookView& book, const Clock& clock) = 0;
  virtual void OnFill(const Fill& fill, const Clock& clock) = 0;
  virtual void OnTimer(const BookView& book, const Clock& clock) = 0;

  virtual void OnEnd() {}

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] const StrategyDiagnostics& diagnostics() const { return diag_; }

  // Quoting values the strategy last computed, surfaced for the run summary
  // and for the S1-vs-S2 inventory-path figure (F6).
  [[nodiscard]] virtual double last_reservation_price_ticks() const { return 0.0; }
  [[nodiscard]] virtual double last_half_spread_ticks() const { return 0.0; }
  [[nodiscard]] virtual double last_sigma_ticks_per_sqrt_s() const { return 0.0; }

 protected:
  StrategyContext ctx_;
  StrategyDiagnostics diag_;
};

}  // namespace lob
