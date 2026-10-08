# Finding: the live-UI-smoke self-test fails intermittently, ~6 % of runs

**Not caused by this branch.** Nothing here is fixed, hardened, widened or
added to `KNOWN_FAILING` — this file is the evidence for an issue someone
else should own.

## What happened

`box-check` run 1 on this branch (gate-1, PG 18, 2026-10-08 09:05, log
`../box-check-ubuntu-20261008-090532.log`) came back `Executed: 98 (passed 97,
failed 1)`. The single failure was `tests/ui_live_smoke.sh`'s **self-test**
(issue #224 bypass suite / #252 review round 1 finding 3) — the step that
injects a synthetic 400 ms blink and confirms the blink gate still catches it:

    SELF-TEST FAIL: the injected blink was NOT caught and the tab verdict was
    True (self_test={'tick': 2, 'ratio': 0.0, 'measured': True, 'note': None,
    'inject_blink_ms': 400, 'caught': False})

The real walk that precedes it passed in full: **11/11 tabs**, `overall=PASS`,
`failed_tabs: []` (`../ui_live/summary.json`), including Waterfall with 101
executions and Scatter with 2000 points under the new three-tier load.

## It pre-dates this branch

Searched every `box-check-*.log` on this Mac that actually ran the self-test
(`grep -q "self-test -- injecting"`), across all worktrees:

| | count |
|---|---|
| box-check runs that ran the self-test | **63** |
| of those, self-test passed | 59 |
| of those, self-test FAILED | **4** (6.3 %) |

The four: `top-queries-qsort` 2026-10-01 22:41, `agent-aa38ac88aa3aff30a`
2026-10-02 14:21, `agent-a4378baf8c0cc60db` 2026-10-07 18:24, and this one.
The 2026-10-01 log carries a **byte-identical** signature — same `tick: 2`,
same `ratio: 0.0`, same `measured: True`, same `note: None`, same
`caught: False` — a week before any commit on this branch.

(The 2026-10-07 one failed differently: `--inject-tick=2 was never reached on
tab 'sessions'`. Two distinct failure modes of the same step.)

## What the run-1 record shows, and what it rules out

`summary.json` in this directory, tab `sessions`, `blink_sweep`:

| tick | achieved offsets (ms) | ratios |
|---|---|---|
| 1 | 215, 508, 1010, 1514, 2010 | 0, 0, 0, 0 |
| 2 (inject) | 214, 512, 1011, 1514, 2009 | 0, 0, 0, 0 |
| 3 | 211, 513, 1010, 1509, 2007 | 0, 0, 0, 0 |
| 4 | 274, 547, 1012, 1510, 2008 | 0, 0, 0, 0 |
| 5 | 275, 569, 1009, 1526, 2008 | 0, 0, 0, 0 |
| 6 | 284, 619, 1008, 1516, 2034 | 0, 0, 0, 0 |

Targets are 200/500/1000/1500/2000 ms and the achieved offsets track them to
within ~20 ms on the inject tick. `frame_spacing` `ok: true`.

So **drift is ruled out** — the frame nominally inside the overlay's window
(the injection runs `mount+700` .. `mount+1100`) was taken at offset 1011.

What is left, and what an owner of this should look at first: `capture_ms` on
that tick is 232-451 ms per frame. `_capture_at_offset` records `achieved` from
a `Date.now()` read taken BEFORE `page.screenshot`, so the frame nominally "at
1011 ms" has its pixels grabbed somewhere in 1011..1243 — straddling the
overlay's 1100 ms removal. With a 400 ms overlay and a ~240 ms capture, exactly
one sweep frame can see it and only if its grab lands in the first ~90 ms of
its own capture window. That is a coin flip, and a ~6 % loss rate is the shape
one would expect.

A contributing factor worth measuring separately: this tab's
`panel_dims.box_height` is **4001 px** (148 session rows), and screenshot cost
scales with it. Session-row count is load-dependent, so the odds may not be
constant across workloads. I did not measure that and am not claiming it.

## What was deliberately NOT done

- No retry, no widened tolerance, no `KNOWN_FAILING` entry (CLAUDE.md: that
  table is only for a filed product bug being fixed in `src/`; timing
  intermittency gets investigated or moved, never listed).
- No change to `tests/ui_live_smoke.py`, the injection window, the capture
  path, `BLINK_THRESHOLD` or the self-test's verdict. Making this gate pass by
  loosening it is the one outcome that would be worse than the flake.
- box-check was re-run once to obtain gate evidence. BOTH runs are reported;
  the re-run is not a fix and does not close this finding.
