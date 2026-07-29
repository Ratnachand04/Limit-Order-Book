# Phase 0 — the recorder

> **Non-negotiable, from the master plan Part 9:** *"the recorder goes live
> before anything else. Every day un-recorded is a day of evaluation data you
> can never recover."*

The recorder is the only part of this project with a deadline that cannot be
moved. The C++ simulator can be written in November; the market data for
September can only be recorded in September.

## What it records

Per symbol, into hourly-rotated gzip JSONL:

| Stream | Why |
|---|---|
| `<symbol>@depth@100ms` | the diff-depth updates the book is rebuilt from |
| `<symbol>@aggTrade` | trades with the aggressor-side flag — without it a trade cannot fill a queue-tracked order (§3.7) |
| `<symbol>@bookTicker` | BBO, kept only to cross-check the reconstructed book |
| REST `/depth` every 30 min | resync anchors for the §2.4 sync protocol |

Every line is `{"t": <local recv µs>, "c": <channel>, "s": <symbol>, "d": <verbatim payload>}`.
The exchange payload is stored **unmodified** — the recorded stream is the
single source of truth (§4.1), and a recorder that reinterprets a field makes
every downstream number unverifiable.

## No trading, ever

Public market-data websockets and public REST depth only. No API keys, no
signed requests, no order endpoints, no account or withdrawal endpoints. This is
a research simulator (CLAUDE.md rule 5).

## Start it

```bash
python -m pip install -r recorder/requirements.txt

# 1. Choose the symbols from observed spreads, once, in week 0.
python recorder/symbol_scout.py --symbols BTCUSDT ETHUSDT SOLUSDT ARBUSDT --minutes 20
#    Edit recorder/symbols.yaml with the answer and COMMIT it.

# 2. Go live.
python recorder/recorder.py --config recorder/symbols.yaml
```

On Windows, use the working interpreter explicitly if `python` on PATH is a
broken install:

```powershell
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" recorder\recorder.py --config recorder\symbols.yaml
```

## Keep it alive

```bash
# every 5 minutes from cron
*/5 * * * * cd /srv/lob && python recorder/watchdog.py \
    --out data/raw --symbols BTCUSDT ETHUSDT SOLUSDT ARBUSDT \
    --restart-cmd "python recorder/recorder.py --config recorder/symbols.yaml" \
    --log logs/recorder.log >> logs/watchdog.log 2>&1
```

The watchdog restarts on **silence**, not only on death: a recorder whose socket
is open but which has written nothing for ten minutes is a dead recorder that
has not noticed yet.

### systemd

```ini
# /etc/systemd/system/lob-recorder.service
[Unit]
Description=LOB market data recorder
After=network-online.target

[Service]
Type=simple
WorkingDirectory=/srv/lob
ExecStart=/usr/bin/python3 recorder/recorder.py --config recorder/symbols.yaml
Restart=always
RestartSec=5
StandardOutput=append:/srv/lob/logs/recorder.log
StandardError=append:/srv/lob/logs/recorder.log

[Install]
WantedBy=multi-user.target
```

### Windows Task Scheduler

Trigger *At startup* and *Every 5 minutes*, action:

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "cd D:\MONEY\LOB; & $env:LOCALAPPDATA\Programs\Python\Python312\python.exe recorder\watchdog.py --out data\raw --symbols BTCUSDT ETHUSDT --restart-cmd 'python recorder\recorder.py --config recorder\symbols.yaml'"
```

## Restart procedure (part of the Phase 0 gate)

1. `python recorder/disk_report.py --out data/raw` — note the last recorded hour.
2. Stop the old process (Ctrl-C, `systemctl stop lob-recorder`, or kill the PID).
3. Restart with the same command. Files are opened in **append** mode, so a
   restart inside an hour continues the existing file rather than truncating it.
4. Re-run `disk_report.py` and confirm the current hour is growing again.
5. Every restart is a discontinuity by definition. The recorder writes a
   `reconnect` entry to `data/raw/gaps.jsonl`; the converter turns the interval
   after it into dirty events, and the analytics excludes them (§4.4). Nothing
   needs to be done by hand — but check the gap count did not jump unexpectedly.

## Check on it

```bash
python recorder/disk_report.py --out data/raw --json data/raw/report.json
```

Reports size per symbol per day, **missing hours**, suspiciously empty hours,
the sequence-gap tally by symbol and reason, and days of disk runway at the
observed rate. §4.3 guesses 1–3 GB/day/symbol compressed and says to check after
day one; this is how you check.

## Phase 0 acceptance gate

- [ ] 24 h unattended run completed
- [ ] `disk_report.py` shows no missing hours over that window
- [ ] gap report generated and reviewed
- [ ] restart procedure above executed once, deliberately, and it worked
- [ ] symbol choice committed to `recorder/symbols.yaml` **before** any results exist
