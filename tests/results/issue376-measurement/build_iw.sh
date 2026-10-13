#!/bin/sh
set -e
W="/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-ac6ce72dc9ecbbff0"
B="/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/b"
mkdir -p "$B"
CFLAGS="-g -O2 -Wall -I$W/src -I/opt/homebrew/include -DPGWT_SERVER"
for f in summary_writer summary_reader compute event_reader event_writer wait_event idle_rule cJSON; do
  cc $CFLAGS -c "$W/src/$f.c" -o "$B/$f.o"
done
cc $CFLAGS -o "$B/iwcapture" "$W/tests/results/issue376-measurement/iwcapture.c" \
  "$B/summary_writer.o" "$B/summary_reader.o" "$B/compute.o" "$B/event_reader.o" \
  "$B/event_writer.o" "$B/wait_event.o" "$B/idle_rule.o" "$B/cJSON.o" \
  -L/opt/homebrew/lib -llz4 -lz -lm
echo built
