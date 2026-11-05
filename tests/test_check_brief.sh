#!/usr/bin/env bash
# Black-box the checker with a local gh object stream; no GitHub access.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT
export BRIEF_GH_CALLS="$TMP/gh-calls"
export PGWT_LEAD_STATE_DIR="$TMP"
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
  'repos/{owner}/{repo}/issues/707/comments') exec sleep 5 ;;
  'repos/{owner}/{repo}/issues/115/comments'|'repos/{owner}/{repo}/issues/799/comments'|'repos/{owner}/{repo}/issues/797/comments') ;;
  'repos/{owner}/{repo}/issues/606/comments')
    printf '%s\n' '{"id":6601,"body":"Keep \"quoted\" paths intact.\nRun the tests."}' '{"id":6602,"body":""}' ;;
  *) echo "unexpected endpoint: $2" >&2; exit 9 ;;
esac
STUB
chmod +x "$TMP/gh"

pass=0; fail=0; LAST_OUT=''
check() {
  local desc="$1" want_rc="$2" content="$3" rc
  printf '%s\n' "$content" > "$TMP/brief.md"
  : > "$TMP/.briefs-passed"
  LAST_OUT=$(PGWT_GH="$TMP/gh" bash "$ROOT/scripts/lead/check-brief.sh" "$TMP/brief.md" 2>&1)
  rc=$?
  if [ "$rc" = 0 ]; then
    local epoch digest path expected
    read -r epoch digest path < "$TMP/.briefs-passed"
    expected=$(shasum -a 256 "$TMP/brief.md" | cut -d ' ' -f 1)
    if [ "$digest" = "$expected" ] && [ "$path" = "$TMP/brief.md" ] &&
       [[ "$epoch" =~ ^[0-9]+$ ]] &&
       [ "$(/usr/bin/grep -c . "$TMP/.briefs-passed")" = 1 ]; then
      pass=$((pass+1)); echo "ok   pass records exact brief bytes and absolute path"
    else
      fail=$((fail+1)); echo "FAIL bad or absent pass record: $desc"
    fi
  elif [ -s "$TMP/.briefs-passed" ]; then
    fail=$((fail+1)); echo "FAIL unsuccessful check recorded a pass: $desc"
  fi
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
check "fetch-only brief still needs a scope decision" 1 "${FETCH#*$'\n'}"
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
PGWT_GH_TIMEOUT=1 check "gh timeout is an error" 2 '#707'
if [[ "$LAST_OUT" == *'timeout after 1s'* ]]; then
  pass=$((pass+1)); echo 'ok   error names the gh timeout'
else
  fail=$((fail+1)); echo 'FAIL timeout error did not name the timeout'
fi

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

check "ids-only paste fails even with a valid scope quote" 1 '#101
```
gh api repos/example/project/issues/101/comments --paginate
4101 4102
```
CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'
check "every id also needs its body prefix" 1 '#101
```
gh api repos/example/project/issues/101/comments --paginate
{"id":4101,"body":"Keep the implementation limited to the parser and its tests."}
{"id":4102}
```
CONTROLLING SCOPE DECISION: #101 comment 4101 "Keep the implementation limited to the parser and its tests."'
check "JSON-escaped body prefixes and empty bodies pass" 0 '#606
```
gh api repos/example/project/issues/606/comments --paginate
{"id":6601,"body":"Keep \"quoted\" paths intact.\nRun the tests."}
{"id":6602,"body":""}
```
CONTROLLING SCOPE DECISION: #606 comment 6601 "Keep "quoted" paths intact."'
check "raw body prefixes pass" 0 '#606
```
gh api repos/example/project/issues/606/comments --paginate
6601: Keep "quoted" paths intact.
6602:
```
CONTROLLING SCOPE DECISION: #606 comment 6601 "Keep "quoted" paths intact."'

QUOTE='Keep the implementation limited to the parser and its tests.'
check "real 19-character scope substring fails" 1 "$FETCH
CONTROLLING SCOPE DECISION: #101 comment 4101 \"${QUOTE:0:19}\""
check "real 20-character scope substring passes" 0 "$FETCH
CONTROLLING SCOPE DECISION: #101 comment 4101 \"${QUOTE:0:20}\""

for reference in 'issue 115' 'https://github.com/owner/repo/issues/115' 'GH-115' '#115' \
                 'pull request 799' 'https://github.com/owner/repo/pull/799' 'PR 799' 'PR #799'; do
  : > "$BRIEF_GH_CALLS"
  check "dispatch recognises $reference" 1 "Fix $reference."
  case "$reference" in *115*) number=115 ;; *) number=799 ;; esac
  if /usr/bin/grep -qx "repos/{owner}/{repo}/issues/$number/comments" "$BRIEF_GH_CALLS"; then
    pass=$((pass+1)); echo "ok   live fetch requested for $number"
  else
    fail=$((fail+1)); echo "FAIL live fetch not requested for $number"
  fi
done

for word in build run step line job attempt case item test rank number no. version v; do
  for separator in '' ' '; do
    : > "$BRIEF_GH_CALLS"
    check "$word${separator}#42 is not an issue" 1 "Fix issue 115 after $word${separator}#42."
    if /usr/bin/grep -qx 'repos/{owner}/{repo}/issues/115/comments' "$BRIEF_GH_CALLS" &&
       ! /usr/bin/grep -q '/issues/42/comments$' "$BRIEF_GH_CALLS"; then
      pass=$((pass+1)); echo 'ok   #115 asked for; #42 excluded'
    else
      fail=$((fail+1)); echo 'FAIL wrong dispatch numbers for non-issue #42'
    fi
  done
done
: > "$BRIEF_GH_CALLS"
check "non-issue exclusions are case-insensitive" 0 'BUILD #42 RUN#42 STEP #42 LINE#42 JOB #42 ATTEMPT#42 CASE #42 ITEM#42 TEST #42 RANK#42 NUMBER #42 NO.#42 VERSION #42 V#42'
if [ ! -s "$BRIEF_GH_CALLS" ]; then
  pass=$((pass+1)); echo 'ok   non-issue numbers never query gh'
else
  fail=$((fail+1)); echo 'FAIL non-issue numbers queried gh'
fi

FETCH115='```console
gh api repos/example/project/issues/115/comments --paginate
[]
```'
SCOPE115='CONTROLLING SCOPE DECISION: #115 none (0 comments)'
check "fetch command shell comment still dispatches #797" 1 '#115
```console
gh api repos/example/project/issues/115/comments --paginate  # then fix #797
[]
```
CONTROLLING SCOPE DECISION: #115 none (0 comments)'
if [[ "$LAST_OUT" == *'#797'* ]]; then
  pass=$((pass+1)); echo 'ok   refusal names #797 from command comment'
else
  fail=$((fail+1)); echo 'FAIL command comment dispatch was hidden'
fi
check "command comment dispatch passes with its own evidence" 0 '#115
```console
gh api repos/example/project/issues/115/comments --paginate  # then fix #797
[]
```
CONTROLLING SCOPE DECISION: #115 none (0 comments)
```
gh api repos/example/project/issues/797/comments --paginate
[]
```
CONTROLLING SCOPE DECISION: #797 none (0 comments)'
check "quoted shell # is not a command comment" 0 '#115
```console
gh api repos/example/project/issues/115/comments --paginate --jq " #797"
[]
```
CONTROLLING SCOPE DECISION: #115 none (0 comments)'
check "non-reference command comments do not create issues" 0 "$FETCH115
$SCOPE115"

for reference in '#123456' '#1234567' 'issue 123456' 'issues/123456' 'pull/123456' \
                 'pull request 123456' 'PR 123456' 'PR #123456' 'GH-123456' \
                 '#12345suffix' 'issue 12345suffix' 'GH-12345suffix'; do
  : > "$BRIEF_GH_CALLS"
  check "out-of-range or non-word reference $reference is ignored" 0 "Discuss $reference."
  if [ ! -s "$BRIEF_GH_CALLS" ]; then
    pass=$((pass+1)); echo 'ok   rejected reference never reaches gh'
  else
    fail=$((fail+1)); echo 'FAIL rejected reference reached gh'
  fi
done
: > "$BRIEF_GH_CALLS"
check "five-digit issue is still a reference" 2 '#12345'
if /usr/bin/grep -qx 'repos/{owner}/{repo}/issues/12345/comments' "$BRIEF_GH_CALLS"; then
  pass=$((pass+1)); echo 'ok   five-digit reference reaches gh'
else
  fail=$((fail+1)); echo 'FAIL five-digit reference was ignored'
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
