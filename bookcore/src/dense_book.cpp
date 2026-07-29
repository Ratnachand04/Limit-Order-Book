#include <lob/book/dense_book.hpp>

#include <bit>
#include <string>

namespace lob {
namespace {

constexpr std::size_t kWords = (DenseBook::kWindowSize + 63U) / 64U;

inline void SetBit(std::vector<std::uint64_t>& bits, std::size_t i) {
  bits[i >> 6U] |= (std::uint64_t{1} << (i & 63U));
}

inline void ClearBit(std::vector<std::uint64_t>& bits, std::size_t i) {
  bits[i >> 6U] &= ~(std::uint64_t{1} << (i & 63U));
}

// Highest set bit index <= `from`; -1 when there is none.
std::ptrdiff_t FindPrevSet(const std::vector<std::uint64_t>& bits, std::ptrdiff_t from) {
  if (from < 0) {
    return -1;
  }
  if (from >= static_cast<std::ptrdiff_t>(DenseBook::kWindowSize)) {
    from = static_cast<std::ptrdiff_t>(DenseBook::kWindowSize) - 1;
  }
  std::size_t w = static_cast<std::size_t>(from) >> 6U;
  const unsigned bit = static_cast<unsigned>(from) & 63U;
  std::uint64_t v = bits[w];
  if (bit != 63U) {
    v &= (std::uint64_t{1} << (bit + 1U)) - 1U;
  }
  while (true) {
    if (v != 0) {
      const unsigned top = 63U - static_cast<unsigned>(std::countl_zero(v));
      return static_cast<std::ptrdiff_t>((w << 6U) + top);
    }
    if (w == 0) {
      return -1;
    }
    --w;
    v = bits[w];
  }
}

// Lowest set bit index >= `from`; -1 when there is none.
std::ptrdiff_t FindNextSet(const std::vector<std::uint64_t>& bits, std::ptrdiff_t from) {
  if (from >= static_cast<std::ptrdiff_t>(DenseBook::kWindowSize)) {
    return -1;
  }
  if (from < 0) {
    from = 0;
  }
  std::size_t w = static_cast<std::size_t>(from) >> 6U;
  const unsigned bit = static_cast<unsigned>(from) & 63U;
  std::uint64_t v = bits[w] & (~std::uint64_t{0} << bit);
  while (true) {
    if (v != 0) {
      const unsigned low = static_cast<unsigned>(std::countr_zero(v));
      const std::ptrdiff_t idx = static_cast<std::ptrdiff_t>((w << 6U) + low);
      return idx < static_cast<std::ptrdiff_t>(DenseBook::kWindowSize) ? idx : -1;
    }
    ++w;
    if (w >= bits.size()) {
      return -1;
    }
    v = bits[w];
  }
}

}  // namespace

DenseBook::DenseBook() {
  bid_.qty.assign(kWindowSize, 0);
  bid_.bits.assign(kWords, 0);
  ask_.qty.assign(kWindowSize, 0);
  ask_.bits.assign(kWords, 0);
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------
void DenseBook::ApplyDepth(Side side, Ticks price_ticks, Lots64 new_qty) {
  if (!anchored_) {
    anchor_ = price_ticks;
    anchored_ = true;
  }
  SetLevel(side, price_ticks, new_qty);
  MaybeReanchor();
}

void DenseBook::SetLevel(Side side, Ticks px, Lots64 qty) {
  const std::ptrdiff_t i = IndexOf(px);
  if (InWindowIndex(i)) {
    SetWindowLevel(side, i, px, qty);
  } else {
    SetFarLevel(side, px, qty);
  }
}

void DenseBook::SetWindowLevel(Side side, std::ptrdiff_t index, Ticks px, Lots64 qty) {
  SideData& s = (side == Side::kBid) ? bid_ : ask_;
  const std::size_t ui = static_cast<std::size_t>(index);
  const Lots64 old = s.qty[ui];
  const Lots64 fresh = qty > 0 ? qty : 0;
  if (old == fresh) {
    return;
  }
  s.qty[ui] = fresh;

  if (old <= 0 && fresh > 0) {
    SetBit(s.bits, ui);
    ++s.window_levels;
    if (side == Side::kBid) {
      if (!HasBid(win_best_bid_) || px > win_best_bid_) {
        win_best_bid_ = px;
      }
    } else {
      if (!HasAsk(win_best_ask_) || px < win_best_ask_) {
        win_best_ask_ = px;
      }
    }
  } else if (old > 0 && fresh <= 0) {
    ClearBit(s.bits, ui);
    --s.window_levels;
    if (side == Side::kBid) {
      if (px == win_best_bid_) {
        RefreshWindowBestBid(index - 1);
      }
    } else {
      if (px == win_best_ask_) {
        RefreshWindowBestAsk(index + 1);
      }
    }
  }
  // old > 0 && fresh > 0: a size change at an existing level; cursors unmoved.
}

void DenseBook::SetFarLevel(Side side, Ticks px, Lots64 qty) {
  if (side == Side::kBid) {
    if (qty > 0) {
      far_bids_[px] = qty;
    } else {
      far_bids_.erase(px);
    }
  } else {
    if (qty > 0) {
      far_asks_[px] = qty;
    } else {
      far_asks_.erase(px);
    }
  }
}

void DenseBook::RefreshWindowBestBid(std::ptrdiff_t search_from) {
  const std::ptrdiff_t found = FindPrevSet(bid_.bits, search_from);
  win_best_bid_ = (found < 0) ? kNoBid : PriceOf(found);
}

void DenseBook::RefreshWindowBestAsk(std::ptrdiff_t search_from) {
  const std::ptrdiff_t found = FindNextSet(ask_.bits, search_from);
  win_best_ask_ = (found < 0) ? kNoAsk : PriceOf(found);
}

void DenseBook::MaybeReanchor() {
  const Ticks bb = BestBid();
  const Ticks ba = BestAsk();
  Ticks centre = 0;
  if (HasBid(bb) && HasAsk(ba)) {
    centre = static_cast<Ticks>((static_cast<std::int64_t>(bb) + static_cast<std::int64_t>(ba)) / 2);
  } else if (HasBid(bb)) {
    centre = bb;
  } else if (HasAsk(ba)) {
    centre = ba;
  } else {
    return;  // empty book; nothing to centre on
  }
  const std::int64_t drift =
      static_cast<std::int64_t>(centre) - static_cast<std::int64_t>(anchor_);
  const std::int64_t limit = kHalfWindow / 2;
  // Re-anchor only past half the window, so the operation cannot thrash: after
  // it, drift is zero and another 16k ticks of movement are needed to retrigger.
  if (drift > limit || drift < -limit) {
    Reanchor(centre);
  }
}

void DenseBook::Reanchor(Ticks new_anchor) {
  RebuildFrom(CollectBids(), CollectAsks(), new_anchor);
  ++reanchor_count_;
}

std::map<Ticks, Lots64, std::greater<Ticks>> DenseBook::CollectBids() const {
  std::map<Ticks, Lots64, std::greater<Ticks>> out(far_bids_);
  std::ptrdiff_t i = FindPrevSet(bid_.bits, static_cast<std::ptrdiff_t>(kWindowSize) - 1);
  while (i >= 0) {
    out[PriceOf(i)] = bid_.qty[static_cast<std::size_t>(i)];
    i = FindPrevSet(bid_.bits, i - 1);
  }
  return out;
}

std::map<Ticks, Lots64, std::less<Ticks>> DenseBook::CollectAsks() const {
  std::map<Ticks, Lots64, std::less<Ticks>> out(far_asks_);
  std::ptrdiff_t i = FindNextSet(ask_.bits, 0);
  while (i >= 0) {
    out[PriceOf(i)] = ask_.qty[static_cast<std::size_t>(i)];
    i = FindNextSet(ask_.bits, i + 1);
  }
  return out;
}

void DenseBook::RebuildFrom(const std::map<Ticks, Lots64, std::greater<Ticks>>& bids,
                            const std::map<Ticks, Lots64, std::less<Ticks>>& asks,
                            Ticks new_anchor) {
  bid_.qty.assign(kWindowSize, 0);
  bid_.bits.assign(kWords, 0);
  bid_.window_levels = 0;
  ask_.qty.assign(kWindowSize, 0);
  ask_.bits.assign(kWords, 0);
  ask_.window_levels = 0;
  far_bids_.clear();
  far_asks_.clear();
  win_best_bid_ = kNoBid;
  win_best_ask_ = kNoAsk;
  anchor_ = new_anchor;
  anchored_ = true;

  for (const auto& [px, qty] : bids) {
    if (qty > 0) {
      SetLevel(Side::kBid, px, qty);
    }
  }
  for (const auto& [px, qty] : asks) {
    if (qty > 0) {
      SetLevel(Side::kAsk, px, qty);
    }
  }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
void DenseBook::ApplySnapshotBegin() {
  in_snapshot_ = true;
  pending_bids_.clear();
  pending_asks_.clear();
}

void DenseBook::ApplySnapshotLevel(Side side, Ticks price_ticks, Lots64 qty) {
  if (qty <= 0) {
    return;
  }
  if (!in_snapshot_) {
    ApplyDepth(side, price_ticks, qty);
    return;
  }
  if (side == Side::kBid) {
    pending_bids_[price_ticks] = qty;
  } else {
    pending_asks_[price_ticks] = qty;
  }
}

void DenseBook::ApplySnapshotEnd() {
  if (!in_snapshot_) {
    return;
  }
  // Re-centre on the snapshot's own mid: a snapshot is exactly the moment when
  // the best information about where the window should sit is available.
  Ticks centre = anchor_;
  if (!pending_bids_.empty() && !pending_asks_.empty()) {
    centre = static_cast<Ticks>((static_cast<std::int64_t>(pending_bids_.begin()->first) +
                                 static_cast<std::int64_t>(pending_asks_.begin()->first)) /
                                2);
  } else if (!pending_bids_.empty()) {
    centre = pending_bids_.begin()->first;
  } else if (!pending_asks_.empty()) {
    centre = pending_asks_.begin()->first;
  }
  RebuildFrom(pending_bids_, pending_asks_, centre);
  pending_bids_.clear();
  pending_asks_.clear();
  in_snapshot_ = false;
}

void DenseBook::Clear() {
  bid_.qty.assign(kWindowSize, 0);
  bid_.bits.assign(kWords, 0);
  bid_.window_levels = 0;
  ask_.qty.assign(kWindowSize, 0);
  ask_.bits.assign(kWords, 0);
  ask_.window_levels = 0;
  far_bids_.clear();
  far_asks_.clear();
  win_best_bid_ = kNoBid;
  win_best_ask_ = kNoAsk;
  anchor_ = 0;
  anchored_ = false;
  in_snapshot_ = false;
  pending_bids_.clear();
  pending_asks_.clear();
  reanchor_count_ = 0;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
Ticks DenseBook::BestBid() const {
  const Ticks far = far_bids_.empty() ? kNoBid : far_bids_.begin()->first;
  if (!HasBid(win_best_bid_)) {
    return far;
  }
  if (!HasBid(far)) {
    return win_best_bid_;
  }
  return win_best_bid_ > far ? win_best_bid_ : far;
}

Ticks DenseBook::BestAsk() const {
  const Ticks far = far_asks_.empty() ? kNoAsk : far_asks_.begin()->first;
  if (!HasAsk(win_best_ask_)) {
    return far;
  }
  if (!HasAsk(far)) {
    return win_best_ask_;
  }
  return win_best_ask_ < far ? win_best_ask_ : far;
}

Lots64 DenseBook::QtyAt(Side side, Ticks price_ticks) const {
  const std::ptrdiff_t i = IndexOf(price_ticks);
  if (InWindowIndex(i)) {
    const SideData& s = (side == Side::kBid) ? bid_ : ask_;
    return s.qty[static_cast<std::size_t>(i)];
  }
  if (side == Side::kBid) {
    const auto it = far_bids_.find(price_ticks);
    return it == far_bids_.end() ? 0 : it->second;
  }
  const auto it = far_asks_.find(price_ticks);
  return it == far_asks_.end() ? 0 : it->second;
}

std::size_t DenseBook::LevelCount(Side side) const {
  return side == Side::kBid ? bid_.window_levels + far_bids_.size()
                            : ask_.window_levels + far_asks_.size();
}

Lots64 DenseBook::DepthWithin(Side side, Ticks n_ticks) const {
  if (n_ticks < 0) {
    return 0;
  }
  Lots64 total = 0;
  if (side == Side::kBid) {
    const Ticks best = BestBid();
    if (!HasBid(best)) {
      return 0;
    }
    const Ticks floor_px = best - n_ticks;
    std::ptrdiff_t i = FindPrevSet(bid_.bits, IndexOf(best));
    while (i >= 0) {
      const Ticks px = PriceOf(i);
      if (px < floor_px) {
        break;
      }
      total += bid_.qty[static_cast<std::size_t>(i)];
      i = FindPrevSet(bid_.bits, i - 1);
    }
    for (const auto& [px, qty] : far_bids_) {  // descending
      if (px > best) {
        continue;  // already counted only if in window; far levels above best
      }
      if (px < floor_px) {
        break;
      }
      total += qty;
    }
  } else {
    const Ticks best = BestAsk();
    if (!HasAsk(best)) {
      return 0;
    }
    const Ticks ceil_px = best + n_ticks;
    std::ptrdiff_t i = FindNextSet(ask_.bits, IndexOf(best));
    while (i >= 0) {
      const Ticks px = PriceOf(i);
      if (px > ceil_px) {
        break;
      }
      total += ask_.qty[static_cast<std::size_t>(i)];
      i = FindNextSet(ask_.bits, i + 1);
    }
    for (const auto& [px, qty] : far_asks_) {  // ascending
      if (px < best) {
        continue;
      }
      if (px > ceil_px) {
        break;
      }
      total += qty;
    }
  }
  return total;
}

BookIntegrity DenseBook::CheckInvariants() const {
  // Bitmap and quantities must agree, or every cursor slide is unsound.
  for (std::size_t i = 0; i < kWindowSize; ++i) {
    const bool bid_bit = (bid_.bits[i >> 6U] >> (i & 63U)) & 1U;
    if (bid_bit != (bid_.qty[i] > 0)) {
      return BookIntegrity::Fail("DenseBook: bid bitmap disagrees with quantity at index " +
                                 std::to_string(i));
    }
    const bool ask_bit = (ask_.bits[i >> 6U] >> (i & 63U)) & 1U;
    if (ask_bit != (ask_.qty[i] > 0)) {
      return BookIntegrity::Fail("DenseBook: ask bitmap disagrees with quantity at index " +
                                 std::to_string(i));
    }
    if (bid_.qty[i] < 0 || ask_.qty[i] < 0) {
      return BookIntegrity::Fail("DenseBook: negative quantity at index " + std::to_string(i));
    }
  }
  for (const auto& [px, qty] : far_bids_) {
    if (qty <= 0) {
      return BookIntegrity::Fail("DenseBook: non-positive far bid at tick " + std::to_string(px));
    }
  }
  for (const auto& [px, qty] : far_asks_) {
    if (qty <= 0) {
      return BookIntegrity::Fail("DenseBook: non-positive far ask at tick " + std::to_string(px));
    }
  }
  const Ticks bb = BestBid();
  const Ticks ba = BestAsk();
  if (HasBid(bb) && HasAsk(ba) && bb >= ba) {
    return BookIntegrity::Fail("DenseBook: crossed book, best_bid " + std::to_string(bb) +
                               " >= best_ask " + std::to_string(ba));
  }
  return BookIntegrity{};
}

}  // namespace lob
