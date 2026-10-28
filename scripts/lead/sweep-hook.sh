#!/usr/bin/env bash
# Stop / SubagentStop hook. Refreshes SYNCHRONOUSLY when the cache is stale,
# then BLOCKS the stop when the sweep found a FINDING with no disposition
# recorded this turn.
#
# Why synchronous: four attempts to spawn a surviving background refresh all
# failed on macOS (plain &, pid-locked &, mkdir-locked & trap, nohup+disown) --
# a hook's children die with its process group, and the detached variants hung.
# The real problem was that the reconciler took 2-5 min because it issued one
# API call per branch. It now takes ~8s, so the hook just runs it and the
# harness supervises the process, which is the one thing that has always worked.
#
# Failure behaviour: on timeout or non-zero exit the OLD cache is kept and the
# banner says so. A stale cache is a finding, never silently-served old data.
#
# Blocking: a hook on Stop/SubagentStop controls continuation only through its
# EXIT CODE -- exit 2 means "prevent the stop, continue the conversation" and
# the lead sees this script's stderr as the reason. stdin carries the event's
# JSON (see https://code.claude.com/docs/en/hooks -- "Stop input"); its
# stop_hook_active field is true exactly when this Stop event is itself a
# continuation Claude Code produced because an earlier Stop hook blocked. We
# MUST honour that field and exit 0 unconditionally when it is true: a hook
# that blocks every time traps the session (the harness caps at 8 consecutive
# blocks and then forces the stop anyway, but that is a crash rail, not a
# design). One block per genuine stop is the contract: it blocks once, the
# lead writes a disposition, and the next Stop (stop_hook_active=true) goes
# through regardless of whether that disposition covers everything.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
CACHE="$D/.sweep-latest.txt"; LOG="$D/.sweep.log"; MAXAGE=600   # 10 min
DISP="$D/.dispositions"

input="$(cat)"
stop_active=$(printf '%s' "$input" | /usr/bin/python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    d = {}
print("true" if d.get("stop_hook_active") is True else "false")
' 2>/dev/null)
[ "$stop_active" = "true" ] || stop_active=false

age=999999
[ -s "$CACHE" ] && age=$(( $(date +%s) - $(stat -f %m "$CACHE" 2>/dev/null || echo 0) ))

if [ "$age" -ge "$MAXAGE" ]; then
  start=$(date -u +%H:%M:%SZ)
  if timeout 40 "$D/sweep.sh" > "$CACHE.tmp" 2>&1 && [ -s "$CACHE.tmp" ]; then
    /usr/bin/tr -d '\000' < "$CACHE.tmp" > "$CACHE" && rm -f "$CACHE.tmp"
    echo "$start refresh ok" >> "$LOG"; age=0
  else
    rc=$?; rm -f "$CACHE.tmp"
    echo "$start refresh FAILED rc=$rc" >> "$LOG"
    printf '!! SWEEP REFRESH FAILED (rc=%s) -- the cache below is %dm old, not current.\n' "$rc" $(( age / 60 ))
  fi
fi

if [ ! -s "$CACHE" ]; then
  printf '!! LEAD SWEEP: NO DATA and the refresh did not produce any.\n'
  printf '!! You may NOT claim nothing is idle, stranded or unclosed. Run:\n'
  printf '!!   scripts/lead/sweep.sh\n'
  exit 0
fi

printf '=== LEAD SWEEP (%dm old) ===\n' $(( age / 60 ))
n=$(/usr/bin/grep -acE 'FINDING|CANDIDATE|UNKNOWN' "$CACHE" 2>/dev/null || echo 0)
/usr/bin/grep -aE 'FINDING|UNKNOWN' "$CACHE" 2>/dev/null | head -15
c=$(/usr/bin/grep -acE 'CANDIDATE' "$CACHE" 2>/dev/null || echo 0)
[ "$c" -gt 0 ] && printf '   (+%d CANDIDATE lines -- a mention is not a fix; verify content on master)\n' "$c"
printf '=== %d open items. Act on them or say why not. ===\n' "$n"

# ---- the blocking gate ------------------------------------------------------
# Loop protection first, per the docs cited above: never re-evaluate once
# Claude Code is already continuing because of an earlier block this turn.
if [ "$stop_active" = "true" ]; then
  : > "$DISP" 2>/dev/null || true   # consumed: this chain already had its one block
  exit 0
fi

findings=$(/usr/bin/grep -aE '^ *FINDING: ' "$CACHE" 2>/dev/null || true)
if [ -z "$findings" ]; then
  : > "$DISP" 2>/dev/null || true
  exit 0
fi

touch "$DISP" 2>/dev/null || true
missing=""
nmissing=0
while IFS= read -r line; do
  [ -n "$line" ] || continue
  text=$(printf '%s' "$line" | /usr/bin/sed -E 's/^[[:space:]]*FINDING:[[:space:]]*//')
  id=$(printf '%s' "$text" | shasum -a 256 | cut -c1-10)
  # A disposition counts only when it has the right vocabulary AND names this
  # finding's id -- quantity alone (e.g. one MY-CHOICE line for N findings)
  # must not satisfy N findings.
  if /usr/bin/grep -aqE "^(ACTED|MY-CHOICE|UNKNOWN)[[:space:]].*\[$id\]" "$DISP" 2>/dev/null; then
    continue
  fi
  nmissing=$((nmissing+1))
  missing="${missing}[$id] ${text}
"
done <<EOF
$findings
EOF

if [ "$nmissing" -gt 0 ]; then
  {
    printf 'LEAD SWEEP BLOCK: %d finding(s) have no disposition recorded this turn in %s\n' "$nmissing" "$DISP"
    printf 'Add one line per finding below, using this vocabulary only:\n'
    printf '  ACTED <command that was run> [id]\n'
    printf '  MY-CHOICE <reason> [id]\n'
    printf '  UNKNOWN <what could not be determined> [id]\n'
    printf -- '-- findings lacking a disposition --\n'
    printf '%s' "$missing"
  } >&2
  exit 2
fi

: > "$DISP" 2>/dev/null || true   # consumed: fresh dispositions required next turn
exit 0
