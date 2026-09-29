# Mac-side demo walk — manual sign-off

Owner decision, 2026-09-29: walk this by hand in real Chrome on the Mac's
built-in display. Fill in this copy and sign it. The VM capture verdict does
not establish the Mac-side result. A walk against a different tree or tag is
a different experiment and does not count toward the same sequence.

Use `docs/DEMO_REHEARSAL_CRITERIA.md` for the attribution and attempt rules.
No numeric time-to-first-paint bound is set here; it must be pre-registered
separately from the uncontended dry run before counted attempt one.

## 1. Pre-flight

- [ ] Use the demo topology: real Chrome and the Go bridge on the Mac, ssh to
  the VM running the daemon and PostgreSQL.
- [ ] Use the Mac's built-in screen as **Main Display**. Detach the second
  display. Record the display identity below.
- [ ] Chrome is full screen at 100% page zoom. After the full-screen animation
  settles, read `innerWidth`, `innerHeight`, and `devicePixelRatio` twice.
  Both readings must agree: **1710 x 981 CSS at DPR 2**. A transient viewport
  reading previously matched the old Safari pin and was wrong for Chrome.
- [ ] No implementing agents and no local VM work run for the whole walk.
- [ ] Record `memory_pressure` and load at the start. Pressure must be normal.

## 2. Walk the eleven tabs in demo order

Keep the live workload running. Visit each row below in order. For **each**
tab, check for console errors, blank panels, data present, and paint without
a visible spinner. Record each result in the table. A visible defect is a
failure; there is no known-failing-tab exemption.

Reach **Timeline through a Sessions-row drill-down**. A bare Timeline tab
click shows a "select a session" prompt. The earlier 30 s Timeline paint
finding timed that prompt and is **retracted**; it did not time a chart.
After Timeline, perform step 3 before continuing to Transitions.

| Order | Tab | No console errors | No blank panel | Data present | Paints without visible spinner | Pass / fail; note |
|---:|---|---|---|---|---|---|
| 1 | Overview | ___ | ___ | ___ | ___ | ___ |
| 2 | Events | ___ | ___ | ___ | ___ | ___ |
| 3 | Sessions | ___ | ___ | ___ | ___ | ___ |
| 4 | Queries | ___ | ___ | ___ | ___ | ___ |
| 5 | Histogram | ___ | ___ | ___ | ___ | ___ |
| 6 | Timeline (Sessions-row drill-down) | ___ | ___ | ___ | ___ | ___ |
| 7 | Transitions | ___ | ___ | ___ | ___ | ___ |
| 8 | Concurrency | ___ | ___ | ___ | ___ | ___ |
| 9 | Waterfall | ___ | ___ | ___ | ___ | ___ |
| 10 | Scatter | ___ | ___ | ___ | ___ | ___ |
| 11 | Matrix | ___ | ___ | ___ | ___ | ___ |

## 3. Bridge-drop recovery

1. [ ] Drop the bridge mid-walk while the Chrome tab remains open.
2. [ ] Bring the bridge back.
3. [ ] Observe whether the UI reconnects without a reload, rather than
   remaining on an error chip and blank panels. Record elapsed recovery time
   and any lost state. Source inspection's 16 s backoff cap is not evidence
   of recovery.

Resume the tab walk at Transitions after the bridge recovers.

## 4. Freshness

- [ ] Watch successive live ticks. The view visibly advances; a frozen chart
  is a failure even if it is neither blank nor throwing.
- [ ] At the end of the walk, each time-axis tab's newest bucket is within
  two ticks of wall clock, as required by the criteria. Record any exception.

## 5. Close-out

- [ ] Record `memory_pressure` and load at the end. Pressure must be normal.
- [ ] Record every tick that looked slow, with `memory_pressure` and load
  observed at that tick. A tick with non-normal pressure is **void**, not a
  failure. An over-bound tick with normal pressure and a clean VM-side tick
  is a client-side product finding; both sides red is a product failure.
- [ ] Fill in every result and sign and date this walk. Only a signed-off
  manual walk can advance "Mac-side clean m of M".

## Filled-in result

Date and time of walk: ____________________

Tree / tag walked (commit if applicable): ____________________

VM / capture attempt ID and capture-side result: ____________________

Chrome version: ____________________    macOS version: ____________________

Built-in display identity / Main Display confirmed: ____________________

Viewport reading 1 (CSS width x height, DPR): ____________________

Viewport reading 2 (CSS width x height, DPR): ____________________

Start `memory_pressure` / load: ____________________

End `memory_pressure` / load: ____________________

| Section | Pass / fail / void | Evidence or failure reason |
|---|---|---|
| 1. Pre-flight | ___ | ___ |
| 2. Eleven-tab walk | ___ | ___ |
| 3. Bridge-drop recovery | ___ | ___ |
| 4. Freshness | ___ | ___ |
| 5. Close-out | ___ | ___ |

Freshness observations / exceptions: ____________________

Bridge recovery observed? ______  Elapsed time: ______  State lost: ______

Slow ticks (tab, time, observed `memory_pressure` / load, VM-side result,
finding or void reason):

________________________________________________________________________

________________________________________________________________________

Other failures / notes: __________________________________________________

Overall Mac-side result (clean / failed / void): ____________________

Human signature: ____________________    Sign-off date: ____________________
