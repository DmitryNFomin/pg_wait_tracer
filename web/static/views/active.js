/* pgwt — "active" view: the Average Active Sessions chart.
 *
 * This is the persistent top chart (Oracle ASH-style AAS). It sits above the
 * tabs, does not tear down on tab switches, and follows the
 * { id, requests, build, mount, enter, leave } contract; app.js drives it
 * directly via build()/mount() on each refresh.
 *
 * ── U2b: the renderer seam (ECharts -> uPlot) ────────────────────────────────
 * The measured ECharts gesture pipeline floor made 60 fps impossible at
 * instrument preload (real-Chromium gesture-to-paint p95 = 61.2/59.4/67.6 ms
 * at the 1258-bucket x 16-series strip, tests/results/gesture_gate.json — the
 * docs/INSTRUMENT_ARCHITECTURE.md §5 gate tripped RED -> path (b)). The AAS
 * pane's DEFAULT renderer is now uPlot (lib/uplot-aas.js + the vendored
 * bundle): a camera move is ONE u.setScale('x', {min,max}) viewport transform
 * over resident data — no option pipeline, no percent round-trips.
 *
 *   seam        ?renderer=echarts|uplot (default uplot). The ECharts path is
 *               kept fully intact as the A/B baseline and the rollback; it is
 *               a harmless view choice, so no loopback gating (contrast the
 *               ?ws= override). Shared between renderers: requests(), the
 *               strip cache preload, the camera, the external name-keyed
 *               legend chips (U1), the fidelity chip, the U0 error/empty
 *               states, and lib/selection.js brush-select.
 *   camera      GENUINELY AUTHORITATIVE under uPlot (this closes U2a's
 *               "authoritative by convention" caveat): OUR handlers mutate
 *               the camera (wheel = cursor-anchored zoomAt, shift+drag =
 *               panByFrac, dblclick = app zoom-out, plain drag = brush) and
 *               the renderer draws FROM camera state via an rAF-coalesced
 *               setScale. uPlot's own gesture model is disabled
 *               (cursor.drag off in the spec; its dblclick autoscale unbound
 *               here) — there is no library gesture state left to mirror.
 *   data        strip cache unchanged (U2a): mount() preloads/refines the
 *               camera's quantized 3x strip; renderFromCamera() paints
 *               synchronously from whatever the cache holds. Under uPlot a
 *               strip swap is u.setData(aligned, false) — the `false` is
 *               load-bearing (never re-ranges x; the camera window is the x
 *               scale) — followed by a setScale to the camera window. A
 *               camera window wider than the cached strip just renders the
 *               strip at its true position (Maps-style stretch via scales).
 *   honesty     the sampled/mixed shading, escalation band + live edge, and
 *               N-CPUs line paint from overlayGeometry via draw hooks that
 *               recompute against the CURRENT x scale on every draw — the
 *               trust marks exist in every camera state (bands clip, lines
 *               drop but never move into view; constraint D).
 *   live        follow mode stays poll-and-replace at 5 s; a followTick
 *               slides the viewport via the same rAF setScale path, and the
 *               tick's mount doubles as the live-edge strip refresh.
 *
 * ── U2a (kept verbatim for ?renderer=echarts) ───────────────────────────────
 * The ECharts path still mirrors inside-dataZoom percent events into the
 * camera (authoritative by convention) and repaints through full setOption —
 * see the U2a notes inline in the echarts-only sections below.
 */

import { buildAasOption } from '../lib/builders/aas.js';
import {
    buildUplotSpec, overlayGeometry, overlayHooks, hitTest,
    compareHooks, drawDiffStrip,
} from '../lib/uplot-aas.js';
import { shiftQuantized, shiftWindow } from '../lib/camera.js';
import { fullyCoversStrip } from '../lib/stripcache.js';
import { CancelledError } from '../lib/transport.js';
import {
    SAMPLED_BAND_COLOR, MIXED_BAND_COLOR, SAMPLED_BORDER, MIXED_BORDER,
} from '../lib/builders/fidelity.js';
import { attachSelection } from '../lib/selection.js';
import { esc, fmtDuration, fmtTime } from '../lib/format.js';

const NS_PER_MS = 1e6;

/* Wheel-zoom shaping: one full wheel notch (deltaY = ±100 in pixel mode)
 * zooms 1.25x; trackpad micro-deltas scale proportionally through the pow.
 * Per-event factor is clamped so a hostile/broken deltaY can never teleport
 * the window. Line-mode deltas (Firefox deltaMode 1) are ~3 lines/notch. */
const WHEEL_NOTCH_FACTOR = 1.25;
const WHEEL_FACTOR_MIN = 0.5;
const WHEEL_FACTOR_MAX = 2;

/* ── Click→drill gesture discrimination (U2, review P3 wire 1) ───────────────
 * A drill CLICK must not fire for (a) a brush drag — the selection overlay
 * owns plain drags, and the browser still emits `click` after one — or (b)
 * the first click of the dblclick zoom-out. The heuristic, documented here
 * because it IS the gesture-language boundary:
 *   movement   pointerdown→click displacement > CLICK_SLOP_PX (the selection
 *              overlay's own MIN_DRAG_PX threshold) ⇒ it was a drag: ignore.
 *   delay      the drill fires only after DRILL_CLICK_DELAY_MS with no second
 *              click; a second click inside the window cancels it and lets
 *              the app's dblclick zoom-out win. The 550 ms window covers
 *              Blink's 500 ms double-click interval; the pane's actual
 *              `dblclick` event ALSO cancels pending work, so correctness is
 *              event-driven rather than only a timer guess.
 *   modifiers  shift belongs to pan, ctrl/meta to legend multi-select: any
 *              modifier ⇒ not a drill. Button 0 only.
 * The hit is resolved at CLICK time (coordinates are only valid then); only
 * the ACTION is deferred. */
const CLICK_SLOP_PX = 5;
const DRILL_CLICK_DELAY_MS = 550;

/* Tiny shared gate used by both renderer paths. onClick resolves the hit
 * immediately but defers the ACTION; cancel() clears a pending action
 * (leave()). Any click within the delay window of the previous one counts
 * as the dblclick's second half — even if the first click resolved to
 * nothing (empty area) — so a zoom-out can never be chased by a drill. */
function makeClickGate() {
    let down = null;
    let timer = null;
    let lastClickTs = 0;
    return {
        onPointerDown(e) {
            down = (e.button === 0 && !e.shiftKey && !e.ctrlKey && !e.metaKey)
                ? { x: e.clientX, y: e.clientY } : null;
        },
        onClick(e, resolve) {
            if (!down) return;
            const moved = Math.abs(e.clientX - down.x) > CLICK_SLOP_PX ||
                          Math.abs(e.clientY - down.y) > CLICK_SLOP_PX;
            down = null;
            if (moved) return;                       // drag: the brush owns it
            const now = Date.now();
            const second = (now - lastClickTs) < DRILL_CLICK_DELAY_MS;
            lastClickTs = now;
            if (second) {                            // dblclick pair:
                if (timer) { clearTimeout(timer); timer = null; }
                return;                              // zoom-out owns it
            }
            const action = resolve();                // hit-test NOW
            if (!action) return;
            timer = setTimeout(() => { timer = null; action(); },
                DRILL_CLICK_DELAY_MS);
        },
        cancel() {
            if (timer) { clearTimeout(timer); timer = null; }
            down = null;
        },
    };
}

/* The uPlot renderer's URL seam. Reading location is guarded so the module
 * stays importable under Node (the pure builders it re-exports are
 * Node-tested elsewhere; this view itself is browser-only). */
function resolveRenderer() {
    try {
        if (typeof location !== 'undefined' && location.search) {
            const p = new URLSearchParams(location.search).get('renderer');
            if (p === 'echarts' || p === 'uplot') return p;
        }
    } catch (e) { /* hostile URL / no DOM: fall through to the default */ }
    return 'uplot';
}

export function createActiveView() {
    const renderer = resolveRenderer();

    // ── Shared state ─────────────────────────────────────────────────────────
    let el = null;             // chart container (#aas-chart-container)
    // Legend state, keyed by series NAME (U1, review P2 — an index Set turned
    // a CPU solo into an arbitrary-event solo once the server re-ranked the
    // event series). `selected` = visible names; `hovered` = the name being
    // hover-soloed (re-applied after a mid-hover live tick); `names` = the
    // last rendered series set, so a set change resets the selection.
    const legend = { selected: null, hovered: null, names: null };
    const resetLegend = () => {
        legend.selected = null; legend.hovered = null; legend.names = null;
    };

    // ── yMax hysteresis (U2, review P7 — THE view-level home) ───────────────
    // The builders compute the pure per-payload policy yMax; smoothing it
    // over time is view state, so it lives here (both renderers). Rule:
    // GROW immediately (a spike must never clip), SHRINK only after
    // Y_SHRINK_HOLD_TICKS consecutive data mounts whose policy yMax stayed
    // below the applied one — then adopt the latest target. "Tick" means ONLY
    // a successful 5 s live poll; deliberate window/filter changes reset the
    // state, and provisional strip-cache repaints neither seed nor advance it.
    // "stable" = the shrink demand persisted, not that the target was
    // byte-identical (live data wobbles; demanding equality would pin the
    // axis high forever). Reset on empty state and on leave().
    const Y_SHRINK_HOLD_TICKS = 3;
    const yHyst = { applied: null, run: 0 };
    function applyYMaxHysteresis(target) {
        if (yHyst.applied == null || target >= yHyst.applied) {
            yHyst.applied = target;
            yHyst.run = 0;
        } else if (++yHyst.run >= Y_SHRINK_HOLD_TICKS) {
            yHyst.applied = target;
            yHyst.run = 0;
        }
        return yHyst.applied;
    }
    function resetYMaxHysteresis() { yHyst.applied = null; yHyst.run = 0; }
    function yMaxForMount(target, ctx) {
        const reason = ctx && ctx.aasMountReason;
        if (reason === 'live-tick') return applyYMaxHysteresis(target);
        resetYMaxHysteresis();
        if (reason === 'provisional') return target;
        return applyYMaxHysteresis(target);
    }

    // ── Click→drill resolution (U2, review P3 wire 1) ────────────────────────
    // Shared by both renderers once a click resolves to a hitTest result over
    // a spec-shaped surface ({seriesNames, seriesIds, breakdown}):
    //   class mode  → drill into the clicked class (filterKey 'class', value =
    //                 the class label — the same intent the overview table
    //                 emits);
    //   event mode  → drill into the clicked event by event_id (the events-
    //                 table filter); a payload without ids (old server) does
    //                 not drill — never guess an id.
    // The returned closure is what the click gate defers behind the dblclick
    // window; ctx.onDrill pauses live + pivots (app.js PIVOT/FilterStack).
    function drillActionForHit(hit, spec, ctx) {
        if (!hit || !ctx.onDrill) return null;
        if (spec.breakdown === 'events') {
            const id = spec.seriesIds ? spec.seriesIds[hit.seriesIdx - 1] : null;
            if (id == null) return null;
            return () => ctx.onDrill({
                filterKey: 'event_id', filterValue: id, label: hit.seriesName,
            });
        }
        return () => ctx.onDrill({
            filterKey: 'class', filterValue: hit.seriesName,
            label: hit.seriesName,
        });
    }

    // Click→drill gate (U2, P3 wire 1) — one instance, whichever renderer is
    // active owns it; see makeClickGate for the documented heuristic.
    const drillGate = makeClickGate();

    // ── ECharts-only state ───────────────────────────────────────────────────
    let chart = null;          // ECharts instance — owned here, nowhere else
    let detachSel = null;      // selection-overlay teardown (both renderers)
    // U2a: the ns extent of the last mounted option's pinned time axis.
    // 'datazoom' events carry start/end PERCENTS of exactly this extent.
    let renderedAxis = null;
    // U2 (P3 wire 1): the last mounted ECharts payload — the click walk
    // rebuilds a spec-shaped surface from it (see onEchartsDrillClick).
    let echartsLatest = null;  // { data, opts, model }
    let echartsCtx = null;     // ctx captured at enter (drill wiring)

    // ── uPlot-only state ─────────────────────────────────────────────────────
    let UPlotCtor = null;      // the vendored constructor (ctx.uplot)
    let u = null;              // uPlot instance — owned here, nowhere else
    let uHost = null;          // inner host div (el keeps its padding box)
    let readoutEl = null;      // compact crosshair readout card
    let diffCanvas = null;     // D2 signed A-B lane below the uPlot pane
    let compareNoteEl = null;  // quiet B-loading/failure/renderer honesty note
    let baselineFailed = false; // persists across provisional A-only repaints
    // The latest painted state: { data, opts, spec }. The overlay draw hooks,
    // the hit-test readout, and the legend restack all read THIS — it is the
    // single source for "what is on the canvas right now".
    let latest = null;
    let hiddenKey = null;      // '\u0000'-joined hidden names (restack dedup)
    let paintedYMax = null;    // y scale top currently applied to the instance
    let enterCtx = null;       // ctx captured at enter (camera/gesture wiring)
    let unsubCamera = null;
    let rafId = null;
    let panning = null;        // {x, width} during a shift-drag pan

    // One resolution knob for the poll fetch AND the camera quantization, so
    // strip resolution tracks the pane width exactly like the poll does.
    function targetBuckets() {
        return Math.min(Math.floor((el ? el.clientWidth : 800) / 4), 300);
    }

    function baselinePredates(ctx) {
        if (!ctx.compare || !ctx.compare.enabled) return false;
        const b = shiftWindow({ from: ctx.timeRange.from, to: ctx.timeRange.to },
            ctx.compare.offsetNs);
        return b.from < ctx.server.fromNs;
    }

    /* Fire-and-forget preload/refine of the camera's quantized 3x strip.
     * ensure() never rejects and dedups by strip key, so calling this on
     * every paint is one Map lookup when warm. Arrivals repaint through the
     * app's stripCache.onData subscription. */
    function ensureStrip(ctx) {
        if (!ctx.camera || !ctx.stripCache) return;
        const q = ctx.camera.quantize(targetBuckets());
        // U2a review F2: in follow mode the window slides every 5 s tick, so
        // the quantized strip KEY changes every tick and a naive ensure()
        // refetches the full 3x strip each time (~4-6x the AAS wire bytes,
        // continuously). Perfetto-style refill instead: keep serving the
        // cached strip while it still covers the window at adequate
        // resolution, and refetch only when the window drifts within a
        // quarter-span of the strip's right edge (or resolution degrades
        // beyond one power of two). Detached-mode gestures still refine
        // immediately (their windows change key only when the user moves).
        const have = ctx.stripCache.get(q);
        if (have && have.exact) {
            // A can be warm while a newly-enabled B is not.
        } else if (have &&
            have.resNs <= q.resNs * 2 &&
            have.stripFrom <= q.winFromNs &&
            have.stripTo >= q.winToNs &&
            (have.stripTo - q.winToNs) > (q.winToNs - q.winFromNs) / 4) {
            // still covered, far from edge
        } else {
            ctx.stripCache.ensure(q);
        }
        if (ctx.compare && ctx.compare.enabled && !baselinePredates(ctx)) {
            ctx.stripCache.ensure(shiftQuantized(q, ctx.compare.offsetNs));
        }
    }

    /* Builder opts shared by both renderers: the current window, the camera's
     * quantized strip extent (axis skirt), and the daemon escalation status. */
    function builderOpts(ctx) {
        const opts = {
            numCpus: ctx.server.numCpus,
            win: { from: ctx.timeRange.from, to: ctx.timeRange.to },
            escalationStatus: ctx.getEscalationStatus ? ctx.getEscalationStatus() : null,
        };
        if (ctx.camera) {
            const q = ctx.camera.quantize(targetBuckets());
            opts.axis = { from: q.stripFrom, to: q.stripTo };
        }
        return opts;
    }

    /* Status line: window + peak AAS (identical text for both renderers). */
    function setStatusLine(ctx, model) {
        if (!ctx.setStatus) return;
        const dur = fmtDuration(ctx.timeRange.span());
        const peak = (model.maxAas || 0).toFixed(1);
        ctx.setStatus(ctx.server.numCpus + ' CPUs · ' + dur +
            ' window · peak ' + peak + ' AAS', 'connected');
    }

    function setEmptyStatus(ctx) {
        if (!ctx.setStatus) return;
        ctx.setStatus(ctx.server.numCpus + ' CPUs · ' +
            fmtDuration(ctx.timeRange.span()) + ' window · no data',
            'connected');
    }

    function clearLegendAndChip() {
        const legDiv = document.getElementById('aas-legend');
        if (legDiv) legDiv.innerHTML = '';
        const chip = document.getElementById('aas-fidelity-chip');
        if (chip) chip.remove();
        resetLegend();
    }

    function renderCompareNote(model, ctx) {
        const notes = [];
        if (ctx && ctx.compare && ctx.compare.enabled && renderer === 'echarts') {
            notes.push('Compare visuals require the uPlot renderer');
        }
        if (model && model.baselineUnavailable) {
            notes.push('Baseline unavailable; showing A only');
        } else if (model && model.baselineLoading) {
            notes.push('Baseline loading; showing A only');
        }
        if (!notes.length) {
            if (compareNoteEl) compareNoteEl.style.display = 'none';
            return;
        }
        if (!compareNoteEl) {
            compareNoteEl = document.createElement('div');
            compareNoteEl.className = 'aas-compare-note compare-note';
            el.appendChild(compareNoteEl);
        }
        compareNoteEl.textContent = notes.join(' · ');
        compareNoteEl.style.display = 'block';
    }

    function rememberBaselineState(model, ctx) {
        if (!ctx || !ctx.compare || !ctx.compare.enabled ||
            (model && (model.baselinePredates || model.compareData))) {
            baselineFailed = false;
        } else if (model && model.baselineUnavailable) {
            baselineFailed = true;
        }
        // baselineLoading is provisional: it must not erase a known failure.
    }

    // ── uPlot mount path ─────────────────────────────────────────────────────

    /* The x scale target: the camera window when available (the camera is the
     * viewport), else the spec's own window. Always in axis ms. */
    function viewportScale(ctx, spec) {
        const cam = ctx && ctx.camera;
        if (cam && cam.toNs > cam.fromNs) {
            return { min: cam.fromNs / NS_PER_MS, max: cam.toNs / NS_PER_MS };
        }
        return spec.xWindow;
    }

    /* rAF-coalesced viewport repaint: any number of camera events inside one
     * frame collapse into ONE setScale over resident data — the whole point
     * of the substrate swap (no fetch, no rebuild in the gesture loop). */
    function scheduleViewportPaint() {
        if (!u || rafId != null) return;
        rafId = requestAnimationFrame(() => {
            rafId = null;
            paintViewport();
        });
    }

    function paintViewport() {
        if (!u || !enterCtx || !enterCtx.camera) return;
        const cam = enterCtx.camera;
        if (!(cam.toNs > cam.fromNs)) return;
        u.setScale('x', { min: cam.fromNs / NS_PER_MS,
                          max: cam.toNs / NS_PER_MS });
        if (latest) drawDiffStrip(diffCanvas, latest.spec.compare,
            { min: cam.fromNs / NS_PER_MS, max: cam.toNs / NS_PER_MS },
            u.bbox);
    }

    function measureUplot() {
        // uHost is a block child inside el's padding box, so clientWidth is
        // the content width; el's min-height (CSS) / explicit height (the
        // resize handle) give the pane height.
        const w = uHost ? uHost.clientWidth : 800;
        let h = uHost ? uHost.clientHeight : (el ? el.clientHeight : 300);
        if (!(h > 0)) h = 300;
        return { width: Math.max(200, w || 800), height: Math.max(120, h) };
    }

    /* Current-view overlay geometry for the draw hooks: recomputed from the
     * latest painted payload against the CURRENT x scale on every draw, so
     * the honesty overlays track every camera state AND every data swap.
     * The CURRENT y scale top rides along (P7): an N-CPUs reference above it
     * becomes the explicit top-edge "N CPUs ↑" affordance — decided against
     * the hysteresis-applied scale, so the reference line reappears the
     * instant the axis actually accommodates it. */
    function overlayGeo(uu) {
        if (!latest) return { rects: [], vlines: [], hlines: [] };
        return overlayGeometry(latest.data, latest.opts,
            { min: uu.scales.x.min, max: uu.scales.x.max,
              yMax: uu.scales.y.max });
    }

    function compareGeo() {
        return latest && latest.spec ? latest.spec.compare : null;
    }

    function paintDiff(ctx, spec) {
        if (!diffCanvas) return;
        const active = !!(ctx && ctx.compare && ctx.compare.enabled);
        diffCanvas.style.display = active ? 'block' : 'none';
        if (el) el.classList.toggle('compare-active', active);
        if (!active) return;
        drawDiffStrip(diffCanvas, spec && spec.compare,
            viewportScale(ctx, spec || { xWindow: { min: 0, max: 1 } }),
            u && u.bbox);
    }

    /* Construct a fresh instance from a spec (initial mount + the rare
     * series-set-change path: class<->event flips, top-N membership shifts —
     * never inside a gesture). */
    function rebuildUplot(spec, ctx) {
        if (!UPlotCtor) {
            // Deployment bug (vendor script missing): thrown from mount, so
            // the P1 error-card path names it — never a silent blank pane.
            throw new Error('uPlot vendor bundle not loaded (vendor/uPlot.iife.min.js)');
        }
        if (u) { u.destroy(); u = null; }
        if (!uHost) return;
        uHost.textContent = '';            // drop a previous empty-state card
        const o = spec.uplotOpts;
        const size = measureUplot();
        o.width = size.width;              // mount contract: real pixels here
        o.height = size.height;
        // Honesty overlays (rects behind the series, lines above) + the
        // crosshair readout, merged before construction per the module
        // contract.
        const honestyHooks = overlayHooks(overlayGeo);
        const ghostHooks = compareHooks(compareGeo);
        o.hooks = {
            drawAxes: (honestyHooks.drawAxes || []).concat(ghostHooks.drawAxes || []),
            draw: honestyHooks.draw || [],
            setCursor: [(uu) => updateReadout(uu)],
        };
        // uPlot's dblclick handler autoscales x to the full data extent —
        // that would fight the app's dblclick zoom-out (initChartView), so
        // unbind it. The spec already disables cursor drag select/zoom;
        // uPlot merges cursor opts deeply, so only dblclick is overridden.
        o.cursor = Object.assign({}, o.cursor,
            { bind: { dblclick: () => null } });
        // P7 hysteresis: the constructed instance pins y to the APPLIED top
        // (>= the spec's policy value while a shrink is being held).
        const displayYMax = yMaxForMount(spec.yMax, ctx);
        o.scales = Object.assign({}, o.scales,
            { y: Object.assign({}, o.scales.y, { range: [0, displayYMax] }) });
        u = new UPlotCtor(o, spec.alignedData, uHost);
        u.setScale('x', viewportScale(ctx, spec));
        paintedYMax = displayYMax;
        paintDiff(ctx, spec);
    }

    /* Restack + repaint for a visibility change (legend chips). Follows the
     * module's legend-toggle contract: rebuild the spec with hiddenNames
     * (hidden series keep their slot -> uPlot indices stay stable), then
     * setSeries/delBand/addBand/setData(…, false) in one batch. */
    function applyUplotVisibility(visible) {
        if (!u || !latest) return;
        const names = latest.spec.seriesNames;
        const hidden = names.filter(n => !visible.has(n));
        const key = hidden.join('\u0000');
        if (key === hiddenKey) return;     // no-op guard (every mount ends
        hiddenKey = key;                   // with a trailing apply)
        const spec = buildUplotSpec(latest.data,
            Object.assign({}, latest.opts, { hiddenNames: hidden }));
        latest.spec = spec;
        u.batch(() => {
            u.delBand(null);
            spec.bands.forEach(b => u.addBand({ series: b.series.slice() }));
            names.forEach((n, i) => u.setSeries(i + 1, { show: visible.has(n) }, false));
            u.setData(spec.alignedData, false);   // false: x window stays put
            u.setScale('x', viewportScale(enterCtx || {}, spec));
        });
        paintDiff(enterCtx, spec);
    }

    function uplotEmptyState(ctx, model) {
        // UI-5: an empty window must not leave the PREVIOUS window's paint on
        // screen while the tables say "No data" — same contract as the
        // ECharts branch, uPlot form: destroy the instance, say so.
        if (u) { u.destroy(); u = null; }
        latest = null;
        hiddenKey = null;
        paintedYMax = null;
        resetYMaxHysteresis();   // P7: a fresh window starts a fresh axis
        if (readoutEl) readoutEl.style.display = 'none';
        paintDiff(ctx, { compare: null, xWindow: { min: 0, max: 1 } });
        if (uHost) {
            uHost.innerHTML =
                '<div class="aas-empty">No data in selected range</div>';
        }
        clearLegendAndChip();
        if (ctx.setCompareEvidence) {
            ctx.setCompareEvidence(model && model.isCompare ? model.data : null,
                (model && model.compareData) || null,
                !!(model && model.baselinePredates),
                !!(model && model.baselineUnavailable));
        }
        setEmptyStatus(ctx);
    }

    function mountUplot(model, ctx) {
        if (!uHost) return;                 // disposed
        rememberBaselineState(model, ctx);
        ensureStrip(ctx);
        renderCompareNote(model, ctx);
        if (!model.hasData) { uplotEmptyState(ctx, model); return; }

        const spec0 = model.spec;
        const names = spec0.seriesNames;

        // Visibility must be decided BEFORE painting (the stack itself
        // changes with hidden series). Same U1 reset rule renderLegend
        // enforces: a series-set change resets the selection.
        const sameSet = !!legend.names && legend.names.length === names.length &&
            legend.names.every((n, i) => n === names[i]);
        const visible = (sameSet && legend.selected)
            ? legend.selected : new Set(names);
        const hidden = names.filter(n => !visible.has(n));
        const spec = hidden.length
            ? buildUplotSpec(model.data,
                Object.assign({}, model.opts, { hiddenNames: hidden }))
            : spec0;

        const setChanged = !u || !latest ||
            latest.spec.seriesNames.length !== names.length ||
            latest.spec.seriesNames.some((n, i) => n !== names[i]) ||
            latest.spec.seriesColors.some((c, i) => c !== spec.seriesColors[i]);

        latest = { data: model.data, opts: model.opts, spec };
        hiddenKey = hidden.join('\u0000');

        if (setChanged) {
            // Rare, non-gesture path (live top-N churn / drill mode flip).
            rebuildUplot(spec, ctx);
        } else {
            // Data swap under a live instance (5 s tick, settle-refine strip
            // swap, stripcache arrival): setData(…, false) never re-ranges x;
            // the camera window is re-asserted explicitly. yMax is pinned via
            // an explicit y setScale when the APPLIED scale top moved —
            // spec.yMax first passes the P7 view-level hysteresis (grow now,
            // shrink after 3 held ticks), so live wobble never breathes the
            // axis (verified against the pinned bundle: explicit min/max
            // land exactly — with data present they bypass the range fn).
            const displayYMax = yMaxForMount(spec.yMax, ctx);
            u.batch(() => {
                u.delBand(null);
                spec.bands.forEach(b => u.addBand({ series: b.series.slice() }));
                names.forEach((n, i) =>
                    u.setSeries(i + 1, { show: visible.has(n) }, false));
                u.setData(spec.alignedData, false);
                if (displayYMax !== paintedYMax) {
                    u.setScale('y', { min: 0, max: displayYMax });
                    paintedYMax = displayYMax;
                }
                u.setScale('x', viewportScale(ctx, spec));
            });
        }

        renderLegend(spec, applyUplotVisibility, ctx, legend);
        renderFidelityChip(spec, ctx);
        paintDiff(ctx, spec);
        if (ctx.setCompareEvidence) {
            ctx.setCompareEvidence(model.isCompare ? model.data : null,
                model.compareData || null, !!model.baselinePredates,
                !!model.baselineUnavailable);
        }
        setStatusLine(ctx, spec);
    }

    /* Compact crosshair readout: bucket time (UTC) + band under the cursor +
     * its unstacked value + the stack total — the essentials of the ECharts
     * axis tooltip, resolved via the pure hitTest over the painted spec. */
    function updateReadout(uu) {
        if (!readoutEl || !el) return;
        const c = uu.cursor;
        if (!latest || c.left == null || c.left < 0 ||
            c.top == null || c.top < 0) {
            readoutEl.style.display = 'none';
            return;
        }
        const xMs = uu.posToVal(c.left, 'x');
        const yAas = uu.posToVal(c.top, 'y');
        const hit = hitTest(latest.spec, xMs, yAas);
        if (!hit) { readoutEl.style.display = 'none'; return; }
        const spec = latest.spec;
        const t = fmtTime(spec.alignedData[0][hit.bucketIdx] * NS_PER_MS,
                          spec.bucketNs);
        const val = spec.rawValues[hit.seriesIdx - 1][hit.bucketIdx] || 0;
        const topIdx = spec.stackIdxs.length
            ? spec.stackIdxs[spec.stackIdxs.length - 1] : 0;
        const total = topIdx
            ? (spec.alignedData[topIdx][hit.bucketIdx] || 0) : 0;
        const color = spec.seriesColors[hit.seriesIdx - 1];
        // Series names are server/extension-derived (UI-6) — esc() them; the
        // color comes from our own identity service (rgb() only).
        readoutEl.innerHTML =
            '<b>' + esc(t) + '</b> UTC · ' +
            '<span style="color:' + color + '">●</span> ' +
            esc(hit.seriesName) + ' <b>' + val.toFixed(2) + '</b>' +
            ' · total <b>' + total.toFixed(2) + '</b>';
        readoutEl.style.display = 'block';
        const overR = uu.over.getBoundingClientRect();
        const hostR = el.getBoundingClientRect();
        let lx = overR.left - hostR.left + c.left + 14;
        const ly = overR.top - hostR.top + c.top + 14;
        const maxX = el.clientWidth - readoutEl.offsetWidth - 6;
        if (lx > maxX) lx = Math.max(0, maxX);
        readoutEl.style.left = lx + 'px';
        readoutEl.style.top = ly + 'px';
    }

    // ── uPlot gesture handlers (the camera loop) ─────────────────────────────

    /* Wheel = cursor-anchored zoom: fracX from the cursor over the PLOT area
     * (u.over), factor from deltaY. Camera-only — the repaint rides the
     * camera subscription's rAF setScale; app.js's onChange pauses live and
     * schedules the settle-refine (existing U0/U2a machinery). */
    function onWheel(e) {
        if (!u || !enterCtx || !enterCtx.camera) return;
        e.preventDefault();                 // the pane owns the wheel
        const r = u.over.getBoundingClientRect();
        if (!(r.width > 0)) return;
        const frac = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
        let dy = e.deltaY;
        if (e.deltaMode === 1) dy *= 33;    // line-mode (Firefox) -> ~px
        if (!isFinite(dy) || dy === 0) return;
        const factor = Math.min(WHEEL_FACTOR_MAX, Math.max(WHEEL_FACTOR_MIN,
            Math.pow(WHEEL_NOTCH_FACTOR, -dy / 100)));
        if (factor !== 1) enterCtx.camera.zoomAt(frac, factor);
    }

    /* Shift+drag = pan (plain drag stays with the brush overlay, which
     * ignores shift-drags — the U2a gesture language, unchanged). */
    function onPointerDown(e) {
        if (!u || !enterCtx || !enterCtx.camera) return;
        if (e.button !== 0 || !e.shiftKey) return;
        e.preventDefault();
        panning = { x: e.clientX, width: u.over.getBoundingClientRect().width };
        if (el && el.setPointerCapture) {
            try { el.setPointerCapture(e.pointerId); } catch (err) { /* ok */ }
        }
    }

    function onPointerMove(e) {
        if (!panning || !enterCtx || !enterCtx.camera) return;
        const dx = e.clientX - panning.x;
        if (dx === 0 || !(panning.width > 0)) return;
        panning.x = e.clientX;
        // Dragging right moves the window BACK in time (grab-the-canvas).
        enterCtx.camera.panByFrac(-dx / panning.width);
    }

    function onPointerUp() { panning = null; }

    /* Click→drill (U2, P3 wire 1), uPlot path: pointerup-without-drag →
     * u.posToVal over the plot area → pure hitTest → drill intent. The hit
     * resolves against latest.spec, which already reflects legend visibility
     * (hidden series are out of the cumulative chain) — a click can never
     * drill into a band that is not on screen. Clicks outside the plot box
     * (axes, padding) resolve to nothing. */
    function onUplotDrillClick(e) {
        drillGate.onClick(e, () => {
            if (!u || !latest || !enterCtx) return null;
            const r = u.over.getBoundingClientRect();
            const px = e.clientX - r.left;
            const py = e.clientY - r.top;
            if (px < 0 || py < 0 || px > r.width || py > r.height) return null;
            const hit = hitTest(latest.spec,
                u.posToVal(px, 'x'), u.posToVal(py, 'y'));
            return drillActionForHit(hit, latest.spec, enterCtx);
        });
    }

    function onCameraChanged() {
        // Every camera movement — gesture zoom/pan, brush 'set', live 'tick'
        // slide, Live-button 'attach' — repaints as a pure viewport
        // transform. App-side effects (pause live, settle-refine, tab
        // refresh) belong to app.js's own subscription.
        scheduleViewportPaint();
    }

    function enterUplot(ctx) {
        UPlotCtor = ctx.uplot ||
            (typeof window !== 'undefined' ? window.uPlot : null);
        // A missing vendor bundle surfaces at mount (rebuildUplot throws into
        // the P1 error-card path) — enter() must never kill the boot.
        enterCtx = ctx;
        uHost = document.createElement('div');
        uHost.className = 'aas-uplot-host';
        el.appendChild(uHost);
        readoutEl = document.createElement('div');
        readoutEl.className = 'aas-readout';
        readoutEl.style.display = 'none';
        el.appendChild(readoutEl);
        diffCanvas = document.createElement('canvas');
        diffCanvas.className = 'aas-diff-strip';
        diffCanvas.style.display = 'none';
        el.appendChild(diffCanvas);

        // Plain-drag brush select via the shared overlay. The adapter maps
        // el-local pixels to axis ms through the LIVE x scale (u.posToVal),
        // which the camera owns — px->time is pure camera math.
        detachSel = attachSelection(el, {
            convertFromPixel: (_grid, pt) => {
                if (!u) return null;
                const overR = u.over.getBoundingClientRect();
                const elR = el.getBoundingClientRect();
                return [u.posToVal(pt[0] - (overR.left - elR.left), 'x'), 0];
            },
        }, {
            onSelect: (range) => {
                if (ctx.onZoom) ctx.onZoom(range.from * NS_PER_MS,
                                           range.to * NS_PER_MS);
            },
        });

        el.addEventListener('wheel', onWheel, { passive: false });
        el.addEventListener('pointerdown', onPointerDown);
        window.addEventListener('pointermove', onPointerMove);
        window.addEventListener('pointerup', onPointerUp);
        // Click→drill (P3 wire 1): the gate tracks pointerdown for its
        // movement check and defers the drill behind the dblclick window.
        el.addEventListener('pointerdown', drillGate.onPointerDown);
        el.addEventListener('click', onUplotDrillClick);
        el.addEventListener('dblclick', drillGate.cancel);
        if (ctx.camera) unsubCamera = ctx.camera.onChange(onCameraChanged);
    }

    function leaveUplot() {
        if (rafId != null) {
            cancelAnimationFrame(rafId);
            rafId = null;
        }
        if (unsubCamera) { unsubCamera(); unsubCamera = null; }
        if (el) {
            el.removeEventListener('wheel', onWheel);
            el.removeEventListener('pointerdown', onPointerDown);
            el.removeEventListener('pointerdown', drillGate.onPointerDown);
            el.removeEventListener('click', onUplotDrillClick);
            el.removeEventListener('dblclick', drillGate.cancel);
        }
        drillGate.cancel();
        window.removeEventListener('pointermove', onPointerMove);
        window.removeEventListener('pointerup', onPointerUp);
        if (detachSel) { detachSel(); detachSel = null; }
        if (u) { u.destroy(); u = null; }
        if (uHost && uHost.parentNode) uHost.parentNode.removeChild(uHost);
        uHost = null;
        if (readoutEl && readoutEl.parentNode) {
            readoutEl.parentNode.removeChild(readoutEl);
        }
        readoutEl = null;
        if (diffCanvas && diffCanvas.parentNode) {
            diffCanvas.parentNode.removeChild(diffCanvas);
        }
        diffCanvas = null;
        if (compareNoteEl && compareNoteEl.parentNode) {
            compareNoteEl.parentNode.removeChild(compareNoteEl);
        }
        compareNoteEl = null;
        if (el) el.classList.remove('compare-active');
        latest = null;
        hiddenKey = null;
        paintedYMax = null;
        resetYMaxHysteresis();
        panning = null;
        enterCtx = null;
        baselineFailed = false;
    }

    // ── ECharts mount path (U2a, kept intact — A/B baseline + rollback) ─────

    function mountEcharts(model, ctx) {
        if (!chart) return;                 // disposed
        rememberBaselineState(model, ctx);
        ensureStrip(ctx);
        renderCompareNote(model, ctx);
        if (!model.hasData) {
            // UI-5: an empty window must not leave the PREVIOUS window's
            // paint on screen while the tables say "No data" — clear the
            // chart and say so explicitly.
            renderedAxis = null;
            echartsLatest = null;
            resetYMaxHysteresis();          // P7: fresh window, fresh axis
            chart.clear();
            chart.setOption({
                backgroundColor: 'transparent',
                graphic: [{
                    type: 'text', left: 'center', top: 'middle',
                    style: { text: 'No data in selected range',
                             fill: '#666', fontSize: 13 },
                }],
            }, true);
            clearLegendAndChip();
            if (ctx.setCompareEvidence) {
                ctx.setCompareEvidence(model.isCompare ? model.data : null,
                    model.compareData || null, !!model.baselinePredates,
                    !!model.baselineUnavailable);
            }
            setEmptyStatus(ctx);
            return;
        }
        // P7 view-level hysteresis, ECharts form: decide BOTH the axis top and
        // the N-CPUs line/chip against the applied top. Rebuild only while a
        // live-tick shrink is held; the builder stays pure and its honesty
        // decision sees the same live scale that uPlot's overlayGeometry gets.
        const displayYMax = yMaxForMount(model.option.yAxis.max, ctx);
        if (displayYMax !== model.option.yAxis.max) {
            const painted = buildAasOption(model.data,
                Object.assign({}, model.opts, { appliedYMax: displayYMax }));
            painted.data = model.data;
            painted.opts = model.opts;
            model = painted;
        }
        chart.setOption(model.option, true);
        renderedAxis = model.axisRange || null;
        echartsLatest = { data: model.data, opts: model.opts, model };
        // One batched legend update per gesture. The annotation series is not
        // in seriesNames, so its trust marks can never be switched off here.
        const applyVisible = (visible) => {
            const sel = {};
            model.seriesNames.forEach(n => { sel[n] = visible.has(n); });
            chart.setOption({ legend: { selected: sel } });
        };
        renderLegend(model, applyVisible, ctx, legend);
        renderFidelityChip(model, ctx);
        if (ctx.setCompareEvidence) {
            ctx.setCompareEvidence(model.isCompare ? model.data : null,
                model.compareData || null, !!model.baselinePredates,
                !!model.baselineUnavailable);
        }
        setStatusLine(ctx, model);
    }

    /* Click→drill (U2, P3 wire 1), ECharts fallback path: the review-spec'd
     * mechanism — series triggerLineEvent (set in buildAasOption, so the
     * stacked band polygons are event-bearing), convertFromPixel, and the
     * cumulative-sum walk. The walk REUSES the pure hitTest by rebuilding
     * the spec surface over the same payload with the painted visibility
     * (one tested walk implementation, zero drift between renderers). The
     * gate listens at the DOM level so a second click ANYWHERE — including
     * off-band — cancels the pending drill (dblclick zoom-out discipline). */
    function onEchartsDrillClick(e) {
        drillGate.onClick(e, () => {
            if (!chart || !echartsLatest || !echartsCtx) return null;
            const r = el.getBoundingClientRect();
            const px = e.clientX - r.left;
            const py = e.clientY - r.top;
            if (!chart.containPixel({ gridIndex: 0 }, [px, py])) return null;
            const pt = chart.convertFromPixel({ gridIndex: 0 }, [px, py]);
            if (!pt || pt[0] == null || pt[1] == null) return null;
            const names = echartsLatest.model.seriesNames;
            const visible = legend.hovered != null
                ? new Set([legend.hovered])
                : (legend.selected || new Set(names));
            const hidden = names.filter(n => !visible.has(n));
            const spec = buildUplotSpec(echartsLatest.data,
                Object.assign({}, echartsLatest.opts, { hiddenNames: hidden }));
            return drillActionForHit(hitTest(spec, pt[0], pt[1]), spec,
                echartsCtx);
        });
    }

    function enterEcharts(ctx) {
        chart = ctx.echarts.init(el, 'dark');
        renderedAxis = null;
        echartsCtx = ctx;
        el.addEventListener('pointerdown', drillGate.onPointerDown);
        el.addEventListener('click', onEchartsDrillClick);
        el.addEventListener('dblclick', drillGate.cancel);
        // Drag-select overlay → zoom the window. PLAIN drag only — the
        // overlay ignores shift-drags, which belong to the inside-dataZoom
        // pan (constraint C). pixelRangeToTime returns x-axis units, ms on
        // the U2a time axis: convert to ns at this boundary.
        detachSel = attachSelection(el, chart, {
            onSelect: (range) => {
                if (ctx.onZoom) ctx.onZoom(range.from * NS_PER_MS,
                                           range.to * NS_PER_MS);
            },
        });
        // U2a gesture mirror: inside-dataZoom events → camera. The batch
        // entries carry start/end as PERCENTS of the pinned axis extent;
        // dispatched actions may instead carry startValue/endValue (ms).
        // The camera is authoritative by convention (§5) on THIS path only —
        // the uPlot path above closes the caveat. Our own setOption never
        // fires 'datazoom', so every event here is a real user gesture.
        chart.on('datazoom', (e) => {
            const cam = ctx.camera;
            if (!cam || !renderedAxis) return;
            const b = (e && e.batch && e.batch[0]) || e || {};
            let fromNs, toNs;
            if (b.startValue != null && b.endValue != null) {
                fromNs = b.startValue * NS_PER_MS;
                toNs = b.endValue * NS_PER_MS;
            } else if (b.start != null && b.end != null) {
                const span = renderedAxis.to - renderedAxis.from;
                fromNs = renderedAxis.from + (b.start / 100) * span;
                toNs = renderedAxis.from + (b.end / 100) * span;
            } else {
                return;
            }
            if (!(toNs > fromNs)) return;
            // Percent round-trip guard: ignore sub-ppm echoes of the
            // current camera window (a no-op "gesture" must not detach).
            const eps = (toNs - fromNs) * 1e-6;
            if (Math.abs(fromNs - cam.fromNs) < eps &&
                Math.abs(toNs - cam.toNs) < eps) return;
            cam.setWindow(fromNs, toNs);
        });
    }

    function leaveEcharts() {
        if (detachSel) { detachSel(); detachSel = null; }
        if (el) {
            el.removeEventListener('pointerdown', drillGate.onPointerDown);
            el.removeEventListener('click', onEchartsDrillClick);
            el.removeEventListener('dblclick', drillGate.cancel);
        }
        drillGate.cancel();
        if (chart) { chart.dispose(); chart = null; }
        renderedAxis = null;
        echartsLatest = null;
        echartsCtx = null;
        resetYMaxHysteresis();
    }

    // ── The view object ──────────────────────────────────────────────────────

    return {
        id: 'active',
        renderer,

        /* Fetch the AAS data for the current window + filters. Uses a
         * single-flight channel so a new refresh supersedes a pending one. */
        async requests(ctx) {
            const t = ctx.timeRange;
            const params = {
                from: t.from,
                to: t.to,
                buckets: targetBuckets(),
                filters: ctx.filters.snapshot(),
            };
            // Class drill-down (no specific event): break down by events.
            const f = ctx.filters.filters;
            if (f.class && !f.event_id) params.detail = 'events';
            const a = ctx.transport.request(ctx.channel('aas'), 'aas', params);
            if (!ctx.compare || !ctx.compare.enabled) return a;
            const predates = baselinePredates(ctx);
            if (predates) {
                return { compare: true, a: await a, b: null,
                    baselinePredates: true };
            }
            const q = shiftQuantized(ctx.camera.quantize(targetBuckets()),
                ctx.compare.offsetNs);
            const [aData, bResult] = await Promise.all([a, ctx.stripCache.ensure(q)]);
            if (!bResult.ok) {
                if (bResult.stale) {
                    throw new CancelledError('superseded baseline strip');
                }
                return { compare: true, a: aData, b: null,
                    baselinePredates: false, baselineUnavailable: true };
            }
            return { compare: true, a: aData, b: bResult.payload,
                baselinePredates: false, baselineUnavailable: false };
        },

        /* PURE: data -> renderer model. Same inputs to both builders (the
         * cross-renderer parity is test-pinned in uplot-aas.test.mjs). The
         * uPlot model keeps the raw payload alongside the spec: the mount
         * layer restacks on visibility changes and the overlay hooks
         * recompute geometry from it on every draw. */
        build(data, ctx) {
            const pair = data && data.compare ? data : null;
            const primary = pair ? pair.a : data;
            const opts = builderOpts(ctx);
            if (pair) {
                opts.compareData = pair.b;
                opts.compareOffsetNs = ctx.compare.offsetNs;
                opts.baselinePredates = pair.baselinePredates;
                opts.compareProvisional = !!(ctx.isLiveTick && ctx.isLiveTick());
            }
            if (renderer === 'uplot') {
                const spec = buildUplotSpec(primary, opts);
                return {
                    data: primary, compareData: pair && pair.b,
                    baselinePredates: !!(pair && pair.baselinePredates),
                    baselineUnavailable: !!(pair && pair.baselineUnavailable),
                    baselineLoading: !!(pair && pair.baselineLoading),
                    isCompare: !!pair, opts, spec,
                    hasData: spec.hasData,
                    maxAas: spec.maxAas,
                    seriesNames: spec.seriesNames,
                };
            }
            const m = buildAasOption(primary, opts);
            // U2 (P3 wire 1): the raw payload + builder opts ride along so
            // the click walk can rebuild the spec surface as painted.
            m.data = primary;
            m.compareData = pair && pair.b;
            m.baselinePredates = !!(pair && pair.baselinePredates);
            m.baselineUnavailable = !!(pair && pair.baselineUnavailable);
            m.baselineLoading = !!(pair && pair.baselineLoading);
            m.isCompare = !!pair;
            m.opts = opts;
            return m;
        },

        mount(_el, model, ctx) {
            if (renderer === 'uplot') { mountUplot(model, ctx); return; }
            mountEcharts(model, ctx);
        },

        /* U2a: synchronous repaint from the strip cache for the CURRENT
         * camera window — stale/coarse strips are served stretched at their
         * true time positions (the Maps model); the exact strip is ensured in
         * the background and swaps in via the app's onData subscription.
         * Never touches the network path directly. Returns whether a cached
         * strip was painted (false = keep the current gesture paint). */
        renderFromCamera(ctx) {
            const alive = renderer === 'uplot' ? !!uHost : !!chart;
            if (!alive || !ctx.camera || !ctx.stripCache) return false;
            const q = ctx.camera.quantize(targetBuckets());
            ctx.stripCache.ensure(q);        // refine (deduped, never rejects)
            const hit = ctx.stripCache.get(q);
            if (!hit) return false;          // nothing cached overlaps: keep paint
            let payload = hit.payload;
            if (ctx.compare && ctx.compare.enabled) {
                const predates = baselinePredates(ctx);
                let b = null;
                if (!predates) {
                    const bq = shiftQuantized(q, ctx.compare.offsetNs);
                    ctx.stripCache.ensure(bq);
                    const bhit = ctx.stripCache.get(bq);
                    if (fullyCoversStrip(bhit, bq)) b = bhit.payload;
                }
                payload = { compare: true, a: hit.payload, b,
                    baselinePredates: predates,
                    baselineUnavailable: !predates && !b && baselineFailed,
                    baselineLoading: !predates && !b && !baselineFailed };
            }
            const model = this.build(payload, ctx);
            this.mount(el, model, ctx);
            return true;
        },

        enter(ctx) {
            el = ctx.chartEl;
            if (!el) return;
            resetLegend();
            if (renderer === 'uplot') enterUplot(ctx);
            else enterEcharts(ctx);
        },

        leave() {
            if (renderer === 'uplot') leaveUplot();
            else leaveEcharts();
            const chip = document.getElementById('aas-fidelity-chip');
            if (chip) chip.remove();
            if (compareNoteEl && compareNoteEl.parentNode) {
                compareNoteEl.parentNode.removeChild(compareNoteEl);
            }
            compareNoteEl = null;
            el = null;
            baselineFailed = false;
            resetLegend();
        },

        resize() {
            if (renderer === 'uplot') {
                if (u) {
                    u.setSize(measureUplot());
                    if (latest) paintDiff(enterCtx, latest.spec);
                }
                return;
            }
            if (chart) chart.resize();
        },

        /* Read-only debug/test surface (Playwright + the gesture-gate agent):
         * what is painted, from which renderer, under which viewport — plus
         * the CURRENT honesty-overlay geometry, so tests can assert the trust
         * marks exist under the live camera state without reaching into
         * canvas pixels. */
        debug() {
            if (renderer === 'uplot') {
                return {
                    renderer,
                    mounted: !!u,
                    hasData: !!latest,
                    seriesNames: latest ? latest.spec.seriesNames.slice() : [],
                    seriesShow: u ? u.series.slice(1).map(s => !!s.show) : [],
                    bucketCount: latest ? latest.spec.alignedData[0].length : 0,
                    xScale: (u && u.scales && u.scales.x)
                        ? { min: u.scales.x.min, max: u.scales.x.max } : null,
                    yMax: paintedYMax,
                    yHysteresis: { applied: yHyst.applied, run: yHyst.run },
                    overlays: (latest && u)
                        ? overlayGeometry(latest.data, latest.opts,
                            { min: u.scales.x.min, max: u.scales.x.max,
                              yMax: u.scales.y.max })
                        : null,
                    compare: latest ? latest.spec.compare : null,
                };
            }
            return { renderer, mounted: !!chart,
                     yHysteresis: { applied: yHyst.applied, run: yHyst.run } };
        },
    };
}

/* Fidelity legend chip: explains the sampled/exact distinction next to the AAS
 * legend. Rendered into a small host inserted after the series legend so the
 * shading band on the chart is never unexplained. Hidden for fully-exact
 * windows (the common case) to avoid noise. Renderer-agnostic: reads only the
 * shared model surface (fidelity/fidelityLabel/shading/escalation), which both
 * builders emit identically. */
function renderFidelityChip(model, ctx) {
    const host = ctx.chartEl;
    if (!host || !host.parentNode) return;

    let chip = document.getElementById('aas-fidelity-chip');
    const fid = model.fidelity;
    const showBand = model.shading && model.shading.showLegend;

    if (!showBand) {
        if (chip) chip.remove();
        return;
    }
    if (!chip) {
        chip = document.createElement('div');
        chip.id = 'aas-fidelity-chip';
        chip.style.cssText =
            'display:flex;flex-wrap:wrap;gap:10px;justify-content:center;' +
            'padding:0 20px 8px;background:#1e1e3a;font-size:11px;color:#aaa;';
        const legDiv = document.getElementById('aas-legend');
        if (legDiv && legDiv.parentNode) legDiv.parentNode.insertBefore(chip, legDiv.nextSibling);
        else host.parentNode.insertBefore(chip, host.nextSibling);
    }

    const sampledSwatch =
        '<span style="width:14px;height:10px;display:inline-block;border-radius:2px;' +
        'background:' + SAMPLED_BAND_COLOR + ';border:1px dashed ' + SAMPLED_BORDER + '"></span>';
    const mixedSwatch =
        '<span style="width:14px;height:10px;display:inline-block;border-radius:2px;' +
        'background:' + MIXED_BAND_COLOR + ';border:1px dashed ' + MIXED_BORDER + '"></span>';

    let html = '<span>' + esc(model.fidelityLabel) + '</span>';
    if (fid === 'sampled') {
        html += '<span style="display:inline-flex;align-items:center;gap:5px">' +
            sampledSwatch + 'shaded = sampled (estimated, not exact)</span>';
    } else if (fid === 'mixed') {
        html += '<span style="display:inline-flex;align-items:center;gap:5px">' +
            sampledSwatch + 'sampled</span>' +
            '<span style="display:inline-flex;align-items:center;gap:5px">' +
            mixedSwatch + 'mixed window</span>';
    }
    if (model.escalation) {
        html += '<span style="color:' +
            (model.escalation.isAnomaly ? '#E53935' : '#4fc3f7') + '">● ' +
            esc(model.escalation.label) + '</span>';
    }
    chip.innerHTML = html;
}

/* External HTML legend with solo / multi-select / hover — kept out of the chart
 * for full control (matches the old ApexCharts custom legend behavior).
 *
 * U1 (review P2): selection is keyed by series NAME and lives in `leg`
 * ({selected, hovered, names}, owned by the view). It resets whenever the
 * series set changes — class<->event mode switch, or the top-N event set
 * shifting between ticks — so a CPU solo can never silently become a solo of
 * whatever series inherited the index. Visibility is applied as ONE batched
 * call per gesture, and an in-progress hover-solo is tracked in leg.hovered
 * and RE-APPLIED after a live-tick rebuild — the old trailing apply(sel) used
 * to cancel the solo under a stationary cursor.
 *
 * U2b: renderer-agnostic — `applyVisible(visibleNameSet)` is the ONE renderer
 * hook (ECharts: a batched legend.selected setOption; uPlot: the recipe
 * restack + setSeries/bands/setData swap). Chip rendering, name keying, and
 * the hover/click semantics are shared verbatim. */
function renderLegend(model, applyVisible, ctx, leg) {
    const names = model.seriesNames;
    const colors = model.seriesColors;
    const host = ctx.chartEl;
    if (!host) return;

    const sameSet = !!leg.names && leg.names.length === names.length &&
        leg.names.every((n, i) => n === names[i]);
    if (!sameSet) {
        leg.selected = new Set(names);
        leg.hovered = null;
    }
    leg.names = names.slice();

    let legDiv = document.getElementById('aas-legend');
    if (!legDiv) {
        legDiv = document.createElement('div');
        legDiv.id = 'aas-legend';
        legDiv.style.cssText = 'display:flex;flex-wrap:wrap;gap:4px;justify-content:center;padding:8px 20px;background:#1e1e3a;';
        host.parentNode.insertBefore(legDiv, host.nextSibling);
    }

    legDiv.innerHTML = names.map((name, i) =>
        `<span class="aleg" data-i="${i}" style="display:inline-flex;align-items:center;gap:4px;` +
        `cursor:pointer;padding:3px 10px;border-radius:4px;font-size:11px;color:#ccc;` +
        `border:1px solid ${colors[i]};border-left:3px solid ${colors[i]};` +
        `user-select:none;transition:opacity 0.1s">` +
        `<span style="width:8px;height:8px;border-radius:2px;background:${colors[i]}"></span>` +
        `${esc(name)}</span>`
    ).join('');

    // Chip opacity always reflects the persistent selection, not a hover.
    function paintChips() {
        legDiv.querySelectorAll('.aleg').forEach(elx => {
            elx.style.opacity = leg.selected.has(names[+elx.dataset.i]) ? '1' : '0.3';
        });
    }

    legDiv.querySelectorAll('.aleg').forEach(elx => {
        const name = names[+elx.dataset.i];
        elx.addEventListener('mouseenter', () => {
            leg.hovered = name;
            applyVisible(new Set([name]));
        });
        elx.addEventListener('mouseleave', () => {
            leg.hovered = null;
            applyVisible(leg.selected);
        });
        elx.addEventListener('click', (e) => {
            let set = leg.selected;
            if (e.metaKey || e.ctrlKey) {
                if (set.has(name)) { if (set.size > 1) set.delete(name); }
                else set.add(name);
            } else {
                if (set.size === 1 && set.has(name)) set = new Set(names);
                else set = new Set([name]);
            }
            leg.selected = set;
            // The click states the user's new intent; a later refresh must
            // show this selection, not resurrect the pre-click hover solo.
            leg.hovered = null;
            paintChips();
            applyVisible(set);
        });
    });

    // A live tick replaces the chips mid-hover; the detached chip never fires
    // mouseleave, so clear a stuck hover when the cursor leaves the legend
    // itself. Plain assignment (not addEventListener): legDiv persists across
    // refreshes and must carry exactly one, current-closure handler.
    legDiv.onmouseleave = () => {
        if (leg.hovered != null) {
            leg.hovered = null;
            applyVisible(leg.selected);
        }
    };

    paintChips();
    // Re-apply the CURRENT state after the rebuild: the in-progress
    // hover-solo if there is one, else the persistent selection.
    applyVisible(leg.hovered != null ? new Set([leg.hovered]) : leg.selected);
}
