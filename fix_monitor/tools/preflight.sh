#!/usr/bin/env bash
# Validates everything the compose stack needs, before anything is started.
# Read-only: no containers, no network, no builds.
set -u
cd "$(dirname "$0")/.." || exit 1

rc=0
ok()   { echo "  ok   $*"; }
bad()  { echo "  FAIL $*"; rc=1; }

echo "== files the stack mounts =="
for f in deploy/docker-compose.yml deploy/Dockerfile deploy/prometheus.yml \
         deploy/fixmon.docker.ini deploy/quickfix.docker.cfg \
         deploy/grafana/provisioning/datasources/prometheus.yml \
         deploy/grafana/provisioning/dashboards/dashboards.yml \
         grafana/alerts.yml grafana/fixmon-sessions.json grafana/fixmon-config.json \
         tools/gen_logs.py; do
    [ -f "$f" ] && ok "$f" || bad "$f missing"
done

echo
echo "== syntax =="
python3 - <<'PY' || rc=1
import json, sys
for f in ("grafana/fixmon-sessions.json", "grafana/fixmon-config.json"):
    try:
        d = json.load(open(f))
        print(f"  ok   {f}  uid={d.get('uid')}  panels={len(d.get('panels', []))}")
    except Exception as e:
        print(f"  FAIL {f}: {e}"); sys.exit(1)
PY

if python3 -c "import yaml" 2>/dev/null; then
    python3 - <<'PY' || rc=1
import yaml, sys
for f in ("deploy/docker-compose.yml", "deploy/prometheus.yml", "grafana/alerts.yml"):
    try:
        yaml.safe_load(open(f)); print(f"  ok   {f}")
    except Exception as e:
        print(f"  FAIL {f}: {e}"); sys.exit(1)
PY
else
    echo "  --   pyyaml not installed, YAML syntax not checked (compose will catch it)"
fi

echo
echo "== the two dashboards must not collide on uid =="
u1=$(python3 -c "import json;print(json.load(open('grafana/fixmon-sessions.json'))['uid'])")
u2=$(python3 -c "import json;print(json.load(open('grafana/fixmon-config.json'))['uid'])")
[ "$u1" != "$u2" ] && ok "uids distinct: $u1, $u2" || bad "both dashboards use uid $u1"

echo
echo "== every metric a dashboard or alert asks for must be one fixmon declares =="
python3 - <<'PY' || rc=1
import json, re, sys

declared = set(re.findall(r'declare_(?:counter|gauge)\(\s*"([a-z0-9_]+)"',
                          open("src/metrics.cpp").read()))
declared |= {"up"}  # Prometheus' own

used = set()
for f in ("grafana/fixmon-sessions.json", "grafana/fixmon-config.json"):
    for m in re.finditer(r'"expr"\s*:\s*"((?:[^"\\]|\\.)*)"', open(f).read()):
        used |= set(re.findall(r'\bfixmon_[a-z0-9_]+', m.group(1)))
used |= set(re.findall(r'\bfixmon_[a-z0-9_]+', open("grafana/alerts.yml").read()))

missing = sorted(u for u in used if u not in declared)
if missing:
    print("  FAIL dashboards/alerts reference undeclared metrics:")
    for m in missing:
        print("       ", m)
    sys.exit(1)
print(f"  ok   {len(used)} referenced metrics all exist ({len(declared)} declared)")
PY

echo
echo "== the engine cfg must point at the volume the loggen writes to =="
grep -q '^FileLogPath=/logs' deploy/quickfix.docker.cfg \
  && ok "FileLogPath=/logs" || bad "quickfix.docker.cfg FileLogPath is not /logs"
grep -q 'naming quickfix' deploy/docker-compose.yml \
  && ok "loggen uses the QuickFIX file naming" \
  || bad "loggen is not using --naming quickfix, names will not match the cfg"
grep -q '^quickfix_config' deploy/fixmon.docker.ini \
  && ok "collector imports the engine cfg" || bad "fixmon.docker.ini has no quickfix_config"
grep -q '^\[session\]' deploy/fixmon.docker.ini \
  && echo "  note a [session] block is still present; discovery is not being exercised" \
  || ok "no [session] block: the session list comes only from the engine cfg"

echo
[ $rc -eq 0 ] && echo "all preflight checks passed" || echo "PREFLIGHT FAILED"
exit $rc

