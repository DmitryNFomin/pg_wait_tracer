#!/usr/bin/env bash
# Regression for stale cross-translation-unit struct layouts. A daemon.h
# change must invalidate discovery.o, which writes struct pgwt_daemon fields
# later read by daemon.o/sampler.o. Missing this edge once silently disabled
# the sampled command-active gate while the final link still succeeded.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HEADER="$ROOT/src/daemon.h"
DEPFILE="$ROOT/build/discovery.d"
STAMP=$(mktemp)

cleanup() {
    touch -r "$STAMP" "$HEADER"
    rm -f "$STAMP"
}
trap cleanup EXIT
touch -r "$HEADER" "$STAMP"

make -s -C "$ROOT" build/discovery.o

if [[ ! -f "$DEPFILE" ]] || ! grep -q 'src/daemon.h' "$DEPFILE"; then
    echo "FAIL: build/discovery.d does not track src/daemon.h"
    exit 1
fi

touch "$HEADER"
if make -q -C "$ROOT" build/discovery.o; then
    echo "FAIL: touching daemon.h left discovery.o incorrectly up to date"
    exit 1
fi

make -s -C "$ROOT" build/discovery.o
if ! make -q -C "$ROOT" build/discovery.o; then
    echo "FAIL: discovery.o is still stale after its header-triggered rebuild"
    exit 1
fi

echo "PASS: daemon.h invalidates and rebuilds discovery.o"
