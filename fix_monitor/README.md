# fixmon

A FIX session monitor fed by QuickFIX logs and by the engine's own
configuration. Two log adapters, one normalised event stream, Prometheus
metrics and a SQLite event store.

No sniffing, no proxy, nothing in the order path. It asks for read access and
nothing else — which is why there is no TLS problem, no root requirement, and
why it behaves the same on Linux and Windows.

## Why two adapters

| Source | What it gives you |
|---|---|
| `*.messages.log` | Application layer: message flow, rejects, sequence tracking, order states |
| `*.event.log` | Session layer: the **reason** for a disconnect, seq mismatch detection, heartbeat timeouts, reconnects |

The message log tells you the flow stopped. The event log tells you **why** it
stopped. Correlating the two on `session_id` plus time is the actual job of
this project.

## Architecture

```
  engine .cfg  ──> QuickFixSettings ──> session list + log paths
                     (credentials are dropped here)
                               |
                               v
  *.messages.log ──> MessageLogAdapter ─┐
                                        ├─> MPMC queue ─> pipeline ─┬─> SessionRegistry
  *.event.log    ──> EventLogAdapter  ──┘   (bounded)  (one consumer)├─> MetricRegistry ─> /metrics
                                                                     └─> EventStore (SQLite)
```

Each adapter tails in its own thread and pushes events into a shared bounded
Vyukov MPMC queue. A single consumer thread advances the state machine,
increments metrics and writes batches to the store.

The queue is bounded on purpose: if the consumer falls behind it drops rather
than grows, and counts what it dropped. An unbounded buffer is not acceptable
on a machine that is also running a FIX engine.

### Two data paths

- **Prometheus** — aggregates only. Labels are limited to `session`,
  `direction`, `msg_type` and `reason`. ClOrdID, symbol and account ID never
  reach a metric. The registry enforces a fixed series limit, so a bad label
  cannot take the scrape target down.
- **SQLite** — full fidelity. Every message and every session event, together
  with its raw line. Evidence lives here; dashboards live in Prometheus.

### Adding a new source

Implement the `ISourceAdapter` interface and emit the same `Event` struct. The
schema is already shaped for pcap: the `capture_ts_ns` and `source` fields are
there, and `capture_ts_ns` stays null for log sources. Adding a pcap adapter
changes none of the layers below it.

## Session discovery: the engine's cfg is the source

The engine already knows which sessions it runs and where it writes their logs.
Restating that in a second file is how the two drift apart: a session gets added
to the engine, the monitor is not updated, and the monitor keeps reporting green
for something it is not watching. So the session list is read from the engine's
own cfg.

```bash
# the engine cfg is the only source, no fixmon.ini at all
./build/fixmon --quickfix /etc/quickfix/session.cfg

# or from the [global] block of fixmon.ini (repeatable, also accepts a comma list)
#   quickfix_config = /etc/quickfix/session.cfg

# see what will be watched before starting anything
./build/fixmon fixmon.ini --print-config
```

What gets taken: `BeginString`, the comp IDs, `SessionQualifier`, `HeartBtInt`,
`ConnectionType`, `StartTime`/`EndTime`, `FileLogPath` and the log paths
resolved underneath it. `HeartBtInt` feeds the staleness threshold directly —
the monitor's threshold is now the engine's real heartbeat rather than a number
kept in sync by hand.

The cfg is opened read-only and never written back. Key comparison is
case-insensitive: silently losing `FileLogPath` to a capitalisation mismatch
would be the worst mistake this reader could make.

### Finding the log file

QuickFIX C++ writes `FIX.4.4-BROKER1-VENUEX.messages.current.log`, quickfix/j
ends in `.messages.log`, and some deployments drop the BeginString entirely.
Rather than assume one name, the candidates are tried in order:

1. Candidate prefixes x `.current.log` / `.log` — does it exist on disk?
2. If not, scan the directory; split file names on `-` into tokens and match
   comp IDs **exactly** (not as substrings, so a `VENUEX2` file does not answer
   for `VENUEX`).
3. If half of the pair was found, derive the other from its name.
4. If nothing matches, watch the canonical name and say so — the tailer opens
   the file the moment it appears.

If `FileLogPath` is relative it is resolved against the working directory first,
then against the cfg file's own directory. For acceptors with `TargetCompID=*`,
the counterparties the engine actually writes logs for are read from the
directory and each becomes its own session.

If `FileLogPath` is absent entirely (file logging disabled), the session is
still registered but `fixmon_session_log_sources` stays 0. "Configured but
reading nothing" is the one failure where every other metric looks perfectly
healthy.

The `[session]` block is now only needed for what the engine cfg cannot express,
or expresses wrongly. When a session appears in both, the field the operator
wrote wins, and every field they did not write is filled from the engine.

### Credentials

`Password`, `Username`, `SSLPrivateKeyPassword` and any setting whose name has
that shape are dropped **at parse time**: the value never reaches a variable, a
metric, the event store or a log line. Only the key name is kept, so startup can
state that they were ignored deliberately:

```
config: imported FIX.4.4:BROKER1->VENUEX from quickfix.cfg
session: FIX.4.4:BROKER1->VENUEX  hb=30s  initiator  <- quickfix.cfg
    messages: ./logs/BROKER1-VENUEX.messages.log
    events  : ./logs/BROKER1-VENUEX.event.log
    schedule: 00:00:00 - 00:00:00
    credentials ignored: Username=<redacted> Password=<redacted>
```

The rule lives in one place (`include/fixmon/redact.hpp`) and two different
layers use the same predicate: the cfg reader drops the value, and the metric
registry refuses a **label** by the same name. Matching looks at the shape of
the name rather than a fixed list of settings, so a vendor key nobody has seen
before is caught too.

The second layer is a net for code that has not been written yet: a scrape
endpoint is usually unauthenticated and its contents flow into a TSDB and into
dashboards — the worst possible destination for a password.

`fixmon_metric_series_redacted_total` should stay 0 forever. If it moves, some
code path tried to publish a credential-bearing series and the registry stopped
it; that is a defect, not a capacity problem.

To verify against the live metrics endpoint:

```bash
tools/verify_no_secrets.sh ./build/fixmon ./quickfix.cfg
```

The script reads the real credential values out of the cfg and looks for them in
the `/metrics` and `/sessions` output. The tests assert the same thing at every
layer, but the layer an auditor looks at is the layer Prometheus reads.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/fixmon_selftest     # 100+ checks
```

Dependencies: sqlite3 and pthread. Nothing else.

### Where SQLite comes from

`cmake/ResolveSQLite3.cmake` tries four routes and produces the same
`SQLite::SQLite3` target in every case, so the rest of the project never learns
where it came from.

| `FIXMON_SQLITE_PROVIDER` | Behaviour |
|---|---|
| `auto` (default) | System first, then a local directory, then download |
| `system` | System only; if missing, print install commands and fail |
| `local` | `FIXMON_SQLITE_SOURCE_DIR` only (air-gapped environments) |
| `fetch` | Download even if the system has it |

The system is tried first because the distro package is patched and receives
security updates through the normal channel. A vendored amalgamation freezes at
whatever version you pinned, and SQLite does get CVEs. Downloading is the
convenient route, not the preferred one.

```bash
# no sqlite on the system: download and link it statically
cmake -B build

# pinning a version (sqlite.org keeps every release under its year directory)
cmake -B build -DFIXMON_SQLITE_YEAR=2024 -DFIXMON_SQLITE_VERSION_ID=3460000 \
               -DFIXMON_SQLITE_SHA256=<hash>

# air-gapped
cmake -B build -DFIXMON_SQLITE_PROVIDER=local \
               -DFIXMON_SQLITE_SOURCE_DIR=/opt/sqlite-amalgamation
```

**The default download URL has not been verified.** If you get a 404, set
`FIXMON_SQLITE_YEAR` and `FIXMON_SQLITE_VERSION_ID` to a release that actually
exists on sqlite.org, or point `FIXMON_SQLITE_URL` at a mirror you trust.

If `FIXMON_SQLITE_SHA256` is left empty the download runs with no integrity
check and CMake warns about it. Never leave it empty in a build that ships.

### Using a real package manager

CMake is not doing package management here — `FetchContent` means "download the
source and build it"; there is no dependency resolution, no version conflict
handling, no binary cache. If you need those, use vcpkg or Conan. Both work
through `find_package`, so the resolver picks them up at its `system` layer with
no CMake changes.

```bash
# vcpkg (vcpkg.json manifest is in the repo)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# conan (conanfile.txt is in the repo)
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
```

Being honest about what has actually been exercised: the Windows build was done
with MinGW g++ and Ninja using `FIXMON_SQLITE_PROVIDER=local`. The vcpkg and
Conan manifests are committed but have not been run end to end, and MSVC has not
been attempted at all. Since Windows has no apt, one of vcpkg or the download
route is what you will realistically use there — expect to shake something out
on the first try.

## Running

```bash
./build/fixmon fixmon.ini                       # ini, plus quickfix_config if present
./build/fixmon --quickfix /etc/quickfix/fix.cfg # no ini, collector defaults
./build/fixmon fixmon.ini --print-config        # resolve, print, exit
```

Discovery is never silent: every import decision and every log file that could
not be found is reported on `config:` lines before anything starts.

Endpoints:

- `GET /metrics` — Prometheus text exposition
- `GET /sessions` — session state as JSON (for debugging)
- `GET /healthz`

### Trying it with fake logs

```bash
mkdir -p logs && touch logs/BROKER1-VENUEX.{messages,event}.log
./build/fixmon --quickfix quickfix.cfg &
python3 tools/gen_logs.py --dir ./logs --duration 30
curl -s localhost:9109/metrics | grep -v '^#'
```

The generator injects real failure scenarios: a sequence gap plus resend, a
session-level reject, a rejected ExecutionReport, a business reject, a heartbeat
timeout, a disconnect with a reason followed by a reconnect, seq-too-low, and a
line no rule recognises.

For the full stack with Prometheus and Grafana on top, see `DEMO.md`.

## Configuration

```ini
[global]
db_path          = ./fixmon.db
metrics_port     = 9109
queue_size       = 65536
batch_size       = 500
batch_flush_ms   = 200
poll_interval_ms = 100
from_beginning   = false
stale_after_multiple = 2
snapshot_interval_s  = 30

# Take sessions from the engine's own cfg. Repeatable; a relative path is
# resolved against the directory holding this ini file.
quickfix_config = ./quickfix.cfg

# Only for what the engine cfg cannot express, or expresses wrongly.
[session]
begin_string       = FIX.4.4
sender_comp_id     = BROKER1
target_comp_id     = VENUEX
message_log        = ./logs/BROKER1-VENUEX.messages.log
event_log          = ./logs/BROKER1-VENUEX.event.log
heartbeat_interval = 30
```

One block per session. Either log path may be left empty; that session simply
runs one adapter short. `session_qualifier`, `sender_sub_id` and
`target_sub_id` can also be given — needed when the engine builds its log file
names out of them.

## Metrics

**Application layer**

| Metric | Labels |
|---|---|
| `fixmon_messages_total` | session, direction, msg_type, msg_type_name |
| `fixmon_session_rejects_total` | session, reason, ref_tag |
| `fixmon_business_rejects_total` | session, reason |
| `fixmon_exec_rejects_total` | session, reason |

**Session layer**

| Metric | Labels |
|---|---|
| `fixmon_session_events_total` | session, type |
| `fixmon_seq_gaps_total` | session, **src** |
| `fixmon_seq_gap_messages_total` | session, **src** |
| `fixmon_seq_too_low_total` | session |
| `fixmon_resend_requests_total` | session, **src** |
| `fixmon_heartbeat_timeouts_total` | session |
| `fixmon_disconnects_total` | session |
| `fixmon_reconnect_attempts_total` | session |
| `fixmon_logon_rejects_total` | session |

The `src` label matters: the same gap is both derived from the message flow and
described in the event log. Adding them would be double counting, so they are
split by source. And when the two disagree, that disagreement is itself a
finding.

**State**

`fixmon_session_up`, `fixmon_session_state` (1 disconnected, 2 connecting,
3 logon_pending, 4 logged_on, 5 stale), `fixmon_last_message_age_seconds`,
`fixmon_next_expected_seq_num`, `fixmon_last_outgoing_seq_num`

**Configuration**

| Metric | Label / meaning |
|---|---|
| `fixmon_session_info` | session, begin_string, connection_type, source — always 1, for joins |
| `fixmon_session_log_sources` | session — attached adapter count; 0 = nothing is being read |
| `fixmon_session_config_redacted` | session — **how many** credentials were found in the cfg and dropped |

The labels on `fixmon_session_info` were chosen by hand: identity and transport
role. Host, port, store path and credentials are deliberately absent; this
series is joined to the others on `session` and does not need more.

```
fixmon_session_info{session="FIX.4.4:BROKER1->VENUEX",begin_string="FIX.4.4",connection_type="initiator",source="quickfix_config"} 1
fixmon_session_log_sources{session="FIX.4.4:BROKER1->VENUEX"} 2
fixmon_session_config_redacted{session="FIX.4.4:BROKER1->VENUEX"} 2
```

**The collector itself**

`fixmon_events_dropped_total`, `fixmon_queue_depth`,
`fixmon_store_rows_written_total`, `fixmon_store_write_errors_total`,
`fixmon_metric_series`, `fixmon_metric_series_rejected_total`,
`fixmon_metric_series_redacted_total`

## Event store

WAL plus `synchronous=NORMAL`, with batches inside an explicit transaction.
One-row-at-a-time inserts do not survive this workload.

Two tables:

- `events` — every message and every session event, with the raw line in the
  `raw` column. Indexes are on `(session_id, ts_ns)`, matching the query shape.
- `session_snapshots` — session state every 30 seconds, so "what did this
  session look like at 14:32" can be answered without replaying events from the
  beginning.

An incident timeline query:

```sql
SELECT datetime(ts_ns/1000000000,'unixepoch') AS t,
       source,
       COALESCE(msg_type_name, session_event) AS what,
       text
FROM events
WHERE session_id = 'FIX.4.4:BROKER1->VENUEX'
  AND ts_ns BETWEEN ? AND ?
  AND (session_event IN ('disconnected','heartbeat_timeout',
                         'seq_num_too_high','seq_num_too_low')
       OR msg_type IN ('3','j')
       OR (msg_type='8' AND ord_status='8'))
ORDER BY ts_ns;
```

The output looks like this:

```
t                    source       what                   text
2026-09-04 20:29:10  event_log    seq_num_too_high       MsgSeqNum too high, expecting 67 but received 73
2026-09-04 20:29:14  message_log  Reject                 Required tag missing
2026-09-04 20:29:17  message_log  ExecutionReport        Instrument not tradable at this time
2026-09-04 20:29:21  event_log    heartbeat_timeout      Test Request timed out
2026-09-04 20:29:23  event_log    disconnected           Socket exception, connection reset by peer
```

That merge of two sources onto one timeline is exactly the point.

## Grafana and alerts

`grafana/fixmon-sessions.json` ("FIX Sessions") — session state, time since last
message, message rates, reject breakdown, sequence health, collector health.

`grafana/fixmon-config.json` ("FIX Session Discovery") — what was discovered from
the engine cfg: sessions, how many log sources each one attached, and how many
credentials were dropped.

`grafana/alerts.yml` — session down, stale, flapping, seq-too-low, rising reject
rate, heartbeat timeout, plus the collector's own health. The thresholds are
starting values and should be tuned per counterparty.

## Known limits

What a log source structurally cannot see:

- TCP-level health (retransmits, zero-window, RTT)
- Wire latency — only the engine's own timestamp exists, `capture_ts_ns` is empty
- Anything that never reached the engine: TLS handshake failures, firewall
  drops, a venue not answering the SYN
- Nothing at all, if log level is turned down or logging is off

Some of these can be closed with cheap checks (a TCP connect probe, certificate
expiry checks, reading socket state); the rest need the pcap adapter.

Event log lines that cannot be parsed are not discarded — they are stored raw as
`unparsed` and counted under
`fixmon_session_events_total{type="unparsed"}`. That metric rising is the signal
that an engine version or a vendor started using new wording.

The parse rules currently live in `src/event_log_adapter.cpp`. They are
deliberately shaped as a table: lifting them out would not change this adapter's
logic.

The engine cfg is read once, at startup. If a session is added to the engine
while fixmon is running, fixmon has to be restarted. Watching the cfg and hot
reloading is possible but is not done today.

