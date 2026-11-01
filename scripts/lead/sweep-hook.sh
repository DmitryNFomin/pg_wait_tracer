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
# through regardless of whether that disposition covers everything -- and
# does NOT consume the dispositions file, so that unevaluated response gets
# exactly one real evaluation, at the next stop_hook_active=false Stop.
#
# .dispositions is read into memory and truncated BEFORE it is evaluated
# (never evaluated by re-reading the file). That closes the crash window by
# construction: once the truncate has run, no later crash or interruption can
# leave a stale on-disk entry that satisfies a DIFFERENT, later turn's
# evaluation of the same recurring finding (a BEHIND PR, a stranded branch and
# a ready-not-started blocker are all exactly the kind of finding that stays
# byte-identical, hence same sha256 id, turn after turn until someone acts).
# This applies to BOTH gates below (plain findings and the escalation gate):
# the read/truncate happens exactly once, before either is evaluated, so a
# turn with zero plain findings still gets its escalation items evaluated
# against THIS turn's dispositions, not skipped by an early return.
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
  if timeout 40 "$D/sweep.sh" "$D" > "$CACHE.tmp" 2>&1 && [ -s "$CACHE.tmp" ]; then
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
# grep -c prints "0" on zero matches but still EXITS 1 (not an error -- "no
# match" is success-shaped everywhere else in grep); `|| echo 0` therefore
# fired on top of that already-printed "0" and doubled it into literally
# "0\n0", which then broke the `-gt` test below with "integer expected". Two
# definite answers glued together is not a fallback, it is a second bug
# riding on the first -- `|| true` supplies nothing on top of grep's own
# output and only matters for a real failure (unreadable file), where $n/$c
# stay empty and the -gt test below fails loud rather than printing a count
# that was never computed.
n=$(/usr/bin/grep -acE 'FINDING|CANDIDATE|UNKNOWN' "$CACHE" 2>/dev/null || true)
/usr/bin/grep -aE 'FINDING|UNKNOWN' "$CACHE" 2>/dev/null | head -15
c=$(/usr/bin/grep -acE 'CANDIDATE' "$CACHE" 2>/dev/null || true)
[ -n "$c" ] && [ "$c" -gt 0 ] && printf '   (+%d CANDIDATE lines -- a mention is not a fix; verify content on master)\n' "$c"
printf '=== %s open items. Act on them or say why not. ===\n' "${n:-UNKNOWN}"

# ---- the blocking gate ------------------------------------------------------
# Loop protection first, per the docs cited above: never re-evaluate once
# Claude Code is already continuing because of an earlier block this turn.
# Deliberately NO truncate here: a disposition the lead just wrote in response
# to the block that produced this continuation must survive, unevaluated, to
# the next real (stop_hook_active=false) Stop -- that is the one and only
# evaluation it gets. Consuming it here would silently discard it instead.
if [ "$stop_active" = "true" ]; then
  exit 0
fi

findings=$(/usr/bin/grep -aE '^ *FINDING: ' "$CACHE" 2>/dev/null || true)

# Read-then-truncate-then-evaluate, in that order and nothing between the
# first two: once the file is empty on disk, no crash point from here on can
# leave a stale entry that satisfies a LATER, different turn's evaluation of
# the same (possibly byte-identical, commonly-recurring) finding. The decision
# below is made entirely from $disp_content in memory, never by re-reading
# $DISP, so consuming it first changes nothing about what gets evaluated or
# what the lead is told it is missing. Unconditional (not gated on $findings
# being non-empty): the escalation gate below must see this turn's
# dispositions even when there are zero plain findings.
touch "$DISP" 2>/dev/null || true
disp_content=$(cat "$DISP" 2>/dev/null || true)
: > "$DISP" 2>/dev/null || true

missing=""
nmissing=0

if [ -n "$findings" ]; then
while IFS= read -r line; do
  [ -n "$line" ] || continue
  text=$(printf '%s' "$line" | /usr/bin/sed -E 's/^[[:space:]]*FINDING:[[:space:]]*//')
  id=$(printf '%s' "$text" | shasum -a 256 | cut -c1-10)
  # A disposition counts only when it has the right vocabulary AND names this
  # finding's id -- quantity alone (e.g. one MY-CHOICE line for N findings)
  # must not satisfy N findings.
  if printf '%s\n' "$disp_content" | /usr/bin/grep -aqE "^(ACTED|MY-CHOICE|UNKNOWN)[[:space:]].*\[$id\]"; then
    continue
  fi
  nmissing=$((nmissing+1))
  missing="${missing}[$id] ${text}
"
done <<EOF
$findings
EOF
fi

# ---- the escalating-obligation gate ----------------------------------------
# A ready-not-started item (see sweep.sh section [9]) that survives ESC_N
# consecutive successful scans stops accepting MY-CHOICE/ACTED/UNKNOWN: only
# RUNNING or BLOCKED-BY satisfies it from here. ESC_N counts actual sweep.sh
# executions, not Stop events -- sweep-hook.sh above only reruns sweep.sh
# once the cache is >= MAXAGE (600s) old, so consecutive scans are already
# at least 10 minutes apart in wall-clock time regardless of how many turns
# happen in between. ESC_N=3 -> at least 30 minutes of an item sitting ready,
# across at least 3 separate Stop events the lead had a chance to act on, is
# the floor before this gate narrows the vocabulary -- long enough that a
# busy stretch of tool calls between two Stops isn't mistaken for drift,
# short enough to catch an eleven-hour "blocked" well before hour one.
#
# The cap: never demand a start the project cap (3 actively-implementing,
# CLAUDE.md) forbids. budget = max(cap - running, 0); only the `budget`
# most-overdue escalated items are forced this turn. At budget 0 escalation
# PAUSES entirely rather than pressuring a false BLOCKED-BY as the only
# non-over-cap answer -- that would just move the dishonesty, not remove it.
# `running` itself is sanity-bounded by sweep.sh before it ever reaches this
# file (a STATUS OK line whose running count is implausible for a 3-agent-cap
# project is refused there, not trusted here) -- see sweep.sh section [9].
ESC_N=3
DQ="$D/.dispatch-queue"
dq_age=999999
[ -s "$DQ" ] && dq_age=$(( $(date +%s) - $(stat -f %m "$DQ" 2>/dev/null || echo 0) ))
if [ "$dq_age" -ge "$MAXAGE" ]; then
  echo "DISPATCH QUEUE: STALE or missing (>= ${MAXAGE}s old) -- escalation suspended until a fresh sweep succeeds"
else
  esc_out=$(ESC_DISP_CONTENT="$disp_content" /usr/bin/python3 - "$DQ" "$ESC_N" <<'PY'
import os, re, sys
dq, N = sys.argv[1], int(sys.argv[2])
disp = os.environ.get("ESC_DISP_CONTENT", "")
try:
    with open(dq) as f:
        lines = f.read().splitlines()
except FileNotFoundError:
    print("BANNER DISPATCH QUEUE: UNKNOWN (no .dispatch-queue file -- run scripts/lead/sweep.sh)")
    raise SystemExit
if len(lines) < 2:
    print("BANNER DISPATCH QUEUE: UNKNOWN (truncated file)")
    raise SystemExit
status_line = lines[1].strip()
if status_line.startswith("STATUS UNKNOWN"):
    print(f"BANNER DISPATCH QUEUE: {status_line}")
    raise SystemExit
if status_line != "STATUS OK":
    print(f"BANNER DISPATCH QUEUE: UNKNOWN (malformed status line {status_line!r})")
    raise SystemExit

running, cap = None, None
items = []
all_ids = set()
for ln in lines[2:]:
    if ln.startswith("CAP "):
        m = re.match(r"CAP running=(\d+) cap=(\d+)", ln)
        if m:
            running, cap = int(m.group(1)), int(m.group(2))
        continue
    if not ln.startswith("issue-"):
        continue
    parts = ln.split(" ", 3)
    if len(parts) < 3:
        continue
    rid, num = parts[0], parts[1]
    try:
        count = int(parts[2])
    except ValueError:
        continue
    title = parts[3] if len(parts) > 3 else ""
    all_ids.add(rid)
    items.append((rid, num, count, title))

if running is None or cap is None:
    print("BANNER DISPATCH QUEUE: UNKNOWN (no CAP line -- cannot bound escalation)")
    raise SystemExit

escalated = sorted([it for it in items if it[2] >= N], key=lambda x: (-x[2], x[1]))
budget = max(cap - running, 0)
forced = escalated[:budget]

print(f"BANNER DISPATCH QUEUE: {len(items)} ready, {len(escalated)} escalated (>= {N} scans), "
      f"running={running} cap={cap}, forcing {len(forced)}")
if escalated and budget == 0:
    print(f"BANNER DISPATCH ESCALATION PAUSED: running={running} >= cap={cap}, no headroom to force a start")
elif len(forced) < len(escalated):
    print(f"BANNER DISPATCH ESCALATION PARTIAL: only {len(forced)} of {len(escalated)} forced (headroom={budget})")

for rid, num, count, title in forced:
    pat_running = re.compile(r'^RUNNING[^\n]*\[' + re.escape(rid) + r'\]', re.M)
    pat_blocked = re.compile(r'^BLOCKED-BY[ \t]+(\S+)[^\n]*\[' + re.escape(rid) + r'\]', re.M)
    if pat_running.search(disp):
        continue
    bm = pat_blocked.search(disp)
    if bm and bm.group(1) in all_ids:
        continue
    print(f'MISSING {rid} issue#{num} ready {count} consecutive scans ("{title}") -- '
          f'needs RUNNING <agent-or-PR> [{rid}] or BLOCKED-BY <id> [{rid}] naming another queue item')
PY
)
  printf '%s\n' "$esc_out" | /usr/bin/grep '^BANNER ' | sed 's/^BANNER //'
  while IFS= read -r eline; do
    case "$eline" in
      MISSING\ *)
        nmissing=$((nmissing+1))
        missing="${missing}${eline#MISSING }
"
        ;;
    esac
  done <<EOF2
$esc_out
EOF2
fi

if [ "$nmissing" -gt 0 ]; then
  {
    printf 'LEAD SWEEP BLOCK: %d finding(s) have no disposition recorded this turn in %s\n' "$nmissing" "$DISP"
    printf 'Add one line per finding below. Vocabulary for a plain FINDING [hash-id]:\n'
    printf '  ACTED <command that was run> [id]\n'
    printf '  MY-CHOICE <reason> [id]\n'
    printf '  UNKNOWN <what could not be determined> [id]\n'
    printf 'An escalated dispatch-queue item [issue-N] is stricter -- MY-CHOICE/ACTED/UNKNOWN\n'
    printf 'do not satisfy it; only:\n'
    printf '  RUNNING <agent-or-PR> [issue-N]\n'
    printf '  BLOCKED-BY <other-queue-id> [issue-N]\n'
    printf -- '-- items lacking a disposition --\n'
    printf '%s' "$missing"
  } >&2
  exit 2
fi

exit 0
