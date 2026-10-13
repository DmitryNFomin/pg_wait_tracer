#!/bin/sh
set -e
W="/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-ac6ce72dc9ecbbff0"
B="/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/b"
mkdir -p "$B"
CFLAGS="-g -O2 -Wall -I$W/src -I/opt/homebrew/include -DPGWT_SERVER"
for f in summary_writer summary_reader compute event_reader event_writer wait_event idle_rule cJSON; do
  cc $CFLAGS -c "$W/src/$f.c" -o "$B/$f.o"
done
cat > "$B/stub.c" <<'EOF'
#include <stdio.h>
int pgwt_proc_open(const char *const argv[], FILE **out);
int pgwt_proc_close(FILE *fp);
int pgwt_proc_open(const char *const argv[], FILE **out) { (void)argv; (void)out; return -1; }
int pgwt_proc_close(FILE *fp) { (void)fp; return -1; }
EOF
cc $CFLAGS -c "$B/stub.c" -o "$B/stub.o"
cc $CFLAGS -o "$B/test_io_worker_summary" "$W/tests/test_io_worker_summary.c" \
  "$B/summary_writer.o" "$B/summary_reader.o" "$B/compute.o" \
  "$B/event_writer.o" "$B/wait_event.o" "$B/idle_rule.o" "$B/cJSON.o" "$B/stub.o" \
  -L/opt/homebrew/lib -llz4 -lz -lm
echo built
