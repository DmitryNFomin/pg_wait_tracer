#!/usr/bin/env bash
# Exercise the actual hook protocol, with live gh calls replaced by a local stub.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 2
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT
export PGWT_GH="$TMP/gh" GUARD_GH_CALLS="$TMP/gh-calls"
cat > "$PGWT_GH" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$GUARD_GH_CALLS"
if [ "${GUARD_GH_FAIL:-0}" = 1 ]; then
  echo 'stub gh failing' >&2
  exit 7
fi
if [ "$#" -ne 5 ] || [ "$1" != api ] ||
   [ "$2" != 'repos/{owner}/{repo}/issues/115/comments' ] ||
   [ "$3" != --paginate ] || [ "$4" != --jq ] ||
   [ "$5" != '.[] | {id: .id, body: .body}' ]; then
  echo 'unexpected gh arguments' >&2
  exit 9
fi
printf '%s\n' '{"id":4115,"body":"Keep the implementation limited to guards and tests."}'
STUB
chmod +x "$PGWT_GH"
GOOD='#115
```console
gh api repos/example/project/issues/115/comments --paginate
{"id":4115,"body":"Keep the implementation limited to guards and tests."}
```
CONTROLLING SCOPE DECISION: #115 comment 4115 "Keep the implementation limited to guards and tests."'
BAD='Implement issue 115.'
printf '%s\n' "$GOOD" > "$TMP/good.md"
printf '%s\n' "$BAD" > "$TMP/bad.md"

pass=0; fail=0
event() {
  python3 - "$1" "$2" "${3-codex}" "${4:-}" "${5:-$ROOT}" > "$TMP/event.json" <<'PY'
import json
import sys
tool, text, kind, isolation, cwd = sys.argv[1:]
data = ({"prompt": text, "subagent_type": kind, "isolation": isolation}
        if tool == "Agent" else {"command": text})
print(json.dumps({"tool_name": tool, "tool_input": data, "cwd": cwd}))
PY
}
check() {
  local desc="$1" want="$2" rc
  bash "$ROOT/scripts/lead/dispatch-guard.sh" < "$TMP/event.json" > "$TMP/stdout" 2> "$TMP/stderr"
  rc=$?
  if [ "$rc" = "$want" ] && [ ! -s "$TMP/stdout" ] &&
     { [ "$want" != 0 ] || [ ! -s "$TMP/stderr" ]; }; then
    pass=$((pass+1)); echo "ok   $desc (exit $rc)"
  else
    fail=$((fail+1)); echo "FAIL $desc (want $want, got $rc)"
    cat "$TMP/stdout" "$TMP/stderr"
  fi
}
no_calls() {
  if [ ! -s "$GUARD_GH_CALLS" ]; then
    pass=$((pass+1)); echo 'ok   no gh call'
  else
    fail=$((fail+1)); echo 'FAIL unexpected gh call'; cat "$GUARD_GH_CALLS"
  fi
}

event Agent "$BAD"
check "Agent issue without fetch is refused" 2
if /usr/bin/grep -q 'CHECK BRIEF BLOCK: #115' "$TMP/stderr" &&
   /usr/bin/grep -q '^```console$' "$TMP/stderr" &&
   /usr/bin/grep -q '^gh api repos/{owner}/{repo}/issues/N/comments --paginate$' "$TMP/stderr" &&
   /usr/bin/grep -q '^CONTROLLING SCOPE DECISION: #N comment <id> "<quote>"$' "$TMP/stderr"; then
  pass=$((pass+1)); echo 'ok   refusal includes checker output and fixed guidance'
else
  fail=$((fail+1)); echo 'FAIL refusal guidance incomplete'
fi
event Agent "$GOOD"
check "Agent valid fetch and scope pass" 0

for kind in '' codex claude general-purpose; do
  event Agent "$BAD" "$kind"
  check "implementation Agent type '$kind' is gated" 2
done
printf '%s\n' '{"tool_name":"Agent","tool_input":{"prompt":"Fix #115"}}' > "$TMP/event.json"
check "omitted Agent type is gated" 2
event Agent "$BAD" Explore worktree
check "worktree Agent is gated regardless of type" 2
: > "$GUARD_GH_CALLS"
event Agent "$BAD" Explore
check "non-worktree Explore is outside the gate" 0
no_calls

printf '%s\n' "$BAD" > "$TMP/contract.md"
event Agent "$TMP/contract.md"
check "path-only contract without fetch is refused" 2
printf '%s\n' "$GOOD" > "$TMP/contract.md"
event Agent "$TMP/contract.md"
check "path-only contract with evidence passes" 0
printf '%s\n' "$BAD" > "$TMP/second.txt"
event Agent "Read $TMP/contract.md and $TMP/second.txt"
check "prompt and every contract are included" 0
printf '%s\n' 'Also implement #799.' > "$TMP/second.txt"
event Agent "Read $TMP/contract.md and $TMP/second.txt"
check "second contract cannot hide an uncovered issue" 2
printf '%s\n' "$BAD" > "$TMP/contract with spaces.txt"
event Agent "Read \"$TMP/contract with spaces.txt\"."
check "quoted contract path with spaces is read" 2
printf '%s\n' "$GOOD" > "$TMP/contract with spaces.txt"
event Agent "Read \"$TMP/contract with spaces.txt\"."
check "quoted contract path with evidence passes" 0
event Agent "Read \`$TMP/contract with spaces.txt\`."
check "backtick contract path with spaces is read" 0

event Bash "scripts/lead/codex-task.sh '$TMP/bad.md' agent/x"
check "Bash codex-task bad brief is refused" 2
event Bash "scripts/lead/codex-task.sh '$TMP/good.md' agent/x"
check "Bash codex-task good brief passes" 0
event Bash 'scripts/lead/codex-task.sh bad.md agent/x' '' '' "$TMP"
check "relative Bash brief path is read from event cwd" 2
event Bash 'scripts/lead/codex-task.sh good.md agent/x' '' '' "$TMP"
check "relative Bash good brief passes outside hook directory" 0
event Bash "bash scripts/lead/codex-task.sh '$TMP/bad.md' agent/x"
check "Bash wrapper around codex-task is gated" 2
event Bash "bash -lc 'scripts/lead/codex-task.sh $TMP/bad.md agent/x'"
check "shell -c invocation is gated" 2
event Bash "true && scripts/lead/codex-task.sh '$TMP/bad.md' agent/x"
check "compound Bash dispatch is gated" 2
event Bash "true
scripts/lead/codex-task.sh '$TMP/bad.md' agent/x"
check "multiline Bash dispatch is gated" 2
event Bash 'scripts/lead/codex-task.sh tests/no-such-brief.md agent/x'
check "unreadable named brief fails closed" 2
event Bash 'scripts/lead/codex-task.sh'
check "missing brief argument fails closed" 2

for command in 'printf hello' 'echo "unterminated' 'echo scripts/lead/codex-task.sh brief.md agent/x' \
               "codex exec --cd '$ROOT' '$TMP/bad.md'"; do
  : > "$GUARD_GH_CALLS"
  event Bash "$command"
  check "unrelated Bash is silent: $command" 0
  no_calls
done
: > "$GUARD_GH_CALLS"
event Agent 'Explain this small function.'
check "Agent without an issue reference passes" 0
no_calls

for malformed in '{broken' '' '[]' '{"tool_name":"Agent","tool_input":null}' \
                 '{"tool_name":"Bash","tool_input":{"command":3}}'; do
  printf '%s\n' "$malformed" > "$TMP/event.json"
  check "malformed event fails closed: $malformed" 2
done
event Agent "$GOOD"
export GUARD_GH_FAIL=1
check "live gh failure is refused" 2
unset GUARD_GH_FAIL

mkdir "$TMP/no-python"
PATH="$TMP/no-python" /bin/bash "$ROOT/scripts/lead/dispatch-guard.sh" < "$TMP/event.json" > "$TMP/stdout" 2> "$TMP/stderr"
rc=$?
if [ "$rc" = 2 ] && /usr/bin/grep -q 'python3 unavailable' "$TMP/stderr"; then
  pass=$((pass+1)); echo 'ok   missing python3 fails closed'
else
  fail=$((fail+1)); echo "FAIL missing python3 (exit $rc)"
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
