Evidence for #334 (the cache truncation assertions could not fail) and #348
(the startup refusal reason was lost when the request write hit EPIPE).

All of it was produced on the persistent gate box pgwt-gate (root@2.28.45.47,
kernel 6.8.0-139-generic) on 2026-10-11, under flock /tmp/pgwt-box-check.lock,
against a tree whose tests/ and src/ files are byte-identical to the commit
that precedes this one -- see provenance.log for the md5sums, which match:

  cf40ebd444c89ec523d861ce8d72a75d  tests/test_data_current_trace_cache.py
  f3aacc0222f4fa0115108aeffae7ca75  tests/server_harness.py
  021ea9ca9c59a1e8c3d8e21c6b925e25  tests/data_current_trace_cache_mutations.py
  5d9f32d2428f7ee53554d071caa98550  src/server.c  (unmodified master)

provenance.log
    Host, kernel, UTC timestamp and the md5sums above.

vacuity-master.log              <- THE REASON THIS BRANCH EXISTS
    origin/master's OWN test file, run against a pgwt-server mutated to retain
    AND serve the blocks the truncation removed. It passes 5/5, answering
    {"events": [], "total_count": 0, "fidelity": "none"} with curcache
    served=0. Master's version compares one empty answer to another, so it
    cannot distinguish a correct server from one serving a stale superset.
    Produced by probe-vacuity-master.sh.

mutations.log
    tests/data_current_trace_cache_mutations.py (committed, reproducible):
    baseline green for both functions it later mutates, then three src/
    mutants, each in a throwaway copy of the tree:
      1. "retained stale blocks"      cur_cache_sync() made a no-op
      2. "served stale superset"      + reader block index extended from the
                                      cached descriptors, so the block loop
                                      walks blocks the file no longer has
      3. "refusal without a reason"   server_init()'s diagnostic replaced by
                                      one that does not name the reason
    Mutant 2 is the one master's version approves (see vacuity-master.log);
    here it goes red on 7425 -> 7425 plus the uncached differential.

sabotage.log
    Six TEST-side sabotages (probe-sabotage_all.py), one per way this gate's
    detection could be made unreachable. Each must make the named assertion
    FAIL:
      S1 cut by byte count again (size // 3)      -> the nonemptiness guards
      S2 PGWT_CURRENT_TRACE_CACHE=0               -> the require_used() guard
      S3 a truncation that removes nothing         -> the file-shrank check
      S4 a one-block fixture (nothing to remove)   -> refuses, never skips
      S5 block_extents stops accounting for the tail
      S6 block_extents answers on a file it could not read

epipe-parent-harness.log
    The new bypass case bypass_refusal_reason_survives_epipe run against the
    PARENT tests/server_harness.py (no EPIPE branch): 3/6, with both
    reason-retrieval checks red. The silent-stub case still passes there, as
    it should -- it is the false-negative guard, not the fix's target.

full-suite.log
    The whole test_data_current_trace_cache.py on the box: 102/102.

The box-check that gates the branch is tests/results/box-check-ubuntu-
20261011-223624.log (exit 0, 103 passed / 0 failed, Live UI smoke PASS,
tests/results/ui_live/run.id = 1791751109).
