#!/usr/bin/env bash

# Proves, against a running collector, that no credential reached any surface
# the collector produces.

# The unit tests assert this at every layer, but the layers that matter to an
# auditor are the ones somebody else can read, so check those directly:

#   /metrics    what Prometheus scrapes
#   /sessions   what the diagnosis endpoint hands out
#   the store   the sqlite file, including its write-ahead log

# Two sources of secrets, because they leak by different routes:

#   the engine config  - credentials the collector was told to ignore
#   the message logs   - Username/Password inside a Logon the engine recorded

# The second is the one that bites. A Logon carries tag 553/554 in clear text,
# the engine writes it to the message log like any other message, and the
# collector reads that file on purpose. Checking only the config would pass a
# build that copied every password straight into the event store.

#   tools/verify_no_secrets.sh [fixmon-binary] [quickfix.cfg] [port]

set -u

BIN=${1:-./build/fixmon}
CFG=${2:-./quickfix.cfg}
PORT=${3:-9109}

[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 2; }
[ -f "$CFG" ] || { echo "no engine config at $CFG"; exit 2; }

# A corporate proxy will happily answer for 127.0.0.1 and hand back its own
# redirect page, which would make this check pass for the wrong reason.
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY
CURL=(curl -s --noproxy '*' --max-time 5)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; kill %1 2>/dev/null' EXIT

# ---------------------------------------------------------------------------
# Start the collector and find out where it put things.
# ---------------------------------------------------------------------------
"$BIN" --quickfix "$CFG" </dev/null >"$TMP/run.log" 2>&1 &

for _ in $(seq 1 40); do
  "${CURL[@]}" -o "$TMP/metrics" "http://127.0.0.1:$PORT/metrics" && [ -s "$TMP/metrics" ] && break
  sleep 0.5
done
"${CURL[@]}" -o "$TMP/sessions" "http://127.0.0.1:$PORT/sessions"

if [ ! -s "$TMP/metrics" ]; then
  echo "could not scrape http://127.0.0.1:$PORT/metrics"
  cat "$TMP/run.log"
  exit 1
fi

# The collector prints these; parsing them beats guessing, because a wrong
# guess here shows up as "clean" rather than as an error.
DB=$(sed -n 's/^event store: \([^(]*\)$/\1/p' "$TMP/run.log" | head -1 | sed 's/[[:space:]]*$//')
MSG_LOGS=$(sed -n 's/^[[:space:]]*messages: //p' "$TMP/run.log")

echo "checking: /metrics /sessions${DB:+ $DB}"
if [ -n "$MSG_LOGS" ]; then
  echo "message logs watched:"
  echo "$MSG_LOGS" | sed 's/^/  /'
fi

# Give the tail readers a moment to reach the store, otherwise an empty
# database passes for a clean one.
sleep 2

SURFACES=("$TMP/metrics" "$TMP/sessions")
if [ -n "$DB" ]; then
  for f in "$DB" "$DB-wal" "$DB-shm"; do
    [ -f "$f" ] && SURFACES+=("$f")
  done
fi

# ---------------------------------------------------------------------------
# Collect the secrets that must not appear on any of them.
# ---------------------------------------------------------------------------
: >"$TMP/secrets"

# From the engine config, by name lookup, so we test the real values rather
# than a placeholder.
grep -iE '^[[:space:]]*[A-Za-z_]*(password|passwd|secret|token|username|apikey|privatekey)[A-Za-z_]*[[:space:]]*=' "$CFG" \
  | cut -d= -f2- | tr -d ' \r' | grep -v '^$' >>"$TMP/secrets" || true

# From the message logs: the value of any credential tag the engine recorded.
# Both delimiters, because logs get rendered with | as often as with SOH.
if [ -n "$MSG_LOGS" ]; then
  while IFS= read -r log; do
    [ -f "$log" ] || continue
    tr '\001' '|' <"$log" \
      | grep -oE '(^|\|)(553|554|925|96|91|1402|1404)=[^|]+' \
      | cut -d= -f2- | grep -v '^$' >>"$TMP/secrets" || true
  done <<<"$MSG_LOGS"
fi

# A value short enough to collide with ordinary text would report a leak that
# is not there, and a false alarm here is how a check stops being trusted.
sort -u "$TMP/secrets" | awk 'length($0) >= 4' >"$TMP/secrets.u"
mv "$TMP/secrets.u" "$TMP/secrets"

rc=0
count=$(wc -l <"$TMP/secrets" | tr -d ' ')
echo "credential values to search for: $count"

if [ "$count" = "0" ]; then
  echo "NOTE: no credentials found in the config or in the logs - this run proves"
  echo "      nothing about masking. Point it at a session that logs on with a"
  echo "      Username/Password before trusting a clean result."
fi

# ---------------------------------------------------------------------------
# The checks.
# ---------------------------------------------------------------------------
while IFS= read -r secret; do
  [ -n "$secret" ] || continue
  for surface in "${SURFACES[@]}"; do
    if LC_ALL=C grep -qaF -- "$secret" "$surface" 2>/dev/null; then
      echo "LEAK: a credential value appears in $(basename "$surface")"
      rc=1
    fi
  done
done <"$TMP/secrets"

if grep -qiE '(password|passwd|secret|token|username|apikey|privatekey)[^_]*=' "$TMP/metrics"; then
  echo "LEAK: a credential-shaped label name appears on the endpoint"
  grep -inE '(password|passwd|secret|token|username|apikey)' "$TMP/metrics"
  rc=1
fi

# Any credential tag still carrying a value in a stored body. Catches the case
# where a tag was added to FIX and not to us: the value is unknown to the list
# above, but the shape gives it away.
if [ -n "$DB" ]; then
  for f in "$DB" "$DB-wal"; do
    [ -f "$f" ] || continue
    if LC_ALL=C grep -qaE '(553|554|925|1402|1404)=[^<]' "$f" 2>/dev/null; then
      echo "LEAK: a credential tag in the store still carries a value ($(basename "$f"))"
      rc=1
    fi
  done
fi

# This counter moving means some code path tried and the registry stopped it.
refused=$(grep -E '^fixmon_metric_series_redacted_total ' "$TMP/metrics" | awk '{print $2}')
[ "${refused:-0}" = "0" ] || { echo "WARNING: registry refused $refused credential-labelled series"; rc=1; }

if [ $rc -eq 0 ]; then
  echo "clean: no credential value on /metrics, /sessions or in the event store"
  grep -E '^fixmon_session_(info|config_redacted|log_sources)' "$TMP/metrics"
fi
exit $rc