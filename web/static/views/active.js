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
} from '../lib/uplot-aas.js';
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

    // ── ECharts-only state ───────────────────────────────────────────────────
    let chart = null;          // ECharts instance — owned here, nowhere else
    let detachSel = null;      // selection-overlay teardown (both renderers)
    // U2a: the ns extent of the last mounted option's pinned time axis.
    // 'datazoom' events carry start/end PERCENTS of exactly this extent.
    let renderedAxis = null;

    // ── uPlot-only state ─────────────────────────────────────────────────────
    let UPlotCtor = null;      // the vendored constructor (ctx.uplot)
    let u = null;              // uPlot instance — owned here, nowhere else
    let uHost = null;          // inner host div (el keeps its padding box)
    let readoutEl = null;      // compact crosshair readout card
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
        if (have && have.exact) return;                 // exact strip cached
        if (have &&
            have.resNs <= q.resNs * 2 &&
            have.stripFrom <= q.winFromNs &&
            have.stripTo >= q.winToNs &&
            (have.stripTo - q.winToNs) > (q.winToNs - q.winFromNs) / 4) {
            return;                                     // still covered, far from edge
        }
        ctx.stripCache.ensure(q);
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
    }

    function measureUplot() {
        // uHost is a block child inside el's padding box, so clientWidth is
        // the content width; el's min-height (CSS) / explicit height (the
        // resize handle) give the pane height.
        const w = uHost ? uHost.clientWidth : 800;
        let h = el ? el.clientHeight : 300;
        if (!(h > 0)) h = 300;
        return { width: Math.max(200, w || 800), height: Math.max(120, h) };
    }

    /* Current-view overlay geometry for the draw hooks: recomputed from the
     * latest painted payload against the CURRENT x scale on every draw, so
     * the honesty overlays track every camera state AND every data swap. */
    function overlayGeo(uu) {
        if (!latest) return { rects: [], vlines: [], hlines: [] };
        return overlayGeometry(latest.data, latest.opts,
            { min: uu.scales.x.min, max: uu.scales.x.max });
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
        o.hooks = Object.assign({}, overlayHooks(overlayGeo), {
            setCursor: [(uu) => updateReadout(uu)],
        });
        // uPlot's dblclick handler autoscales x to the full data extent —
        // that would fight the app's dblclick zoom-out (initChartView), so
        // unbind it. The spec already disables cursor drag select/zoom;
        // uPlot merges cursor opts deeply, so only dblclick is overridden.
        o.cursor = Object.assign({}, o.cursor,
            { bind: { dblclick: () => null } });
        u = new UPlotCtor(o, spec.alignedData, uHost);
        u.setScale('x', viewportScale(ctx, spec));
        paintedYMax = spec.yMax;
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
    }

    function uplotEmptyState(ctx) {
        // UI-5: an empty window must not leave the PREVIOUS window's paint on
        // screen while the tables say "No data" — same contract as the
        // ECharts branch, uPlot form: destroy the instance, say so.
        if (u) { u.destroy(); u = null; }
        latest = null;
        hiddenKey = null;
        paintedYMax = null;
        if (readoutEl) readoutEl.style.display = 'none';
        if (uHost) {
            uHost.innerHTML =
                '<div class="aas-empty">No data in selected range</div>';
        }
        clearLegendAndChip();
        setEmptyStatus(ctx);
    }

    function mountUplot(model, ctx) {
        if (!uHost) return;                 // disposed
        ensureStrip(ctx);
        if (!model.hasData) { uplotEmptyState(ctx); return; }

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
            // an explicit y setScale when the payload's scale top moved
            // (verified against the pinned bundle: explicit min/max land
            // exactly — with data present they bypass the range fn).
            u.batch(() => {
                u.delBand(null);
                spec.bands.forEach(b => u.addBand({ series: b.series.slice() }));
                names.forEach((n, i) =>
                    u.setSeries(i + 1, { show: visible.has(n) }, false));
                u.setData(spec.alignedData, false);
                if (spec.yMax !== paintedYMax) {
                    u.setScale('y', { min: 0, max: spec.yMax });
                    paintedYMax = spec.yMax;
                }
                u.setScale('x', viewportScale(ctx, spec));
            });
        }

        renderLegend(spec, applyUplotVisibility, ctx, legend);
        renderFidelityChip(spec, ctx);
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
        }
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
        latest = null;
        hiddenKey = null;
        paintedYMax = null;
        panning = null;
        enterCtx = null;
    }

    // ── ECharts mount path (U2a, kept intact — A/B baseline + rollback) ─────

    function mountEcharts(model, ctx) {
        if (!chart) return;                 // disposed
        ensureStrip(ctx);
        if (!model.hasData) {
            // UI-5: an empty window must not leave the PREVIOUS window's
            // paint on screen while the tables say "No data" — clear the
            // chart and say so explicitly.
            renderedAxis = null;
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
            setEmptyStatus(ctx);
            return;
        }
        chart.setOption(model.option, true);
        renderedAxis = model.axisRange || null;
        // One batched legend update per gesture. The annotation series is not
        // in seriesNames, so its trust marks can never be switched off here.
        const applyVisible = (visible) => {
            const sel = {};
            model.seriesNames.forEach(n => { sel[n] = visible.has(n); });
            chart.setOption({ legend: { selected: sel } });
        };
        renderLegend(model, applyVisible, ctx, legend);
        renderFidelityChip(model, ctx);
        setStatusLine(ctx, model);
    }

    function enterEcharts(ctx) {
        chart = ctx.echarts.init(el, 'dark');
        renderedAxis = null;
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
        if (chart) { chart.dispose(); chart = null; }
        renderedAxis = null;
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
            return ctx.transport.request(ctx.channel('aas'), 'aas', params);
        },

        /* PURE: data -> renderer model. Same inputs to both builders (the
         * cross-renderer parity is test-pinned in uplot-aas.test.mjs). The
         * uPlot model keeps the raw payload alongside the spec: the mount
         * layer restacks on visibility changes and the overlay hooks
         * recompute geometry from it on every draw. */
        build(data, ctx) {
            const opts = builderOpts(ctx);
            if (renderer === 'uplot') {
                const spec = buildUplotSpec(data, opts);
                return {
                    data, opts, spec,
                    hasData: spec.hasData,
                    maxAas: spec.maxAas,
                    seriesNames: spec.seriesNames,
                };
            }
            return buildAasOption(data, opts);
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
            const model = this.build(hit.payload, ctx);
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
            el = null;
            resetLegend();
        },

        resize() {
            if (renderer === 'uplot') {
                if (u) u.setSize(measureUplot());
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
                    overlays: (latest && u)
                        ? overlayGeometry(latest.data, latest.opts,
                            { min: u.scales.x.min, max: u.scales.x.max })
                        : null,
                };
            }
            return { renderer, mounted: !!chart };
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
