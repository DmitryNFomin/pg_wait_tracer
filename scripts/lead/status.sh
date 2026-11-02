#!/usr/bin/env bash
# status.sh [path...] -- render the owner-facing status skeleton instead of
# typing it. Every line in here comes from a query this turn; the lead fills
# in only the free-text "my choice" reasons and the "needs you" section.
#
# Usage: scripts/lead/status.sh [path...]
#   Extra args are file paths to check with held.sh (optional, exact vocabulary
#   HELD/FREE straight from that script).
set -u
D="$(cd "$(dirname "$0")" && pwd)"
CACHE="$D/.sweep-latest.txt"
MAXAGE=600

age=999999
[ -s "$CACHE" ] && age=$(( $(date +%s) - $(stat -f %m "$CACHE" 2>/dev/null || echo 0) ))
stale=""
[ "$age" -ge "$MAXAGE" ] && stale=" (STALE -- run scripts/lead/sweep.sh)"

echo "=== LEAD STATUS $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="

if [ ! -s "$CACHE" ]; then
  echo "COUNTS: UNKNOWN -- no sweep cache. Run scripts/lead/sweep.sh first."
else
  f=$(/usr/bin/grep -acE '^ *FINDING: ' "$CACHE" 2>/dev/null || echo 0)
  c=$(/usr/bin/grep -acE '^ *CANDIDATE: ' "$CACHE" 2>/dev/null || echo 0)
  u=$(/usr/bin/grep -acE 'UNKNOWN' "$CACHE" 2>/dev/null || echo 0)
  printf 'COUNTS: %d findings, %d candidates, %d unknown (sweep %dm old%s)\n' "$f" "$c" "$u" $(( age / 60 )) "$stale"
fi

echo "RUNNING:"
if command -v pgrep >/dev/null 2>&1; then
  hits=$(pgrep -f 'codex exec' 2>/dev/null || true)
  if [ -n "$hits" ]; then
    echo "  codex: running ($(printf '%s' "$hits" | tr '\n' ' '))"
  else
    echo "  codex: idle"
  fi
else
  echo "  codex: UNKNOWN (pgrep not available)"
fi
live=$(git worktree list --porcelain 2>/dev/null | /usr/bin/awk '/^branch /{sub("refs/heads/","",$2); print $2}' | /usr/bin/grep '^agent/' || true)
if [ -n "$live" ]; then
  nlive=$(printf '%s\n' "$live" | /usr/bin/grep -c .)
  echo "  worktree: $nlive live agent/* branches (showing 10):"
  printf '%s\n' "$live" | head -10 | /usr/bin/sed 's/^/    /'
  [ "$nlive" -gt 10 ] && echo "    ... and $((nlive - 10)) more (git worktree list)"
else
  echo "  worktree: (none live)"
fi
if [ -s "$CACHE" ]; then
  /usr/bin/grep -aE '^ *BOX ' "$CACHE" 2>/dev/null | /usr/bin/sed 's/^ *BOX/  box:/'
fi

echo "TOP LEDGER ITEMS NOT RUNNING:"
if [ ! -s "$CACHE" ]; then
  echo "  UNKNOWN -- no sweep cache, this block cannot be computed. Never render this as \"(none)\"."
else
  ledger=$(/usr/bin/grep -aE '^ *FINDING: issue #[0-9]+ is READY-NOT-STARTED' "$CACHE" 2>/dev/null || true)
  if [ -z "$ledger" ]; then
    echo "  (none computed ready-not-started this sweep -- see COUNTS/UNKNOWN above; absence here is not the same as zero UNKNOWNs)"
  else
    printf '%s\n' "$ledger" | while IFS= read -r line; do
      item=$(printf '%s' "$line" | /usr/bin/sed -E 's/^[[:space:]]*FINDING:[[:space:]]*issue (#[0-9]+) is READY-NOT-STARTED -- /\1 /')
      echo "  READY-NOT-STARTED ${item} -- my choice: <fill in why this is not running>"
    done
  fi
fi

if [ "$#" -gt 0 ]; then
  echo "FILE RESERVATIONS (this turn's held.sh query):"
  "$D/held.sh" "$@" 2>&1 | /usr/bin/sed 's/^/  /'
fi

echo "NEEDS YOU: <free prose goes here only, nothing else>"
