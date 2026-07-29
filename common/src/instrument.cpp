#include <lob/instrument.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lob {
namespace {

// tick_size * lot_size, expressed in 1e-8 units, must be a whole number for the
// integer cash ledger to be exact.  Anything within this tolerance of an
// integer is accepted (the inputs come from decimal strings in a config file,
// so e.g. 0.01 * 0.00001 * 1e8 lands at 9.999999999999998 rather than 10).
constexpr double kIntegerTolerance = 1e-6;

}  // namespace

Instrument::Instrument(std::string symbol, std::uint8_t symbol_id, double tick_size,
                       double lot_size)
    : symbol_(std::move(symbol)),
      symbol_id_(symbol_id),
      tick_size_(tick_size),
      lot_size_(lot_size) {
  if (!(tick_size_ > 0.0) || !std::isfinite(tick_size_)) {
    throw std::invalid_argument("Instrument " + symbol_ + ": tick_size must be finite and > 0");
  }
  if (!(lot_size_ > 0.0) || !std::isfinite(lot_size_)) {
    throw std::invalid_argument("Instrument " + symbol_ + ": lot_size must be finite and > 0");
  }
  inv_tick_ = 1.0 / tick_size_;
  inv_lot_ = 1.0 / lot_size_;

  const double raw = tick_size_ * lot_size_ * static_cast<double>(kCashScale);
  const double rounded = std::round(raw);
  if (rounded < 1.0 || std::fabs(raw - rounded) > kIntegerTolerance * std::max(1.0, rounded)) {
    throw std::invalid_argument(
        "Instrument " + symbol_ +
        ": tick_size * lot_size must be a whole number of 1e-8 cash units for the integer "
        "ledger to be exact (got " + std::to_string(raw) +
        "). Widen kCashScale or adjust the instrument definition.");
  }
  cash_per_tick_lot_ = static_cast<std::int64_t>(rounded);
}

Ticks Instrument::ToTicks(double price) const {
  const double scaled = price * inv_tick_;
  if (!std::isfinite(scaled) || scaled < static_cast<double>(std::numeric_limits<Ticks>::min()) ||
      scaled > static_cast<double>(std::numeric_limits<Ticks>::max())) {
    throw std::out_of_range("Instrument " + symbol_ + ": price " + std::to_string(price) +
                            " does not fit the int32 tick grid");
  }
  return static_cast<Ticks>(std::llround(scaled));
}

Lots Instrument::ToLots(double qty) const {
  const double scaled = qty * inv_lot_;
  if (!std::isfinite(scaled) || scaled < static_cast<double>(std::numeric_limits<Lots>::min()) ||
      scaled > static_cast<double>(std::numeric_limits<Lots>::max())) {
    throw std::out_of_range("Instrument " + symbol_ + ": quantity " + std::to_string(qty) +
                            " does not fit the int32 lot grid");
  }
  return static_cast<Lots>(std::llround(scaled));
}

void InstrumentTable::Add(const Instrument& inst) {
  const std::size_t id = inst.symbol_id();
  if (present_[id]) {
    throw std::invalid_argument("InstrumentTable: duplicate symbol_id " + std::to_string(id) +
                                " (" + by_id_[id].symbol() + " vs " + inst.symbol() + ")");
  }
  by_id_[id] = inst;
  present_[id] = true;
  ++count_;
}

const Instrument& InstrumentTable::ById(std::uint8_t id) const {
  if (!present_[id]) {
    throw std::out_of_range("InstrumentTable: no instrument registered for symbol_id " +
                            std::to_string(id));
  }
  return by_id_[id];
}

const Instrument* InstrumentTable::Find(std::string_view symbol) const {
  for (std::size_t i = 0; i < by_id_.size(); ++i) {
    if (present_[i] && by_id_[i].symbol() == symbol) {
      return &by_id_[i];
    }
  }
  return nullptr;
}

bool InstrumentTable::Contains(std::uint8_t id) const { return present_[id]; }

}  // namespace lob
