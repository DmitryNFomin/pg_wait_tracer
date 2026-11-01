#!/usr/bin/env bash
# selftest.sh -- regression test for the Stop/SubagentStop blocking gate in
# sweep-hook.sh. Exercises the ON-DISK working copy (the exact bytes Claude
# Code actually executes as the hook) against a synthetic, isolated cache --
# never the live scripts/lead cache this repo's own agents are using -- so it
# never touches gh, ssh, codex, or this worktree's real .sweep-latest.txt /
# .dispositions.
#
# Testing `git show HEAD:...` instead of the on-disk file was the vacuity a
# review caught directly: with the gate disabled by an UNCOMMITTED edit (the
# file that is actually wired in and executing), that version reported a
# clean pass, because it was grading the old, good, committed copy -- a
# dangerous edit never reaches a fresh clone, but Claude Code runs it RIGHT
# NOW. So this script tests on-disk bytes, and separately refuses (distinct
# exit code) when on-disk differs from HEAD at all: a clean pass against a
# dirty tree is not evidence that what ships (HEAD) is sound, whether the
# dirty edit happens to be better, worse, or merely different.
#
# Usage: scripts/lead/selftest.sh   (exits non-zero on any failed case)
#   exit 3: scripts/lead/sweep-hook.sh is not committed at HEAD at all
#   exit 4: on-disk differs from HEAD (cases below still ran, for diagnosis,
#           but this verdict cannot stand in for "HEAD is sound")
#   exit 6: scripts/lead/sweep-hook.sh is not executable on disk -- a mode
#           that cannot be the hook that ever runs, committed or not; refused
#           before copying rather than silently chmod +x'd into something
#           that would run in this sandbox but never in production.
set -u
ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null)"
[ -n "$ROOT" ] || { echo "SELFTEST SETUP FAILED: not inside a git repository." >&2; exit 5; }
cd "$ROOT" || exit 5
REL="scripts/lead/sweep-hook.sh"

if ! git ls-files --error-unmatch "$REL" >/dev/null 2>&1; then
  echo "SELFTEST REFUSED: $REL is not committed at HEAD." >&2
  echo "This means there is no shipped version to vouch for at all -- selftest" >&2
  echo "refuses to grade an on-disk-only script as if it were the product." >&2
  exit 3
fi

if [ ! -x "$REL" ]; then
  echo "SELFTEST REFUSED: $REL is not executable on disk." >&2
  echo "A non-executable mode -- whether dirty or committed to HEAD -- is a" >&2
  echo "hook that cannot run at all. Blindly chmod +x'ing a sandbox copy would" >&2
  echo "report a clean pass over a hook that never executes in production:" >&2
  echo "the same shape of bug this script exists to catch, in itself." >&2
  exit 6
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
HOOK="$TMP/sweep-hook.sh"
cp "$REL" "$HOOK"
chmod +x "$HOOK"

CACHE="$TMP/.sweep-latest.txt"
DISP="$TMP/.dispositions"

pass=0; fail=0
check() {
  local desc="$1" want_rc="$2" stdin_json="$3"
  local out rc
  out=$(printf '%s' "$stdin_json" | "$HOOK" 2>&1)
  rc=$?
  if [ "$rc" = "$want_rc" ]; then
    pass=$((pass+1)); echo "ok   $desc (exit $rc)"
  else
    fail=$((fail+1)); echo "FAIL $desc (want exit $want_rc, got $rc)"
    printf '%s\n' "$out" | sed 's/^/       /'
  fi
}

write_cache() {
  cat > "$CACHE" <<'CACHE_EOF'
== sweep-TEST ==
   FINDING: fixture-a needs attention
   FINDING: fixture-b needs attention
== end sweep-TEST ==
CACHE_EOF
}

id_of() { printf '%s' "$1" | shasum -a 256 | cut -c1-10; }
IDA=$(id_of "fixture-a needs attention")
IDB=$(id_of "fixture-b needs attention")

# ---- case 1: no cache at all -> NO DATA banner, exit 0, no crash ------------
rm -f "$CACHE" "$DISP"
check "no cache at all" 0 '{}'

# ---- case 2: findings present, no dispositions file -> BLOCK ---------------
write_cache; rm -f "$DISP"
check "findings with zero dispositions" 2 '{"stop_hook_active": false}'

# ---- case 3: both dispositions present -> PASS, and the file is consumed ---
write_cache
cat > "$DISP" <<EOF
MY-CHOICE fixture reason A [$IDA]
MY-CHOICE fixture reason B [$IDB]
EOF
check "both findings dispositioned" 0 '{"stop_hook_active": false}'
if [ -s "$DISP" ]; then
  fail=$((fail+1)); echo "FAIL dispositions file not consumed after a pass"
else
  pass=$((pass+1)); echo "ok   dispositions file consumed after a pass"
fi

# ---- case 4 (THE negative control): delete ONE disposition line -> BLOCK, --
# and the block must name exactly the finding that lost its line.
write_cache
cat > "$DISP" <<EOF
MY-CHOICE fixture reason A [$IDA]
EOF
out=$(printf '%s' '{"stop_hook_active": false}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 2 ] && printf '%s' "$out" | /usr/bin/grep -q "\[$IDB\]" && ! printf '%s' "$out" | /usr/bin/grep -q "MISSING.*\[$IDA\]"; then
  pass=$((pass+1)); echo "ok   deleting one disposition blocks and names only that finding"
else
  fail=$((fail+1)); echo "FAIL deleting one disposition did not block the right finding (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# ---- case 5: stop_hook_active=true bypasses even with zero dispositions ----
write_cache; rm -f "$DISP"
check "stop_hook_active=true never blocks (loop protection)" 0 '{"stop_hook_active": true}'

# ---- case 6: a malformed disposition line (wrong vocabulary) does not count
write_cache
cat > "$DISP" <<EOF
whatever fixture reason A [$IDA]
MY-CHOICE fixture reason B [$IDB]
EOF
check "malformed vocabulary line does not satisfy its finding" 2 '{"stop_hook_active": false}'

# ---- case 7: stop_hook_active=true must NOT consume dispositions -- a
# response the lead just wrote in reply to the block has not been evaluated
# yet and must survive to the next real (stop_hook_active=false) Stop.
write_cache
cat > "$DISP" <<EOF
MY-CHOICE fixture reason A [$IDA]
MY-CHOICE fixture reason B [$IDB]
EOF
check "stop_hook_active=true (bypass)" 0 '{"stop_hook_active": true}'
if [ -s "$DISP" ]; then
  pass=$((pass+1)); echo "ok   stop_hook_active=true did not consume the dispositions file"
else
  fail=$((fail+1)); echo "FAIL stop_hook_active=true ate the dispositions file before it was ever evaluated"
fi

# ---- case 8 (the reorder regression): dispositions present -> PASS -> file
# consumed -> a SECOND invocation against the SAME findings with no new
# dispositions written must BLOCK. Under the old bottom-of-function truncate
# this also passed (no crash in between two sequential calls), so this case
# does not by itself prove the crash window is closed -- it proves the
# observable half of the contract: the file can never be read twice for two
# different turns' worth of credit.
write_cache
cat > "$DISP" <<EOF
MY-CHOICE fixture reason A [$IDA]
MY-CHOICE fixture reason B [$IDB]
EOF
check "reorder: first invocation passes on fresh dispositions" 0 '{"stop_hook_active": false}'
check "reorder: second invocation, same findings, no new dispositions -> BLOCK" 2 '{"stop_hook_active": false}'

# =============================================================================
# The dispatch-queue escalation gate (sweep-hook.sh's ESC_N section). These
# use an EMPTY main cache throughout (write_empty_cache) specifically because
# an earlier draft's "if no findings, exit 0 immediately" short-circuit would
# have skipped escalation entirely whenever the top-5-capped FINDING list in
# the main cache happened to be empty -- case 9 below is the regression test
# for exactly that bug, not just a happy-path check.
DQ="$TMP/.dispatch-queue"
write_empty_cache() { printf '== sweep-TEST ==\n== end sweep-TEST ==\n' > "$CACHE"; }
write_dq() { printf '%s' "$1" > "$DQ"; }

DQ_READY='# dispatch-queue v1
STATUS OK
CAP running=0 cap=3
issue-612 612 4 Fix the thing
issue-650 650 1 Another thing'

DQ_AT_CAP='# dispatch-queue v1
STATUS OK
CAP running=3 cap=3
issue-612 612 4 Fix the thing'

DQ_UNKNOWN='# dispatch-queue v1
STATUS UNKNOWN gh issue query failed'

DQ_BELOW_N='# dispatch-queue v1
STATUS OK
CAP running=0 cap=3
issue-612 612 2 Fix the thing'

# ---- case 9 (THE regression test): escalated item, zero plain findings, ----
# MY-CHOICE only -> still BLOCKS. This is "item ready for N+1 scans with a
# MY-CHOICE disposition -> hook exits 2" from the brief.
write_empty_cache; write_dq "$DQ_READY"
printf 'MY-CHOICE not now [issue-612]\n' > "$DISP"
check "escalated item (count=4 >= N=3) with only MY-CHOICE blocks even with zero plain findings" 2 '{"stop_hook_active": false}'

# ---- case 10: same item, RUNNING disposition -> passes ---------------------
write_empty_cache; write_dq "$DQ_READY"
printf 'RUNNING agent/fix-612 [issue-612]\n' > "$DISP"
check "escalated item with RUNNING passes" 0 '{"stop_hook_active": false}'

# ---- case 11: BLOCKED-BY naming a REAL other queue id -> passes ------------
write_empty_cache; write_dq "$DQ_READY"
printf 'BLOCKED-BY issue-650 [issue-612]\n' > "$DISP"
check "escalated item with BLOCKED-BY naming a real queue id passes" 0 '{"stop_hook_active": false}'

# ---- case 12: BLOCKED-BY naming a NONEXISTENT id -> still blocks -----------
write_empty_cache; write_dq "$DQ_READY"
printf 'BLOCKED-BY issue-999999 [issue-612]\n' > "$DISP"
check "BLOCKED-BY naming an id absent from the queue does not satisfy it" 2 '{"stop_hook_active": false}'

# ---- case 13 (THE negative control for UNKNOWN != empty): the queue query --
# failed (STATUS UNKNOWN). The hook must say so, and must NOT silently pass
# as though the ready list were confirmed empty.
write_empty_cache; write_dq "$DQ_UNKNOWN"
rm -f "$DISP"
out=$(printf '%s' '{"stop_hook_active": false}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && printf '%s' "$out" | /usr/bin/grep -q "DISPATCH QUEUE: STATUS UNKNOWN"; then
  pass=$((pass+1)); echo "ok   failed queue query reads UNKNOWN visibly, not silently empty"
else
  fail=$((fail+1)); echo "FAIL failed queue query did not surface UNKNOWN (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# ---- case 14: no .dispatch-queue file at all -> visibly missing/stale, ----
# never read as empty. A missing file has no mtime, so it hits the dq_age
# staleness guard (age defaults to 999999) before the python parser even
# runs -- a different code path from case 13's STATUS UNKNOWN content, and
# both must be distinguishable from a silent "0 escalated".
write_empty_cache; rm -f "$DQ" "$DISP"
out=$(printf '%s' '{"stop_hook_active": false}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && printf '%s' "$out" | /usr/bin/grep -q "DISPATCH QUEUE: STALE or missing"; then
  pass=$((pass+1)); echo "ok   missing dispatch-queue file reads STALE-or-missing, not empty"
else
  fail=$((fail+1)); echo "FAIL missing dispatch-queue file did not surface STALE-or-missing (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# ---- case 15 (THE cap control): running == cap -> escalation PAUSES, a ----
# bare MY-CHOICE (no RUNNING/BLOCKED-BY) passes, and the pause is visible.
write_empty_cache; write_dq "$DQ_AT_CAP"
printf 'MY-CHOICE no headroom [issue-612]\n' > "$DISP"
out=$(printf '%s' '{"stop_hook_active": false}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && printf '%s' "$out" | /usr/bin/grep -q "DISPATCH ESCALATION PAUSED"; then
  pass=$((pass+1)); echo "ok   at cap (running=cap), escalation pauses instead of forcing an over-cap start"
else
  fail=$((fail+1)); echo "FAIL at-cap case did not pause (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# ---- case 16: below the N threshold -> MY-CHOICE still satisfies -----------
write_empty_cache; write_dq "$DQ_BELOW_N"
printf 'MY-CHOICE not yet [issue-612]\n' > "$DISP"
check "ready item below N=3 scans still accepts MY-CHOICE" 0 '{"stop_hook_active": false}'

# ---- case 17 (ordering control): a disposition from an earlier turn does ---
# not satisfy a later one -- the same queue, no new disposition, must block
# again after the previous pass consumed $DISP.
write_empty_cache; write_dq "$DQ_READY"
printf 'RUNNING agent/fix-612 [issue-612]\n' > "$DISP"
check "escalation: first turn's RUNNING passes" 0 '{"stop_hook_active": false}'
check "escalation: same queue, no new disposition this turn, blocks again" 2 '{"stop_hook_active": false}'

# =============================================================================
# absence-prepass.py -- the deterministic pre-pass audit-report.sh runs
# before any codex call. Tested directly (fast, no codex) and once through
# audit-report.sh with a stubbed codex, to prove the wiring skips the call.
# =============================================================================
PP="scripts/lead/absence-prepass.py"

LAST_PP_OUT=""
check_pp() {
  local desc="$1" want_rc="$2" content="$3"
  local f rc
  f="$TMP/pp-$$-$RANDOM.md"
  printf '%s\n' "$content" > "$f"
  LAST_PP_OUT=$(/usr/bin/python3 "$PP" "$f" 2>&1)
  rc=$?
  if [ "$rc" = "$want_rc" ]; then
    pass=$((pass+1)); echo "ok   $desc (exit $rc)"
  else
    fail=$((fail+1)); echo "FAIL $desc (want exit $want_rc, got $rc)"
    printf '%s\n' "$LAST_PP_OUT" | sed 's/^/       /'
  fi
}

# case 9: an absence claim with nothing pasted nearby -> BLOCK (exit 1), and
# the blocking line must actually be quoted, not just a bare verdict.
check_pp "unevidenced absence claim blocks" 1 \
  'There is no mechanical check for this today.'
if printf '%s' "$LAST_PP_OUT" | /usr/bin/grep -q 'no mechanical check'; then
  pass=$((pass+1)); echo "ok   block output quotes the offending phrase"
else
  fail=$((fail+1)); echo "FAIL block output did not quote the offending line"
fi

# case 10 (the negative control for case 9): the SAME claim, with a pasted
# command+output fenced block nearby, passes. This is what proves the gate
# discriminates instead of blocking (or passing) everything unconditionally.
check_pp "same claim, with pasted evidence nearby, passes" 0 \
'There is no mechanical check for this today.

```
$ grep -rn "closing_issue" scripts/lead/*.sh
(no output)
```'

# case 11: empty draft -> nothing to flag, exit 0 (not a crash, not a false
# BLOCK on an empty report).
check_pp "empty draft has nothing to flag" 0 ''

# case 12: absence-prepass.py itself given a nonexistent file -> exit 2
# (usage/IO error), never 0 and never silence.
out=$(/usr/bin/python3 "$PP" "$TMP/does-not-exist-$$.md" 2>&1); rc=$?
if [ "$rc" = 2 ]; then
  pass=$((pass+1)); echo "ok   absence-prepass.py on a missing file exits 2 (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL absence-prepass.py on a missing file (want exit 2, got $rc)"
fi

# case 13: wired through audit-report.sh with a stubbed codex -- a blocking
# draft must short-circuit to VERDICT: BLOCK WITHOUT ever invoking codex.
# A codex stub that touches a marker file proves the call was skipped, not
# merely that its output was discarded.
STUBBIN="$TMP/stubbin"; mkdir -p "$STUBBIN"
MARKER="$TMP/codex-was-called-$$"
cat > "$STUBBIN/codex" <<EOSTUB
#!/usr/bin/env bash
touch "$MARKER"
echo "VERDICT: SEND"
EOSTUB
chmod +x "$STUBBIN/codex"
BLOCK_DRAFT="$TMP/block-draft-$$.md"
printf 'There is no mechanical check for this today.\n' > "$BLOCK_DRAFT"
rm -f "$MARKER"
out=$(PATH="$STUBBIN:$PATH" bash scripts/lead/audit-report.sh "$BLOCK_DRAFT" 2>&1)
rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^VERDICT: BLOCK' && [ ! -e "$MARKER" ]; then
  pass=$((pass+1)); echo "ok   audit-report.sh pre-pass BLOCKs without calling codex (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL audit-report.sh pre-pass (rc=$rc marker_exists=$([ -e "$MARKER" ] && echo yes || echo no))"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 14 (negative control for 13): a clean draft DOES reach the stub codex.
CLEAN_DRAFT="$TMP/clean-draft-$$.md"
printf 'Everything here is measured and pasted.\n' > "$CLEAN_DRAFT"
rm -f "$MARKER"
out=$(PATH="$STUBBIN:$PATH" bash scripts/lead/audit-report.sh "$CLEAN_DRAFT" 2>&1)
rc=$?
if [ "$rc" = 0 ] && [ -e "$MARKER" ]; then
  pass=$((pass+1)); echo "ok   audit-report.sh calls codex for a clean draft (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL audit-report.sh did not call codex for a clean draft (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 15: a missing draft file is BLOCK, never a silent pass.
out=$(bash scripts/lead/audit-report.sh "$TMP/no-such-draft-$$.md" 2>&1); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^VERDICT: BLOCK'; then
  pass=$((pass+1)); echo "ok   audit-report.sh on a missing draft is BLOCK, not a pass (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL audit-report.sh on a missing draft (rc=$rc)"
fi

# =============================================================================
# check-box-evidence.sh -- standalone, unwired. git and gh are stubbed so
# this never touches the real repo's worktrees or GitHub.
# =============================================================================
CBE="scripts/lead/check-box-evidence.sh"
CBEBIN="$TMP/cbebin"; mkdir -p "$CBEBIN"
REALGIT="$(command -v git)"
cat > "$CBEBIN/git" <<EOSTUB
#!/usr/bin/env bash
if [ "\$1" = "worktree" ] && [ "\$2" = "list" ]; then
  cat "\$STUB_WT_PORCELAIN"
  exit 0
fi
exec "$REALGIT" "\$@"
EOSTUB
chmod +x "$CBEBIN/git"
cat > "$CBEBIN/gh" <<'EOSTUB'
#!/usr/bin/env bash
if [ "$1 $2" = "issue view" ]; then
  N="$3"
  state=""
  IFS=',' read -r -a pairs <<< "${STUB_ISSUE_STATES:-}"
  for p in "${pairs[@]}"; do
    k="${p%%:*}"; v="${p#*:}"
    [ "$k" = "$N" ] && { state="$v"; break; }
  done
  if [ -z "$state" ]; then
    echo "stub gh: issue view failure for #$N (no fixture state)" >&2
    exit 1
  fi
  echo "$state"
  exit 0
fi
case "$STUB_GH_MODE" in
  fail) echo "stub gh: simulated API failure" >&2; exit 1 ;;
  json) cat "$STUB_GH_JSON" ;;
  *) echo "stub gh: unhandled mode '$STUB_GH_MODE'" >&2; exit 1 ;;
esac
EOSTUB
chmod +x "$CBEBIN/gh"

CBE_FIXTURE="$TMP/cbe-fixture"; rm -rf "$CBE_FIXTURE"; mkdir -p "$CBE_FIXTURE/tests/results"
cat > "$TMP/cbe-wt.porcelain" <<EOF
worktree $TMP/cbe-decoy
HEAD 0000000000000000000000000000000000000000
branch refs/heads/agent/decoy-branch

worktree $CBE_FIXTURE
HEAD 1111111111111111111111111111111111111111
branch refs/heads/agent/42-demo-fixture

worktree $CBE_FIXTURE
HEAD 1111111111111111111111111111111111111111
branch refs/heads/agent/lead-stuff

worktree $CBE_FIXTURE
HEAD 1111111111111111111111111111111111111111
branch refs/heads/agent/55-66-vX
EOF

run_cbe() {
  local mode="$1" json="$2" states="${3:-}"
  PATH="$CBEBIN:$PATH" STUB_GH_MODE="$mode" STUB_GH_JSON="$json" STUB_ISSUE_STATES="$states" \
    STUB_WT_PORCELAIN="$TMP/cbe-wt.porcelain" bash "$CBE" 123
}

mk_gh_json() {
  # $1 = headRefOid, $2 = headRefName, $3 = comma-separated closing issue numbers ("" = none)
  local f="$TMP/cbe-gh-$$-$RANDOM.json"
  local refs="[]"
  if [ -n "$3" ]; then
    refs="["
    local first=1
    IFS=',' read -r -a nums <<< "$3"
    for n in "${nums[@]}"; do
      [ "$first" = 1 ] || refs="$refs,"
      refs="$refs{\"number\":$n}"
      first=0
    done
    refs="$refs]"
  fi
  printf '{"headRefOid":"%s","headRefName":"%s","closingIssuesReferences":%s}\n' "$1" "$2" "$refs" > "$f"
  printf '%s' "$f"
}

# case 16: EVIDENCE-OK, no NO-CLOSING-REF (branch does not match agent/N-) -> exit 0
rm -f "$CBE_FIXTURE"/tests/results/*.log
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<'EOF'
box-check: commit=1111111111111111111111111111111111111111 tree-hash=abc
EOF
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/lead-stuff "")
out=$(run_cbe json "$json"); rc=$?
if [ "$rc" = 0 ] && printf '%s' "$out" | /usr/bin/grep -q '^EVIDENCE-OK 1111'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: EVIDENCE-OK, no closing-ref check for non-numbered branch (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence EVIDENCE-OK case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 17 (the real bug this script exists for): EVIDENCE-STALE when the log's
# commit= does not match HEAD -- e.g. a gh pr update-branch merge commit ran
# after the last box-check. Issue #42 is fixtured CLOSED so section [2]
# contributes nothing here -- this case isolates the evidence check alone.
json=$(mk_gh_json 2222222222222222222222222222222222222222 agent/42-demo-fixture "")
out=$(run_cbe json "$json" "42:CLOSED"); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^EVIDENCE-STALE 1111.*!= 2222'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: EVIDENCE-STALE on a mismatched commit= header (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence EVIDENCE-STALE case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 18: EVIDENCE-MISSING -- the thing being checked is ABSENT, not wrong.
# Must not be confused with EVIDENCE-OK or silence.
rm -f "$CBE_FIXTURE"/tests/results/*.log
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/lead-stuff "")
out=$(run_cbe json "$json"); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^EVIDENCE-MISSING$'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: EVIDENCE-MISSING when no log exists (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence EVIDENCE-MISSING case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<'EOF'
box-check: commit=1111111111111111111111111111111111111111 tree-hash=abc
EOF

# case 19: NO-CLOSING-REF-CANDIDATE fires when the branch names an OPEN
# issue but closingIssuesReferences is empty.
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/42-demo-fixture "")
out=$(run_cbe json "$json" "42:OPEN"); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^NO-CLOSING-REF-CANDIDATE #123 -> issue #42 '; then
  pass=$((pass+1)); echo "ok   check-box-evidence: candidate fires on an empty ref list for an OPEN issue (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence candidate-fires case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 20 (negative control for 19): the SAME branch name, but the issue IS
# in closingIssuesReferences -- the check must not flag a PR that has one,
# and must not even need to query the issue's state to know that (no entry
# in STUB_ISSUE_STATES for 42 here).
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/42-demo-fixture "42")
out=$(run_cbe json "$json" ""); rc=$?
if [ "$rc" = 0 ] && ! printf '%s' "$out" | /usr/bin/grep -q 'NO-CLOSING-REF'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: a real closing ref is not flagged (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence should not have flagged a PR with a real ref (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 20b (the real bug this round fixes): the branch names an issue with
# an empty ref list, but that issue is already CLOSED -- nothing to close,
# so this must NOT be flagged. Live data before this fix: 2 of 4 flagged
# issues (#390, #230) were exactly this case.
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/42-demo-fixture "")
out=$(run_cbe json "$json" "42:CLOSED"); rc=$?
if [ "$rc" = 0 ] && ! printf '%s' "$out" | /usr/bin/grep -q 'NO-CLOSING-REF'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: a closed issue with no ref is not flagged (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence flagged an already-closed issue (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 20c: a multi-issue branch (agent/507-490-v2 shape) must check EVERY
# leading numeric token, not just the first -- and only the OPEN one with no
# ref is a candidate; the CLOSED one must be silent.
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/55-66-vX "")
out=$(run_cbe json "$json" "55:OPEN,66:CLOSED"); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'issue #55' \
   && ! printf '%s' "$out" | /usr/bin/grep -q 'issue #66'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: multi-issue branch flags only the open, uncovered one (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence multi-issue case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 20d: the issue-state lookup itself fails (gh error / unknown issue)
# -> UNKNOWN, exit non-zero, never silently treated as open or closed.
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/42-demo-fixture "")
out=$(run_cbe json "$json" ""); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: issue-state lookup failure reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence issue-state-failure case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 21: gh failing outright (API error) -> UNKNOWN, exit non-zero. An
# empty finding list and a failed query must never look alike.
out=$(run_cbe fail ""); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: gh failure reports UNKNOWN, not OK (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence gh-failure case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 22: gh missing entirely from PATH (dependency exiting 127) -> UNKNOWN,
# not a silent pass. Uses a PATH with no gh stub at all.
out=$(PATH="/usr/bin:/bin" STUB_WT_PORCELAIN="$TMP/cbe-wt.porcelain" bash "$CBE" 123 2>&1); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: gh missing from PATH (127) reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence gh-missing case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 23: malformed JSON (missing closingIssuesReferences field) -> UNKNOWN,
# an unparseable field never silently becomes "no findings".
badjson="$TMP/cbe-bad-$$.json"
printf '{"headRefOid":"1111111111111111111111111111111111111111","headRefName":"agent/42-demo-fixture"}\n' > "$badjson"
out=$(run_cbe json "$badjson"); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: malformed PR JSON reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence malformed-JSON case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 24: an unreadable log file -> UNKNOWN, not silently skipped as if it
# did not exist (that would collapse "unreadable" into "absent").
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<'EOF'
box-check: commit=1111111111111111111111111111111111111111 tree-hash=abc
EOF
chmod 000 "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log"
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/lead-stuff "")
if [ "$(id -u)" = "0" ]; then
  echo "skip check-box-evidence unreadable-log case: running as root, chmod 000 has no effect"
else
  out=$(run_cbe json "$json"); rc=$?
  if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
    pass=$((pass+1)); echo "ok   check-box-evidence: unreadable log reports UNKNOWN (exit $rc)"
  else
    fail=$((fail+1)); echo "FAIL check-box-evidence unreadable-log case (rc=$rc)"
    printf '%s\n' "$out" | sed 's/^/       /'
  fi
fi
chmod 644 "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" 2>/dev/null

# case 25: a log with no parseable commit= header -> UNKNOWN, not silently
# treated as a mismatch or a pass.
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<'EOF'
some unrelated log line
EOF
json=$(mk_gh_json 1111111111111111111111111111111111111111 agent/lead-stuff "")
out=$(run_cbe json "$json"); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: unparseable commit= header reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence unparseable-header case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 26: non-numeric PR argument -> UNKNOWN before any gh call is made.
out=$(PATH="$CBEBIN:$PATH" STUB_GH_MODE="fail" STUB_GH_JSON="" \
      STUB_WT_PORCELAIN="$TMP/cbe-wt.porcelain" bash "$CBE" not-a-number 2>&1); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: non-numeric PR argument reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence bad-argument case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

rm -rf "$CBE_FIXTURE" "$TMP/cbe-decoy"

# =============================================================================
# box-evidence-bulk.py -- sweep.sh's bulk equivalent of check-box-evidence.sh
# (section [10]). Pure: it makes no gh/git calls of its own, so these tests
# feed it fixture files directly -- no process stubbing needed, and no path
# by which a stubbed dependency exiting 126/127 could hide inside THIS
# script (that risk lives in sweep.sh's own `gh pr list` / `git worktree
# list` calls, which already degrade via the same `|| echo UNKNOWN` pattern
# every other bulk call in sweep.sh uses).
# =============================================================================
BEB="scripts/lead/box-evidence-bulk.py"
BEB_FIX="$TMP/beb-fixtures"; mkdir -p "$BEB_FIX/wtA/tests/results" "$BEB_FIX/wtB/tests/results"

beb_prs() {  # $1 = JSON array of PR objects
  printf '%s' "$1" > "$TMP/beb-prs.json"
}
beb_issues() {  # $1 = comma-separated open issue numbers, "" for none
  local out="[]"
  if [ -n "$1" ]; then
    out="["
    local first=1
    IFS=',' read -r -a ns <<< "$1"
    for n in "${ns[@]}"; do
      [ "$first" = 1 ] || out="$out,"
      out="$out{\"number\":$n}"
      first=0
    done
    out="$out]"
  fi
  printf '%s' "$out" > "$TMP/beb-issues.json"
}
beb_wt_ok() {  # writes a real porcelain dump: $1 branch -> $2 path, repeated pairs
  : > "$TMP/beb-wt.txt"
  while [ $# -ge 2 ]; do
    {
      printf 'worktree %s\n' "$2"
      printf 'HEAD 1111111111111111111111111111111111111111\n'
      printf 'branch refs/heads/%s\n\n' "$1"
    } >> "$TMP/beb-wt.txt"
    shift 2
  done
}
run_beb() { /usr/bin/python3 "$BEB" "$TMP/beb-prs.json" "$TMP/beb-issues.json" "$TMP/beb-wt.txt" 2>&1; }

# case 27 (empty input is absence, not a crash): zero open PRs -> exit 0,
# no output at all. The "thing being checked is absent" case this project's
# bypass suite calls out by name, kept distinguishable from every error path
# below by printing NOTHING, not an empty-looking finding.
beb_prs '[]'; beb_issues ''; beb_wt_ok
out=$(run_beb); rc=$?
if [ "$rc" = 0 ] && [ -z "$out" ]; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: zero open PRs is a silent, clean exit 0"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk zero-PRs case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 28: EVIDENCE-OK (log commit matches HEAD) produces NO output -- a
# clean pass must not read the same as "nothing was checked".
printf 'box-check: commit=aaaa0000 tree-hash=x\n' > "$BEB_FIX/wtA/tests/results/box-check-ubuntu-1.log"
beb_prs '[{"number":1,"headRefOid":"aaaa0000","headRefName":"agent/lead-stuff","closingIssuesReferences":[]}]'
beb_issues ''; beb_wt_ok agent/lead-stuff "$BEB_FIX/wtA"
out=$(run_beb); rc=$?
if [ "$rc" = 0 ] && [ -z "$out" ]; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: EVIDENCE-OK is silent (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk EVIDENCE-OK case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 29 (the real bug class): EVIDENCE-STALE when the log's commit does
# not match headRefOid (e.g. a gh pr update-branch merge commit ran after
# the last box-check).
beb_prs '[{"number":2,"headRefOid":"bbbb9999","headRefName":"agent/lead-stuff","closingIssuesReferences":[]}]'
out=$(run_beb); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'EVIDENCE-STALE aaaa0000 != bbbb9999'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: EVIDENCE-STALE on a mismatched commit (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk EVIDENCE-STALE case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 30: EVIDENCE-MISSING -- the branch has no worktree at all (absent,
# not wrong). Must not be confused with EVIDENCE-OK (silent) or STALE.
beb_prs '[{"number":3,"headRefOid":"cccc1111","headRefName":"agent/no-worktree","closingIssuesReferences":[]}]'
out=$(run_beb); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'EVIDENCE-MISSING'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: EVIDENCE-MISSING when no worktree/log exists (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk EVIDENCE-MISSING case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 31 (THE negative control for "wired, not merely present"): the exact
# same fixture as case 30, but the branch is NOT agent/* -- must be silently
# skipped. Proves the check discriminates by branch prefix instead of
# flagging every open PR regardless of name.
beb_prs '[{"number":4,"headRefOid":"cccc1111","headRefName":"not-an-agent-branch","closingIssuesReferences":[]}]'
out=$(run_beb); rc=$?
if [ "$rc" = 0 ] && [ -z "$out" ]; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: a non-agent/* branch is not evaluated at all"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk non-agent-branch case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 32: malformed/missing open-PR JSON -> UNKNOWN, exit 3. An unparseable
# input file must never look like "zero PRs" (case 27).
printf 'not json at all' > "$TMP/beb-prs.json"
out=$(run_beb); rc=$?
if [ "$rc" = 3 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN: cannot read open-PR data'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: malformed open-PR JSON reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk malformed-PR-JSON case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 33: the worktree-list query itself failed (sentinel) -- evidence
# checks are skipped and said so, but the closing-ref check (independent of
# worktrees) still runs rather than going silent along with it.
beb_prs '[{"number":5,"headRefOid":"dddd2222","headRefName":"agent/42-demo","closingIssuesReferences":[]}]'
beb_issues '42'
printf 'WORKTREE-LIST-FAILED\n' > "$TMP/beb-wt.txt"
out=$(/usr/bin/python3 "$BEB" "$TMP/beb-prs.json" "$TMP/beb-issues.json" "$TMP/beb-wt.txt" 2>&1); rc=$?
if printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN: git worktree list failed' \
   && printf '%s' "$out" | /usr/bin/grep -q 'NO-CLOSING-REF'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: worktree-list failure degrades evidence only, closing-ref check still runs (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk worktree-list-failed case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 34: open-issues.json missing entirely -> UNKNOWN once, but evidence
# checking for OTHER PRs still runs (a partial failure must not silence
# everything else this run could have told the lead).
rm -f "$TMP/beb-issues-missing.json"
beb_prs '[{"number":6,"headRefOid":"bbbb9999","headRefName":"agent/lead-stuff","closingIssuesReferences":[]}]'
beb_wt_ok agent/lead-stuff "$BEB_FIX/wtA"
out=$(/usr/bin/python3 "$BEB" "$TMP/beb-prs.json" "$TMP/beb-issues-missing.json" "$TMP/beb-wt.txt" 2>&1); rc=$?
if printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN: cannot read open-issue data' \
   && printf '%s' "$out" | /usr/bin/grep -q 'EVIDENCE-STALE'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: missing open-issues file degrades closing-ref only, evidence check still runs (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk missing-issues-file case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 35 (negative control for the closing-ref check): a branch naming an
# OPEN issue that IS in closingIssuesReferences must not be flagged.
beb_prs '[{"number":7,"headRefOid":"aaaa0000","headRefName":"agent/lead-stuff","closingIssuesReferences":[{"number":99}]}]'
beb_issues '99'; beb_wt_ok agent/lead-stuff "$BEB_FIX/wtA"
out=$(run_beb); rc=$?
if [ "$rc" = 0 ] && [ -z "$out" ]; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: a real closing ref is not flagged (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk real-closing-ref case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 36: a CLOSED issue (absent from the open-issues list) with no
# closing ref must NOT be flagged -- nothing is left to close. Live data
# before check-box-evidence.sh drew this distinction: 2 of 4 flagged issues
# were exactly this case (#390, #230).
beb_prs '[{"number":8,"headRefOid":"aaaa0000","headRefName":"agent/55-demo","closingIssuesReferences":[]}]'
beb_issues ''; beb_wt_ok agent/55-demo "$BEB_FIX/wtA"
out=$(run_beb); rc=$?
if [ "$rc" = 0 ] && ! printf '%s' "$out" | /usr/bin/grep -q 'NO-CLOSING-REF'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: a closed issue with no ref is not flagged (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk closed-issue case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 37: a multi-issue branch (agent/55-66-vX shape) flags only the OPEN,
# uncovered one -- every leading numeric token is parsed, not just the first.
beb_prs '[{"number":9,"headRefOid":"aaaa0000","headRefName":"agent/55-66-vX","closingIssuesReferences":[]}]'
beb_issues '55'; beb_wt_ok agent/55-66-vX "$BEB_FIX/wtA"
out=$(run_beb); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'issue #55' \
   && ! printf '%s' "$out" | /usr/bin/grep -q 'issue #66'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: multi-issue branch flags only the open, uncovered one (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk multi-issue case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

rm -rf "$BEB_FIX"

echo "== $pass passed, $fail failed =="

if ! git diff --quiet HEAD -- "$REL"; then
  echo "SELFTEST REFUSED: $REL on disk differs from HEAD." >&2
  echo "The cases above ran against the ON-DISK copy (the one actually" >&2
  echo "executing right now), but that is not evidence about HEAD -- what" >&2
  echo "ships, what a reviewer reads, what a fresh clone gets. Commit or" >&2
  echo "revert so the tree is clean, then re-run." >&2
  exit 4
fi

[ "$fail" -eq 0 ]
