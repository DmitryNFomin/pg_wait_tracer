#!/usr/bin/env bash
# check-snapshot-history.sh — the guard that makes tests/web_snapshots/VERSION
# RULE 3 self-enforcing: refuse a change that touches a
# tests/web_snapshots/*.png baseline without also adding or modifying a
# tests/web_snapshots/history/*.md entry IN THE SAME CHANGESET. Pure git, no
# Linux/BPF/PG dependency — wired into `make check` (scripts/check.sh) so it
# runs on the Mac.
#
# "Changeset" = one of:
#   - each commit strictly between this branch's base and HEAD that touches
#     tests/web_snapshots (walked individually, oldest first)
#   - the CURRENT WORKING TREE against HEAD (folds together staged and
#     unstaged changes), if it differs
# Checking PER CHANGESET, not the base..HEAD range as one bucket, matters: a
# two-commit branch where commit 1 regenerates table_events.png WITH a
# history entry and commit 2 silently regenerates table_queries.png with
# none must fail on commit 2's table_queries.png specifically — a range-wide
# "some history file changed somewhere" check would wrongly pass because
# commit 1's entry satisfies it.
#
# Base resolution is FAIL-CLOSED: if neither origin/master nor a local
# master branch resolves a merge-base, this refuses with a "no comparable
# base" error rather than quietly comparing HEAD to itself (which would make
# every already-committed regeneration invisible — exactly the shape a
# shallow/detached CI checkout or an unusual clone can take). A guard that
# cannot see a base must refuse, not approve.
#
# Usage: scripts/check-snapshot-history.sh [repo-dir]
#   repo-dir defaults to this script's own repository; a test harness passes
#   a scratch git repo to exercise the logic in isolation (see
#   tests/test_check_snapshot_history.sh).
set -euo pipefail

repo="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$repo"

resolve_base() {
    local ref
    for ref in origin/master master; do
        if git rev-parse --verify -q "$ref" >/dev/null; then
            if git merge-base HEAD "$ref" 2>/dev/null; then
                return 0
            fi
        fi
    done
    return 1
}

base=$(resolve_base) || {
    echo "FAIL: snapshot-history guard: no comparable base ref (origin/master or" >&2
    echo "master) resolvable from HEAD ($(git rev-parse HEAD 2>/dev/null || echo '?')) —" >&2
    echo "refusing to approve blind. Fetch origin/master (or ensure a local master" >&2
    echo "branch exists) before running this guard." >&2
    exit 1
}

fail=0

# One changeset's files -> FAIL (naming the unaccounted PNGs) if any *.png
# changed with no tests/web_snapshots/history/*.md change alongside it.
check_changeset() {
    local label="$1" files="$2"
    local pngs histories
    pngs=$(printf '%s\n' "$files" | grep -E '\.png$' || true)
    histories=$(printf '%s\n' "$files" | grep -E '^tests/web_snapshots/history/.+\.md$' || true)
    if [[ -n "$pngs" && -z "$histories" ]]; then
        echo "FAIL: $label changed baseline PNG(s) with no tests/web_snapshots/history/ entry in the same changeset:" >&2
        printf '%s\n' "$pngs" | sed 's/^/    /' >&2
        return 1
    fi
    return 0
}

# Each commit in (base, HEAD] that touches tests/web_snapshots, oldest first.
commits=$(git rev-list --reverse "$base..HEAD" -- tests/web_snapshots 2>/dev/null || true)
if [[ -n "$commits" ]]; then
    while IFS= read -r c; do
        [[ -z "$c" ]] && continue
        files=$(git diff-tree --no-commit-id --name-only -r "$c" -- tests/web_snapshots)
        [[ -z "$files" ]] && continue
        subject=$(git log -1 --format=%s "$c")
        check_changeset "commit $c ($subject)" "$files" || fail=1
    done <<< "$commits"
fi

# Uncommitted staged/unstaged changes against HEAD, if any.
wt_files=$(git diff --name-only HEAD -- tests/web_snapshots)
if [[ -n "$wt_files" ]]; then
    check_changeset "uncommitted working-tree changes" "$wt_files" || fail=1
fi

if [[ $fail -ne 0 ]]; then
    echo "Add tests/web_snapshots/history/<YYYY-MM-DD>-<issue>-<slug>.md recording" \
         "each regeneration, in the same commit (or staged alongside it) as the" \
         "PNG — see tests/web_snapshots/VERSION RULE 3." >&2
    exit 1
fi

echo "OK: snapshot-history guard (base $base)"
exit 0
