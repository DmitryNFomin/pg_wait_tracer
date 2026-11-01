#!/usr/bin/env bash
# test_pre_push_hook.sh -- exercises scripts/git-hooks/pre-push as git itself
# invokes it: a REAL `git push` against a disposable local bare remote, from
# a disposable repo and from a linked worktree of it. No text parsing, no
# synthetic JSON -- every case below runs the actual hook at the actual
# invocation (issue #230, replacing scripts/hooks/push-guard.sh's
# command-text parser and its tests/test_push_guard.py, both retired here).
#
# core.hooksPath is set to an ABSOLUTE path pointing at the PRIMARY repo's
# scripts/git-hooks/ (reproducing how this project's own clone is actually
# configured -- #467) to prove the hook still resolves per-PUSHING-worktree
# tree/stamp even though the hook script itself is read from elsewhere.
#
# No network, no GitHub: the remote is a local --bare repo.
#
# Usage: tests/test_pre_push_hook.sh (run from anywhere; resolves its own dir)
# PGWT_TEST_PRE_PUSH_HOOK can supply an alternate hook script to exercise
# (e.g. a deliberately regressed one, to show these checks go red against
# the bug they were written for -- same convention test_push_guard.py used).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOK_SRC="${PGWT_TEST_PRE_PUSH_HOOK:-$REPO_ROOT/scripts/git-hooks/pre-push}"
HASH_SRC="$REPO_ROOT/scripts/tree-hash.sh"

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}

matches() { grep -q -- "$2" <<<"$1"; }
check()              { if [[ "$1" == "true" ]]; then report true "$2"; else report false "$2"; fi; }
check_contains()     { if matches "$1" "$2"; then report true "$3"; else report false "$3"; fi; }
check_not_contains() { if matches "$1" "$2"; then report false "$3"; else report true "$3"; fi; }

# Keep inherited Git repository/configuration overrides out of these
# fixtures, same isolation the old test_push_guard.py used.
isolate_git_env() {
    unset "${!GIT_@}" 2>/dev/null || true
    export GIT_CONFIG_NOSYSTEM=1
    export GIT_CONFIG_GLOBAL=/dev/null
}
isolate_git_env

tmproot="$(mktemp -d)"
trap 'rm -rf "$tmproot"' EXIT

git_q() { git -C "$1" "${@:2}" >/dev/null 2>&1; }

make_fixture() {
    local base="$1"
    rm -rf "$base"
    mkdir -p "$base/main/scripts/git-hooks" "$base/main/scripts"
    cp "$HOOK_SRC" "$base/main/scripts/git-hooks/pre-push"
    chmod +x "$base/main/scripts/git-hooks/pre-push"
    cp "$HASH_SRC" "$base/main/scripts/tree-hash.sh"
    chmod +x "$base/main/scripts/tree-hash.sh"
    echo ".pgwt-check.stamp" > "$base/main/.gitignore"
    echo "one" > "$base/main/data.txt"
    git_q "$base/main" init -q -b main
    git_q "$base/main" config --local user.email test@example.com
    git_q "$base/main" config --local user.name Test
    git_q "$base/main" add -A
    git_q "$base/main" commit -q -m initial
    # ABSOLUTE core.hooksPath, pointing at the PRIMARY checkout -- the #467
    # configuration -- shared via the common .git/config, so the linked
    # worktree below inherits it too without any per-worktree setup.
    git_q "$base/main" config --local core.hooksPath "$base/main/scripts/git-hooks"
    git -C "$base/main" init -q --bare "$base/remote.git"
    git_q "$base/main" remote add origin "$base/remote.git"
    git_q "$base/main" worktree add -q -b feature "$base/wt" main
}

# Each worktree's own tree-hash output, computed via ITS OWN copy of
# scripts/tree-hash.sh (relative-to-script resolution, same as the hook).
tree_hash_of() { ( cd "$1" && ./scripts/tree-hash.sh ); }

stamp_correctly() { tree_hash_of "$1" > "$1/.pgwt-check.stamp"; }
stamp_stale()     { echo "0000000000000000000000000000000000000000" > "$1/.pgwt-check.stamp"; }

do_push() {
    # $1 = worktree dir, $2 = branch, rest = extra env assignments (VAR=val)
    local dir="$1" branch="$2"
    git -C "$dir" push origin "$branch" 2>&1
}

# ── Case 1: no stamp at all -- refused, message names the missing stamp ──
base="$tmproot/c1"; make_fixture "$base"
out=$(do_push "$base/main" main); rc=$?
check "$([[ $rc -ne 0 ]] && echo true || echo false)" \
    "no .pgwt-check.stamp: push is refused (nonzero exit)"
check_contains "$out" "no .pgwt-check.stamp" \
    "no .pgwt-check.stamp: message names the missing-stamp case"

# ── Case 2: stamp present but stale (tree changed since) -- refused ──────
base="$tmproot/c2"; make_fixture "$base"
stamp_stale "$base/main"
out=$(do_push "$base/main" main); rc=$?
check "$([[ $rc -ne 0 ]] && echo true || echo false)" \
    "stale stamp: push is refused (nonzero exit)"
check_contains "$out" "tree changed since the last passing" \
    "stale stamp: message names the mismatch/dirty-tree case, distinct from stamp-missing"

# ── Case 3: stamp matches the real tree -- push SUCCEEDS ─────────────────
base="$tmproot/c3"; make_fixture "$base"
stamp_correctly "$base/main"
out=$(do_push "$base/main" main); rc=$?
check "$([[ $rc -eq 0 ]] && echo true || echo false)" \
    "correct stamp: push succeeds"
check_contains "$out" "new branch" \
    "correct stamp: the ref actually landed on the remote"

# ── Cases 1-3, repeated from the LINKED WORKTREE (the case #203/#241 got
#    wrong: resolving the main checkout's tree/stamp instead of the
#    worktree actually being pushed from) ────────────────────────────────
base="$tmproot/c4"; make_fixture "$base"
# Give the two trees genuinely different content/hashes so a wrong-repo
# resolution would be caught by hash mismatch, not accidentally agree.
echo "worktree-only change" >> "$base/wt/data.txt"
out=$(do_push "$base/wt" feature); rc=$?
check "$([[ $rc -ne 0 ]] && echo true || echo false)" \
    "linked worktree, no stamp: push is refused"
check_contains "$out" "no .pgwt-check.stamp" \
    "linked worktree, no stamp: message names the missing-stamp case"

base="$tmproot/c5"; make_fixture "$base"
echo "worktree-only change" >> "$base/wt/data.txt"
stamp_correctly "$base/wt"
# Main's own tree is intentionally UNSTAMPED and would fail if the hook
# resolved to the wrong repo.
out=$(do_push "$base/wt" feature); rc=$?
check "$([[ $rc -eq 0 ]] && echo true || echo false)" \
    "linked worktree, correctly stamped FOR ITS OWN TREE: push succeeds \
(proves the hook checked the worktree's tree, not the main checkout's)"

# ── Equivalence of invocation shapes: git -C, two-line cd, && chain ──────
# This is the point of #230/#241: the old text parser treated these three
# differently (one worked, one was refused as "ambiguous", one mis-resolved
# the repository). The real hook runs on the actual push regardless of how
# the shell got there, so all three must now agree.
base="$tmproot/c6"; make_fixture "$base"
echo "worktree-only change" >> "$base/wt/data.txt"
stamp_correctly "$base/wt"
out_gitc=$(git -C "$base/wt" push origin feature 2>&1); rc_gitc=$?
# Re-stamp before each re-push: a real second push of the same ref is a
# no-op update, so re-run the same scenario fresh for each shape.
base="$tmproot/c6b"; make_fixture "$base"
echo "worktree-only change" >> "$base/wt/data.txt"
stamp_correctly "$base/wt"
out_cd=$(bash -c 'cd "$1"
git push origin "$2"' _ "$base/wt" feature 2>&1); rc_cd=$?
base="$tmproot/c6c"; make_fixture "$base"
echo "worktree-only change" >> "$base/wt/data.txt"
stamp_correctly "$base/wt"
out_chain=$(bash -c 'cd "$1" && git push origin "$2"' _ "$base/wt" feature 2>&1); rc_chain=$?
if [[ $rc_gitc -ne 0 || $rc_cd -ne 0 || $rc_chain -ne 0 ]]; then
    printf 'git -C: rc=%s %s\ncd two-line: rc=%s %s\n&&-chain: rc=%s %s\n' \
        "$rc_gitc" "$out_gitc" "$rc_cd" "$out_cd" "$rc_chain" "$out_chain" >&2
fi
check "$([[ $rc_gitc -eq 0 && $rc_cd -eq 0 && $rc_chain -eq 0 ]] && echo true || echo false)" \
    "git -C <wt> push / two-line cd+push / &&-chained push all succeed identically for a verified tree"

# Same equivalence on the REFUSED side (no stamp at all).
base="$tmproot/c7"; make_fixture "$base"
rc_gitc=0; git -C "$base/wt" push origin feature >/dev/null 2>&1 || rc_gitc=$?
base="$tmproot/c7b"; make_fixture "$base"
rc_cd=0; bash -c 'cd "$1"
git push origin "$2"' _ "$base/wt" feature >/dev/null 2>&1 || rc_cd=$?
base="$tmproot/c7c"; make_fixture "$base"
rc_chain=0; bash -c 'cd "$1" && git push origin "$2"' _ "$base/wt" feature >/dev/null 2>&1 || rc_chain=$?
check "$([[ $rc_gitc -ne 0 && $rc_cd -ne 0 && $rc_chain -ne 0 ]] && echo true || echo false)" \
    "git -C <wt> push / two-line cd+push / &&-chained push are all refused identically for an unstamped tree"

# ── A command containing an odd number of apostrophes, and NO push at all,
#    must be completely unaffected -- there is no PreToolUse text parser
#    left to misfire on it (the old scripts/hooks/push-guard.sh blocked
#    exactly this shape; this is the #241/#230 "quoted mention" case). ────
# This exact shape -- an odd number of apostrophes in a heredoc, no push
# anywhere in it -- is what scripts/hooks/push-guard.sh's command-text
# parser rejected outright with "invalid or unparseable tool command: No
# closing quotation" (#230/#241), even though nothing here is a push. With
# that parser retired there is nothing left to inspect this command's TEXT
# at all; it just runs as an ordinary shell command.
odd_file="$tmproot/odd.txt"
cat > "$odd_file" <<'EOF'
it can't be a push; neither can 'tests' alone
EOF
rc=$?
check "$([[ $rc -eq 0 ]] && echo true || echo false)" \
    "a heredoc with an odd number of apostrophes and no push runs to completion (exit 0)"
check "$([[ "$(cat "$odd_file")" == "it can't be a push; neither can 'tests' alone" ]] && echo true || echo false)" \
    "...and its content is exactly what was written, unaltered by anything resembling the old parser"

# ── PGWT_SKIP_PUSH_GUARD still bypasses, for a human ──────────────────────
base="$tmproot/c8"; make_fixture "$base"
out=$(PGWT_SKIP_PUSH_GUARD=1 git -C "$base/main" push origin main 2>&1); rc=$?
check "$([[ $rc -eq 0 ]] && echo true || echo false)" \
    "PGWT_SKIP_PUSH_GUARD=1 bypasses the hook even with no stamp"

# ── Refuse, don't approve, when the hook cannot tell: tree-hash.sh absent
#    from the pushing worktree (a tool the check depends on is missing). A
#    stamp must exist first, or the hook would refuse at the earlier
#    stamp-missing check without ever reaching the tool-missing path this
#    case targets. ────────────────────────────────────────────────────────
base="$tmproot/c9"; make_fixture "$base"
stamp_stale "$base/main"
rm -f "$base/main/scripts/tree-hash.sh"
out=$(do_push "$base/main" main); rc=$?
check "$([[ $rc -ne 0 ]] && echo true || echo false)" \
    "scripts/tree-hash.sh missing from the pushing tree: refused, not silently approved"
check_contains "$out" "could not hash" \
    "missing tree-hash.sh: message says it could not hash the tree"

# ── GIT-INHERENT RESIDUAL, demonstrated but NOT gated pass/fail: if the
#    hook file itself loses its executable bit, git silently SKIPS it (only
#    an advisory hint on stderr, exit 0). A fresh checkout is safe (git
#    tracks the +x bit, mode 100755 -- `git ls-tree` confirms this repo's
#    scripts/git-hooks/pre-push is 100755), but this is the one way "cannot
#    tell" still approves instead of refusing, and it is outside what a
#    hook SCRIPT can defend against: it is never invoked at all. Recorded
#    as an observation, not a check() call, so this finding cannot silently
#    flip the suite green/red as git's own advisory behavior changes; it is
#    reported in the agent's report as a tracked residual instead.
base="$tmproot/c10"; make_fixture "$base"
chmod -x "$base/main/scripts/git-hooks/pre-push"
out=$(do_push "$base/main" main); rc=$?
echo "  INFO (not a pass/fail gate): non-executable pre-push hook -> exit=$rc (0 means git silently skipped it; this is a known git-level residual, see tests/test_pre_push_hook.sh's comment above this case)"

echo
echo "test_pre_push_hook: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
