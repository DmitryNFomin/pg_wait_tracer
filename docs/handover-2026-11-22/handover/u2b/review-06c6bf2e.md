READY-with-nits

# Review of 8d85321d..06c6bf2e (agent/947-coverage-server, round 14), one round

Reviewer: Claude (did not write the code). Worktree read: scratchpad/wt-u2bf at 06c6bf2e; `scripts/tree-hash.sh` there prints 7669633e0b58c3e2ee2bc52c2f20674a1e97db0b. That is the tree the EL8/Ubuntu tiers in /private/tmp/u2b-r14-evidence ran (VERIFIED: the tree hash I ran matches the evidence's `built-from=`). I do not judge the Linux tier evidence here.
Probes: scratchpad/rv9/ (probe_witness.py/.out, mutate.py/.out, open_snapshot_8d85321d.c).

No blockers. There is one should-fix, which can go into the running follow-up round. There are three nits and two design questions for the lead.

## Should-fix

S1. tests/coverage_loss.py:118. `CapturedDaemon` creates the `-T` directory before it starts the daemon (`Path(directory).mkdir(parents=True, exist_ok=True)`), so it can put its stderr log there. That removes a scenario that tests/test_evidence_capture_live.py relied on at 8d85321d. Line 116 says "The legacy writer must create it first": the test deliberately leaves `base/trace` absent, so the daemon has to create it.
- Why that matters: src/evidence_capture.c:131-136 calls `stat()` on the directory and refuses with "evidence output directory unavailable" if it is missing. The daemon creates the directory in the writer (src/daemon.c:1532, then src/event_writer.c:1278 `mkdir`) before evidence init (src/daemon.c:1906).
- Failure scenario (read from source, not reproduced; needs Linux): a regression moves `pgwt_evidence_capture_init` ahead of the writer, or the writer stops creating the directory.
  - Expected: the optin arm fails ordinarily, because it asserts that "EVIDENCE CAPTURE REFUSED" is not in the log and that there is exactly one evidence file. It did fail ordinarily at 8d85321d.
  - Actual at 06c6bf2e: the directory already exists, init succeeds, and the arm passes.
- This is the one independent check (question 4) that no longer fails the way it did before. It is the only scoped test whose `-T` did not already exist. The others use mkdtemp, mktemp -d or an explicit mkdir; VERIFIED by grep.
- Fix: do not create the directory. Put the log next to it (for example in its parent, or in a sibling mkdtemp registered in `_CLEANUP`) and keep the existing COVERAGE-EVIDENCE print.

## Nits

N1. src/open_snapshot.c:422-445. The COUNTERS_UNKNOWN line carries `detect` = the opening tick's `publish_from_ns`. The reader's taint for that gap ends at its GAP_CLOSE, which is written when the counters become readable again (:446), and that close is not logged. So for UNKNOWN the line does not reproduce the withheld range, which item 1 asks for. It cannot cause a wrong excuse, because any UNKNOWN gap is refused (tests/coverage_loss.py:318-319). VERIFIED by reading, and by the host probe: case "unknown" logs detect=400 while the close comes at 600.

N2. Three raw-trace cross-checks in the helper are correct but are not pinned by any registered control. I deleted each one separately and tests/test_coverage_loss.py still passed (VERIFIED, rv9/mutate.out):
- the GAP_OPEN start/observation equality (:315-317);
- the GAP_CLOSE presence and detect equality (:321-323);
- the logged-floor vs raw-WATERMARK pin (:333-336).

My own probes show the code does refuse each case: "raw start differs by 1ns", "raw GAP_CLOSE missing", "logged floor != raw watermark floor". Add three fixture cases.
- These deletions are equivalent to the unmutated helper, so they are not holes: the component check (the KeyError in the totals lookup refuses anyway), the reason check (the raw reason byte refuses anyway), the bounds check (clipped ranges cannot equal an out-of-bounds range) and the foreign-boot `return None` (it explains nothing).
- Deleting the open, close and reason checks together is caught by the raw_gap_missing control.

N3. The diagnostics test compares the switch-off bytes against 8d85321d only when `--baseline-snapshot` is passed. The registered run (tests/unit_tests.list) compares switch unset vs "0" vs "no", and switch-on writer calls vs switch-off. That is sufficient for inertness. I reran it with the baseline and all three cases are identical (VERIFIED, output below).

## 1. src/open_snapshot.c, the only src/ change

- **Switch off is inert.** VERIFIED by reading every hunk and by running `tests/test_coverage_gap_diagnostics.py --baseline-snapshot rv9/open_snapshot_8d85321d.c`. That gives PASS for one, two and unknown, with stdout (every writer call) and stderr byte-identical to 8d85321d.
  - `cov_log_gap` returns before any output unless `PGWT_TEST_COVERAGE_GAPS` is exactly "1".
  - Moving `t->base_known = false` after the UNKNOWN gap open is equivalent: `cov_gap` and the writer do not read `base_known`.
  - The DROP path keeps the short-circuit: on an open failure there is no close and `rc=-1`, the same as `a || b` before.
  - Nothing else in what is recorded or withheld changed. The other build edits only add counter_vector.c to two host builds, because `pgwt_counter_component_names` is now referenced. The server does not compile open_snapshot.c (SERVER_SRCS, Makefile:62-84).
- **The line matches what the reader uses** (src/coverage.c:316-353 and 398-407). VERIFIED by reading:
  - `start` = `t->w_ns` = the GAP_OPEN `at`.
  - `floor` = `t->fd_ns`. This is the floor of the last WATERMARK written: a failed WATERMARK write restores (w, fd, fi), so the tracker never holds a floor that is not on disk. The reader takes the floor of the last WATERMARK with W <= start, and the DB floor is the one that feeds the top-level `uncovered` (server.c:5337, 5511).
  - `detect` = `in->publish_from_ns`, which is exactly the GAP_CLOSE `end` = the reader's `hi`.
- **SEAL tick:** `pgwt_cov_tracker_seal` runs the same tick, so the line is identical. VERIFIED: in the host probe, case "one" is a gap detected at SEAL.
- **Overlapping gaps:** each is its own interval and the reader takes their union. VERIFIED on a real EL8 capture. In the tiered stop witness, 36 overlapping gaps share floor 51222795642247, and the response range [1795255413593589766, 1795255450595439375] equals [floor, detect of gap 36] under one wall offset, 1795204190797947519 at both ends (computed).
- **Print path:** no buffer is involved (direct fprintf with `%llu` casts and a static name table). It is called only from the main loop (daemon.c:1926) and the clean-shutdown path (daemon.c:2263), not from a signal handler; `handle_signal` is called from the loop at daemon.c:1946.
  - The line takes several write() calls, so another thread's stderr could interleave. A corrupted line raises a JSON error, which the helper turns into a refusal (coverage_loss.py:349). That fails safe. INFERRED.

## 2. Attempts to make the witness excuse what it must not

Method: rv9/probe_witness.py drives the real `loss_signature` with the helper's own fixture plus hand-built traces. Result: 41 probes, 0 unexpected (VERIFIED, rv9/probe_witness.out).

Refused:
- A second uncovered piece that no gap explains.
- A range past detect.
- A range shorter than the clipped gap.
- A foreign-boot gap line, with or without a valid gap beside it.
- COUNTERS_UNKNOWN, in the window or outside it.
- An unfiled component, alone or mixed with a filed one, or in a gap nested inside a filed one.
- A second reason, or a piece with another reason.
- A complete verdict, including one that carries a capture_loss piece.
- Deltas above the totals.
- Missing totals, or totals from another boot.
- An init delta above the init total.
- Missing raw GAP records, a missing GAP_CLOSE, or a raw reason COUNTERS_UNKNOWN when the log says DROP.
- A raw start off by 1 ns.
- A forged floor.
- A gap entirely outside the request.
- A zero-length piece.
- A piece 1 ns past the request end, or 1 ns before its start.
- start == end.
- Touching pieces that are not merged.
- An empty uncovered list.
- detect 1 ns short of the end.
- Floor 1 ns after start.
- Two disjoint gaps checked against one merged range.
- bool or float bounds, a bool delta, or a response carrying `error`.
- A duplicated gap line whose summed deltas exceed the totals.

Excused, correctly:
- detect 1 ns past the end (it is clipped).
- Two disjoint gaps checked against the matching two pieces.
- A duplicated line whose totals cover the sum.

Excused, and noted:
- A raw-only COUNTERS_UNKNOWN gap nested inside a logged DROP gap. The helper checks logged → raw, not raw → logged, and the brief asks only for the first.
- This needs a stderr line that was never written. In the current tests no path produces one: all three gap-open paths log, and every daemon writing into a scoped trace dir has the switch on, including both ui_live_smoke daemons. VERIFIED by grep. Not reported as a finding.

A complete response with a wrong number, and a non-numeric failure next to a valid witness, both exit 1. VERIFIED by the controls, and by the real-test routes in test_capture_smoke_diagnostics: daemon_server (lost, wrong_te) → 1, rc=2 → 1, and cardinality mask/rc failures → 1.

## 3. Item 5 controls

All are registered: tests/test_coverage_loss.py is in tests/unit_tests.list. All are **helper-level**, with synthetic responses and fixture traces. None runs the real reader or server. At 8d85321d the witness controls were mock-based too, so the brief's "otherwise on the helper" applies (VERIFIED from the diff of test_capture_smoke_diagnostics).

Each negative is a one-variable change from the passing fixture, and each fails for its intended reason:
- no_logged_gap, gap_short_of_range, second_reason, complete_wrong_number, unfiled_component, counters_unknown, deltas_exceed_totals, raw_gap_missing, wrong_boot and counter_reset all exit 1.
- independent_check_fails exits 1. This one is trivial: `exit_code(failed)`.

Positives: seal_ring_drop → 94, init_only → 95, ring_and_init → 95. VERIFIED by rerunning on the Mac.

Real test functions routed through mocked server replies (test_capture_smoke_diagnostics, 24 tests, OK; VERIFIED by rerunning): daemon_server, cardinality, CPU straddle, pure-CPU, escalation and the controlled-PID arms.

The real reader's range does equal the logged gaps on EL8. VERIFIED by the two live witnesses in the evidence (tiered, evidence/tiny). That is live evidence, not a registered control.

## 4. Lost assertions and evidence retention

- Compared with 8d85321d, every changed test now runs at least as many independent checks, except S1.
  - test_daemon_server: no longer returns early on loss. It now checks to>from, the partition, and that the time_model and top_events answers carry no error.
  - test_execution_cardinality: the from/to/truncated checks moved before the witness.
  - test_capture_smoke: the controlled per-PID checks are graded on their own responses, the query drill is no longer gated on query_loss, and error checks were added.
  - The live-view CPU* check is still ungated.
  - VERIFIED by reading each diff.
- Changes that are not lost assertions:
  - The removed `controlled_resp` and `loss_resp` queries only fed the old classifier.
  - The diagnostics expectations that changed from 1 to 94 ("capture_loss 7→7" and "after_seal") encode the new rule. The equivalent negative ("no logged gap" → 1) is still there.
- Evidence on a non-pass exit:
  - Every launch through `capture_daemon` logs into its trace dir. `_finish` prints COVERAGE-EVIDENCE and keeps every registered dir unless `_EXIT == 0`; exceptions count, because `_EXIT` stays None.
  - Every direct trace `rm -rf` in the scoped tests was replaced, apart from the copy-then-remove in `keep_failed_trace_dir`. VERIFIED by grep.
  - ui_live_smoke.sh keeps TRACE_DIR and DAEMON_LOG (+ .totals.json) unless rc=0 and cleanup is clean.
  - test_coverage_loss covers this: exits 0/1/94/95 give cleaned/kept/kept/kept. VERIFIED by running.
- Passing runs clean up. test_execution_cardinality now also removes its trace on a pass, which it never did before; that is harmless.

## 5. Registration

- tests/unit_tests.list appends test_coverage_gap_diagnostics.py and test_coverage_loss.py.
- `scripts/check-test-registration.sh`: OK (append-only).
- `scripts/check-runner-registration.sh`: OK (226 scanned).
- `git diff --check 8d85321d 06c6bf2e`: clean.
- tests/test_run_all_known_failing.py: passes. It pins the exact table, which has the new `test_capture_smoke_full|959|el8@4.18|exit=95` right after the 871 full entry. It checks that exit 95 is known only for full smoke on el8 4.18, and red for the other six tests, for el8 5.14 and for Ubuntu.
- run_all.sh `known_failing_issue NAME RC` picks the entry that accepts the RC and otherwise falls back to the first entry, so a failure is reported as a "different failure". VERIFIED by running and reading.
- #959 and #871 are both OPEN (gh issue view).

## Design questions for the lead (not defects of this round)

Q1. Which components does exit 95 count? tests/coverage_loss.py:342-348 takes the union over every gap that overlaps the window, not the gaps a range "needs" (the brief's wording).
- In the EL8 tiered stop, the range equals gap 36's range, and gap 36 is ring-only. The init gaps 9, 17 and 29 are nested inside it.
- Under a minimal-cover reading that run is exit 94, already listed, and the tier would have continued. Under the implementer's reading it is 95, which is not listed.
- The stricter reading is defensible: the nested init gaps withhold [floor, detect_29] on their own, so fixing #871 alone would not clear this window. But the stop depended on this choice.

Q2. Can a SEAL-detected loss ever pass the real totals check? The control socket is answered only inside the main loop. After the loop ends come the final drain and then `pgwt_coverage_shutdown`, which reads the counters for the SEAL tick (src/daemon.c:2046-2066, 2263).
- Drops counted between the monitor's last reply (at most about 50 ms before the loop exits) and that SEAL read are in the SEAL gap's delta but never in the socket totals. That makes `total < sum` (coverage_loss.py:337-341), a refusal and exit 1.
- Scenario, read from source and not reproduced: EL8, a scoped test with load still running at the daemon's natural exit, and drops in that last window. Expected per brief item 5: 94. Actual: 1.
- This fails safe, so the gate goes red rather than giving a wrong excuse. But the item-5 SEAL positive control holds only on the helper's synthetic totals. A different second source for the post-loop counters would be a design change; this is not a request for a tolerance.
