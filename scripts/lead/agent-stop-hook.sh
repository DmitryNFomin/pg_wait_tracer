#!/usr/bin/env bash
# SubagentStop hook, registered ALONGSIDE sweep-hook.sh (never in place of
# it): removes agent_id from the shared ledger (see agent-ledger.py and
# agent-start-hook.sh). Always exits 0 -- this script must never add its own
# blocking semantics on top of sweep-hook.sh's, which is the one SubagentStop
# hook in this chain allowed to block (SubagentStop: "Prevents the subagent
# from stopping").
set -u
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
[ -n "$agent_id" ] && /usr/bin/python3 "$D/agent-ledger.py" stop "$LEDGER" "$agent_id" 2>/dev/null
exit 0
