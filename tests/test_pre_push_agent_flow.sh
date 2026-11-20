#!/usr/bin/env bash
# The pre-push hook's agent-branch rules (#767).
#
# Each case runs the real hook as a script inside a throwaway git repo, with a
# fake `gh` and a fake `origin` on PATH/disk, and asserts the exit status.
# The hook must FAIL OPEN: only a definite "on origin and no PR" blocks.
set -uo pipefail

HOOK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/git-hooks/pre-push
pass=0; fail=0

note() { printf '  %s\n' "$*"; }
check() { # check <name> <expected-rc> <actual-rc>
    if [[ "$2" == "$3" ]]; then
        pass=$((pass+1)); printf 'PASS: %s (rc=%s)\n' "$1" "$3"
    else
        fail=$((fail+1)); printf 'FAIL: %s -- expected rc=%s, got rc=%s\n' "$1" "$2" "$3"
    fi
}

# Build a sandbox repo whose tree-hash matches its stamp, so the hook's
# pre-existing stamp check passes and we isolate the new logic.
make_repo() { # make_repo <branch>
    local d; d=$(mktemp -d)
    git -C "$d" init -q
    git -C "$d" config user.email t@t; git -C "$d" config user.name t
    mkdir -p "$d/scripts"
    printf '#!/bin/sh\necho SAME\n' > "$d/scripts/tree-hash.sh"
    chmod +x "$d/scripts/tree-hash.sh"
    printf 'SAME\n' > "$d/.pgwt-check.stamp"
    git -C "$d" add -A >/dev/null 2>&1
    git -C "$d" commit -qm init >/dev/null 2>&1
    git -C "$d" checkout -q -b "$1" 2>/dev/null
    printf '%s' "$d"
}

# A fake origin that either has the branch or does not.
add_origin() { # add_origin <repo> <branch-exists:yes|no>
    local d="$1" o; o=$(mktemp -d)
    git -C "$o" init -q --bare
    git -C "$d" remote add origin "$o"
    if [[ "$2" == yes ]]; then
        git -C "$d" push -q origin HEAD:"refs/heads/$(git -C "$d" rev-parse --abbrev-ref HEAD)" 2>/dev/null
    fi
}

# A fake gh whose behaviour we choose.
fake_gh() { # fake_gh <dir> <mode:nopr|haspr|broken>
    local b="$1/bin"; mkdir -p "$b"
    case "$2" in
        nopr)   printf '#!/bin/sh\necho "no pull requests found for branch" >&2\nexit 1\n' > "$b/gh" ;;
        haspr)  printf '#!/bin/sh\necho "{\\"number\\":7}"\n' > "$b/gh" ;;
        broken) printf '#!/bin/sh\necho "HTTP 502 Bad Gateway" >&2\nexit 1\n' > "$b/gh" ;;
    esac
    chmod +x "$b/gh"
    printf '%s' "$b"
}

run_hook() { # run_hook <repo> <extra-PATH-or-empty>
    local d="$1" extra="${2:-}"
    ( cd "$d" && PATH="${extra:+$extra:}$PATH" bash "$HOOK" </dev/null >/dev/null 2>&1 )
    printf '%s' "$?"
}

echo "=== pre-push agent-flow rules ==="

# 1. conforming NEW branch (not on origin) -> allowed
d=$(make_repo agent/123-good); add_origin "$d" no
check "new agent/<issue>-slug branch is allowed" 0 "$(run_hook "$d")"
rm -rf "$d"

# 2. NEW branch without an issue number -> blocked
d=$(make_repo agent/no-number-here); add_origin "$d" no
check "new agent branch without an issue number is blocked" 2 "$(run_hook "$d")"
rm -rf "$d"

# 3. branch ON origin with no PR -> blocked
d=$(make_repo agent/123-good); add_origin "$d" yes; g=$(fake_gh "$d" nopr)
check "branch on origin with no PR is blocked" 2 "$(run_hook "$d" "$g")"
rm -rf "$d"

# 4. branch ON origin WITH a PR -> allowed
d=$(make_repo agent/123-good); add_origin "$d" yes; g=$(fake_gh "$d" haspr)
check "branch on origin with a PR is allowed" 0 "$(run_hook "$d" "$g")"
rm -rf "$d"

# 5. gh failing for any other reason -> FAIL OPEN
d=$(make_repo agent/123-good); add_origin "$d" yes; g=$(fake_gh "$d" broken)
check "gh returning 502 fails open (push allowed)" 0 "$(run_hook "$d" "$g")"
rm -rf "$d"

# 6. non-agent branch is never touched
d=$(make_repo master); add_origin "$d" no
check "master is never blocked by these rules" 0 "$(run_hook "$d")"
rm -rf "$d"

# 7. grandfathering: a badly-named branch ALREADY on origin is allowed through
#    the naming rule (it is only the PR rule that can stop it).
d=$(make_repo agent/legacy-name); add_origin "$d" yes; g=$(fake_gh "$d" haspr)
check "pre-existing badly-named branch is grandfathered" 0 "$(run_hook "$d" "$g")"
rm -rf "$d"

echo
echo "pre_push_agent_flow: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
