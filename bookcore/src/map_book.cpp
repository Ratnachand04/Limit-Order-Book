#include <lob/book/map_book.hpp>

#include <string>

namespace lob {

void BookView::Apply(const Event& e) {
  switch (e.Type()) {
    case EventType::kDepth:
      ApplyDepth(e.SideOf(), e.price_ticks, e.qty_lots);
      break;
    case EventType::kSnapshotBegin:
      ApplySnapshotBegin();
      break;
    case EventType::kSnapshotLevel:
      ApplySnapshotLevel(e.SideOf(), e.price_ticks, e.qty_lots);
      break;
    case EventType::kSnapshotEnd:
      ApplySnapshotEnd();
      break;
    case EventType::kTrade:
      // Trades do not move the book on their own: the exchange publishes the
      // resulting level change on the depth stream.  Applying both would
      // double-count the consumed quantity.
      break;
  }
}

void MapBook::ApplyDepth(Side side, Ticks price_ticks, Lots64 new_qty) {
  if (side == Side::kBid) {
    if (new_qty <= 0) {
      bids_.erase(price_ticks);
    } else {
      bids_[price_ticks] = new_qty;
    }
  } else {
    if (new_qty <= 0) {
      asks_.erase(price_ticks);
    } else {
      asks_[price_ticks] = new_qty;
    }
  }
}

void MapBook::ApplySnapshotBegin() {
  in_snapshot_ = true;
  pending_bids_.clear();
  pending_asks_.clear();
}

void MapBook::ApplySnapshotLevel(Side side, Ticks price_ticks, Lots64 qty) {
  if (qty <= 0) {
    return;  // a zero level in a snapshot simply is not there
  }
  if (!in_snapshot_) {
    // Tolerate a level without a preceding Begin (a truncated file) by treating
    // it as a plain depth update rather than silently dropping liquidity.
    ApplyDepth(side, price_ticks, qty);
    return;
  }
  if (side == Side::kBid) {
    pending_bids_[price_ticks] = qty;
  } else {
    pending_asks_[price_ticks] = qty;
  }
}

void MapBook::ApplySnapshotEnd() {
  if (!in_snapshot_) {
    return;
  }
  // Rebuild, never patch (Part 11 pitfall #4).
  bids_.swap(pending_bids_);
  asks_.swap(pending_asks_);
  pending_bids_.clear();
  pending_asks_.clear();
  in_snapshot_ = false;
}

void MapBook::Clear() {
  bids_.clear();
  asks_.clear();
  pending_bids_.clear();
  pending_asks_.clear();
  in_snapshot_ = false;
}

Ticks MapBook::BestBid() const { return bids_.empty() ? kNoBid : bids_.begin()->first; }

Ticks MapBook::BestAsk() const { return asks_.empty() ? kNoAsk : asks_.begin()->first; }

Lots64 MapBook::QtyAt(Side side, Ticks price_ticks) const {
  if (side == Side::kBid) {
    const auto it = bids_.find(price_ticks);
    return it == bids_.end() ? 0 : it->second;
  }
  const auto it = asks_.find(price_ticks);
  return it == asks_.end() ? 0 : it->second;
}

std::size_t MapBook::LevelCount(Side side) const {
  return side == Side::kBid ? bids_.size() : asks_.size();
}

Lots64 MapBook::DepthWithin(Side side, Ticks n_ticks) const {
  if (n_ticks < 0) {
    return 0;
  }
  Lots64 total = 0;
  if (side == Side::kBid) {
    if (bids_.empty()) {
      return 0;
    }
    const Ticks floor_px = bids_.begin()->first - n_ticks;
    for (const auto& [px, qty] : bids_) {  // descending
      if (px < floor_px) {
        break;
      }
      total += qty;
    }
  } else {
    if (asks_.empty()) {
      return 0;
    }
    const Ticks ceil_px = asks_.begin()->first + n_ticks;
    for (const auto& [px, qty] : asks_) {  // ascending
      if (px > ceil_px) {
        break;
      }
      total += qty;
    }
  }
  return total;
}

BookIntegrity MapBook::CheckInvariants() const {
  for (const auto& [px, qty] : bids_) {
    if (qty <= 0) {
      return BookIntegrity::Fail("MapBook: non-positive quantity resting at bid tick " +
                                 std::to_string(px));
    }
  }
  for (const auto& [px, qty] : asks_) {
    if (qty <= 0) {
      return BookIntegrity::Fail("MapBook: non-positive quantity resting at ask tick " +
                                 std::to_string(px));
    }
  }
  if (!bids_.empty() && !asks_.empty()) {
    const Ticks bb = bids_.begin()->first;
    const Ticks ba = asks_.begin()->first;
    if (bb >= ba) {
      return BookIntegrity::Fail("MapBook: crossed book, best_bid " + std::to_string(bb) +
                                 " >= best_ask " + std::to_string(ba));
    }
  }
  return BookIntegrity{};
}

}  // namespace lob
