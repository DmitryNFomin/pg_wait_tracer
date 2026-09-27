#!/usr/bin/env bash
# orphan-reap.sh — Claude Code Stop / SubagentStop hook. Kills any process
# still carrying this session's PGWT_OWNER marker, so an agent that dies
# (including on its own usage limit, mid-background-job) does not leave a
# background shell running for hours after it stops existing. Does not
# depend on the agent's cooperation: it runs on the stop event itself, not
# on anything the agent does.
#
# Marker mechanism: orphan-guard.sh (the PreToolUse hook) tags every
# *backgrounded* command it allows by prefixing it with
#   export PGWT_OWNER="<session_id>";
#   pgwt_sig=$(ps -o lstart=,comm= -p $$); printf '%s\t%s\n' "$$" "$pgwt_sig" >> <pids_file>
# before it runs. This hook reads that same <pids_file> and, for each
# entry, re-queries the SAME `ps -o lstart=,comm=` for that pid and only
# kills it if the signature still matches.
#
# This identity check exists because a bare pid is not an identity: a
# session backgrounds a long job (pid 4321, recorded), the job finishes on
# its own well before the agent stops, 4321 stays in the registry (it is
# only pruned wholesale at Stop), the agent keeps forking git/node/python
# for hours, the OS recycles 4321 onto one of those, and Stop fires -- a
# bare "is this pid alive" check would then kill an unrelated live
# process. Start-time + command name together are extremely unlikely to
# collide for an OS-recycled pid within one session's lifetime, and a
# mismatch (or the pid simply being gone) means the entry is dropped
# WITHOUT being killed -- silence, not a kill, is the safe default here.
#
# Documented-mechanism gap (see CLAUDE.md implementer contract, "report the
# gap instead of faking it"): the obvious design -- scan every process's
# environment for PGWT_OWNER=<session_id> -- is NOT implementable on macOS
# with an unprivileged, non-root `ps`. `ps -E`/`ps eww <pid>` are documented
# to "display the environment", but on this machine (Darwin 24.6, verified
# empirically, not assumed) neither shows another process's environment to
# its owning non-root user; the ENVIRON column is simply absent. There is no
# /proc on macOS to fall back to either. Linux has /proc/<pid>/environ, but
# that path is unexercised here (this hook has only run on macOS) and is
# NOT included below rather than shipping an untested branch.
# The self-registered pid-file above is what actually works cross-run on
# this platform, and still carries the same PGWT_OWNER value (exported, as
# asked) for anything downstream that wants to read its OWN marker.
#
# Second open gap, also unverified rather than assumed: the docs say Stop
# fires "when the agent considers stopping" / SubagentStop "when a subagent
# considers stopping" -- both read as a cooperative decision point. Whether
# either fires on an ABRUPT termination (the agent hits its usage limit
# mid-turn) is not stated anywhere found, and could not be tested without
# deliberately exhausting an agent's usage limit, which is out of scope
# here. If it does not fire on that path, this reaper never runs for
# exactly the case the task cares about most, and something else (a
# separate janitor, e.g. a periodic sweep, timeout on run_in_background
# itself) is needed to close that gap.
#
# Wired in .claude/settings.json.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
input=$(cat)

sid=$(printf '%s' "$input" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session_id","") or "")' 2>/dev/null)
[[ -z "$sid" ]] && exit 0

# PGWT_ORPHAN_GUARD_DIR overrides the pids directory -- test-only escape
# hatch (tests/test_orphan_guard.sh) so the bypass suite never touches the
# real registry under this checkout's own .claude/, same pattern as
# PGWT_CHECK_LOCK_FILE in tests/test_check_lock.sh.
pids_dir="${PGWT_ORPHAN_GUARD_DIR:-$root/.claude/orphan-guard/pids}"
pids_file="$pids_dir/$sid.pids"
[[ -f "$pids_file" ]] || exit 0

killed=0
skipped_mismatch=0
while IFS=$'\t' read -r pid recorded_sig; do
    [[ -z "$pid" ]] && continue
    if ! [[ "$pid" =~ ^[0-9]+$ ]]; then
        continue   # drop garbage lines, never treat them as a pid to kill
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        continue   # already gone -- nothing to verify or kill
    fi
    current_sig=$(ps -o lstart=,comm= -p "$pid" 2>/dev/null)
    if [[ -z "$recorded_sig" || "$current_sig" != "$recorded_sig" ]]; then
        # No recorded signature (older/foreign entry) or the pid answers
        # kill -0 but is no longer the SAME process we registered -- the OS
        # recycled it onto something unrelated. Refuse to kill; drop the
        # stale entry and move on. A miss here must never become a kill.
        skipped_mismatch=$((skipped_mismatch + 1))
        continue
    fi
    kill "$pid" 2>/dev/null && killed=$((killed + 1))
    # Best-effort escalation, one shot -- not a wait/verify loop (kill(2)
    # is asynchronous: a pid can still answer kill -0 for a moment after
    # SIGKILL while it is a not-yet-reaped zombie, so re-checking liveness
    # immediately here would misreport a successful kill as "still
    # running". We do not poll for the real exit; one TERM + one KILL is
    # the entire attempt, matching the no-retry-loop rule.
    kill -9 "$pid" 2>/dev/null || true
    # Every pid we looked at (dead-already, killed, mismatched, or garbage)
    # is dropped from the file: this is a one-shot marker, not a monitor,
    # so it must not become another accumulating task-record pile.
done < "$pids_file"

rm -f "$pids_file"
echo "orphan-reap: session $sid — killed $killed live process(es), skipped $skipped_mismatch (pid alive but signature mismatch -- not ours to kill)"
exit 0
