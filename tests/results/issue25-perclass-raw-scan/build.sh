#!/usr/bin/env bash
# Build the raw per-class wait-CPU scanner, its fixture generator, and the
# replay server used only for the INDEPENDENT record total (`--dump`).
# Offline build: demos/build-server-local.sh does not work on macOS, so the
# capture daemon's process helpers are stubbed with ENOSYS, exactly as
# tests/results/perclass-wait-cpu/build_server.sh does.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:-$ROOT/tests/results/issue25-perclass-raw-scan/bin}"
mkdir -p "$OUT"
EXTRA=()
if [[ "$(uname -s)" == Darwin ]]; then
    PREFIX="$(brew --prefix)"
    EXTRA=(-I"$PREFIX/include" -L"$PREFIX/lib")
fi
CFLAGS=(-O2 -g -Wall -Wextra -Wno-unused-parameter -DPGWT_SERVER -I"$ROOT/src")

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

cc "${CFLAGS[@]}" "${EXTRA[@]}" \
   "$ROOT/tests/wait_cpu_scan.c" \
   "$ROOT/src/event_reader.c" "$ROOT/src/event_writer.c" \
   "$ROOT/src/wait_event.c" "$ROOT/src/idle_rule.c" "$ROOT/src/cJSON.c" \
   "$TMP_BUILD/proc_stub.c" \
   -lz -llz4 -lm -o "$OUT/wait_cpu_scan"

cc "${CFLAGS[@]}" "${EXTRA[@]}" \
   "$ROOT/tests/wait_cpu_fixture.c" \
   "$ROOT/src/event_writer.c" "$ROOT/src/wait_event.c" \
   "$ROOT/src/idle_rule.c" "$ROOT/src/cJSON.c" "$TMP_BUILD/proc_stub.c" \
   -lz -llz4 -lm -o "$OUT/wait_cpu_fixture"

# The replay server: only used to produce the independent event census the
# scanner is checked against.
SOURCES=(server compute block_agg event_reader event_writer summary_writer
         summary_reader wait_event idle_rule cmdline backend_meta cJSON)
FILES=()
for source in "${SOURCES[@]}"; do FILES+=("$ROOT/src/$source.c"); done
cc -O2 -g -Wall -Wextra -Wno-unused-parameter -DPGWT_SERVER \
   "-DPGWT_BUILD_VERSION=\"$(git -C "$ROOT" rev-parse HEAD)\"" \
   -include "$ROOT/demos/replay-compat.h" -I"$ROOT/src" \
   "${EXTRA[@]}" "${FILES[@]}" "$TMP_BUILD/proc_stub.c" \
   -lz -llz4 -lm -o "$OUT/pgwt-server"
echo "built: $OUT/wait_cpu_scan $OUT/wait_cpu_fixture $OUT/pgwt-server"
