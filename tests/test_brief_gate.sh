#!/usr/bin/env bash
# Exercise the real hooks beside isolated, fresh caches, as lead/selftest.sh
# does. A local sweep stub makes even an accidental refresh network-free.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
TMP=$(mktemp -d) || exit 2
trap 'chmod -R u+rwX "$TMP"; rm -rf "$TMP"' EXIT
D="$TMP/hooks"
mkdir -p "$D" "$TMP/state"
export PGWT_LEAD_STATE_DIR="$TMP/state"
cp "$ROOT/scripts/lead/sweep-hook.sh" "$ROOT/scripts/lead/agent-start-hook.sh" "$D/"
cat > "$D/sweep.sh" <<'STUB'
#!/usr/bin/env bash
echo 'sandbox refresh unavailable' >&2
exit 88
STUB
# Keep the start hook's production ledger operation observable and isolated.
export GATE_LEDGER_CALLS="$TMP/ledger-calls"
cat > "$D/agent-ledger.py" <<'STUB'
import json, os, sys
with open(os.environ["GATE_LEDGER_CALLS"], "a") as out:
    out.write(json.dumps(sys.argv[1:]) + "\n")
STUB
chmod +x "$D/"*.sh
NOW=$(date +%s)
SHA=$(printf '%064d' 0)
pass=0; fail=0; OUT=''; RC=0
ok() { pass=$((pass+1)); printf 'ok   %s\n' "$*"; }
bad() { fail=$((fail+1)); printf 'FAIL %s\n' "$*"; printf '%s\n' "$OUT"; }
reset_state() {
  rm -rf "$PGWT_LEAD_STATE_DIR/.spawns" "$PGWT_LEAD_STATE_DIR/.briefs-passed" "$PGWT_LEAD_STATE_DIR/.spawn-payloads"
  : > "$D/.dispositions"
  printf '== sandbox: zero findings ==\n' > "$D/.sweep-latest.txt"
  printf '# dispatch-queue v1\nSTATUS OK\nCAP running=0 cap=3\n' > "$D/.dispatch-queue"
  : > "$PGWT_LEAD_STATE_DIR/.spawns"
  : > "$PGWT_LEAD_STATE_DIR/.briefs-passed"
}
spawn() { printf '%s %s %s\n' "$NOW" "$1" "${2:-implementer}" >> "$PGWT_LEAD_STATE_DIR/.spawns"; }
brief() { printf '%s %s %s\n' "$1" "$SHA" "$TMP/brief with spaces.md" >> "$PGWT_LEAD_STATE_DIR/.briefs-passed"; }
stop() {
  local event="${1-}"
  [ -n "$event" ] || event='{}'
  OUT=$(printf '%s' "$event" | bash "$D/sweep-hook.sh" 2> "$TMP/stop-err")
  RC=$?
  STOP_STDOUT=$OUT
  OUT="$OUT
$(cat "$TMP/stop-err")"
}
expect() {
  local desc="$1" want="$2" text="${3:-}"
  if [ "$RC" = "$want" ] && [[ "$OUT" == *"$text"* ]]; then ok "$desc"; else bad "$desc (want $want / $text, got $RC)"; fi
}
empty() { if [ ! -s "$1" ]; then ok "$2"; else bad "$2"; fi; }

reset_state; spawn impl-a
stop
expect 'implementer without pass blocks and names agent/type' 2 'impl-a (implementer)'
if [[ "$OUT" == *'scripts/lead/check-brief.sh <brief-file> BEFORE spawning'* ]]; then ok 'block tells lead how and when to check'; else bad 'missing check instruction'; fi

reset_state; spawn impl-a; brief "$((NOW-1))"
stop
expect 'earlier brief passes' 0
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'matched spawn consumed'
empty "$PGWT_LEAD_STATE_DIR/.briefs-passed" 'used pass consumed'

reset_state; spawn impl-a; brief "$NOW"
stop
expect 'equal epochs pass' 0

reset_state; spawn impl-late; brief "$((NOW+1))"
stop
expect 'later brief cannot justify earlier spawn' 2 impl-late
if [ -s "$PGWT_LEAD_STATE_DIR/.briefs-passed" ]; then ok 'unused pass retained'; else bad 'unused pass lost'; fi

reset_state; spawn impl-a; spawn impl-b; brief "$((NOW-1))"
stop
expect 'two dispatches require distinct passes' 2 impl-b
if [ "$(/usr/bin/grep -c . "$PGWT_LEAD_STATE_DIR/.spawns")" = 1 ]; then ok 'only unmatched dispatch persists'; else bad 'wrong retained dispatch count'; fi

reset_state
for kind in adviser reviewer ui-reviewer Explore Plan claude-code-guide statusline-setup; do spawn "agent-$kind" "$kind"; done
brief "$((NOW-1))"
stop
expect 'all advisory types pass without consuming briefs' 0
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'advisory spawns dropped'
if [ -s "$PGWT_LEAD_STATE_DIR/.briefs-passed" ]; then ok 'advisory spawns do not consume pass'; else bad 'advisory consumed pass'; fi

reset_state
printf '%s' '{"agent_id":"unknown-kind","hook_event_name":"SubagentStart"}' | bash "$D/agent-start-hook.sh"
if /usr/bin/grep -Eq '^[0-9]+ unknown-kind unknown$' "$PGWT_LEAD_STATE_DIR/.spawns"; then ok 'untyped spawn recorded as unknown'; else bad 'untyped spawn missing'; fi
stop
expect 'unknown type is advisory and reports missing payload type' 0 '1 with agent_type ABSENT'
if [[ "$STOP_STDOUT" == *'SPAWN PAYLOAD DIAGNOSTIC: 1 spawn(s) since last stop'* &&
      "$STOP_STDOUT" == *'keys seen: agent_id,hook_event_name'* &&
      "$STOP_STDOUT" == *'agent_type NOT DELIVERED -- spawn gate cannot classify; check the keys above'* ]]; then
  ok 'missing type diagnostic is loud on stdout'
else bad 'missing stdout payload diagnostic'; fi
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'unknown spawn never persists'
empty "$PGWT_LEAD_STATE_DIR/.spawn-payloads" 'payload diagnostics consumed at genuine stop'
stop
if [[ "$OUT" != *'SPAWN PAYLOAD DIAGNOSTIC:'* ]]; then ok 'diagnostic does not recur'; else bad 'diagnostic recurred'; fi

reset_state
printf '%s missing-type\n' "$NOW" > "$PGWT_LEAD_STATE_DIR/.spawns"
stop; expect 'absent recorded type is advisory' 0
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'absent recorded type never persists'

reset_state
for value in null 42 '{}' '[]' '""' '"two words"'; do
  printf '{"agent_id":"bad-type","agent_type":%s}' "$value" | bash "$D/agent-start-hook.sh"
done
stop; expect 'non-token payload types are advisory' 0 '0 with agent_type ABSENT'
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'non-token payload types never persist'

reset_state
printf '%s' '{"agent_id":"typed","agent_type":"reviewer","z":0}' | bash "$D/agent-start-hook.sh"
printf '%s' '{"agent_id":"untyped","a":0}' | bash "$D/agent-start-hook.sh"
stop; expect 'diagnostic counts spawns and missing types' 0 '2 spawn(s) since last stop, 1 with agent_type ABSENT'
if [[ "$STOP_STDOUT" == *'keys seen: a,agent_id,agent_type,z;'* ]]; then ok 'diagnostic unions and sorts payload keys'; else bad 'wrong key union'; fi

for event in '{"stop_hook_active":true,"hook_event_name":"Stop"}' '{"hook_event_name":"SubagentStop"}'; do
  reset_state; spawn bypass; brief "$((NOW-1))"
  printf 'MY-CHOICE preserve this [bypass]\n' > "$D/.dispositions"
  cp "$D/.dispositions" "$TMP/disp-before"
  cp "$PGWT_LEAD_STATE_DIR/.spawns" "$TMP/spawns-before"
  cp "$PGWT_LEAD_STATE_DIR/.briefs-passed" "$TMP/passes-before"
  printf '%s bypass keys=agent_id agent_type=ABSENT\n' "$NOW" > "$PGWT_LEAD_STATE_DIR/.spawn-payloads"
  cp "$PGWT_LEAD_STATE_DIR/.spawn-payloads" "$TMP/payloads-before"
  stop "$event"
  expect "bypass event $event passes" 0
  if cmp -s "$D/.dispositions" "$TMP/disp-before" &&
     cmp -s "$PGWT_LEAD_STATE_DIR/.spawns" "$TMP/spawns-before" &&
     cmp -s "$PGWT_LEAD_STATE_DIR/.briefs-passed" "$TMP/passes-before" &&
     cmp -s "$PGWT_LEAD_STATE_DIR/.spawn-payloads" "$TMP/payloads-before" &&
     [[ "$OUT" != *'SPAWN PAYLOAD DIAGNOSTIC:'* ]]; then
    ok 'bypass preserves dispositions, records and diagnostics without a banner'
  else bad 'bypass changed state'; fi
done

reset_state; spawn persistent
stop; expect 'initial unmatched dispatch blocks' 2 persistent
stop; expect 'unmatched dispatch blocks at next genuine stop' 2 persistent
printf 'MY-CHOICE deliberate exception [other-id]\n' > "$D/.dispositions"
stop; expect 'another agent disposition cannot clear dispatch' 2 persistent
printf 'MY-CHOICE deliberate exception [persistent]\n' > "$D/.dispositions"
stop; expect 'matching MY-CHOICE clears persistent dispatch' 0
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'cleared dispatch removed'
empty "$D/.dispositions" 'disposition consumed at genuine stop'
stop; expect 'cleared dispatch does not recur' 0

reset_state; spawn expired; brief "$((NOW-86401))"
stop; expect 'expired pass cannot cover dispatch' 2 expired
empty "$PGWT_LEAD_STATE_DIR/.briefs-passed" 'passes older than 24h discarded'

reset_state; printf 'malformed spawn\n' > "$PGWT_LEAD_STATE_DIR/.spawns"
stop; expect 'malformed spawn fails safe' 2 'UNKNOWN: malformed .spawns'
stop; expect 'malformed spawn persists' 2 'UNKNOWN: malformed .spawns'
reset_state; rm "$PGWT_LEAD_STATE_DIR/.spawns"; mkdir "$PGWT_LEAD_STATE_DIR/.spawns"
stop; expect 'unreadable spawn file fails safe' 2 'UNKNOWN: cannot read/update .spawns'

reset_state; spawn no-cache; rm "$D/.sweep-latest.txt"
stop; expect 'missing sweep cache cannot bypass brief gate' 2 no-cache

reset_state
OUT=$(printf '%s' '{"agent_id":"start-a","agent_type":"implementer"}' | bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'start hook exits zero' 0
if /usr/bin/grep -Eq '^[0-9]+ start-a implementer$' "$PGWT_LEAD_STATE_DIR/.spawns" &&
   /usr/bin/grep -Eq '^[0-9]+ start-a keys=agent_id,agent_type agent_type=PRESENT$' "$PGWT_LEAD_STATE_DIR/.spawn-payloads" &&
   /usr/bin/grep -Fq '["start", "/Users/dmitryfomin/work/git/pg_wait_tracer/scripts/lead/.active-agents", "start-a"]' "$GATE_LEDGER_CALLS"; then
  ok 'start records spawn and preserves canonical ledger operation'
else bad 'start record or ledger call missing'; fi
OUT=$(printf '%s' '{"agent_id":"start-unknown"}' | bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'start without type exits zero' 0
if /usr/bin/grep -Eq '^[0-9]+ start-unknown unknown$' "$PGWT_LEAD_STATE_DIR/.spawns" &&
   /usr/bin/grep -Eq '^[0-9]+ start-unknown keys=agent_id agent_type=ABSENT$' "$PGWT_LEAD_STATE_DIR/.spawn-payloads"; then ok 'absent type recorded as unknown with diagnostic'; else bad 'absent type lost'; fi
OUT=$(printf '%s' 'not json' | bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'unparseable start payload exits zero' 0
if /usr/bin/grep -Eq '^[0-9]+ unknown keys=UNPARSEABLE agent_type=ABSENT$' "$PGWT_LEAD_STATE_DIR/.spawn-payloads"; then ok 'unparseable payload diagnostic recorded'; else bad 'unparseable diagnostic missing'; fi
reset_state
mkdir "$PGWT_LEAD_STATE_DIR/.spawn-payloads"
OUT=$(printf '%s' '{"agent_id":"diag-unwritable","agent_type":"implementer"}' | bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'start exits zero when diagnostic file cannot be written' 0
if /usr/bin/grep -Eq '^[0-9]+ diag-unwritable implementer$' "$PGWT_LEAD_STATE_DIR/.spawns"; then ok 'diagnostic failure preserves spawn recording'; else bad 'diagnostic failure prevented spawn record'; fi
reset_state
rm "$PGWT_LEAD_STATE_DIR/.spawns"; mkdir "$PGWT_LEAD_STATE_DIR/.spawns"
OUT=$(printf '%s' '{"agent_id":"spawn-unwritable"}' | bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'start exits zero when spawn file cannot be written' 0
if /usr/bin/grep -Eq '^[0-9]+ spawn-unwritable keys=agent_id agent_type=ABSENT$' "$PGWT_LEAD_STATE_DIR/.spawn-payloads"; then ok 'spawn failure preserves payload diagnostic'; else bad 'spawn failure prevented diagnostic'; fi
mkdir "$TMP/unwritable"; chmod 500 "$TMP/unwritable"
OUT=$(printf '%s' '{"agent_id":"unwritable","agent_type":"implementer"}' | PGWT_LEAD_STATE_DIR="$TMP/unwritable" bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'start always exits zero for unwritable state' 0
printf 'not a directory\n' > "$TMP/not-a-dir"
OUT=$(printf '%s' '{"agent_id":"invalid-state"}' | PGWT_LEAD_STATE_DIR="$TMP/not-a-dir" bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'start always exits zero even if state path is a file' 0

# Use PGWT_GH for the real checker, then let its actual pass feed the gate.
cat > "$TMP/gh" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
chmod +x "$TMP/gh"
export PGWT_GH="$TMP/gh"
reset_state
printf 'Implement issue #115.\n' > "$TMP/brief.md"
OUT=$(bash "$ROOT/scripts/lead/check-brief.sh" "$TMP/brief.md" 2>&1); RC=$?
expect 'incomplete checker brief fails' 1
empty "$PGWT_LEAD_STATE_DIR/.briefs-passed" 'failed checker adds no record'
cat > "$TMP/brief.md" <<'BRIEF'
Implement issue #115.
```console
gh api repos/owner/repo/issues/115/comments --paginate
[]
```
CONTROLLING SCOPE DECISION: #115 none (0 comments)
BRIEF
OUT=$(bash "$ROOT/scripts/lead/check-brief.sh" "$TMP/brief.md" 2>&1); RC=$?
expect 'complete checker brief passes' 0
if /usr/bin/grep -Eq "^[0-9]+ [0-9a-f]{64} $TMP/brief.md$" "$PGWT_LEAD_STATE_DIR/.briefs-passed"; then ok 'checker appends pass record'; else bad 'checker pass record missing'; fi
OUT=$(printf '%s' '{"agent_id":"integrated","agent_type":"implementer"}' | bash "$D/agent-start-hook.sh" 2>&1); RC=$?
expect 'integrated dispatch starts' 0
stop; expect 'real checker and start records satisfy real Stop gate' 0
empty "$PGWT_LEAD_STATE_DIR/.briefs-passed" 'real pass consumed'
empty "$PGWT_LEAD_STATE_DIR/.spawns" 'real spawn consumed'

printf '%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
