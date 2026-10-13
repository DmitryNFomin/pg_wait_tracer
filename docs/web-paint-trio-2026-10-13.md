# Live-view paint investigation: #197, #160, #172

Investigation against `6bfe7d3`, 2026-10-13. No product patch and no issue
closure are justified by the evidence collected here. This is the safe,
partial result of the optional three-issue contract.

## Current mechanisms

- `refresh()` already calls `runConcurrentRefresh()` for the AAS pane and
  `ViewManager.refresh()` (merged as `898a85c`, #251). The sequential AAS
  round trip described in #197's original body is no longer present.
- `createTimelineView()`'s `ensureShell()` preserves the chart host, instance,
  and selection overlay; `mount()` updates its option. `buildTimelineOption()`
  sets `animation: false`. Dense spans already use `aggregateTimelineColumns()`.
- `createTransitionsView()`'s `requests()` fetches only `transitions`.
  `loadVariants()` is user initiated (merged as `f6da230`, #320).
  `renderDFG()` preserves the chart instance and `buildTransitionsOption()`
  sets `animation: false`. The two sequential requests diagnosed in #172's
  earlier comments no longer describe this implementation.
- `buildQueriesModel()` uses the pure `buildTableModel()`; `mountTable()`
  replaces the table synchronously. An `innerHTML` assignment alone is not
  evidence of a separately painted blank frame. Replacing this shared table
  component speculatively would expand the risk beyond the reported symptom.

No shared **remaining product** root cause was established. The issues' own
comments identify historical measurement defects; they also explicitly warn
that a green mount-anchored sweep alone cannot rule out an earlier transient.

## Recovered historical evidence

The original `agent/diag-walk-239` artifacts remain in the local worktree
`/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/diag-walk-239/tests/results/demo_rehearsal/`.
Its checkout HEAD is `ee0cbed4031814568d6f75a08a3e7c4fccb0c26f` and `run.id`
is `1790685360`. This identifies the retained checkout and run; it does not
prove the exact dirty tree or browser version at capture time.

[The machine-readable extraction](web-paint-trio-2026-10-13.json) records the
source summary/video SHA-256 values, measured counts, per-tick final sweep
offsets, and the exact video samples inspected. Queries and Transitions each
have 18/18 measured ticks across early/middle/late, with worst consecutive
sweep ratio **0.0** against the unchanged **0.001** threshold. These are
historical measurements from an **uncounted** diagnostic run, not a new
before/after test of this branch.

The retained early videos are 1280 x 900 at 25 fps. Inspecting Queries at
12 s and 18 s shows the same leading query identities but changed data:
the `END;` row's DB Time changes from **2740.9s to 2789.5s** and Execs
from **9.7K to 9.8K**. Transitions at 12 s and 16.8 s shows CPU time
**49.5s to 49.9s**, with total transitions **577,607 to 581,465**; the
graph remains present at the inspected 15, 16.8, 17.2, and 17.6 s samples.
Thus these sampled differences contain legitimate data updates. They do
**not** identify the original failing frame pair, prove that every frame
around a mount is stable, or exclude a flash between sampled frames.

Frames can be reproduced with `ffmpeg -ss <seconds> -i <video> -frames:v 1
<local-output.png>`. Decoded WebM pixels were also compared using
`frame_diff_ratio()` without a pixel tolerance. Codec changes affected even
flat backgrounds, so those video diffs cannot be substituted for the gate's
lossless screenshot ratios. No threshold, tolerance, retry, or sleep was
added to a test; extracted PNGs stay local.

Timeline's historical final sweep offsets range **2006-2021 ms**, compared
with Queries' **2006-2025 ms** and Transitions' **2006-2070 ms**. This is
measured harness offset data, **not paint latency**. `ViewManager.refresh()`
records `lastMount` after `mount()` returns, before a browser paint is
guaranteed. A before/after Timeline paint delta is **unmeasured**, as are
before/after product blink ratios, because this branch applies no product fix.

## Remaining work

#160 and #172 remain open: inspect the actual original failing pair with
mount/request timing, and confirm on a counted attempt at the demo viewport.
The recovered run's 1280 x 900 frames do not provide that confirmation.
#197 remains open: measure the actual demo client's mount and paint timing.
Its Safari comment predates the Chrome switch recorded in
`DEMO_REHEARSAL_CRITERIA.md`; the current demo pin is 1710 x 981 at DPR 2.
Neither the old VM sweep nor a mock browser run would establish that result.

No `web/` file changed, so `make ui-gallery` and cell justifications are not
applicable. No snapshot PNGs were committed; no `make box-check`, push, or
pull request was performed.
