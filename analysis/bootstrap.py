"""Stationary block bootstrap for daily PnL (master plan §3.10).

Why blocks and not plain resampling: daily PnL is serially dependent -- a
volatile week is volatile on Monday and Tuesday both -- and an i.i.d. bootstrap
would treat each day as independent and report a confidence interval that is far
too narrow. Resampling in blocks of about three days preserves the local
dependence structure.

Why daily and not per-minute: intraday autocorrelation makes a per-minute Sharpe
fantasy (Part 11 pitfall #8). Aggregate to daily, annualise with sqrt(365), and
say in the same breath how many days the sample has.

Nothing here invents data. Every function takes an array of *measured* daily
PnL and returns statistics about it.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Sequence

import numpy as np

DEFAULT_BLOCK_DAYS = 3
DEFAULT_RESAMPLES = 10_000
TRADING_DAYS_PER_YEAR = 365  # crypto trades every day; do not use 252 here


@dataclass(frozen=True)
class BootstrapResult:
    point: float
    lower: float
    upper: float
    stderr: float
    resamples: int
    block_size: int
    n_observations: int

    def __str__(self) -> str:
        return (
            f"{self.point:.6g}  [{self.lower:.6g}, {self.upper:.6g}]  "
            f"(n={self.n_observations}, block={self.block_size}, B={self.resamples})"
        )


def stationary_block_indices(
    n: int, block_size: int, rng: np.random.Generator
) -> np.ndarray:
    """Politis-Romano stationary bootstrap indices.

    Block lengths are geometric with mean `block_size`, which makes the
    resampled series stationary -- unlike fixed-length blocks, which impose an
    artificial period.
    """
    if n <= 0:
        return np.empty(0, dtype=np.int64)
    p = 1.0 / max(1, block_size)
    out = np.empty(n, dtype=np.int64)
    idx = int(rng.integers(0, n))
    for i in range(n):
        out[i] = idx
        if rng.random() < p:
            idx = int(rng.integers(0, n))
        else:
            idx = (idx + 1) % n
    return out


def bootstrap_statistic(
    values: Sequence[float],
    statistic=np.mean,
    *,
    block_size: int = DEFAULT_BLOCK_DAYS,
    resamples: int = DEFAULT_RESAMPLES,
    alpha: float = 0.05,
    seed: int = 42,
) -> BootstrapResult:
    """Percentile confidence interval for `statistic` under the block bootstrap."""
    arr = np.asarray(list(values), dtype=float)
    arr = arr[np.isfinite(arr)]
    n = arr.size
    if n == 0:
        return BootstrapResult(math.nan, math.nan, math.nan, math.nan, 0, block_size, 0)
    if n == 1:
        v = float(statistic(arr))
        return BootstrapResult(v, v, v, 0.0, 0, block_size, 1)

    rng = np.random.default_rng(seed)
    draws = np.empty(resamples, dtype=float)
    for b in range(resamples):
        draws[b] = statistic(arr[stationary_block_indices(n, block_size, rng)])

    return BootstrapResult(
        point=float(statistic(arr)),
        lower=float(np.quantile(draws, alpha / 2.0)),
        upper=float(np.quantile(draws, 1.0 - alpha / 2.0)),
        stderr=float(np.std(draws, ddof=1)),
        resamples=resamples,
        block_size=block_size,
        n_observations=n,
    )


def annualised_sharpe(daily_pnl: Sequence[float]) -> float:
    """Sharpe from DAILY PnL, annualised with sqrt(365).

    Quote this only alongside the sample size and the bootstrap interval. A
    60-day sample bounds how seriously any Sharpe can be taken, and the honest
    answer to "do you believe it?" starts with that fact (Part 12).
    """
    arr = np.asarray(list(daily_pnl), dtype=float)
    arr = arr[np.isfinite(arr)]
    if arr.size < 2:
        return math.nan
    sd = float(np.std(arr, ddof=1))
    if sd == 0.0:
        return math.nan
    return float(np.mean(arr)) / sd * math.sqrt(TRADING_DAYS_PER_YEAR)


def bootstrap_sharpe(
    daily_pnl: Sequence[float],
    *,
    block_size: int = DEFAULT_BLOCK_DAYS,
    resamples: int = DEFAULT_RESAMPLES,
    alpha: float = 0.05,
    seed: int = 42,
) -> BootstrapResult:
    def stat(sample: np.ndarray) -> float:
        sd = float(np.std(sample, ddof=1))
        if sd == 0.0:
            return math.nan
        return float(np.mean(sample)) / sd * math.sqrt(TRADING_DAYS_PER_YEAR)

    return bootstrap_statistic(
        daily_pnl, stat, block_size=block_size, resamples=resamples, alpha=alpha, seed=seed
    )


def difference_interval(
    a: Sequence[float],
    b: Sequence[float],
    *,
    block_size: int = DEFAULT_BLOCK_DAYS,
    resamples: int = DEFAULT_RESAMPLES,
    alpha: float = 0.05,
    seed: int = 42,
) -> BootstrapResult:
    """Paired interval for mean(a) - mean(b) over the same days.

    Used for RQ1 (touch vs queue-aware) and RQ5 (S3 vs S2). Both series come
    from the SAME days and the same data, so they must be resampled with the
    same block indices -- resampling them independently would throw away the
    pairing and inflate the interval.
    """
    xa = np.asarray(list(a), dtype=float)
    xb = np.asarray(list(b), dtype=float)
    if xa.size != xb.size:
        raise ValueError(f"paired series must be the same length ({xa.size} vs {xb.size})")
    mask = np.isfinite(xa) & np.isfinite(xb)
    xa, xb = xa[mask], xb[mask]
    n = xa.size
    if n == 0:
        return BootstrapResult(math.nan, math.nan, math.nan, math.nan, 0, block_size, 0)

    rng = np.random.default_rng(seed)
    draws = np.empty(resamples, dtype=float)
    for i in range(resamples):
        idx = stationary_block_indices(n, block_size, rng)
        draws[i] = float(np.mean(xa[idx]) - np.mean(xb[idx]))

    return BootstrapResult(
        point=float(np.mean(xa) - np.mean(xb)),
        lower=float(np.quantile(draws, alpha / 2.0)),
        upper=float(np.quantile(draws, 1.0 - alpha / 2.0)),
        stderr=float(np.std(draws, ddof=1)),
        resamples=resamples,
        block_size=block_size,
        n_observations=n,
    )


def break_even_fee_bp(fees_bp: Sequence[float], total_pnl: Sequence[float]) -> float:
    """Linear interpolation of the fee at which total PnL crosses zero (RQ4).

    Returns NaN when the curve never crosses inside the swept range -- which is
    itself a result, and must be reported as "never breaks even below X bp"
    rather than extrapolated into a number that was never measured.
    """
    fees = np.asarray(list(fees_bp), dtype=float)
    pnl = np.asarray(list(total_pnl), dtype=float)
    order = np.argsort(fees)
    fees, pnl = fees[order], pnl[order]
    for i in range(1, fees.size):
        y0, y1 = pnl[i - 1], pnl[i]
        if (y0 > 0 >= y1) or (y0 < 0 <= y1):
            if y1 == y0:
                return float(fees[i])
            t = y0 / (y0 - y1)
            return float(fees[i - 1] + t * (fees[i] - fees[i - 1]))
    return math.nan
