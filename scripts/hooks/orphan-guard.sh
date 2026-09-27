#!/usr/bin/env bash
# orphan-guard.sh — Claude Code PreToolUse hook (Bash). Refuses commands that
# create a process detached from this shell, and any backgrounded shell
# (manual `&` or the Bash tool's own run_in_background) shaped like a poll
# loop -- both are how a background shell keeps running long after the agent
# that started it is gone (13h/23h-old shells, eleven detached watchers,
# issue: PreToolUse Bash hook for orphan prevention).
#
# Decision logic lives in orphan_guard_lib.py (pure, unit-testable in
# isolation by tests/test_orphan_guard.sh). This wrapper only does the
# Claude Code I/O contract: read the hook JSON on stdin, call the decision
# engine, and either:
#   - allow (exit 0, no output),
#   - allow + tag the command for the reaper (exit 0, JSON on stdout with
#     hookSpecificOutput.updatedInput.command prefixed with an
#     `export PGWT_OWNER=<session_id>` + a self-registration line so
#     orphan-reap.sh can find and kill it later), or
#   - refuse (exit 2, reason on stderr — the documented "blocking error"
#     path, same convention as scripts/hooks/push-guard.sh).
#
# Fail-closed, not fail-open: if the decision engine cannot be evaluated at
# all (unparseable stdin, python3 missing/not executable, any exception),
# this refuses rather than silently allowing everything through -- a gate
# that cannot see must refuse, never approve.
#
# Wired in .claude/settings.json.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
input=$(cat)

decision=$(printf '%s' "$input" | python3 "$root/scripts/hooks/orphan_guard_decide.py" 2>/dev/null)
py_rc=$?

if [[ $py_rc -ne 0 || -z "$decision" ]]; then
    echo "orphan-guard: could not evaluate this command's safety (decision engine exited $py_rc) -- refusing conservatively rather than approving blind. This should not happen; report it instead of bypassing the hook." >&2
    exit 2
fi

kind=$(printf '%s' "$decision" | cut -f1)
sid=$(printf '%s' "$decision" | cut -f2)
reason=$(printf '%s' "$decision" | cut -f4-)

case "$kind" in
    refuse)
        echo "$reason" >&2
        exit 2
        ;;
    mark)
        cmd=$(printf '%s' "$input" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("command",""))' 2>/dev/null)
        if [[ -z "$cmd" ]]; then
            # could not re-extract the command text; do not silently drop
            # the marker with no signal -- refuse conservatively instead.
            echo "orphan-guard: could not re-extract command text to tag it for reaping -- refusing conservatively." >&2
            exit 2
        fi
        # PGWT_ORPHAN_GUARD_DIR overrides the pids directory -- test-only
        # escape hatch (tests/test_orphan_guard.sh), same pattern as
        # PGWT_CHECK_LOCK_FILE in tests/test_check_lock.sh.
        pids_dir="${PGWT_ORPHAN_GUARD_DIR:-$root/.claude/orphan-guard/pids}"
        mkdir -p "$pids_dir" 2>/dev/null || true
        pids_file="$pids_dir/$sid.pids"
        marker="export PGWT_OWNER=\"$sid\"; printf '%s\\n' \"\$\$\" >> \"$pids_file\" 2>/dev/null; "
        updated="$marker$cmd"
        out=$(python3 -c '
import json, sys
updated = sys.argv[1]
print(json.dumps({
    "hookSpecificOutput": {
        "permissionDecision": "allow",
        "updatedInput": {"command": updated},
    },
    "systemMessage": "orphan-guard: tagged for reaping (PGWT_OWNER=" + sys.argv[2] + ")",
}))
' "$updated" "$sid" 2>/dev/null)
        if [[ -z "$out" ]]; then
            echo "orphan-guard: could not build the tagged command -- refusing conservatively." >&2
            exit 2
        fi
        printf '%s\n' "$out"
        exit 0
        ;;
    allow)
        exit 0
        ;;
    *)
        echo "orphan-guard: unrecognised decision '$kind' from the decision engine -- refusing conservatively." >&2
        exit 2
        ;;
esac
