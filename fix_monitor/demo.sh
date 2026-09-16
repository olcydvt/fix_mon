#!/usr/bin/env bash
#
# Local demo without Docker.
#
# The point of this run: the collector is never told what sessions exist. It is
# pointed at the FIX engine's own config file, finds the session there, works
# out where the engine writes its logs, tails them, and drops every credential
# on the way through.
#
#   ./demo.sh              # 45 second run
#   ./demo.sh 90           # longer run
#
set -euo pipefail

DURATION="${1:-45}"
PORT="${PORT:-9109}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# A corporate http_proxy will happily answer for 127.0.0.1 and hand back its own
# redirect page, which makes every check below pass for the wrong reason. Only
# curl gets the bypass: unsetting the variables outright would also cut CMake
# off from the network it may need to fetch sqlite.
CURL=(curl -s --noproxy '*')

# Written into the demo engine config, then looked for in the metrics output.
# It must never turn up there.
SECRET="hunter2-demo-secret"

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

cleanup() {
    [[ -n "${FIXMON_PID:-}"  ]] && kill "$FIXMON_PID"  2>/dev/null || true
    [[ -n "${LOGGEN_PID:-}"  ]] && kill "$LOGGEN_PID"  2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

say "0/8  port check"
# The subshell releases the descriptor on exit. Do NOT add `exec 3>&- 2>/dev/null`
# here: a bare `exec` with redirections applies them to the shell permanently,
# which silently sends the rest of this script's stderr to /dev/null.
if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    cat >&2 <<MSG

Port $PORT is already in use. Most likely an earlier run of this script left a
collector behind, or the compose stack is up.

  who has it   : ss -ltnp | grep $PORT
  stop a stray : pkill -x fixmon
  stop compose : (cd deploy && docker compose down)

Or run this demo on another port:  PORT=9209 ./demo.sh

MSG
    exit 1
fi
echo "     :$PORT free"

say "1/8  build"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
echo "     fixmon $(./build/fixmon --version | awk '{print $2}')"

say "2/8  selftest"
./build/fixmon_selftest | tail -1

say "3/8  reset demo state"
rm -rf logs demo-store demo.db demo.db-wal demo.db-shm
mkdir -p logs demo-store

# Stand-in for the FIX engine's own config file. Nothing here is fixmon's
# format - this is what the engine reads.
cat > .demo-engine.cfg <<CFG
# QuickFIX engine configuration. fixmon opens this read-only.
[DEFAULT]
ConnectionType=initiator
ReconnectInterval=30
FileStorePath=./demo-store
FileLogPath=./logs
StartTime=00:00:00
EndTime=00:00:00
HeartBtInt=30
SenderCompID=BROKER1
UseDataDictionary=Y

# Real deployments keep these here. fixmon drops the values at parse time and
# keeps only the key names.
Username=broker1
Password=$SECRET

[SESSION]
BeginString=FIX.4.4
TargetCompID=VENUEX
SocketConnectHost=venue-gw.example.com
SocketConnectPort=9823
DataDictionary=FIX44.xml
CFG

# Collector config: global knobs only. There is no [session] block anywhere -
# the session list comes entirely from the engine's file.
cat > .demo.ini <<INI
[global]
db_path              = ./demo.db
metrics_port         = $PORT
queue_size           = 65536
batch_size           = 500
batch_flush_ms       = 200
poll_interval_ms     = 100
from_beginning       = false
stale_after_multiple = 2
snapshot_interval_s  = 5

quickfix_config = ./.demo-engine.cfg
INI

# The engine would create these on its first write. Pre-creating them keeps the
# demo deterministic; the tailer would pick them up either way.
touch logs/FIX.4.4-BROKER1-VENUEX.messages.current.log \
      logs/FIX.4.4-BROKER1-VENUEX.event.current.log
echo "     .demo-engine.cfg (engine)  .demo.ini (collector, no session block)"

say "4/8  what the collector worked out from the engine config"
./build/fixmon .demo.ini --print-config

say "5/8  start collector"
./build/fixmon .demo.ini &
FIXMON_PID=$!
sleep 1

for i in $(seq 1 20); do
    if "${CURL[@]}" -f "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then break; fi
    sleep 0.5
    [[ $i -eq 20 ]] && { echo "collector did not come up"; exit 1; }
done
echo "     up on :$PORT"

say "6/8  generate traffic for ${DURATION}s (with injected failures)"
python3 tools/gen_logs.py --dir ./logs --duration "$DURATION" --rate 10 \
        --naming quickfix &
LOGGEN_PID=$!
wait "$LOGGEN_PID" || true
unset LOGGEN_PID
sleep 2

say "7/8  credential check against the live endpoint"
"${CURL[@]}" "http://127.0.0.1:$PORT/metrics"  > .demo-metrics.txt
"${CURL[@]}" "http://127.0.0.1:$PORT/sessions" > .demo-sessions.json

if grep -qF -- "$SECRET" .demo-metrics.txt .demo-sessions.json; then
    echo "     LEAK: the password from .demo-engine.cfg reached the endpoint"
    exit 1
fi
if grep -qiE 'password|username' .demo-metrics.txt; then
    echo "     LEAK: a credential-shaped label reached the endpoint"
    exit 1
fi
echo "     clean: '$SECRET' is in the engine config and nowhere on :$PORT"
grep -E '^fixmon_session_(info|log_sources|config_redacted)' .demo-metrics.txt \
  | sed 's/^/     /'

say "8/8  results"

echo
echo "--- session state (/sessions) ---"
python3 -m json.tool < .demo-sessions.json

echo
echo "--- key metrics (/metrics) ---"
grep -Ev '^#' .demo-metrics.txt \
  | grep -E 'session_up|session_state|seq_gaps_total|seq_too_low|rejects_total|disconnects|heartbeat_timeouts|reconnect|dropped|store_|redacted' \
  | sort

echo
echo "--- incident timeline from the event store ---"
python3 - <<'PY'
import sqlite3, os
if not os.path.exists("demo.db"):
    print("no demo.db"); raise SystemExit
c = sqlite3.connect("demo.db")
rows = c.execute("""
SELECT time(ts_ns/1000000000,'unixepoch') t, source,
       COALESCE(msg_type_name, session_event) what, COALESCE(text,'') detail
FROM events
WHERE session_event IN ('disconnected','heartbeat_timeout','seq_num_too_high',
                        'seq_num_too_low','logon_received','reconnect_attempt','unparsed')
   OR msg_type IN ('3','j') OR (msg_type='8' AND ord_status='8')
ORDER BY ts_ns""").fetchall()
for r in rows:
    print(f"  {r[0]}  {r[1]:<12} {r[2]:<22} {r[3][:58]}")
n, = c.execute("SELECT COUNT(*) FROM events").fetchone()
s, = c.execute("SELECT COUNT(*) FROM session_snapshots").fetchone()
print(f"\n  {n} events, {s} snapshots stored in demo.db")
PY

# Keep the collector up when a human is watching, so Prometheus can be pointed
# at it. Exit cleanly when run from a script or CI.
if [[ -t 1 && "${DEMO_KEEP_RUNNING:-1}" == "1" ]]; then
    cat <<EOF

--- next ---
  Collector is still running on :$PORT (Ctrl-C to stop).
  Point Prometheus at localhost:$PORT, or bring up the full stack:

      cd deploy && docker compose up --build

  The compose stack also wants :$PORT, so stop this first, or run it on
  another port:

      FIXMON_PORT=9209 docker compose up --build

  If you lose this terminal:  pkill -x fixmon

EOF
    wait "$FIXMON_PID" 2>/dev/null || true
else
    echo
    echo "--- done (non-interactive: collector stopped) ---"
fi

