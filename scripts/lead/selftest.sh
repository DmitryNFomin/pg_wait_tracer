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
