#!/bin/sh
# Deterministic, network-free tests of the on-disk wrapper. Only fixture .git
# directories are written. PATH stubs Codex; Git is real except for explicitly
# injected fetch/cat-file failures. Each test owns unique external clone paths.
set -eu
here=$(CDPATH='' cd -P "$(dirname "$0")" && pwd -P)
wrapper="$here/codex-task.sh"
[ -x "$wrapper" ] || { printf 'FAIL: wrapper must be executable\n' >&2; exit 1; }
tmp=$(mktemp -d /private/tmp/pgwt-codex-selftest.XXXXXX)
prefix=${tmp##*/}
root=/private/tmp/pgwt-codex
cleanup() {
    if [ -f "$tmp/owned-paths" ]; then
        while IFS= read -r owned; do rm -rf "$owned"; done < "$tmp/owned-paths"
    fi
    rm -rf "$tmp"
}
trap cleanup 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fail() { printf 'FAIL: %s\n' "$*" >&2; cat "$tmp/out" "$tmp/err" >&2; exit 1; }
: > "$tmp/out"
: > "$tmp/err"
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME='Harness Test' GIT_COMMITTER_NAME='Harness Test'
export GIT_AUTHOR_EMAIL='harness@example.invalid' GIT_COMMITTER_EMAIL='harness@example.invalid'
REAL_GIT=$(command -v git)
export REAL_GIT
"$REAL_GIT" init -q --initial-branch=master "$tmp/seed"
printf 'base\n' > "$tmp/seed/base.txt"
"$REAL_GIT" -C "$tmp/seed" add base.txt
"$REAL_GIT" -C "$tmp/seed" commit -qm base
"$REAL_GIT" clone -q --bare "$tmp/seed" "$tmp/upstream.git"
"$REAL_GIT" clone -q "$tmp/upstream.git" "$tmp/real"
# Move upstream after cloning the real repository: the task must fetch a fresh
# base rather than accidentally cutting from the source's stale origin/master.
"$REAL_GIT" -C "$tmp/seed" commit -q --allow-empty -m 'fresh upstream'
"$REAL_GIT" -C "$tmp/upstream.git" fetch -q "$tmp/seed" refs/heads/master:refs/heads/master
fresh=$("$REAL_GIT" -C "$tmp/seed" rev-parse HEAD)
FIXTURE_REAL="$tmp/real"
# Execute an exact copy in the fixture repository, including executable mode;
# the wrapper identifies the real repository by its own location, not cwd.
mkdir -p "$FIXTURE_REAL/scripts/lead"
cp "$wrapper" "$FIXTURE_REAL/scripts/lead/codex-task.sh"
cmp -s "$wrapper" "$FIXTURE_REAL/scripts/lead/codex-task.sh" || fail 'fixture wrapper differs from on-disk bytes'
wrapper="$FIXTURE_REAL/scripts/lead/codex-task.sh"
STUB_CALLS="$tmp/codex-calls"
STUB_GIT_CALLS="$tmp/git-calls"
export FIXTURE_REAL STUB_CALLS STUB_GIT_CALLS
: > "$STUB_CALLS"
: > "$STUB_GIT_CALLS"
mkdir "$tmp/bin"
cat > "$tmp/bin/codex" <<'STUB'
#!/bin/sh
set -eu
[ "$#" -eq 6 ] && [ "$1" = exec ] && [ "$2" = --cd ] &&
    [ "$3" = "$STUB_CLONE" ] && [ "$4" = --sandbox ] &&
    [ "$5" = workspace-write ] && [ "$6" = - ] || exit 91
[ "$(pwd -P)" = "$STUB_CLONE" ] || exit 92
[ "$(git branch --show-current)" = "$STUB_BRANCH" ] || exit 93
input=$(cat)
printf '%s\n' "$input" | grep -Fqx "Working directory: $STUB_CLONE" || exit 94
printf '%s\n' "$input" | grep -Fqx "Branch: $STUB_BRANCH" || exit 95
printf '%s\n' "$input" | grep -Fq 'Do not create your own clone.' || exit 96
printf '%s\n' "$input" | grep -Fqx BRIEF_SENTINEL || exit 97
printf '%s\n' "$STUB_BRANCH" >> "$STUB_CALLS"
case "$STUB_MODE" in
    noop) exit 0 ;;
    missing)
        git checkout -q --detach
        git branch -D "$STUB_BRANCH" >/dev/null
        exit 0 ;;
    commit|commit-fail)
        printf 'task change\n' >> task.txt
        git add task.txt
        git commit -qm 'stub task commit'
        [ "$STUB_MODE" != commit-fail ] || exit 7 ;;
    *) exit 98 ;;
esac
STUB
cat > "$tmp/bin/git" <<'STUB'
#!/bin/sh
set -eu
case " $* " in
    *' clone '*|*' fetch '*) printf '%s\n' "$*" >> "$STUB_GIT_CALLS" ;;
esac
if [ "${1:-}" = -C ] && [ "${2:-}" = "$FIXTURE_REAL" ]; then
    if [ "${3:-}" = fetch ] && [ "$STUB_GIT_MODE" = fetch-fail ]; then
        echo 'stub git: forced fetch failure' >&2
        exit 73
    fi
    if [ "${3:-}" = cat-file ] && [ "$STUB_GIT_MODE" = unreachable ]; then
        echo 'stub git: forced unreachable object' >&2
        exit 74
    fi
fi
exec "$REAL_GIT" "$@"
STUB
chmod 755 "$tmp/bin/codex" "$tmp/bin/git"
PATH="$tmp/bin:$PATH"
export PATH
printf 'BRIEF_SENTINEL\n' > "$tmp/brief"
PGWT_LEAD_STATE_DIR="$tmp/lead-state"
export PGWT_LEAD_STATE_DIR
mkdir "$PGWT_LEAD_STATE_DIR"
record_brief() {
    digest=$(shasum -a 256 < "$tmp/brief")
    digest=${digest%% *}
    printf '%s %s %s\n' "${1:-$(date +%s)}" "$digest" "$tmp/brief" > "$PGWT_LEAD_STATE_DIR/.briefs-passed"
}
record_brief
cp "$PGWT_LEAD_STATE_DIR/.briefs-passed" "$tmp/record-before"
mkdir -p "$root"

new_branch() {
    STUB_BRANCH="$prefix/$1"
    flat=$(printf '%s' "$STUB_BRANCH" | sed 's/%/%25/g; s,/,%2F,g')
    STUB_CLONE="$root/$flat"
    [ ! -e "$STUB_CLONE" ] && [ ! -L "$STUB_CLONE" ] || fail 'test path already exists'
    [ ! -e "$STUB_CLONE.lock" ] && [ ! -L "$STUB_CLONE.lock" ] || fail 'test lock already exists'
    printf '%s\n' "$STUB_CLONE" "$STUB_CLONE.lock" >> "$tmp/owned-paths"
    export STUB_BRANCH STUB_CLONE
}
run_wrapper() {
    STUB_MODE=$1
    STUB_GIT_MODE=${2:-normal}
    export STUB_MODE STUB_GIT_MODE
    rc=0
    (cd "$tmp" && "$wrapper" "$tmp/brief" "$STUB_BRANCH") > "$tmp/out" 2> "$tmp/err" || rc=$?
}
expect_refusal() {
    [ "$rc" -eq "$1" ] || fail "expected exit $1, got $rc"
    grep -Fq "$2" "$tmp/err" || fail "missing diagnostic: $2"
    [ ! -s "$tmp/out" ] || fail 'failure printed a success block or unverified SHA'
}
expect_unchecked() {
    calls_before=$(/usr/bin/grep -c . "$STUB_CALLS" || true)
    git_before=$(/usr/bin/grep -c . "$STUB_GIT_CALLS" || true)
    run_wrapper commit
    expect_refusal 15 'BRIEF_UNCHECKED: run scripts/lead/check-brief.sh <brief> first (no passing record for this exact brief content)'
    [ ! -e "$STUB_CLONE" ] && [ ! -e "$STUB_CLONE.lock" ] || fail 'unchecked brief created clone or lock'
    [ "$(/usr/bin/grep -c . "$STUB_CALLS" || true)" = "$calls_before" ] || fail 'unchecked brief ran Codex'
    [ "$(/usr/bin/grep -c . "$STUB_GIT_CALLS" || true)" = "$git_before" ] || fail 'unchecked brief ran clone/fetch'
}
verify_success() {
    [ "$rc" -eq 0 ] || fail "expected success, got exit $rc"
    sha=$("$REAL_GIT" -C "$STUB_CLONE" rev-parse "refs/heads/$STUB_BRANCH")
    landed=$("$REAL_GIT" -C "$FIXTURE_REAL" rev-parse "refs/heads/$STUB_BRANCH")
    [ "$sha" = "$landed" ] || fail 'branch was not fetched'
    [ "$("$REAL_GIT" -C "$FIXTURE_REAL" cat-file -t "$sha")" = commit ] || fail 'SHA unreachable in real repo'
    expected_diff=$("$REAL_GIT" -C "$FIXTURE_REAL" diff --name-only "origin/master...$STUB_BRANCH")
    printf '%s\n' "$STUB_CLONE" "$STUB_BRANCH" "$sha" "$expected_diff" > "$tmp/expected"
    cmp -s "$tmp/expected" "$tmp/out" || fail 'final output differs from path, branch, SHA, diff'
    [ "$expected_diff" = task.txt ] || fail 'unexpected task diff'
}

new_branch happy
happy_branch=$STUB_BRANCH
happy_clone=$STUB_CLONE
run_wrapper commit
verify_success
first_sha=$sha
[ "$("$REAL_GIT" -C "$STUB_CLONE" rev-parse "$sha^")" = "$fresh" ] || fail 'branch was not cut from current upstream master'
[ -s "$STUB_CLONE/.git/objects/info/alternates" ] || fail 'clone did not share local objects'
printf '1 happy: exit=0 clone=%s branch=%s sha=%s reachable=commit diff=%s; fresh base + shared objects verified\n' "$STUB_CLONE" "$STUB_BRANCH" "$sha" "$expected_diff"

new_branch noop
run_wrapper noop
expect_refusal 21 'branch did not move'
printf '2 no commit: exit=%s; %s\n' "$rc" "$(grep '^BRANCH_UNCHANGED:' "$tmp/err")"

new_branch agent/foo-bar
run_wrapper commit
verify_success
[ "$(dirname "$STUB_CLONE")" = "$root" ] || fail 'slash created nested directory'
[ ! -d "$root/$prefix" ] || fail 'unexpected nested branch path'
printf '3 slash: exit=0 clone=%s; one flat directory verified\n' "$STUB_CLONE"

new_branch unsafe
mkdir "$FIXTURE_REAL/inside"
ln -s "$FIXTURE_REAL/inside" "$STUB_CLONE"
calls_before=$(wc -l < "$STUB_CALLS")
git_before=$(wc -l < "$STUB_GIT_CALLS")
run_wrapper commit
expect_refusal 12 'clone path resolves inside the repository'
[ "$(wc -l < "$STUB_CALLS")" = "$calls_before" ] || fail 'Codex ran on unsafe path'
[ "$(wc -l < "$STUB_GIT_CALLS")" = "$git_before" ] || fail 'clone/fetch ran on unsafe path'
[ -z "$(ls -A "$FIXTURE_REAL/inside")" ] || fail 'unsafe destination was written'
printf '4 inside repo (forced symlink): exit=%s; %s; no clone/fetch/Codex, destination untouched\n' "$rc" "$(grep '^UNSAFE_CLONE_PATH:' "$tmp/err")"

STUB_BRANCH=$happy_branch
STUB_CLONE=$happy_clone
printf 'prior untracked work\n' > "$STUB_CLONE/prior-work.txt"
printf 'prior dirty tracked work\n' > "$STUB_CLONE/base.txt"
run_wrapper commit
verify_success
grep -Fq 'Reusing clone:' "$tmp/err" || fail 'reuse was not announced'
"$REAL_GIT" -C "$STUB_CLONE" merge-base --is-ancestor "$first_sha" "$sha" || fail 'prior commit was clobbered'
[ "$("$REAL_GIT" -C "$STUB_CLONE" rev-list --count "$first_sha..$sha")" -eq 1 ] || fail 'reuse lost prior history'
[ "$(cat "$STUB_CLONE/prior-work.txt")" = 'prior untracked work' ] || fail 'untracked work clobbered'
[ "$(cat "$STUB_CLONE/base.txt")" = 'prior dirty tracked work' ] || fail 'dirty tracked work clobbered'
cmp -s "$PGWT_LEAD_STATE_DIR/.briefs-passed" "$tmp/record-before" || fail 'passing record consumed or changed across dispatch/retry'
printf '5 reuse: exit=0; %s; prior commit is ancestor, exactly one new commit, dirty tracked + untracked sentinels intact\n' "$(grep '^Reusing clone:' "$tmp/err")"

new_branch fetch-fail
run_wrapper commit fetch-fail
expect_refusal 22 FETCH_FAILED
if "$REAL_GIT" -C "$FIXTURE_REAL" show-ref --verify --quiet "refs/heads/$STUB_BRANCH"; then fail 'failed fetch created destination branch'; fi
printf '6 fetch failure: exit=%s; %s; stdout empty (no SHA), destination branch absent\n' "$rc" "$(grep '^FETCH_FAILED:' "$tmp/err")"

new_branch missing-brief
mv "$tmp/brief" "$tmp/saved-brief"
calls_before=$(wc -l < "$STUB_CALLS")
run_wrapper commit
expect_refusal 10 BRIEF_MISSING
[ "$(wc -l < "$STUB_CALLS")" = "$calls_before" ] || fail 'Codex ran without brief'
[ ! -e "$STUB_CLONE" ] || fail 'missing brief created clone'
mv "$tmp/saved-brief" "$tmp/brief"
printf 'extra missing brief: exit=%s; %s\n' "$rc" "$(grep '^BRIEF_MISSING:' "$tmp/err")"

new_branch missing-branch
run_wrapper missing
expect_refusal 20 BRANCH_MISSING
printf 'extra deleted branch: exit=%s; %s\n' "$rc" "$(grep '^BRANCH_MISSING:' "$tmp/err")"

new_branch unreachable
run_wrapper commit unreachable
expect_refusal 23 COMMIT_UNREACHABLE
printf 'extra unreachable commit (forced cat-file failure): exit=%s; %s; stdout empty\n' "$rc" "$(grep '^COMMIT_UNREACHABLE:' "$tmp/err")"

new_branch codex-fail
run_wrapper commit-fail
expect_refusal 24 CODEX_FAILED
sha=$("$REAL_GIT" -C "$STUB_CLONE" rev-parse "refs/heads/$STUB_BRANCH")
[ "$("$REAL_GIT" -C "$FIXTURE_REAL" rev-parse "refs/heads/$STUB_BRANCH")" = "$sha" ] || fail 'Codex failure lost committed work'
[ "$("$REAL_GIT" -C "$FIXTURE_REAL" cat-file -t "$sha")" = commit ] || fail 'salvaged commit unreachable'
printf 'extra Codex failure: exit=%s; %s; stdout empty\n' "$rc" "$(grep '^CODEX_FAILED:' "$tmp/err")"

new_branch unchecked-brief
printf '%s %064d %s\n' "$(date +%s)" 0 "$tmp/brief" > "$PGWT_LEAD_STATE_DIR/.briefs-passed"
expect_unchecked
printf 'extra unchecked brief: exit=%s; no clone, lock, fetch or Codex invocation\n' "$rc"

new_branch edited-brief
record_brief
printf 'edited after validation\n' >> "$tmp/brief"
expect_unchecked
printf 'extra edited brief: exit=%s; previous content record rejected\n' "$rc"
printf 'BRIEF_SENTINEL\n' > "$tmp/brief"

new_branch stale-brief
record_brief "$(( $(date +%s) - 86401 ))"
expect_unchecked
printf 'extra stale brief: exit=%s; record older than 24h rejected\n' "$rc"

new_branch missing-records
rm "$PGWT_LEAD_STATE_DIR/.briefs-passed"
expect_unchecked
printf 'extra missing records: exit=%s; no state file created\n' "$rc"
[ ! -e "$PGWT_LEAD_STATE_DIR/.briefs-passed" ] || fail 'refusal created brief records'

new_branch unreadable-records
mkdir "$PGWT_LEAD_STATE_DIR/.briefs-passed"
expect_unchecked
rmdir "$PGWT_LEAD_STATE_DIR/.briefs-passed"
printf 'extra unreadable records (directory): exit=%s\n' "$rc"

new_branch unavailable-hash
record_brief
cat > "$tmp/bin/shasum" <<'STUB'
#!/bin/sh
exit 127
STUB
chmod 755 "$tmp/bin/shasum"
expect_unchecked
rm "$tmp/bin/shasum"
printf 'extra unavailable hash tool: exit=%s\n' "$rc"
printf 'PASS: existing wrapper controls, reusable checked brief, and six brief-gate refusals\n'
