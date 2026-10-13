#!/usr/bin/env bash
# Hash every file in a .git-free, exported source tree ON the receiving box.
# Invoked through SSH stdin from the launcher's copy, not the transferred copy.
# A temporary Git repository supplies identical blob/tree/mode semantics to
# tree-hash.sh; force-add includes tracked files that match .gitignore patterns.
set -euo pipefail
cd "$1"
hash_repo=$(mktemp -d)
trap 'rm -rf "$hash_repo"' EXIT
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
git init -q --bare --object-format=sha1 "$hash_repo"
git --git-dir="$hash_repo" --work-tree="$PWD" \
    -c core.autocrlf=false -c core.filemode=true -c core.attributesfile=/dev/null \
    add -A -f -- .
git --git-dir="$hash_repo" write-tree
