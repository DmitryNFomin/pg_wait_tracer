mutate294.out -- the CAPTURED output of ../mutate.sh, which was previously
claimed but not evidenced. Run on gate-2 (root@142.132.191.18) against the
final tree of this branch, 2026-10-07.

Each of 11 mutations is applied to src/, the affected unit test is rebuilt and
run, and the tree is restored. A mutation that left the suite GREEN would be
reported as such -- that would mean the gate cannot see the defect it exists to
catch. None did.

  M1  a SECOND definition of "in a command" (state != idle instead of
      RUNNING/FASTPATH)                                     -> 3 failures
  M2  an always-closed predicate: the "both sides agree because both are
      always false" trap                                    -> 14 failures
  M3  the live gate re-read APPROVES when it cannot resolve a row   -> 1
  M4  a FAILED fresh read fabricates an open gate                  -> 3
  M5  the at-risk set widened past "about to be dropped", so the recheck
      rescues samples that were never dropped                       -> 9
  M6  the recheck decides on UNREAD values -- the signature of placing it
      before the batched wait_event read                             -> 7
  M7  a missing fresh-read function silently approves everything     -> 2
  M8  a recovered sample loses the query id that came with its gate  -> 1
  M9  one at-risk target counted in TWO outcomes                     -> 2
  M10 a recovery tallied as a confirmation (cross-bucket misallocation,
      same totals) -- the case the OLD conservation check could not see,
      because both at-risk targets took the same outcome and the identity
      read 2 == 2+0+0                                                -> 3
  M11 a failed read silently tallied as a confirmed close, i.e. a
      measurement failure presented as a fact                        -> 2

  BASE test_sampler                 rc=0, 0 failures (149/149 checks)
  BASE test_backend_status_layout   rc=0, 0 failures (150/150 checks)

M10 and M11 exist because of a review finding: the conservation check as first
written drove both at-risk targets to the SAME outcome, so it could not
distinguish correct accounting from a cross-bucket swap. The check now drives
all three outcomes in one tick and asserts each bucket independently, and M10 /
M11 are the proof that it now goes red on exactly that defect.
