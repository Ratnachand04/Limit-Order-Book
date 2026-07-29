// The order book interface (master plan §4.4).
//
// Two implementations exist and must agree exactly:
//   * MapBook   -- std::map per side.  The correctness reference.
//   * DenseBook -- dense array over a +/-32k-tick window with a map fallback
//                  outside it and sliding best-price cursors.  The hot path.
//   * DualBook  -- runs both and asserts they agree (the dual-run mode).
//
// The virtual calls here are made at strategy-callback cadence (100 ms timers,
// trades, fills), not per depth event, so the indirection is irrelevant to the
// throughput target.  The benchmarks in bench/ call the concrete classes
// directly, which is what the "apply_depth p50 < 100 ns" figure refers to.
#pragma once

#include <cstdint>
#include <string>

#include <lob/types.hpp>

namespace lob {

// Result of an integrity check.  The book never throws on bad market data --
// it reports, and the caller decides whether to resync or abort (§4.4).
struct BookIntegrity {
  bool ok = true;
  std::string message;

  explicit operator bool() const { return ok; }
  static BookIntegrity Fail(std::string msg) { return BookIntegrity{false, std::move(msg)}; }
};

class BookView {
 public:
  virtual ~BookView() = default;

  // --- mutation ------------------------------------------------------------
  // `new_qty` is the ABSOLUTE quantity now resting at the level; 0 deletes it.
  // This matches the diff-depth stream and the binary schema exactly, so the
  // replayer never has to compute a delta it could get wrong.
  virtual void ApplyDepth(Side side, Ticks price_ticks, Lots64 new_qty) = 0;

  // A snapshot REBUILDS the side from scratch rather than patching it.
  // Patching is Part 11 pitfall #4: a book patched across a gap stays subtly
  // wrong forever, and the corruption shows up as crossed levels much later.
  virtual void ApplySnapshotBegin() = 0;
  virtual void ApplySnapshotLevel(Side side, Ticks price_ticks, Lots64 qty) = 0;
  virtual void ApplySnapshotEnd() = 0;

  virtual void Clear() = 0;

  // Convenience: dispatches one binary Event to the calls above.
  void Apply(const Event& e);

  // --- queries -------------------------------------------------------------
  [[nodiscard]] virtual Ticks BestBid() const = 0;
  [[nodiscard]] virtual Ticks BestAsk() const = 0;
  [[nodiscard]] virtual Lots64 QtyAt(Side side, Ticks price_ticks) const = 0;
  [[nodiscard]] virtual std::size_t LevelCount(Side side) const = 0;

  // Total quantity resting within `n_ticks` of the best price on that side,
  // inclusive of the best.  n_ticks == 0 means the best level alone.
  [[nodiscard]] virtual Lots64 DepthWithin(Side side, Ticks n_ticks) const = 0;

  [[nodiscard]] virtual BookIntegrity CheckInvariants() const = 0;

  // --- derived quantities --------------------------------------------------
  [[nodiscard]] bool HasBothSides() const { return HasBid(BestBid()) && HasAsk(BestAsk()); }

  // Mid in HALF ticks (best_bid + best_ask), kept integral so it can be
  // compared and stored without floating point.  Undefined unless
  // HasBothSides(); callers must check.
  [[nodiscard]] std::int64_t MidX2Ticks() const {
    return static_cast<std::int64_t>(BestBid()) + static_cast<std::int64_t>(BestAsk());
  }

  [[nodiscard]] double MidTicks() const { return static_cast<double>(MidX2Ticks()) * 0.5; }

  [[nodiscard]] Ticks SpreadTicks() const { return BestAsk() - BestBid(); }

  [[nodiscard]] Lots64 BestQty(Side side) const {
    const Ticks px = side == Side::kBid ? BestBid() : BestAsk();
    if (side == Side::kBid ? !HasBid(px) : !HasAsk(px)) {
      return 0;
    }
    return QtyAt(side, px);
  }

  // §3.6 order-book imbalance I = Q_b / (Q_b + Q_a) over the top level.
  // Returns 0.5 when the book has no quantity on either side, which is the
  // neutral value and keeps the weighted mid equal to the mid.
  [[nodiscard]] double TopImbalance() const {
    const Lots64 qb = BestQty(Side::kBid);
    const Lots64 qa = BestQty(Side::kAsk);
    const Lots64 total = qb + qa;
    if (total <= 0) {
      return 0.5;
    }
    return static_cast<double>(qb) / static_cast<double>(total);
  }

  // §3.6 weighted mid m_w = I * P_ask + (1 - I) * P_bid, in ticks.
  // Heavier bid queue pushes fair value toward the ask -- the thin side is the
  // one likelier to be consumed next.
  [[nodiscard]] double WeightedMidTicks() const {
    if (!HasBothSides()) {
      return MidTicks();
    }
    const double i = TopImbalance();
    return i * static_cast<double>(BestAsk()) + (1.0 - i) * static_cast<double>(BestBid());
  }
};

}  // namespace lob
