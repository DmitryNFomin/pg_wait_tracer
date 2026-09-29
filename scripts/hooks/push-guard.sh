#!/usr/bin/env bash
# Claude Code PreToolUse hook. Require a passing check stamp for the tree
# selected by the guarded git push command.
set -uo pipefail
input=$(cat)
[[ "${PGWT_SKIP_PUSH_GUARD:-}" == "1" ]] && exit 0
target=$(printf '%s' "$input" | python3 "$(dirname "$0")/push-guard-target.py")
result=$?
case "$result" in
    0) exit 0 ;; # No git push in the command.
    1) ;;        # One push; target is the worktree holding its branch.
    *) echo "push blocked: cannot determine the repository being pushed ($target)." >&2; exit 2 ;;
esac
root=$(git -C "$target" rev-parse --show-toplevel 2>/dev/null) || {
    echo "push blocked: cannot determine the repository being pushed from '$target'." >&2
    exit 2
}
cd "$root" || exit 2
if [[ ! -f .pgwt-check.stamp ]]; then
    echo "push blocked: no .pgwt-check.stamp — run 'make check' (or 'make check-fast') first." >&2
    exit 2
fi
now=$(scripts/tree-hash.sh) || {
    echo "push blocked: could not hash the target tree in '$root'." >&2
    exit 2
}
stamp=$(cat .pgwt-check.stamp) || exit 2
if [[ "$now" != "$stamp" ]]; then
    echo "push blocked: tree changed since the last passing 'make check' (stamp $stamp, tree $now). Re-run 'make check'." >&2
    exit 2
fi
exit 0
