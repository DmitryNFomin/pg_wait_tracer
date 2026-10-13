#!/usr/bin/env bash
# Offline measurement build: no Linux proc implementation or capture daemon.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:-$ROOT/pgwt-server}"
TMP_BUILD="$(mktemp -d)"
trap 'rm -rf "$TMP_BUILD"' EXIT
cat > "$TMP_BUILD/proc_stub.c" <<'EOF'
#include <errno.h>
#include "spawn.h"
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[]) {
    (void)p; (void)argv; errno = ENOSYS; return -1;
}
int pgwt_proc_close(struct pgwt_proc *p) {
    (void)p; errno = ENOSYS; return -1;
}
EOF
EXTRA=()
if [[ "$(uname -s)" == Darwin ]]; then
    PREFIX="$(brew --prefix)"
    EXTRA=(-I"$PREFIX/include" -L"$PREFIX/lib")
fi
SOURCES=(server compute block_agg event_reader event_writer summary_writer
         summary_reader wait_event idle_rule cmdline backend_meta cJSON)
FILES=()
for source in "${SOURCES[@]}"; do FILES+=("$ROOT/src/$source.c"); done
cc -O2 -g -Wall -Wextra -Wno-unused-parameter -DPGWT_SERVER \
   "-DPGWT_BUILD_VERSION=\"$(git -C "$ROOT" rev-parse HEAD)\"" \
   -include "$ROOT/demos/replay-compat.h" -I"$ROOT/src" \
   "${EXTRA[@]}" "${FILES[@]}" "$TMP_BUILD/proc_stub.c" \
   -lz -llz4 -lm -o "$OUT"
