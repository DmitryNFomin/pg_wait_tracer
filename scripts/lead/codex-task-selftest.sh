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
cp "$here/brief_contract.py" "$here/run-budget.py" "$FIXTURE_REAL/scripts/lead/"
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
printf '%s\n' "$input" | grep -Fq '.codex-commit-msg' || exit 99
printf '%s\n' "$STUB_BRANCH" >> "$STUB_CALLS"
case "$STUB_MODE" in
    budget-noop|budget-dirty)
        if [ "$STUB_MODE" = budget-dirty ]; then
            printf 'partial tracked work\n' >> base.txt
            printf 'partial budget work\n' > task.txt
            printf 'new partial file\n' > partial.txt
            printf 'Budget partial work\n\nPreserve on timeout.\n' > .codex-commit-msg
        fi
        # Both parent and descendant ignore TERM: the supervisor must kill the group.
        exec python3 -c '
import os, signal, subprocess, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
child = subprocess.Popen(["python3", "-c", "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(60)"])
with open(os.environ["STUB_CHILD_PID"], "w") as f:
    f.write(str(child.pid))
time.sleep(60)
' ;;
    exit124)
        printf 'normal command failure\n' > task.txt
        exit 124 ;;
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
    dirty|dirty-locked|dirty-msg|commit-then-dirty-locked)
        # A round that commits early and then leaves more work it cannot commit.
        if [ "$STUB_MODE" = commit-then-dirty-locked ]; then
            printf 'early change\n' > early.txt
            git add early.txt
            git commit -qm 'stub early commit'
        fi
        # What the real implementer leaves behind when it cannot commit (#777):
        # a tracked edit, an untracked text file and an untracked binary.
        printf 'uncommitted change\n' >> task.txt
        printf 'brand new\n' > untracked.txt
        printf '\000\001\002binary\n' > bin.dat
        # dirty-locked also leaves an index lock behind, which is how a denied
        # or crashed `git add` inside the sandbox presents itself afterwards.
        case "$STUB_MODE" in dirty-locked|commit-then-dirty-locked) : > .git/index.lock ;; esac
        [ "$STUB_MODE" != dirty-msg ] ||
            printf 'Implementer subject line\n\nImplementer body.\n' > .codex-commit-msg
        exit 0 ;;
    *) exit 98 ;;
esac
STUB
cat > "$tmp/bin/git" <<'STUB'
#!/bin/sh
set -eu
case " $* " in
    *' clone '*|*' fetch '*) printf '%s\n' "$*" >> "$STUB_GIT_CALLS" ;;
esac
if [ "${1:-}" = -C ] && [ "${2:-}" = "$STUB_CLONE" ] &&
   [ "${3:-}" = fetch ] && [ "$STUB_GIT_MODE" = mutate-brief ]; then
    printf 'Changed source brief during fetch\n' > "$STUB_BRIEF"
fi
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
write_brief() {
    cat > "$tmp/brief" <<'BRIEF'
## Authorised by
Owner-approved wrapper test.
## Task
BRIEF_SENTINEL
## Excluded
Network and product code.
## Done
Run the fixture and verify its fetched commit.
## Budget
Rounds: 1
Wall-clock minutes: 30
## Report by
The test deadline.
BRIEF
}
write_brief
STUB_BRIEF="$tmp/brief"
export STUB_BRIEF
PGWT_LEAD_STATE_DIR="$tmp/lead-state"
export PGWT_LEAD_STATE_DIR
mkdir "$PGWT_LEAD_STATE_DIR"
record_brief() {
    digest=$(shasum -a 256 < "$tmp/brief")
    digest=${digest%% *}
    printf '%s %s %s\n' "${1:-$(date +%s)}" "$digest" "$tmp/brief" > "$PGWT_LEAD_STATE_DIR/.briefs-passed"
}
"$here/check-brief.sh" "$tmp/brief" > "$tmp/check-out" || fail 'valid brief was refused'
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
    [ "$expected_diff" = "${1:-task.txt}" ] || fail "unexpected task diff: $expected_diff"
}

# The wrapper's own commit (#777): the tip must carry the Codex trailer that
# scripts/board-author.py reads, and the message file must never be committed.
verify_auto_commit() {
    grep -Fq "AUTO_COMMIT: $sha" "$tmp/err" || fail 'no AUTO_COMMIT line naming the landed tip'
    "$REAL_GIT" -C "$FIXTURE_REAL" log -1 --format=%B "$sha" | "$REAL_GIT" interpret-trailers --parse |
        grep -Fqx 'Co-authored-by: Codex <noreply@openai.com>' || fail 'auto-commit lacks the Codex trailer'
    if "$REAL_GIT" -C "$FIXTURE_REAL" ls-tree -r --name-only "$sha" | grep -Fqx .codex-commit-msg; then
        fail 'the commit message file was committed'
    fi
    [ -z "$("$REAL_GIT" -C "$STUB_CLONE" status --porcelain)" ] || fail 'clone still dirty after auto-commit'
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
verify_success 'base.txt
prior-work.txt
task.txt'
verify_auto_commit
grep -Fq 'Reusing clone:' "$tmp/err" || fail 'reuse was not announced'
"$REAL_GIT" -C "$STUB_CLONE" merge-base --is-ancestor "$first_sha" "$sha" || fail 'prior commit was clobbered'
# Codex's own commit plus the wrapper's commit of what was left dirty.
[ "$("$REAL_GIT" -C "$STUB_CLONE" rev-list --count "$first_sha..$sha")" -eq 2 ] || fail 'reuse: expected the stub commit plus one auto-commit'
[ "$(cat "$STUB_CLONE/prior-work.txt")" = 'prior untracked work' ] || fail 'untracked work clobbered'
[ "$(cat "$STUB_CLONE/base.txt")" = 'prior dirty tracked work' ] || fail 'dirty tracked work clobbered'
[ "$("$REAL_GIT" -C "$FIXTURE_REAL" show "$sha:prior-work.txt")" = 'prior untracked work' ] || fail 'reuse: leftover work did not land'
printf '5 reuse: exit=0; %s; prior commit is ancestor, stub commit + one auto-commit, dirty tracked + untracked sentinels intact and landed\n' "$(grep '^Reusing clone:' "$tmp/err")"

cmp -s "$PGWT_LEAD_STATE_DIR/.briefs-passed" "$tmp/record-before" || fail 'passing record changed across retry'

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

# ---------------------------------------------------------------------------
# Salvage controls (#777): the wrapper's last job is to make uncommitted work
# reachable. The failure that matters is not a missing patch, it is a patch
# reported as "clean" or written incomplete while the work exists only in the
# clone, so each control below asserts the LOUD outcome.
# ---------------------------------------------------------------------------
salvage_path="$tmp/brief-salvage.patch"

new_branch salvage-happy
rm -f "$salvage_path"
run_wrapper dirty
# #777: a round that could not commit used to end BRANCH_UNCHANGED (21) with
# only a patch. The wrapper now commits it, so it must end like a committed one.
verify_success 'bin.dat
task.txt
untracked.txt'
verify_auto_commit
[ "$("$REAL_GIT" -C "$FIXTURE_REAL" log -1 --format=%s "$sha")" = "codex round on $STUB_BRANCH" ] ||
    fail 'auto-commit: wrong default subject'
"$REAL_GIT" -C "$FIXTURE_REAL" cat-file -e "$sha:bin.dat" || fail 'auto-commit: binary file did not land'
grep -Fq "SALVAGE_PATCH: $salvage_path" "$tmp/err" || fail 'salvage: no SALVAGE_PATCH line'
[ -s "$salvage_path" ] || fail 'salvage: patch is empty'
grep -Fq 'untracked.txt' "$salvage_path" || fail 'salvage: untracked file missing from the patch'
grep -Fq 'GIT binary patch' "$salvage_path" || fail 'salvage: binary file not captured with --binary'
"$REAL_GIT" -C "$STUB_CLONE" apply --check --reverse "$salvage_path" ||
    fail 'salvage: the patch does not describe the clone it came from'
printf 'extra salvage happy path: exit=%s; uncommitted round landed as %s with trailer; patch has untracked + binary and reverse-applies\n' "$rc" "$sha"

new_branch auto-commit-msg
rm -f "$salvage_path"
run_wrapper dirty-msg
verify_success 'bin.dat
task.txt
untracked.txt'
verify_auto_commit
[ "$("$REAL_GIT" -C "$FIXTURE_REAL" log -1 --format=%s "$sha")" = 'Implementer subject line' ] ||
    fail 'auto-commit: the implementer message was not used'
"$REAL_GIT" -C "$FIXTURE_REAL" log -1 --format=%b "$sha" | grep -Fqx 'Implementer body.' ||
    fail 'auto-commit: the implementer body was lost'
if grep -Fq '.codex-commit-msg' "$salvage_path"; then fail 'the commit message file leaked into the salvage patch'; fi
# A second round in the same clone must not reuse the first round's message.
run_wrapper dirty
[ "$rc" -eq 0 ] || fail "auto-commit second round: expected 0, got $rc"
sha=$("$REAL_GIT" -C "$FIXTURE_REAL" rev-parse "refs/heads/$STUB_BRANCH")
[ "$("$REAL_GIT" -C "$FIXTURE_REAL" log -1 --format=%s "$sha")" = "codex round on $STUB_BRANCH" ] ||
    fail 'auto-commit: a stale message from the previous round was reused'
printf 'extra auto-commit message: implementer subject + body used, file not committed, stale message not reused\n'

new_branch salvage-clean
rm -f "$salvage_path"
run_wrapper noop
[ "$rc" -eq 21 ] || fail "salvage-clean: expected 21, got $rc"
grep -Fq 'SALVAGE_PATCH: none (working tree clean)' "$tmp/err" ||
    fail 'salvage-clean: a clean tree must say so'
[ ! -e "$salvage_path" ] || fail 'salvage-clean: left an empty patch behind'
printf 'extra salvage clean tree: exit=%s; reports clean and writes nothing\n' "$rc"

new_branch salvage-unwritable
rm -f "$salvage_path"
mkdir -p "$tmp/nowrite"
cp "$tmp/brief" "$tmp/nowrite/brief.md"
chmod 500 "$tmp/nowrite"
STUB_MODE=dirty STUB_GIT_MODE=normal
export STUB_MODE STUB_GIT_MODE
rc=0
(cd "$tmp" && "$wrapper" "$tmp/nowrite/brief.md" "$STUB_BRANCH") > "$tmp/out" 2> "$tmp/err" || rc=$?
chmod 700 "$tmp/nowrite"
grep -Fq 'SALVAGE_FAILED' "$tmp/err" ||
    fail 'salvage-unwritable: an unwritable target must say SALVAGE_FAILED'
grep -Fq 'working tree clean' "$tmp/err" &&
    fail 'salvage-unwritable: reported a clean tree while work was only in the clone'
# The patch could not be written, but the commit path is independent of it.
[ "$rc" -eq 0 ] || fail "salvage-unwritable: the round should still land, got $rc"
sha=$("$REAL_GIT" -C "$FIXTURE_REAL" rev-parse "refs/heads/$STUB_BRANCH")
verify_auto_commit
printf 'extra salvage unwritable target: exit=%s; patch refused loudly, never says clean, round still landed\n' "$rc"

new_branch salvage-locked-index
rm -f "$salvage_path"
run_wrapper dirty-locked
rm -f "$STUB_CLONE/.git/index.lock"
grep -Fq 'SALVAGE_FAILED' "$tmp/err" ||
    fail 'salvage-locked-index: a locked index must say SALVAGE_FAILED, not a partial patch'
grep -Fq 'working tree clean' "$tmp/err" &&
    fail 'salvage-locked-index: reported a clean tree while work was only in the clone'
[ ! -s "$salvage_path" ] ||
    grep -Fq 'untracked.txt' "$salvage_path" ||
    fail 'salvage-locked-index: wrote a patch that silently omits the untracked files'
# Negative control for the commit path: when it cannot commit it must say so
# and must not report a landed branch.
[ "$rc" -eq 21 ] || fail "salvage-locked-index: expected BRANCH_UNCHANGED (21), got $rc"
grep -Fq 'AUTO_COMMIT_FAILED' "$tmp/err" || fail 'salvage-locked-index: a failed commit must say AUTO_COMMIT_FAILED'
[ ! -s "$tmp/out" ] || fail 'salvage-locked-index: printed a success block without a commit'
printf 'extra salvage locked index: exit=%s; refuses rather than writing a patch missing files\n' "$rc"

# The fail-open the reviewer found: the tip moved (an early commit) AND later
# work could not be committed. A moved tip must not turn that into success.
new_branch auto-commit-incomplete
rm -f "$salvage_path"
run_wrapper commit-then-dirty-locked
rm -f "$STUB_CLONE/.git/index.lock"
expect_refusal 26 AUTO_COMMIT_INCOMPLETE
grep -Fq 'AUTO_COMMIT_FAILED' "$tmp/err" || fail 'auto-commit-incomplete: the failed commit was not reported'
[ -n "$("$REAL_GIT" -C "$STUB_CLONE" status --porcelain)" ] || fail 'auto-commit-incomplete: control is vacuous, the clone is clean'
printf 'extra auto-commit incomplete: exit=%s; %s; stdout empty (no success block)\n' "$rc" "$(grep '^AUTO_COMMIT_INCOMPLETE:' "$tmp/err" | cut -c1-60)"


new_branch unchecked-brief
printf '%s %064d %s\n' "$(date +%s)" 0 "$tmp/brief" > "$PGWT_LEAD_STATE_DIR/.briefs-passed"
expect_unchecked
printf 'extra unchecked brief: exit=%s; no clone, lock, fetch or Codex invocation\n' "$rc"

new_branch edited-brief
record_brief
printf 'edited after validation\n' >> "$tmp/brief"
expect_unchecked
printf 'extra edited brief: exit=%s; previous content record rejected\n' "$rc"
write_brief

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
new_branch frozen-brief
write_brief
"$here/check-brief.sh" "$tmp/brief" > "$tmp/check-out" || fail 'brief refused'
run_wrapper commit mutate-brief
verify_success
[ "$(cat "$tmp/brief")" = 'Changed source brief during fetch' ] || fail 'brief mutation control never ran'
printf 'extra frozen brief: exit=0; dispatched checked bytes despite source edit during fetch\n'

new_branch command-exit124
write_brief
"$here/check-brief.sh" "$tmp/brief" > "$tmp/check-out" || fail 'brief refused'
run_wrapper exit124
expect_refusal 24 CODEX_FAILED
printf 'extra command exit 124: exit=24 CODEX_FAILED; distinguished from deadline expiry\n'

# A checked brief with a wall-clock budget must stop the whole process group,
# auto-commit tracked/untracked partial work, and fetch it before refusing.
STUB_CHILD_PID="$tmp/child-pid"
export STUB_CHILD_PID
for mode in budget-dirty budget-noop; do
    new_branch "$mode"
    write_brief
    sed 's/Wall-clock minutes: 30/Wall-clock minutes: 0.02/' "$tmp/brief" > "$tmp/short-brief"
    mv "$tmp/short-brief" "$tmp/brief"
    "$here/check-brief.sh" "$tmp/brief" > "$tmp/check-out" || fail 'short budget brief refused'
    started=$(date +%s)
    run_wrapper "$mode"
    elapsed=$(( $(date +%s) - started ))
    expect_refusal 27 BUDGET_EXCEEDED
    [ "$elapsed" -lt 15 ] || fail 'budget did not bound execution time'
    [ -s "$STUB_CHILD_PID" ] || fail 'budget fixture never started child'
    child=$(cat "$STUB_CHILD_PID")
    # A killed child can remain a zombie until the OS reaps it; it cannot write.
    state=$(ps -o stat= -p "$child" 2>/dev/null || true)
    case "$state" in ''|*Z*) ;; *) fail "budget left child running: $child ($state)" ;; esac
    sha=$("$REAL_GIT" -C "$STUB_CLONE" rev-parse HEAD)
    landed=$("$REAL_GIT" -C "$FIXTURE_REAL" rev-parse "refs/heads/$STUB_BRANCH")
    [ "$sha" = "$landed" ] || fail 'budget failure lost fetched work'
    if [ "$mode" = budget-dirty ]; then
        verify_auto_commit
        "$REAL_GIT" -C "$FIXTURE_REAL" show "$sha:base.txt" | grep -Fqx 'partial tracked work' || fail 'partial tracked work lost'
        [ "$("$REAL_GIT" -C "$FIXTURE_REAL" show "$sha:partial.txt")" = 'new partial file' ] || fail 'partial untracked work lost'
        [ "$("$REAL_GIT" -C "$FIXTURE_REAL" log -1 --format=%s "$sha")" = 'Budget partial work' ] || fail 'partial message lost'
        grep -Fq "work was fetched and verified at $sha" "$tmp/err" || fail 'budget salvage message lost'
        outcome='work fetched'
    else
        [ "$sha" = "$fresh" ] || fail 'no-op budget moved branch'
        grep -Fq "branch did not move; no new work was committed; unchanged tip fetched and verified at $sha" "$tmp/err" || fail 'no-op budget message is inaccurate'
        if grep -Fq 'work was fetched and verified' "$tmp/err"; then fail 'no-op budget claims fetched work'; fi
        outcome='branch unchanged; no new work committed; tip fetched and verified'
    fi
    printf 'extra %s: exit=27; process group stopped; %s; elapsed=%ss\n' "$mode" "$outcome" "$elapsed"
done
printf 'PASS: existing wrapper/salvage controls, six brief-gate refusals, and budget stop/salvage/fetch controls\n'
