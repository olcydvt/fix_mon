# Demo — end to end

Two routes. Which one you pick depends on what you want to show.

| | Setup | What it shows | Time |
|---|---|---|---|
| **A. Collector only** | cmake + g++ | Collection, parsing, state, event store | ~2 min |
| **B. Full stack** | Docker | The above plus Prometheus and Grafana dashboards | ~5 min |

A third route exists once you have real engines: point the collector at their
configs and containerise only the dashboards. See **Watching a real engine
pair** in `README.md` and `deploy/docker-compose.live.yml`.

---

## A. Collector only (no Docker)

### Setup

```bash
# Debian/Ubuntu
sudo apt install -y g++ cmake make python3

# sqlite is optional: without it CMake downloads and links it statically
sudo apt install -y libsqlite3-dev
```

macOS: `brew install cmake python3` (sqlite is already there).

### Run

```bash
./demo.sh          # a 40 second run
./demo.sh 120      # longer
```

The script builds, runs the self-test, sets up a clean demo environment, starts
the collector, runs the log generator with failures injected, then prints
session state, the metrics, and the incident timeline out of the event store.

Run it from a terminal and the collector stays up so you can point Prometheus at
`localhost:9109`. Called from another script it shuts itself down
(`DEMO_KEEP_RUNNING=0` forces that).

### Expected output

```
--- key metrics (/metrics) ---
fixmon_business_rejects_total{session="FIX.4.4:BROKER1->VENUEX",reason="2"} 1
fixmon_disconnects_total{session="FIX.4.4:BROKER1->VENUEX"} 1
fixmon_heartbeat_timeouts_total{session="FIX.4.4:BROKER1->VENUEX"} 1
fixmon_seq_gaps_total{session="FIX.4.4:BROKER1->VENUEX",src="event_log"} 1
fixmon_seq_gaps_total{session="FIX.4.4:BROKER1->VENUEX",src="message_log"} 1
fixmon_session_up{session="FIX.4.4:BROKER1->VENUEX"} 1.000000
fixmon_store_rows_written_total 517
fixmon_events_dropped_total 0

--- incident timeline from the event store ---
  17:38:40  event_log    seq_num_too_high       MsgSeqNum too high, expecting 65 but received 71
  17:38:44  message_log  Reject                 Required tag missing
  17:38:47  message_log  ExecutionReport        Instrument not tradable at this time
  17:38:47  message_log  BusinessMessageReject  Unknown security
  17:38:51  event_log    heartbeat_timeout      Test Request timed out.
  17:38:53  event_log    disconnected           Socket exception, connection reset by peer
  17:38:54  event_log    reconnect_attempt      Attempting to reconnect in 5 seconds
```

That last block is the point of the demo: look at the `source` column and watch
two sources merge onto one timeline. The message log gives you the rejects, the
event log gives you the **reason** for the disconnect. Neither one produces this
table on its own.

---

## B. Full stack (Docker)

### Setup

Docker is all you need — no compiler, no sqlite, no Python on the host.

```bash
docker --version
docker compose version     # works if you have v2
docker-compose --version   # if not, you have v1
```

**The v1/v2 difference matters.** The command name differs (`docker compose` vs
`docker-compose`) and v1 rejects file format 3.8. That is why
`docker-compose.yml` uses **format 3.7**, which both accept. v2 prints a
harmless "obsolete" note about the `version` key; ignore it.

The file was checked against both parsers: `docker-compose config` (v1.25.0) and
`docker compose config` (v2).

If you are on v1, move the machine to v2. v1 dates from 2019, is Python based
and is no longer maintained:

```bash
mkdir -p ~/.docker/cli-plugins
ARCH=$(uname -m)   # x86_64 or aarch64
curl -SL "https://github.com/docker/compose/releases/latest/download/docker-compose-linux-${ARCH}" \
  -o ~/.docker/cli-plugins/docker-compose
chmod +x ~/.docker/cli-plugins/docker-compose
docker compose version
```

One binary, ~32 MB, and you do not have to remove the old `docker-compose` —
they coexist. For a system-wide install use `/usr/local/lib/docker/cli-plugins`
instead of `~/.docker/cli-plugins`.

**But do not delete the `version: "3.7"` line from the repo.** On v2 it costs
one cosmetic warning line. In exchange the file opens on every machine that
still has the old `docker-compose` installed. Plenty of mid-size brokers run an
older Ubuntu LTS and get 1.x from apt, and a compose file that will not open on
a customer's server is far more expensive than a warning.

### Run

```bash
cd deploy
docker compose up --build      # Compose v2
docker-compose up --build      # Compose v1
```

The first build takes a few minutes, and **while it runs `loggen` is already
`Up` and `fixmon` does not exist yet**. If you run `docker compose ps` in
another terminal and see a single service, that is not a fault, the build is
still going. To build just the image: `docker compose build fixmon`.

Then:

| Service | Address | Note |
|---|---|---|
| Grafana | http://localhost:3000 | admin / admin, anonymous viewing enabled |
| Prometheus | http://localhost:9090 | rules show up under Alerts |
| Collector | http://localhost:9109/metrics | raw metrics |
| Session JSON | http://localhost:9109/sessions | debugging |

Grafana comes with its dashboards and datasource provisioned, nothing to set up
by hand. **FIX Sessions** and **FIX Session Discovery** are both ready.

Four services:

- `loggen` — stands in for a FIX engine, writing QuickFIX-shaped logs into a
  shared volume, looping
- `fixmon` — reads those logs through a **read-only** mount, the same
  arrangement as production: the logs belong to the engine, the collector only
  reads
- `prometheus` — scrapes the collector every 10 seconds and loads the alert
  rules
- `grafana` — datasource and dashboards predefined

Nothing tells the collector which sessions exist: `fixmon.docker.ini` has no
`[session]` block. It derives the session, and the log file names, from the
engine's own `quickfix.docker.cfg`. The first log lines say so:

```
config: imported FIX.4.4:BROKER1->VENUEX from /etc/fixmon/quickfix.cfg
session: FIX.4.4:BROKER1->VENUEX  hb=30s  initiator  <- /etc/fixmon/quickfix.cfg
    messages: /logs/FIX.4.4-BROKER1-VENUEX.messages.current.log
    events  : /logs/FIX.4.4-BROKER1-VENUEX.event.current.log
    credentials ignored: Username=<redacted> Password=<redacted>
```

### Verify the stack with one command

```bash
tools/verify_stack.sh
```

It checks the four containers, the collector's healthcheck, that session
discovery came from the engine cfg, that **both log sources** are attached, that
the credential values in the cfg do not appear on any endpoint, the Prometheus
target, and both Grafana dashboards. Run it before a demo; better than finding
"No data" on stage.

```
discovery from the engine config
  ok    session discovered: FIX.4.4:BROKER1->VENUEX
  ok    both log sources attached (messages + event)
  ok    reading logs (70645 lines)
credentials
  ok    2 credential values from the engine cfg absent from /metrics and /sessions
  ok    no credential-shaped label names
  ok    collector reports dropping 2 credential keys
prometheus
  ok    scrape target up
  ok    12 alert rules loaded
grafana
  ok    grafana healthy
  ok    dashboard provisioned: FIX Sessions
  ok    dashboard provisioned: FIX Session Discovery

stack verified
```

`fixmon_session_log_sources` at **0** means the session is configured but
nothing is being read — the one failure where every other metric looks perfectly
healthy and only this one shows it. At `1` you have half the picture, usually
because the event log name did not match.

### Showing that credentials do not leak

```bash
grep -i password deploy/quickfix.docker.cfg    # it is in the cfg
curl -s --noproxy '*' localhost:9109/metrics | grep -ci 'password\|username'   # 0
```

### What to show

1. **Open the dashboard** — session LOGGED ON, message rates flowing
2. **Discovery** — no session was configured on the collector, it found one in
   the engine cfg; the `FIX Session Discovery` dashboard shows this
3. **Sequence health panel** — the moment a gap is flagged by both sources; the
   `src` label keeps them apart
4. **Reject rate panel** — broken down by reject code
5. **Session events panel** — disconnect, heartbeat timeout, reconnect
6. **Prometheus → Alerts** — `FixSessionDown`, `FixSeqNumTooLow`
7. **Collector health panel** — no drops, empty queue, and the unparsed line
   count (the generator writes an unrecognised line on purpose, it shows here)

### Triggering a failure live

Actually taking the session down during a demo lands well:

```bash
docker compose stop loggen     # v1: docker-compose stop loggen
```

Within 30–60 seconds `fixmon_last_message_age_seconds` climbs, the session goes
**STALE**, and `FixSessionStale` moves to Pending. Then:

```bash
docker compose start loggen
```

### Cleanup

```bash
docker compose down -v      # -v removes the volumes too
# v1: docker-compose down -v
```

---

## What has actually been tested

Being honest about it:

| Piece | Status |
|---|---|
| Collector, adapters, state machine, event store | Tested, 100+ checks pass |
| `demo.sh` end to end | Run; the output above is real |
| Metric names ↔ dashboard and alert queries | Compared automatically, no mismatch |
| Datasource uid ↔ dashboard uid | Verified |
| Every YAML/JSON and compose mount path | Verified |
| Compose file, v1.25.0 parser | `docker-compose config`, valid |
| Compose file, v2 parser | `docker compose config`, valid (only the `version` warning) |
| **The Docker Compose stack itself** | **Run** — Rocky Linux 8 / WSL2, Docker 26.1.3, four services `Up`, `fixmon` healthy |
| `tools/verify_stack.sh` | Run against the live stack, all 16 checks passed |
| **A real QuickFIX engine pair** | **Run** — live initiator and acceptor, both directions monitored |
| **Windows / MSVC build** | **Not attempted** (MinGW works, MSVC untried) |

The stack really did come up: the image built, the self-test passed inside the
build, the session was discovered from the engine cfg, both log sources
attached, the Prometheus target came back UP and both dashboards provisioned.
The `verify_stack.sh` output above is from that run.

### The real-engine run

This is the strongest evidence in the list. The collector was run against a
**real QuickFIX initiator and acceptor**, not the log generator, and the
following were observed live:

- The session reached `logged_on`, with each direction tracked as its own
  session
- Killing one end produced a **disconnect**, and once the heartbeat tolerance
  expired the session went **stale**
- A **NewOrderSingle** that was sent showed up in the flow
- With the session made persistent, the sequence number was changed by hand and
  the collector caught the **sequence mismatch** and showed it on the Grafana
  Sequence Number panel

That last one matters most. The gap is both derived from the message stream and
narrated in the engine's event log, and the `src` label keeps the two apart.
Seeing that against a real engine's own wording is a different thing from seeing
it against text the generator imitates.

To reproduce this setup, see **Watching a real engine pair** in `README.md`.

---

## Troubleshooting

**`cmake` cannot find sqlite** — expected, it falls back to downloading. With no
network: `sudo apt install libsqlite3-dev`, or
`-DFIXMON_SQLITE_PROVIDER=local -DFIXMON_SQLITE_SOURCE_DIR=...`

**Metrics are empty** — check the two files under `logs/` exist and match the
paths in `fixmon.ini`. The collector only sees lines written after it starts;
set `from_beginning = true` to read existing content too.

**`bind: address already in use`** — something else on the host holds that port.
Usually a `./demo.sh` you ran earlier: in interactive mode the collector stays
up on purpose and keeps 9109.

```bash
ss -ltnp | grep 9109        # who holds it
pkill -f 'build/fixmon'     # kill what demo.sh left behind
```

Or change the host ports — compose takes them from the environment:

```bash
FIXMON_PORT=9110 GRAFANA_PORT=3001 PROMETHEUS_PORT=9091 docker compose up --build
```

The names have to match the compose file exactly. Get one wrong and compose
silently falls back to the default, the port clash persists, and nothing errors.

The collector's host port is optional anyway: Prometheus reaches it over the
compose network as `fixmon:9109`. If you are not going to `curl /metrics` from
the host you can delete the `ports:` block from the `fixmon` service entirely.

**Only `loggen` is up, the other three are missing** — most likely not a fault.
`loggen` pulls a ready-made image and starts in seconds; `fixmon` builds from
source and takes a few minutes the first time. Prometheus and Grafana depend on
it, so they queue behind it. To confirm the build is still running:

```bash
docker compose logs -f fixmon      # container logs start flowing once it builds
docker compose build fixmon        # or isolate the build
```

If the build genuinely fails, that second command shows the error plainly. If it
finished and the services still are not there, try `docker compose up -d` again.

**Grafana will not open / a service is down** — check all four are `Up` with
`docker-compose ps`. An empty list means the stack never started;
`docker-compose logs --tail=50` says why. The most common cause is the fixmon
image failing to build: the `grafana` → `prometheus` → `fixmon` chain means one
failure keeps Grafana from starting at all. To try just the build:
`docker-compose build fixmon`.

**"Unsupported config option for services"** — you are on Compose v1 and the
file has no `version` key. The one in this repo does (3.7); you may be using a
different copy.

**Grafana "No data"** — in Prometheus, Status → Targets, check the `fixmon`
target is UP. If it is not, the container name is not resolving.

**`curl localhost:9109` returns something unexpected** — on corporate networks,
if `http_proxy`/`https_proxy` are set, curl sends even localhost requests to the
proxy and you end up reading the proxy's error page. That is why
`tools/verify_stack.sh` passes `--noproxy '*'` on every call. Do the same by
hand:

```bash
curl -s --noproxy '*' localhost:9109/metrics | head
```

**`docker compose logs loggen` looks empty** — Python buffers stdout, so lines
may not appear immediately even while the generator runs. Log output is not a
real health signal here; look at the files in the volume instead:

```bash
docker compose exec fixmon sh -c 'wc -l /logs/*.log'
```

**Session stays "unknown"** — `sender_comp_id` in the config has to match tag 49
in the log exactly; direction detection depends on it.

**The `unparsed` counter is climbing** — the engine's wording does not match the
rule table. The lines are kept raw, not lost:
`SELECT text FROM events WHERE session_event='unparsed'`, then add a rule to the
table in `src/event_log_adapter.cpp`.

In the demo stack this counter being **above zero is normal**:
`tools/gen_logs.py` deliberately writes one unrecognised line per round
(`Vendor-specific condition XYZ-4471...`). Showing that behaviour is the point —
the collector does not discard a line it does not understand, it counts it and
stores it.

