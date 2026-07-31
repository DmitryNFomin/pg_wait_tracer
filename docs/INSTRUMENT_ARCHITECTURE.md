# pg_wait_tracer — Instrument architecture: camera + strip cache (adopted UI direction)

_Adopted 2026-07-31, at v0.13. Evidence base: library source reads (TradingView
lightweight-charts v5.2.0, Perfetto, speedscope, Chrome DevTools, Firefox
Profiler), a Node gesture benchmark run on the repo's vendored ECharts bundle,
and measurements reproduced against the repo's own `pgwt-server` binary; backend
claims cite file:line. Companion: `docs/VISUALIZATION_REVIEW.md` (amended by §6
below)._

---

## 1. The camera concept (primer)

The best way in is Google Maps. Dragging or zooming the map does not ask
Google's server to "re-render the map for the new position." The **world** (the
planet's tiles) conceptually exists as one continuous thing; the client holds a
**camera** — a tiny piece of local state saying *"I am looking at this
rectangle, at this zoom level."* Dragging mutates that state instantly and
redraws whatever tiles are already downloaded through the new position. Zooming
into an area with no sharp tiles shows **stretched blurry tiles immediately** —
which sharpen a moment later as fine tiles arrive. Movement is local; detail
arrival is background. The user is never waiting on the network to move.

Mapped onto this tool, the world is the trace timeline — one continuous ribbon
of time, in nanoseconds, from trace start to NOW:

```
world (the whole trace):
├──────────────────────────────────────────────────────────────┤
09:00                                                        NOW

camera (what's on screen):            ┌────────────┐
                                      │ viewStart… │
                                      │ …viewEnd   │
                                      └────────────┘
                                      14:32      14:47
```

The camera is literally two numbers — `viewStart_ns` and `viewEnd_ns`.
Everything else is arithmetic:

**Drawing** — every frame, for each cached AAS bucket at time `t`:

```
x_px = (t - viewStart) / (viewEnd - viewStart) * chartWidth
```

One multiply-divide per point; a full AAS strip is ~36k floats — sub-millisecond.

**Pan** (drag by `dx` pixels):

```
shift = dx * (viewEnd - viewStart) / chartWidth
viewStart -= shift;  viewEnd -= shift        // done; redraw
```

**Cursor-anchored zoom** (wheel at pixel `px` — the "TradingView 10 lines"):

```
t_anchor  = viewStart + (px / width) * span   // timestamp under the cursor
span      = span / zoomFactor                 // shrink (or grow) the window
viewStart = t_anchor - (px / width) * span    // keep that timestamp pinned
viewEnd   = viewStart + span
```

The moment under the mouse stays pinned under the mouse while everything
expands around it — that is why trading-chart zoom feels "physical."

**Brush-select** is the same formula run backwards (pixels → time) — which
`web/static/lib/selection.js` already implements; it is the one piece of camera
math the client already has.

**Why it changes the feel.** Today, "where am I looking" does not exist in the
client at all — it exists only as the `from/to` parameters of the last server
request, so the only way to look elsewhere is to ask the server, wait, and
rebuild the chart. With a camera, "where am I looking" is client state mutated
at input speed; the redraw uses whatever strips are cached. Zooming past cached
resolution draws the coarse strip stretched (instant, slightly chunky) while the
client asks the server in the background for the same window at finer
resolution; when the strip lands, the picture sharpens in place. The strip cache
is the tile store; **the C server is already the tile generator** — its
`{from,to,buckets}` interface at arbitrary resolution is exactly the tile
protocol, measured at 3–22 ms per strip (§3).

**Live mode is a camera mode.** *Follow* = the right edge is glued to NOW and
slides forward each tick (the project's live-means-NOW rule, structurally
enforced). Any pan/zoom **detaches** the camera — it stops re-anchoring, so a
live tick can never yank the view out from under an investigation. The Live
button re-attaches. Two states, explicit transitions, testable.

**How deep zoom goes** — the answer comes from the capture tier, not the UI:

| Zoom depth | What is shown | Why |
|---|---|---|
| 15 min window | AAS stacked area, ~300 buckets of 3 s | any tier |
| 10 s window | ~5 ms buckets, honest | exact-tier events carry ns timestamps; bucketing does per-event interval clipping, honest at any width, no minimum (§3 row 3) |
| 1 s window | ~0.5 ms buckets — sub-millisecond | same; small windows hold few events, strips stay fast |
| below ~1 ms buckets | still valid, but the natural representation changes | "average active sessions" degenerates to a 0/1 square wave per backend; individual wait intervals (the timeline/Gantt surface) become the right rendering — the Perfetto pattern: aggregated when zoomed out, individual slices when zoomed in; the same camera drives both |

Two honest boundaries, stated rather than hidden: **sampled-only regions have a
hard floor at the sample period (~100 ms default)** — zooming below it must show
"sampled — resolution floor," never fabricated smoothness; in `--mode full` (or
inside escalation windows) data is ns-precise and the floor effectively
disappears. And a numeric footnote: ns epochs exceed float64 precision (~256 ns
ulp at current epochs), so the camera rebases time to the trace origin
internally.

**Testing.** The camera is a pure function of (state, gesture) → state — no DOM,
no canvas, no server. Cursor-anchored zoom and the follow/detached transitions
are tested with `node --test` exactly like the existing pure builders.

---

## 2. Dashboard vs instrument — the paradigm distinction

There are two architectures, and they are fundamentally different at the
source-code level, not just the UX level.

**The DASHBOARD model (request/response repaint).** The viewport is a *query
parameter*. Changing it means asking the server a new question and repainting
the answer. This is what pg_wait_tracer does today, and it is what Grafana
(zoom-triggers-requery, open since 2017, issue #10144), ClickHouse's built-in
dashboard (destroy-and-recreate uPlot per refresh, by deliberate choice),
Datadog, Oracle EM ASH Analytics, and RDS Performance Insights all do.

**The INSTRUMENT model (camera over cached multi-resolution data).** The
viewport is *client state* — two scalars in TradingView's engine (`barSpacing` +
`rightOffset`), a `HighPrecisionTimeSpan` in Perfetto. Input events mutate the
camera synchronously; every animation frame renders whatever data is locally
held through a cheap world→screen transform; data arrival is a separate,
asynchronous, eventually-consistent pipeline that *catches up to the camera*.
The server is never in the pan/zoom loop.

**The wheel-zoom, step by step, in each:**

*pg_wait_tracer today (dashboard):* there is no wheel-zoom; the only gesture is
drag-select. Mouseup → pixel→time (`web/static/lib/selection.js:85-93`) →
`zoomTo` → **two round trips, serialized client-side by sequential `await`s**
(`app.js:314-317`: AAS refresh, then active-tab refresh; the two zoom requests
never queue concurrently at the server — the strictly-FIFO stdin loop,
`src/server.c:3300-3313`, adds head-of-line blocking only when *other* traffic
such as status polls or auto-refresh interleaves). Responses travel uncompressed
over SSH (`web/bridge.go:48-70,124-137`). **Nothing paints until the response
lands** — the old frame just sits there — then a full `setOption(option, true)`
notMerge replace (`views/active.js:87`). Zoom-out pops a history stack and
refetches from scratch (`lib/state.js:77-88`). There is no client-side data
cache at all. Each gesture is a transaction; over WAN, each costs 2×RTT + FIFO
exposure + repaint.

*TradingView (instrument, verified in source):* wheel event → `zoom(zoomPoint,
scale)` mutates `barSpacing`, then corrects `rightOffset` so **the bar under the
cursor stays pinned under the cursor** (10 lines of math in `time-scale.ts`) →
an invalidation mask coalesces with any others into the next
`requestAnimationFrame` → repaint from local arrays in <16 ms. Zero network.
History refills in the background when the camera drifts within ~10 bars of the
loaded edge. Live data arrives as `update(bar)` appends; full-data replacement
is documented as the anti-pattern.

*Perfetto (instrument, out-of-core — the closer analog, because its data also
lives in a query engine):* camera moves at 60fps; the **previous query's data is
retained and rendered under the new transform** (stretched stale pixels),
truly-unknown regions get a "Loading" checkerboard, a cancellable query keyed by
`(start, end, power-of-2 resolution)` with a 3×-viewport prefetch skirt refines
the frame one query later. Zoom is never gated on a query.

**Which one does the product vision require?** "Select arbitrary windows, brush
regions, drill down" — the dashboard model does all of that; the tool does most
of it today. "**Zoom in/out smoothly**" — taken literally as continuous
cursor-anchored zoom rather than select-and-wait, requires at minimum the
*camera half* of the instrument: data resident client-side, viewport as client
state, server demoted to background refiner. It does **not** automatically
require the full instrument (bespoke 60fps typed-array renderer, kinetic scroll,
per-pixel hit-testing).

**And the incumbents? They are dashboards, both of them.** OEM ASH Analytics is
drag-the-selection-window-edges → "the expanded graph will refresh"; OCI
Performance Hub exposes resolution as a manual Low/Medium/High reload knob; no
wheel-zoom or continuous pan anywhere in the docs. RDS PI is drag-select →
release → refetch at a server-chosen ~100–200 points (and AWS is sunsetting the
PI console in July 2026). **Matching OEM/PI therefore requires nothing** — the
tool already roughly matches their interaction model. The instrument is how to
*beat* them; and since the dissatisfaction driving this document is precisely
with the model the incumbents use, matching them is not the target. One
calibration: at today's scale (~300 buckets × ~18 series ≈ 5k points) rendering
technology is irrelevant — but at instrument-grade preload (2000+ buckets) the
gesture benchmark (§4a) shows **ECharts' pipeline, not the canvas, becomes the
budget item**. The felt problem reads as a renderer problem; the evidence says
it is primarily a *data-residency and interaction-loop* problem, with a real,
measured pipeline-cost ceiling on how far ECharts can carry the fix.

---

## 3. What the instrument model requires — checklist vs. what already exists

The striking result of the backend audit (measured rows reproduced against the
`pgwt-server` binary): **the server is already most of an instrument backend.**
It is a deterministic, resolution-flexible, random-access strip server. The
missing half is almost entirely client-side.

| # | Requirement (distilled from TradingView/Perfetto/Firefox-Profiler) | Status in pg_wait_tracer |
|---|---|---|
| 1 | **Camera object** — time window as synchronously-mutated client state; input never touches the network | **Missing.** Zoom is a transaction (`app.js:504-514`); zoom history is a 10-deep refetch stack (`lib/state.js:69-88`) |
| 2 | **Client-resident strip cache** (the working dataset in memory) | **Missing entirely** — no data cache exists anywhere in the client |
| 3 | **Arbitrary-resolution strip queries** to fill the cache | **Exists.** `{from,to,buckets}` with no min bucket width and no cap (`src/server.c:298-308`, `src/compute.c:358-375`); exact area-integral bucketing (per-event interval clipping, `compute.c:429-457`), honest at any zoom |
| 4 | **Cheap enough to serve fine strips** | **Exists, measured**: 2000-bucket strip ~18–22 ms, 300-bucket zoomed strip ~3–4 ms (two independent traces agree). The only pain is wire format: ~333–359 B/bucket verbose JSON (`server.c:1762-1784`), ~666–681 KB for 2000 buckets, **no compression anywhere** (`web/bridge.go:48-71,124-137`; no `ssh -C`); gzip measures at 4–5× |
| 5 | **LOD ladder with honest floors** (Perfetto's power-of-2 resolution keys) | **Half exists.** Server has honest fidelity floors per tier — exact = ns (`compute.c:370-373`), sampled = 100 ms default (`src/pg_wait_tracer.c:280`), summaries = 1 s (`compute.c:1466-1467`, tier selection `server.c:1586-1593`) — client-side power-of-2 quantization (the trick that makes caching work) is missing |
| 6 | **Cache-safe determinism** | **Exists de facto** — byte-identical replay reproduced over closed data — but **unstamped**: the wall re-anchoring hazard (newest file's mono→wall offset is canonical, `server.c:1168-1184`) and open escalation windows (`server.c:1070-1077`) can silently shift served history. Needs a closed-data watermark + clock-generation stamp; `handle_info` (`server.c:1660-1698`) carries neither today. Cheap addition |
| 7 | **Append path for the live edge** | Poll-and-replace today; `current.trace` re-read per request (`server.c:1395-1398`). For camera purposes a tail-strip refetch suffices at 5 s cadence; a true delta protocol only if live cadence drops to ~1 s |
| 8 | **Progressive refinement / stale retention** (Perfetto's defining trick) | **Missing, and actively prevented**: single-flight cancel (`lib/transport.js:95-111`) and the epoch chokepoint (`lib/view-manager.js:85-122`, discards at `:109,:117`) throw away stale-but-useful data — correct for the dashboard model, wrong for a camera |
| 9 | **Concurrent prefetch** | Client: possible via non-superseding `transport.send` (`transport.js:88-90`). Server: serial FIFO, no cancellation, superseded requests compute fully (`server.c:3300-3313`) — tolerable at 3–22 ms compute, a head-of-line risk under WAN prefetch traffic |
| 10 | **Cursor-anchored zoom math** | Exists in the vendored ECharts bundle, verified in source: `InsideZoomView.ts:110-118` rescales around the percent-point; anchor is the **live** cursor per wheel event (`RoamController.ts:240`); ~10 lines if bespoke |
| 11 | **Crosshair + hit-testing** | ECharts axisPointer covers bucket-level; and since 5.2.2, `series-line.triggerLineEvent:true` packs event data on the area polygon too (`LineView.ts:923-926`), so band-level click + `convertFromPixel` + a ~30-line cumulative-sum walk gives **band+bucket from any pixel inside ECharts**. What ECharts cannot do is invert to the contributing **raw sample/backend** — but only because it holds bucketed series: a *data-residency* gap, renderer-independent once raw samples are client-resident |
| 12 | **Overload refusal** | **Exists and is ahead of peers**: DUR-9 `window_too_large` structured refusal (`reject_overload`, `server.c:1640-1656`; ~44.74 M event bound from min(25% RAM, 2 GiB)/48 B at `server.c:708-719`, reproduced byte-for-byte with `PGWT_LOAD_MAX_EVENTS`) — an instrument must render this as a refusal region; the client currently drops it silently: command errors reject with `transport:false` (`transport.js:76-78`) and `onRequestError` only acts on `transport===true` (`app.js:243-246`); the string `window_too_large` appears nowhere in `web/static` (review P1) |
| 13 | **Honesty overlays as camera-transformable data** (sampled shading, escalation band, N-CPUs line) | Exist in builders (`lib/builders/fidelity.js`, `aas.js:113-120`) but ride `series[0]` markArea/markLine as renderer decorations — the exact P2 defect. Must become view-model geometry that any renderer transforms |

**Bottom line of the table:** three cheap protocol additions
(watermark/generation stamp, compression, a cache-fill channel), zero hard
server work, and one genuinely new client subsystem (camera + strip cache +
refine path). The dashboard feel is a client-architecture property, not a
backend limitation.

---

## 4. The three paths, costed for one maintainer

### (a) ECharts-max: preload fine strips + `dataZoom` inside + live tick

**The work (~3–7 days on top of the review's Phase 2 chassis):** switch the AAS
x-axis to a ms value axis pinned to `[from,to]`; preload a strip beyond the
visible window; enable `dataZoom:{type:'inside'}` with wheel+drag (cursor
anchoring verified correct in the vendored source; drag-pan is default-on),
`filterMode:'none'`; on gesture settle (debounced ~150 ms), refetch a finer
strip and swap it under the fixed axis, preserving dataZoom state; live tick
stays poll-and-replace at 5 s. Two non-items, verified: no throttle work is
needed (inside-zoom auto-throttle is 20 ms when chart `animation:false` — which
`builders/aas.js:124-131` already sets), and the gesture path never touches
option merge — inside zoom dispatches a `dataZoom` *action*.

**How close it gets:** instant wheel-zoom and drag-pan within the cached extent,
no blank waits, refinement arriving in compute(3–22 ms)+RTT. **This decisively
beats OEM/PI** and eliminates the specific dissatisfaction (server round-trip
inside the gesture).

**The measured ceiling.** Each gesture step runs ECharts' full pipeline —
`restoreData` → data processors (dataZoom filter **and** stack recompute) →
**coordinate-system recreation** → visual tasks → re-render of *all* series —
rather than a viewport transform. Benchmarked on the repo's vendored bundle
(Node SSR, stacked areas, `animation:false`, inside zoom, `filterMode:'none'`;
"action-only" = the JS pipeline floor beneath any paint):

| scale | action-only p50 / p95 per step |
|---|---|
| 300×18 (today) | 8.9 / 18.1 ms |
| 1200×18 (preload, low corner) | 20.4 / 32.8 ms |
| 2000×16 | 24–34 / 43–49 ms |
| 2400×18 (preload, high corner) | 42.5 / 59.5 ms |

Because `filterMode:'none'` filters nothing, **cost scales with the whole cached
strip, not the visible window** — preload size directly buys gesture cost. At
2000×16 this is ~20–30 fps territory; at the high corner 10–20 fps before any
painting. Caveat, honestly: Node SSR is not a browser (same V8, no GPU paint) —
treat these as a floor no renderer tuning removes. One documented limit stands
regardless: **there is no append path for stacked areas** — `appendData`
supports only scatter and `lines`-trajectory series, not even plain `line`
(official docs). Further consequences: no stale-tile rendering past the cached
edge (nothing in ECharts can draw there); no kinetic scroll (absent from
`RoamController.ts`); and with `filterMode:'none'` the y-extent derives from the
full cached strip, so **y never rescales to the visible window during zoom** —
aligned with P7 stability, but a product behavior to state, since
y-rescale-on-zoom would require a filtering mode that re-runs filtering per
step.

### (b) One instrument-grade AAS component; ECharts retained everywhere else

**Default implementation — uPlot substrate.** The case is strong:
`docs/ROADMAP_AND_STATUS.md:202` already pre-scoped "uPlot for AAS" as the
contained adapter-swap the builders/mount seam exists for; uPlot ships an
official 159-LOC `stack.js` recipe; Grafana — a funded team facing exactly this
problem — ships stacked time series on uPlot; and the bespoke path's hidden
weeks live precisely in time-axis ticks and input-device edge cases, which uPlot
provides proven. Cost: **~500–900 owned LOC, roughly 1–3 weeks** — stacking,
honesty overlays as draw-hook geometry, settle-refine, and band hit-testing are
owned work under either substrate.

**Fallback — fully bespoke canvas**, costed against measured analogs (Firefox
Profiler, speedscope, lightweight-charts):

| Subsystem | LOC estimate | Calibration (measured against the cited sources) |
|---|---|---|
| Canvas host (dpr, rAF, resize lifecycle) | ~150–300 | FF `Canvas.tsx` 564, 13 separate dpr touchpoints |
| Camera + TimeScale (world↔px, anchored zoom, clamps) | ~200 | FF `Viewport.tsx` 911 |
| Stacked-band renderer (cumulative Float32 columns) | ~200–350 | with hit-test row ≈ FF `ActivityGraphFills` 884 minus React/Flow discount |
| Input state machine, mouse-first (wheel norm., drag-pan, brush coexistence, dblclick) | ~300–500 | FF brush alone is a separate 507-LOC `Selection.tsx`; the repo's existing `selection.js` is 105 LOC — a modest head start |
| Touch/pinch (if in scope) | +400–800 | only touch-capable calibration in the corpus: lightweight-charts `mouse-event-handler.ts` 871 + kinetic 143; FF Viewport and speedscope contain **zero** touch code |
| Time-axis ticks/labels | ~250–450 *as an explicit descope* (single-TZ, non-calendar-grade) | no trace viewer does calendar time (FF `Ruler.tsx` 93, relative); production calendar axes run 3–5× (LWC 844+570+883) |
| Crosshair + readout + band hit-test | ~200–330 | speedscope pan-zoom view 873 total |
| Honesty overlays as camera-space geometry | ~150–250 | partly P2-owed regardless of renderer |
| Legend/theming/a11y/wheel-delta edge cases | ~200–400 | — |
| LOD strip cache + settle-refine | ~300 | **shared with (a) — already sunk by the time any trigger fires** |
| **Total** | **~2.5–4k LOC, ~5–8 weeks** full scope; **~1.8–2.5k, ~3–5 weeks** under the explicit mouse-first/single-TZ descope; incremental cost over (a) runs ~300–450 LOC below face value (shared cache + P2 overlap) | closest whole-scope analog: FF's 2,359 measured LOC covers camera+host+fills+hit-test *only* |

**What (b) buys over (a), either substrate:** a viewport transform instead of a
per-step pipeline (restacking 36k floats is sub-millisecond; the Firefox
activity graph — *literally an AAS-by-class chart* — recomputes per-pixel
stacking every frame), Perfetto-style stale-strip rendering with a loading
skirt, structural ownership of the honesty layer under every interaction, and a
clean surface for single-camera compare overlays. Per-pixel *sample*
attribution is **not** on this list — it flips on data residency, not renderer
(see trigger #2).

### (c) Full Perfetto-style canvas core for all time views

Replace AAS + session timeline + heatmap + concurrency + future
waterfall/matrix/scatter with one track engine under one camera. Calibrated
cost: DevTools' flame-chart widget alone ~5.5k LOC; Perfetto's track/timeline
engine ~10–13k atop its query engine. For one maintainer: **months**,
forfeiting the ECharts-shaped test capital across every view simultaneously,
discarding ECharts where it demonstrably earns its keep (unit-tested
custom-series Gantt, heatmap+visualMap), and requiring real server work
(cancellation/priority on the FIFO loop at `server.c:3300-3313`) to feed a
many-track camera. No surviving case: the vision is a single AAS surface +
drill (OEM-shaped), not multi-track sync. Defensible only on a product pivot to
a trace-viewer identity. **Rejected.**

---

## 5. Decision

**Build the instrument's data layer now, renderer-agnostic; drive it through
ECharts first; gate the substrate swap on a written measurement.** Commit to the
HYBRID *architecture* (camera + strip cache + refine protocol as pure modules
outside any renderer), ship path (a) as its first rendering backend — days, not
weeks — and hold path (b), **uPlot-substrate by default, bespoke canvas as
fallback**, as a pre-scoped replacement of one mount layer. Two priced caveats:

*First, not all path-(a) work survives a later swap.* The data layer, protocol
changes, and view-model-ification of the overlays survive any outcome; but in
(a), ECharts' inside-dataZoom owns gesture handling and viewport state, so the
renderer-agnostic camera is authoritative only by convention, mirrored via
events — if (b) triggers, the camera↔dataZoom sync glue and one round of AAS
pixel-test re-baselining are discarded. That is ~2–4 days, not zero, and the
camera module is only *proven* renderer-agnostic once a second backend exists.
Priced and accepted.

*Second, "may never be needed" would be too soft about (b).* The vendored-bundle
benchmark puts the pipeline floor at 20/33 ms (p50/p95) per step at even the low
preload corner — straddling or exceeding any honest smoothness bar. The expected
path is **(a), then (b)** — probably (b)-via-uPlot at 1–3 weeks — and saying so
plainly makes the gate a commitment device instead of a deferral. What (a)-first
buys is not avoidance of (b); it is the complaint-fix (server out of the
gesture) within days, a real-browser measurement instead of a guess, and a
shared data layer that makes (b) cheap.

**Triggers for (b):**

1. **Frame budget:** target is **p95 gesture-to-paint ≤ 16.7 ms** (the 60fps bar
   every studied instrument holds); p95 > 16.7 ms sustained after a *timeboxed*
   (≤3 days) tuning pass → schedule (b); p95 > 33 ms → begin (b) immediately.
   Measured via recorded gesture scripts in the fixture gallery at ≥1200 cached
   buckets × 18 series. If 30 fps is instead accepted as sufficient, that is a
   product decision to be written down as such — not hidden inside a threshold.
   (A >33 ms-only gate would allow permanent ~35 fps choppiness without ever
   tripping — hence the two-level rule.)
2. **Sample-level attribution** — "click this pixel → which backend/query
   produced it." Band+bucket drill works in ECharts today (`triggerLineEvent` +
   `convertFromPixel` + ~30 lines); this trigger fires only when **raw-sample**
   inversion is required, and what it actually demands is raw samples resident
   client-side — a data-residency milestone that then favors an owned renderer,
   not a renderer defect per se.
3. **Compare mode with single-camera diff composition** — two windows under one
   camera with diff shading. Compare itself is the review's scheduled
   destination (VISUALIZATION_REVIEW.md names it the largest capability gap),
   and a first-generation PI-style side-by-side is ECharts-feasible; the trigger
   is specifically the single-camera overlay design.
4. **Live cadence ≤1 s with smooth right-edge follow** — no stacked-area append
   path exists in ECharts (`appendData` covers only scatter/`lines`-trajectory),
   so this is a permanent `setOption` fight.

Trigger for (c): only a deliberate product pivot to multi-track synced-timeline
identity.

**The migration-safe first step:** implement `lib/camera.js` — TimeWindow +
cursor-anchored zoom math + power-of-2 resolution quantization + an explicit
follow/detached state machine with tested transitions (this project has shipped
real bugs against exactly this invariant — "last 15 min must always mean NOW";
the `has_closed_data` live-suppression fix — a pan must detach the camera so the
5 s tick never re-anchors the user, and follow mode must never show a stale
right edge) — pure, ~150–200 LOC, Node-tested exactly like `selection.js`. Plus
`lib/stripcache.js` (strips keyed by `{from,to,resolution,generation}`,
stale-but-useful retention). Plus three small backend items: closed-data
watermark + clock-generation stamp in responses (guards the `server.c:1168-1184`
re-anchoring hazard and the `server.c:1070-1077` escalation window;
`handle_info` at `server.c:1660-1698` carries neither), wire compression
(permessage-deflate in `bridge.go` or `ssh -C` — 4–5× measured), and a strip
channel through `transport.send` that bypasses the epoch-discard
(`view-manager.js:109`) for cache fills. None touch ECharts; all are testable in
the existing pure-module style; (a) and both (b) substrates consume them
unchanged.

**Non-negotiable guardrails:**

- **P1/P2 land first.** A buttery zoom over event colors that reshuffle every 5
  seconds is still "weird"; broken ≡ empty ≡ stale is still the worst defect in
  the codebase — including the confirmed silent swallow of `window_too_large`
  (`transport.js:76-78` → `app.js:243-246`). Camera-lite is judged only after
  identity and error visibility are fixed.
- **The honesty layer survives every renderer.** Sampled shading, escalation
  bands, the N-CPUs line, and DUR-9 refusals become camera-space view-model
  geometry (P2's annotation-series redesign executed with exactly this in mind),
  never renderer decorations. An instrument that smoothly zooms into fabricated
  confidence would betray the project's most differentiating property.
- **Test capital preserved by construction:** camera math (including
  follow/detached transitions), cache policy, and stacked geometry as pure
  Node-tested modules; gallery cells gain recorded gesture scripts — which is
  also where trigger #1's number lives, written down before any (b) work starts.

---

## 6. Amendments to docs/VISUALIZATION_REVIEW.md (amend, not restate)

The review is almost entirely upheld — with one correction: **it answered "is
ECharts the right library?" rigorously and never asked "is request/response the
right interaction model?"** The library verdict stands; the model verdict was
absent. Specific amendments:

1. **§1/§7 "Stay on ECharts. Full stop."** → "Stay on ECharts as the renderer,
   for now. The request/response model — not the library — is the gap between
   this tool and the stated vision; the escape hatch the review itself cites
   (builders emit view models, only mount speaks ECharts,
   `docs/ROADMAP_AND_STATUS.md:202`) is hereby promoted from escape hatch to
   load-bearing seam."
2. **§7 uPlot dismissal** — stands as a *wholesale-migration* verdict only. Two
   corrections: its scale framing ("polling ≤300 pre-aggregated buckets")
   described the dashboard status quo as a requirement, and uPlot is
   *reinstated as the default substrate for the single AAS instrument pane* —
   exactly the contained per-chart experiment `ROADMAP_AND_STATUS.md:202`
   pre-scoped (official 159-LOC stacking recipe; Grafana precedent).
3. **§8 "What NOT to do": "pursue appendData/streaming"** — keep, strengthened:
   `appendData` supports only scatter and `lines`-trajectory series (not even
   plain `line`), per official docs. Delete the implication that
   poll-and-replace is the terminal model. Add: *"do not put the server inside
   the pan/zoom gesture loop."*
4. **Phase 2 chassis** — the "one time-axis convention (ms coordinates)" item
   upgrades to: camera module (with follow/detached state) + ms value-axis
   pinned to `[from,to]` + strip cache + settle-refine. The P7 fix (yMax
   hysteresis, window quantization) merges with the camera's power-of-2
   quantization — the same phase-stability idea — and gains one documented
   consequence: under `filterMode:'none'`, y-extent is the full cached strip's,
   so zoom-into-a-quiet-region keeps global yMax by design. Camera-lite slots as
   **Phase 2a**; days, not weeks, once Phase 1's identity work is done.
5. **Transport (§2 praise of single-flight/epoch)** — still correct for
   dashboard panes; add the documented exception: the strip-cache channel
   retains superseded-but-useful data (`transport.js:95-111`,
   `view-manager.js:109` get a bypass, not a rewrite).
6. **New small server backlog:** closed-data watermark + clock-generation stamp
   (`server.c:1168-1184` hazard); wire compression (`bridge.go:48-71`).
   Explicitly deferred: server-side cancellation/priority on the FIFO loop
   (`server.c:3300-3313`) — compute is 3–22 ms measured; only needed if
   prefetch traffic materializes.
7. **§5 drill wiring** — unchanged, including the note that *session timeline*
   zoom stays refetch-based (server-side coalescing makes refetch correct
   there). The instrument model applies to the AAS strip surface; do not
   generalize by reflex.
8. **§8 "Acknowledge and park"** gains one entry: instrument-grade AAS pane —
   **default uPlot substrate ~500–900 owned LOC / ~1–3 weeks; bespoke-canvas
   fallback ~2.5–4k LOC / ~5–8 weeks full scope, or ~1.8–2.5k / ~3–5 weeks
   under an explicit mouse-first single-TZ descope** (calibration: FF Viewport
   911 + Canvas 564 + ActivityGraphFills 884 + Selection 507; speedscope
   pan-zoom 873; the only touch-capable calibration is lightweight-charts'
   871-LOC input handler); incremental cost over path (a) runs ~300–450 LOC
   below face value (shared strip cache, P2-owed overlay work). Gated on the
   four triggers in §5; full Perfetto-style track engine rejected absent a
   product pivot. Compare-mode design note: two cameras over one strip cache is
   substantially cheaper than two dashboards.
9. **§6 gallery** — add recorded-gesture replay to the live-tick replay item;
   it doubles as trigger #1's measurement harness, with the Node-SSR floor
   numbers from §4a (8.9→42.5 ms p50 per step, 300→2400 buckets) recorded as
   the pre-registered baseline.

---

## 7. Verdict

Trading platforms are built on a fundamentally different model, and it is now
pinned down precisely: a camera over client-resident multi-resolution data, with
the server demoted to background refiner — not merely a better renderer. The
incumbents to surpass are dashboards, so the instrument is how to beat them, not
the price of entry. The backend is, measurably and somewhat accidentally,
already an instrument's backend (`server.c:298-308`, `compute.c:358-375`,
`server.c:1640-1656`); the client is 0% of one. The wrong response is a renderer
rewrite; the right response is to build the camera (with live-follow semantics)
and cache as renderer-neutral modules, let ECharts draw them first, fix identity
and failure-visibility regardless — those defects survive every paradigm — and
hold a pre-scoped substrate swap behind a written 16.7 ms gate. The measured
ECharts pipeline floor means that gate should be *expected* to trip at
instrument-grade preload, so the honest plan is not "canvas in a drawer,
probably never" but "(a) first, (b)-via-uPlot when the written number says so —
likely soon after."
