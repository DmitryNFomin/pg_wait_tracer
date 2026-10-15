#!/bin/sh
# Build the server-tier objects tests/Makefile expects, on this Mac.
# The top-level Makefile's server_%.o rule needs the BPF skeleton path set up;
# this does the same compiles directly (-DPGWT_SERVER, no BPF) plus a stub for
# src/spawn.c, which uses Linux pipe2(2). Linux boxes build these the normal
# way via `make`; this is only so the summary/compute C units can be run here.
set -e
cd "$(dirname "$0")/../../.."
mkdir -p build
CC=${CC:-cc}
CFLAGS="-g -O2 -Wall -Isrc -Iinclude -I/opt/homebrew/include -DPGWT_SERVER"
for f in summary_writer summary_reader event_writer event_reader cmdline; do
    $CC $CFLAGS -c "src/$f.c" -o "build/server_$f.o"
done
for f in wait_event idle_rule cJSON; do
    $CC $CFLAGS -c "src/$f.c" -o "build/$f.o"
done
$CC $CFLAGS -c tests/results/issue408-measurement/spawn_stub.c -o build/spawn.o
echo "built: $(ls build | tr '\n' ' ')"
