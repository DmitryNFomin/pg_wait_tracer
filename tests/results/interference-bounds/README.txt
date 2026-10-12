#359 interference probe saturation

Parent: 75717896130e60f9518105577d5e4da01dc7c2ee (branch agent/cx-359-interference starts here).
Read #359 and #327, and followed #358's accum_grow / whole-result refusal.

parent.txt: SAME parent binary, only --pairs differs. 4096 pairs RETURNED
correctly (exit 0); 4097 pairs did not terminate in 30s (exit 124).
fixed.txt: 4096 and 4097 pairs RETURNED exact per-pair and aggregate totals;
30-row limit returns the correct top pairs after all pairs accumulate.
All four allocation sites refuse with failed=1, rows=NULL, num_rows=0.

The fixture is tests/test_interference_bounds.c: three rounds of time-disjoint
pair groups, repeated with reversed PID order, pid 0, distinct whole-ms
per-pair durations, and two wait events. 4097 pairs use 24582 intervals,
below the existing 50000 interval cap. Scores, sorting, event identity,
each pair's accumulated overlap, and the total are checked exactly.
No tolerances, retries, test hardening, or KNOWN_FAILING changes.

Reproduce on macOS (spawn_stub.c avoids spawn.c's Linux-only pipe2):

  mkdir -p /tmp/pgwt-359-parent
  git archive 75717896130e60f9518105577d5e4da01dc7c2ee src | tar -x -C /tmp/pgwt-359-parent
  cc -g -O2 -Wall -I /tmp/pgwt-359-parent/src -DPGWT_SERVER \
    -DPGWT_INTERFERENCE_PARENT -o /tmp/pgwt-359-parent/probe \
    tests/test_interference_bounds.c /tmp/pgwt-359-parent/src/compute.c \
    /tmp/pgwt-359-parent/src/wait_event.c /tmp/pgwt-359-parent/src/idle_rule.c \
    /tmp/pgwt-359-parent/src/cJSON.c tests/results/probe-bounds/spawn_stub.c -lm
  timeout 30 /tmp/pgwt-359-parent/probe --pairs 4096  # exit 0
  timeout 30 /tmp/pgwt-359-parent/probe --pairs 4097  # exit 124

  cc -g -O2 -Wall -I src -DPGWT_SERVER -o /tmp/pgwt-359-fixed \
    tests/test_interference_bounds.c src/compute.c src/wait_event.c \
    src/idle_rule.c src/cJSON.c tests/results/probe-bounds/spawn_stub.c -lm
  timeout 30 /tmp/pgwt-359-fixed                     # exit 0

The registered Linux test builds with real spawn.o, like #358's test.
Only the interference section of compute.c and result struct changed;
server.c gained only handle_interference's failed arm.

Gate capacity check before starting: fuser /tmp/pgwt-box-check.lock reported
pgwt-gate holders 3067357 3105121 3105122 3108350 and pgwt-gate-2 holders
3545676 3545678. Both are also CI runners. Use EPHEMERAL=1, KEEP=0;
box-check deletes its own VM by id and confirms HTTP 404 in its log.
