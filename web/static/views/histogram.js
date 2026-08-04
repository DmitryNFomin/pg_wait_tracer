/* pgwt — "histogram" view: the latency-over-time heatmap + class/event selectors.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3). requests() fetches the event list (for the selectors) and the
 * heatmap (for the current filter state) on single-flight channels; build()
 * is PURE (lib/builders/histogram.js — heatmap option + selector model);
 * mount() paints the selector shell + feeds the option to the view-owned ECharts
 * instance. The view OWNS its ECharts instance: created lazily, disposed in
 * leave() — no module-level chart global. Window-resize is per-instance.
 *
 * U2 (P8 + P3 wires 4/7):
 *   - The class/event selectors WRITE THROUGH to the FilterStack via the
 *     ctx.onDrill intent surface. They are a projection of ctx.filters,
 *     never a second filter state — the old currentFilters() merged DOM
 *     <select> values over the filters at fetch time, invisible to
 *     breadcrumbs and clobbered by live ticks (P3's disease). Deleted.
 *   - <option> lists are never rebuilt under an open dropdown (a live tick
 *     landing mid-selection snapped the list shut).
 *   - visualMap max is STICKY: recomputed on window/filter change, grown (not
 *     shrunk) on live ticks — the sliding max recolored every cell every 5s.
 *   - Heatmap cell click emits the 'heatmap-cell' pivot intent through
 *     ctx.onDrill (time-bucket isolate; the app's registry routes it).
 */

import { buildHeatmapOption, buildSelectorModel } from '../lib/builders/histogram.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';
import { fmtCount } from '../lib/format.js';

const BUCKETS = () =>
    Math.min(Math.floor((typeof window !== 'undefined' ? window.innerWidth : 1800) / 6), 300);
const LIVE_TICK_NS = 5 * 1e9;

export function nextStickyMax(previous, raw, window) {
    const key = window.span + '|' + window.anchor + '|' + window.filters;
    const liveSlide = window.live && previous && previous.filters === window.filters &&
        previous.span === window.span && window.anchor > previous.anchor &&
        window.anchor - previous.anchor <= LIVE_TICK_NS;
    return {
        key, span: window.span, anchor: window.anchor, filters: window.filters,
        value: liveSlide ? Math.max(previous.value, raw) : raw,
    };
}

export function createHistogramView() {
    let chart = null;          // ECharts instance — owned here, nowhere else
    let el = null;             // the view's container
    let events = [];           // cached event list (drives the selectors)
    let ctxRef = null;         // last ctx (selector edits + cell clicks need it)
    let lastData = null;       // last heatmap payload (cell-click → bucket time)
    let vmaxState = null;      // {key,span,anchor,filters,value}, raw-count domain

    function fetchHeatmap(filters) {
        return ctxRef.transport.request(ctxRef.channel('heatmap'), 'heatmap', {
            from: ctxRef.timeRange.from, to: ctxRef.timeRange.to,
            buckets: BUCKETS(), filters,
        });
    }

    /* P8: sticky visualMap max. Its identity includes the window anchor, span,
     * and filters: only a same-span, same-filter FORWARD slide of at most one
     * 5 s live tick inherits the old ceiling. A pan/jump at the same span is a
     * new window and recomputes. Live ticks still grow immediately if their
     * own max exceeds the ceiling (never silently saturate), never shrink. */
    function stickyMax(data) {
        const span = ctxRef.timeRange.span();
        const anchor = ctxRef.timeRange.to;
        const filterKey = JSON.stringify(ctxRef.filters.snapshot());
        const raw = (data && data.max_count) || 1;
        vmaxState = nextStickyMax(vmaxState, raw,
            { span, anchor, filters: filterKey,
              live: ctxRef.isLiveTick ? ctxRef.isLiveTick() : false });
        return vmaxState.value;
    }

    /* P3 wire 4: heatmap cell click → the 'heatmap-cell' pivot intent through
     * ctx.onDrill — the ONE intent surface (registry + semantics live in
     * app.js: pause live, switch to the queries tab, zoomTo the bucket
     * window). The cell's identity scope (class/event) already lives in the
     * FilterStack — the selects write through to it — so the intent carries
     * only the TIME isolate; re-drilling the already-active event filter
     * would push a noise breadcrumb. */
    function onCellClick(p) {
        if (!ctxRef || !ctxRef.onDrill || !lastData || !p || !p.data) return;
        const t0 = (lastData.times || [])[p.data[0]];
        const bns = lastData.bucket_ns || 0;
        if (t0 == null || !(bns > 0)) return;
        ctxRef.onDrill({ pivot: 'heatmap-cell', from: t0, to: t0 + bns });
    }

    function renderHeatmap(data) {
        const host = document.getElementById('heatmap-container');
        if (!host) return;
        // EXACT-required (A3): heatmap is unavailable over a sampled-only window.
        if (isUnavailable(data)) {
            if (chart) { chart.dispose(); chart = null; }
            mountUnavailablePanel(host, data, ctxRef);
            return;
        }
        lastData = data;
        const model = buildHeatmapOption(data, { maxCount: stickyMax(data) });
        if (!model.hasData) {
            if (chart) { chart.dispose(); chart = null; }
            host.innerHTML = '<div class="loading">No data for selected event/range</div>';
            return;
        }
        if (!chart) {
            host.innerHTML = '';   // clear a leftover "No data" placeholder
            chart = ctxRef.echarts.init(host, 'dark');
            chart.on('click', onCellClick);
        }
        chart.setOption(model.option, true);
    }

    /* FilterStack write-through (P3 wire 7): a dropdown edit IS a filter
     * drill — it goes through the app's ONE intent surface (ctx.onDrill) so
     * every projection stays honest in the same breath: pause-live (P4),
     * FilterStack history + chip labels, breadcrumb/filter-bar repaint, URL
     * hash (P9), strip-cache invalidation, and the refresh that refetches
     * the heatmap under the standard epoch chokepoint. The 'histogram-event'
     * pivot is the histogram-DESTINATION intent (wire 6) — emitted from the
     * histogram itself it lands exactly here, carrying whichever dimension
     * the dropdown edited. Clearing a dimension (back to "All") names the
     * key(s) in removeKey; the APP owns every FilterStack mutation, including
     * the shared cache invalidation + history bookkeeping. */
    function emitSelectorEdit(filterKey, filterValue, label, removeKey) {
        if (!ctxRef || !ctxRef.onDrill) return;
        const intent = { pivot: 'histogram-event' };
        if (removeKey && removeKey.length) {
            intent.removeKey = removeKey.length === 1 ? removeKey[0] : removeKey;
        }
        if (filterKey) {
            intent.filterKey = filterKey;
            intent.filterValue = filterValue;
            intent.label = label;
        }
        ctxRef.onDrill(intent);
    }

    function onClassChange() {
        const cs = document.getElementById('hm-class');
        if (!cs || !ctxRef) return;
        // A class change always invalidates a narrower event selection.
        const removeKeys = [];
        if ('event_id' in ctxRef.filters.filters) removeKeys.push('event_id');
        if (cs.value) {
            emitSelectorEdit('class', cs.value, cs.value, removeKeys);
        } else {
            if ('class' in ctxRef.filters.filters) removeKeys.push('class');
            emitSelectorEdit(null, null, null, removeKeys);
        }
    }

    function onEventChange() {
        const es = document.getElementById('hm-event');
        if (!es || !ctxRef) return;
        const eid = es.value ? parseInt(es.value) : NaN;
        if (!isNaN(eid)) {
            const ev = events.find(e => e.event_id === eid);
            emitSelectorEdit('event_id', eid, ev ? ev.name : String(eid));
        } else {
            const removeKeys = 'event_id' in ctxRef.filters.filters ? ['event_id'] : [];
            emitSelectorEdit(null, null, null, removeKeys);
        }
    }

    /* Repopulate the <option>s of the class + event selects from `events`,
     * reflecting the FilterStack state (the selects never carry state of
     * their own). Called on every refresh — so it must NOT rebuild while the
     * user has a dropdown open: a live tick landing mid-selection snapped the
     * list shut (P8). The user-driven change path calls populateEventSelect()
     * directly and is not gated. */
    function populateSelectors() {
        const classSelect = document.getElementById('hm-class');
        const eventSelect = document.getElementById('hm-event');
        if (!classSelect || !eventSelect) return;
        const ae = typeof document !== 'undefined' ? document.activeElement : null;
        const model = buildSelectorModel(events, fmtCount);
        const f = ctxRef.filters.filters;

        if (ae !== classSelect) {
            classSelect.innerHTML = '<option value="">All</option>';
            for (const cls of model.classes) {
                const opt = document.createElement('option');
                opt.value = cls; opt.textContent = cls; classSelect.appendChild(opt);
            }
            classSelect.value = f.class || '';
        }
        if (ae !== eventSelect) populateEventSelect(f.class || '');
    }

    function populateEventSelect(selectedClass) {
        const classSelect = document.getElementById('hm-class');
        const eventSelect = document.getElementById('hm-event');
        if (!classSelect || !eventSelect) return;
        const model = buildSelectorModel(events, fmtCount);
        const selClass = selectedClass == null ? classSelect.value : selectedClass;
        const list = selClass ? (model.eventsByClass[selClass] || []) : model.allEvents;

        eventSelect.innerHTML =
            '<option value="">All' + (selClass ? ' ' + selClass : '') + '</option>';
        for (const ev of list) {
            const opt = document.createElement('option');
            opt.value = ev.event_id; opt.textContent = ev.label;
            eventSelect.appendChild(opt);
        }
        const f = ctxRef.filters.filters;
        eventSelect.value = f.event_id != null ? String(f.event_id) : '';
    }

    function ensureShell(container) {
        if (document.getElementById('heatmap-container')) return;
        container.innerHTML =
            '<div class="event-selector">' +
            '  <label>Class</label>' +
            '  <select id="hm-class"><option value="">All</option></select>' +
            '  <label>Event</label>' +
            '  <select id="hm-event"><option value="">All</option></select>' +
            '</div>' +
            '<div id="heatmap-container"></div>';
        document.getElementById('hm-class').addEventListener('change', () => {
            populateEventSelect();
            document.getElementById('hm-event').value = '';
            onClassChange();
        });
        document.getElementById('hm-event').addEventListener('change', onEventChange);
    }

    return {
        id: 'histogram',

        async requests(ctx) {
            ctxRef = ctx;
            const evData = await ctx.transport.request(ctx.channel('events'),
                'top_events', { from: ctx.timeRange.from, to: ctx.timeRange.to, filters: {} });
            events = (evData && evData.rows) || [];
            const hm = await fetchHeatmap(ctx.filters.snapshot());
            return { events, heatmap: hm };
        },

        build(data) {
            // PURE pre-compute; mount applies it (the DOM selectors still drive
            // a possible re-fetch, but the initial paint is from this model).
            return {
                selectors: buildSelectorModel(data.events, fmtCount),
                heatmap: buildHeatmapOption(data.heatmap),
                raw: data.heatmap,
            };
        },

        mount(container, model, ctx) {
            ctxRef = ctx;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            el = container;
            ensureShell(container);
            populateSelectors();
            renderHeatmap(model.raw);
        },

        enter(ctx) {
            ctxRef = ctx;   // chart is created lazily in renderHeatmap (needs the
                            // inner #heatmap-container element that mount builds)
        },

        leave() {
            if (chart) { chart.dispose(); chart = null; }
            el = null;
            lastData = null;
        },

        resize() { if (chart) chart.resize(); },
    };
}
