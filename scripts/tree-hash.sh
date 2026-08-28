#!/usr/bin/env bash
# Hash of the working tree content (tracked + untracked, ignoring .gitignore'd
# files). Same input -> same hash; used by check.sh / push-guard.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
cp "$(git rev-parse --git-path index)" "$tmp" 2>/dev/null || true
GIT_INDEX_FILE="$tmp" git add -A . >/dev/null 2>&1
GIT_INDEX_FILE="$tmp" git write-tree
