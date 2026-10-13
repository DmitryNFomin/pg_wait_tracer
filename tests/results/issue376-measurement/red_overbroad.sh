#!/bin/sh
# The OVER-BROAD mutant the reviewer found undetectable: the writer excludes
# every non-zero category, deleting checkpointer/walwriter/bgwriter/autovacuum
# time from all five accumulators. Both io_worker tests must go RED.
set -e
W="/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-ac6ce72dc9ecbbff0"
S="/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/red-overbroad"
rm -rf "$S"
mkdir -p "$S/src" "$S/tests" "$S/b"
cp "$W"/src/*.c "$S/src/"
cp "$W"/src/*.h "$S/src/"
cp "$W"/src/*.inc "$S/src/"
cp "$W"/tests/test_io_worker_parity.c "$S/tests/"
cp "$W"/tests/test_io_worker_summary.c "$S/tests/"
perl -pi -e 's/^    bool io_worker = \(cat_flag & PGWT_EVENT_FLAG_IO_WORKER\) != 0;/    bool io_worker = cat_flag != 0;   \/* OVER-BROAD MUTANT *\//' "$S/src/summary_writer.c"
grep -n "OVER-BROAD MUTANT" "$S/src/summary_writer.c"
CFLAGS="-g -O2 -Wall -I$S/src -I/opt/homebrew/include -DPGWT_SERVER"
for f in summary_writer summary_reader compute event_reader event_writer cmdline wait_event idle_rule cJSON; do
  cc $CFLAGS -c "$S/src/$f.c" -o "$S/b/$f.o" 2>/dev/null
done
cat > "$S/b/stub.c" <<'EOS'
#include <stdio.h>
int pgwt_proc_open(const char *const argv[], FILE **out);
int pgwt_proc_close(FILE *fp);
int pgwt_proc_open(const char *const argv[], FILE **out) { (void)argv; (void)out; return -1; }
int pgwt_proc_close(FILE *fp) { (void)fp; return -1; }
EOS
cc $CFLAGS -c "$S/b/stub.c" -o "$S/b/stub.o"
OBJ="$S/b/summary_writer.o $S/b/summary_reader.o $S/b/compute.o $S/b/event_reader.o $S/b/event_writer.o $S/b/cmdline.o $S/b/wait_event.o $S/b/idle_rule.o $S/b/cJSON.o $S/b/stub.o"
cc $CFLAGS -o "$S/b/tp" "$S/tests/test_io_worker_parity.c" $OBJ -L/opt/homebrew/lib -llz4 -lz -lm 2>/dev/null
cc $CFLAGS -o "$S/b/ts" "$S/tests/test_io_worker_summary.c" $OBJ -L/opt/homebrew/lib -llz4 -lz -lm 2>/dev/null
cd "$S/tests"
echo "=== test_io_worker_summary under the over-broad mutant ==="
"$S/b/ts" 2>&1 | grep -E "FAIL|checks," || true
echo "=== test_io_worker_parity (unit tier) under the over-broad mutant ==="
"$S/b/tp" 2>&1 | grep -E "FAIL|checks," || true
