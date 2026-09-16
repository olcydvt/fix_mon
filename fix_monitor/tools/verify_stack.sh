#!/usr/bin/env bash
#
# End-to-end check against a running compose stack. verify_no_secrets.sh starts
# its own collector and proves the redaction rule; this one asks the same
# question of the stack the demo actually shows on screen, plus the wiring that
# only exists in compose: the shared log volume, the Prometheus target, and the
# provisioned Grafana dashboards.
#
#   tools/verify_stack.sh                 (from deploy/ or the repo root)
#
# Ports follow the same env vars as docker-compose.yml.
#
set -u

FIXMON_PORT=${FIXMON_PORT:-9109}
PROMETHEUS_PORT=${PROMETHEUS_PORT:-9090}
GRAFANA_PORT=${GRAFANA_PORT:-3000}

# Run from deploy/ so the compose file and the engine config are both to hand.
cd "$(dirname "$0")/../deploy" || exit 2

# A corporate proxy will answer for localhost and hand back its own page, which
# would make every check below pass or fail for the wrong reason.
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY
CURL=(curl -s --noproxy '*' --max-time 5)

rc=0
pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; rc=1; }

# --- containers ------------------------------------------------------------
echo "containers"
for svc in fixmon fixmon-loggen fixmon-prometheus fixmon-grafana; do
  status=$(docker inspect -f '{{.State.Status}}' "$svc" 2>/dev/null)
  if [ "$status" = "running" ]; then pass "$svc running"; else fail "$svc ${status:-missing}"; fi
done

# The collector's healthcheck probes the real scrape port, so a healthy result
# means more than "the process did not exit".
health=$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{end}}' fixmon 2>/dev/null)
[ "$health" = "healthy" ] && pass "fixmon healthcheck healthy" || fail "fixmon healthcheck ${health:-none}"

# --- discovery -------------------------------------------------------------
# The compose file never tells the collector which sessions exist; if a session
# is on the endpoint, it came from the engine's own cfg.
echo "discovery from the engine config"
METRICS=$(mktemp); SESSIONS=$(mktemp)
trap 'rm -f "$METRICS" "$SESSIONS"' EXIT
"${CURL[@]}" -o "$METRICS" "http://127.0.0.1:$FIXMON_PORT/metrics"
"${CURL[@]}" -o "$SESSIONS" "http://127.0.0.1:$FIXMON_PORT/sessions"

[ -s "$METRICS" ] || { fail "cannot scrape :$FIXMON_PORT/metrics"; echo; exit 1; }

grep -q '^fixmon_session_info' "$METRICS" \
  && pass "session discovered: $(grep -m1 '^fixmon_session_info' "$METRICS" | sed 's/.*session="\([^"]*\)".*/\1/')" \
  || fail "no fixmon_session_info series"

# 0 means configured but nothing is being read - the one failure that leaves
# every other metric looking healthy.
#
# Gauges render as 2.000000, so compare as a number rather than as text.
sources=$(awk '/^fixmon_session_log_sources/ {print int($NF); exit}' "$METRICS")
case "${sources:-0}" in
  2) pass "both log sources attached (messages + event)" ;;
  1) fail "only one log source attached - half the picture" ;;
  *) fail "no log sources attached (fixmon_session_log_sources=${sources:-absent})" ;;
esac

# Discovery can succeed while the files stay empty, so confirm bytes moved.
lines=$(awk '/^fixmon_lines_read_total/ {n+=$NF} END {print int(n)}' "$METRICS")
[ "${lines:-0}" -gt 0 ] 2>/dev/null && pass "reading logs ($lines lines)" || fail "no log lines read"

# --- credentials -----------------------------------------------------------
echo "credentials"
SECRETS=$(grep -iE '^[[:space:]]*[A-Za-z_]*(password|passwd|secret|token|username|apikey|privatekey)[A-Za-z_]*[[:space:]]*=' \
          quickfix.docker.cfg | cut -d= -f2- | tr -d ' \r' | grep -v '^$')
n=0; leaked=0
for secret in $SECRETS; do
  n=$((n + 1))
  grep -qF -- "$secret" "$METRICS" "$SESSIONS" && { fail "credential value on endpoint: $secret"; leaked=1; }
done
[ $leaked -eq 0 ] && pass "$n credential values from the engine cfg absent from /metrics and /sessions"

grep -qiE '(password|passwd|secret|token|username|apikey)[^_]*=' "$METRICS" \
  && fail "credential-shaped label name on endpoint" \
  || pass "no credential-shaped label names"

dropped=$(grep -m1 '^fixmon_session_config_redacted' "$METRICS" | awk '{print $NF}')
[ "${dropped:-0}" != "0" ] && pass "collector reports dropping ${dropped%.*} credential keys" \
                           || pass "no credential keys in this config"

# --- prometheus ------------------------------------------------------------
echo "prometheus"
targets=$("${CURL[@]}" "http://127.0.0.1:$PROMETHEUS_PORT/api/v1/targets")
case "$targets" in
  *'"health":"up"'*) pass "scrape target up" ;;
  '')                fail "prometheus not answering on :$PROMETHEUS_PORT" ;;
  *)                 fail "scrape target not up yet (give it 15s)" ;;
esac

rules=$("${CURL[@]}" "http://127.0.0.1:$PROMETHEUS_PORT/api/v1/rules")
count=$(printf '%s' "$rules" | grep -o '"name":"Fix[A-Za-z]*"' | wc -l)
[ "$count" -gt 0 ] && pass "$count alert rules loaded" || fail "no alert rules loaded"

# --- grafana ---------------------------------------------------------------
echo "grafana"
code=$("${CURL[@]}" -o /dev/null -w '%{http_code}' "http://127.0.0.1:$GRAFANA_PORT/api/health")
[ "$code" = "200" ] && pass "grafana healthy" || fail "grafana http=$code"

# Anonymous viewer is enabled, so the dashboard list needs no credentials.
boards=$("${CURL[@]}" "http://127.0.0.1:$GRAFANA_PORT/api/search?type=dash-db")
for want in "FIX Sessions" "FIX Session Discovery"; do
  case "$boards" in
    *"$want"*) pass "dashboard provisioned: $want" ;;
    *)         fail "dashboard missing: $want" ;;
  esac
done

echo
[ $rc -eq 0 ] && echo "stack verified" || echo "stack has problems (see FAIL above)"
exit $rc

