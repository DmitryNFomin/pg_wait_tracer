tests/results/exec-index-wiring/ — evidence for the executions-index guard
layer (branch agent/executions-index-wiring, code commit ff0eb2a).

WHAT IS IN HERE
---------------
unit-green-macos.txt         tests/test_exec_index_guards.c, 29/29, macOS
web-unit-green.log           tests/web_unit/execution-counts.test.mjs, 10/10
red-ui-poison-cells.log      the SAME web test with the fix reverted in
                             web/static/lib/builders/waterfall.js (the two
                             column formatters put back to String(r.n_events)
                             and the executionCountsKnown() line removed from
                             executionHasDetail): 6 pass, 4 FAIL. That is the
                             demonstrated red for criterion 3.
mutations-summary.txt        tests/exec_index_guard_mutations.py --self-test:
                             11 mutations of src/compute.c, 11 RED; 2
                             semantic no-op controls, both GREEN; 5
                             harness-self-test paths, all ok; "0 not
                             detected / PASSED".
mutations/g01..g11.txt       the full unit-test output of each mutant, so a
                             reviewer can see WHICH checks went red rather
                             than trusting the one-line verdict.
make-check-full.log          make check (full) on this Mac, CHECK PASSED.
GATE STATUS: NO LIVE-TIER PASS EXISTS FOR THIS BRANCH YET.
Stated plainly because an earlier revision of this file said "No Hetzner VM
was created", which became false the moment one was. What actually happened,
in order:

box-check-gate1.log          ATTEMPT 1, ABANDONED. make box-check against the
                             persistent gate-1 box (pgwt-gate,
                             root@2.28.45.47). Four lines, no verdict: it sat
                             on "waiting for /tmp/pgwt-box-check.lock" for
                             ~30 minutes behind a live run_all plus two other
                             queued runs, and was killed rather than left
                             holding a queue slot. This file contains NO
                             result from gate-1.
box-check-ephemeral.log      ATTEMPT 2, FAILED (exit 1), and the failure was
                             MINE, not the tree's: the launcher exported
                             PG=all, but scripts/box-check.sh:307 builds its
                             argument as `[[ -n "$PG" ]] && pgarg="--pg-version
                             $PG"`, so "all" is spelled as the EMPTY string and
                             a literal PG=all made run_all.sh look for a
                             postmaster of major version "all"
                             ("ERROR: --pg-version all requested but no running
                             PostgreSQL all postmaster was found"). The Linux
                             build and C units ran; the live tier never
                             started, and no tests/results/ui_live/ was
                             produced. So there is also no live-UI-smoke
                             verdict here.
                             A Hetzner VM WAS created for this attempt:
                             pgwt-dev-1791559654-29179, id 169539426,
                             2.28.138.111, kept (KEEP=1) for reuse and to be
                             deleted by id by this agent.

Consequence, so no reviewer has to work it out: criteria 2+3 are NOT
gate-ready on the evidence in this directory. make check passed; the live
tier has not run to completion even once. src/ changed, so the
docs-only/tests-only re-run exemption in CLAUDE.md does not apply.

HOW THE RED WAS PRODUCED
------------------------
The two guards are NEW functions, so running the new C test against the
parent tree (f78ab4c) does not compile — there is nothing there to call. Two
substitutes, both stronger than a bare parent-tree run:

1. tests/test_exec_index_guards.c carries a VERBATIM COPY of the predicate
   that shipped at f78ab4c (old_execution_matches_request) and asserts the
   differential on the one fixture that distinguishes them: a row with
   matches_event_filter = -1 under a class filter. The copy returns 1 (KEEP);
   the shipped predicate returns -1 (UNDECIDABLE). The defect is therefore
   demonstrated inside the test suite itself, on every run, not argued about
   in a commit message.

2. exec_index_guard_mutations.py G01 re-injects that exact predicate into
   src/compute.c and the test goes RED with 5 failing checks. G02..G11 are
   the other false-negative shapes (undecidable collapsed to keep, collapsed
   to drop, `== -1` instead of `< 0`, boundary dimensions demoted below the
   event one, a NULL row reading as keep, counts_known needing BOTH fields
   poisoned, counts_known always approving, counts_known rejecting the
   legitimate 0/0 row). All RED.

G08 FOUND A REAL BLIND SPOT, and it is recorded here rather than quietly
fixed: on the first run, turning `>= 0` into `!= -1` left the test GREEN,
because every fixture used exactly the -1 sentinel. A stayed-green mutant is
the TEST failing, so the fixture (n_events = -7, n_workers = INT_MIN) was
added; G08 is now RED on it.

THE HARNESS'S OWN FALSE-NEGATIVE PATHS (--self-test)
----------------------------------------------------
A mutation harness that silently could not apply, build or run reports
"0 not detected" and reads as success. Each of these must come back FAIL,
and the self-test asserts that it does:

  needle matches ZERO times      -> FAIL (mutation did not apply)
  needle matches SEVERAL times   -> FAIL (ambiguous, cannot attribute)
  mutant does not COMPILE        -> FAIL (not evidence)
  toolchain missing (exit 127)   -> FAIL (cannot see => refuse)
  a dependency exiting 126       -> observable as 126, not as a pass

Plus: the pristine baseline must be GREEN before any mutant is judged, and
two semantic no-op CONTROLS must stay GREEN — a test that goes red on a
comment is reacting to something other than the behaviour it claims to pin.

WHAT IS NOT IN HERE
-------------------
The index SERVE path. Routing executions/waterfall/exec_scatter at
pgwt_exec_index_query() needs a bit-exact source for n_events/n_workers, and
neither a sub-range raw replay nor the boundary row set can supply one on its
own: the per-pid open-row stack and the plan-phase carry at the window edge
are not derivable from struct pgwt_exec_index_entry, and
pgwt_compute_executions' worker dedup (worker_last_row) is a whole-stream
quantity. "Bit-exact or it does not ship" outranks shipping the wiring, so
the two prerequisite guards (Phase 3 review criteria 2 and 3) shipped alone
and the serve path is an open design question — see the agent report.
