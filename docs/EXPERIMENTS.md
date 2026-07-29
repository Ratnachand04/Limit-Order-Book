# Pre-registration

**Status: NOT YET FROZEN.** No data has been recorded and no calibration has
been run, so the parameter values below are still placeholders. This document
becomes binding the moment the calibration window closes — see *Freezing
procedure*.

---

## Why this file exists

Master plan §3.10: *"before touching the out-of-sample window, commit
`EXPERIMENTS.md` — the exact strategy configs, parameter values (frozen from
calibration), metrics, and the full experiment matrix — so nobody, including
you, can accuse you of tuning on the test set."*

Pre-registration is multiple-testing discipline. Without it, a project with 324
matrix cells and three queue assumptions will find *something* that looks good,
and neither the author nor a reader can tell whether it was a finding or a
search. The commit hash of this file, dated before the first evaluation run, is
the proof.

---

## Data plan

| | |
|---|---|
| Venue | Binance USD-M perpetual futures (public market data only) |
| Streams | `@depth@100ms`, `@aggTrade`, `@bookTicker`, REST `/depth` every 30 min |
| Symbols | *TBD — see below* |
| Calibration window | weeks 1–3 of recording |
| Evaluation window | weeks 4–6+ of recording |

### Symbols — **to be fixed in week 0, before any results exist**

Run `python recorder/symbol_scout.py --symbols ... --minutes 20`, then fill in:

| Role | Symbol | Median spread (ticks) | Median spread (bp) | Chosen on |
|---|---|---|---|---|
| Tight major | BTCUSDT | | | |
| Tight major | ETHUSDT | | | |
| Wider mid-cap | *TBD* | | | |
| Wider mid-cap | *TBD* | | | |

Selection rule, fixed in advance: keep both tight majors regardless of how they
look (they are the degenerate-economics case the paper explains), and add two
instruments whose median spread is **≥ 3–5 ticks and ≥ 2 bp**.

Part 11 pitfall #12: once chosen, symbols are **kept**, including the boring
ones. Dropping a symbol after seeing its results is survivorship bias.

---

## Frozen parameters

*(Empty until calibration runs. Every cell must be filled from a calibration
artefact, with the artefact's path recorded.)*

### λ(δ) = A·e^(−kδ) — §3.3

| Symbol | A | k (price⁻¹) | R² | Fit depth (ticks) | Calibration file |
|---|---|---|---|---|---|
| | | | | | |

Produced by `lob_calibrate`. Report both the near-region fit (the region
actually quoted in) and the full-range fit. **If the tail bends, say so** — the
strategies use the near-region fit and the paper shows both.

### σ estimator — §3.2

| Parameter | Value | Rationale |
|---|---|---|
| Sampling interval Δ | 1 s | faster picks up bid-ask bounce; slower loses reactivity |
| Rolling window | 600 s | |

Both fixed in advance and not swept.

### γ selection — §3.4

Candidates `{1e-6, 1e-5, 1e-4}`, selected on **calibration data only**, by
mean daily PnL under PESS at 2 bp maker fee.

| Symbol | Selected γ | Selected on | Never re-selected after |
|---|---|---|---|
| | | | |

### Microprice — §3.6, gate for S3

Before S3 is run on the evaluation window, regress `m_{t+h} − m_t` on
`(m_w − m)_t` for h ∈ {1, 5, 10} s on calibration data:

| Symbol | h = 1 s R² | h = 5 s R² | h = 10 s R² | IC |
|---|---|---|---|---|
| | | | | |

**Pre-committed decision rule:** S3 is still run and reported whatever these
show. If the microprice has no predictive content on an instrument, that is a
finding about the instrument, and RQ5's answer for it is "no measurable
improvement". A negative result is reported with the same prominence as a
positive one.

### S3 toxicity pull

| Parameter | Value |
|---|---|
| Flow window | 2.0 s |
| Threshold (\|signed\| / total) | 0.75 |
| Pull duration | 1.0 s |

Fixed in advance, not tuned.

---

## The matrix

```
{PESS, OPT, PROP}
× {S0_touch, S0_queue, S1, S2_AS, S2_GLFT, S3}
× {maker fee bp: −0.5, 0, 1, 2, 5, 10}
× {latency ms: 5, 50, 200}
× {4 symbols}
```

= **1296 cells**, run once, by `lob_sweep --matrix configs/matrix.yaml`.

Fixed for every cell: seed 42, `q_max` 5, order size 1 lot,
`requote_min_ticks` 2, timer 100 ms.

---

## Primary metrics

Declared in advance. No metric is added, removed or redefined after results are
seen.

1. Fill rate — fills per quote-hour, and fills per placement
2. Mean edge at fill (bp of notional)
3. Adverse selection at 1 s and 10 s (bp)
4. Net markout (bp)
5. Daily PnL decomposition: spread capture / inventory / fees / total
6. Break-even maker fee (bp), per strategy and symbol
7. Touch-vs-queue gap ratios (RQ1): fill count ratio and daily PnL difference

**Primary endpoint for RQ1:** the ratio of fill counts and the paired daily-PnL
difference between `S0_touch` and `S0_queue`, PESS, 2 bp, 50 ms — one cell,
declared now.

### Statistics

- Aggregate to **daily** PnL. Never per-minute (Part 11 pitfall #8).
- Stationary block bootstrap, mean block 3 days, 10,000 resamples, 95%
  percentile intervals.
- Paired comparisons (RQ1, RQ5) resample both series with the **same** block
  indices — they come from the same days and the pairing is information.
- Any Sharpe: from daily PnL, annualised with √365, always quoted with the
  sample size and the interval.
- Secondary splits, also declared in advance: Asian / EU / US session, and
  top-vs-bottom volatility terciles.

---

## Figures

| | |
|---|---|
| F1 | markout curves by strategy |
| F2 | fill-probability heatmap: depth × queue fraction (RQ2) |
| F3 | PnL decomposition across the fee grid (RQ4) |
| F4 | touch vs queue-aware fill counts and PnL (RQ1) |
| F5 | λ(δ) calibration fit |
| F6 | inventory paths, S1 vs S2 |

All produced by `analysis/make_figures.py` from committed CSVs only.

---

## Validation gates — checked before any result is reported

A run that fails any of these is not evidence.

- [ ] PnL decomposition residual is **exactly 0** for every day of every cell
- [ ] `DualBook` reports zero mismatches on a full replay
- [ ] Two independent processes produce byte-identical CSVs
- [ ] Probe fill probability **falls** with depth (§3.8)
- [ ] Probe fill probability **falls** as queue-fraction-ahead rises
- [ ] PESS ≤ PROP ≤ OPT fill counts in every cell
- [ ] Dirty intervals excluded from analytics; the excluded fraction reported
- [ ] Trade-attribution residual reported, not assumed away

---

## Freezing procedure

1. Close the calibration window. Record the last calibration date here.
2. Run `lob_calibrate` on the calibration days; commit the JSON to `data/calib/`
   and record its path and hash in this file.
3. Select γ on calibration data. Record it here.
4. Run the microprice regression. Record R² and IC here.
5. Fill in every empty cell above.
6. **Commit this file.** Note the commit hash in the paper's reproducibility
   appendix.
7. Only then run the evaluation matrix.

After step 6, no parameter in this document changes. If something is discovered
that would have changed a choice, it goes in the paper's discussion as a
limitation — it does not go back and change the pre-registration.

## Honesty policy

- All configs pre-registered before the evaluation runs.
- No metric deleted after the fact.
- Negative results reported with the same prominence as positive ones. At retail
  fee tiers the honest expected outcome is that passive market making on tight
  majors loses money, and *that finding, properly measured and explained, is the
  paper*.
- Every figure regenerable by one script from the committed CSVs.
- Every number traceable to a run ID in `data/results/matrix/index.csv`.
