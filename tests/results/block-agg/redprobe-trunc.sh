#!/usr/bin/env bash
# RED probe for the repointed truncation assertion in
# tests/test_data_current_trace_cache.py.
#
# The property guarded: "A cache that cannot prove what it holds must refuse to
# answer, never answer anyway" (src/server.c:1185). cur_cache_sync() is what
# proves it -- path/header identity, the file still having at least the cached
# blocks, and every cached block's (first ts, file offset) still agreeing with
# the reader's block index.
#
# The break: make cur_cache_sync() a no-op. The entry then keeps blocks the
# truncated file no longer has and serves the stale superset. If the repointed
# assertion cannot see that, it is decoration.
#
# Every failure mode below exits non-zero with a distinct message rather than
# printing a red it did not earn: a missing anchor, a build break, or a missing
# binary is NOT evidence that the gate bites.
set -u
D=/root/pgwt-redprobe-ba
rm -rf "$D"
mkdir -p "$D"
cp -a /root/pgwt-check/agent_block-transition-aggregate/. "$D"/
cd "$D" || exit 99

python3 - <<'PY'
import io, sys
p = "src/server.c"
s = io.open(p, encoding="utf-8").read()
anchor = ("static void cur_cache_sync(struct cur_trace_cache *cc, "
          "const char *path,\n"
          "                           const struct pgwt_event_reader *reader)\n"
          "{\n")
if s.count(anchor) != 1:
    print("ANCHOR NOT FOUND (count=%d) -- NOT APPLIED" % s.count(anchor))
    sys.exit(1)
s = s.replace(anchor, anchor + "    return; /* RED PROBE: validate nothing */\n")
io.open(p, "w", encoding="utf-8").write(s)
print("MUTATION APPLIED: cur_cache_sync() is now a no-op")
PY
if [ $? -ne 0 ]; then
    echo "MUTATION NOT APPLIED -- refusing to report a red"
    exit 98
fi

if ! make pgwt-server >/tmp/redprobe_build.log 2>&1; then
    echo "BUILD FAILED -- a build break is not a red"
    tail -20 /tmp/redprobe_build.log
    exit 97
fi
make -C tests gen_test_traces >/dev/null 2>&1
if [ ! -x ./pgwt-server ]; then
    echo "no pgwt-server binary -- not a red"
    exit 96
fi
if [ ! -x ./tests/gen_test_traces ]; then
    echo "no gen_test_traces binary -- not a red"
    exit 95
fi

echo "=== MUTATED run: tests/test_data_current_trace_cache.py ==="
python3 tests/test_data_current_trace_cache.py >/tmp/redprobe_run.log 2>&1
echo "MUTATED_EXIT=$?"
echo "--- truncation section + tally ---"
grep -nE "truncation:|curcache |truncated read matches|never the cached superset|vanished blocks were dropped|did not keep blocks|tests passed" /tmp/redprobe_run.log | tail -30
echo "--- every FAIL line ---"
grep -c "FAIL" /tmp/redprobe_run.log
grep -nE "^ *FAIL" /tmp/redprobe_run.log | head -30
