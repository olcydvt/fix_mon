#!/usr/bin/env bash
#
# Local demo without Docker. Builds the collector, starts it against a
# generated QuickFIX log pair, injects the failure modes, and prints what came
# out. Use this to see the collector working before wiring up Prometheus and
# Grafana.
#
#   ./demo.sh              # 40 second run
#   ./demo.sh 90           # longer run
#
set -euo pipefail

DURATION="${1:-45}"
PORT="${PORT:-9109}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

cleanup() {
    [[ -n "${FIXMON_PID:-}"  ]] && kill "$FIXMON_PID"  2>/dev/null || true
    [[ -n "${LOGGEN_PID:-}"  ]] && kill "$LOGGEN_PID"  2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

port_owner() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltnp 2>/dev/null | grep ":$1 " || true
    elif command -v lsof >/dev/null 2>&1; then
        lsof -iTCP:"$1" -sTCP:LISTEN 2>/dev/null || true
    fi
}

say "0/6  port check"
# The subshell releases the descriptor on exit. Do NOT add `exec 3>&- 2>/dev/null`
# here: a bare `exec` with redirections applies them to the shell permanently,
# which silently sends the rest of this script's stderr to /dev/null.
if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    cat >&2 <<MSG

Port $PORT is already in use. Most likely an earlier run of this script left a
collector behind, or the compose stack is up.

  who has it   : ss -ltnp | grep $PORT
  stop a stray : pkill -f 'fixmon .*\\.ini'
  stop compose : (cd deploy && docker compose down)

Or run this demo on another port:  PORT=9209 ./demo.sh

MSG
    exit 1
fi
echo "     :$PORT free"

say "1/6  build"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
echo "     fixmon $(./build/fixmon --version | awk '{print $2}')"

say "2/6  selftest"
./build/fixmon_selftest | tail -1

say "3/6  reset demo state"
rm -rf logs demo.db demo.db-wal demo.db-shm
mkdir -p logs
touch logs/BROKER1-VENUEX.messages.log logs/BROKER1-VENUEX.event.log

sed -e 's|^db_path .*|db_path = ./demo.db|' \
    -e "s|^metrics_port .*|metrics_port = $PORT|" \
    -e 's|^snapshot_interval_s .*|snapshot_interval_s = 5|' fixmon.ini > .demo.ini
echo "     logs/  and demo.db"

say "4/6  start collector"
./build/fixmon .demo.ini &
FIXMON_PID=$!
sleep 1

for i in $(seq 1 20); do
    if curl -sf "http://localhost:$PORT/healthz" >/dev/null 2>&1; then break; fi
    sleep 0.5
    [[ $i -eq 20 ]] && { echo "collector did not come up"; exit 1; }
done
echo "     up on :$PORT"

say "5/6  generate traffic for ${DURATION}s (with injected failures)"
python3 tools/gen_logs.py --dir ./logs --duration "$DURATION" --rate 10 &
LOGGEN_PID=$!
wait "$LOGGEN_PID" || true
unset LOGGEN_PID
sleep 2

say "6/6  results"

echo
echo "--- session state (/sessions) ---"
curl -s "http://localhost:$PORT/sessions" | python3 -m json.tool

echo
echo "--- key metrics (/metrics) ---"
curl -s "http://localhost:$PORT/metrics" \
  | grep -Ev '^#' \
  | grep -E 'session_up|session_state|seq_gaps_total|seq_too_low|rejects_total|disconnects|heartbeat_timeouts|reconnect|dropped|store_' \
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

  If you lose this terminal:  pkill -f 'fixmon .*\\.ini'

EOF
    wait "$FIXMON_PID" 2>/dev/null || true
else
    echo
    echo "--- done (non-interactive: collector stopped) ---"
fi
