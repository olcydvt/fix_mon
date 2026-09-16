#!/usr/bin/env bash
#
# Proves, against a running collector, that nothing from the engine's config
# reached the scrape endpoint except the labels we chose by hand.
#
# The unit tests assert this at every layer, but the layer that matters to an
# auditor is the one Prometheus actually reads, so check that one directly.
#
#   tools/verify_no_secrets.sh [fixmon-binary] [quickfix.cfg] [port]
#
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

# Every credential value present in the engine config, by name lookup, so the
# check tests the real secrets rather than a placeholder.
SECRETS=$(grep -iE '^[[:space:]]*[A-Za-z_]*(password|passwd|secret|token|username|apikey|privatekey)[A-Za-z_]*[[:space:]]*=' "$CFG" \
          | cut -d= -f2- | tr -d ' \r' | grep -v '^$')

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; kill %1 2>/dev/null' EXIT

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

rc=0
for secret in $SECRETS; do
  if grep -qF -- "$secret" "$TMP/metrics" "$TMP/sessions"; then
    echo "LEAK: a credential value from $CFG appears on the endpoint"
    rc=1
  fi
done
if grep -qiE '(password|passwd|secret|token|username|apikey|privatekey)[^_]*=' "$TMP/metrics"; then
  echo "LEAK: a credential-shaped label name appears on the endpoint"
  grep -inE '(password|passwd|secret|token|username|apikey)' "$TMP/metrics"
  rc=1
fi

# This counter moving means some code path tried and the registry stopped it.
refused=$(grep -E '^fixmon_metric_series_redacted_total ' "$TMP/metrics" | awk '{print $2}')
[ "${refused:-0}" = "0" ] || { echo "WARNING: registry refused $refused credential-labelled series"; rc=1; }

if [ $rc -eq 0 ]; then
  echo "clean: no credential value or credential-shaped label on /metrics or /sessions"
  grep -E '^fixmon_session_(info|config_redacted|log_sources)' "$TMP/metrics"
fi
exit $rc

