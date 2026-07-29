# Socratic — Phase 7: experiments and the paper

**1. The matrix has 1296 cells. Explain why that is scientifically *better* than
reporting one well-chosen configuration, and then explain the danger it creates
and what `docs/EXPERIMENTS.md` does about it.**
Look at: `configs/matrix.yaml`, master plan Part 7 and §3.10.
Probe: what exactly does a pre-registration commit hash prove?

**2. Why is the block bootstrap used instead of an ordinary one, and why are RQ1
and RQ5 resampled with *shared* block indices?**
Look at: `analysis/bootstrap.py::difference_interval`.
Probe: what property of daily PnL makes i.i.d. resampling wrong, and which way
does the error go?

**3. `make_figures.py` skips a figure when its input CSV is absent, rather than
producing an empty or illustrative one. Argue that this is the right behaviour
even though it makes a fresh clone produce nothing.**
Look at: `SKIPPED` handling in `analysis/make_figures.py`, CLAUDE.md rule 4.

**4. `break_even_fee_bp` returns NaN when the PnL curve never crosses zero inside
the swept fee range. What is the correct sentence to write in the paper for such
a cell, and what would be wrong with extrapolating?**
Look at: `analysis/bootstrap.py::break_even_fee_bp`, RQ4.

**5. Suppose the evaluation runs show that every strategy loses money at every
realistic fee level. Write the abstract's one-sentence framing of that result.**
Look at: master plan's opening rule, and the README's "Assumptions &
limitations".
Probe: what makes an honest negative result *more* valuable in an interview than
a positive one you cannot fully defend?

---

**Bonus:** pick any number you intend to put on your CV and trace it, out loud,
from the CV line back through the table, the CSV, the run ID, the config, the
commit hash, and the raw data file. If any link in that chain is missing, fix it
before the number goes anywhere.
