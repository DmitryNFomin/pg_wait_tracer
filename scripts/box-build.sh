#!/usr/bin/env bash
# Called from the synced checkout by box-check.sh, under its box-wide flock.
# The marker records a COMPLETE build; it never authorises output reuse.
set -euo pipefail
tree_hash="${1:?usage: box-build.sh <tree-hash> <commit>}"
commit="${2:?usage: box-build.sh <tree-hash> <commit>}"
marker=.pgwt-box-built-from
built_from=$(cat "$marker" 2>/dev/null || true)
rm -f "$marker"
if ! synced_hash=$(python3 scripts/box-source-hash.py .pgwt-box-source-manifest); then
    echo "box-check: source verification tree-hash=$tree_hash synced-tree-hash=unavailable; refusing build" >&2
    exit 1
fi
echo "box-check: source verification tree-hash=$tree_hash synced-tree-hash=$synced_hash"
[[ "$synced_hash" == "$tree_hash" ]] || { echo 'box-check: source hash mismatch; refusing build' >&2; exit 1; }
echo "box-check: build cache reset (built-from=${built_from:-missing} tree-hash=$tree_hash; unconditional)"
make clean-box
make -j"$(nproc)"
make -C tests clean
make build-tests
make pgwt-client
printf '%s\n' "$synced_hash" > "$marker"
built_from=$(cat "$marker")
echo "box-check: commit=$commit tree-hash=$tree_hash built-from=$built_from"
