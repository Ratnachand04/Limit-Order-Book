#include <lob/book/dual_book.hpp>

namespace lob {

void DualBook::Report(const std::string& what) const {
  ++mismatches_;
  if (first_mismatch_.empty()) {
    first_mismatch_ = what;
  }
  if (on_mismatch_) {
    on_mismatch_(what);
  }
}

void DualBook::ApplyDepth(Side side, Ticks price_ticks, Lots64 new_qty) {
  reference_.ApplyDepth(side, price_ticks, new_qty);
  fast_.ApplyDepth(side, price_ticks, new_qty);
  if (check_every_update_) {
    const BookIntegrity cmp = Compare();
    if (!cmp) {
      Report(cmp.message + "  [after ApplyDepth " + std::string(SideName(side)) + " px=" +
             std::to_string(price_ticks) + " qty=" + std::to_string(new_qty) + "]");
    }
  }
}

void DualBook::ApplySnapshotBegin() {
  reference_.ApplySnapshotBegin();
  fast_.ApplySnapshotBegin();
}

void DualBook::ApplySnapshotLevel(Side side, Ticks price_ticks, Lots64 qty) {
  reference_.ApplySnapshotLevel(side, price_ticks, qty);
  fast_.ApplySnapshotLevel(side, price_ticks, qty);
}

void DualBook::ApplySnapshotEnd() {
  reference_.ApplySnapshotEnd();
  fast_.ApplySnapshotEnd();
  const BookIntegrity cmp = Compare();
  if (!cmp) {
    Report(cmp.message + "  [after ApplySnapshotEnd]");
  }
}

void DualBook::Clear() {
  reference_.Clear();
  fast_.Clear();
  mismatches_ = 0;
  first_mismatch_.clear();
}

BookIntegrity DualBook::CheckInvariants() const {
  const BookIntegrity a = reference_.CheckInvariants();
  if (!a) {
    return a;
  }
  const BookIntegrity b = fast_.CheckInvariants();
  if (!b) {
    return b;
  }
  return Compare();
}

BookIntegrity DualBook::Compare() const {
  if (reference_.BestBid() != fast_.BestBid()) {
    return BookIntegrity::Fail("DualBook: best_bid differs -- map " +
                               std::to_string(reference_.BestBid()) + " vs dense " +
                               std::to_string(fast_.BestBid()));
  }
  if (reference_.BestAsk() != fast_.BestAsk()) {
    return BookIntegrity::Fail("DualBook: best_ask differs -- map " +
                               std::to_string(reference_.BestAsk()) + " vs dense " +
                               std::to_string(fast_.BestAsk()));
  }
  if (reference_.LevelCount(Side::kBid) != fast_.LevelCount(Side::kBid)) {
    return BookIntegrity::Fail("DualBook: bid level count differs -- map " +
                               std::to_string(reference_.LevelCount(Side::kBid)) + " vs dense " +
                               std::to_string(fast_.LevelCount(Side::kBid)));
  }
  if (reference_.LevelCount(Side::kAsk) != fast_.LevelCount(Side::kAsk)) {
    return BookIntegrity::Fail("DualBook: ask level count differs -- map " +
                               std::to_string(reference_.LevelCount(Side::kAsk)) + " vs dense " +
                               std::to_string(fast_.LevelCount(Side::kAsk)));
  }
  for (const auto& [px, qty] : reference_.bids()) {
    if (fast_.QtyAt(Side::kBid, px) != qty) {
      return BookIntegrity::Fail("DualBook: bid quantity differs at tick " + std::to_string(px) +
                                 " -- map " + std::to_string(qty) + " vs dense " +
                                 std::to_string(fast_.QtyAt(Side::kBid, px)));
    }
  }
  for (const auto& [px, qty] : reference_.asks()) {
    if (fast_.QtyAt(Side::kAsk, px) != qty) {
      return BookIntegrity::Fail("DualBook: ask quantity differs at tick " + std::to_string(px) +
                                 " -- map " + std::to_string(qty) + " vs dense " +
                                 std::to_string(fast_.QtyAt(Side::kAsk, px)));
    }
  }
  return BookIntegrity{};
}

}  // namespace lob
