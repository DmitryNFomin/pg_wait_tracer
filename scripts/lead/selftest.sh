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
