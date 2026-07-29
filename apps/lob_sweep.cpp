// lob_sweep -- run the full experiment matrix (master plan Part 7).
//
//   {queue: PESS, OPT, PROP}
// x {S0_touch, S0_queue, S1, S2_AS, S2_GLFT, S3}
// x {maker fee bp: -0.5, 0, 1, 2, 5, 10}
// x {latency ms: 5, 50, 200}
// x {symbols}
//
// Every cell is one replay over the same event file, so the whole matrix is
// cheap: the expensive part (reading and decoding the data) is shared, and only
// the configuration changes.  Turning one strategy into a MAP of viability
// across fees and latencies is far more scientific than reporting a single
// cherry-picked configuration -- and the fee-viability frontier is RQ4.
//
// Runs are sequential and each resets the RNG to the configured seed, so the
// matrix is reproducible cell by cell and independent of the order it is run in.
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <lob/analytics/recorders.hpp>
#include <lob/config.hpp>
#include <lob/converter/event_io.hpp>
#include <lob/csv_writer.hpp>
#include <lob/sim/simulator.hpp>
#include <lob/strategy/factory.hpp>
#include <lob/yaml.hpp>

namespace {

struct Axis {
  std::vector<std::string> strategies;
  std::vector<std::string> queue_models;
  std::vector<double> fees_bp;
  std::vector<std::int64_t> latencies_ms;
  std::vector<std::string> inputs;
};

std::vector<std::string> ReadStrings(const lob::yaml::Node& n,
                                     const std::vector<std::string>& fallback) {
  if (!n.IsSequence() || n.size() == 0) {
    return fallback;
  }
  std::vector<std::string> out;
  for (const lob::yaml::Node& e : n.elements()) {
    out.push_back(e.AsString("sweep[]"));
  }
  return out;
}

std::vector<double> ReadDoubles(const lob::yaml::Node& n, const std::vector<double>& fallback) {
  if (!n.IsSequence() || n.size() == 0) {
    return fallback;
  }
  std::vector<double> out;
  for (const lob::yaml::Node& e : n.elements()) {
    out.push_back(e.AsDouble("sweep[]"));
  }
  return out;
}

std::vector<std::int64_t> ReadInts(const lob::yaml::Node& n,
                                   const std::vector<std::int64_t>& fallback) {
  if (!n.IsSequence() || n.size() == 0) {
    return fallback;
  }
  std::vector<std::int64_t> out;
  for (const lob::yaml::Node& e : n.elements()) {
    out.push_back(e.AsInt("sweep[]"));
  }
  return out;
}

std::string Sanitise(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back((c == '.' || c == '-' || c == ' ' || c == '/') ? '_' : c);
  }
  return out;
}

void PrintUsage() {
  std::cout << R"(lob_sweep -- run the Part 7 experiment matrix

  --config PATH   base experiment YAML
  --matrix PATH   sweep YAML (configs/matrix.yaml); axes omitted here fall back
                  to the master plan Part 7 grid
  --output DIR    root results directory (one subdirectory per cell)
  --input  PATH   binary event file (repeatable; overrides the matrix axis)
  --dry-run       list the cells without running them
  -h, --help
)";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string matrix_path;
  std::string output_root = "data/results/matrix";
  std::vector<std::string> input_override;
  bool dry_run = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 >= argc) {
        std::cerr << "lob_sweep: " << a << " needs a value\n";
        std::exit(2);
      }
      dst = argv[++i];
    };
    std::string v;
    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if (a == "--config") {
      next(config_path);
    } else if (a == "--matrix") {
      next(matrix_path);
    } else if (a == "--output") {
      next(output_root);
    } else if (a == "--input") {
      next(v);
      input_override.push_back(v);
    } else if (a == "--dry-run") {
      dry_run = true;
    } else {
      std::cerr << "lob_sweep: unknown argument '" << a << "'\n";
      PrintUsage();
      return 2;
    }
  }

  if (config_path.empty()) {
    std::cerr << "lob_sweep: --config is required\n\n";
    PrintUsage();
    return 2;
  }

  try {
    const lob::RunConfig base = lob::RunConfig::FromFile(config_path);

    // Defaults are the Part 7 grid verbatim.
    Axis axis;
    axis.strategies = {"S0_touch", "S0_queue", "S1", "S2_AS", "S2_GLFT", "S3"};
    axis.queue_models = {"PESS", "OPT", "PROP"};
    axis.fees_bp = {-0.5, 0.0, 1.0, 2.0, 5.0, 10.0};
    axis.latencies_ms = {5, 50, 200};
    axis.inputs = base.input_path.empty() ? std::vector<std::string>{}
                                          : std::vector<std::string>{base.input_path};

    if (!matrix_path.empty()) {
      const lob::yaml::Node m = lob::yaml::ParseFile(matrix_path);
      axis.strategies = ReadStrings(m["strategies"], axis.strategies);
      axis.queue_models = ReadStrings(m["queue_models"], axis.queue_models);
      axis.fees_bp = ReadDoubles(m["fees_bp"], axis.fees_bp);
      axis.latencies_ms = ReadInts(m["latency_ms"], axis.latencies_ms);
      axis.inputs = ReadStrings(m["inputs"], axis.inputs);
    }
    if (!input_override.empty()) {
      axis.inputs = input_override;
    }
    if (axis.inputs.empty()) {
      std::cerr << "lob_sweep: no inputs (set input_path, the matrix `inputs` axis, or --input)\n";
      return 2;
    }

    const std::size_t cells = axis.inputs.size() * axis.strategies.size() *
                              axis.queue_models.size() * axis.fees_bp.size() *
                              axis.latencies_ms.size();
    std::cout << "lob_sweep: " << cells << " cells\n";

    lob::CsvWriter index;
    if (!dry_run) {
      index.Open(output_root + "/index.csv",
                 {"run_id", "input", "strategy", "queue_model", "fill_model", "maker_fee_bp",
                  "latency_ms", "seed", "output_dir", "events", "fills", "identity_residual_x2",
                  "wall_seconds"});
    }

    std::size_t done = 0;
    for (const std::string& input : axis.inputs) {
      // Read once per input; every cell over it replays the same array.
      const std::vector<lob::Event> events = lob::ReadAllEvents(input);
      if (events.empty()) {
        std::cerr << "lob_sweep: " << input << " contains no events; skipping\n";
        continue;
      }

      for (const std::string& strategy_name : axis.strategies) {
        for (const std::string& queue_name : axis.queue_models) {
          for (const double fee_bp : axis.fees_bp) {
            for (const std::int64_t latency_ms : axis.latencies_ms) {
              lob::RunConfig cfg = base;
              cfg.input_path = input;
              cfg.strategy.name = strategy_name;
              cfg.strategy.glft_mode = (strategy_name == "S2_GLFT");
              cfg.strategy.use_weighted_mid = (strategy_name == "S3");
              cfg.queue_model = lob::ParseQueueModel(queue_name);
              cfg.fill_model = (strategy_name == "S0_touch") ? "TOUCH" : "QUEUE";
              cfg.fees.maker_tenth_bp = static_cast<std::int64_t>(std::llround(fee_bp * 10.0));
              cfg.latency.in_us = latency_ms * lob::kUsPerMilli;
              cfg.latency.out_us = latency_ms * lob::kUsPerMilli;

              std::string cell = strategy_name + "_" + queue_name + "_fee" +
                                 Sanitise(std::to_string(fee_bp).substr(0, 5)) + "_lat" +
                                 std::to_string(latency_ms);
              cfg.run_id = cell;
              cfg.output_dir = output_root + "/" + cell;

              ++done;
              if (dry_run) {
                std::cout << "  [" << done << "/" << cells << "] " << cell << "\n";
                continue;
              }

              const auto t0 = std::chrono::steady_clock::now();
              const lob::IntensityCalibration calib =
                  lob::LoadIntensityCalibration(cfg.strategy);
              std::unique_ptr<lob::Strategy> strategy =
                  lob::MakeStrategy(cfg.strategy, calib);

              lob::RunTags tags;
              tags.run_id = cfg.run_id;
              tags.strategy = cfg.strategy.name;
              tags.symbol = cfg.PrimaryInstrument().symbol();
              tags.queue_assumption = std::string(lob::QueueModelName(cfg.queue_model));
              tags.maker_fee_bp = cfg.fees.maker_bp();
              tags.latency_in_ms = latency_ms;
              tags.latency_out_ms = latency_ms;
              tags.seed = cfg.seed;

              lob::RunRecorders rec(cfg.output_dir, tags, cfg.PrimaryInstrument());
              lob::Simulator sim(cfg, *strategy);
              sim.set_recorders(&rec);
              const lob::SimulatorStats& stats = sim.Run(events);

              rec.WriteMarkouts(sim.markouts());
              rec.WriteDailyPnl(sim.markouts().Summarise());
              rec.WriteQueueStats(sim.queue_tracker().stats());
              rec.WriteManifest(cfg.Manifest());
              rec.CloseAll();

              const std::int64_t residual = sim.ledger().IdentityResidualX2();
              const auto t1 = std::chrono::steady_clock::now();
              const double wall =
                  std::chrono::duration<double>(t1 - t0).count();

              index.Row(cfg.run_id, input, cfg.strategy.name, queue_name, cfg.fill_model,
                        cfg.fees.maker_bp(), latency_ms, cfg.seed, cfg.output_dir,
                        static_cast<std::uint64_t>(events.size()), stats.fills, residual, wall);
              index.Flush();

              std::cout << "  [" << done << "/" << cells << "] " << cell << "  fills="
                        << stats.fills << "  residual=" << residual << "  " << wall << "s\n";

              if (residual != 0) {
                // The identity is exact by construction, so a non-zero residual
                // means the run is wrong.  Stopping is the only honest response.
                std::cerr << "lob_sweep: FATAL -- PnL identity violated in cell " << cell
                          << "\n";
                index.Close();
                return 4;
              }
            }
          }
        }
      }
    }

    if (!dry_run) {
      index.Close();
      std::cout << "lob_sweep: matrix complete -- index at " << output_root << "/index.csv\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "lob_sweep: " << e.what() << "\n";
    return 1;
  }
}
