#!/bin/sh
# Build test_block_agg out of tree (macOS: spawn.c uses pipe2, Linux-only).
# Mirrors the recipe in tests/Makefile exactly, minus the prebuilt objects.
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
WD="${WD:-/tmp/pgwt_bagg_build_$$}"
mkdir -p "$WD"
cat > "$WD/spawn_stub.c" <<'EOF'
#include "spawn.h"
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{ (void)p; (void)argv; return -1; }
int pgwt_proc_close(struct pgwt_proc *p) { (void)p; return -1; }
EOF
INC=""
for d in /opt/homebrew/include /usr/local/include; do [ -d "$d" ] && INC="$INC -I$d"; done
LIB=""
for d in /opt/homebrew/lib /usr/local/lib; do [ -d "$d" ] && LIB="$LIB -L$d"; done
# shellcheck disable=SC2086
${CC:-cc} -g -O1 -Wall -Wextra -I"$ROOT/src" $INC $LIB -DPGWT_SERVER \
    -o "$WD/test_block_agg" \
    "$ROOT/tests/test_block_agg.c" \
    "$ROOT/src/block_agg.c" "$ROOT/src/compute.c" \
    "$ROOT/src/event_reader.c" "$ROOT/src/event_writer.c" \
    "$ROOT/src/summary_writer.c" \
    "$ROOT/src/wait_event.c" "$ROOT/src/idle_rule.c" \
    "$ROOT/src/cJSON.c" "$WD/spawn_stub.c" \
    -llz4 -lz -lm
echo "$WD/test_block_agg"
