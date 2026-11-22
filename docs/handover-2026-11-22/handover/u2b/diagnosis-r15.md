# Diagnosis r15: EL8 test_daemon_server and test_ui_live_smoke at c52ba5ae
EL8 VM <ip> (4.18.0-553); tier tree /root/u2b-r14 (built-from eeb62031 = r15 tree; both el8-1 and el8-2 ran there).
/root/u2b-r13 holds binaries built from 959f827b sources (src md5s match `git show 959f827b:`). VM work dir /root/diag15; copies in scratchpad/u2b/diag15/{vm/,*.txt}.
The tree's test files (test_daemon_server.py, coverage_loss.py, ui_live_smoke.py/.sh) match c52ba5ae by md5. VERIFIED.

## 1. test_daemon_server: zero-length range
Measurements (VERIFIED, diag15/covdump*.txt and q_ds_el8_1.txt, own tool vm/tool/covdump.c linked to r14 objects):
- Retained trace (both EL8 runs): v6, clean=1, bad=0, blocks_scanned == committed (600/600, 596/596), no REC_BAD. Records: BOOT,
  25 WATERMARKs all with W == floor == the first gap start, and SEAL a=W_final == that same value. Daemon stderr: `good=1 gaps=24 promotions=0`
  (el8-1) and `good=0 gaps=25 promotions=0` (el8-2). The default window is from == to == retained_from (empty=1).
- Server `info`: from_ns == to_ns = 1795260049769631363, coverage incomplete reasons ["not_captured"], gaps [{24 s, capture_loss}], 2,150,400 events.
- `time_model`/`top_events` sent the way the test sends them (`from_ns`/`to_ns` keys) -> code coverage_unverified, reasons ["not_captured"], uncovered [], no rows.
  The same with `from`/`to` = [info.from, last gap end] -> reasons ["capture_loss"], uncovered [that range].
- The 959f827b server (r13) gives the same answers on the same directory (old_vs_new_server.txt). Removing the daemon stderr and totals files from a
  copy leaves info unchanged apart from now_ns (md5 equal). VERIFIED.
- server.c:773-778 parses only `from`/`to`. `from_ns`/`to_ns` in a request are ignored, so the request falls back to the default window (server.c:5309, coverage_trace.c:431-460:
  a clean boot ends at SEAL, and an end below S is clamped to S). Empty and default -> empty_default -> failed -> "not_captured" (server.c:5333-5340, 5507-5508). VERIFIED by reading.
- Repro (VERIFIED, vm/*.log): c52ba5ae test alone x3 -> exit 94 every time. In those runs the window was non-empty: drops stopped after pgbench ended,
  `good=3 promotions=2`. With an extra background pgbench (mine, killed by PID) so that drops last through the final tick: c52ba5ae x2 -> exit 1 with the
  tier's exact FAIL set (zero range, DB Time 0, 0 events, un-excused); 959f827b tests/ with the same binaries x2 -> exit 94 (4/4 + witness). Plain 959f827b x2 -> 94.
Interpretation: when drops last until shutdown, W never advances, and the server correctly certifies an empty window. The c52ba5ae test grades loss on
info's window [S,S]. coverage_loss.py:283-284 rejects it twice: start >= end, and reasons != ["capture_loss"]. So the run counts as un-excused
and the numeric checks run against withheld replies. The 959f827b test graded an explicit non-empty window (`from`/`to`=capture_to, old lines
140,180-184) before the range check, so it ended in a known loss. Not the damaged-file fix (bad=0, identical old/new server output). Not the gap diagnostic,
and not the stderr or totals file in the directory. VERIFIED for the mechanism; INFERRED: the tier sustains drops longer than a standalone run (more events:
596-600 blocks vs 476-482 standalone). Why it does is NOT DETERMINED; pg_stat_activity/load during the tier would say.

## 2. test_ui_live_smoke: 60 s timeout
- The timeout is in assert_default_live(capture_start, 60000) on the first tab (overview): nothing is printed before the FAIL. The page waits for
  PANEL_CHECKS[overview].ok, i.e. rendered numbers (ui_live_smoke.py:802-803). The numbers are withheld (coverage-unavailable), so it times out.
  numeric_loss (ui_live_smoke.py:850-867) returns falsy and the timeout is re-raised. INFERRED from code + log (the run's replies were not retained).
- Retained UI daemon log: 20 DROP gaps (deltas 1-14, sum 48 = totals ringbuf_drops_total 48), the first 3 s after start. VERIFIED.
- Live repro (VERIFIED, diag15/live_run1.txt): own daemon + 25 tx/s pgbench, live_span=900 time_model/top_events every 5 s. Before the first gap:
  verdict live_edge with rows. After it, every reply was incomplete, reasons ["capture_loss","not_yet_covered"], uncovered ends with not_yet_covered (the live tail), no rows.
  The c52ba5ae loss_signature accepted 0/16 of these and the 959f827b rule 16/16. r13's log shows the same mixed reasons being accepted (box.log:19018ff).
Interpretation: the rewritten rule demands reasons == ["capture_loss"] and every uncovered piece being capture_loss (coverage_loss.py:283, 288).
A Live window always has a not_yet_covered tail at the live edge, so no Live loss reply can ever qualify. The old rule only needed
"capture_loss" in reasons plus drops > 0, which gave exit 94. Test-only and EL8-only through real loss (Ubuntu had no gaps, so replies were live_edge with rows).

## 3. Product or test; user-visible?
- Both failures come from the test rewrite: tests/test_daemon_server.py:174-190 (with 179 checked before any loss grading) for failure 1,
  tests/coverage_loss.py:283-284/288 as used by ui_live_smoke.py:864 for failure 2. Neither is the damaged-file fix or the gap diagnostic. Both appear only with real EL8 loss.
- No wrong number and no wrong "complete" reached a user: every lossy reply was coverage_unverified, without rows, and the UI showed coverage-unavailable. VERIFIED (replies above).
- Older product question (not introduced here, same in 959f827b; INFERRED it may count as a contradictory label under RULE 2 (b), for advisers to judge):
  when the whole capture is loss, `info` advertises from_ns == to_ns and coverage reason "not_captured", while its own `gaps` list says 24 s of capture_loss
  and num_events=2.15M (coverage_trace.c:450-458 clamp, server.c:5507-5508).
  Test 1's request also sends the wrong keys (`from_ns`/`to_ns`, test_daemon_server.py:185,207). The server ignores them, which is why it passes elsewhere.

## 4. What would have to change (not designed)
1. test_daemon_server.py: grade loss on a non-empty explicit window sent as `from`/`to` (e.g. capture start .. capture end) before asserting
   `to_ns > from_ns`, or excuse the empty-range check when that window is a witnessed loss. Optionally, product: info should not report the reason
   "not_captured" for a range that its own gaps list says is capture_loss.
2. coverage_loss.py:283/288: accept a trailing not_yet_covered live-edge piece (and its reason) in Live replies while still requiring the loss
   pieces to be explained by the gap evidence. Test-only.
