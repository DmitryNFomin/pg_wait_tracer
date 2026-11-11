#!/usr/bin/env bash
# Called from the synced checkout by box-check.sh, under its box-wide flock.
# A marker certifies a COMPLETE build, never a partially failed rebuild.
set -euo pipefail
tree_hash="${1:?usage: box-build.sh <tree-hash> <commit>}"
commit="${2:?usage: box-build.sh <tree-hash> <commit>}"
marker=.pgwt-box-built-from
built_from=$(cat "$marker" 2>/dev/null || true)
rm -f "$marker"
if [[ "$built_from" != "$tree_hash" ]]; then
    echo "box-check: build cache reset (built-from=${built_from:-missing} tree-hash=$tree_hash)"
    make clean-box
else
    echo "box-check: build cache reuse (built-from=$built_from tree-hash=$tree_hash)"
fi
make -j"$(nproc)"
make -C tests clean
make build-tests
make pgwt-client
printf '%s\n' "$tree_hash" > "$marker"
built_from=$(cat "$marker")
echo "box-check: commit=$commit tree-hash=$tree_hash built-from=$built_from"
