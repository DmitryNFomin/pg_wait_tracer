#!/usr/bin/env bash
# classify_changed_files.sh — the change classifier extracted out of
# .github/workflows/ci.yml's `changed_files` job (issue #167).
#
# Decides how much of the self-hosted gate-box capture matrix a CI run needs:
#   src_changed=true   -> src/ (including src/bpf/) changed; the FULL matrix
#                         runs, since src/ is the only thing that can change
#                         capture, accounting, discovery or the daemon.
#   docs_only=true      -> every changed file is under docs/ or ends in .md;
#                         no capture cell is needed at all.
#
# Usage:
#   classify_changed_files.sh <event_name>
#   (changed file paths, one per line, on stdin — empty stdin means "no
#   files known for this trigger", e.g. a push whose `before` SHA is
#   unavailable)
#
# merge_group (issue for merge-queue readiness): a queue entry can bundle
# SEVERAL PRs into one temporary merge commit. GitHub does expose
# merge_group.base_sha/head_sha, but base_sha is the branch tip at the
# moment the queue *entry* was formed, not a stable per-entry merge-base --
# it can be superseded as other entries join or leave ahead of this one, so
# a diff against it is not reliably "exactly what this batch changes". A
# merge-queue entry is also the LAST gate before code lands on master, so
# the wrong place to be clever: this classifier always calls it a full
# change for merge_group, ignoring any files it might otherwise be given.
classify_changed_files() {
    local event_name="$1"
    local files="$2"

    if [[ "$event_name" == "merge_group" ]]; then
        echo "src_changed=true"
        echo "docs_only=false"
        return 0
    fi

    if [[ -z "$files" ]]; then
        echo "src_changed=true"
        echo "docs_only=false"
        return 0
    fi

    local src_changed docs_only
    if grep -qE '^src/' <<<"$files"; then
        src_changed=true
    else
        src_changed=false
    fi

    # docs-only: every changed file is under docs/ or ends in .md
    if grep -qvE '^docs/|\.md$' <<<"$files"; then
        docs_only=false
    else
        docs_only=true
    fi

    echo "src_changed=$src_changed"
    echo "docs_only=$docs_only"
}

# Only run when executed directly (not when sourced by the test harness).
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    set -euo pipefail
    event_name="${1:?usage: classify_changed_files.sh <event_name> (files on stdin)}"
    files="$(cat)"
    classify_changed_files "$event_name" "$files"
fi
