#!/usr/bin/env bash
# LIVE red: the real walk, real Chrome, with the bridge KILLED.
#
# This is the false negative that mattered: the predecessor harness excluded a
# tab that never rendered from its worst case, so the component most likely to
# break -- the bridge -- is the one whose breakage made the margin look SAFE.
# Three tabs are enough to show it (each costs a 15 s readiness timeout x 3
# navigations); a short tab list is itself one of the things paint_analyse.py
# refuses, which is also demonstrated here.
set -uo pipefail
WT=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a5701cfbf20ffbcb6
EV="$WT/tests/results/demo_paint_20261007"
HERE="$(cd "$(dirname "$0")" && pwd)"

# Never pkill -f (it matches this script's own command line): find the bridge
# by the port it holds, show what it is, then kill it by pid.
holder=$(lsof -nP -iTCP:8384 -sTCP:LISTEN -t 2>/dev/null)
echo "bridge holding 8384:"; ps -p "${holder:-0}" -o pid=,args= 2>/dev/null || echo "  (none)"
[ -n "$holder" ] && kill "$holder"
for _ in $(seq 1 20); do lsof -nP -iTCP:8384 -sTCP:LISTEN -t >/dev/null 2>&1 || break; sleep 1; done
if lsof -nP -iTCP:8384 -sTCP:LISTEN -t >/dev/null 2>&1; then echo "bridge still up; red demo aborted"; exit 1; fi
echo "bridge is down: curl -> $(curl -s -m 3 -o /dev/null -w '%{http_code}' http://localhost:8384/ || echo refused)"

python3 "$EV/paint_walk2.py" --label redlive-bridge-down \
    --tabs overview,queries,waterfall \
    --out "$EV/redlive-bridge-down.json" 2>&1 | tail -30

echo
echo "=== predecessor arithmetic vs paint_analyse.verdict, on THIS real file ==="
cd "$EV" && python3 - <<'PY'
import json, red_demo_predecessor as rd, paint_analyse as pa
w = json.load(open("redlive-bridge-down.json"))
old = rd.predecessor_verdict([w])
new = pa.verdict([w])
print("tabs in the file:", {k: {"samples": v.get("samples"), "error": v.get("error","")[:40]}
                            for k, v in w["results"].items()})
print("console/load errors:", w.get("console_error_count"), w.get("load_errors")[:2])
print()
print("predecessor      : worst=%s ms  margin=%.2fx  -> %s"
      % (old["worst_ms"], old["margin"],
         "APPROVED (no tab rendered, yet a margin was printed)"))
print("paint_analyse    : ok=%s  reason=%s" % (new["ok"], new["reason"]))
PY
