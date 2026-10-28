#!/usr/bin/env bash
# SubagentStop hook, registered ALONGSIDE sweep-hook.sh (never in place of
# it), and listed BEFORE it in .claude/settings.json: removes agent_id from
# the shared ledger (see agent-ledger.py and agent-start-hook.sh).
#
# The ordering is belt-and-braces, not load-bearing: per the hooks reference
# ("When an event fires, its matching hooks run in parallel"), both entries
# in SubagentStop's array run regardless of either's exit code -- sweep-hook.sh
# blocking (exit 2) does not skip this script, so there is no leak where a
# blocked stop leaves an orphaned +1 in the ledger. Listed first anyway: it
# costs nothing, and keeps the decrement visually and structurally ahead of
# the one hook in this chain allowed to block (SubagentStop: "Prevents the
# subagent from stopping").
#
# Always exits 0 -- this script must never add its own blocking semantics on
# top of sweep-hook.sh's.
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
