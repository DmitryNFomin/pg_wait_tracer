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

# Resolve the interpreter ONCE, from this checkout, and use it for BOTH
# renders. The base worktree below lives under $TMPDIR, outside the repo, so
# it does not see the repo-root .python-version pyenv pin; if we let it fall
# back to a bare `python3` lookup it can silently pick a different (pyenv
# global) interpreter that lacks Playwright, render zero PNGs, and still let
# the gallery report a clean-looking "0 changed" (see issue #118).
py=$(python3 -c 'import sys; print(sys.executable)') || {
    echo "FAIL: could not resolve a python3 interpreter"; exit 1; }
if ! "$py" -c 'import playwright' 2>/dev/null; then
    echo "FAIL: $py has no playwright module — pin it via .python-version/PYENV_VERSION" \
         "or install: $py -m pip install --user playwright==1.60.0 && $py -m playwright install chromium"
    exit 1
fi
echo "python: $py"

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
gallery_port_base=$("$py" tests/free_ports.py "$gallery_port_span") \
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

# render LABEL DIR SNAPDIR PORT LOG — run one side and fail loudly (instead of
# letting a SKIP or a zero-PNG render pass through as a quiet "0 changed").
render() {
    local label="$1" dir="$2" snapdir="$3" port="$4" log="$5"
    ( cd "$dir" && PGWT_SNAP_DIR="$snapdir" PGWT_UPDATE_SNAPSHOTS=1 PGWT_SNAP_PORT="$port" \
        "$py" tests/test_web_ui_snapshots.py ) > "$log" 2>&1 \
        || { echo "$label render failed — $log"; exit 1; }
    if grep -q '^SKIP:' "$log"; then
        echo "FAIL: $label render was skipped by $py — $(grep -m1 '^SKIP:' "$log")" \
             "(see $log; pin the interpreter via .python-version/PYENV_VERSION)"
        exit 1
    fi
    local n
    n=$(find "$snapdir" -name '*.png' 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$n" -eq 0 ]]; then
        echo "FAIL: $label render produced 0 PNGs using $py — see $log"
        exit 1
    fi
    echo "$label: $n PNGs ($py)"
}

echo "rendering before ($(git rev-parse --short "$base_ref")) ..."
render before "$wt" "$here/$out/before" "$before_snap_port" "$out/before.log"
echo "rendering after (working tree) ..."
render after "$here" "$here/$out/after" "$after_snap_port" "$out/after.log"

"$py" tests/ui_gallery_report.py "$out/before" "$out/after" "$out/index.html" \
    --base "$(git rev-parse --short "$base_ref")" --head "$(git rev-parse --short HEAD)$(git diff --quiet || echo '+dirty')"
echo "gallery: $out/index.html"
