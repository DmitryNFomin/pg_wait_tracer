#!/usr/bin/env bash
# check-snapshot-history.sh — the guard that makes tests/web_snapshots/VERSION
# RULE 3 self-enforcing: refuse a change that touches a
# tests/web_snapshots/*.png baseline without also adding or modifying a
# tests/web_snapshots/history/*.md entry. Pure git, no Linux/BPF/PG
# dependency — wired into `make check` (scripts/check.sh) so it runs on the
# Mac.
#
# "Changed" = the diff between this branch's merge-base with origin/master
# (falling back to a local `master` branch, or HEAD itself if neither
# resolves — e.g. a scratch repo with no such branch) and the CURRENT
# WORKING TREE. `git diff <base>` (one ref, no --cached) folds together
# already-committed changes on the branch AND uncommitted staged/unstaged
# changes against that base, so this catches both:
#   - a branch that already committed a baseline regen with no history entry
#   - a developer who just `git add`ed a regenerated PNG locally
#
# Usage: scripts/check-snapshot-history.sh [repo-dir]
#   repo-dir defaults to this script's own repository; a test harness passes
#   a scratch git repo to exercise the logic in isolation (see
#   tests/test_check_snapshot_history.sh).
set -euo pipefail

repo="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$repo"

base=$(git merge-base HEAD origin/master 2>/dev/null) \
    || base=$(git merge-base HEAD master 2>/dev/null) \
    || base=$(git rev-parse HEAD)

changed=$(git diff --name-only "$base" -- tests/web_snapshots)

png_changed=$(printf '%s\n' "$changed" | grep -E '\.png$' || true)
history_changed=$(printf '%s\n' "$changed" | grep -E '^tests/web_snapshots/history/.+\.md$' || true)

if [[ -n "$png_changed" && -z "$history_changed" ]]; then
    echo "FAIL: baseline PNG(s) changed vs $base with no tests/web_snapshots/history/ entry:" >&2
    printf '%s\n' "$png_changed" | sed 's/^/  /' >&2
    echo "Add tests/web_snapshots/history/<YYYY-MM-DD>-<issue>-<slug>.md recording" \
         "the regeneration — see tests/web_snapshots/VERSION RULE 3." >&2
    exit 1
fi

echo "OK: snapshot-history guard (base $base)"
exit 0
