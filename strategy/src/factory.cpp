#include <lob/strategy/factory.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <lob/json.hpp>
#include <lob/strategy/strategies.hpp>

namespace lob {
namespace {

// Reads {"k": ..., "A": ...} from either the top level or a "combined" object,
// so the calibration script can emit per-side fits alongside the pooled one.
bool ReadKA(json::Reader& r, double& k, double& a, bool& saw_k, bool& saw_a) {
  if (!r.EnterObject()) {
    return false;
  }
  std::string_view key;
  while (r.NextMember(key)) {
    if (key == "k") {
      if (!r.ReadNumberLoose(k)) {
        return false;
      }
      saw_k = true;
    } else if (key == "A" || key == "a") {
      if (!r.ReadNumberLoose(a)) {
        return false;
      }
      saw_a = true;
    } else if (!r.SkipValue()) {
      return false;
    }
  }
  return r.ok();
}

}  // namespace

IntensityCalibration LoadIntensityCalibration(const StrategyConfig& config) {
  IntensityCalibration out;
  out.k = config.k_fallback;
  out.a = config.a_fallback;
  out.source = "config fallback (k_fallback / A_fallback)";

  if (config.k_a_calibration_path.empty()) {
    return out;
  }

  std::ifstream in(config.k_a_calibration_path, std::ios::binary);
  if (!in) {
    out.error = "cannot open calibration file: " + config.k_a_calibration_path;
    return out;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();

  double k = out.k;
  double a = out.a;
  bool saw_k = false;
  bool saw_a = false;

  json::Reader r(text);
  if (!r.EnterObject()) {
    out.error = "calibration file is not a JSON object";
    return out;
  }
  std::string_view key;
  while (r.NextMember(key)) {
    if (key == "k") {
      if (r.ReadNumberLoose(k)) {
        saw_k = true;
      }
    } else if (key == "A" || key == "a") {
      if (r.ReadNumberLoose(a)) {
        saw_a = true;
      }
    } else if (key == "combined" || key == "pooled") {
      if (!ReadKA(r, k, a, saw_k, saw_a)) {
        out.error = "malformed 'combined' object in calibration file";
        return out;
      }
    } else if (!r.SkipValue()) {
      out.error = "malformed calibration JSON";
      return out;
    }
  }
  if (!r.ok()) {
    out.error = std::string("malformed calibration JSON: ") + std::string(r.error());
    return out;
  }
  if (!saw_k || !saw_a) {
    out.error = "calibration file has no usable k / A pair";
    return out;
  }
  if (!(k > 0.0) || !(a > 0.0)) {
    out.error = "calibration k and A must both be > 0";
    return out;
  }
  out.k = k;
  out.a = a;
  out.from_file = true;
  out.source = config.k_a_calibration_path;
  return out;
}

const std::vector<std::string>& StrategyNames() {
  static const std::vector<std::string> kNames = {"S0_touch", "S0_queue", "S1",
                                                  "S2_AS",    "S2_GLFT",  "S3"};
  return kNames;
}

std::unique_ptr<Strategy> MakeStrategy(const StrategyConfig& config,
                                       const IntensityCalibration& calibration) {
  const std::string& n = config.name;
  if (n == "S0_touch" || n == "S0_queue") {
    // Same code, two fill models.  That is the entire point of RQ1: any
    // difference in the results is attributable to the fill model alone.
    return std::make_unique<S0Joiner>(n);
  }
  if (n == "S1") {
    return std::make_unique<S1FixedSpread>();
  }
  if (n == "S2_AS" || n == "S2_GLFT") {
    auto s = std::make_unique<S2AvellanedaStoikov>(config.glft_mode || n == "S2_GLFT");
    s->SetIntensity(calibration.k, calibration.a);
    return s;
  }
  if (n == "S3") {
    auto s = std::make_unique<S3MicroToxicity>(config.glft_mode);
    s->SetIntensity(calibration.k, calibration.a);
    return s;
  }

  std::ostringstream msg;
  msg << "unknown strategy '" << n << "'. Known strategies:";
  for (const std::string& known : StrategyNames()) {
    msg << " " << known;
  }
  throw std::invalid_argument(msg.str());
}

std::unique_ptr<Strategy> MakeStrategy(const StrategyConfig& config) {
  return MakeStrategy(config, LoadIntensityCalibration(config));
}

}  // namespace lob
