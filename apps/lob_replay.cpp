// lob_replay -- run one (config, data range) and write the analytics CSVs.
//
//   lob_replay --config configs/s2_as.yaml \
//              --input data/binary/BTCUSDT_2026-09-01.lobbin \
//              --output data/results/s2_as_pess_fee20
//
// Command-line overrides exist so the experiment matrix (Part 7) is a sweep
// over one config file rather than a directory of near-identical copies.
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <lob/analytics/recorders.hpp>
#include <lob/config.hpp>
#include <lob/converter/event_io.hpp>
#include <lob/sim/simulator.hpp>
#include <lob/strategy/factory.hpp>

namespace {

void PrintUsage() {
  std::cout << R"(lob_replay -- replay one configuration over one binary event file

  --config PATH        experiment YAML (master plan Appendix A)
  --input  PATH        binary .lobbin event file (overrides input_path)
  --output DIR         results directory (overrides output_dir)

Overrides, for sweeps:
  --strategy NAME      S0_touch | S0_queue | S1 | S2_AS | S2_GLFT | S3
  --queue MODEL        PESS | OPT | PROP
  --fill-model MODEL   QUEUE | TOUCH
  --fee-bp X           maker fee in basis points (may be negative for a rebate)
  --latency-ms N       sets both the inbound and outbound constant legs
  --seed N
  --run-id NAME
  --dual-book          run map and dense books together and assert they agree
  --integrity-every N  run the O(levels) invariant check every N market events
  --quiet
  -h, --help
)";
}

bool Need(int argc, char** argv, int& i, const char* flag, std::string& out) {
  if (i + 1 >= argc) {
    std::cerr << "lob_replay: " << flag << " needs a value\n";
    return false;
  }
  out = argv[++i];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string input_override;
  std::string output_override;
  std::string strategy_override;
  std::string queue_override;
  std::string fill_override;
  std::string run_id_override;
  std::string fee_override;
  std::string latency_override;
  std::string seed_override;
  std::string integrity_override;
  bool dual_book = false;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if (a == "--config") {
      if (!Need(argc, argv, i, "--config", config_path)) return 2;
    } else if (a == "--input") {
      if (!Need(argc, argv, i, "--input", input_override)) return 2;
    } else if (a == "--output") {
      if (!Need(argc, argv, i, "--output", output_override)) return 2;
    } else if (a == "--strategy") {
      if (!Need(argc, argv, i, "--strategy", strategy_override)) return 2;
    } else if (a == "--queue") {
      if (!Need(argc, argv, i, "--queue", queue_override)) return 2;
    } else if (a == "--fill-model") {
      if (!Need(argc, argv, i, "--fill-model", fill_override)) return 2;
    } else if (a == "--fee-bp") {
      if (!Need(argc, argv, i, "--fee-bp", fee_override)) return 2;
    } else if (a == "--latency-ms") {
      if (!Need(argc, argv, i, "--latency-ms", latency_override)) return 2;
    } else if (a == "--seed") {
      if (!Need(argc, argv, i, "--seed", seed_override)) return 2;
    } else if (a == "--run-id") {
      if (!Need(argc, argv, i, "--run-id", run_id_override)) return 2;
    } else if (a == "--integrity-every") {
      if (!Need(argc, argv, i, "--integrity-every", integrity_override)) return 2;
    } else if (a == "--dual-book") {
      dual_book = true;
    } else if (a == "--quiet") {
      quiet = true;
    } else {
      std::cerr << "lob_replay: unknown argument '" << a << "'\n";
      PrintUsage();
      return 2;
    }
  }

  if (config_path.empty()) {
    std::cerr << "lob_replay: --config is required\n\n";
    PrintUsage();
    return 2;
  }

  try {
    lob::RunConfig config = lob::RunConfig::FromFile(config_path);

    if (!input_override.empty()) {
      config.input_path = input_override;
    }
    if (!output_override.empty()) {
      config.output_dir = output_override;
    }
    if (!strategy_override.empty()) {
      config.strategy.name = strategy_override;
      config.strategy.glft_mode = (strategy_override == "S2_GLFT");
      config.strategy.use_weighted_mid = (strategy_override == "S3");
      if (fill_override.empty()) {
        config.fill_model = (strategy_override == "S0_touch") ? "TOUCH" : "QUEUE";
      }
    }
    if (!queue_override.empty()) {
      config.queue_model = lob::ParseQueueModel(queue_override);
    }
    if (!fill_override.empty()) {
      config.fill_model = fill_override;
    }
    if (!fee_override.empty()) {
      const double bp = std::strtod(fee_override.c_str(), nullptr);
      config.fees.maker_tenth_bp = static_cast<std::int64_t>(std::llround(bp * 10.0));
    }
    if (!latency_override.empty()) {
      const long long ms = std::strtoll(latency_override.c_str(), nullptr, 10);
      config.latency.in_us = ms * lob::kUsPerMilli;
      config.latency.out_us = ms * lob::kUsPerMilli;
    }
    if (!seed_override.empty()) {
      config.seed = std::strtoull(seed_override.c_str(), nullptr, 10);
    }
    if (!run_id_override.empty()) {
      config.run_id = run_id_override;
    }
    if (dual_book) {
      config.dual_book_check = true;
    }

    if (config.input_path.empty()) {
      std::cerr << "lob_replay: no input (set input_path in the config or pass --input)\n";
      return 2;
    }

    const lob::IntensityCalibration calib = lob::LoadIntensityCalibration(config.strategy);
    if (!calib.error.empty()) {
      // Loud, not fatal: a run on fallback parameters is still a valid run, but
      // it must never be mistaken for a calibrated one.
      std::cerr << "lob_replay: WARNING -- " << calib.error
                << "; falling back to k=" << calib.k << " A=" << calib.a << "\n";
    }
    std::unique_ptr<lob::Strategy> strategy = lob::MakeStrategy(config.strategy, calib);

    const std::vector<lob::Event> events = lob::ReadAllEvents(config.input_path);
    if (events.empty()) {
      std::cerr << "lob_replay: " << config.input_path << " contains no events\n";
      return 3;
    }

    lob::RunTags tags;
    tags.run_id = config.run_id;
    tags.strategy = config.strategy.name;
    tags.symbol = config.PrimaryInstrument().symbol();
    tags.queue_assumption = std::string(lob::QueueModelName(config.queue_model));
    tags.maker_fee_bp = config.fees.maker_bp();
    tags.latency_in_ms = config.latency.in_us / lob::kUsPerMilli;
    tags.latency_out_ms = config.latency.out_us / lob::kUsPerMilli;
    tags.seed = config.seed;

    lob::RunRecorders recorders(config.output_dir, tags, config.PrimaryInstrument());

    lob::Simulator sim(config, *strategy);
    sim.set_recorders(&recorders);
    if (!integrity_override.empty()) {
      sim.set_integrity_check_every(std::strtoull(integrity_override.c_str(), nullptr, 10));
    }

    const lob::SimulatorStats& stats = sim.Run(events);

    recorders.WriteMarkouts(sim.markouts());
    recorders.WriteDailyPnl(sim.markouts().Summarise());
    recorders.WriteQueueStats(sim.queue_tracker().stats());

    std::string manifest = config.Manifest();
    manifest += "calibration:\n";
    manifest += "  source: " + calib.source + "\n";
    manifest += "  from_file: " + std::string(calib.from_file ? "true" : "false") + "\n";
    manifest += "  k: " + std::to_string(calib.k) + "\n";
    manifest += "  A: " + std::to_string(calib.a) + "\n";
    manifest += "input:\n";
    manifest += "  path: " + config.input_path + "\n";
    manifest += "  events: " + std::to_string(events.size()) + "\n";
    manifest += "  first_exch_ts_us: " + std::to_string(events.front().exch_ts_us) + "\n";
    manifest += "  last_exch_ts_us: " + std::to_string(events.back().exch_ts_us) + "\n";
    recorders.WriteManifest(manifest);
    recorders.CloseAll();

    // The §3.9 assertion.  Every term is an integer, so the residual must be
    // exactly zero; a non-zero value is a defect, not accumulated error.
    const std::int64_t residual = sim.ledger().IdentityResidualX2();
    if (residual != 0) {
      std::cerr << "lob_replay: FATAL -- PnL decomposition identity violated, residual = "
                << residual << " (x2 units)\n"
                << sim.ledger().IdentityReport();
      return 4;
    }

    if (!quiet) {
      std::cout << "=== " << config.run_id << " ===\n"
                << stats.Report() << "\n"
                << sim.ledger().IdentityReport() << "\n"
                << "strategy diagnostics\n"
                << "  quotes placed                " << strategy->diagnostics().quotes_placed
                << "\n"
                << "  quotes cancelled             " << strategy->diagnostics().quotes_cancelled
                << "\n"
                << "  requotes suppressed          "
                << strategy->diagnostics().requotes_suppressed_by_min_move << "\n"
                << "  blocked by min edge          "
                << strategy->diagnostics().quotes_blocked_by_min_edge << "\n"
                << "  blocked by inventory cap     "
                << strategy->diagnostics().quotes_blocked_by_inventory_cap << "\n"
                << "  toxicity pulls               " << strategy->diagnostics().toxicity_pulls
                << "\n\n"
                << "results written to " << config.output_dir << "\n";
    }
    if (stats.dual_book_mismatches > 0) {
      std::cerr << "lob_replay: FATAL -- dense and map books disagreed "
                << stats.dual_book_mismatches << " times\n";
      return 5;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "lob_replay: " << e.what() << "\n";
    return 1;
  }
}
