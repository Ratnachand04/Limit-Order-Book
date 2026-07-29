// DenseBook -- the hot path (master plan §4.4, §4.10).
//
// Layout per side:
//   * a dense std::vector<Lots64> covering [anchor - 32768, anchor + 32768]
//     ticks, indexed by (price_ticks - anchor + kHalfWindow);
//   * a 1-bit-per-tick occupancy bitmap over the same range, so "find the next
//     non-empty level below this one" is a word scan with countl_zero rather
//     than a walk over 65k int64s;
//   * a std::map fallback for levels outside the window (deep, rarely touched);
//   * cached best-price cursors that slide on update.
//
// Why this shape (Part 11 pitfalls #2/#3 and §4.10): node-based maps allocate
// and chase pointers on every touch of the book, which is the dominant cost in
// replay once JSON parsing has been moved offline.  A dense array turns
// apply_depth into one bounds check, one store and a cursor update.
//
// The window is re-anchored when the mid drifts past half of it.  At a $0.01
// tick, +/-32768 ticks is only +/-$327 around the anchor, so on BTC this fires
// several times a day -- it is a normal operation, not an error path.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include <lob/book/book_view.hpp>

namespace lob {

class DenseBook final : public BookView {
 public:
  static constexpr Ticks kHalfWindow = 32768;
  static constexpr std::size_t kWindowSize = 2U * static_cast<std::size_t>(kHalfWindow) + 1U;

  DenseBook();

  void ApplyDepth(Side side, Ticks price_ticks, Lots64 new_qty) override;
  void ApplySnapshotBegin() override;
  void ApplySnapshotLevel(Side side, Ticks price_ticks, Lots64 qty) override;
  void ApplySnapshotEnd() override;
  void Clear() override;

  [[nodiscard]] Ticks BestBid() const override;
  [[nodiscard]] Ticks BestAsk() const override;
  [[nodiscard]] Lots64 QtyAt(Side side, Ticks price_ticks) const override;
  [[nodiscard]] std::size_t LevelCount(Side side) const override;
  [[nodiscard]] Lots64 DepthWithin(Side side, Ticks n_ticks) const override;
  [[nodiscard]] BookIntegrity CheckInvariants() const override;

  // --- introspection, for tests and benchmarks -----------------------------
  [[nodiscard]] Ticks anchor() const { return anchor_; }
  [[nodiscard]] bool anchored() const { return anchored_; }
  [[nodiscard]] std::uint64_t reanchor_count() const { return reanchor_count_; }
  [[nodiscard]] std::size_t far_level_count(Side side) const {
    return side == Side::kBid ? far_bids_.size() : far_asks_.size();
  }

  // Forces the window to re-centre.  Exposed so tests can drive the path
  // deterministically instead of waiting for a price move.
  void Reanchor(Ticks new_anchor);

 private:
  struct SideData {
    std::vector<Lots64> qty;     // dense quantities over the window
    std::vector<std::uint64_t> bits;  // occupancy, 1 bit per tick
    std::size_t window_levels = 0;
  };

  [[nodiscard]] std::ptrdiff_t IndexOf(Ticks px) const {
    return static_cast<std::ptrdiff_t>(px) - static_cast<std::ptrdiff_t>(anchor_) +
           static_cast<std::ptrdiff_t>(kHalfWindow);
  }
  [[nodiscard]] static bool InWindowIndex(std::ptrdiff_t i) {
    return i >= 0 && i < static_cast<std::ptrdiff_t>(kWindowSize);
  }
  [[nodiscard]] Ticks PriceOf(std::ptrdiff_t index) const {
    return static_cast<Ticks>(index + static_cast<std::ptrdiff_t>(anchor_) -
                              static_cast<std::ptrdiff_t>(kHalfWindow));
  }

  void SetLevel(Side side, Ticks px, Lots64 qty);
  void SetWindowLevel(Side side, std::ptrdiff_t index, Ticks px, Lots64 qty);
  void SetFarLevel(Side side, Ticks px, Lots64 qty);
  void RefreshWindowBestBid(std::ptrdiff_t search_from);
  void RefreshWindowBestAsk(std::ptrdiff_t search_from);
  void MaybeReanchor();
  void RebuildFrom(const std::map<Ticks, Lots64, std::greater<Ticks>>& bids,
                   const std::map<Ticks, Lots64, std::less<Ticks>>& asks, Ticks new_anchor);
  [[nodiscard]] std::map<Ticks, Lots64, std::greater<Ticks>> CollectBids() const;
  [[nodiscard]] std::map<Ticks, Lots64, std::less<Ticks>> CollectAsks() const;

  SideData bid_;
  SideData ask_;
  std::map<Ticks, Lots64, std::greater<Ticks>> far_bids_;
  std::map<Ticks, Lots64, std::less<Ticks>> far_asks_;

  Ticks anchor_ = 0;
  bool anchored_ = false;
  Ticks win_best_bid_ = kNoBid;
  Ticks win_best_ask_ = kNoAsk;
  std::uint64_t reanchor_count_ = 0;

  bool in_snapshot_ = false;
  std::map<Ticks, Lots64, std::greater<Ticks>> pending_bids_;
  std::map<Ticks, Lots64, std::less<Ticks>> pending_asks_;
};

}  // namespace lob
