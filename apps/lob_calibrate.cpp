// lob_calibrate -- fit lambda(delta) = A * exp(-k * delta)  (master plan §3.3).
//
// The empirical-hazard method, exactly as specified:
//
//   Over a calibration window, for each side and each distance delta on a grid
//   of 1..D ticks: each second, imagine a fresh infinitesimal quote at distance
//   delta from the current mid; count the second as an "execution" if
//   aggressive flow traded through that price (for a bid at m - delta: some
//   sell-aggressor trade printed at <= m - delta).  Then
//   lambda_hat(delta) = executions / total seconds, and fitting
//   ln lambda_hat(delta) = ln A - k * delta by least squares gives A_hat, k_hat.
//
// The fit is reported over the near region actually quoted in, together with
// the full curve, because the tail bends -- and the honest thing is to show
// both and say which one the strategy uses (§3.3, "report the fit plot").
//
// Output is the JSON that StrategyConfig::k_A_calibration points at, plus a CSV
// of the raw points for figure F5.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <lob/book/dense_book.hpp>
#include <lob/converter/event_io.hpp>
#include <lob/csv_writer.hpp>
#include <lob/instrument.hpp>

namespace {

struct Fit {
  double a = 0.0;
  double k = 0.0;
  double r_squared = 0.0;
  std::size_t points = 0;
  bool ok = false;
};

// Ordinary least squares of ln(lambda) on delta (in PRICE units), so that
// k comes out in 1/price -- the same units the master plan's worked example
// uses (k = 0.035 $^-1).
Fit FitExponential(const std::vector<double>& delta_price, const std::vector<double>& lambda,
                   std::size_t first, std::size_t last) {
  Fit f;
  std::vector<double> xs;
  std::vector<double> ys;
  for (std::size_t i = first; i < last && i < lambda.size(); ++i) {
    if (lambda[i] > 0.0) {
      xs.push_back(delta_price[i]);
      ys.push_back(std::log(lambda[i]));
    }
  }
  f.points = xs.size();
  if (xs.size() < 2) {
    return f;
  }
  const double n = static_cast<double>(xs.size());
  double sx = 0.0;
  double sy = 0.0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    sx += xs[i];
    sy += ys[i];
  }
  const double mx = sx / n;
  const double my = sy / n;
  double sxx = 0.0;
  double sxy = 0.0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    sxx += (xs[i] - mx) * (xs[i] - mx);
    sxy += (xs[i] - mx) * (ys[i] - my);
  }
  if (!(sxx > 0.0)) {
    return f;
  }
  const double slope = sxy / sxx;
  const double intercept = my - slope * mx;

  double ss_res = 0.0;
  double ss_tot = 0.0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const double pred = intercept + slope * xs[i];
    ss_res += (ys[i] - pred) * (ys[i] - pred);
    ss_tot += (ys[i] - my) * (ys[i] - my);
  }
  f.k = -slope;  // ln lambda = ln A - k * delta
  f.a = std::exp(intercept);
  f.r_squared = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 0.0;
  f.ok = std::isfinite(f.a) && std::isfinite(f.k);
  return f;
}

void PrintUsage() {
  std::cout << R"(lob_calibrate -- fit lambda(delta) = A exp(-k delta) from recorded events

  --input  PATH   binary .lobbin event file (repeatable)
  --symbol NAME
  --tick   SIZE
  --lot    SIZE
  --out    PATH   calibration JSON (default: calib.json)
  --points PATH   per-delta CSV for figure F5 (optional)
  --max-depth N   deepest delta in ticks to evaluate (default 50)
  --fit-depth N   deepest delta included in the headline fit (default 10)
  --bucket-ms N   hazard sampling period in ms (default 1000, i.e. 1 s per §3.3)
  -h, --help
)";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string symbol;
  std::string out_path = "calib.json";
  std::string points_path;
  double tick = 0.0;
  double lot = 0.0;
  int max_depth = 50;
  int fit_depth = 10;
  long long bucket_ms = 1000;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 >= argc) {
        std::cerr << "lob_calibrate: " << a << " needs a value\n";
        std::exit(2);
      }
      dst = argv[++i];
    };
    std::string v;
    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if (a == "--input") {
      next(v);
      inputs.push_back(v);
    } else if (a == "--symbol") {
      next(symbol);
    } else if (a == "--tick") {
      next(v);
      tick = std::strtod(v.c_str(), nullptr);
    } else if (a == "--lot") {
      next(v);
      lot = std::strtod(v.c_str(), nullptr);
    } else if (a == "--out") {
      next(out_path);
    } else if (a == "--points") {
      next(points_path);
    } else if (a == "--max-depth") {
      next(v);
      max_depth = std::atoi(v.c_str());
    } else if (a == "--fit-depth") {
      next(v);
      fit_depth = std::atoi(v.c_str());
    } else if (a == "--bucket-ms") {
      next(v);
      bucket_ms = std::strtoll(v.c_str(), nullptr, 10);
    } else {
      std::cerr << "lob_calibrate: unknown argument '" << a << "'\n";
      PrintUsage();
      return 2;
    }
  }

  if (inputs.empty() || symbol.empty() || tick <= 0.0 || lot <= 0.0) {
    std::cerr << "lob_calibrate: --input, --symbol, --tick and --lot are required\n\n";
    PrintUsage();
    return 2;
  }
  if (max_depth < 1 || fit_depth < 2 || bucket_ms <= 0) {
    std::cerr << "lob_calibrate: --max-depth >= 1, --fit-depth >= 2, --bucket-ms > 0\n";
    return 2;
  }
  fit_depth = std::min(fit_depth, max_depth);

  try {
    const lob::Instrument inst(symbol, 0, tick, lot);
    const std::size_t depths = static_cast<std::size_t>(max_depth);

    std::vector<std::uint64_t> bid_hits(depths, 0);
    std::vector<std::uint64_t> ask_hits(depths, 0);
    std::uint64_t buckets = 0;

    lob::DenseBook book;
    const lob::Ts bucket_us = bucket_ms * lob::kUsPerMilli;

    bool have_bucket = false;
    lob::Ts bucket_start = 0;
    bool have_bucket_mid = false;
    double bucket_mid_ticks = 0.0;
    // Deepest aggressive prints seen inside the current bucket.
    lob::Ticks lowest_sell_agg = 0;
    lob::Ticks highest_buy_agg = 0;
    bool saw_sell_agg = false;
    bool saw_buy_agg = false;

    auto close_bucket = [&]() {
      if (!have_bucket || !have_bucket_mid) {
        return;
      }
      ++buckets;
      for (std::size_t d = 0; d < depths; ++d) {
        const double delta_ticks = static_cast<double>(d + 1);
        // A bid at m - delta executes if a sell-aggressor printed at or below it.
        if (saw_sell_agg &&
            static_cast<double>(lowest_sell_agg) <= bucket_mid_ticks - delta_ticks) {
          ++bid_hits[d];
        }
        if (saw_buy_agg &&
            static_cast<double>(highest_buy_agg) >= bucket_mid_ticks + delta_ticks) {
          ++ask_hits[d];
        }
      }
      saw_sell_agg = false;
      saw_buy_agg = false;
      have_bucket_mid = false;
    };

    for (const std::string& path : inputs) {
      lob::EventReader reader(path);
      lob::Event e;
      while (reader.Next(e)) {
        // Dirty intervals are excluded: a calibration computed across a
        // sequence gap is fitted to a book we know was wrong (§4.4).
        const bool dirty = e.IsDirty();

        if (!have_bucket) {
          have_bucket = true;
          bucket_start = e.exch_ts_us - (e.exch_ts_us % bucket_us);
        }
        while (e.exch_ts_us >= bucket_start + bucket_us) {
          close_bucket();
          bucket_start += bucket_us;
        }

        switch (e.Type()) {
          case lob::EventType::kDepth:
            book.ApplyDepth(e.SideOf(), e.price_ticks, e.qty_lots);
            break;
          case lob::EventType::kSnapshotBegin:
            book.ApplySnapshotBegin();
            break;
          case lob::EventType::kSnapshotLevel:
            book.ApplySnapshotLevel(e.SideOf(), e.price_ticks, e.qty_lots);
            break;
          case lob::EventType::kSnapshotEnd:
            book.ApplySnapshotEnd();
            break;
          case lob::EventType::kTrade:
            if (!dirty) {
              if (e.SideOf() == lob::Side::kAsk) {  // sell-aggressor: hits bids
                if (!saw_sell_agg || e.price_ticks < lowest_sell_agg) {
                  lowest_sell_agg = e.price_ticks;
                }
                saw_sell_agg = true;
              } else {
                if (!saw_buy_agg || e.price_ticks > highest_buy_agg) {
                  highest_buy_agg = e.price_ticks;
                }
                saw_buy_agg = true;
              }
            }
            break;
        }

        // The mid at the START of the bucket is the reference the imaginary
        // quote is placed against.
        if (!have_bucket_mid && !dirty && book.HasBothSides()) {
          bucket_mid_ticks = book.MidTicks();
          have_bucket_mid = true;
        }
      }
    }
    close_bucket();

    if (buckets == 0) {
      std::cerr << "lob_calibrate: no usable sampling buckets -- the input has no clean "
                   "two-sided book\n";
      return 3;
    }

    // lambda_hat(delta) = executions / total seconds.
    const double seconds = static_cast<double>(buckets) *
                           (static_cast<double>(bucket_us) / static_cast<double>(lob::kUsPerSecond));
    std::vector<double> delta_price(depths);
    std::vector<double> lam_bid(depths);
    std::vector<double> lam_ask(depths);
    std::vector<double> lam_both(depths);
    for (std::size_t d = 0; d < depths; ++d) {
      delta_price[d] = static_cast<double>(d + 1) * tick;
      lam_bid[d] = static_cast<double>(bid_hits[d]) / seconds;
      lam_ask[d] = static_cast<double>(ask_hits[d]) / seconds;
      lam_both[d] = 0.5 * (lam_bid[d] + lam_ask[d]);
    }

    const std::size_t fit_last = static_cast<std::size_t>(fit_depth);
    const Fit fit_bid = FitExponential(delta_price, lam_bid, 0, fit_last);
    const Fit fit_ask = FitExponential(delta_price, lam_ask, 0, fit_last);
    const Fit fit_both = FitExponential(delta_price, lam_both, 0, fit_last);
    const Fit fit_full = FitExponential(delta_price, lam_both, 0, depths);

    if (!fit_both.ok) {
      std::cerr << "lob_calibrate: the fit did not converge -- too few non-zero lambda points. "
                   "Try a longer input or a smaller --fit-depth.\n";
      return 3;
    }

    if (!points_path.empty()) {
      lob::CsvWriter pts(points_path, {"symbol", "delta_ticks", "delta_price", "lambda_bid",
                                       "lambda_ask", "lambda_both", "executions_bid",
                                       "executions_ask", "seconds"});
      for (std::size_t d = 0; d < depths; ++d) {
        pts.Row(symbol, static_cast<std::int64_t>(d + 1), delta_price[d], lam_bid[d], lam_ask[d],
                lam_both[d], bid_hits[d], ask_hits[d], seconds);
      }
      pts.Close();
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::cerr << "lob_calibrate: cannot write " << out_path << "\n";
      return 1;
    }
    out.setf(std::ios::fmtflags(0), std::ios::floatfield);
    out.precision(12);
    out << "{\n"
        << "  \"symbol\": \"" << symbol << "\",\n"
        << "  \"tick_size\": " << tick << ",\n"
        << "  \"method\": \"empirical hazard, master plan 3.3\",\n"
        << "  \"bucket_ms\": " << bucket_ms << ",\n"
        << "  \"buckets\": " << buckets << ",\n"
        << "  \"seconds\": " << seconds << ",\n"
        << "  \"fit_depth_ticks\": " << fit_depth << ",\n"
        << "  \"max_depth_ticks\": " << max_depth << ",\n"
        << "  \"combined\": {\"A\": " << fit_both.a << ", \"k\": " << fit_both.k
        << ", \"r_squared\": " << fit_both.r_squared << ", \"points\": " << fit_both.points
        << "},\n"
        << "  \"bid\": {\"A\": " << fit_bid.a << ", \"k\": " << fit_bid.k
        << ", \"r_squared\": " << fit_bid.r_squared << "},\n"
        << "  \"ask\": {\"A\": " << fit_ask.a << ", \"k\": " << fit_ask.k
        << ", \"r_squared\": " << fit_ask.r_squared << "},\n"
        << "  \"full_range\": {\"A\": " << fit_full.a << ", \"k\": " << fit_full.k
        << ", \"r_squared\": " << fit_full.r_squared << "},\n"
        << "  \"A\": " << fit_both.a << ",\n"
        << "  \"k\": " << fit_both.k << "\n"
        << "}\n";
    out.close();

    std::cout << "lambda(delta) calibration for " << symbol << "\n"
              << "  sampling buckets   " << buckets << " (" << seconds << " s)\n"
              << "  near-region fit    A = " << fit_both.a << ", k = " << fit_both.k
              << " (R^2 = " << fit_both.r_squared << ", " << fit_both.points << " points)\n"
              << "  full-range fit     A = " << fit_full.a << ", k = " << fit_full.k
              << " (R^2 = " << fit_full.r_squared << ")\n"
              << "  written to         " << out_path << "\n";
    if (fit_full.r_squared + 0.05 < fit_both.r_squared) {
      std::cout << "  note: the full-range fit is materially worse than the near-region fit, "
                   "i.e. the tail bends. Quote from the near region and say so (3.3).\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "lob_calibrate: " << e.what() << "\n";
    return 1;
  }
}
