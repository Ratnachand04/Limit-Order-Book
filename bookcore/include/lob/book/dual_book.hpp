// DualBook -- runs the reference and the fast path side by side and asserts
// they agree (master plan §4.4, "array and map agree in dual-run mode").
//
// This is the mechanism that makes the dense path trustworthy: every unit test,
// every property/fuzz test and the debug CI job replay through DualBook, so any
// divergence between the two implementations is caught at the event that caused
// it rather than as a mysterious PnL difference days later.
//
// Reads are answered by the REFERENCE book.  If the fast path has drifted, the
// simulation still behaves correctly and the mismatch is reported rather than
// silently propagated into results.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <lob/book/book_view.hpp>
#include <lob/book/dense_book.hpp>
#include <lob/book/map_book.hpp>

namespace lob {

class DualBook final : public BookView {
 public:
  // Called with a description whenever the two paths disagree.  Defaults to
  // recording the first mismatch; tests install a handler that fails the test.
  using MismatchHandler = std::function<void(const std::string&)>;

  DualBook() = default;
  explicit DualBook(MismatchHandler on_mismatch) : on_mismatch_(std::move(on_mismatch)) {}

  void ApplyDepth(Side side, Ticks price_ticks, Lots64 new_qty) override;
  void ApplySnapshotBegin() override;
  void ApplySnapshotLevel(Side side, Ticks price_ticks, Lots64 qty) override;
  void ApplySnapshotEnd() override;
  void Clear() override;

  [[nodiscard]] Ticks BestBid() const override { return reference_.BestBid(); }
  [[nodiscard]] Ticks BestAsk() const override { return reference_.BestAsk(); }
  [[nodiscard]] Lots64 QtyAt(Side side, Ticks price_ticks) const override {
    return reference_.QtyAt(side, price_ticks);
  }
  [[nodiscard]] std::size_t LevelCount(Side side) const override {
    return reference_.LevelCount(side);
  }
  [[nodiscard]] Lots64 DepthWithin(Side side, Ticks n_ticks) const override {
    return reference_.DepthWithin(side, n_ticks);
  }
  [[nodiscard]] BookIntegrity CheckInvariants() const override;

  // Full structural comparison of the two books.  O(levels); called by the
  // tests after each scenario and by the replayer every `check_every` events.
  [[nodiscard]] BookIntegrity Compare() const;

  [[nodiscard]] const MapBook& reference() const { return reference_; }
  [[nodiscard]] const DenseBook& fast() const { return fast_; }
  [[nodiscard]] std::uint64_t mismatches() const { return mismatches_; }
  [[nodiscard]] const std::string& first_mismatch() const { return first_mismatch_; }

  // Compare after every mutation.  Correct but O(levels) per event, so it is
  // off by default and enabled by the unit tests.
  void set_check_every_update(bool on) { check_every_update_ = on; }

 private:
  void Report(const std::string& what) const;

  MapBook reference_;
  DenseBook fast_;
  MismatchHandler on_mismatch_;
  mutable std::uint64_t mismatches_ = 0;
  mutable std::string first_mismatch_;
  bool check_every_update_ = false;
};

}  // namespace lob
