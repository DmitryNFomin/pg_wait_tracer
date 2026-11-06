#!/usr/bin/env bash
# SubagentStart hook: records agent_id into the shared ledger (see
# agent-ledger.py) that sweep.sh counts as "agents currently running" --
# replaces the retained-live-worktree-count proxy, which read 227 against a
# cap of 3 on this repo and permanently paused the dispatcher's escalation
# gate (a worktree is evidence an agent ONCE existed, not that one runs now).
#
# SubagentStart cannot block subagent creation regardless of this script's
# exit code or crash (hooks reference: "SubagentStart hooks can't block
# subagent creation"), so there is no failure mode here that can ever
# prevent an agent from starting -- this always exits 0.
set -u
# The ledger is a MACHINE-WIDE count (it feeds the machine-wide 3-agent cap),
# not a per-worktree one like .sweep-latest.txt/.dispatch-queue/.dispositions
# -- so, like sweep.sh's own hardcoded `cd`, it lives at one canonical
# checkout path rather than next to whichever worktree's hook fired.
LEDGER="/Users/dmitryfomin/work/git/pg_wait_tracer/scripts/lead/.active-agents"
STATE="${PGWT_LEAD_STATE_DIR:-/Users/dmitryfomin/work/git/pg_wait_tracer/scripts/lead}"
D="$(cd "$(dirname "$0")" && pwd)"
agent_id=$(cat | /usr/bin/python3 -c '
import fcntl, json, os, sys, time
try:
    d = json.load(sys.stdin)
    if not isinstance(d, dict):
        raise ValueError("payload must be an object")
    keys = ",".join(sorted(d))
except Exception:
    d = {}
    keys = "UNPARSEABLE"
print(d.get("agent_id") or "")
def token(value):
    return value if isinstance(value, str) and value and not any(c.isspace() for c in value) else "unknown"
try:
    with open(os.path.join(sys.argv[1], ".brief-gate.lock"), "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        epoch = int(time.time())
        aid, kind = token(d.get("agent_id")), token(d.get("agent_type"))
        presence = "PRESENT" if "agent_type" in d else "ABSENT"
        for name, line in ((".spawns", f"{epoch} {aid} {kind}\n"),
                           (".spawn-payloads", f"{epoch} {aid} keys={keys} agent_type={presence}\n")):
            try:
                with open(os.path.join(sys.argv[1], name), "a", encoding="utf-8") as records:
                    records.write(line)
            except OSError:
                pass
except OSError:
    pass
' "$STATE" 2>/dev/null)
[ -n "$agent_id" ] && /usr/bin/python3 "$D/agent-ledger.py" start "$LEDGER" "$agent_id" 2>/dev/null
exit 0
