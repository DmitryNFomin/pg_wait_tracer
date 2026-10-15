#!/bin/sh
# Build test_interval_index against the INTEGRATION tree's compute.c.
#   $1 = "pristine"  -> integration's own interval_index + test (reproduce the
#                       reported regression)
#   $1 = "mine"      -> my branch's interval_index + test, integration compute
set -e
W=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a8454736303f140de
S=/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/f
I=$S/integ
D=$S/mix_$1
rm -rf "$D"; mkdir -p "$D"
cp "$I"/src/*.h "$D"/
if [ "$1" = "mine" ]; then
  cp "$W"/src/interval_index.c "$W"/src/interval_index.h "$D"/
  TEST="$W/tests/test_interval_index.c"
else
  cp "$I"/src/interval_index.c "$I"/src/interval_index.h "$D"/
  TEST="$I/tests/test_interval_index.c"
fi
cc -g -O1 -w -I "$D" -I "$I/src" -DPGWT_SERVER -o "$D/t" \
  "$TEST" "$D/interval_index.c" "$I/src/compute.c" \
  "$I/src/wait_event.c" "$I/src/idle_rule.c" "$I/src/cJSON.c" \
  "$S/mac_stub.c" -lm
echo "built $D/t"
