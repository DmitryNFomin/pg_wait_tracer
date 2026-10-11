#!/bin/sh
# Prove the PARENT (origin/master) version of bypass_truncated_file_under_meta
# cannot fail: run master's test file against a pgwt-server mutated to retain
# AND serve the blocks the truncation removed. A PASS here is the vacuity.
set -e
rm -rf /root/pgwt-334-vacuous
cp -a /root/pgwt-334-iter /root/pgwt-334-vacuous
cd /root/pgwt-334-vacuous
cp /root/master_tests/test_data_current_trace_cache.py tests/
cp /root/master_tests/server_harness.py tests/
python3 - <<'PY'
needle = "    if (cc)\n        cur_cache_sync(cc, path, &reader);"
mut = """    /* mutant: ignore the lower committed watermark for cached blocks */
    if (cc && cc->lo_block == 0 && cc->n_blocks > reader.num_blocks) {
        int held = cc->n_blocks;
        struct pgwt_block_index_entry *idx =
            realloc(reader.block_index, (size_t)held * sizeof(*idx));
        if (idx) {
            reader.block_index = idx;
            for (int b = reader.num_blocks; b < held; b++) {
                idx[b].timestamp_ns = cc->blk[b].index_ts;
                idx[b].file_offset = cc->blk[b].file_offset;
            }
            reader.num_blocks = held;
        }
    }"""
src = open("src/server.c").read()
assert src.count(needle) == 1, src.count(needle)
open("src/server.c", "w").write(src.replace(needle, mut))
print("mutated")
PY
make pgwt-server >/tmp/vac-build.log 2>&1
cd tests
python3 -c "
import sys; sys.path.insert(0,'.')
import test_data_current_trace_cache as m
r = m.TestRunner('MASTER version + serve-stale mutant')
m.bypass_truncated_file_under_meta(r)
ok = r.summary()
print('VERDICT: master assertion', 'PASSED (VACUOUS)' if ok else 'failed (would have caught it)')
raise SystemExit(0)
"
