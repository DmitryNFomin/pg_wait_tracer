/* pgwt — pure builder: AAS data -> uPlot spec (Track U, Phase U2b).
 *
 * The AAS pane's RENDERER swap. The ECharts gesture pipeline's floor made
 * 60 fps impossible at instrument preload (real-Chromium gesture-to-paint
 * p95 = 61.2/59.4/67.6 ms at a 1258-bucket x 16-series strip,
 * tests/results/gesture_gate.json — the written gate in
 * docs/INSTRUMENT_ARCHITECTURE.md §5 tripped RED -> path (b), uPlot
 * substrate). uPlot renders a viewport transform, not an option pipeline:
 * a camera move is ONE u.setScale('x', {min,max}) call over resident data.
 * The camera (lib/camera.js) becomes genuinely authoritative here — our
 * handlers mutate the camera, and the renderer draws FROM camera state;
 * there is no library-owned gesture model left to mirror.
 *
 * Everything upstream is unchanged: the camera and strip cache stay pure ns
 * modules; this builder consumes the SAME inputs as buildAasOption
 * (lib/builders/aas.js) — the aas response payload plus
 * { numCpus, win, axis, escalationStatus } — and emits the same
 * seriesNames/seriesColors contract, so the external HTML legend, the
 * fidelity chip, and the U1 identity-color service carry over verbatim.
 *
 * ── Verified against the PINNED vendored bundle ─────────────────────────────
 * Every uPlot behavior this module leans on was read out of the readable twin
 * of the vendored bundle (dist/uPlot.iife.js at tag 1.6.32 — see
 * web/static/vendor/README.md; re-verify these on any version bump):
 *   ms:1        opts.ms = 1 makes x timestamps UNIX MILLISECONDS (line 2977,
 *               `const ms = opts.ms || 1e-3`); tick generation switches to the
 *               ms increments/stamps tables (line 3159).
 *   setScale    the default x range fn is snapNumX = pass-through
 *               `[dataMin, dataMax]` (lines 2852-2856), and setScales() runs
 *               explicit pending min/max through it unchanged — so
 *               u.setScale('x', {min,max}) lands EXACTLY. The camera window
 *               is the x scale range; no percent round-trips, no echo events.
 *   setData     u.setData(data, false) never re-ranges x (autoScaleX at line
 *               3900 runs only on resetScales) — the strip-swap-under-a-
 *               fixed-camera path (stale strip keeps drawing, finer strip
 *               swaps in under the same window).
 *   y pin       a y scale `range: [0, yMax]` ARRAY becomes a constant range
 *               fn with auto disabled (initScale, lines 3016-3080): y is
 *               pinned exactly like the ECharts yAxis min/max was.
 *   bands       a band { series: [upper, lower] } clips the UPPER series'
 *               fill down to the lower edge (fillStroke, lines 4316-4336;
 *               band fill defaults to the upper series' own fill). This is
 *               the official stacked-series recipe's mechanism (demos/
 *               stack.js at the same tag, mirrored in stackSeries below).
 *   tzDate      uPlot's own UTC fast path is
 *               `new Date(+date + date.getTimezoneOffset() * 6e4)` (lines
 *               966-971); utcTzDate below is that exact shim, so tick
 *               PLACEMENT is UTC-calendar-aligned (UI-11). Label TEXT never
 *               goes near it — the axis values fn formats via fmtTime
 *               (toUTCString), TZ-stable for pixel baselines.
 *   values fn   a function axis.values is used as-is on time axes (line
 *               3766-3778) — uPlot's locale stamp templates never run.
 *
 * ── Mount contract (the impure side, for the integrator) ────────────────────
 *   const spec = buildUplotSpec(data, opts);
 *   const u = new uPlot(spec.uplotOpts, spec.alignedData, el);
 *   u.setScale('x', spec.xWindow);          // land on the camera window
 *   // camera gesture (wheel zoom / shift-drag pan / dblclick zoom-out):
 *   //   camera.zoomAt()/panByFrac() -> u.setScale('x', {min: camFromNs/1e6,
 *   //   max: camToNs/1e6}) — nothing else. Plain drag stays with the
 *   //   lib/selection.js brush overlay (gesture language unchanged).
 *   // strip swap (settle-refine / live tick / stripcache onData):
 *   //   const s2 = buildUplotSpec(newData, newOpts);
 *   //   u.setData(s2.alignedData, false);  // false: x window stays put
 *   //   (series set changed? rebuild the instance — rare, non-gesture path)
 *   // legend toggle (name-keyed, U1): rebuild spec with opts.hiddenNames,
 *   //   then u.setSeries(idx, {show}), u.delBand(null),
 *   //   spec.bands.forEach(b => u.addBand(b)), u.setData(spec.alignedData,
 *   //   false) — the recipe's setSeries pattern. name -> uPlot index is
 *   //   seriesNames.indexOf(name) + 1 (slot order never changes).
 *   // honesty overlays: merge overlayHooks(() => geometryForCurrentView)
 *   //   into uplotOpts.hooks BEFORE construction; recompute geometry from
 *   //   the CURRENT data/opts on every draw so the overlays exist in every
 *   //   camera state (constraint D — an instrument that zooms into
 *   //   fabricated confidence betrays the project).
 *
 * The pure parts (buildUplotSpec, stackSeries, hitTest, overlayGeometry,
 * utcTzDate, withAlpha) touch no DOM, no network, no Date.now, and are
 * Node-tested (tests/web_unit/uplot-aas.test.mjs). The draw hooks
 * (drawOverlayRects/drawOverlayLines) are thin canvas painters over the pure
 * geometry, exercised by the Playwright/snapshot suites.
 */

import { WAIT_CLASSES, classIndex, eventColor, fmtTime } from './format.js';
import {
    buildFidelityShading, buildEscalationAnnotation, fidelityOf, fidelityLabel,
    SAMPLED_BAND_COLOR, MIXED_BAND_COLOR, SAMPLED_BORDER, MIXED_BORDER,
    ESC_MANUAL_COLOR, ESC_ANOMALY_COLOR, ESC_MANUAL_BORDER, ESC_ANOMALY_BORDER,
} from './builders/fidelity.js';

/* ns→ms at the spec boundary — the ONE conversion site, same rule as the
 * ECharts builder: everything upstream (camera, strip cache, fidelity/
 * escalation view-model geometry) stays in ns. */
const NS_PER_MS = 1e6;
const nsToMs = (ns) => ns / NS_PER_MS;

/* Stacked-area fill opacity — matches the ECharts areaStyle {opacity: 0.85}
 * so the recolor at swap time is zero. */
export const AREA_FILL_ALPHA = 0.85;

/* N-CPUs reference line color (same as the ECharts markLine). */
export const NCPUS_COLOR = '#E53935';

/* ── y-axis policy (Track U Phase U2, review P7) ─────────────────────────────
 * yMax = max(maxAas*1.2, min(numCpus*1.5, maxAas*4), 1) — the capacity
 * reference may lift the axis to at most 4x the data peak, so a 64-vCPU host
 * at AAS≈3 no longer flattens the stack into a sliver under the red line.
 * When the cap puts the line off-scale, overlayGeometry (given the live y
 * scale top) swaps it for an explicit 'ncpus-offscale' top-edge affordance —
 * the reference never silently vanishes. Duplicated verbatim in
 * lib/builders/aas.js (drop-in interchangeable builders); the parity test
 * pins both. Hysteresis (grow now, shrink after 3 stable ticks) lives at the
 * VIEW level (views/active.js), never here — builders stay pure per-payload. */
function policyYMax(maxAas, numCpus) {
    return Math.max(
        maxAas * 1.2,
        numCpus > 0 ? Math.min(numCpus * 1.5, maxAas * 4) : 0,
        1
    );
}

/* 'rgb(r,g,b)' -> 'rgba(r,g,b,a)'. Non-rgb() strings pass through untouched
 * (the color service only ever emits rgb(), pinned by its tests). */
export function withAlpha(color, alpha) {
    const m = /^rgb\((\d+),\s*(\d+),\s*(\d+)\)$/.exec(color);
    if (!m) return color;
    return 'rgba(' + m[1] + ',' + m[2] + ',' + m[3] + ',' + alpha + ')';
}

/* UTC tzDate for uPlot tick generation: a Date shifted so its LOCAL getters
 * read UTC components — byte-for-byte what uPlot.tzDate(date, 'UTC') does
 * (vendored source lines 966-971), reimplemented here so the pure spec never
 * references the uPlot global. Receives epoch MILLISECONDS (ms:1). Label
 * text does not depend on this (fmtTime renders via toUTCString); only tick
 * PLACEMENT does, and only on non-UTC hosts inside their own DST-flip hour
 * is placement approximate — the same trade uPlot itself ships. */
export function utcTzDate(tsMs) {
    const d = new Date(tsMs);
    return new Date(tsMs + d.getTimezoneOffset() * 60000);
}

/* Identity-ordered series extraction — the SAME ordering + color rules as
 * buildAasOption (U1, review P2), duplicated because the two builders must
 * stay drop-in interchangeable while both exist (the parity test in
 * tests/web_unit/uplot-aas.test.mjs compares them input-for-input):
 *   event mode: order by class (canonical WAIT_CLASSES rank; unknown classes
 *   after all known ones), then event name — never the server's per-window
 *   AAS rank; colors from the eventColor identity service.
 *   class mode: WAIT_CLASSES order and colors.
 * Values are rounded to 4 decimals BEFORE stacking (parity with the ECharts
 * per-point rounding). */
function identitySeries(data) {
    const buckets = (data && data.buckets) || [];
    const isEventBreakdown = data && data.breakdown === 'events' && data.series;
    if (isEventBreakdown) {
        const order = data.series.map((s, idx) => ({ name: s.name, idx }));
        order.sort((a, b) => {
            const ca = classIndex(a.name), cb = classIndex(b.name);
            const ra = ca < 0 ? WAIT_CLASSES.length : ca;
            const rb = cb < 0 ? WAIT_CLASSES.length : cb;
            if (ra !== rb) return ra - rb;
            return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
        });
        return {
            names: order.map(o => o.name),
            colors: order.map(o => eventColor(null, o.name)),
            // U2 (P3 wire 1): per-series event_id for click→drill (null when
            // the server/fixture omits it) — same contract as buildAasOption.
            ids: order.map(o => {
                const id = data.series[o.idx] && data.series[o.idx].event_id;
                return id != null ? id : null;
            }),
            values: order.map(o =>
                buckets.map(b => +(b.aas[o.idx] || 0).toFixed(4))),
        };
    }
    return {
        names: WAIT_CLASSES.map(wc => wc.label),
        colors: WAIT_CLASSES.map(c => c.color),
        ids: WAIT_CLASSES.map(() => null),   // class drills go by name
        values: WAIT_CLASSES.map(wc =>
            buckets.map(b => +(b[wc.key] || 0).toFixed(4))),
    };
}

/* The official uPlot stack.js recipe pattern, name-aware: cumulative sums
 * bottom→top in slot order, hidden series pass through UNSTACKED (they keep
 * their alignedData slot so uPlot series indices never shift across legend
 * toggles), bands clip each visible series' fill down to the next visible
 * series below it. The recipe's inert [-1, top] band is dropped (uPlot
 * ignores it: no series ever matches upper == -1; the topmost fill is
 * clipped by the band where IT is the upper edge, and the bottom series
 * fills to zero).
 *
 * Returns { alignedData (without x), bands, stackIdxs } where stackIdxs are
 * the uPlot series indices (1-based; 0 is x) in the cumulative chain,
 * bottom→top. Totals are permutation-invariant by construction: the sum over
 * a bucket does not depend on input order, and slot order is identity order.
 */
export function stackSeries(values, hidden) {
    const nBuckets = values.length ? values[0].length : 0;
    const acc = new Array(nBuckets).fill(0);
    const stacked = [];
    const stackIdxs = [];
    values.forEach((vals, i) => {
        if (hidden[i]) {
            stacked.push(vals.slice());          // recipe: raw pass-through
        } else {
            stacked.push(vals.map((v, j) => (acc[j] += v)));
            stackIdxs.push(i + 1);
        }
    });
    const bands = [];
    for (let k = 0; k + 1 < stackIdxs.length; k++) {
        bands.push({ series: [stackIdxs[k + 1], stackIdxs[k]] });
    }
    return { stacked, bands, stackIdxs };
}

/* data: aas response { buckets[], bucket_ns, max_aas, breakdown?, series?,
 *                      fidelity, sample_period_ns, fidelity_ranges? }
 * opts: {
 *   numCpus,
 *   win:  { from, to },   // ns — the VISIBLE camera window -> xWindow
 *   axis: { from, to },   // ns, OPTIONAL — the quantized 3x strip skirt;
 *                         //   unioned with win into axisRange (anchors the
 *                         //   whole-response fidelity shading, exactly like
 *                         //   the ECharts pinned-axis extent did)
 *   escalationStatus,     // daemon control status (observed_start_ns stamped
 *                         //   by app.js pollDaemon)
 *   hiddenNames,          // OPTIONAL iterable of series names the legend has
 *                         //   switched off: excluded from the cumulative
 *                         //   chain + bands, slot retained, show:false
 * }
 *
 * Returns {
 *   uplotOpts,      // ready-to-construct opts (width/height are inert
 *                   //   placeholders — the mount layer sets real pixels)
 *   alignedData,    // [xsMs, cum1..cumN] — N slots in identity order
 *   bands,          // uPlot band descriptors (also on uplotOpts.bands; NOTE:
 *                   //   uPlot normalizes band objects in place at construct)
 *   stackIdxs,      // visible cumulative chain, uPlot indices bottom→top
 *   seriesNames,    // FULL identity-ordered name list (hidden included) —
 *   seriesColors,   //   the external legend contract, same as buildAasOption
 *   rawValues,      // per-series UNSTACKED rounded values (tooltip source)
 *   overlays,       // overlayGeometry(data, opts, null) — absolute,
 *                   //   viewport-independent honesty geometry
 *   xWindow,        // {min,max} ms — the setScale('x', ...) target
 *   yMax,           // pinned y range top (same formula as the ECharts yAxis)
 *   bucketMs, bucketNs, hasData, maxAas,
 *   fidelity, fidelityLabel, shading, escalation,   // chip/legend surface
 *   axisRange, window,                              // ns, parity fields
 * } */
export function buildUplotSpec(data, opts) {
    opts = opts || {};
    const numCpus = opts.numCpus || 0;
    const bns = (data && data.bucket_ns) || 0;
    const buckets = (data && data.buckets) || [];

    const { names, colors, ids, values } = identitySeries(data);
    const hiddenSet = new Set(opts.hiddenNames || []);
    const hidden = names.map(n => hiddenSet.has(n));

    const xs = buckets.map(b => nsToMs(b.t));
    const { stacked, bands, stackIdxs } = stackSeries(values, hidden);
    const alignedData = [xs].concat(stacked);

    const maxAas = (data && data.max_aas) || 0;
    const yMax = policyYMax(maxAas, numCpus);

    // Window / axis extents (ns) — the same derivation as buildAasOption:
    // win falls back to the bucket span (Node tests, degenerate callers);
    // axisRange = opts.axis ∪ win, never narrower than the window.
    const xMinNs = buckets.length ? buckets[0].t : 0;
    const xMaxNs = buckets.length ? buckets[buckets.length - 1].t : NS_PER_MS;
    const win = opts.win || { from: xMinNs, to: xMaxNs };
    let axisFromNs = (opts.axis && opts.axis.from != null) ? opts.axis.from : win.from;
    let axisToNs = (opts.axis && opts.axis.to != null) ? opts.axis.to : win.to;
    if (win.from < axisFromNs) axisFromNs = win.from;
    if (win.to > axisToNs) axisToNs = win.to;
    if (!(axisToNs > axisFromNs)) axisToNs = axisFromNs + NS_PER_MS;

    // The viewport: the camera window as an explicit x scale range. The
    // strip may be wider — that is the whole point (pan/zoom serve resident
    // data; the scale range is the viewport transform).
    const xWindow = win.to > win.from
        ? { min: nsToMs(win.from), max: nsToMs(win.to) }
        : { min: nsToMs(axisFromNs), max: nsToMs(axisToNs) };

    const shading = buildFidelityShading(data, { from: axisFromNs, to: axisToNs });
    const escAnno = buildEscalationAnnotation(opts.escalationStatus,
        { from: win.from, to: win.to });

    const seriesDefs = [{}].concat(names.map((name, i) => ({
        label: name,
        stroke: colors[i],
        fill: withAlpha(colors[i], AREA_FILL_ALPHA),
        width: 1,
        points: { show: false },
        show: !hidden[i],
    })));

    const uplotOpts = {
        // Inert placeholders: uPlot needs pixel dims at construct time and
        // only the mount layer knows them (clientWidth). Purity boundary.
        width: 800,
        height: 300,
        // x timestamps are UNIX MILLISECONDS (verified: opts.ms, source line
        // 2977) — same axis units as the ECharts time axis, so the selection
        // overlay's ms->ns boundary conversion carries over unchanged.
        ms: 1,
        // UTC tick placement (UI-11). Labels are formatted below via fmtTime
        // and never consult this.
        tzDate: utcTzDate,
        // External HTML legend only — uPlot's own legend stays off, exactly
        // like the hidden ECharts legend component.
        legend: { show: false },
        // WE own every gesture (U2b): wheel = cursor-anchored camera zoom,
        // shift+drag = camera pan, plain drag = lib/selection.js brush,
        // dblclick = zoom-out — all wired by the mount layer straight into
        // the camera. uPlot's built-in drag-select/zoom is disabled so it
        // can never race the gesture language.
        cursor: {
            drag: { x: false, y: false, setScale: false },
            points: { show: false },
        },
        scales: {
            // time:true + default pass-through range: setScale('x', xWindow)
            // is exact (snapNumX, source lines 2852-2856). auto:false is
            // declarative belt-and-braces; the real guard is that every
            // setData call goes through the documented (data, false) form.
            x: { time: true, auto: false },
            // Array range -> constant range fn + auto off (initScale): y is
            // PINNED to [0, yMax], the ECharts yAxis min/max equivalent.
            y: { auto: false, range: [0, yMax] },
        },
        axes: [
            {
                stroke: '#888',
                font: '10px sans-serif',
                size: 28,
                gap: 4,
                ticks: { show: true, stroke: '#333', width: 1 },
                grid: { show: false },
                // Explicit UTC formatter (constraint B): fmtTime renders via
                // toUTCString — TZ-stable for pixel baselines, never uPlot's
                // locale stamp templates (a values FUNCTION bypasses them,
                // source lines 3766-3778).
                values: (u, splits) => splits.map(v => fmtTime(v * NS_PER_MS, bns)),
            },
            {
                stroke: '#888',
                font: '10px sans-serif',
                size: 55,
                gap: 4,
                ticks: { show: true, stroke: '#333', width: 1 },
                grid: { stroke: '#2a2a4a', width: 1 },
                values: (u, splits) => splits.map(v => v.toFixed(1)),
            },
        ],
        series: seriesDefs,
        bands,
    };

    return {
        uplotOpts,
        alignedData,
        bands,
        stackIdxs,
        seriesNames: names,
        seriesColors: colors,
        // U2 (P3 wire 1): parallel to seriesNames — event_id per series in
        // event mode, nulls in class mode; plus which mode this spec is in.
        // hitTest gives seriesIdx; seriesIds[seriesIdx-1] + breakdown resolve
        // the click to a drill intent (parity-pinned vs buildAasOption).
        seriesIds: ids,
        breakdown: (data && data.breakdown === 'events' && data.series)
            ? 'events' : 'classes',
        rawValues: values,
        overlays: overlayGeometry(data, opts, null),
        xWindow,
        yMax,
        bucketNs: bns,
        bucketMs: bns > 0 ? nsToMs(bns) : 0,
        hasData: buckets.length > 0,
        maxAas,
        fidelity: fidelityOf(data),
        fidelityLabel: fidelityLabel(fidelityOf(data)),
        shading,
        escalation: escAnno,
        axisRange: { from: axisFromNs, to: axisToNs },
        window: { from: win.from, to: win.to },
    };
}

/* Cumulative-sum hit test: which series' band is under (xMs, yAas)?
 *
 *   spec  a buildUplotSpec return (needs alignedData, stackIdxs,
 *         seriesNames, bucketMs)
 *   xMs   x in axis units (ms) — e.g. u.posToVal(cursorLeft, 'x')
 *   yAas  y in AAS units — e.g. u.posToVal(cursorTop, 'y')
 *
 * Returns { seriesName, seriesIdx (uPlot index), bucketIdx } or null.
 * Semantics:
 *   - a bucket OWNS [t, t + bucketMs): before the first bucket, past a
 *     bucket's width (a gap in a sparse strip, or past the last bucket's
 *     end) -> null — never attribute a hit to data that is not there;
 *   - yAas < 0 or above the stack top -> null;
 *   - walk the VISIBLE cumulative chain bottom→top; the first cumulative
 *     >= yAas wins; zero-thickness layers (no contribution in this bucket)
 *     are unhittable — the boundary y belongs to the series whose band ends
 *     there, matching what the eye sees;
 *   - a null cumulative (gap bucket) -> null. */
export function hitTest(spec, xMs, yAas) {
    if (!spec || !spec.alignedData || !spec.alignedData[0]) return null;
    const xs = spec.alignedData[0];
    const n = xs.length;
    if (!n) return null;
    if (yAas == null || !isFinite(yAas) || yAas < 0) return null;
    if (xMs == null || !isFinite(xMs) || xMs < xs[0]) return null;

    // Rightmost bucket with xs[i] <= xMs (xs ascending — server buckets are).
    let lo = 0, hi = n - 1;
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (xs[mid] <= xMs) lo = mid; else hi = mid - 1;
    }
    const bucketIdx = lo;
    if (spec.bucketMs > 0 && xMs >= xs[bucketIdx] + spec.bucketMs) return null;

    let prev = 0;
    for (const si of (spec.stackIdxs || [])) {
        const cum = spec.alignedData[si][bucketIdx];
        if (cum == null) return null;               // gap bucket
        if (yAas <= cum && cum - prev > 0) {
            return {
                seriesName: spec.seriesNames[si - 1],
                seriesIdx: si,
                bucketIdx,
            };
        }
        prev = cum;
    }
    return null;                                     // above the stack top
}

/* ── Honesty overlays as camera-space geometry (constraint D) ────────────────
 *
 * The SAME server facts the ECharts 'pgwt-annotations' series carried —
 * sampled/mixed fidelity shading, the escalation band + live-edge line, the
 * N-CPUs reference line — as plain rects/lines in axis units, for the draw
 * hooks to paint. Geometry is ABSOLUTE (data-anchored): a shading band sits
 * where the sampled data actually is, and zooming/panning can neither move
 * it nor stretch it over data it does not cover.
 *
 *   data, opts    exactly buildUplotSpec's inputs
 *   scaleWindow   OPTIONAL {min,max} in ms — the current x scale range —
 *                 plus an optional yMax: the CURRENT y scale top (the view
 *                 passes u.scales.y.max, so the decision below respects the
 *                 view-level yMax hysteresis, not just the per-payload
 *                 policy).
 *                 When given: rects are CLAMPED to it (visual clip) and
 *                 dropped when fully outside; vlines outside it are DROPPED,
 *                 never clamped — dragging the escalation edge into view
 *                 would fabricate its position; and an N-CPUs line above
 *                 yMax becomes an 'ncpus-offscale' TOP-EDGE affordance
 *                 ("N CPUs ↑") instead of a silently dropped reference
 *                 (P7 — the capacity reference must never vanish; drawing
 *                 the line itself at the rim would lie about where capacity
 *                 sits, so only the labeled arrow chip is painted). When
 *                 null: absolute, viewport-independent geometry (what
 *                 spec.overlays holds; the painters also clip to the plot
 *                 box, so either form renders correctly).
 *
 * Returns {
 *   rects:  [{ kind:'sampled'|'mixed'|'escalation', x0, x1 (ms),
 *              fill, stroke, dash }],          // full plot height
 *   vlines: [{ kind:'escalation-edge', x (ms), color, label }],
 *   hlines: [{ kind:'ncpus', y (AAS), color, dash, label } |
 *            { kind:'ncpus-offscale', y (= scale top), color, label }],
 * }
 *
 * Whole-response fidelity bands anchor to axisRange (opts.axis ∪ win — the
 * full rendered strip extent), exactly like the ECharts markArea did: any
 * data the camera can pan onto is shaded wherever it is. The escalation
 * band still covers ONLY the observed span (UI-10: no observed start -> no
 * band, just the labeled edge — never invent a duration the server didn't
 * report). */
export function overlayGeometry(data, opts, scaleWindow) {
    opts = opts || {};
    const buckets = (data && data.buckets) || [];
    const xMinNs = buckets.length ? buckets[0].t : 0;
    const xMaxNs = buckets.length ? buckets[buckets.length - 1].t : NS_PER_MS;
    const win = opts.win || { from: xMinNs, to: xMaxNs };
    let axisFromNs = (opts.axis && opts.axis.from != null) ? opts.axis.from : win.from;
    let axisToNs = (opts.axis && opts.axis.to != null) ? opts.axis.to : win.to;
    if (win.from < axisFromNs) axisFromNs = win.from;
    if (win.to > axisToNs) axisToNs = win.to;
    if (!(axisToNs > axisFromNs)) axisToNs = axisFromNs + NS_PER_MS;

    const rects = [];
    const vlines = [];
    const hlines = [];

    const shading = buildFidelityShading(data, { from: axisFromNs, to: axisToNs });
    for (const b of shading.bands) {
        const mixed = b.kind === 'mixed';
        rects.push({
            kind: b.kind,
            x0: nsToMs(b.from != null ? b.from : axisFromNs),
            x1: nsToMs(b.to != null ? b.to : axisToNs),
            fill: mixed ? MIXED_BAND_COLOR : SAMPLED_BAND_COLOR,
            stroke: mixed ? MIXED_BORDER : SAMPLED_BORDER,
            dash: [4, 4],
        });
    }

    const esc = buildEscalationAnnotation(opts.escalationStatus,
        { from: win.from, to: win.to });
    if (esc) {
        const border = esc.isAnomaly ? ESC_ANOMALY_BORDER : ESC_MANUAL_BORDER;
        if (esc.from != null && esc.to != null && esc.to > esc.from) {
            rects.push({
                kind: 'escalation',
                x0: nsToMs(esc.from),
                x1: nsToMs(esc.to),
                fill: esc.isAnomaly ? ESC_ANOMALY_COLOR : ESC_MANUAL_COLOR,
                stroke: border,
                dash: null,
            });
        }
        if (esc.to != null) {
            vlines.push({
                kind: 'escalation-edge',
                x: nsToMs(esc.to),
                color: border,
                label: esc.label,
            });
        }
    }

    if (opts.numCpus > 0) {
        hlines.push({
            kind: 'ncpus',
            y: opts.numCpus,
            color: NCPUS_COLOR,
            dash: [4, 4],
            label: opts.numCpus + ' CPUs',
        });
    }

    if (scaleWindow && scaleWindow.min != null && scaleWindow.max != null) {
        const lo = scaleWindow.min, hi = scaleWindow.max;
        const clamped = [];
        for (const r of rects) {
            const x0 = Math.max(r.x0, lo);
            const x1 = Math.min(r.x1, hi);
            // Mark viewport-cut sides: a border stroke at a CUT edge would
            // draw a "band boundary" where the band actually continues past
            // the viewport rim (U2b review F3) — the painter skips those.
            if (x1 > x0) clamped.push(Object.assign({}, r, {
                x0, x1,
                clampedLeft: x0 > r.x0,
                clampedRight: x1 < r.x1,
            }));
        }
        rects.length = 0;
        rects.push(...clamped);
        const kept = vlines.filter(l => l.x >= lo && l.x <= hi);
        vlines.length = 0;
        vlines.push(...kept);
        // hlines are y-space: unaffected by the x viewport — EXCEPT the P7
        // off-scale transform: with the CURRENT y scale top supplied, an
        // N-CPUs reference above it turns into the explicit top-edge
        // affordance (label-only; the painter draws no line for it — a line
        // at the rim would claim capacity sits there).
        if (scaleWindow.yMax != null) {
            for (let i = 0; i < hlines.length; i++) {
                const l = hlines[i];
                if (l.kind === 'ncpus' && l.y > scaleWindow.yMax) {
                    hlines[i] = {
                        kind: 'ncpus-offscale',
                        y: scaleWindow.yMax,
                        color: l.color,
                        dash: null,
                        label: opts.numCpus + ' CPUs ↑',
                    };
                }
            }
        }
    }

    return { rects, vlines, hlines };
}

/* ── Draw hooks (thin canvas painters over the pure geometry) ────────────────
 *
 * Impure by nature (canvas 2D, device pixels). They only translate the
 * geometry above via u.valToPos and clip to u.bbox — no data logic lives
 * here. Rect painting belongs BEHIND the stacked areas (hook 'drawAxes':
 * after grid, before series — the ECharts markArea layering); lines paint
 * ABOVE the stack (hook 'draw': after everything — the markLine layering).
 */

/* Fidelity/escalation band rects, full plot height, clipped to the plot box. */
export function drawOverlayRects(u, geo) {
    if (!geo || !geo.rects || !geo.rects.length) return;
    const ctx = u.ctx;
    const { left, top, width, height } = u.bbox;
    ctx.save();
    ctx.beginPath();
    ctx.rect(left, top, width, height);
    ctx.clip();
    for (const r of geo.rects) {
        let x0 = u.valToPos(r.x0, 'x', true);
        let x1 = u.valToPos(r.x1, 'x', true);
        if (x1 < left || x0 > left + width) continue;
        // Pixel clamp: unclamped absolute geometry may sit far off-view;
        // huge coords are legal but pointless — the clip already bounds
        // the visible result.
        x0 = Math.max(x0, left - 2);
        x1 = Math.min(x1, left + width + 2);
        ctx.fillStyle = r.fill;
        ctx.fillRect(x0, top, x1 - x0, height);
        if (r.stroke && !(r.clampedLeft && r.clampedRight)) {
            // Border strokes mark TRUE band boundaries only: a side the
            // viewport clamp cut (clampedLeft/Right) is not a boundary —
            // the band continues past the rim (U2b review F3).
            ctx.strokeStyle = r.stroke;
            ctx.lineWidth = 1;
            ctx.setLineDash(r.dash || []);
            ctx.beginPath();
            if (!r.clampedLeft) {
                ctx.moveTo(x0, top);
                ctx.lineTo(x0, top + height);
            }
            if (!r.clampedRight) {
                ctx.moveTo(x1, top);
                ctx.lineTo(x1, top + height);
            }
            ctx.stroke();
            ctx.setLineDash([]);
        }
    }
    ctx.restore();
}

/* Escalation live-edge vline(s) + N-CPUs hline, with their label chips. An
 * off-viewport vline is simply not painted (its x never moves — honesty). */
export function drawOverlayLines(u, geo) {
    if (!geo) return;
    const vlines = geo.vlines || [];
    const hlines = geo.hlines || [];
    if (!vlines.length && !hlines.length) return;
    const ctx = u.ctx;
    const { left, top, width, height } = u.bbox;
    const dpr = (typeof uPlot !== 'undefined' && uPlot.pxRatio) ||
        (typeof devicePixelRatio !== 'undefined' ? devicePixelRatio : 1);
    const pad = 2 * dpr;
    ctx.save();
    ctx.font = (10 * dpr) + 'px sans-serif';
    ctx.textBaseline = 'top';
    for (const l of vlines) {
        const x = u.valToPos(l.x, 'x', true);
        if (x < left || x > left + width) continue;
        ctx.strokeStyle = l.color;
        ctx.lineWidth = dpr;
        ctx.setLineDash([]);
        ctx.beginPath();
        ctx.moveTo(x, top);
        ctx.lineTo(x, top + height);
        ctx.stroke();
        if (l.label) {
            const w = ctx.measureText(l.label).width + 2 * pad;
            // Label sits just left of the edge line (the edge is usually the
            // window's right rim), inside the plot.
            const lx = Math.max(left, x - w);
            ctx.fillStyle = l.color;
            ctx.fillRect(lx, top, w, 12 * dpr + 2 * pad);
            ctx.fillStyle = '#fff';
            ctx.fillText(l.label, lx + pad, top + pad);
        }
    }
    for (const l of hlines) {
        const y = u.valToPos(l.y, 'y', true);
        if (y < top || y > top + height) continue;
        // P7 off-scale affordance: label chip only, at the top edge — never a
        // line (the reference is ABOVE the scale; a rim line would lie).
        const lineless = l.kind === 'ncpus-offscale';
        if (!lineless) {
            ctx.strokeStyle = l.color;
            ctx.lineWidth = dpr;
            ctx.setLineDash((l.dash || []).map(d => d * dpr));
            ctx.beginPath();
            ctx.moveTo(left, y);
            ctx.lineTo(left + width, y);
            ctx.stroke();
            ctx.setLineDash([]);
        }
        if (l.label) {
            const w = ctx.measureText(l.label).width + 2 * pad;
            const lh = 12 * dpr + 2 * pad;
            const lx = left + width - w;
            const ly = Math.max(top, y - lh);
            ctx.fillStyle = l.color;
            ctx.fillRect(lx, ly, w, lh);
            ctx.fillStyle = '#fff';
            ctx.fillText(l.label, lx + pad, ly + pad);
        }
    }
    ctx.restore();
}

/* Hook bundle for the mount layer. getGeo(u) must return the CURRENT
 * geometry — typically overlayGeometry(latestData, latestOpts,
 * { min: u.scales.x.min, max: u.scales.x.max }) from a closure over the
 * latest strip payload, so the honesty overlays track every camera state
 * AND every data swap without reconstructing the chart. Merge into
 * uplotOpts.hooks before construction. */
export function overlayHooks(getGeo) {
    return {
        drawAxes: [(u) => drawOverlayRects(u, getGeo(u))],
        draw: [(u) => drawOverlayLines(u, getGeo(u))],
    };
}
