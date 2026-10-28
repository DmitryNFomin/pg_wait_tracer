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

# Same purity guard, extended to the absence-claim pre-pass and the box/
# closing-ref evidence checker: grading an uncommitted copy of either would
# be exactly the vacuity the comment above describes, just for a different
# script.
for extra in scripts/lead/absence-prepass.py scripts/lead/check-box-evidence.sh; do
  if ! git ls-files --error-unmatch "$extra" >/dev/null 2>&1; then
    echo "SELFTEST REFUSED: $extra is not committed at HEAD." >&2
    exit 3
  fi
  if ! git diff --quiet HEAD -- "$extra"; then
    echo "SELFTEST REFUSED: $extra on disk differs from HEAD." >&2
    exit 4
  fi
done
if [ ! -x scripts/lead/check-box-evidence.sh ]; then
  echo "SELFTEST REFUSED: scripts/lead/check-box-evidence.sh is not executable on disk." >&2
  exit 6
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
