#!/usr/bin/env bash
# Synthetic history only: all writes to Git metadata happen in throwaway
# repositories. Run on macOS or Linux without builds, network, or services.
set -euo pipefail
GUARD="$(cd "$(dirname "$0")/../scripts" && pwd)/check-contract-guard.sh"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
# Ignore developer signing, hooks, aliases, trailer configuration, and any
# enclosing Git environment so fixtures are deterministic.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_COUNT=0
repos=0
passed=0

new_repo() {
    repos=$((repos + 1))
    git init -q -b main "$scratch/repo-$repos"
    cd "$scratch/repo-$repos"
    git config user.name 'Contract guard test'
    git config user.email 'contract-guard@example.invalid'
    git config core.hooksPath /dev/null
    mkdir contracts
    printf 'original\n' > contracts/answer
    printf 'original\n' > implementation
    save 'initial fixture'
    base=$(git rev-parse HEAD)
}

save() {
    git add -A
    git -c commit.gpgsign=false commit -q -m "$1"
}

check() {
    local want="$1" label="$2" from="$3" to="$4" expected="${5:-}" out rc
    if out=$("$GUARD" "$from" "$to" 2>&1); then rc=0; else rc=$?; fi
    case "$want" in
        OK) [[ $rc -eq 0 && $out == "CONTRACT-GUARD OK $expected commits" ]] ;;
        ERROR) [[ $rc -eq 1 && $out == *'CONTRACT-GUARD ERROR '* && $out != *'CONTRACT-GUARD OK '* ]] ;;
        *) [[ $rc -eq 1 && $out == "CONTRACT-GUARD $expected $want "* && $out != *$'\n'* ]] ;;
    esac || { printf 'FAIL: %s (exit %s)\n%s\n' "$label" "$rc" "$out"; exit 1; }
    passed=$((passed + 1))
}

ok() { check OK "$1" "${3:-$base}" HEAD "$2"; }
bad() { check "$1" "$2" "${3:-$base}" HEAD "$(git rev-parse HEAD)"; }
reason=$'change contract\n\nContract-Change: correct the independently reviewed answer'

new_repo
printf 'implementation only\n' > implementation
save 'implementation only'
ok 'rule 1: implementation alone passes' 1
check OK 'empty range' HEAD HEAD 0
check ERROR 'unresolvable base fails closed' no-such-base HEAD
check ERROR 'unresolvable head fails closed' "$base" no-such-head
blob=$(git rev-parse HEAD:implementation)
check ERROR 'non-commit object fails closed' "$base" "$blob"

new_repo
printf 'new contract\n' > contracts/new
save 'new contract, no trailer needed'
ok 'rules 1 and 2: isolated addition passes without trailer' 1

new_repo
printf 'new contract\n' > contracts/new
printf 'mixed\n' > implementation
save "$reason"
bad 1 'rule 1: mixed addition fails even with trailer'

new_repo
printf 'changed\n' > contracts/answer
printf 'mixed\n' > implementation
save 'mixed, also missing trailer'
bad 1 'rule 1 takes precedence: exactly one line per commit'

new_repo
printf 'changed\n' > contracts/answer
save 'modify without reason'
bad 2 'rule 2: existing contract modification needs trailer'
first_bad=$(git rev-parse HEAD)
printf 'original\n' > contracts/answer
save "$reason"
check 2 'per-commit checking survives a later reversion' "$base" HEAD "$first_bad"

new_repo
printf 'changed\n' > contracts/answer
save "$reason"
ok 'rule 2: modification with trailer passes' 1
printf 'changed again\n' > contracts/answer
save 'second commit lacks reason'
bad 2 'per-commit checking: an earlier trailer cannot cover a later commit'

for message in 'no trailer' "$reason"; do
    new_repo
    rm contracts/answer
    save "$message"
    if [[ $message == 'no trailer' ]]; then
        bad 2 'rule 2: deletion without trailer fails'
    else
        ok 'rule 2: deletion with trailer passes' 1
    fi
    new_repo
    mv contracts/answer contracts/renamed
    save "$message"
    if [[ $message == 'no trailer' ]]; then
        bad 2 'rule 2: rename without trailer fails'
    else
        ok 'rule 2: rename with trailer passes' 1
    fi
done

new_repo
mv contracts/answer moved-out
save "$reason"
bad 1 'rule 1: moving out of contracts fails'
new_repo
mv implementation contracts/moved-in
save "$reason"
bad 1 'rule 1: moving into contracts fails'

for message in \
    $'change\n\nContract-Change: ' \
    $'change\n\nContract-Change: \t  ' \
    $'change\n\nContract-Change: mentioned in body\n\nThis is prose, not a trailer.' \
    $'change\n\nContract-Change: first line\n continued reason'; do
    new_repo
    printf 'changed\n' > contracts/answer
    save "$message"
    bad 2 'rule 2: empty, body-only, or multi-line reason is not a valid trailer'
done

new_repo
printf 'changed\n' > contracts/answer
git add -A
git -c commit.gpgsign=false commit -q --allow-empty-message -m ''
bad 2 'rule 2: an empty commit message fails with the rule number'

new_repo
printf 'changed\n' > contracts/answer
save 'Contract-Change: subject alone is not a trailer'
bad 2 'rule 2: a subject is not a trailer'

new_repo
printf 'changed\n' > contracts/answer
save "$reason"$'\nReviewed-by: Test Reviewer'
ok 'rule 2: trailer can share a block with other trailers' 1

new_repo
chmod +x contracts/answer
save 'mode change'
bad 2 'rule 2: mode-only changes need a trailer'

# Plain merge: the merge itself brings contracts AND implementation from
# the side parent. Check side commits too, and separately simulate master
# being excluded by the base (including a pre-guard, untrailed change).
new_repo
git checkout -q -b side
printf 'side contract\n' > contracts/answer
save 'pre-guard contract change'
side=$(git rev-parse HEAD)
git checkout -q main
printf 'main implementation\n' > implementation
save 'main implementation'
git merge -q --no-ff side -m 'plain master merge'
ok 'rule 3: inherited change from excluded master is exempt' 2 "$side"
check 2 'rule 3: side commit in the range is still checked' "$base" HEAD "$side"

new_repo
git checkout -q -b side
printf 'side contract\n' > contracts/answer
save "$reason"
printf 'side implementation\n' > implementation
save 'side implementation'
git checkout -q main
printf 'main file\n' > main-only
save 'main implementation'
git merge -q --no-ff side -m 'plain checked branch merge'
ok 'rule 3: checked branch merge is exempt, including mixed merge diff' 4

# New merge resolutions must go through rules 1 and 2, even when there is
# no textual merge conflict. --no-commit lets the fixture inject that edit.
for mixed in yes no; do
    new_repo
    git checkout -q -b side
    printf 'side contract\n' > contracts/answer
    save "$reason"
    if [[ $mixed == yes ]]; then
        printf 'side file\n' > side-implementation
        save 'side implementation'
    fi
    git checkout -q main
    printf 'main implementation\n' > implementation
    save 'main implementation'
    git merge -q --no-ff --no-commit side 2>/dev/null
    printf 'novel merge resolution\n' > contracts/answer
    save 'merge adds its own contract change'
    if [[ $mixed == yes ]]; then
        bad 1 'rule 3: novel merge contract plus implementation fails rule 1'
    else
        bad 2 'rule 3: novel merge contract without trailer fails rule 2'
        git -c commit.gpgsign=false commit -q --amend -m "$reason"
        ok 'rule 3: isolated novel merge resolution with trailer passes' 3
    fi
done

new_repo
git checkout -q -b side
rm contracts/answer
save "$reason"
git checkout -q main
printf 'main implementation\n' > implementation
save 'main implementation'
git merge -q --no-ff side -m 'inherit deletion'
ok 'rule 3: inherited deletion is exempt' 3

new_repo
git checkout -q -b side
printf 'side file\n' > side-implementation
save 'side implementation'
git checkout -q main
printf 'main implementation\n' > implementation
save 'main implementation'
git merge -q --no-ff --no-commit side 2>/dev/null
rm contracts/answer
save "$reason"
bad 1 'rule 3: novel deletion in a mixed merge fails rule 1'

# Octopus merge: different entries may be inherited from different parents.
new_repo
git checkout -q -b side-one
printf 'one\n' > 'contracts/answer[1]'
save 'add one'
git checkout -q -b side-two "$base"
printf 'two\n' > contracts/two
save 'add two'
git checkout -q main
printf 'main implementation\n' > implementation
save 'main implementation'
git merge -q --no-ff side-one side-two -m 'plain octopus merge' >/dev/null
ok 'rule 3: octopus inherits literal paths from separate parents' 4

new_repo
odd=$'contracts/space tab\tand newline\nanswer'
printf 'odd\n' > "$odd"
save 'add unusual filename'
odd_base=$(git rev-parse HEAD)
printf 'changed\n' > "$odd"
save 'change unusual filename'
bad 2 'NUL-delimited filenames stay on one diagnostic line' "$odd_base"

# A disconnected range includes a root; root additions still obey rule 1.
new_repo
git checkout -q --orphan contract-root
git rm -q -rf .
mkdir contracts
printf 'new\n' > contracts/root
save 'isolated root contract addition'
ok 'root contract additions pass without trailer' 1
printf 'mixed\n' > implementation
save 'implementation'
root_head=$(git rev-parse HEAD)
git checkout -q --orphan mixed-root
git rm -q -rf .
mkdir contracts
printf 'mixed\n' > contracts/root
printf 'mixed\n' > implementation
save 'mixed root'
bad 1 'root mixed contract addition fails rule 1' "$root_head"

# A local depth-limited fetch can resolve both endpoints yet lack ancestors.
git init -q -b main "$scratch/shallow"
git -C "$scratch/shallow" fetch -q --depth=1 "file://$PWD" HEAD
cd "$scratch/shallow"
check ERROR 'shallow history refuses approval' FETCH_HEAD FETCH_HEAD
cd "$scratch"
check ERROR 'outside a repository refuses approval' HEAD HEAD

printf 'OK: contract-guard self-test (%s cases)\n' "$passed"
