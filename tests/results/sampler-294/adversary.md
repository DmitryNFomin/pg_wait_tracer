1. Passes while broken:
   - predicate-identity differential is satisfied if BOTH derivations return
     cmd_open=false for every state (state_running misresolved out of range).
     Pinned: the same test asserts >=1 true and >=1 false over the enumerated
     states, plus a mutation probe.
   - order-race test is satisfied if the fake reader is invoked for targets
     that were never at risk. Pinned: per-index call counter asserted exactly.
2. Single component failing -> skip/hang instead of fail:
   - pgwt_pgbs_read_cmd_gate failing (backend gone, layout unvalidated, EPERM)
     must leave cmd_open 0 and count read_failed; never fabricate 1. Coherency
     retry is bounded (32) so a changecount storm cannot hang the tick.
   - a NULL reader / n==0 must return 0 recovered and touch nothing (refuse).
3. Timing/ordering dependence:
   - the fix IS an ordering change: recheck must sit AFTER read_targets and
     BEFORE build_batch. Pinned by a test asserting the recheck is a no-op
     when valid[]==0 for all targets -- the exact signature of placing it
     before the wait_event read.
   - live numbers are wall-clock; pinned by n>=5 and reported spread, never by
     widening a tolerance.
