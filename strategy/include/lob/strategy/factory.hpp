// Strategy construction from configuration.
//
// A run is (config, data range) -> CSVs, so the only place a strategy name
// turns into an object is here.  Adding a strategy means adding one case, and
// the experiment matrix picks it up with no other change.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <lob/config.hpp>
#include <lob/sim/strategy.hpp>

namespace lob {

// The lambda(delta) = A * exp(-k * delta) fit of §3.3, loaded from the JSON the
// calibration script writes.  `from_file` is false when the configured
// fallbacks were used, which the run manifest records -- a result computed off
// fallback parameters must never be mistaken for a calibrated one.
struct IntensityCalibration {
  double k = 0.035;
  double a = 1.0;
  bool from_file = false;
  std::string source;
  std::string error;
};

IntensityCalibration LoadIntensityCalibration(const StrategyConfig& config);

// Names accepted by MakeStrategy, in ladder order.
const std::vector<std::string>& StrategyNames();

// Throws std::invalid_argument on an unknown name.
std::unique_ptr<Strategy> MakeStrategy(const StrategyConfig& config,
                                       const IntensityCalibration& calibration);

// Convenience: loads the calibration itself.
std::unique_ptr<Strategy> MakeStrategy(const StrategyConfig& config);

}  // namespace lob
