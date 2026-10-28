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
D="$(cd "$(dirname "$0")" && pwd)"
agent_id=$(cat | /usr/bin/python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    d = {}
print(d.get("agent_id") or "")
' 2>/dev/null)
[ -n "$agent_id" ] && /usr/bin/python3 "$D/agent-ledger.py" start "$LEDGER" "$agent_id" 2>/dev/null
exit 0
