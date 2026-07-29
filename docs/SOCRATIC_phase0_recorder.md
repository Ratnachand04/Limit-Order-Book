# Socratic — Phase 0: the recorder

Answer these in writing before moving on. If an answer needs the code open, that
is fine; if it needs the code open *and* still comes out vague, that is the
signal to reread §2.4 and §4.3.

**1. Why is the recorder in Python when the rest of the project is C++, and what
is the one-sentence answer if an interviewer implies it is a cop-out?**
Look at: `recorder/recorder.py` module docstring, master plan §4.3.
Probe: what would have been lost by spending three days on a C++ recorder first?

**2. The recorder stores the exchange payload verbatim under `"d"`, and adds
only a local receive timestamp. Why is reinterpreting even one field — say,
converting prices to floats at record time — a mistake that cannot be undone?**
Look at: `RotatingJsonlWriter.write`, master plan §4.1 ("event-sourced").
Probe: which downstream claim stops being verifiable?

**3. Files are opened in *append* mode and rotate on the event's UTC hour, not on
elapsed time. Give a concrete failure that the append-and-hour-stamp choice
prevents.**
Look at: `RotatingJsonlWriter._open`, `hour_stamp`.
Probe: what happens if the recorder restarts 40 minutes into an hour?

**4. The recorder logs sequence gaps but never tries to repair them. Where does
repair happen instead, and why is that separation worth the extra moving part?**
Look at: `GapLog` docstring, `check_sequence`, `converter/src/converter.cpp`.
Probe: if the recorder patched gaps, what would the raw file no longer be?

**5. The watchdog restarts on *silence*, not only on process death. Describe the
failure mode that a liveness-only check would miss, and say how long this project
can tolerate it before data is lost.**
Look at: `recorder/watchdog.py`, `DEFAULT_STALE_SECONDS`.
Probe: what is the recovery cost of an hour of silence, versus a week of it?

---

**Bonus, and the one that actually matters:** what is the disk-per-day figure you
measured after day one, and how many days of runway does it give you? (§4.3
guesses 1–3 GB/day/symbol and says to check. `disk_report.py` is how.) If you
cannot answer this from a real number, the recorder has not run long enough.
