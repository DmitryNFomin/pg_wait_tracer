# The visual checklist — seven words, keyed to gallery cells

_Phase U1 (Track U). Source vocabulary: `docs/VISUALIZATION_REVIEW.md` §4 ("the
'looks weird' decoder"). This document makes each word operational: what it
means, exactly which fixture-gallery cell exercises it, and the confirmed
historical instances (P-numbers cite the review's findings)._

**The gallery:** `web/static/dev/gallery.html` — every pure builder × curated
deterministic states, rendered through the real builders and the app's real
`echarts.init(el,'dark')` + `setOption(option, true)` path. Open it with
`python3 tests/mock_server.py` → `http://127.0.0.1:18765/dev/gallery.html`
(or `python3 -m http.server 8000 --directory web/static`). No WebSocket needed.
Cell ids are `gallery-<builder>-<state>`; every cell's header shows its
manifest id (`<builder>/<state>`) and its checklist tags. Cells with ▶ replay a
recorded live-tick sequence (1 s/tick; ⏮/⏭ step deterministically; scripts use
`window.__gallery.setTick('<cellId>', n)`).

When something looks wrong, do not say "looks weird" — name the word it
violates, name the cell, and file it with the template at the end.

---

## STABILITY

**The same identity keeps the same color, position, and selection across
refresh, sort, and drill.** An event that was pink stays pink; the series you
soloed stays soloed; the axis does not breathe when the data has not changed.

**Check it:** step `gallery-aas-live-ticks` with ⏮/⏭ (or
`window.__gallery.setTick('gallery-aas-live-ticks', n)`). The recorded ticks
re-rank the server-side top-8 every tick — `Lock:transactionid` surges from
outside the top-8 to rank 1 while `IO:DataFileRead` declines. Watch one event
across ticks: its color and its position in the stack must never change; when
an event enters or leaves the top-N, the others must not reshuffle. Cross-check
`gallery-aas-dense-events` (16 events at the top-N cap): every event's tint
must belong to its class hue, identically to the same event anywhere else in
the app.

**Historical instances:** index-keyed event colors and stack order (P2);
index-keyed legend Set soloing the wrong series after a drill (P2); yMax
breathing and bucket phase shimmer (P7); heatmap global recolor per tick (P8);
DFG dragged positions discarded (P5); dropdown clobbered mid-selection (P8).

## CONTINUITY

**Tick N → N+1 is a smooth update, never a teardown.** No flash to blank, no
replayed draw-in animation, no lost tooltip or hover state on a live refresh.

**Check it:** press ▶ on `gallery-aas-live-ticks` and let it loop. Each tick
goes through the exact live-refresh path (`setOption(option, true)` on a
persistent chart instance). Hover the chart while it plays: the frame must
update under a stationary cursor without the chart blinking or your axis
pointer vanishing. Any first-paint look on tick 2+ is a CONTINUITY violation.

**Historical instances:** dispose/re-init flash and tooltip loss on the live
timeline (P5); slider-drag rebuild storms on the DFG (P5); mid-hover legend
rebuild canceling an in-progress hover-solo (P5).

## ALIGNMENT

**Every mark sits where its data says, inside the axes.** Markers in the
containing bucket, bands at their recorded bounds, bars clipped to the plot.

**Check it:**
- `gallery-concurrency-burst-final-bucket` — the only burst is inside the LAST
  bucket; the red triangle belongs at the far right. (Before U0 it clamped to
  bucket 0 — the far left.) `gallery-concurrency-dense-bursts` repeats it at
  300-bucket scale, plus early and mid-storm bursts.
- `gallery-timeline-pre-window-start` — waits that began before the window
  clamp to the left plot edge; nothing may paint over the "PID n" axis labels.
  Tooltips still show the raw (pre-window) start.
- `gallery-aas-escalated-live-edge` — the escalation edge line must actually
  render, pinned at the axis edge (it had NEVER rendered before U0).
- `gallery-aas-mixed-escalation` — amber bands exactly over the
  `fidelity_ranges` sampled sub-ranges; the escalation band starts at the
  observed start, not at the window start.
- `gallery-fidelity-annotation-observed-anomaly` — band-track card: band and
  edge at the model's coordinates.

**Historical instances:** burst triangle at bucket 0 for last-bucket bursts
(P6); escalation markLine placed past axis max, never painted (P2); timeline
bars over the PID labels (P6); the 160 px sticky-column slot (P11).

## OCCLUSION

**Nothing overlaps or clips at dense data.** Labels stay legible, marks stay
distinguishable, at the fixture's worst case — not just at demo densities.

**Check it:** `gallery-timeline-dense-50pids` (50 rows × 8 bars: row labels
readable, bars within bands); `gallery-transitions-dense-dfg` (18 nodes /
34 links: node labels must not fuse into an unreadable pile);
`gallery-aas-dense` (300 buckets, 11 classes: axis labels hide-overlap, the
Lock storm at buckets 140–169 clearly visible); `gallery-histogram-dense`
(full 60×16 grid).

**Historical instances:** DFG pixel-baked layout bunching after resize (P5);
no `hideOverlap` outside aas.js; same-class stacked-bar segments fusing into
one indistinguishable block (P11).

## SEMANTICS

**An encoding means the same thing everywhere.** A hue that means IO in one
chart may not mean "hot" in another; a pixel proportion that means share of
time must actually be that share.

**Check it:** `gallery-histogram-one-hot-cell` (one 50 000-count cell over a
≤16 floor: if every other cell collapses into the two darkest stops, the ramp
is hiding the distribution — and its rainbow stops currently reuse class hues
as intensity); `gallery-aas-dense-events` and `gallery-aas-unicode-names`
(event tints must read as their class); `gallery-timeline-micro-vs-macro`
(a 5 µs and a 5 ms wait both render ≥1 px — the floor must not make µs read as
ms); `gallery-transitions-variants-html` and
`gallery-table-configs-queries-hostile-sql` (stacked segment widths: min-width
clamps silently renormalize proportions, and sub-1% segments must not lie).

**Historical instances:** rainbow heatmap ramp reusing class hues as intensity
(P8); EVENT_PALETTE hues colliding with class meanings (P2); "Other"
conflating skipped-known with genuinely-other; 1 px floor equalizing µs ≡ ms;
min-width renormalization breaking proportion=value (P11).

## HIERARCHY

**Visual weight matches analytical importance.** The anomaly is the loudest
thing on screen; reference chrome never dominates data.

**Check it:** `gallery-aas-one-huge-value` (a 480-AAS Lock spike on a 64-vCPU
host: is the spike the loudest thing, or does the red "64 CPUs" line flatten
the data to a sliver?); `gallery-concurrency-one-huge-peak` (same question for
the 400-session peak); `gallery-transitions-threshold-simplified` (at 25%
threshold only the dominant edges survive — do the surviving nodes read as the
important ones?); `gallery-fidelity-metrics-escalated` (warn rows visibly
louder than healthy rows).

**Historical instances:** a reference line dominating a flattened data sliver
(P7); the honesty shading faintest during busy periods; error states carrying
zero visual weight (P1).

## FEEDBACK

**Every failure / empty / stale / truncated state is visually distinct and
labeled.** Broken must never be pixel-identical to idle; refusals must be
worded; truncation must be announced.

**Check it:** the `empty` cell of every builder (`gallery-aas-empty`,
`gallery-timeline-empty`, `gallery-histogram-empty`,
`gallery-transitions-empty`, `gallery-concurrency-empty`) — each must say so
in words. `gallery-aas-all-zero` vs `gallery-aas-empty`: an idle-but-captured
window is NOT the same state as no data. `gallery-fidelity-unavailable-*`
(the server's structured refusal, with and without the escalate affordance);
`gallery-fidelity-annotation-mid-escalation` (unknown start → edge line only,
no fabricated band — UI-10); `gallery-timeline-dense-50pids` (truncation
stated: 400 of 1200); `gallery-table-configs-events-null-latency` (null pct /
null percentiles render "—", never 0). The `injection`-tagged cells
(`gallery-aas-hostile-names`, `gallery-timeline-hostile-sql`,
`gallery-transitions-hostile-names`, `gallery-concurrency-hostile-event-names`,
`gallery-table-configs-queries-hostile-sql`) are FEEDBACK's security twin:
hostile strings must render inert as text.

**Historical instances:** all of P1 (silent swallow sites, unrendered
`window_too_large`); silent table truncation (P10); the fidelity chip
describing shading that is no longer on screen (P2).

---

## First catch: stacked AAS layers are not painted at all (found building U1)

A worked example of the vocabulary doing its job. Every `aas` gallery cell
shows it, most plainly `gallery-aas-dense` and `gallery-aas-dense-events`:
**only the first series' area renders; every stacked layer above it is
invisible.** The committed production baseline
`tests/web_snapshots/aas_chart_overview.png` pins the same defect — a single
green CPU sawtooth peaking at 2.4 while the mock's `max_aas` is 4.5 with IO /
LWLock / Timeout / Extension all non-zero. Named properly: an **ALIGNMENT**
violation (marks not where the data says — the layers are missing), with a
**SEMANTICS** consequence (the chart reads "CPU-only load" during a mixed
storm).

Root cause (SSR bisect + real-Chromium canvas verification against the
vendored 5.5.1 bundle, 2026-07-31): ECharts' stacked-area rendering drops
every layer above `series[0]` whenever stacked 2D data rides a `type:'value'`
x-axis — at ANY x magnitude (reproduced at t0=0, t0=1000, and full ns/ms/s
epochs alike; the layer-2 polygon computes at astronomically wrong pixel
coordinates and is discarded). Only a `type:'time'` x-axis is demonstrated
sane, even with raw ns values. Consequence for the U2 chassis item "ms time
coordinates at the builder boundary": neither ms conversion NOR
window-relative offsets fix this — the AAS time axis MUST become
`type:'time'` (binding constraint for the U2a camera wiring). Owned by the
U2a/chassis work, not by the gallery.

---

## Reporting a violation

Use the vocabulary, the cell id, and (for replay cells) the tick:

```
<WORD> violation, cell <builder>/<state>[, tick N]
saw:      <one sentence — what the pixels show>
expected: <one sentence — what the fixture data says should show>
```

Example: `STABILITY violation, cell aas/live-ticks, tick 4 —
saw: Lock:transactionid turns from red to gold and jumps two stack positions.
expected: same tint and stack slot as tick 3; only its height changes.`

Deterministic repro is free: fixtures contain no wall-clock and no RNG, so a
cell id (+ tick) IS the reproduction. Gallery-cell snapshots pin these states
in CI with tight thresholds; a violation fixed without a fixture that shows it
stays fixed only by accident.
