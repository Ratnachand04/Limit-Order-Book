#!/usr/bin/env python3
"""Generate every figure and table from committed CSVs (master plan Part 7).

    python analysis/make_figures.py --results data/results/matrix --out data/figures

Figures, in the master plan's numbering:

  F1  markout curves by strategy                      the money plot
  F2  fill-probability heatmap: depth x queue fraction RQ2
  F3  PnL decomposition across the fee grid           RQ4 frontier
  F4  touch vs queue-aware fill counts and PnL        RQ1
  F5  lambda(delta) calibration fit                   §3.3
  F6  inventory paths, S1 vs S2                       the skew taming q

Tables: matrix summary with block-bootstrap CIs, and the break-even fee per
strategy and symbol.

THE RULE THIS SCRIPT ENFORCES (CLAUDE.md rule 4): every number comes from a CSV
that a real run produced. If an input is missing, the figure is SKIPPED with a
message saying which file was absent. Nothing is ever invented, interpolated
from nothing, or filled with example values. An empty figures directory and an
honest list of what is missing is the correct output when there is no data.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    import pandas as pd
except ImportError:  # pragma: no cover
    print(
        "analysis: missing dependencies.\n"
        "    python -m pip install -r analysis/requirements.txt",
        file=sys.stderr,
    )
    raise

from bootstrap import (  # noqa: E402
    DEFAULT_BLOCK_DAYS,
    bootstrap_statistic,
    break_even_fee_bp,
    difference_interval,
)

SKIPPED: list[str] = []
WRITTEN: list[str] = []


def skip(figure: str, reason: str) -> None:
    SKIPPED.append(f"{figure}: {reason}")
    print(f"  SKIP {figure}  ({reason})")


def save(fig, out_dir: Path, name: str) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    WRITTEN.append(str(path))
    print(f"  wrote {path}")


def load_concat(results: Path, filename: str) -> pd.DataFrame | None:
    """Concatenate `filename` across every run directory under `results`."""
    frames = []
    for path in sorted(results.rglob(filename)):
        try:
            frame = pd.read_csv(path)
        except (OSError, pd.errors.ParserError, pd.errors.EmptyDataError):
            continue
        if not frame.empty:
            frames.append(frame)
    if not frames:
        return None
    return pd.concat(frames, ignore_index=True)


# ---------------------------------------------------------------------------
# F1 -- markout curves
# ---------------------------------------------------------------------------
def figure_f1(results: Path, out_dir: Path) -> None:
    markouts = load_concat(results, "markouts.csv")
    if markouts is None:
        return skip("F1", "no markouts.csv found")
    resolved = markouts[markouts["resolved"].astype(bool)]
    if resolved.empty:
        return skip("F1", "markouts.csv has no resolved samples")

    fig, ax = plt.subplots(figsize=(8, 5))
    for strategy, group in resolved.groupby("strategy"):
        curve = group.groupby("horizon_s")["markout_bp"].mean().sort_index()
        ax.plot(curve.index, curve.values, marker="o", label=str(strategy))
    ax.axhline(0.0, color="black", linewidth=0.8, linestyle="--")
    ax.set_xscale("log")
    ax.set_xlabel("horizon h after the fill (s, log scale)")
    ax.set_ylabel("mean markout (bp of notional)")
    ax.set_title("F1  Markout curves by strategy")
    ax.legend()
    ax.grid(alpha=0.3)
    save(fig, out_dir, "F1_markout_curves.png")

    summary = (
        resolved.groupby(["strategy", "queue_assumption", "horizon_s"])
        .agg(
            fills=("markout_bp", "size"),
            edge_bp=("edge_bp", "mean"),
            adverse_selection_bp=("adverse_selection_bp", "mean"),
            markout_bp=("markout_bp", "mean"),
        )
        .reset_index()
    )
    path = out_dir / "T1_markout_summary.csv"
    summary.to_csv(path, index=False)
    WRITTEN.append(str(path))
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# F2 -- fill-probability heatmap (RQ2)
# ---------------------------------------------------------------------------
def figure_f2(results: Path, out_dir: Path) -> None:
    probes = load_concat(results, "probes.csv")
    if probes is None:
        return skip("F2", "no probes.csv found (run with probes.enabled: true)")
    if probes.empty:
        return skip("F2", "probes.csv is empty")

    # One horizon at a time; pick the middle of the grid for the headline plot.
    horizons = sorted(probes["horizon_s"].unique())
    horizon = horizons[len(horizons) // 2]
    subset = probes[probes["horizon_s"] == horizon].copy()
    if subset.empty:
        return skip("F2", "no probe rows at the chosen horizon")

    subset["qf_bucket"] = pd.cut(
        subset["initial_queue_fraction"], bins=np.linspace(0.0, 1.0, 11), include_lowest=True
    )
    grid = (
        subset.groupby(["depth_ticks", "qf_bucket"], observed=True)["filled"]
        .mean()
        .unstack()
    )
    if grid.empty:
        return skip("F2", "not enough probe coverage to form a grid")

    fig, ax = plt.subplots(figsize=(9, 5))
    im = ax.imshow(grid.values, aspect="auto", origin="lower", cmap="viridis", vmin=0, vmax=1)
    ax.set_xticks(range(len(grid.columns)))
    ax.set_xticklabels([f"{iv.left:.1f}" for iv in grid.columns], rotation=45, ha="right")
    ax.set_yticks(range(len(grid.index)))
    ax.set_yticklabels(grid.index)
    ax.set_xlabel("queue fraction ahead at placement")
    ax.set_ylabel("depth (ticks from the touch)")
    ax.set_title(f"F2  P(fill within {horizon:g}s)   [RQ2]")
    fig.colorbar(im, ax=ax, label="fill probability")
    save(fig, out_dir, "F2_fill_probability_heatmap.png")

    # The simulator's own validation: the monotonicities MUST hold (§3.8).
    by_depth = subset.groupby("depth_ticks")["filled"].mean().sort_index()
    monotone = bool((by_depth.diff().dropna() <= 1e-9).all())
    print(f"  RQ2 check: P(fill) falls with depth: {monotone}")
    if not monotone:
        print("  WARNING: deeper probes filled MORE often. Something in the queue "
              "model is wrong -- do not publish this run (§3.8).")


# ---------------------------------------------------------------------------
# F3 -- PnL decomposition across the fee grid (RQ4)
# ---------------------------------------------------------------------------
def figure_f3(results: Path, out_dir: Path) -> None:
    daily = load_concat(results, "pnl_daily.csv")
    if daily is None:
        return skip("F3", "no pnl_daily.csv found")

    agg = (
        daily.groupby(["strategy", "maker_fee_bp"])
        .agg(
            spread_capture=("spread_capture", "sum"),
            inventory_pnl=("inventory_pnl", "sum"),
            fees=("fees", "sum"),
            total=("total", "sum"),
        )
        .reset_index()
    )
    if agg.empty:
        return skip("F3", "pnl_daily.csv has no rows")

    strategies = sorted(agg["strategy"].unique())
    fig, axes = plt.subplots(1, len(strategies), figsize=(4 * len(strategies), 5), sharey=True,
                             squeeze=False)
    for ax, strategy in zip(axes[0], strategies):
        sub = agg[agg["strategy"] == strategy].sort_values("maker_fee_bp")
        x = np.arange(len(sub))
        ax.bar(x, sub["spread_capture"], label="spread capture")
        ax.bar(x, sub["inventory_pnl"], bottom=sub["spread_capture"], label="inventory")
        ax.bar(x, sub["fees"], bottom=sub["spread_capture"] + sub["inventory_pnl"], label="fees")
        ax.plot(x, sub["total"], color="black", marker="o", linewidth=1.5, label="total")
        ax.axhline(0.0, color="black", linewidth=0.8)
        ax.set_xticks(x)
        ax.set_xticklabels([f"{v:g}" for v in sub["maker_fee_bp"]])
        ax.set_xlabel("maker fee (bp)")
        ax.set_title(strategy)
        ax.grid(alpha=0.3, axis="y")
    axes[0][0].set_ylabel("PnL over the window (quote currency)")
    axes[0][-1].legend(fontsize=8)
    fig.suptitle("F3  PnL decomposition across the fee grid   [RQ4]")
    save(fig, out_dir, "F3_pnl_decomposition.png")

    rows = []
    for strategy, group in agg.groupby("strategy"):
        group = group.sort_values("maker_fee_bp")
        rows.append(
            {
                "strategy": strategy,
                "break_even_fee_bp": break_even_fee_bp(group["maker_fee_bp"], group["total"]),
                "fees_swept": ", ".join(f"{v:g}" for v in group["maker_fee_bp"]),
            }
        )
    table = pd.DataFrame(rows)
    path = out_dir / "T2_break_even_fee.csv"
    table.to_csv(path, index=False)
    WRITTEN.append(str(path))
    print(f"  wrote {path}")
    print("  NaN break-even means the strategy never crossed zero inside the swept "
          "range -- report that, do not extrapolate.")


# ---------------------------------------------------------------------------
# F4 -- touch vs queue-aware (RQ1)
# ---------------------------------------------------------------------------
def figure_f4(results: Path, out_dir: Path) -> None:
    daily = load_concat(results, "pnl_daily.csv")
    if daily is None:
        return skip("F4", "no pnl_daily.csv found")

    touch = daily[daily["strategy"] == "S0_touch"]
    queue = daily[daily["strategy"] == "S0_queue"]
    if touch.empty or queue.empty:
        return skip("F4", "need BOTH S0_touch and S0_queue runs -- that pairing IS RQ1")

    key = ["date", "maker_fee_bp", "latency_in_ms"]
    merged = touch.merge(queue, on=key, suffixes=("_touch", "_queue"))
    if merged.empty:
        return skip("F4", "S0_touch and S0_queue runs do not share any (date, fee, latency)")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 5))
    ax1.scatter(merged["fills_queue"], merged["fills_touch"], alpha=0.7)
    lim = max(merged["fills_touch"].max(), merged["fills_queue"].max(), 1)
    ax1.plot([0, lim], [0, lim], "k--", linewidth=0.8, label="y = x")
    ax1.set_xlabel("fills, queue-aware")
    ax1.set_ylabel("fills, touch rule")
    ax1.set_title("Fill counts, same strategy and data")
    ax1.legend()
    ax1.grid(alpha=0.3)

    ax2.scatter(merged["total_queue"], merged["total_touch"], alpha=0.7)
    lo = min(merged["total_touch"].min(), merged["total_queue"].min())
    hi = max(merged["total_touch"].max(), merged["total_queue"].max())
    ax2.plot([lo, hi], [lo, hi], "k--", linewidth=0.8, label="y = x")
    ax2.axhline(0, color="grey", linewidth=0.6)
    ax2.axvline(0, color="grey", linewidth=0.6)
    ax2.set_xlabel("daily PnL, queue-aware")
    ax2.set_ylabel("daily PnL, touch rule")
    ax2.set_title("Daily PnL, same strategy and data")
    ax2.legend()
    ax2.grid(alpha=0.3)
    fig.suptitle("F4  How wrong is a naive backtest?   [RQ1]")
    save(fig, out_dir, "F4_touch_vs_queue.png")

    fill_ratio = (
        float(merged["fills_touch"].sum()) / float(merged["fills_queue"].sum())
        if merged["fills_queue"].sum() > 0
        else float("nan")
    )
    pnl_gap = difference_interval(
        merged["total_touch"].tolist(), merged["total_queue"].tolist(),
        block_size=DEFAULT_BLOCK_DAYS,
    )
    table = pd.DataFrame(
        [
            {
                "metric": "fill count ratio (touch / queue-aware)",
                "point": fill_ratio,
                "ci_lower": "",
                "ci_upper": "",
                "n_days": int(merged.shape[0]),
            },
            {
                "metric": "daily PnL gap (touch - queue-aware)",
                "point": pnl_gap.point,
                "ci_lower": pnl_gap.lower,
                "ci_upper": pnl_gap.upper,
                "n_days": pnl_gap.n_observations,
            },
        ]
    )
    path = out_dir / "T3_rq1_gap.csv"
    table.to_csv(path, index=False)
    WRITTEN.append(str(path))
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# F5 -- lambda(delta) calibration fit
# ---------------------------------------------------------------------------
def figure_f5(results: Path, out_dir: Path, calib_csv: Path | None) -> None:
    path = calib_csv
    if path is None:
        candidates = sorted(Path("data/calib").glob("*.csv")) if Path("data/calib").is_dir() else []
        path = candidates[0] if candidates else None
    if path is None or not path.exists():
        return skip("F5", "no lambda(delta) points CSV (run lob_calibrate --points ...)")

    points = pd.read_csv(path)
    if points.empty or "lambda_both" not in points:
        return skip("F5", f"{path} has no lambda_both column")

    positive = points[points["lambda_both"] > 0]
    if positive.empty:
        return skip("F5", "every lambda estimate is zero -- the window is too short")

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.semilogy(points["delta_price"], points["lambda_both"], "o", label=r"$\hat\lambda(\delta)$")
    if len(positive) >= 2:
        coeffs = np.polyfit(positive["delta_price"], np.log(positive["lambda_both"]), 1)
        k = -coeffs[0]
        a = float(np.exp(coeffs[1]))
        xs = np.linspace(points["delta_price"].min(), points["delta_price"].max(), 200)
        ax.semilogy(xs, a * np.exp(-k * xs), "-",
                    label=f"fit: A={a:.4g}, k={k:.4g}" + r" $\mathrm{price}^{-1}$")
    ax.set_xlabel(r"$\delta$ (price units from the mid)")
    ax.set_ylabel(r"$\lambda(\delta)$ (executions / s)")
    ax.set_title(r"F5  Fill-intensity calibration $\lambda(\delta)=Ae^{-k\delta}$")
    ax.legend()
    ax.grid(alpha=0.3, which="both")
    save(fig, out_dir, "F5_lambda_fit.png")
    print("  If the tail bends, fit the near region you actually quote in and SAY SO (§3.3).")


# ---------------------------------------------------------------------------
# F6 -- inventory paths
# ---------------------------------------------------------------------------
def figure_f6(results: Path, out_dir: Path) -> None:
    fills = load_concat(results, "fills.csv")
    if fills is None:
        return skip("F6", "no fills.csv found")
    wanted = [s for s in ("S1", "S2_AS", "S2_GLFT") if s in set(fills["strategy"])]
    if not wanted:
        return skip("F6", "need S1 and at least one S2 run to contrast the skew")

    fig, ax = plt.subplots(figsize=(9, 5))
    for strategy in wanted:
        sub = fills[fills["strategy"] == strategy].sort_values("ts_us")
        if sub.empty:
            continue
        signed = np.where(sub["side"] == "BID", sub["qty_lots"], -sub["qty_lots"])
        t = (sub["ts_us"] - sub["ts_us"].iloc[0]) / 1e6
        ax.plot(t, np.cumsum(signed), label=strategy, linewidth=1.2)
    ax.axhline(0.0, color="black", linewidth=0.8, linestyle="--")
    ax.set_xlabel("time since the first fill (s)")
    ax.set_ylabel("inventory (lots)")
    ax.set_title("F6  Inventory paths: does the A-S skew tame q?")
    ax.legend()
    ax.grid(alpha=0.3)
    save(fig, out_dir, "F6_inventory_paths.png")


# ---------------------------------------------------------------------------
# Matrix summary table
# ---------------------------------------------------------------------------
def table_matrix_summary(results: Path, out_dir: Path) -> None:
    daily = load_concat(results, "pnl_daily.csv")
    if daily is None:
        return skip("T4", "no pnl_daily.csv found")

    # The identity assertion, checked once more at the analysis boundary. If a
    # residual is non-zero here, the CSVs are not trustworthy and nothing
    # downstream of them should be published.
    if "identity_residual_x2" in daily:
        bad = daily[daily["identity_residual_x2"] != 0]
        if not bad.empty:
            print(f"  FATAL: {len(bad)} day(s) violate the PnL decomposition identity.")
            print("  These results must not be published. Re-run and fix the ledger first.")

    rows = []
    group_keys = ["strategy", "queue_assumption", "maker_fee_bp", "latency_in_ms"]
    for keys, group in daily.groupby(group_keys):
        ci = bootstrap_statistic(group["total"].tolist(), block_size=DEFAULT_BLOCK_DAYS)
        rows.append(
            {
                **dict(zip(group_keys, keys)),
                "days": ci.n_observations,
                "mean_daily_pnl": ci.point,
                "ci_lower": ci.lower,
                "ci_upper": ci.upper,
                "total_pnl": group["total"].sum(),
                "fills": group["fills"].sum(),
                "quotes_placed": group["quotes_placed"].sum(),
                "adverse_selection_10s_bp": group["adverse_selection_10s_bp"].mean(),
            }
        )
    table = pd.DataFrame(rows).sort_values(group_keys)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "T4_matrix_summary.csv"
    table.to_csv(path, index=False)
    WRITTEN.append(str(path))
    print(f"  wrote {path}  ({len(table)} cells, block-bootstrap CIs)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", default="data/results", help="root of the run outputs")
    parser.add_argument("--out", default="data/figures", help="where to write figures and tables")
    parser.add_argument("--calib", default=None, help="lambda(delta) points CSV for F5")
    args = parser.parse_args()

    results = Path(args.results)
    out_dir = Path(args.out)
    if not results.is_dir():
        print(f"analysis: {results} does not exist. Run lob_replay or lob_sweep first.",
              file=sys.stderr)
        return 1

    print(f"reading run outputs from {results}\n")
    figure_f1(results, out_dir)
    figure_f2(results, out_dir)
    figure_f3(results, out_dir)
    figure_f4(results, out_dir)
    figure_f5(results, out_dir, Path(args.calib) if args.calib else None)
    figure_f6(results, out_dir)
    table_matrix_summary(results, out_dir)

    print(f"\n{len(WRITTEN)} artefact(s) written to {out_dir}")
    if SKIPPED:
        print(f"{len(SKIPPED)} skipped because their inputs do not exist yet:")
        for entry in SKIPPED:
            print(f"  - {entry}")
        print("\nThis is the correct behaviour. Nothing is generated without measured data.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
