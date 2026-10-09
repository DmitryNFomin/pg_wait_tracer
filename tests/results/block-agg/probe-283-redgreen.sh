#!/usr/bin/env bash
# Prove the REPOINTED #283 probe is a real instrument, not decoration:
#   GREEN with cache reuse intact, RED with it deliberately broken.
# Runs entirely on the gate box, in a scratch dir separate from box-check's.
set -uo pipefail
cd "$HOME/pgwt-probe" || exit 1

echo "=== 1. build (reuse intact) ==="
make -j"$(nproc)" pgwt-server >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
make -C tests gen_test_traces >/dev/null 2>&1

echo "=== 2. repointed probe, cache reuse INTACT -> expect PASS ==="
python3 tests/test_data_current_trace_cache.py > /tmp/probe_green.log 2>&1
green=$?
tail -4 /tmp/probe_green.log
echo "green_exit=$green"

echo
echo "=== 3. BREAK #283 cache reuse: cur_cache_get() always misses ==="
cp src/server.c /tmp/server.c.orig
python3 - <<'PY'
p = 'src/server.c'
s = open(p).read()
# The lookup that makes reuse possible. Forcing a miss leaves the cache
# nominally enabled and still storing, but nothing is ever SERVED from it --
# exactly "the already-cached prefix was NOT reused".
old = "static const struct cur_cache_block *\ncur_cache_get("
i = s.index(old)
j = s.index("{", i) + 1
s = s[:j] + "\n    return NULL;   /* MUTANT: #283 reuse broken */" + s[j:]
open(p, 'w').write(s)
print("mutant applied")
PY
make -j"$(nproc)" pgwt-server >/dev/null 2>&1 || { echo "MUTANT BUILD FAILED"; cp /tmp/server.c.orig src/server.c; exit 1; }

echo "=== 4. repointed probe with reuse BROKEN -> expect FAIL ==="
python3 tests/test_data_current_trace_cache.py > /tmp/probe_red.log 2>&1
red=$?
grep -E "^  FAIL|FAILED|PASSED" /tmp/probe_red.log | head -8
echo "red_exit=$red"

echo
echo "=== 5. revert ==="
cp /tmp/server.c.orig src/server.c
make -j"$(nproc)" pgwt-server >/dev/null 2>&1 && echo "reverted and rebuilt"

echo
echo "=== VERDICT ==="
if [ "$green" -eq 0 ] && [ "$red" -ne 0 ]; then
    echo "PROBE IS REAL: green with reuse intact (exit 0), RED with reuse broken (exit $red)"
else
    echo "PROBE IS NOT EVIDENCE: green_exit=$green red_exit=$red"
    echo "(green must be 0 and red must be non-zero)"
fi
