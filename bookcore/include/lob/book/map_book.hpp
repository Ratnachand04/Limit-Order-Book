// MapBook -- the correctness reference implementation (master plan §4.4).
//
// One std::map per side, bids ordered descending so begin() is always the best.
// Node-based and allocating, therefore slow; that is fine and deliberate. Its
// job is to be obviously right, so that DenseBook can be checked against it.
#pragma once

#include <functional>
#include <map>

#include <lob/book/book_view.hpp>

namespace lob {

class MapBook final : public BookView {
 public:
  using BidMap = std::map<Ticks, Lots64, std::greater<Ticks>>;
  using AskMap = std::map<Ticks, Lots64, std::less<Ticks>>;

  MapBook() = default;

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

  [[nodiscard]] const BidMap& bids() const { return bids_; }
  [[nodiscard]] const AskMap& asks() const { return asks_; }
  [[nodiscard]] bool in_snapshot() const { return in_snapshot_; }

 private:
  BidMap bids_;
  AskMap asks_;
  // Levels accumulated during a snapshot, swapped in atomically at
  // ApplySnapshotEnd so a partial snapshot can never be observed.
  BidMap pending_bids_;
  AskMap pending_asks_;
  bool in_snapshot_ = false;
};

}  // namespace lob
