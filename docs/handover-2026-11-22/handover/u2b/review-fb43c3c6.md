# Review: agent/947-coverage-server, round 4 (80b88663..fb43c3c6), whole unit 4059fcde..fb43c3c6

**Verdict: NOT-READY. 1 blocker (B1), 1 nit. The blocker is a user-facing defect this unit introduces. Fixing it needs a design decision and a web or server change, both outside this unit's brief. All round-4 code that is in scope is correct.**

## What changed (round 4)
- Live default window: while a boot is live, it now ends at W instead of floor(W) (src/coverage_trace.c:415). A wait that is still open no longer shrinks the window to almost nothing.
- Integer JSON clock values are read exactly from the request text (server.c:690 `request_clock`). They are no longer rounded through a double, which moved them by up to 128 ns. `from:0` / `to:0` mean "default" again, as on master.
- Replay checks for duplicate or overlapping files before the coverage verdict can return (replay.c:20, :146).
- Damaged committed data is refused as `trace_unreadable` again, ahead of coverage withholding:
  - a meta count larger than the reader's block count (server.c:2934);
  - a dispatch-level guard (server.c:9487).
- Test windows now stay inside recorded coverage. The mock server gives coverage/fidelity_ranges/from/to on every covered view, as the real server does.

## Evidence
- Check stamp: `.pgwt-check.stamp` is absent in wt-u2b4. `scripts/tree-hash.sh` = 502bbb7100ec3e9e9a8a4a4ac3cf82a92294fa89. I did not run make check (read-only brief).
- Box-check: both runs are still in progress. Read at 10:58Z:
  - g3/el8-fb43c3c6.log: 2,950 lines, at sampled_attr_io, no summary yet.
  - g3/ubu-fb43c3c6.log: 158 lines.
  - Both headers say `source commit=fb43c3c6… tree-hash=502bbb71…`, which matches the tip and the tree.
  - No run_all summary exists yet for this tip.
- Host runs on an exported copy of fb43c3c6 (scratchpad/rv-u2b4):
  - `test_coverage_consumers.py`: all 21 cases PASS. Timing: max 13.0 ms, mean 3.9 ms.
  - `coverage_consumers_mutants.py`: "16 built, 16 red on named cases". Each of the 6 new mutants goes red at its named assertion.
- UI gallery: not applicable, since web/ is unchanged.

## Codex's root-cause table, row by row
| Row | Codex | My judgement |
|---|---|---|
| test_wait_cpu_scan | test | Agree. splice() does nothing without a SEAL, so a real COVERAGE block is now inserted. The assertion is unchanged. |
| test_summary_raw_parity | test | Agree. The fixture now declares coverage up to 351 s, the window the test asks for. The parity assertion is unchanged. |
| test_window_additivity | both | Agree. Three parts: (1) 0 = default matches master (4059fcde:4915 `req->from_ns ? … : earliest`). (2) D4 now expects `invalid_window`, which is spec row 22, an independent oracle. (3) The new "after SEAL is withheld" control is an added check, not a looser one. |
| test_current_trace | test | Agree. The synthetic clock is now pinned to its header and SEAL, which is stricter than the old "within 1 year". |
| test_protocol_drift | both | Agree. The mock's fidelity_ranges on every covered view matches add_coverage (server.c:5489). Info now carries coverage. The status/metrics grouping still requires all three pairs. The partial-final-bucket check fits the ceiling-division buckets. The trace_unreadable code is restored by a product fix. |
| test_cli | product | Agree. Before the fix, the replay coverage early-return came before the duplicate check. |
| test_evidence_capture_live, test_execution_cardinality | product | Agree. A wall-clock ns value (~1.8e18) is above 2^53, so the old double path rounded it. The numeric_clock case and its mutant pin the fix. |
| test_capture_smoke_tiered/_full | test | Agree, and the tests still check real things (question 2 below). |
| test_phase_absent | test | Agree. It now waits for published coverage around the workload. No retry and no relaxed assertion. |
| test_ui_live_smoke | both | **Half wrong.** The "dead floor" half is a real product fix. The "fifteen-minute pre-start window" half is a product UX defect. The test change hides it instead of fixing it; see B1. |

Question 2 (capture smoke):
- server_query's default window is now [info.from_ns, info.to_ns], the latest boot's [S, SEAL]. The wait, attribution and CPU checks are all still there.
- Pure-CPU straddle: the AAS window now ends at SEAL instead of shutdown + 3 s. Nothing is recorded after SEAL, so the AAS integral (mean × span) is unchanged. The ±20% /proc ratio is unchanged.
- Ring stall: the trace window now matches the /proc delta's interval, and the bound is still `> 500 ms`. A narrower window can only lower the trace CPU, so this is stricter, not weaker.

Question 3 (product fixes): all four are correct.
- Replay preflight: the same span-collision logic as before, just run earlier.
- `request_clock`: matches cJSON's first, case-insensitive key. Escaped keys, nested objects and exponent or negative numbers fall back to the old parse.
- Zero defaults: match master.
- Mock changes: match the real server's shapes.

Question 4 (regressions of earlier findings): none.
- Live totals: the shape case passes and the mutant is red.
- Empty-clip info: passes and the mutant is red.
- Cache: 14 cache sub-cases pass, timing is under 50 ms, and refresh_once passes.
- Pilot bypass: passes and both mutants are red.
- Restart withholding on every route: passes.
- New behaviour: `add_coverage` now skips withholding when the response already carries an `error`. Only refusals carry one, so no data leaks through this.

## Findings
1. **Blocker (tag-blocker b and d, introduced by this unit): for the first 15 minutes of every capture, the default live UI withholds every whole-window number and shows a wrong reason.**
   - Code: web/static/lib/state.js:101 and :137 (`from = now − 900 s`, always explicit) meets spec §2 (live_edge needs S_b ≤ from) and the merged verdict. The test that hides it: tests/ui_live_smoke.py:694-713 and :1530-1535.
   - Input: a live daemon that started less than 15 minutes ago. The user opens the UI in its default state: Live mode, last 15 minutes.
   - Expected: the tabs show the capture's numbers, as master does today, with any label about the time before the capture that the spec allows.
   - Actual, reproduced on a host build of fb43c3c6 (Codex's live daemon fixture, request [now−900 s, now)):
     - These routes return `{code:"coverage_unverified"}` with no numbers: time_model, top_events, top_sessions, top_queries, transitions, concurrency, lock_chains, interference, variants, fingerprints. That covers the Overview time model and the Events, Sessions, Queries, Transitions and Concurrency tabs.
     - aas and heatmap keep only the buckets after S, with their totals withheld.
     - session_timeline, executions and exec_scatter keep their rows with the label.
     - reasons = [retention, not_yet_covered, not_captured]. The merged web renders that as "Coverage unverified — whole-window metrics unavailable (older than the retained data) (not yet confirmed — live) (not captured)" (computed with web/static/lib/builders/fidelity.js). On a fresh, live, never-rotated capture, "older than the retained data" and "not yet confirmed" are both wrong.
     - The `retention` reason comes from [boot_start, S): the BOOT record is written after the start time (open_snapshot.c:605 passes `d->start_ts`). I saw this on the fixture. The size of that gap on a real daemon is read from source, not measured.
   - The same thing happens for 15 minutes after every daemon restart, as `restart`. That case is #947's intended verdict, but it lands on the default view too.
   - Why the gate stays green: the live smoke now waits for a complete `info` and then sets `span=min(60, age−2)` through the URL hash. It never opens the default 15-minute view, even on an older capture. On the earlier tip, the same gate went red on all 11 tabs (g1/ubu-c99a35ce.log: "Live UI smoke: overall=FAIL"). In Codex's own words (ui_live_smoke.py:1531): "A new daemon cannot certify the default last fifteen minutes."
   - Where the fix belongs: not in this unit's src/ alone.
     - The server must not move an explicit `from`. That rule is in the brief and spec, and rows 23 and 24c expect `incomplete` for a window that starts before the capture.
     - Option (a), web: anchor the live/default window at max(now − span, latest boot's S). The server already reports S as `info.from_ns` (checked: daemon_clock, from_ns == fixture S). Caveat: the web holds ns values as JS doubles, so clamping to exactly S can land up to 128 ns below it, which reads as incomplete [retention]. The web would have to round up to the next microsecond.
     - Option (b), server: a spec amendment adding a server-resolved live span or a "since capture start" left clip, a counterpart of A3's live_edge, so the server picks [max(now − span, S), now).
     - Either way, someone has to decide whether the default live view stops at the current boot after a restart. That decision goes to the advisers (RULE 1).
     - The `retention` label on [boot_start, S) of a boot's first file is a separate model/spec gap: rule 4 says "retention" where nothing was removed.
   - Shipping this unit without that change needs an explicit owner exclusion (RULE 2).

2. **Nit: a rotation race can produce one false `trace_unreadable`** (server.c:2920-2941 `cov_scan_file`). Read from source, not reproduced.
   - The meta count is read before the reader opens the file.
   - Rotation renames current.trace, unlinks the meta, and the next write creates a new current.trace (event_writer.c:1768-1783).
   - If the server reads the old meta (N blocks) and then opens the new file (1 block), N > num_blocks marks the file unreadable. Every non-info route refuses for that one request.
   - The window is microseconds, once an hour, and the next refresh clears it.

## Design questions for the owner
- B1 needs a decision before merge: (a) a web unit that clamps the live/default window to the latest boot's S, landed with or before this unit; (b) a spec amendment for a server-resolved live span or left clip; or (c) an owner exclusion. Advisers first (RULE 1).
