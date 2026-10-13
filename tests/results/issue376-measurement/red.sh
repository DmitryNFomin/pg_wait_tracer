#!/bin/sh
# Build the new unit test against a PATCHED copy of src/ where one half of the
# fix is reverted, and run it. $1 = which revert: writer | hoist | heatmap
set -e
W="/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-ac6ce72dc9ecbbff0"
S="/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/red-$1"
rm -rf "$S"
mkdir -p "$S/src" "$S/tests" "$S/b"
cp "$W"/src/*.c "$W"/src/*.h "$W"/src/*.inc "$S/src/" 2>/dev/null || true
cp "$W"/tests/test_io_worker_summary.c "$S/tests/"

case "$1" in
writer)
  # Revert ONLY the behaviour, keeping the signature, so the test still
  # compiles: the writer ignores what the producer told it.
  perl -pi -e 's/^    bool io_worker = \(cat_flag & PGWT_EVENT_FLAG_IO_WORKER\) != 0;/    bool io_worker = false;   \/* REVERTED for the red run *\//' "$S/src/summary_writer.c"
  grep -n "bool io_worker = false" "$S/src/summary_writer.c"
  ;;
hoist)
  # Put the resolve back BELOW the summary push (the pre-fix layout) and pass
  # 0, exactly as the daemon did before.
  perl -0pi -e 's/    struct pgwt_pid_accum \*pa = NULL;\n    uint32_t cat_flag = 0;\n    if \(!PGWT_IS_MARKER\(we\)\) \{\n        pa = pgwt_get_or_create_pid\(acc, evt->pid\);\n        cat_flag = pgwt_live_pid_cat_flag\(d, pa, evt->pid\);\n    \}\n//' "$S/src/event_stream.c"
  perl -pi -e 's/pgwt_summary_push_event\(d->summary_writer, evt, cat_flag\);/pgwt_summary_push_event(d->summary_writer, evt, 0);/' "$S/src/event_stream.c"
  perl -pi -e 's/^    \/\* pa \/ cat_flag were resolved above the summary push \(v6\) -- the same/    struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);\n    uint32_t cat_flag = pgwt_live_pid_cat_flag(d, pa, evt->pid);\n    \/* REVERTED: resolve below the push/' "$S/src/event_stream.c"
  grep -n "pgwt_summary_push_event\|pgwt_live_pid_cat_flag" "$S/src/event_stream.c"
  ;;
heatmap)
  perl -0pi -e 's/        if \(ev->flags & PGWT_EVENT_FLAG_IO_WORKER\)\n            continue;\n\n        uint64_t ev_ts = ev->timestamp_ns;/\n        uint64_t ev_ts = ev->timestamp_ns;/' "$S/src/compute.c"
  grep -c "PGWT_EVENT_FLAG_IO_WORKER" "$S/src/compute.c"
  ;;
*) echo "usage: red.sh writer|hoist|heatmap"; exit 1 ;;
esac

CFLAGS="-g -O2 -Wall -I$S/src -I/opt/homebrew/include -DPGWT_SERVER"
for f in summary_writer summary_reader compute event_writer wait_event idle_rule cJSON; do
  cc $CFLAGS -c "$S/src/$f.c" -o "$S/b/$f.o"
done
cat > "$S/b/stub.c" <<'EOF'
#include <stdio.h>
int pgwt_proc_open(const char *const argv[], FILE **out);
int pgwt_proc_close(FILE *fp);
int pgwt_proc_open(const char *const argv[], FILE **out) { (void)argv; (void)out; return -1; }
int pgwt_proc_close(FILE *fp) { (void)fp; return -1; }
EOF
cc $CFLAGS -c "$S/b/stub.c" -o "$S/b/stub.o"
cc $CFLAGS -o "$S/b/t" "$S/tests/test_io_worker_summary.c" \
  "$S/b/summary_writer.o" "$S/b/summary_reader.o" "$S/b/compute.o" \
  "$S/b/event_writer.o" "$S/b/wait_event.o" "$S/b/idle_rule.o" "$S/b/cJSON.o" "$S/b/stub.o" \
  -L/opt/homebrew/lib -llz4 -lz -lm
# cwd must be the patched tests dir so the section-4 source gate reads ../src
cd "$S/tests" && "$S/b/t"; echo "exit=$?"
