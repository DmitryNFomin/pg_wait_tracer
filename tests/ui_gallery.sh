#!/usr/bin/env bash
# ui_gallery.sh — before/after screenshot sheet for UI changes.
#
#   tests/ui_gallery.sh [BASE_REF]      (default: merge-base with master)
#
# Renders every snapshot cell of tests/test_web_ui_snapshots.py (app panes,
# exact/sampled fidelity states, the dev fixture gallery) TWICE on this machine:
#   before = a temporary worktree at BASE_REF
#   after  = the current working tree (uncommitted changes included)
# and writes tests/results/ui_gallery/index.html — side-by-side, diff-ranked.
# Both sides render with the same chromium/fonts, so pixel diffs are meaningful
# here even though local PNGs must never be committed as CI baselines.
set -euo pipefail
cd "$(dirname "$0")/.."

base_ref="${1:-$(git merge-base HEAD master)}"
out="tests/results/ui_gallery"
rm -rf "$out"; mkdir -p "$out"
wt=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-gallery-base.XXXXXX")
trap 'git worktree remove --force "$wt" >/dev/null 2>&1 || true' EXIT
git worktree add --detach "$wt" "$base_ref" >/dev/null

here=$PWD
# The base checkout must honour PGWT_SNAP_DIR; older bases wrote baselines
# in-tree only. Carry the current snapshot driver over when the base lacks it
# (the driver is test tooling; fixtures/UI under test stay the base's own).
if ! grep -q PGWT_SNAP_DIR "$wt/tests/test_web_ui_snapshots.py"; then
    cp tests/test_web_ui_snapshots.py "$wt/tests/test_web_ui_snapshots.py"
fi
echo "rendering before ($(git rev-parse --short "$base_ref")) ..."
( cd "$wt" && PGWT_SNAP_DIR="$here/$out/before" PGWT_UPDATE_SNAPSHOTS=1 PGWT_SNAP_PORT=18860 \
    python3 tests/test_web_ui_snapshots.py ) > "$out/before.log" 2>&1 || { echo "before render failed — $out/before.log"; exit 1; }
echo "rendering after (working tree) ..."
PGWT_SNAP_DIR="$here/$out/after" PGWT_UPDATE_SNAPSHOTS=1 PGWT_SNAP_PORT=18880 \
    python3 tests/test_web_ui_snapshots.py > "$out/after.log" 2>&1 || { echo "after render failed — $out/after.log"; exit 1; }

python3 tests/ui_gallery_report.py "$out/before" "$out/after" "$out/index.html" \
    --base "$(git rev-parse --short "$base_ref")" --head "$(git rev-parse --short HEAD)$(git diff --quiet || echo '+dirty')"
echo "gallery: $out/index.html"
