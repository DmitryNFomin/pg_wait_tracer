# Fable: live window, false "retention", idle exact_perf (2026-11-20)
VERDICT: PROCEED WITH CHANGES. Q1 server-resolved span, not a web clamp. Q2 stop at the current boot. Part 2 is a reader fix plus a spec amendment. Part 3's "W never moved" is UNDER-SUPPORTED and its fix needs the owner.
Paths: wt = scratchpad/wt-u2b4 @fb43c3c6; T = agent/933-tickfix @ecd3f611 (git show HEAD:).

## Part 1
1. The verdict is right; the request is wrong. VERIFIED wt web/static/lib/state.js:101-105, :136-141: always `from = now - span`, never clamped.
2. The web clamp as proposed is WRONG: "round up to the next microsecond" still lands below S, because the web's copy of S is already up to 128 ns low before it rounds.
   - MEASURED (node, 2e6 random S near 1.795e18, through Number, ceil-us, JSON.stringify): 246,525 of 2,000,000 (12%) below S; ceil + 1 us: 0 below. Simulation only, not run against the server.
3. Recommend the server resolves it (amendment A4): a live request carries a span and no `from`; the server uses [max(now - span, S of the live boot), live edge]. An explicit `from` is still never moved, so rows 23 and 24c stand. Add `requested_from_ns` and a clip cause (capture_start or restart).
   - Precedent VERIFIED: the server already resolves a default window to the latest boot's [S, W] (wt src/coverage_trace.c:419-420) and echoes resolved from_ns/to_ns plus `requested_to_ns` (wt src/server.c:5459-5463).
   - "One verdict in one place": both options keep the verdict in the server; only this one keeps the boundary S out of the web, which cannot hold it exactly (INFERRED). S is already computed twice (wt src/coverage.c:377, src/coverage_trace.c:419, VERIFIED): any S change must hit both, or make one call the other.
4. UI text when clipped: "Last 15 min — showing since capture start HH:MM:SS"; after a restart "… since daemon restart HH:MM:SS" (or PostgreSQL restart, from restart_cause).
   - Every rate, average and AAS denominator must use the echoed from_ns/to_ns, not liveRangeSecs. NOT CHECKED which web views divide by the requested span; if any does, the clip becomes wrong numbers (RULE 2).
5. Q2: stop at the current boot. Spanning leaves the default view blank on all 10 whole-window routes for 15 minutes after every restart, which is when an operator opens it.
   - Stopping shows the new boot's numbers only, never summed across boots, so #947 holds. The pre-restart chart leaves the default view, so the notice must name the restart and its time; an explicit window across it still returns withheld (restart). This is a product choice: if Astra disagrees it goes to the owner.
6. Q3, the gate. VERIFIED wt tests/ui_live_smoke.py:694-713, :1530-1535: it sets `span=min(60, age-2)`, so the default view is never opened.
   - Required: ui_live_smoke opens the DEFAULT view (no hash span) on the young box daemon and asserts numbers on every whole-window tab plus the clip notice; a second arm restarts the daemon and asserts the same within one refresh. Delete `_select_covered_live` or keep it only as an extra arm.
   - Bypass demonstration: run that gate unchanged on fb43c3c6 and paste the red (the review cites g1/ubu-c99a35ce.log overall=FAIL; I did not open that log). Server-level: a test_coverage_consumers live case with span > age expecting complete or live_edge with requested_from_ns, mutant "ignore S in span resolution" red on it. The mock gallery cannot catch this.

## Part 2
7. Reader defect on top of a spec gap; not the writer.
   - VERIFIED wt src/coverage.c:377: S = max(boot_start, recs[0].a) for any earliest file; :546 labels [boot_start, S) `retention` unconditionally. VERIFIED wt src/event_writer.c:918-919: BOOT observation is the file's own open time; src/open_snapshot.c:605 passes d->start_ts as boot_start, and the file opens later.
   - file_no is already in the model (wt src/coverage.h:26, REC_FILE b = file_no), so the reader can tell that nothing was removed.
8. Smallest correct fix: when the earliest retained file has file_no 0, emit no `retention` and set S = boot_start; keep max() and `retention` when file_no > 0. Spec §2 and A1 item 5 need the amendment and two answer sets. A writer change would alter golden bytes and leave already-recorded v6 captures mislabelled.
   - S = boot_start assumes every record closing in [boot_start, file open) is in file 0: INFERRED from lazy open, not proven. Fail-closed fallback if the box test cannot prove it: keep S and label that stretch `not_captured`.
9. Tests: new verdict row "f0 (file_no 0, BOOT obs 5), [0,50) → complete, no retention" with mutant "ignore file_no" red on it; row 13a (file_no > 0) keeps retention with mutant "never retention" red on it.
   - Box: fresh daemon, `info.from_ns == boot_start`, no reason `retention`, a wait closed before the first file open is present in file 0; print the gap size, which is still unmeasured on a real daemon.

## Part 3
10. "On EL8 … W never moved past boot_start" is UNDER-SUPPORTED twice.
    - Not EL8-only: Ubuntu fails the same assert. VERIFIED scratchpad/a2go/tcov/g1/ubu-ecd3f611.log:17027-17029 and el8-ecd3f611.log:18001-18003.
    - The test samples once, at the first watermark: start_daemon returns when `w_now > 0` (T tests/test_exact_perf_coverage_live.py:120) and :238-240 reads at once, about 1 s after start. W == boot_start there is expected under almost any design; it does not show W stays put.
11. From the code, the frontier cannot advance with zero records. VERIFIED: coverage_cursor moves only on a FRONTIER output (T src/merge_capture.c:296; certified_end is boot_start until the first, :196); a round is requested only when the queue is non-empty or the capture is waiting (T src/percpu_transport.c:648; :654 "idle means no helper work"); `waiting` needs retained or staged records (T src/merge_capture.c:904-905).
    - INFERRED: W advances only when a record arrives, and on a quiet PostgreSQL auxiliary wake-ups are seconds to minutes apart. So a live window whose `to` is more than L = 3 s past W reads incomplete [not_yet_covered] until the next record, and a crash leaves unclean_end from the last W.
12. Cheapest settling measurement: in that arm wait 10 s, then print W - boot_start, rounds_requested and the DATA record count; one EL8 and one Ubuntu run.
13. Smallest fix: on the 1 s coverage tick, request one boundary round only if no FRONTIER arrived since the previous tick (zero extra rounds under load).
    - Cost at most 1 round/s, one wake per CPU; loaded runs do 35-48 rounds/s (g1 logs, ARM lines), so INFERRED well under 1% of a core per helper. Unmeasured: report helper CPU on an idle box.
14. It conflicts with the letter of the owner's decision, not the budget. VERIFIED #871 comment 6848477572: "nothing is woken on an idle box with nothing waiting", chosen over a periodic helper. A 1 Hz round wakes every helper on an idle box.
    - Do not reinterpret "the label is waiting" as demand. Ask the owner one line after the two measurements: "Certifying quiet time needs one helper wake per CPU per second on an idle box (measured X% of a core). Accept, or quiet periods show 'not yet confirmed' until the next event?" No-wake alternative: a round only when a viewer asks over the control socket; free when unwatched, but unobserved quiet time before a crash stays unclean_end.
NOT CHECKED: web denominators; whether coverage_cases.json carries file_no; the core's FRONTIER rule for a data-less round; g3 box logs for fb43c3c6; the c99a35ce log.
