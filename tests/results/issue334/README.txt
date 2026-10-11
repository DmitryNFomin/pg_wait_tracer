Evidence for #334 (the cache truncation assertions could not fail) and #348
(the startup refusal reason was lost when the request write hit EPIPE).

All of it was produced on the persistent gate box pgwt-gate (root@2.28.45.47,
kernel 6.8.0-139-generic) on 2026-10-11/12, under flock
/tmp/pgwt-box-check.lock, against a tree whose tests/ and src/ files are
byte-identical to the commit that precedes this one -- see provenance.log for
the md5sums, which match. Re-captured in full after the review nits, so these
logs describe the FINAL tree, not the first one:

  abcdedd17e3e1e95c9aeed044bda2d73  tests/test_data_current_trace_cache.py
  f3aacc0222f4fa0115108aeffae7ca75  tests/server_harness.py
  2eb10e68c63fbd1576fec244bd6ab4c8  tests/data_current_trace_cache_mutations.py
  0084cabcdc70e52174c2cf1d42202562  src/server.c  (unmodified, post-merge)

src/server.c's md5 is the one AFTER the `gh pr update-branch` merge of master
(#358 touched it). The whole set was re-run on that merged tree rather than
left describing the pre-merge one: every verdict below -- four src mutants red,
seven sabotages red, 102/102, and master's version still vacuous -- was
re-measured there. The earlier capture named 5d9f32d2428f7ee53554d071caa98550,
which is the pre-merge file; if you are comparing against an older copy of
this README, that is the difference.

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
    baseline green for both functions it later mutates, then four src/
    mutants, each in a throwaway copy of the tree:
      1. "retained stale blocks"      cur_cache_sync() made a no-op
      2. "served stale superset"      + reader block index extended from the
                                      cached descriptors, so the block loop
                                      walks blocks the file no longer has
      3. "refusal without a reason"   server_init()'s diagnostic replaced by
                                      one that does not name the reason
      4. "cached block served one     the cache-hit branch serves n-1 events.
          event short"                Invisible to any single request (the
                                      first one decodes; only a repeat is
                                      served), so it is red ONLY on the
                                      repeated-request check -- which is why
                                      that check exists.
    Mutant 2 is the one master's version approves (see vacuity-master.log);
    here it goes red on 7425 -> 7425 plus the uncached differential.

sabotage.log
    Seven TEST-side sabotages (probe-sabotage_all.py), one per way this gate's
    detection could be made unreachable. Each must make the named assertion
    FAIL:
      S1 cut by byte count again (size // 3)      -> the nonemptiness guards
      S2 PGWT_CURRENT_TRACE_CACHE=0               -> the require_used() guard
      S3 a truncation that removes nothing         -> the file-shrank check
      S4 a one-block fixture (nothing to remove)   -> refuses, never skips
      S5 block_extents stops accounting for the tail
      S6 block_extents answers on a file it could not read
      S7 the interposed `info` request deleted     -> 7425 -> 0, RED
    S7 answers the question a reader of that interposed request will ask:
    does this test LEAN on #342 (the one request after a shrink answers
    empty)? No -- without the ordering request the measurement comes back
    empty and the assertion fails, so fixing #342 would BREAK this test
    rather than silently satisfy it.

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
