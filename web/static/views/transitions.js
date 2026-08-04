/* pgwt — "transitions" view: the directly-follows graph (DFG) + flow variants.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3 — the heaviest view, done last). requests() fetches transitions and
 * (optionally) variants on single-flight channels; build() is PURE
 * (lib/builders/transitions.js -> ECharts graph option + the variants HTML);
 * mount() maintains a stable shell (slider + DFG container + variants section)
 * and feeds options to the view-owned ECharts instance. The view OWNS its
 * instance: disposed in leave() — no module-level chart global.
 *
 * U2 lifecycle fixes (P5) — the old view was the churn poster child:
 *   - PERSISTED instance: init once, setOption per render. The old
 *     dispose/init/setOption(_,true) per mount replayed node pop-in and reset
 *     roam/pan on every refresh.
 *   - rAF-THROTTLED Simplify slider updating the live instance. The old
 *     unthrottled `input` handler ran dozens of full rebuild cycles per drag.
 *   - DRAGGED NODE POSITIONS survive refreshes: positions are read back from
 *     the instance before every re-layout (with a zr mouseup capture as the
 *     backstop for getOption lagging under layout:'none') and passed to the
 *     builder as overrides — `draggable:true` finally keeps its promise.
 *   - resize() re-runs the PIXEL LAYOUT (the builder bakes container dims
 *     into node coords), not just chart.resize().
 *
 * U2 drill wiring (P3 wire 5): node click → the 'dfg-event' pivot intent via
 * ctx.onDrill, carrying the server-emitted event_id (feature-detected).
 */

import {
    buildTransitionsOption, buildVariantsHtml,
} from '../lib/builders/transitions.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';
import { esc } from '../lib/format.js';

const DEFAULT_THRESHOLD = 20;

/* Preserve where inside the node the user grabbed it: the cursor delta moves
 * the BUILT centre by the same amount instead of snapping the centre under the
 * pointer when ECharts' getOption() lags the drag. */
export function applyDragOffset(built, grab, pointer) {
    if (!built || !grab || !pointer) return null;
    return { x: built.x + pointer.x - grab.x,
             y: built.y + pointer.y - grab.y };
}

export function createTransitionsView() {
    let chart = null;      // ECharts instance — owned here, nowhere else
    let dataRef = null;    // current transitions payload (slider/resize re-render)
    let ctxRef = null;     // last ctx (slider rAF + click pivot need it)
    let threshold = DEFAULT_THRESHOLD;  // survives refreshes AND tab switches
    let dragPos = {};      // nodeName -> {x,y}: user-dragged overrides (P5)
    let builtPos = {};     // nodeName -> {x,y} as last BUILT (drag detection)
    let down = null;       // pending node mousedown {name, x, y} (drag capture)
    let justDragged = false; // suppress the click that follows a drag
    let rafId = null;      // pending slider frame
    let resizePending = false;

    function disposeChart() {
        if (chart) { chart.dispose(); chart = null; }
        builtPos = {};
        down = null;
        justDragged = false;
    }

    function dfgDims() {
        const el = document.getElementById('dfg-container');
        return { width: (el && el.clientWidth) || 800, height: (el && el.clientHeight) || 550 };
    }

    /* P5: a node whose live x/y differs from what we last BUILT was dragged
     * by the user — record it so the next layout keeps it. Primary read-back
     * path, run before every re-layout. */
    function captureDragPositions() {
        if (!chart) return;
        let opt;
        try { opt = chart.getOption(); } catch (e) { return; }
        const nodes = opt && opt.series && opt.series[0] && opt.series[0].data;
        if (!nodes) return;
        for (const n of nodes) {
            const b = n && builtPos[n.name];
            if (!b || typeof n.x !== 'number' || typeof n.y !== 'number') continue;
            if (Math.abs(n.x - b.x) > 0.5 || Math.abs(n.y - b.y) > 0.5) {
                dragPos[n.name] = { x: n.x, y: n.y };
            }
        }
    }

    /* The backstop for getOption() lagging behind a drag under layout:'none':
     * read the node's position at mouseup — exact via getOption when it did
     * update, else approximated from the pointer pixel. */
    function readNodePosAfterDrag(name, pixel) {
        let opt;
        try { opt = chart.getOption(); } catch (e) { opt = null; }
        const nodes = opt && opt.series && opt.series[0] && opt.series[0].data;
        const n = nodes && nodes.find(nd => nd && nd.name === name);
        if (n && typeof n.x === 'number' && typeof n.y === 'number') {
            const b = builtPos[name];
            if (!b || Math.abs(n.x - b.x) > 0.5 || Math.abs(n.y - b.y) > 0.5) {
                return { x: n.x, y: n.y };   // getOption reflected the drag
            }
        }
        try {
            const grab = chart.convertFromPixel({ seriesIndex: 0 }, [down.x, down.y]);
            const pointer = chart.convertFromPixel({ seriesIndex: 0 }, pixel);
            const b = builtPos[name];
            if (b && grab && pointer && isFinite(grab[0]) && isFinite(grab[1]) &&
                isFinite(pointer[0]) && isFinite(pointer[1])) {
                return applyDragOffset(b,
                    { x: grab[0], y: grab[1] },
                    { x: pointer[0], y: pointer[1] });
            }
        } catch (e) { /* conversion unsupported — keep the built position */ }
        return null;
    }

    /* Slider input and container resize share one layout coalescer: both bake
     * fresh pixel dimensions into the option, at most once per frame. */
    function scheduleRender(resize) {
        resizePending = resizePending || resize;
        if (rafId != null) return;
        const raf = typeof requestAnimationFrame === 'function'
            ? requestAnimationFrame : (fn) => setTimeout(fn, 16);
        rafId = raf(() => {
            rafId = null;
            if (resizePending && chart) chart.resize();
            resizePending = false;
            renderDFG(threshold);
        });
    }

    function wireChartEvents() {
        // P3 wire 5: DFG node → the 'dfg-event' pivot intent through
        // ctx.onDrill (the ONE intent surface; the app's registry routes it
        // to the standard event drill — filter {event_id} + queries tab).
        // eventId comes from the server's node JSON (0 = the CPU* pseudo-
        // node, not a filterable wait event; null = a server predating the
        // field) — both refuse quietly, never throw.
        chart.on('click', (p) => {
            if (justDragged) return;                 // a drag is not a click
            if (!p || p.componentType !== 'series' || p.dataType === 'edge') return;
            const d = p.data || {};
            if (!ctxRef || !ctxRef.onDrill || d.eventId == null || d.eventId === 0) return;
            ctxRef.onDrill({
                pivot: 'dfg-event',
                filterKey: 'event_id', filterValue: d.eventId, label: d.name,
            });
        });
        // Drag capture (P5): arm on node mousedown, measure on zr mouseup.
        chart.on('mousedown', (p) => {
            justDragged = false;
            if (p && p.componentType === 'series' && p.dataType !== 'edge' &&
                p.event && p.data && p.data.name) {
                down = { name: p.data.name, x: p.event.offsetX, y: p.event.offsetY };
            }
        });
        chart.getZr().on('mouseup', (e) => {
            if (!down) return;
            const moved = Math.abs(e.offsetX - down.x) + Math.abs(e.offsetY - down.y) > 3;
            if (moved) {
                justDragged = true;   // the 'click' that follows must not pivot
                const pos = readNodePosAfterDrag(down.name, [e.offsetX, e.offsetY]);
                if (pos) dragPos[down.name] = pos;
            }
            down = null;
        });
    }

    function renderDFG(threshold_) {
        const host = document.getElementById('dfg-container');
        if (!host || !dataRef) return;
        captureDragPositions();   // P5: read back BEFORE re-layout
        const { option, visibleCount } =
            buildTransitionsOption(dataRef, threshold_, dfgDims(), dragPos);
        if (visibleCount === 0) {
            disposeChart();
            host.innerHTML =
                '<p style="color:#666;padding:40px;text-align:center">No transitions above threshold</p>';
            return;
        }
        if (!chart) {
            host.innerHTML = '';   // clear a possible above-threshold placeholder
            chart = ctxRef.echarts.init(host, 'dark');
            wireChartEvents();
        }
        builtPos = {};
        option.series[0].data.forEach(n => { builtPos[n.name] = { x: n.x, y: n.y }; });
        chart.setOption(option, true);   // full replace, NO dispose (P5)
    }

    /* P5: the slider/DFG/variants shell is built once and survives refreshes;
     * mount only updates the pieces that changed. Rebuilt lazily whenever
     * another view (or an unavailable/no-data mount) replaced the container. */
    function ensureShell(el) {
        if (document.getElementById('dfg-container')) return;
        disposeChart();   // shell is being rebuilt — any old chart DOM is gone
        el.innerHTML =
            '<div style="padding:10px 20px;display:flex;align-items:center;gap:12px">' +
                '<span style="color:#888;font-size:12px">Simplify:</span>' +
                '<input type="range" id="dfg-slider" min="0" max="100" value="' +
                    threshold + '" style="width:200px;accent-color:#4fc3f7">' +
                '<span id="dfg-slider-val" style="color:#888;font-size:12px">' +
                    threshold + '%</span>' +
                '<span id="dfg-total" style="color:#666;font-size:11px;margin-left:8px"></span>' +
            '</div>' +
            '<div id="dfg-container" style="width:100%;height:550px;background:#1a1a2e"></div>' +
            '<div id="dfg-variants"></div>';

        const slider = document.getElementById('dfg-slider');
        const sliderVal = document.getElementById('dfg-slider-val');
        // P5: rAF-throttled — the old handler ran a full dispose/init/layout
        // cycle per `input` event (dozens per drag). One frame, one setOption,
        // on the live instance.
        slider.addEventListener('input', () => {
            threshold = +slider.value;
            sliderVal.textContent = slider.value + '%';
            scheduleRender(false);
        });
    }

    return {
        id: 'transitions',

        async requests(ctx) {
            const data = await ctx.transport.request(ctx.channel('table'), 'transitions', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(), num_buckets: 200,
            });
            let variants = null;
            try {
                variants = await ctx.transport.request(ctx.channel('variants'), 'variants', {
                    from: ctx.timeRange.from, to: ctx.timeRange.to,
                    filters: ctx.filters.snapshot(), num_buckets: 20,
                });
            } catch (e) { /* variants optional */ }
            return { transitions: data, variants };
        },

        build(data) {
            const t = data.transitions;
            // EXACT-required (A3): a sampled-only window returns the structured
            // "unavailable" marker for transitions — show the escalate panel.
            if (isUnavailable(t)) return { unavailable: t };
            const hasLinks = !!(t && t.links && t.links.length > 0);
            return {
                transitions: t,
                hasLinks,
                total: (t && t.total) || 0,
                variantsHtml: data.variants ? buildVariantsHtml(data.variants, esc) : '',
            };
        },

        mount(el, model, ctx) {
            ctxRef = ctx;
            if (model.unavailable) {
                disposeChart();
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';

            if (!model.hasLinks) {
                disposeChart();
                el.innerHTML = '<p style="color:#888;padding:20px">No transitions found</p>';
                return;
            }
            dataRef = model.transitions;

            ensureShell(el);
            document.getElementById('dfg-total').textContent =
                Number(model.total).toLocaleString() + ' transitions';
            renderDFG(threshold);
            document.getElementById('dfg-variants').innerHTML = model.variantsHtml || '';
        },

        enter(ctx) { ctxRef = ctx; /* chart created lazily in renderDFG */ },

        leave() {
            if (rafId != null && typeof cancelAnimationFrame === 'function') {
                cancelAnimationFrame(rafId);
            }
            rafId = null;
            resizePending = false;
            disposeChart();
            dataRef = null;
            // dragPos survives leave() on purpose: hand-placed nodes are keyed
            // by event name, and the user's arrangement should greet them when
            // they tab back mid-investigation.
        },

        resize() {
            if (!chart) return;
            // P5: the builder bakes container pixel dims into node coords —
            // chart.resize() alone leaves a stale layout. Re-run the pixel
            // layout (dragged nodes survive via dragPos overrides), coalesced
            // with slider input and resize storms into one frame.
            scheduleRender(true);
        },
    };
}
