// lob_convert -- recorder JSONL -> binary Event file (master plan Phase 1).
//
// Usage:
//   lob_convert --symbol BTCUSDT --tick 0.01 --lot 0.001
//               --in data/raw/BTCUSDT_2026-08-01T00.jsonl
//               --out data/binary/BTCUSDT_2026-08-01T00.lobbin
//               --gaps data/binary/BTCUSDT_2026-08-01T00.gaps.csv
//
// Gzipped input is read through a pipe rather than linked against zlib, which
// keeps the build dependency-free:
//
//   python -m gzip -d < raw.jsonl.gz | lob_convert --in - --out out.lobbin ...
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <lob/converter/converter.hpp>
#include <lob/converter/event_io.hpp>
#include <lob/csv_writer.hpp>
#include <lob/instrument.hpp>

namespace {

void PrintUsage() {
  std::cout << R"(lob_convert -- recorder JSONL to binary Event file

Required:
  --symbol NAME        instrument symbol as written by the recorder (e.g. BTCUSDT)
  --tick   SIZE        tick size, decimal (e.g. 0.01)
  --lot    SIZE        lot size,  decimal (e.g. 0.001)
  --out    PATH        output .lobbin file

Input (repeatable; processed in the order given):
  --in     PATH        a JSONL file, or "-" for stdin

Optional:
  --symbol-id N        id stamped into every record (default 0)
  --market MODE        spot | futures | auto   (default auto)
  --from   US          drop events before this exchange timestamp (microseconds)
  --to     US          drop events after  this exchange timestamp (microseconds)
  --gaps   PATH        write a CSV of detected sequence gaps
  --stats  PATH        write the conversion statistics (default: stdout only)
  --depth-first        apply depth before trades at equal timestamps
                       (default is trades first -- the conservative choice, §3.7)
  -h, --help           this message
)";
}

bool NextArg(int argc, char** argv, int& i, const char* flag, std::string& out) {
  if (i + 1 >= argc) {
    std::cerr << "lob_convert: " << flag << " needs a value\n";
    return false;
  }
  out = argv[++i];
  return true;
}

bool ReadLineFrom(std::istream& in, std::string& line) {
  return static_cast<bool>(std::getline(in, line));
}

}  // namespace

int main(int argc, char** argv) {
  std::string symbol;
  std::string tick_s;
  std::string lot_s;
  std::string out_path;
  std::string gaps_path;
  std::string stats_path;
  std::string market_s = "auto";
  std::vector<std::string> inputs;
  int symbol_id = 0;
  lob::Ts from_us = 0;
  lob::Ts to_us = 0;
  bool trades_first = true;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    std::string v;
    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if (a == "--symbol") {
      if (!NextArg(argc, argv, i, "--symbol", symbol)) return 2;
    } else if (a == "--tick") {
      if (!NextArg(argc, argv, i, "--tick", tick_s)) return 2;
    } else if (a == "--lot") {
      if (!NextArg(argc, argv, i, "--lot", lot_s)) return 2;
    } else if (a == "--out") {
      if (!NextArg(argc, argv, i, "--out", out_path)) return 2;
    } else if (a == "--in") {
      if (!NextArg(argc, argv, i, "--in", v)) return 2;
      inputs.push_back(v);
    } else if (a == "--gaps") {
      if (!NextArg(argc, argv, i, "--gaps", gaps_path)) return 2;
    } else if (a == "--stats") {
      if (!NextArg(argc, argv, i, "--stats", stats_path)) return 2;
    } else if (a == "--market") {
      if (!NextArg(argc, argv, i, "--market", market_s)) return 2;
    } else if (a == "--symbol-id") {
      if (!NextArg(argc, argv, i, "--symbol-id", v)) return 2;
      symbol_id = std::atoi(v.c_str());
    } else if (a == "--from") {
      if (!NextArg(argc, argv, i, "--from", v)) return 2;
      from_us = std::strtoll(v.c_str(), nullptr, 10);
    } else if (a == "--to") {
      if (!NextArg(argc, argv, i, "--to", v)) return 2;
      to_us = std::strtoll(v.c_str(), nullptr, 10);
    } else if (a == "--depth-first") {
      trades_first = false;
    } else {
      std::cerr << "lob_convert: unknown argument '" << a << "'\n";
      PrintUsage();
      return 2;
    }
  }

  if (symbol.empty() || tick_s.empty() || lot_s.empty() || out_path.empty() || inputs.empty()) {
    std::cerr << "lob_convert: --symbol, --tick, --lot, --in and --out are all required\n\n";
    PrintUsage();
    return 2;
  }
  if (symbol_id < 0 || symbol_id > 255) {
    std::cerr << "lob_convert: --symbol-id must be in [0, 255]\n";
    return 2;
  }

  try {
    lob::ConverterOptions options;
    options.instrument = lob::Instrument(symbol, static_cast<std::uint8_t>(symbol_id),
                                         std::strtod(tick_s.c_str(), nullptr),
                                         std::strtod(lot_s.c_str(), nullptr));
    if (market_s == "spot") {
      options.market = lob::binance::Market::kSpot;
    } else if (market_s == "futures") {
      options.market = lob::binance::Market::kFutures;
    } else if (market_s == "auto") {
      options.market = lob::binance::Market::kAuto;
    } else {
      std::cerr << "lob_convert: --market must be spot | futures | auto\n";
      return 2;
    }
    options.min_ts_us = from_us;
    options.max_ts_us = to_us;
    options.trades_before_depth_at_equal_ts = trades_first;

    lob::Converter converter(options);
    std::uint64_t line_number = 0;
    std::string line;

    for (const std::string& path : inputs) {
      if (path == "-") {
        std::ios::sync_with_stdio(false);
        while (ReadLineFrom(std::cin, line)) {
          ++line_number;
          while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
          }
          if (!line.empty()) {
            converter.ProcessLine(line, line_number);
          }
        }
      } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
          std::cerr << "lob_convert: cannot open input " << path << "\n";
          return 1;
        }
        while (ReadLineFrom(in, line)) {
          ++line_number;
          while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
          }
          if (!line.empty()) {
            converter.ProcessLine(line, line_number);
          }
        }
      }
    }

    const std::vector<lob::Event> events = converter.Finish();
    lob::WriteAllEvents(out_path, events);

    if (!gaps_path.empty()) {
      lob::CsvWriter gaps(gaps_path, {"ts_us", "expected_seq", "got_seq", "line", "reason"});
      for (const lob::GapRecord& g : converter.gaps()) {
        gaps.Row(g.ts_us, g.expected, g.got, static_cast<std::int64_t>(g.line_number), g.reason);
      }
      gaps.Close();
    }

    const std::string report = converter.stats().Report();
    std::cout << report;
    if (!stats_path.empty()) {
      std::ofstream sout(stats_path, std::ios::binary | std::ios::trunc);
      sout << report;
    }

    // A file with no events is almost always a mistake (wrong --symbol, or a
    // raw file with no snapshot in it), so say so loudly instead of leaving an
    // empty output for the replayer to trip over later.
    if (events.empty()) {
      std::cerr << "lob_convert: WARNING -- no events were emitted. Check --symbol matches the "
                   "recorder's 's' field, and that the input contains at least one snapshot.\n";
      return 3;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "lob_convert: " << e.what() << "\n";
    return 1;
  }
}
