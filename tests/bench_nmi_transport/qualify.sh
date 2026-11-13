#!/usr/bin/env bash
# Frozen protocol entry point; box-only. Refs #871.
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
    echo 'transport qualification requires Linux; do not run BPF on macOS' >&2
    exit 2
fi
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$here/qualify.py" "$@"
