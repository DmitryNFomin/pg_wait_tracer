#!/bin/sh
# The parity gate's rescoped acceptance tier, against a PATCHED copy of src/
# where the writer ignores the resolved category (the pre-v6 behaviour, kept
# compiling by reverting only the decision). Must go RED on the real capture.
set -e
W="/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-ac6ce72dc9ecbbff0"
S="/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/red-parity"
rm -rf "$S"; mkdir -p "$S/src" "$S/tests" "$S/b"
cp "$W"/src/*.c "$W"/src/*.h "$W"/src/*.inc "$S/src/" 2>/dev/null || true
cp "$W"/tests/test_io_worker_parity.c "$S/tests/"
perl -pi -e 's/^    bool io_worker = \(cat_flag & PGWT_EVENT_FLAG_IO_WORKER\) != 0;/    bool io_worker = false;   \/* REVERTED for the red run *\//' "$S/src/summary_writer.c"
grep -n "bool io_worker = false" "$S/src/summary_writer.c"
CFLAGS="-g -O2 -Wall -I$S/src -I/opt/homebrew/include -DPGWT_SERVER"
for f in summary_writer summary_reader compute event_reader event_writer cmdline wait_event idle_rule cJSON; do
  cc $CFLAGS -c "$S/src/$f.c" -o "$S/b/$f.o" 2>/dev/null
done
cat > "$S/b/stub.c" <<'EOF'
#include <stdio.h>
int pgwt_proc_open(const char *const argv[], FILE **out);
int pgwt_proc_close(FILE *fp);
int pgwt_proc_open(const char *const argv[], FILE **out) { (void)argv; (void)out; return -1; }
int pgwt_proc_close(FILE *fp) { (void)fp; return -1; }
EOF
cc $CFLAGS -c "$S/b/stub.c" -o "$S/b/stub.o"
cc $CFLAGS -o "$S/b/t" "$S/tests/test_io_worker_parity.c" \
  "$S/b/summary_writer.o" "$S/b/summary_reader.o" "$S/b/compute.o" \
  "$S/b/event_reader.o" "$S/b/event_writer.o" "$S/b/cmdline.o" \
  "$S/b/wait_event.o" "$S/b/idle_rule.o" "$S/b/cJSON.o" "$S/b/stub.o" \
  -L/opt/homebrew/lib -llz4 -lz -lm 2>/dev/null
cd "$S/tests"
PGWT_PARITY_TRACE=/Users/dmitryfomin/work/git/pg_wait_tracer/tests/results/demo_rehearsal/trace "$S/b/t" || true
