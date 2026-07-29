#include <lob/config.hpp>

#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace lob {
namespace {

[[noreturn]] void ConfigError(const std::string& msg) {
  throw std::invalid_argument("config: " + msg);
}

// Shortest round-trip representation, locale-independent and identical on every
// conforming toolchain -- the manifest is compared byte-for-byte by the
// determinism test, so operator<< (which honours the stream locale) will not do.
std::string Fmt(double v) {
  char tmp[40];
  const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
  if (res.ec != std::errc{}) {
    return "nan";
  }
  return std::string(tmp, static_cast<std::size_t>(res.ptr - tmp));
}

// Rounds a fee quoted in basis points to the integer tenths-of-a-bp the ledger
// uses.  Rejects anything finer than 0.1 bp rather than silently truncating.
std::int64_t FeeBpToTenths(double bp, const char* which) {
  const double tenths = bp * 10.0;
  const double rounded = std::round(tenths);
  if (std::fabs(tenths - rounded) > 1e-6) {
    ConfigError(std::string(which) + " fee " + std::to_string(bp) +
                " bp is finer than the 0.1 bp resolution of the integer ledger");
  }
  return static_cast<std::int64_t>(rounded);
}

Ts SecondsToUs(double s) { return static_cast<Ts>(std::llround(s * static_cast<double>(kUsPerSecond))); }

Ts MillisToUs(double ms) { return static_cast<Ts>(std::llround(ms * static_cast<double>(kUsPerMilli))); }

}  // namespace

// ---------------------------------------------------------------------------
// QueueModel
// ---------------------------------------------------------------------------
std::string_view QueueModelName(QueueModel m) {
  switch (m) {
    case QueueModel::kPess:
      return "PESS";
    case QueueModel::kOpt:
      return "OPT";
    case QueueModel::kProp:
      return "PROP";
  }
  return "?";
}

QueueModel ParseQueueModel(std::string_view s) {
  if (s == "PESS" || s == "pess" || s == "pessimistic") {
    return QueueModel::kPess;
  }
  if (s == "OPT" || s == "opt" || s == "optimistic") {
    return QueueModel::kOpt;
  }
  if (s == "PROP" || s == "prop" || s == "proportional") {
    return QueueModel::kProp;
  }
  ConfigError("queue_model must be one of PESS | OPT | PROP (got '" + std::string(s) + "')");
}

// ---------------------------------------------------------------------------
// LatencyConfig
// ---------------------------------------------------------------------------
LatencyConfig LatencyConfig::FromYaml(const yaml::Node& n) {
  LatencyConfig c;
  if (n.IsNull()) {
    return c;
  }
  if (n.Has("in")) {
    c.in_us = MillisToUs(n["in"].AsDouble("latency_ms.in"));
  }
  if (n.Has("out")) {
    c.out_us = MillisToUs(n["out"].AsDouble("latency_ms.out"));
  }
  if (n.Has("jitter_exp_ms")) {
    c.jitter_exp_us =
        n["jitter_exp_ms"].AsDouble("latency_ms.jitter_exp_ms") * static_cast<double>(kUsPerMilli);
  }
  if (c.in_us < 0 || c.out_us < 0 || c.jitter_exp_us < 0.0) {
    ConfigError("latency values must be non-negative");
  }
  return c;
}

// ---------------------------------------------------------------------------
// FeeConfig
// ---------------------------------------------------------------------------
FeeConfig FeeConfig::FromYaml(const yaml::Node& n) {
  FeeConfig c;
  if (n.IsNull()) {
    return c;
  }
  if (n.Has("maker")) {
    c.maker_tenth_bp = FeeBpToTenths(n["maker"].AsDouble("fees_bp.maker"), "maker");
  }
  if (n.Has("taker")) {
    c.taker_tenth_bp = FeeBpToTenths(n["taker"].AsDouble("fees_bp.taker"), "taker");
  }
  return c;
}

// ---------------------------------------------------------------------------
// StrategyConfig
// ---------------------------------------------------------------------------
StrategyConfig StrategyConfig::FromYaml(const yaml::Node& n) {
  StrategyConfig c;
  if (n.IsNull()) {
    return c;
  }
  c.name = n["name"].AsStringOr(c.name);

  c.order_size_lots = static_cast<Lots>(n["order_size_lots"].AsIntOr(c.order_size_lots));
  c.q_max_lots = n["q_max"].AsIntOr(c.q_max_lots);
  c.requote_min_ticks = static_cast<Ticks>(n["requote_min_ticks"].AsIntOr(c.requote_min_ticks));
  if (n.Has("timer_ms")) {
    c.timer_us = MillisToUs(n["timer_ms"].AsDouble("strategy.timer_ms"));
  }
  c.fixed_spread_ticks = static_cast<Ticks>(n["spread_ticks"].AsIntOr(c.fixed_spread_ticks));
  c.quote_both_sides = n["quote_both_sides"].AsBoolOr(c.quote_both_sides);

  if (n.Has("sigma_sample_s")) {
    c.sigma_sample_us = SecondsToUs(n["sigma_sample_s"].AsDouble("strategy.sigma_sample_s"));
  }
  if (n.Has("sigma_window_s")) {
    c.sigma_window_us = SecondsToUs(n["sigma_window_s"].AsDouble("strategy.sigma_window_s"));
  }

  c.gamma = n["gamma"].AsDoubleOr(c.gamma);
  c.horizon_s = n["horizon_s"].AsDoubleOr(c.horizon_s);
  c.k_a_calibration_path = n["k_A_calibration"].AsStringOr(c.k_a_calibration_path);
  c.k_fallback = n["k_fallback"].AsDoubleOr(c.k_fallback);
  c.a_fallback = n["A_fallback"].AsDoubleOr(c.a_fallback);
  c.glft_mode = n["glft_mode"].AsBoolOr(c.name == "S2_GLFT");
  c.tick_floor_at_touch = n["tick_floor_at_touch"].AsBoolOr(c.tick_floor_at_touch);

  c.use_weighted_mid = n["use_weighted_mid"].AsBoolOr(c.name == "S3");
  c.toxicity_window_s = n["toxicity_window_s"].AsDoubleOr(c.toxicity_window_s);
  c.toxicity_threshold = n["toxicity_threshold"].AsDoubleOr(c.toxicity_threshold);
  c.toxicity_pull_s = n["toxicity_pull_s"].AsDoubleOr(c.toxicity_pull_s);
  c.enforce_min_edge = n["enforce_min_edge"].AsBoolOr(c.enforce_min_edge);

  // --- validation ---------------------------------------------------------
  if (c.order_size_lots <= 0) {
    ConfigError("strategy.order_size_lots must be > 0");
  }
  if (c.q_max_lots <= 0) {
    ConfigError("strategy.q_max must be > 0 (it is a hard inventory cap)");
  }
  if (c.timer_us <= 0) {
    ConfigError("strategy.timer_ms must be > 0");
  }
  if (!(c.gamma > 0.0)) {
    // gamma converts price-variance into a utility penalty (§3.1); at gamma <= 0
    // the A-S first-order conditions have no finite solution.
    ConfigError("strategy.gamma must be > 0");
  }
  if (!(c.horizon_s > 0.0)) {
    ConfigError("strategy.horizon_s must be > 0");
  }
  if (c.sigma_sample_us <= 0 || c.sigma_window_us < c.sigma_sample_us) {
    ConfigError("strategy.sigma_window_s must be >= sigma_sample_s > 0");
  }
  if (c.requote_min_ticks < 0) {
    ConfigError("strategy.requote_min_ticks must be >= 0");
  }
  if (!(c.toxicity_threshold >= 0.0 && c.toxicity_threshold <= 1.0)) {
    ConfigError("strategy.toxicity_threshold must lie in [0, 1]");
  }
  return c;
}

// ---------------------------------------------------------------------------
// ProbeConfig
// ---------------------------------------------------------------------------
ProbeConfig ProbeConfig::FromYaml(const yaml::Node& n) {
  ProbeConfig c;
  if (n.IsNull()) {
    return c;
  }
  c.enabled = n["enabled"].AsBoolOr(true);
  if (n.Has("interval_s")) {
    c.interval_us = SecondsToUs(n["interval_s"].AsDouble("probes.interval_s"));
  }
  c.size_lots = static_cast<Lots>(n["size_lots"].AsIntOr(c.size_lots));
  if (n["depths_ticks"].IsSequence()) {
    c.depths_ticks.clear();
    for (const yaml::Node& d : n["depths_ticks"].elements()) {
      c.depths_ticks.push_back(static_cast<Ticks>(d.AsInt("probes.depths_ticks[]")));
    }
  }
  if (n["horizons_s"].IsSequence()) {
    c.horizons_us.clear();
    for (const yaml::Node& h : n["horizons_s"].elements()) {
      c.horizons_us.push_back(SecondsToUs(h.AsDouble("probes.horizons_s[]")));
    }
  }
  if (c.enabled && (c.depths_ticks.empty() || c.horizons_us.empty())) {
    ConfigError("probes.enabled requires non-empty depths_ticks and horizons_s");
  }
  if (c.interval_us <= 0) {
    ConfigError("probes.interval_s must be > 0");
  }
  return c;
}

// ---------------------------------------------------------------------------
// MarkoutConfig
// ---------------------------------------------------------------------------
MarkoutConfig MarkoutConfig::FromYaml(const yaml::Node& n) {
  MarkoutConfig c;
  if (n.IsNull()) {
    return c;
  }
  if (n["horizons_s"].IsSequence()) {
    c.horizons_us.clear();
    for (const yaml::Node& h : n["horizons_s"].elements()) {
      c.horizons_us.push_back(SecondsToUs(h.AsDouble("markouts.horizons_s[]")));
    }
  }
  if (c.horizons_us.empty()) {
    ConfigError("markouts.horizons_s must not be empty");
  }
  // The sampler walks horizons in order and relies on them being sorted.
  for (std::size_t i = 1; i < c.horizons_us.size(); ++i) {
    if (c.horizons_us[i] <= c.horizons_us[i - 1]) {
      ConfigError("markouts.horizons_s must be strictly increasing");
    }
  }
  return c;
}

// ---------------------------------------------------------------------------
// RunConfig
// ---------------------------------------------------------------------------
const Instrument& RunConfig::PrimaryInstrument() const {
  if (instruments.empty()) {
    ConfigError("no instruments defined (need `symbols` and `tick_size`/`lot_size` maps)");
  }
  return instruments.front();
}

InstrumentTable RunConfig::BuildInstrumentTable() const {
  InstrumentTable table;
  for (const Instrument& i : instruments) {
    table.Add(i);
  }
  return table;
}

RunConfig RunConfig::FromYaml(const yaml::Node& root) {
  RunConfig c;
  c.run_id = root["run_id"].AsStringOr("");
  c.seed = static_cast<std::uint64_t>(root["seed"].AsIntOr(static_cast<std::int64_t>(c.seed)));
  c.input_path = root["input_path"].AsStringOr(c.input_path);
  c.output_dir = root["output_dir"].AsStringOr(c.output_dir);
  c.dual_book_check = root["dual_book_check"].AsBoolOr(c.dual_book_check);
  c.strategy = StrategyConfig::FromYaml(root["strategy"]);
  c.fill_model = root["fill_model"].AsStringOr(c.strategy.name == "S0_touch" ? "TOUCH" : "QUEUE");
  if (c.fill_model != "TOUCH" && c.fill_model != "QUEUE") {
    ConfigError("fill_model must be TOUCH or QUEUE (got '" + c.fill_model + "')");
  }

  if (root.Has("queue_model")) {
    c.queue_model = ParseQueueModel(root["queue_model"].AsString("queue_model"));
  }
  c.latency = LatencyConfig::FromYaml(root["latency_ms"]);
  c.fees = FeeConfig::FromYaml(root["fees_bp"]);
  c.probes = ProbeConfig::FromYaml(root["probes"]);
  c.markouts = MarkoutConfig::FromYaml(root["markouts"]);

  // --- instruments --------------------------------------------------------
  const yaml::Node& symbols = root["symbols"];
  if (!symbols.IsSequence() || symbols.size() == 0) {
    ConfigError("`symbols` must be a non-empty sequence");
  }
  const yaml::Node& ticks = root["tick_size"];
  const yaml::Node& lots = root["lot_size"];
  if (!ticks.IsMap()) {
    ConfigError("`tick_size` must be a mapping from symbol to tick size");
  }
  if (!lots.IsMap()) {
    ConfigError("`lot_size` must be a mapping from symbol to lot size");
  }
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    const std::string sym = symbols[i].AsString("symbols[]");
    if (!ticks.Has(sym)) {
      ConfigError("tick_size has no entry for symbol " + sym);
    }
    if (!lots.Has(sym)) {
      ConfigError("lot_size has no entry for symbol " + sym);
    }
    if (i > 255) {
      ConfigError("at most 256 symbols are supported (symbol_id is a uint8)");
    }
    c.instruments.emplace_back(sym, static_cast<std::uint8_t>(i),
                               ticks[sym].AsDouble("tick_size." + sym),
                               lots[sym].AsDouble("lot_size." + sym));
  }

  // --- evaluation window --------------------------------------------------
  const yaml::Node& win = root["eval_window"];
  if (win.IsMap()) {
    c.eval_start = win["start"].AsStringOr("");
    c.eval_end = win["end"].AsStringOr("");
  }

  if (c.run_id.empty()) {
    // A stable, human-readable default so results are never anonymous.
    std::ostringstream id;
    id << c.strategy.name << "_" << QueueModelName(c.queue_model) << "_fee"
       << c.fees.maker_tenth_bp << "_lat" << (c.latency.in_us / kUsPerMilli) << "_seed" << c.seed;
    c.run_id = id.str();
  }
  return c;
}

RunConfig RunConfig::FromFile(const std::string& path) {
  return FromYaml(yaml::ParseFile(path));
}

std::string RunConfig::Manifest() const {
  std::ostringstream os;
  os << "# lob_sim run manifest -- generated, do not edit\n";
  os << "run_id: " << run_id << "\n";
  os << "seed: " << seed << "\n";
  os << "queue_model: " << QueueModelName(queue_model) << "\n";
  os << "fill_model: " << fill_model << "\n";
  os << "input_path: " << input_path << "\n";
  os << "output_dir: " << output_dir << "\n";
  os << "eval_window: {start: " << eval_start << ", end: " << eval_end << "}\n";
  os << "latency_us: {in: " << latency.in_us << ", out: " << latency.out_us
     << ", jitter_exp: " << Fmt(latency.jitter_exp_us) << "}\n";
  os << "fees_tenth_bp: {maker: " << fees.maker_tenth_bp << ", taker: " << fees.taker_tenth_bp
     << "}\n";
  os << "strategy:\n";
  os << "  name: " << strategy.name << "\n";
  os << "  order_size_lots: " << strategy.order_size_lots << "\n";
  os << "  q_max: " << strategy.q_max_lots << "\n";
  os << "  requote_min_ticks: " << strategy.requote_min_ticks << "\n";
  os << "  timer_us: " << strategy.timer_us << "\n";
  os << "  spread_ticks: " << strategy.fixed_spread_ticks << "\n";
  os << "  gamma: " << Fmt(strategy.gamma) << "\n";
  os << "  horizon_s: " << Fmt(strategy.horizon_s) << "\n";
  os << "  sigma_sample_us: " << strategy.sigma_sample_us << "\n";
  os << "  sigma_window_us: " << strategy.sigma_window_us << "\n";
  os << "  glft_mode: " << (strategy.glft_mode ? "true" : "false") << "\n";
  os << "  tick_floor_at_touch: " << (strategy.tick_floor_at_touch ? "true" : "false") << "\n";
  os << "  use_weighted_mid: " << (strategy.use_weighted_mid ? "true" : "false") << "\n";
  os << "  k_A_calibration: " << strategy.k_a_calibration_path << "\n";
  os << "instruments:\n";
  for (const Instrument& i : instruments) {
    os << "  - {symbol: " << i.symbol() << ", id: " << static_cast<int>(i.symbol_id())
       << ", tick_size: " << Fmt(i.tick_size())
       << ", lot_size: " << Fmt(i.lot_size()) << "}\n";
  }
  os << "markout_horizons_us: [";
  for (std::size_t i = 0; i < markouts.horizons_us.size(); ++i) {
    os << (i ? ", " : "") << markouts.horizons_us[i];
  }
  os << "]\n";
  os << "probes: {enabled: " << (probes.enabled ? "true" : "false")
     << ", interval_us: " << probes.interval_us << "}\n";
  return os.str();
}

}  // namespace lob
