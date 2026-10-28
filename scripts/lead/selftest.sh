#!/usr/bin/env bash
# selftest.sh -- regression test for the Stop/SubagentStop blocking gate in
# sweep-hook.sh. Runs the COMMITTED script (git show HEAD:scripts/lead/sweep-hook.sh)
# against a synthetic, isolated cache -- never the live scripts/lead cache this
# repo's own agents are using -- so it never touches gh, ssh, codex, or this
# worktree's real .sweep-latest.txt / .dispositions.
#
# Usage: scripts/lead/selftest.sh   (exits non-zero on any failed case)
set -u
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null || echo "$(dirname "$0")/../..")" || exit 1

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
HOOK="$TMP/sweep-hook.sh"
git show HEAD:"scripts/lead/sweep-hook.sh" > "$HOOK" 2>/dev/null
if [ ! -s "$HOOK" ]; then
  echo "SELFTEST SETUP FAILED: 'git show HEAD:scripts/lead/sweep-hook.sh' returned nothing." >&2
  echo "This means the script is not committed at HEAD -- selftest refuses to fall" >&2
  echo "back to the working-tree copy, since that is exactly the vacuity this" >&2
  echo "project has been burned by before. Commit it, then re-run." >&2
  exit 3
fi
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

echo "== $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
