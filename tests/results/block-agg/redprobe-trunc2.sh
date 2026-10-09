#!/usr/bin/env bash
# RED probe, attempt 2, for the repointed truncation assertion.
#
# Attempt 1 (cur_cache_sync() -> no-op) did NOT turn the truncation assertions
# red: `resets 0 -> 1` still fired, because the entry is ALSO dropped at
# src/server.c:2638 when no valid current-trace coverage entry survives. Two
# independent guards defend the same property, so breaking one leaves it held.
#
# Attempt 2 breaks BOTH:
#   M1  cur_cache_sync()                -> no-op  (identity/shrink validation)
#   M2  `if (!have_current) drop`       -> `if (0)` (coverage-vanished drop)
#
# If the truncation assertions still pass with both gone, the assertion cannot
# see a retained superset and I must report it as decoration, not claim a red.
set -u
D=/root/pgwt-redprobe2-ba
rm -rf "$D"
mkdir -p "$D"
cp -a /root/pgwt-check/agent_block-transition-aggregate/. "$D"/
cd "$D" || exit 99

python3 - <<'PY'
import io, sys
p = "src/server.c"
s = io.open(p, encoding="utf-8").read()

m1 = ("static void cur_cache_sync(struct cur_trace_cache *cc, "
      "const char *path,\n"
      "                           const struct pgwt_event_reader *reader)\n"
      "{\n")
m2 = "        if (!have_current)\n            cur_cache_drop(&srv->cur);\n"

for name, a in (("M1", m1), ("M2", m2)):
    if s.count(a) != 1:
        print("ANCHOR %s NOT FOUND (count=%d) -- NOT APPLIED" % (name, s.count(a)))
        sys.exit(1)

s = s.replace(m1, m1 + "    return; /* RED PROBE M1: validate nothing */\n")
s = s.replace(m2, "        if (0) /* RED PROBE M2 */\n            cur_cache_drop(&srv->cur);\n")
io.open(p, "w", encoding="utf-8").write(s)
print("MUTATIONS APPLIED: M1 cur_cache_sync no-op, M2 coverage-vanished drop disabled")
PY
if [ $? -ne 0 ]; then
    echo "MUTATION NOT APPLIED -- refusing to report a red"
    exit 98
fi

if ! make pgwt-server >/tmp/redprobe2_build.log 2>&1; then
    echo "BUILD FAILED -- a build break is not a red"
    tail -20 /tmp/redprobe2_build.log
    exit 97
fi
make -C tests gen_test_traces >/dev/null 2>&1
if [ ! -x ./pgwt-server ] || [ ! -x ./tests/gen_test_traces ]; then
    echo "missing binary -- not a red"
    exit 96
fi

echo "=== MUTATED run (M1+M2): tests/test_data_current_trace_cache.py ==="
python3 tests/test_data_current_trace_cache.py >/tmp/redprobe2_run.log 2>&1
echo "MUTATED_EXIT=$?"
echo "--- truncation section ---"
grep -nE "truncation:|curcache \{|truncated read matches|never the cached superset|vanished blocks were dropped|did not keep blocks|tests passed" /tmp/redprobe2_run.log | tail -20
echo "--- every FAIL line ---"
grep -cE "^ *FAIL" /tmp/redprobe2_run.log
grep -nE "^ *FAIL" /tmp/redprobe2_run.log | head -30
