#!/usr/bin/env bash
# test_push_guard_denylist.sh -- scripts/hooks/push-guard.sh (#230) is a
# four-term substring denylist with NO shell grammar: no repository
# resolution, no quoting logic, nothing to be "ambiguous" about. This
# drives it exactly as Claude Code does, with real JSON on stdin, and
# checks only what a substring test can be checked for: each term alone
# denies, a command with none of them passes untouched, and -- the control
# that matters -- a heredoc with an odd number of apostrophes and none of
# the four terms passes, which is the exact shape the RETIRED command-text
# parser (scripts/hooks/push-guard.sh before #230) rejected outright with
# "invalid or unparseable tool command: No closing quotation" even though
# it contained no push at all.
#
# PGWT_TEST_PUSH_GUARD_DENYLIST can supply an alternate hook script, to
# show these checks go red against a regressed one (same convention as
# tests/test_pre_push_hook.sh's PGWT_TEST_PRE_PUSH_HOOK).
#
# Usage: tests/test_push_guard_denylist.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOK="${PGWT_TEST_PUSH_GUARD_DENYLIST:-$REPO_ROOT/scripts/hooks/push-guard.sh}"

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}
check() { if [[ "$1" == "true" ]]; then report true "$2"; else report false "$2"; fi; }

# Builds the real Claude Code hook JSON ({"tool_input":{"command": ...}})
# for an arbitrary command string, including one containing newlines and
# apostrophes -- json.dumps handles the escaping, not this script.
hook_json() {
    python3 -c 'import json, sys; print(json.dumps({"tool_input": {"command": sys.argv[1]}}))' "$1"
}

run_hook() {
    printf '%s' "$(hook_json "$1")" | bash "$HOOK" 2>&1
}

denied() {
    local command="$1" label="$2"
    local out rc
    out=$(run_hook "$command"); rc=$?
    check "$([[ $rc -ne 0 ]] && echo true || echo false)" "$label: denied (nonzero exit)"
    check "$([[ -n "$out" ]] && echo true || echo false)" "$label: a message was printed, not a silent refusal"
}

allowed() {
    local command="$1" label="$2"
    local rc
    run_hook "$command" >/dev/null 2>&1; rc=$?
    check "$([[ $rc -eq 0 ]] && echo true || echo false)" "$label: allowed (exit 0)"
}

# ── Each of the four terms alone denies ───────────────────────────────────
denied 'PGWT_SKIP_PUSH_GUARD=1 git push origin main' "PGWT_SKIP_PUSH_GUARD=1"
denied 'git config core.hooksPath /tmp/somewhere' "core.hooksPath"
denied 'chmod -x scripts/git-hooks/pre-push' "chmod + git-hooks together"
denied 'git push --no-verify origin main' "--no-verify"

# ── chmod and git-hooks only deny TOGETHER, not alone ─────────────────────
allowed 'chmod +x my-script.sh' "chmod alone (no git-hooks mention)"
allowed 'ls -la scripts/git-hooks' "git-hooks alone (no chmod)"

# ── A command with none of the four terms passes untouched ───────────────
allowed 'git push origin main' "an ordinary push with none of the four terms"
allowed 'git status && git log --oneline -5' "an unrelated command with none of the four terms"

# ── THE control that matters: odd apostrophe count, no push, none of the
#    four denylist terms -- must pass. The retired parser rejected exactly
#    this shape outright, blocking an unrelated command for a quoting
#    reason that had nothing to do with pushing or disarming anything. ──
odd_apostrophe_heredoc=$(cat <<'INNER'
cat > f.txt <<'EOF'
it can't be a push; neither can 'tests' alone
EOF
INNER
)
allowed "$odd_apostrophe_heredoc" \
    "odd-apostrophe heredoc, no push, no denylist term: passes (the shape the retired parser rejected)"

# ── The one accepted false positive: a MENTION, not a push ───────────────
denied 'git commit -m "doc: explain core.hooksPath in README"' \
    "a commit message merely mentioning core.hooksPath: denied too (the accepted tradeoff -- one blocked command, never a silent gap)"

echo
echo "test_push_guard_denylist: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
