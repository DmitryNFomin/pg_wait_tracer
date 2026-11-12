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
#   BOX_EVIDENCE_CONTROL=1 also grades origin/master checkers with the new
#   history cases and requires newest-run provenance refusals to fail.
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
export PGWT_LEAD_STATE_DIR="$TMP"
export AUDIT_REPORT_STATE_DIR="$TMP/audit-state"
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

# ---- case 18: SubagentStop must leave dispositions byte-for-byte intact ---
# The marker cannot satisfy either finding, so exit 0 proves the gate was
# skipped rather than accidentally satisfied. Real payloads omit stop_active.
rm -f "$DQ"
write_cache
printf 'untouched marker content\n' > "$TMP/expected-dispositions"
cp "$TMP/expected-dispositions" "$DISP"
out=$(printf '%s' '{"hook_event_name": "SubagentStop"}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && cmp -s "$TMP/expected-dispositions" "$DISP"; then
  pass=$((pass+1)); echo "ok   SubagentStop passes and preserves dispositions byte-for-byte"
else
  fail=$((fail+1)); echo "FAIL SubagentStop consumed or changed dispositions (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
  cat "$DISP" 2>&1 | sed 's/^/       /'
fi

# No bare touch either: an absent file must stay absent.
rm -f "$DISP"
out=$(printf '%s' '{"hook_event_name": "SubagentStop"}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && [ ! -e "$DISP" ]; then
  pass=$((pass+1)); echo "ok   SubagentStop does not create a dispositions file"
else
  fail=$((fail+1)); echo "FAIL SubagentStop touched dispositions (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# ---- case 19: an explicitly named real Stop still consumes dispositions ---
write_cache
cat > "$DISP" <<EOF
MY-CHOICE fixture reason A [$IDA]
MY-CHOICE fixture reason B [$IDB]
EOF
out=$(printf '%s' '{"stop_hook_active": false, "hook_event_name": "Stop"}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && [ -f "$DISP" ] && [ ! -s "$DISP" ]; then
  pass=$((pass+1)); echo "ok   explicit Stop passes and consumes its dispositions"
else
  fail=$((fail+1)); echo "FAIL explicit Stop did not pass and consume dispositions (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
  cat "$DISP" 2>&1 | sed 's/^/       /'
fi

# ---- case 20 (cross-event ordering): after case 19 consumed the file, -----
# an intervening SubagentStop cannot grant another turn credit for it.
check "reorder: intervening SubagentStop passes with no new dispositions" 0 '{"hook_event_name": "SubagentStop"}'
check "reorder: next real Stop still blocks without new dispositions" 2 '{"stop_hook_active": false, "hook_event_name": "Stop"}'

# Explicit Stop continuations must still preserve the lead's response.
cp "$TMP/expected-dispositions" "$DISP"
out=$(printf '%s' '{"stop_hook_active": true, "hook_event_name": "Stop"}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && cmp -s "$TMP/expected-dispositions" "$DISP"; then
  pass=$((pass+1)); echo "ok   explicit Stop continuation preserves dispositions byte-for-byte"
else
  fail=$((fail+1)); echo "FAIL explicit Stop continuation consumed dispositions (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
  cat "$DISP" 2>&1 | sed 's/^/       /'
fi

# ---- case 21: HELD-BY needs independent evidence from the hook's sibling ---
# held.sh. The stub verifies the exact argument and records every call; no
# real gh calls occur, and unrelated dispositions must not invoke it.
cat > "$TMP/held.sh" <<'HELD_EOF'
#!/usr/bin/env bash
D="$(cd "$(dirname "$0")" && pwd)"
printf '%s\n' "$@" >> "$D/held-calls"
[ "$#" = 1 ] && [ "$1" = "some/file.c" ] || exit 3
cat "$D/held-output"
if [ -f "$D/held-fail" ]; then
  echo 'REFUSED: fixture gh failure' >&2
  exit 3
fi
HELD_EOF
chmod +x "$TMP/held.sh"
write_empty_cache; write_dq "$DQ_READY"
printf 'HELD some/file.c by #694\n' > "$TMP/held-output"
printf 'HELD-BY 694 some/file.c [issue-612]\n' > "$DISP"
check "escalated item with independently verified HELD-BY passes" 0 '{"stop_hook_active": false}'

# ---- case 22: FREE, the wrong PR, and REFUSED never satisfy HELD-BY --------
# Check both exit 2 and the missing item's message: another error must not
# make a negative control look like a correctly enforced escalation.
for held_case in free wrong-pr refused wrong-path pr-prefix malformed; do
  write_empty_cache; write_dq "$DQ_READY"
  printf 'HELD-BY 694 some/file.c [issue-612]\n' > "$DISP"
  rm -f "$TMP/held-fail"
  case "$held_case" in
    free) printf 'FREE some/file.c\n' > "$TMP/held-output" ;;
    wrong-pr) printf 'HELD some/file.c by #111\n' > "$TMP/held-output" ;;
    refused)
      # Even plausible stdout cannot override held.sh's non-zero exit.
      printf 'HELD some/file.c by #694\n' > "$TMP/held-output"
      touch "$TMP/held-fail" ;;
    wrong-path) printf 'HELD some/fileXc by #694\n' > "$TMP/held-output" ;;
    pr-prefix) printf 'HELD some/file.c by #6940\n' > "$TMP/held-output" ;;
    malformed)
      printf 'HELD-BY #694 some/file.c [issue-612]\n' > "$DISP"
      printf 'HELD some/file.c by #694\n' > "$TMP/held-output" ;;
  esac
  out=$(printf '%s' '{"stop_hook_active": false}' | "$HOOK" 2>&1)
  rc=$?
  if [ "$rc" = 2 ] && printf '%s' "$out" | /usr/bin/grep -q 'issue-612 issue#612.*needs RUNNING.*HELD-BY'; then
    pass=$((pass+1)); echo "ok   HELD-BY $held_case still blocks and names the missing item"
  else
    fail=$((fail+1)); echo "FAIL HELD-BY $held_case did not block the right item (rc=$rc)"
    printf '%s\n' "$out" | sed 's/^/       /'
  fi
done
rm -f "$TMP/held-fail" "$TMP/held-calls"

# ---- case 23: verify only relevant HELD-BY lines cause held.sh calls -------
write_empty_cache; write_dq "$DQ_READY"
printf 'HELD-BY 694 some/file.c [issue-650]\n' > "$DISP"
out=$(printf '%s' '{"stop_hook_active": false}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 2 ] && [ ! -e "$TMP/held-calls" ] && printf '%s' "$out" | /usr/bin/grep -q 'issue-612 issue#612.*needs RUNNING'; then
  pass=$((pass+1)); echo "ok   unrelated HELD-BY does not invoke held.sh or satisfy the escalated item"
else
  fail=$((fail+1)); echo "FAIL unrelated HELD-BY invoked held.sh or satisfied escalation (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi
printf 'HELD-BY 694 some/file.c [issue-612]\n' > "$DISP"
cp "$DISP" "$TMP/expected-dispositions"
out=$(printf '%s' '{"hook_event_name": "SubagentStop"}' | "$HOOK" 2>&1)
rc=$?
if [ "$rc" = 0 ] && [ ! -e "$TMP/held-calls" ] && cmp -s "$TMP/expected-dispositions" "$DISP"; then
  pass=$((pass+1)); echo "ok   SubagentStop skips escalation and held.sh, preserving dispositions"
else
  fail=$((fail+1)); echo "FAIL SubagentStop evaluated escalation or consumed dispositions (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
  cat "$DISP" 2>&1 | sed 's/^/       /'
fi

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

# case 10: a pasted command+output alone does not describe search coverage.
check_pp "same claim, with only pasted evidence nearby, blocks" 1 \
'There is no mechanical check for this today.

```
$ grep -rn "closing_issue" scripts/lead/*.sh
(no output)
```'

# Search coverage must name both the searched and unsearched scope.
PP_SEARCH_DRAFT='There is no mechanical check for this today.
SEARCHED: scripts/lead/*.sh for closing_issue
```
$ grep -c closing_issue scripts/lead/*.sh
0
```'
check_pp "grep-c=0 without NOT SEARCHED blocks" 1 "$PP_SEARCH_DRAFT"
if printf '%s' "$LAST_PP_OUT" | /usr/bin/grep -q 'line 1 .*NOT SEARCHED line missing'; then
  pass=$((pass+1)); echo "ok   missing NOT SEARCHED names the claim line"
else
  fail=$((fail+1)); echo "FAIL missing NOT SEARCHED did not name the claim line and requirement"
  printf '%s\n' "$LAST_PP_OUT" | sed 's/^/       /'
fi
check_pp "NOT SEARCHED none is vacuous" 1 "$PP_SEARCH_DRAFT
NOT SEARCHED: none"
check_pp "empty NOT SEARCHED is vacuous" 1 "$PP_SEARCH_DRAFT
NOT SEARCHED:"
check_pp "NOT SEARCHED n/a is vacuous" 1 "$PP_SEARCH_DRAFT
NOT SEARCHED: n/a"
check_pp "evidence with filled SEARCHED and NOT SEARCHED passes" 0 "$PP_SEARCH_DRAFT
NOT SEARCHED: tests/ and remote branches"
check_pp "nearby UNVERIFIED does not excuse another claim" 1 \
'There is no mechanical check for this today.
UNVERIFIED: The feature is not implemented.
SEARCHED: scripts/lead/*.sh for closing_issue
NOT SEARCHED: tests/ and remote branches'

for claim in \
  'The Generated-with trailer is absent everywhere, so the labels do not work.' \
  'there is no Generated-with trailer anywhere; the labels do not work'; do
  PP_TRAILER_DRAFT="$claim
\`\`\`
\$ git log --all --format=%B --grep=Generated-with
(no output)
\`\`\`"
  check_pp "$claim: evidence without search coverage blocks" 1 "$PP_TRAILER_DRAFT"
  check_pp "$claim: filled search coverage passes" 0 "$PP_TRAILER_DRAFT
SEARCHED: local git history for Generated-with trailers
NOT SEARCHED: remote branches and archived repositories"
done

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
# audit-report consumes only the CLI's final-answer file, not stdout.
while [ "\$#" -gt 0 ]; do
  if [ "\$1" = --output-last-message ]; then
    shift
    printf '%s\n' 'Everything here is measured and pasted. -> VERIFIED fixture evidence' 'VERDICT: SEND' > "\$1"
    break
  fi
  shift
done
EOSTUB
chmod +x "$STUBBIN/codex"
BLOCK_DRAFT="$TMP/block-draft-$$.md"
printf 'There is no mechanical check for this today.\n' > "$BLOCK_DRAFT"
rm -f "$MARKER"
out=$(PATH="$STUBBIN:$PATH" AUDIT_REPORT_STATE_DIR="$TMP/audit-state" bash scripts/lead/audit-report.sh "$BLOCK_DRAFT" 2>&1)
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
out=$(PATH="$STUBBIN:$PATH" AUDIT_REPORT_STATE_DIR="$TMP/audit-state" bash scripts/lead/audit-report.sh "$CLEAN_DRAFT" 2>&1)
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
# Real objects in an isolated bare repository, made with plumbing only:
# no staging/committing in the working repository (or even a scratch index).
TREE_REPO="$TMP/evidence.git"
"$REALGIT" init --bare -q "$TREE_REPO" || exit 5
tree_git() { "$REALGIT" -C "$TREE_REPO" "$@"; }
blob_a=$(printf 'original\n' | tree_git hash-object -w --stdin)
blob_b=$(printf 'changed\n' | tree_git hash-object -w --stdin)
sub_a=$(printf '100644 blob %s\tfile.txt\n' "$blob_a" | tree_git mktree)
sub_b=$(printf '100644 blob %s\tfile.txt\n' "$blob_b" | tree_git mktree)
TREE_BASE=$(printf '040000 tree %s\tdocs\n040000 tree %s\tsrc\n' "$sub_a" "$sub_a" | tree_git mktree)
TREE_SRC=$(printf '040000 tree %s\tdocs\n040000 tree %s\tsrc\n' "$sub_a" "$sub_b" | tree_git mktree)
TREE_DOCS=$(printf '040000 tree %s\tdocs\n040000 tree %s\tsrc\n' "$sub_b" "$sub_a" | tree_git mktree)
tree_commit() {
  GIT_AUTHOR_NAME=Selftest GIT_AUTHOR_EMAIL=selftest@example.invalid \
    GIT_COMMITTER_NAME=Selftest GIT_COMMITTER_EMAIL=selftest@example.invalid \
    tree_git commit-tree "$@"
}
TREE_BASE_SHA=$(printf 'base\n' | tree_commit "$TREE_BASE")
TREE_SAME_SHA=$(printf 'same tree, new commit\n' | tree_commit "$TREE_BASE" -p "$TREE_BASE_SHA")
TREE_SRC_SHA=$(printf 'src changed\n' | tree_commit "$TREE_SRC" -p "$TREE_BASE_SHA")
TREE_MISSING=ffffffffffffffffffffffffffffffffffffffff
if tree_git cat-file -e "$TREE_MISSING" 2>/dev/null; then
  echo "SELFTEST SETUP FAILED: supposedly missing object exists" >&2; exit 5
fi
cat > "$CBEBIN/git" <<EOSTUB
#!/usr/bin/env bash
# Both checkers use the production root; redirect only their git calls to
# the real scratch object database. The worktree listing stays synthetic.
if [ "\${1:-}" = "-C" ]; then shift 2; fi
if [ "\$1" = "worktree" ] && [ "\$2" = "list" ]; then
  cat "\$STUB_WT_PORCELAIN"
  exit 0
fi
if [ "\${STUB_DIFF_FAIL:-0}" = 1 ] && [ "\$1" = diff-tree ]; then exit 128; fi
exec "$REALGIT" -C "$TREE_REPO" "\$@"
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
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<EOF
box-check: commit=$TREE_BASE_SHA tree-hash=$TREE_BASE built-from=$TREE_BASE
EOF
json=$(mk_gh_json "$TREE_BASE_SHA" agent/lead-stuff "")
out=$(run_cbe json "$json"); rc=$?
if [ "$rc" = 0 ] && printf '%s' "$out" | /usr/bin/grep -q "^EVIDENCE-OK $TREE_BASE_SHA (tree $TREE_BASE)"; then
  pass=$((pass+1)); echo "ok   check-box-evidence: EVIDENCE-OK, no closing-ref check for non-numbered branch (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence EVIDENCE-OK case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 17: a src tree divergence is a finding.
printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SRC_SHA" "$TREE_BASE" "$TREE_BASE" > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log"
# Issue #42 is CLOSED so
# section [2] contributes nothing: this isolates the evidence check alone.
json=$(mk_gh_json "$TREE_SRC_SHA" agent/42-demo-fixture "")
out=$(run_cbe json "$json" "42:CLOSED"); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^EVIDENCE-TREE-DRIFT '; then
  pass=$((pass+1)); echo "ok   check-box-evidence: EVIDENCE-TREE-DRIFT on a src divergence (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence EVIDENCE-TREE-DRIFT case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 18: EVIDENCE-MISSING -- the thing being checked is ABSENT, not wrong.
# Must not be confused with EVIDENCE-OK or silence.
rm -f "$CBE_FIXTURE"/tests/results/*.log
json=$(mk_gh_json "$TREE_BASE_SHA" agent/lead-stuff "")
out=$(run_cbe json "$json"); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^EVIDENCE-MISSING$'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: EVIDENCE-MISSING when no log exists (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence EVIDENCE-MISSING case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<EOF
box-check: commit=$TREE_BASE_SHA tree-hash=$TREE_BASE built-from=$TREE_BASE
EOF

# case 19: NO-CLOSING-REF-CANDIDATE fires when the branch names an OPEN
# issue but closingIssuesReferences is empty.
json=$(mk_gh_json "$TREE_BASE_SHA" agent/42-demo-fixture "")
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
json=$(mk_gh_json "$TREE_BASE_SHA" agent/42-demo-fixture "42")
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
json=$(mk_gh_json "$TREE_BASE_SHA" agent/42-demo-fixture "")
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
json=$(mk_gh_json "$TREE_BASE_SHA" agent/55-66-vX "")
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
json=$(mk_gh_json "$TREE_BASE_SHA" agent/42-demo-fixture "")
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
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<EOF
box-check: commit=$TREE_BASE_SHA tree-hash=$TREE_BASE built-from=$TREE_BASE
EOF
chmod 000 "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log"
json=$(mk_gh_json "$TREE_BASE_SHA" agent/lead-stuff "")
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

# case 25: a matching-tip header with an unparseable tree -> UNKNOWN.
cat > "$CBE_FIXTURE/tests/results/box-check-ubuntu-20260101-000000.log" <<EOF
box-check: commit=$TREE_BASE_SHA tree-hash=not-hex built-from=not-hex
EOF
json=$(mk_gh_json "$TREE_BASE_SHA" agent/lead-stuff "")
out=$(run_cbe json "$json"); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 1 ] && printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN:'; then
  pass=$((pass+1)); echo "ok   check-box-evidence: unparseable tree-hash= header reports UNKNOWN (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL check-box-evidence unparseable-tree-header case (rc=$rc)"
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

# The same new cases run against each on-disk checker, and optionally the
# origin/master copies below (BOX_EVIDENCE_CONTROL=1). No duplicate control
# assertions: each control must fail its named provenance/history case.
tree_pass=0; tree_fail=0
tree_run() {
  local head="${1:-$TREE_SAME_SHA}"
  if [ "$tree_tool" = cbe ]; then
    local json
    json=$(mk_gh_json "$head" agent/lead-stuff "")
    out=$(run_cbe json "$json" 2>&1); rc=$?
  else
    beb_prs "[{\"number\":123,\"headRefOid\":\"$head\",\"headRefName\":\"agent/lead-stuff\",\"closingIssuesReferences\":[]}]"
    beb_issues ''; beb_wt_ok agent/lead-stuff "$BEB_FIX/wtA"
    out=$(run_beb); rc=$?
  fi
}
tree_log() {
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$1" "$2" "$2" > "$tree_logs/box-check-1.log"
}
tree_assert() {
  local desc="$1" want_rc="$2" pattern ok=1
  shift 2
  [ "$rc" = "$want_rc" ] || ok=0
  for pattern in "$@"; do
    if [ -n "$pattern" ]; then
      printf '%s' "$out" | /usr/bin/grep -Fq -- "$pattern" || ok=0
    else
      [ -z "$out" ] || ok=0
    fi
  done
  if [ "$ok" = 1 ]; then
    pass=$((pass+1)); tree_pass=$((tree_pass+1))
    echo "${TREE_CONTROL:+control }ok   $tree_tool tree evidence: $desc (exit $rc)"
  else
    fail=$((fail+1)); tree_fail=$((tree_fail+1))
    echo "${TREE_CONTROL:+control }FAIL $tree_tool tree evidence: $desc (rc=$rc, want $want_rc)"
    printf '%s\n' "$out" | sed 's/^/       /'
  fi
}
run_built_from_cases() {
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" "$TREE_SRC" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'foreign built-from is refused' 3 UNKNOWN: built-from= 'rerun box-check'

  printf 'box-check: commit=%s tree-hash=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'legacy missing built-from is refused' 3 UNKNOWN: built-from=missing 'rerun box-check'
}
run_history_cases() {
  local match_pattern=""
  [ "$tree_tool" != cbe ] || match_pattern="EVIDENCE-OK $TREE_SAME_SHA (tree $TREE_BASE)"
  rm -f "$tree_logs"/box-check-*.log
  tree_log "$TREE_BASE_SHA" "$TREE_BASE"
  tree_run
  if [ "$tree_tool" = cbe ]; then
    tree_assert 'tree-identical moved commit' 0 "EVIDENCE-OK $TREE_BASE_SHA (tree $TREE_BASE)"
  else
    tree_assert 'tree-identical moved commit' 0 ''
  fi
  printf 'box-check: commit=%s tree-hash=%s\n' "$TREE_BASE_SHA" "$TREE_BASE" > "$tree_logs/box-check-1.log"
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" "$TREE_BASE" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'old other-commit legacy log is ignored' 0 'ignored (pre-#861 format)' "${match_pattern:-ignored (pre-#861 format)}"

  printf 'box-check: commit=%s tree-hash=not-hex\n' "$TREE_BASE_SHA" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'old malformed legacy log is ignored' 0 'ignored (pre-#861 format)' "${match_pattern:-ignored (pre-#861 format)}"

  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_BASE_SHA" "$TREE_SRC" "$TREE_DOCS" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'old other-commit invalid provenance is ignored' 0 "$match_pattern"

  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_BASE_SHA" "$TREE_SRC" "$TREE_SRC" > "$tree_logs/box-check-3.log"
  tree_run
  tree_assert 'newer different tree cannot displace matching tree' 0 "$match_pattern"
  rm -f "$tree_logs/box-check-3.log"

  printf 'box-check: commit=%s tree-hash=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'old matching-tree legacy log is superseded' 0 'ignored (pre-#861 format)' "${match_pattern:-ignored (pre-#861 format)}"

  tree_log "$TREE_SAME_SHA" "$TREE_BASE"
  printf 'box-check: commit=%s tree-hash=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'newest matching-tree legacy log is refused' 3 UNKNOWN: built-from=missing

  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" "$TREE_SRC" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'newest matching-tree bad provenance cannot use older pass' 3 UNKNOWN: built-from=

  tree_log "$TREE_SAME_SHA" "$TREE_SRC"
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" "$TREE_BASE" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'newest matching-tree pass supersedes older drift' 0 "$match_pattern"

  printf 'old log without a certified header\n' > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'old headerless legacy log is ignored' 0 'ignored (pre-#861 format)' "${match_pattern:-ignored (pre-#861 format)}"
  tree_log "$TREE_SAME_SHA" "$TREE_BASE"
  printf 'box-check: source commit=%s tree-hash=%s (build provenance pending)\n' "$TREE_SAME_SHA" "$TREE_BASE" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'newest matching-tree pending build is refused' 3 UNKNOWN: built-from=missing
  rm -f "$tree_logs"/box-check-*.log

  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" "$TREE_SRC" > "$tree_logs/box-check-ubuntu-20260101-000000.log"
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" "$TREE_BASE" > "$tree_logs/box-check-el8-20260102-000000.log"
  tree_run
  tree_assert 'newest timestamp wins across OS prefixes' 0 "$match_pattern"
  rm -f "$tree_logs"/box-check-*.log
}
run_tree_evidence_cases() {
  local tree_tool="$1" tree_logs match_pattern diff_detail
  if [ "$tree_tool" = cbe ]; then
    tree_logs="$CBE_FIXTURE/tests/results"
    match_pattern="EVIDENCE-OK $TREE_SAME_SHA (tree $TREE_BASE)"
    diff_detail='src/file.txt'
  else
    tree_logs="$BEB_FIX/wtA/tests/results"
    match_pattern=''  # bulk OK stays silent
    diff_detail='check-box-evidence.sh 123 for the file list'
  fi
  mkdir -p "$tree_logs"
  rm -f "$tree_logs"/box-check-*.log

  tree_log "$TREE_BASE_SHA" "$TREE_BASE"
  tree_run
  if [ "$tree_tool" = cbe ]; then
    tree_assert 'tree-identical moved commit' 0 "EVIDENCE-OK $TREE_BASE_SHA (tree $TREE_BASE)"
  else
    tree_assert 'tree-identical moved commit' 0 ''
  fi

  tree_log "$TREE_SAME_SHA" "$TREE_BASE"
  tree_run
  tree_assert 'matching tip and tree' 0 "$match_pattern"

  run_built_from_cases
  run_history_cases

  # Same commit= as the tip, but a different tested tree: sha equality must
  # neither override a src finding nor suppress a docs-only note (#714).
  tree_log "$TREE_SAME_SHA" "$TREE_SRC"
  tree_run
  tree_assert 'src drift despite identical commit header' 1 EVIDENCE-TREE-DRIFT FINDING \
    "$diff_detail" "tip=$TREE_BASE logged=$TREE_SRC"

  tree_log "$TREE_SAME_SHA" "$TREE_DOCS"
  tree_run
  tree_assert 'docs-only drift is a note' 0 EVIDENCE-TREE-DRIFT NOTE

  tree_log "$TREE_SAME_SHA" "$TREE_MISSING"
  tree_run
  tree_assert 'unresolvable logged tree is a finding' 1 EVIDENCE-UNIDENTIFIED "$TREE_MISSING"

  rm -f "$tree_logs"/box-check-*.log
  tree_run
  tree_assert 'missing log stays a finding' 1 EVIDENCE-MISSING

  tree_log "$TREE_BASE_SHA" "$TREE_BASE"
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_SRC" "$TREE_SRC" > "$tree_logs/box-check-2.log"
  tree_run
  if [ "$tree_tool" = cbe ]; then
    tree_assert 'any older matching tree wins' 0 "EVIDENCE-OK $TREE_BASE_SHA (tree $TREE_BASE)"
  else
    tree_assert 'any older matching tree wins' 0 ''
  fi

  tree_log "$TREE_SAME_SHA" "$TREE_SRC"
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_DOCS" "$TREE_DOCS" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'newest filename selects the drift severity' 0 EVIDENCE-TREE-DRIFT "logged=$TREE_DOCS"
  rm -f "$tree_logs/box-check-2.log"

  printf 'box-check: commit=%s\n' "$TREE_SAME_SHA" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'missing tree header is UNKNOWN' 3 UNKNOWN: tree-hash=

  tree_log "$TREE_SAME_SHA" not-a-tree
  tree_run
  tree_assert 'malformed tree header is UNKNOWN' 3 UNKNOWN: tree-hash=

  tree_log "$TREE_MISSING" "$TREE_BASE"
  tree_run "$TREE_MISSING"
  tree_assert 'unresolvable tip is UNKNOWN even when commit matches' 3 UNKNOWN: 'tip tree'

  tree_log "$TREE_SAME_SHA" not-a-tree
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_BASE_SHA" "$TREE_BASE" "$TREE_BASE" > "$tree_logs/box-check-2.log"
  tree_run
  tree_assert 'malformed older header is not hidden by a matching log' 3 UNKNOWN: tree-hash=
  rm -f "$tree_logs/box-check-2.log"

  tree_log "$TREE_SAME_SHA" "$TREE_SRC"
  export STUB_DIFF_FAIL=1
  tree_run
  unset STUB_DIFF_FAIL
  tree_assert 'failed diff query is UNKNOWN' 3 UNKNOWN: compare

  printf 'box-check: commit=%s tree-hash=not-hex built-from=%s\n' "$TREE_SAME_SHA" "$TREE_BASE" > "$tree_logs/box-check-1.log"
  tree_run
  tree_assert 'matching-tree malformed tree remains UNKNOWN' 3 UNKNOWN: tree-hash=
}
run_tree_evidence_cases cbe

rm -rf "$CBE_FIXTURE" "$TMP/cbe-decoy"

# =============================================================================
# box-evidence-bulk.py -- sweep.sh's bulk equivalent of check-box-evidence.sh.
# Inputs are fixture files; local read-only git calls use the scratch repo
# through the same stub as the single-PR tests above.
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
run_beb() { PATH="$CBEBIN:$PATH" /usr/bin/python3 "$BEB" "$TMP/beb-prs.json" "$TMP/beb-issues.json" "$TMP/beb-wt.txt" 2>&1; }

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

# case 28: EVIDENCE-OK (log tree matches the tip tree) produces NO output -- a
# clean pass must not read the same as "nothing was checked".
printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_BASE_SHA" "$TREE_BASE" "$TREE_BASE" > "$BEB_FIX/wtA/tests/results/box-check-ubuntu-1.log"
beb_prs "[{\"number\":1,\"headRefOid\":\"$TREE_BASE_SHA\",\"headRefName\":\"agent/lead-stuff\",\"closingIssuesReferences\":[]}]"
beb_issues ''; beb_wt_ok agent/lead-stuff "$BEB_FIX/wtA"
out=$(run_beb); rc=$?
if [ "$rc" = 0 ] && [ -z "$out" ]; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: EVIDENCE-OK is silent (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk EVIDENCE-OK case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 29: a real src tree divergence is a finding.
printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SRC_SHA" "$TREE_BASE" "$TREE_BASE" > "$BEB_FIX/wtA/tests/results/box-check-ubuntu-1.log"
beb_prs "[{\"number\":2,\"headRefOid\":\"$TREE_SRC_SHA\",\"headRefName\":\"agent/lead-stuff\",\"closingIssuesReferences\":[]}]"
out=$(run_beb); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'EVIDENCE-TREE-DRIFT.*FINDING(src/tests)'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: EVIDENCE-TREE-DRIFT on a src divergence (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk EVIDENCE-TREE-DRIFT case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

# case 30: EVIDENCE-MISSING -- the branch has no worktree at all (absent,
# not wrong). Must not be confused with EVIDENCE-OK (silent) or TREE-DRIFT.
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
out=$(PATH="$CBEBIN:$PATH" /usr/bin/python3 "$BEB" "$TMP/beb-prs.json" "$TMP/beb-issues.json" "$TMP/beb-wt.txt" 2>&1); rc=$?
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
beb_prs "[{\"number\":6,\"headRefOid\":\"$TREE_SRC_SHA\",\"headRefName\":\"agent/lead-stuff\",\"closingIssuesReferences\":[]}]"
beb_wt_ok agent/lead-stuff "$BEB_FIX/wtA"
out=$(PATH="$CBEBIN:$PATH" /usr/bin/python3 "$BEB" "$TMP/beb-prs.json" "$TMP/beb-issues-missing.json" "$TMP/beb-wt.txt" 2>&1); rc=$?
if printf '%s' "$out" | /usr/bin/grep -q '^UNKNOWN: cannot read open-issue data' \
   && printf '%s' "$out" | /usr/bin/grep -q 'EVIDENCE-TREE-DRIFT'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: missing open-issues file degrades closing-ref only, evidence check still runs (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk missing-issues-file case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_BASE_SHA" "$TREE_BASE" "$TREE_BASE" > "$BEB_FIX/wtA/tests/results/box-check-ubuntu-1.log"
# case 35 (negative control for the closing-ref check): a branch naming an
# OPEN issue that IS in closingIssuesReferences must not be flagged.
beb_prs "[{\"number\":7,\"headRefOid\":\"$TREE_BASE_SHA\",\"headRefName\":\"agent/lead-stuff\",\"closingIssuesReferences\":[{\"number\":99}]}]"
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
beb_prs "[{\"number\":8,\"headRefOid\":\"$TREE_BASE_SHA\",\"headRefName\":\"agent/55-demo\",\"closingIssuesReferences\":[]}]"
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
beb_prs "[{\"number\":9,\"headRefOid\":\"$TREE_BASE_SHA\",\"headRefName\":\"agent/55-66-vX\",\"closingIssuesReferences\":[]}]"
beb_issues '55'; beb_wt_ok agent/55-66-vX "$BEB_FIX/wtA"
out=$(run_beb); rc=$?
if [ "$rc" = 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'issue #55' \
   && ! printf '%s' "$out" | /usr/bin/grep -q 'issue #66'; then
  pass=$((pass+1)); echo "ok   box-evidence-bulk: multi-issue branch flags only the open, uncovered one (exit $rc)"
else
  fail=$((fail+1)); echo "FAIL box-evidence-bulk multi-issue case (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi

run_tree_evidence_cases beb

# One bad local tip must not abort the other PRs in the bulk run; UNKNOWN
# still wins over any subsequent finding in the overall exit status.
run_beb_bad_tip_case() {
  local tree_tool=beb
  printf 'box-check: commit=%s tree-hash=%s built-from=%s\n' "$TREE_SAME_SHA" "$TREE_SRC" "$TREE_SRC" > "$BEB_FIX/wtA/tests/results/box-check-1.log"
  beb_prs "[{\"number\":1,\"headRefOid\":\"$TREE_MISSING\",\"headRefName\":\"agent/bad-tip\",\"closingIssuesReferences\":[]},{\"number\":2,\"headRefOid\":\"$TREE_SAME_SHA\",\"headRefName\":\"agent/good-tip\",\"closingIssuesReferences\":[]}]"
  beb_issues ''; beb_wt_ok agent/bad-tip "$BEB_FIX/wtA" agent/good-tip "$BEB_FIX/wtA"
  out=$(run_beb); rc=$?
  tree_assert 'bad tip degrades only its own PR' 3 'UNKNOWN: PR #1' 'CANDIDATE: PR #2 (agent/good-tip) EVIDENCE-TREE-DRIFT'
}
run_beb_bad_tip_case

# Disabling just the provenance check must make BOTH refusal cases fail by
# name in BOTH readers. Reuse the same fixtures/assertions, without network.
/usr/bin/python3 - "$CBE" "$BEB" "$TMP" <<'PY'
from pathlib import Path
import sys
for source, before, after, name in (
    (sys.argv[1], 'if [ -z "$b" ] || [ "$b" != "$t" ]; then', 'if false; then', 'unchecked-cbe.sh'),
    (sys.argv[2], 'if not b or b.group(1) != t.group(1):', 'if False:', 'unchecked-beb.py'),
):
    text = Path(source).read_text()
    assert text.count(before) == 1, f'cannot construct unique {name} control'
    Path(sys.argv[3], name).write_text(text.replace(before, after))
PY
if [ "$?" != 0 ]; then
  fail=$((fail+1)); echo 'FAIL cannot construct built-from negative controls'
else
  for control_tool in cbe beb; do
    if (
      CBE="$TMP/unchecked-cbe.sh"; BEB="$TMP/unchecked-beb.py"
      tree_tool="$control_tool"
      if [ "$tree_tool" = cbe ]; then tree_logs="$CBE_FIXTURE/tests/results"; else tree_logs="$BEB_FIX/wtA/tests/results"; fi
      mkdir -p "$tree_logs"; rm -f "$tree_logs"/box-check-*.log
      tree_pass=0; tree_fail=0; TREE_CONTROL=1
      run_built_from_cases
      [ "$tree_fail" = 2 ] && [ "$tree_pass" = 0 ]
    ); then
      pass=$((pass+1)); echo "ok   $control_tool built-from negative control: both refusal cases fail by name"
    else
      fail=$((fail+1)); echo "FAIL $control_tool built-from negative control escaped refusal cases"
    fi
  done
fi

# Mutate the selection policy independently in BOTH readers. Each control
# must fail the named assertion it is intended to protect, without network.
/usr/bin/python3 - "$CBE" "$BEB" "$TMP" <<'PYCONTROL'
from pathlib import Path
import subprocess
import sys
sources = [Path(sys.argv[1]), Path(sys.argv[2])]
mutations = (
    ('newest-only', '[ "$t" = "$TIP_TREE" ] || continue', ':',
     'if t.group(1) == tip_tree:', 'if True:'),
    ('commit-only', 'fallback="$log" # newest identified tree', '[ "$c" = "$HEAD_SHA" ] || continue; fallback="$log"',
     'fallback = (log, line)  # newest identified tree', 'if not line.startswith(f"box-check: commit={head} "):\n            continue\n        fallback = (log, line)'),
    ('oldest', 'selected="$log"', '[ -z "$selected" ] || continue; selected="$log"',
     'if t.group(1) == tip_tree:', 'if t.group(1) == tip_tree and selected is None:'),
    ('filename-sort', 'return (stamp.group(1) if stamp else name, name)', 'return (name, name)',
     'return (stamp.group(1) if stamp else name, name)', 'return (name, name)'),
    ('ignore-pending', 'if [ -z "$line" ]; then', 'if false; then',
     'if not lines:', 'if False:'),
    ('validate-history', '[ "$t" = "$TIP_TREE" ] || continue',
     'if [[ "$line" =~ tree-hash=([^[:space:]]+)[[:space:]]built-from=([^[:space:]]+) ]] && [[ "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}" ]]; then echo "UNKNOWN: old provenance"; exit 3; fi; [ "$t" = "$TIP_TREE" ] || continue',
     'if t.group(1) == tip_tree:',
     'old = re.search(r"tree-hash=(\\S+) built-from=(\\S+)", line)\n        if old and old.group(1) != old.group(2):\n            return ("UNKNOWN", "old provenance", False)\n        if t.group(1) == tip_tree:'),
    ('legacy-poisons', 'echo "$log ignored (pre-#861 format)"',
     'echo "UNKNOWN: legacy log"; exit 3',
     'print(f"   NOTE: PR #{pr.get(\'number\')} {log} ignored (pre-#861 format)")',
     'return ("UNKNOWN", "legacy log", False)'),
)
for name, sh_before, sh_after, py_before, py_after in mutations:
    for src, before, after, ext in ((sources[0], sh_before, sh_after, 'sh'),
                                    (sources[1], py_before, py_after, 'py')):
        text = src.read_text()
        assert text.count(before) == 1, (name, before)
        destination = Path(sys.argv[3], name + '.' + ext)
        mutant = text.replace(before, after)
        if ext == 'py':
            compile(mutant, str(destination), 'exec')
        destination.write_text(mutant)
        if ext == 'sh':
            subprocess.run(['bash', '-n', str(destination)], check=True)
PYCONTROL
if [ "$?" != 0 ]; then
  fail=$((fail+1)); echo 'FAIL cannot construct history negative controls'
else
  for control_tool in cbe beb; do
    for mutation in commit-only newest-only oldest legacy-poisons validate-history filename-sort ignore-pending; do
      case "$mutation" in
        commit-only) named='tree-identical moved commit' ;;
        newest-only) named='newer different tree cannot displace matching tree' ;;
        oldest) named='newest matching-tree legacy log is refused' ;;
        validate-history) named='old other-commit invalid provenance is ignored' ;;
        filename-sort) named='newest timestamp wins across OS prefixes' ;;
        ignore-pending) named='newest matching-tree pending build is refused' ;;
        legacy-poisons) named='old other-commit legacy log is ignored' ;;
      esac
      (
        CBE="$TMP/$mutation.sh"; BEB="$TMP/$mutation.py"
        tree_tool="$control_tool"
        if [ "$tree_tool" = cbe ]; then tree_logs="$CBE_FIXTURE/tests/results"; else tree_logs="$BEB_FIX/wtA/tests/results"; fi
        mkdir -p "$tree_logs"
        run_history_cases
      ) > "$TMP/history-control" 2>&1
      if /usr/bin/grep -Fq "FAIL $control_tool tree evidence: $named" "$TMP/history-control"; then
        pass=$((pass+1)); echo "ok   $control_tool $mutation negative control fails by name: $named"
      else
        fail=$((fail+1)); echo "FAIL $control_tool $mutation history negative control escaped"
        cat "$TMP/history-control"
      fi
    done
  done
fi

echo "== new tree-evidence cases (fixed): $tree_pass passed, $tree_fail failed =="
if [ "${BOX_EVIDENCE_CONTROL:-0}" = 1 ]; then
  if git show origin/master:scripts/lead/check-box-evidence.sh > "$TMP/master-check-box-evidence.sh" \
     && git show origin/master:scripts/lead/box-evidence-bulk.py > "$TMP/master-box-evidence-bulk.py"; then
    for control_tool in cbe beb; do
      (
        CBE="$TMP/master-check-box-evidence.sh"; BEB="$TMP/master-box-evidence-bulk.py"
        tree_tool="$control_tool"
        if [ "$tree_tool" = cbe ]; then tree_logs="$CBE_FIXTURE/tests/results"; else tree_logs="$BEB_FIX/wtA/tests/results"; fi
        mkdir -p "$tree_logs"
        run_history_cases
      ) > "$TMP/master-control" 2>&1
      if /usr/bin/grep -Fq "FAIL $control_tool tree evidence: newest matching-tree legacy log is refused" "$TMP/master-control" \
         && /usr/bin/grep -Fq "FAIL $control_tool tree evidence: newest matching-tree bad provenance cannot use older pass" "$TMP/master-control"; then
        pass=$((pass+1)); echo "ok   $control_tool origin/master control fails newest-run provenance refusals by name"
      else
        fail=$((fail+1)); echo "FAIL $control_tool origin/master must fail newest-run provenance refusals"
        cat "$TMP/master-control"
      fi
    done
  else
    fail=$((fail+1)); echo 'FAIL cannot load origin/master checkers for control'
  fi
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
