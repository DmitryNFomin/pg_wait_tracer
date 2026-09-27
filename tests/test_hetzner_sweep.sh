#!/usr/bin/env bash
# test_hetzner_sweep.sh -- unit tests for tests/hetzner-sweep.sh's decision
# logic (issue #162: a zero cutoff silently became "delete everything").
#
# No network, no real Hetzner token, no machine ever created or deleted:
# every case here drives the script's --dry-run path against a synthetic
# `--servers-file` (a canned `GET /servers` JSON body), which is exactly the
# "fed a synthetic server list" testing hook the script's own header
# documents. Wired into tests/unit_tests.list (runs on the box, not on the
# Mac's `make check` -- same tier as test_ui_live_smoke_lib.py /
# test_demo_rehearsal_lib.py).
#
# Usage: tests/test_hetzner_sweep.sh (run from anywhere; resolves its own dir)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SWEEP="$SCRIPT_DIR/hetzner-sweep.sh"

tests_run=0
tests_passed=0
tests_failed=0

# pass()/fail() take an already-evaluated boolean ("true"/"false" from a
# [[ ]] test) -- never a raw $? or exit code, which mix up 0=success with
# 0=false and are exactly the kind of off-by-inversion bug this test file
# must not itself have.
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

matches() { grep -q -- "$2" <<<"$1"; }

check_contains()    { if matches "$1" "$2"; then report true "$3"; else report false "$3"; fi; }
check_not_contains() { if matches "$1" "$2"; then report false "$3"; else report true "$3"; fi; }
check() { if [[ "$1" == "true" ]]; then report true "$2"; else report false "$2"; fi; }

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

now=$(date +%s)
young_age=30                 # 30s old -- created seconds ago by a concurrent agent
old_age=$((8 * 3600))        # 8h old -- genuinely stale

servers_file="$tmpdir/servers.json"
jq -n --argjson young_created "$((now - young_age))" \
      --argjson old_created "$((now - old_age))" '
{
  servers: [
    {id: 1001, name: "pgwt-dev-young",   labels: {pgwt: "ephemeral", created: ($young_created | tostring)}},
    {id: 1002, name: "pgwt-dev-old",     labels: {pgwt: "ephemeral", created: ($old_created | tostring)}},
    {id: 1003, name: "pgwt-gate",        labels: {}},
    {id: 1004, name: "pgwt-dev-missing", labels: {pgwt: "ephemeral"}},
    {id: 1005, name: "pgwt-dev-garbage", labels: {pgwt: "ephemeral", created: "2026-09-26T00:00:00Z"}},
    {id: 1006, name: "pgwt-gate-2",      labels: {}}
  ]
}' > "$servers_file"

# ── Case 1: cutoff 0 is refused ─────────────────────────────────────────
out=$("$SWEEP" --dry-run --servers-file "$servers_file" --max-age-hours 0 2>&1)
rc=$?
check "$([[ $rc -ne 0 ]] && echo true || echo false)" "cutoff 0 exits non-zero (refused)"
check_contains "$out" "REFUSING" "cutoff 0 refusal message printed"
check_contains "$out" "pgwt-dev-old" "refusal lists the machine it would have matched"
check_contains "$out" "hetzner-vm.sh delete" "refusal points at the by-id delete path"
check_not_contains "$out" "^hetzner-sweep: done" "cutoff 0 refusal never reaches the summary line (nothing evaluated)"

# ── Case 2: cutoff 1 hour is accepted ───────────────────────────────────
out=$("$SWEEP" --dry-run --servers-file "$servers_file" --max-age-hours 1 2>&1)
rc=$?
check "$([[ $rc -eq 0 ]] && echo true || echo false)" "cutoff 1h exits zero (accepted)"
check_not_contains "$out" "REFUSING" "cutoff 1h prints no refusal"

# ── Case 3: a 30s-old machine survives a valid sweep ────────────────────
check_not_contains "$out" "deleting stale ephemeral server pgwt-dev-young" \
    "30s-old machine is not deleted under a valid (1h) cutoff"

# ── Case 4: an 8h-old machine is swept ──────────────────────────────────
check_contains "$out" "deleting stale ephemeral server pgwt-dev-old" \
    "8h-old machine is swept under a valid (1h) cutoff"

# pgwt-gate must never be touched, whatever the cutoff.
check_not_contains "$out" "pgwt-gate" \
    "pgwt-gate (unlabelled, hard-coded protected name) never appears as a candidate"
# pgwt-gate-2 (the second persistent gate box) gets the exact same protection
# — a plain sed for "pgwt-gate" would false-negative-pass this check even if
# pgwt-gate-2's own protection were missing, since "pgwt-gate-2" also
# contains the substring "pgwt-gate"; check it explicitly.
check_not_contains "$out" "pgwt-gate-2" \
    "pgwt-gate-2 (unlabelled, hard-coded protected name) never appears as a candidate"

# ── Case 4b/4c: unknown age (missing/unparseable created=) is protected ──
# Review round 2 (issue #162): the first version of this guard scored an
# unparseable created= label as "infinitely old" (age = max_age + 1), which
# bypassed the protected-age floor and got the machine deleted on an
# ORDINARY sweep -- no --force-all, no low cutoff needed. Reproduced with
# exactly this shape: --dry-run --servers-file under a plain
# --max-age-hours 6. Unknown age must never be scored as old; it is
# skipped and warned about, like an unlabelled pgwt-dev-* server.
out_plain=$("$SWEEP" --dry-run --servers-file "$servers_file" --max-age-hours 6 2>&1)
check_not_contains "$out_plain" "deleting stale ephemeral server pgwt-dev-missing" \
    "a pgwt=ephemeral server with NO created= label is never deleted under a plain 6h sweep"
check_contains "$out_plain" "pgwt-dev-missing" \
    "the missing-label machine is at least mentioned (not silently vanished)"
check_not_contains "$out_plain" "deleting stale ephemeral server pgwt-dev-garbage" \
    "a pgwt=ephemeral server with an UNPARSEABLE created= label is never deleted under a plain 6h sweep"
check_contains "$out_plain" "pgwt-dev-garbage" \
    "the garbage-label machine is at least mentioned (not silently vanished)"

# pgwt-gate-2 (second persistent gate box): never a delete candidate under a
# plain, otherwise-valid sweep either.
check_not_contains "$out_plain" "deleting stale ephemeral server pgwt-gate-2" \
    "pgwt-gate-2 is never deleted under a plain 6h sweep"

# ── Case 5: --force-all with cutoff 0 does delete (the old one) ─────────
out=$("$SWEEP" --dry-run --servers-file "$servers_file" --max-age-hours 0 --force-all 2>&1)
rc=$?
check "$([[ $rc -eq 0 ]] && echo true || echo false)" "--force-all with cutoff 0 exits zero"
check_contains "$out" "deleting stale ephemeral server pgwt-dev-old" \
    "--force-all with cutoff 0 sweeps the 8h-old machine"

# Even under --force-all with cutoff 0 (which would otherwise treat "age
# >= 0" as stale for every server), the protected-age floor still applies
# to the 30s-old machine -- the exact incident this issue is about.
check_not_contains "$out" "deleting stale ephemeral server pgwt-dev-young" \
    "--force-all with cutoff 0 still never deletes the 30s-old machine (protected-age floor)"
check_contains "$out" "pgwt-dev-young" \
    "the 30s-old machine is at least mentioned (not silently vanished)"
check_contains "$out" "protected-age floor" \
    "the 30s-old machine is explicitly reported as protected, not silently skipped"

# Unknown-age machines are never deleted either, even under --force-all with
# cutoff 0 -- unknown age is not "old", so lowering the age bar changes
# nothing about it.
check_not_contains "$out" "deleting stale ephemeral server pgwt-dev-missing" \
    "--force-all with cutoff 0 still never deletes the missing-label machine"
check_not_contains "$out" "deleting stale ephemeral server pgwt-dev-garbage" \
    "--force-all with cutoff 0 still never deletes the garbage-label machine"

# pgwt-gate-2: the second persistent gate box. --force-all with a zero
# cutoff is the single most destructive combination this script accepts
# (it would otherwise treat "age >= 0" as stale for every unprotected
# ephemeral server) -- this is exactly the scenario the task's protection
# guarantee has to survive, not just an ordinary sweep.
check_not_contains "$out" "deleting stale ephemeral server pgwt-gate-2" \
    "--force-all with cutoff 0 still never deletes pgwt-gate-2 (hard-coded protected name)"

# ── Case 6: --servers-file without --dry-run is refused outright ────────
out=$("$SWEEP" --servers-file "$servers_file" --max-age-hours 6 2>&1)
rc=$?
check "$([[ $rc -ne 0 ]] && echo true || echo false)" \
    "--servers-file without --dry-run exits non-zero (refused, never reaches the live-API/token path)"
check_not_contains "$out" "^hetzner-sweep: done" \
    "--servers-file without --dry-run never reaches the summary line (nothing evaluated)"

echo
echo "test_hetzner_sweep: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
