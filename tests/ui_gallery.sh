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

# ── Port allocation (issue #135) ────────────────────────────────────────────
# This can be invoked standalone (and concurrently with another agent's
# `make check` or `make ui-gallery` on the same Mac), so it draws its OWN
# free base rather than relying on a caller's env — no lock, see
# tests/free_ports.py. before/after each get an independent HTTP/WS pair plus
# the +10 sampled-mock pair test_web_ui_snapshots.py needs (PORT MAP: before
# = base+0 [sampled +10], after = base+20 [sampled +30]).
gallery_port_span=40
gallery_port_base=$(python3 tests/free_ports.py "$gallery_port_span") \
    || { echo "free_ports: could not allocate $gallery_port_span free ports"; exit 1; }
before_snap_port=$gallery_port_base
after_snap_port=$((gallery_port_base + 20))
echo "port base: $gallery_port_base (span $gallery_port_span — before=+0 after=+20)"

here=$PWD
# The base checkout must honour PGWT_SNAP_DIR; older bases wrote baselines
# in-tree only. Carry the current snapshot driver over when the base lacks it
# (the driver is test tooling; fixtures/UI under test stay the base's own).
if ! grep -q PGWT_SNAP_DIR "$wt/tests/test_web_ui_snapshots.py"; then
    cp tests/test_web_ui_snapshots.py "$wt/tests/test_web_ui_snapshots.py"
fi
echo "rendering before ($(git rev-parse --short "$base_ref")) ..."
( cd "$wt" && PGWT_SNAP_DIR="$here/$out/before" PGWT_UPDATE_SNAPSHOTS=1 PGWT_SNAP_PORT=$before_snap_port \
    python3 tests/test_web_ui_snapshots.py ) > "$out/before.log" 2>&1 || { echo "before render failed — $out/before.log"; exit 1; }
echo "rendering after (working tree) ..."
PGWT_SNAP_DIR="$here/$out/after" PGWT_UPDATE_SNAPSHOTS=1 PGWT_SNAP_PORT=$after_snap_port \
    python3 tests/test_web_ui_snapshots.py > "$out/after.log" 2>&1 || { echo "after render failed — $out/after.log"; exit 1; }

python3 tests/ui_gallery_report.py "$out/before" "$out/after" "$out/index.html" \
    --base "$(git rev-parse --short "$base_ref")" --head "$(git rev-parse --short HEAD)$(git diff --quiet || echo '+dirty')"
echo "gallery: $out/index.html"
