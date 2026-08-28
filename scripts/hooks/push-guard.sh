#!/usr/bin/env bash
# push-guard.sh — Claude Code PreToolUse hook (Bash). Refuses `git push` unless
# `make check` passed on the CURRENT tree (stamp matches the tree hash).
# Wired in .claude/settings.json. Bypass: PGWT_SKIP_PUSH_GUARD=1 (humans only).
set -uo pipefail
input=$(cat)
cmd=$(printf '%s' "$input" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("command",""))' 2>/dev/null)
case "$cmd" in *"git push"*) ;; *) exit 0 ;; esac
[[ "${PGWT_SKIP_PUSH_GUARD:-}" == "1" ]] && exit 0
root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
cd "$root"
if [[ ! -f .pgwt-check.stamp ]]; then
    echo "push blocked: no .pgwt-check.stamp — run 'make check' (or 'make check-fast') first." >&2
    exit 2
fi
now=$(scripts/tree-hash.sh)
if [[ "$now" != "$(cat .pgwt-check.stamp)" ]]; then
    echo "push blocked: tree changed since the last passing 'make check' (stamp $(cat .pgwt-check.stamp), tree $now). Re-run 'make check'." >&2
    exit 2
fi
exit 0
