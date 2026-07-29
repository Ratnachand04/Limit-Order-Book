// Instrument definition: the single place where the real world (decimal prices
// and quantities) is converted into the integer grid the whole simulator uses.
//
// master plan §4.1:
//   price_ticks = llround(price / tick_size)
//   qty_lots    = llround(qty   / lot_size)
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <lob/types.hpp>

namespace lob {

class Instrument {
 public:
  Instrument() = default;
  Instrument(std::string symbol, std::uint8_t symbol_id, double tick_size, double lot_size);

  [[nodiscard]] const std::string& symbol() const { return symbol_; }
  [[nodiscard]] std::uint8_t symbol_id() const { return symbol_id_; }
  [[nodiscard]] double tick_size() const { return tick_size_; }
  [[nodiscard]] double lot_size() const { return lot_size_; }

  // --- decimal -> integer grid ---------------------------------------------
  [[nodiscard]] Ticks ToTicks(double price) const;
  [[nodiscard]] Lots ToLots(double qty) const;

  // --- integer grid -> decimal (presentation and analytics only) -----------
  [[nodiscard]] double ToPrice(Ticks t) const { return static_cast<double>(t) * tick_size_; }
  [[nodiscard]] double ToQty(Lots64 l) const { return static_cast<double>(l) * lot_size_; }

  // --- exact integer cash --------------------------------------------------
  //
  // notional = price * qty
  //          = (px_ticks * tick_size) * (qty_lots * lot_size)
  //          = px_ticks * qty_lots * (tick_size * lot_size)
  //
  // `cash_per_tick_lot_` is (tick_size * lot_size * 1e8) as an exact integer,
  // so the whole product stays in integers and cash accounting never rounds.
  // The constructor rejects instruments where that product is not a positive
  // whole number of 1e-8 units, rather than silently losing precision.
  [[nodiscard]] Cash Notional(Ticks px_ticks, Lots64 qty_lots) const {
    return static_cast<Cash>(px_ticks) * static_cast<Cash>(qty_lots) * cash_per_tick_lot_;
  }

  // Fee on a notional, with the rate given in *tenths of a basis point* so the
  // whole master-plan fee grid {-0.5, 0, 1, 2, 5, 10} bp is exactly integral.
  // Rounds half away from zero; sign-safe for rebates (negative fee_tenth_bp).
  [[nodiscard]] static Cash Fee(Cash notional, std::int64_t fee_tenth_bp) {
    constexpr std::int64_t kDen = 100'000;  // 1 tenth-bp = 1e-5 of notional
    const std::int64_t num = notional * fee_tenth_bp;
    return (num >= 0) ? (num + kDen / 2) / kDen : -((-num + kDen / 2) / kDen);
  }

  [[nodiscard]] static double CashToDouble(Cash c) {
    return static_cast<double>(c) / static_cast<double>(kCashScale);
  }

  [[nodiscard]] std::int64_t cash_per_tick_lot() const { return cash_per_tick_lot_; }

 private:
  std::string symbol_;
  std::uint8_t symbol_id_ = 0;
  double tick_size_ = 0.0;
  double lot_size_ = 0.0;
  double inv_tick_ = 0.0;
  double inv_lot_ = 0.0;
  std::int64_t cash_per_tick_lot_ = 0;
};

// A small registry so `symbol_id` in the binary stream can be resolved back to
// an Instrument during replay.
class InstrumentTable {
 public:
  void Add(const Instrument& inst);
  [[nodiscard]] const Instrument& ById(std::uint8_t id) const;
  [[nodiscard]] const Instrument* Find(std::string_view symbol) const;
  [[nodiscard]] bool Contains(std::uint8_t id) const;
  [[nodiscard]] std::size_t size() const { return count_; }

 private:
  std::vector<Instrument> by_id_ = std::vector<Instrument>(256);
  std::vector<bool> present_ = std::vector<bool>(256, false);
  std::size_t count_ = 0;
};

}  // namespace lob
