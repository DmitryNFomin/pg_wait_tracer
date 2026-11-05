#!/usr/bin/env bash
# Black-box the checker with a local gh object stream; no GitHub access.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT
export BRIEF_GH_CALLS="$TMP/gh-calls"
cat > "$TMP/gh" <<'STUB'
#!/usr/bin/env bash
set -u
if [ "$#" -ne 5 ] || [ "$1" != api ] || [ "$3" != --paginate ] ||
   [ "$4" != --jq ] || [ "$5" != '.[] | {id: .id, body: .body}' ]; then
  echo 'unexpected gh arguments' >&2
  exit 9
fi
printf '%s\n' "$2" >> "$BRIEF_GH_CALLS"
case "$2" in
  'repos/{owner}/{repo}/issues/101/comments')
    cat <<'JSON'
{
  "id": 4101,
  "body": "Keep the implementation limited to the parser and its tests. Discuss #999 only as background."
}
{"id":4102,"body":"Preserve the existing exit codes.\nRun the focused regression suite."}
JSON
    ;;
  'repos/{owner}/{repo}/issues/202/comments')
    printf '%s\n' '{"id":5201,"body":"This is a different issue with unrelated scope."}' ;;
  'repos/{owner}/{repo}/issues/303/comments') ;;
  'repos/{owner}/{repo}/issues/404/comments') echo 'query failed' >&2; exit 7 ;;
  'repos/{owner}/{repo}/issues/505/comments') printf '%s\n' '{broken json' ;;
  *) echo "unexpected endpoint: $2" >&2; exit 9 ;;
esac
STUB
chmod +x "$TMP/gh"

pass=0; fail=0; LAST_OUT=''
check() {
  local desc="$1" want_rc="$2" content="$3" rc
  printf '%s\n' "$content" > "$TMP/brief.md"
  LAST_OUT=$(PGWT_GH="$TMP/gh" bash "$ROOT/scripts/lead/check-brief.sh" "$TMP/brief.md" 2>&1)
  rc=$?
  if [ "$rc" = "$want_rc" ]; then
    pass=$((pass+1)); echo "ok   $desc (exit $rc)"
  else
    fail=$((fail+1)); echo "FAIL $desc (want exit $want_rc, got $rc)"
    printf '%s\n' "$LAST_OUT" | sed 's/^/       /'
  fi
}

assert_one_101_fetch() {
  local calls total
  calls=$(/usr/bin/grep -c '^repos/{owner}/{repo}/issues/101/comments$' "$BRIEF_GH_CALLS")
  total=$(/usr/bin/grep -c . "$BRIEF_GH_CALLS")
  if [ "$calls" = 1 ] && [ "$total" = 1 ]; then
    pass=$((pass+1)); echo 'ok   only one live fetch for #101; no fetch for #999'
  else
    fail=$((fail+1)); echo "FAIL expected only one #101 fetch, got $calls of $total calls"
  fi
}

FETCH='#101
```console
$ gh api repos/:owner/:repo/issues/101/comments --paginate
{"id":4101,"body":"Keep the implementation limited to the parser and its tests."}
{"id":4102,"body":"Preserve the existing exit codes."}
```'
SCOPE='CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'

check "comments without a fetch block fail" 1 "#101
$SCOPE"
if [[ "$LAST_OUT" == *'#101'* && "$LAST_OUT" == *'fetch'* ]]; then
  pass=$((pass+1)); echo 'ok   refusal names the issue and fetch requirement'
else
  fail=$((fail+1)); echo 'FAIL refusal did not name the issue and fetch requirement'
fi
check "unrelated issue ids fail" 1 '#101
```
$ gh api repos/:owner/:repo/issues/101/comments --paginate
{"id":5201}
```
CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'
check "correct fetch without scope decision fails" 1 "$FETCH"
check "invented scope quote fails" 1 "$FETCH
CONTROLLING SCOPE DECISION: #101 comment 4101 \"Rewrite the entire application and remove every existing test.\""
check "none scope with live comments fails" 1 "$FETCH
CONTROLLING SCOPE DECISION: #101 none (0 comments)"
check "a valid scope cannot excuse a false zero-comment decision" 1 "$FETCH
$SCOPE
CONTROLLING SCOPE DECISION: #101 none (0 comments)"
check "scope decision must fit on one line" 1 "$FETCH
CONTROLLING SCOPE DECISION: #101
comment 4101 \"Keep the implementation limited to the parser and its tests.\""
check "complete brief passes" 0 "$FETCH
$SCOPE"
rm -f "$BRIEF_GH_CALLS"
check "references in fetched comment bodies are not dispatches" 0 '#101
```console
$ gh api repos/:owner/:repo/issues/101/comments --paginate
{"id":4101,"body":"Keep the implementation limited to the parser and its tests. Discuss #999 only as background."}
{"id":4102,"body":"Preserve the existing exit codes.\nRun the focused regression suite."}
```
CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'
assert_one_101_fetch

rm -f "$BRIEF_GH_CALLS"
check "scope quote references are not dispatches; leading issue still counts" 0 "${FETCH#*$'\n'}
CONTROLLING SCOPE DECISION: #101 comment 4101 \"Keep the implementation limited to the parser and its tests. Discuss #999 only as background.\""
assert_one_101_fetch

check "ordinary prose references still count as dispatches" 2 "$FETCH
$SCOPE
Also dispatch #999."
check "non-fetch fenced references still count as dispatches" 2 "$FETCH
$SCOPE
\`\`\`text
Also dispatch #999.
\`\`\`"

check "zero comments with empty-array evidence passes" 0 '#303
```
$ gh api repos/example/project/issues/303/comments --paginate
[]
```
CONTROLLING SCOPE DECISION: #303 none (0 comments)'
check "gh failure is an error" 2 '#404'
check "unparseable gh output is an error" 2 '#505'

check "scope quote whitespace is normalised" 0 "$FETCH
CONTROLLING SCOPE DECISION: #101 comment 4102 \"Preserve the existing exit codes.   Run the focused regression suite.\""
check "comment ids must be whole words" 1 '#101
```
$ gh api repos/example/project/issues/101/comments
14101 41020
```
CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'
check "ids in a separate block do not cover the fetch" 1 '#101
```
$ gh api repos/example/project/issues/101/comments
```
```
4101 4102
```
CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'
check "each referenced issue is checked" 1 "$FETCH
$SCOPE
Also dispatch #202."
check "zero comments without empty output evidence fails" 1 '#303
```
$ gh api repos/example/project/issues/303/comments
```
CONTROLLING SCOPE DECISION: #303 none (0 comments)'
check "no issue reference passes" 0 'A general implementation brief.'
if [ "$LAST_OUT" = 'no issue referenced' ]; then
  pass=$((pass+1)); echo 'ok   no-reference message is explicit'
else
  fail=$((fail+1)); echo 'FAIL no-reference message is incorrect'
fi

rm -f "$BRIEF_GH_CALLS"
check "repeated references fetch one distinct issue once" 0 "$FETCH
$SCOPE
Dispatch #101."
assert_one_101_fetch

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
