/* pgwt — explicit UI state.
 *
 * Replaces the old grab-bag global `state` object that was mutated mid-flight
 * by every code path. State here is split into three concerns, each with a
 * small explicit API:
 *
 *   - TimeRange      : the view window (from/to ns) + server clock + live span.
 *   - FilterStack    : current drill-down filters + breadcrumb history.
 *   - ServerInfo     : static server facts (cpus, trace bounds).
 *
 * Views receive exactly what they need (a snapshot) rather than reaching into
 * a shared mutable object. Nothing here touches the DOM or the network — it is
 * pure data, so it is Node-testable.
 */

const FIFTEEN_MIN_NS = 900 * 1e9;

/* Static server facts, set once on connect and refreshed on info ticks. */
export class ServerInfo {
    constructor() {
        this.fromNs = 0;      // earliest ts in the trace
        this.toNs = 0;        // latest ts in the trace
        this.nowNs = 0;       // server wall clock (live anchor)
        this.numCpus = 0;
        this.numEvents = 0;
        // Version handshake (T7 / TST-11): the server reports these in `info`.
        this.serverVersion = null;
        this.protocol = null;
    }

    update(info) {
        if (!info) return;
        if (info.from_ns) this.fromNs = info.from_ns;
        if (info.to_ns) this.toNs = info.to_ns;
        this.nowNs = info.now_ns || info.to_ns || this.nowNs;
        if (info.num_cpus != null) this.numCpus = info.num_cpus;
        if (info.num_events != null) this.numEvents = info.num_events;
        if (info.server_version != null) this.serverVersion = info.server_version;
        if (info.protocol != null) this.protocol = info.protocol;
    }
}

/* The view window. Single source of truth for from/to (ns). Zoom history and
 * live-span tracking live here too, but no timers (those belong to the
 * view-manager refresh loop). */
export class TimeRange {
    constructor(server) {
        this.server = server;
        this.from = 0;
        this.to = 0;
        this.zoomHistory = [];
        this.liveRangeSecs = 900;   // span used when live mode is on
    }

    /* Initialize to the default "last 15 min ending NOW" window. */
    initDefault() {
        const now = this.server.nowNs || this.server.toNs;
        this.from = now - FIFTEEN_MIN_NS;
        this.to = now;
        this.liveRangeSecs = 900;
    }

    set(from, to) {
        this.from = from;
        this.to = to;
    }

    /* Push the current window onto zoom history, then move to [from,to]. */
    zoomTo(from, to) {
        this.zoomHistory.push({ from: this.from, to: this.to });
        if (this.zoomHistory.length > 10) this.zoomHistory.shift();
        this.from = from;
        this.to = to;
    }

    /* Pop zoom history, or widen the window 2x clamped to trace bounds. */
    zoomOut() {
        if (this.zoomHistory.length > 0) {
            const prev = this.zoomHistory.pop();
            this.from = prev.from;
            this.to = prev.to;
            return;
        }
        const mid = (this.from + this.to) / 2;
        const span = this.to - this.from;
        this.from = Math.max(this.server.fromNs, mid - span);
        this.to = Math.min(this.server.toNs, mid + span);
    }

    /* Re-anchor a live window so it always ends at the latest server clock.
     * "Last N min always means NOW" — never anchor to stale data. */
    anchorLive(rangeSecs) {
        const end = this.server.nowNs || this.server.toNs;
        this.liveRangeSecs = rangeSecs;
        this.from = end - rangeSecs * 1e9;
        this.to = end;
    }

    span() { return this.to - this.from; }
}

/* Filters + breadcrumb stack for drill-down. Pure data transitions; callers
 * decide which view to show.
 *
 * U2 (review P3 wire 7): this stack is the SINGLE source of filter truth —
 * the filter-bar chips, the tab badges, the breadcrumb trail, and the URL
 * hash all render FROM it; none of them keeps a second filter state. To make
 * the chips readable it now also tracks a per-KEY label map (`labels`):
 * `filters` says WHAT is filtered, `labels[key]` says what to CALL it (e.g.
 * event_id 137 -> "IO:DataFileRead"). Labels are snapshotted into breadcrumbs
 * alongside the filters so drillUp restores both together. */
export class FilterStack {
    constructor() {
        this.filters = {};
        this.breadcrumbs = [];     // [{ label, filters, labels, view }]
        this.currentLabel = null;
        this.labels = {};          // filterKey -> human label (chip text)
    }

    snapshot() { return { ...this.filters }; }

    /* Drill into filterKey=filterValue. `fromView` is the view we drilled from
     * (recorded in the breadcrumb so drillUp can restore it). Returns the new
     * filters object. */
    drill(filterKey, filterValue, label, fromView) {
        this.breadcrumbs.push({
            label: this.currentLabel || '',
            filters: { ...this.filters },
            labels: { ...this.labels },
            view: fromView,
        });
        this.filters = { ...this.filters, [filterKey]: filterValue };
        this.currentLabel = label;
        if (label != null) this.labels = { ...this.labels, [filterKey]: label };
        return this.filters;
    }

    /* Restore the breadcrumb at `index`; returns its recorded view id. */
    drillUp(index) {
        const crumb = this.breadcrumbs[index];
        if (!crumb) return null;
        this.filters = { ...crumb.filters };
        this.labels = { ...(crumb.labels || {}) };
        this.currentLabel = crumb.label;
        this.breadcrumbs = this.breadcrumbs.slice(0, index);
        return crumb.view;
    }

    /* U2 (wire 7): drop ONE filter dimension — the filter-bar chip's ✕.
     * History is append-only reality: removing a dimension is a NEW
     * investigation step (the prior state is pushed as a breadcrumb), never a
     * rewrite of where the analyst has been — so the back-crumbs (and the
     * URL-state back button) still restore the removed filter faithfully.
     * Returns true if the key was present and removed. */
    removeFilter(filterKey, fromView) {
        if (!(filterKey in this.filters)) return false;
        this.breadcrumbs.push({
            label: this.currentLabel || '',
            filters: { ...this.filters },
            labels: { ...this.labels },
            view: fromView,
        });
        const nextF = { ...this.filters };
        delete nextF[filterKey];
        const nextL = { ...this.labels };
        delete nextL[filterKey];
        this.filters = nextF;
        this.labels = nextL;
        // No single "current" drill target anymore; the chip bar / breadcrumb
        // derive display text from labels/filters instead.
        this.currentLabel = null;
        return true;
    }

    /* U2 (P9): adopt a filter set wholesale — URL-hash hydration and popstate.
     * A deep link (or a back/forward jump) lands on a STATE, not a path: the
     * breadcrumb history is reset (the hash serializes filters, not the trail)
     * and labels fall back to key=value until the next real drill. */
    restore(filtersObj) {
        this.filters = { ...(filtersObj || {}) };
        this.breadcrumbs = [];
        this.labels = {};
        this.currentLabel = null;
    }

    clear() {
        this.filters = {};
        this.breadcrumbs = [];
        this.labels = {};
        this.currentLabel = null;
    }

    isEmpty() {
        return this.breadcrumbs.length === 0 &&
               Object.keys(this.filters).length === 0;
    }
}

// ── URL hash state (Track U Phase U2, review P9) ─────────────────────────────
//
// The investigation loop's "share" edge: {tab, window|live, filters, sort}
// serialized into location.hash so F5 restores, back/forward retrace, and
// "here's where the problem is — look" can be a link. Pure string codec
// (Node-tested in tests/web_unit/urlstate.test.mjs); app.js owns WHEN to
// write/read it. Deliberately hash-only — no router, no pushState paths.
//
// Format (query-string style, order fixed for canonical comparison):
//   #tab=events&live=1&span=900&f.class=IO&sort=total_ms.desc
//   #tab=timeline&from=1700000000000000000&to=1700000900000000000&f.pid=1234
//
// Two hard rules from the review:
//   - ns timestamps ride as STRINGS (they exceed 2^53; the client's float64
//     ns convention tolerates the 256 ns ulp, but the codec must never round
//     through JSON number formatting — String(Math.round(ns)) is exact for
//     2026 epochs, and parse is Number(str) back into the same float64).
//   - a restored live=1 re-anchors to NOW (live-means-NOW): the hash then
//     carries span, never from/to — a live link can NEVER pin stale time.
//
// Filter value types are restored per the wire contract server.c's
// parse_filters expects: event_id/pid are JSON numbers; class/query_id are
// strings (query_id explicitly — uint64 precision).

const HASH_FILTER_KEYS = { class: true, event_id: true, pid: true, query_id: true };
const HASH_NUMERIC_FILTER_MAX = { event_id: 0xffffffff, pid: 0x7fffffff };

/* state: { tab, live, spanSecs, fromNs, toNs, filters, sort } -> hash string
 * (no leading '#'). Canonical: same state always yields the same string. */
export function serializeHashState(s) {
    const parts = [];
    const push = (k, v) => parts.push(encodeURIComponent(String(k)) + '=' +
        encodeURIComponent(String(v)));
    if (s.tab) push('tab', s.tab);
    if (s.live) {
        push('live', 1);
        if (s.spanSecs) push('span', Math.round(s.spanSecs));
    } else if (s.fromNs != null && s.toNs != null) {
        push('from', String(Math.round(s.fromNs)));
        push('to', String(Math.round(s.toNs)));
    }
    const f = s.filters || {};
    for (const k of Object.keys(f).sort()) {
        if (f[k] == null) continue;
        push('f.' + k, f[k]);
    }
    if (s.sort && s.sort.key) {
        push('sort', s.sort.key + '.' + (s.sort.asc ? 'asc' : 'desc'));
    }
    return parts.join('&');
}

/* '#...' or '...' -> state object, or null for an empty/foreign hash.
 * Hostile input never throws: unparsable pairs are dropped, live wins over a
 * contradictory from/to (live-means-NOW — a crafted hash cannot pin a stale
 * window while claiming to be live). */
export function parseHashState(hash) {
    let raw = String(hash == null ? '' : hash);
    if (raw.startsWith('#')) raw = raw.slice(1);
    if (!raw) return null;
    const s = { tab: null, live: false, spanSecs: null,
                fromNs: null, toNs: null, filters: {}, sort: null };
    let any = false;
    for (const part of raw.split('&')) {
        const eq = part.indexOf('=');
        if (eq <= 0) continue;
        let k, v;
        try {
            k = decodeURIComponent(part.slice(0, eq));
            v = decodeURIComponent(part.slice(eq + 1));
        } catch (e) { continue; }   // malformed %-escape: drop the pair
        if (k === 'tab' && v) { s.tab = v; any = true; }
        else if (k === 'live') { s.live = v === '1' || v === 'true'; any = true; }
        else if (k === 'span') {
            const n = Number(v);
            if (isFinite(n) && n > 0) { s.spanSecs = Math.round(n); any = true; }
        } else if (k === 'from' || k === 'to') {
            const n = Number(v);
            if (isFinite(n) && n > 0) {
                s[k === 'from' ? 'fromNs' : 'toNs'] = n;
                any = true;
            }
        } else if (k.startsWith('f.') && k.length > 2) {
            const fk = k.slice(2);
            if (!HASH_FILTER_KEYS[fk]) continue;
            if (HASH_NUMERIC_FILTER_MAX[fk] != null) {
                const n = Number(v);
                if (Number.isInteger(n) && n >= 0 &&
                    n <= HASH_NUMERIC_FILTER_MAX[fk]) {
                    s.filters[fk] = n;
                    any = true;
                }
            } else {
                s.filters[fk] = v;   // class / query_id stay strings (wire contract)
                any = true;
            }
        } else if (k === 'sort' && v) {
            const dot = v.lastIndexOf('.');
            if (dot > 0) {
                s.sort = { key: v.slice(0, dot), asc: v.slice(dot + 1) === 'asc' };
                any = true;
            }
        }
    }
    if (!any) return null;
    if (s.live) { s.fromNs = null; s.toNs = null; }         // live means NOW
    else if (!(s.fromNs != null && s.toNs != null && s.toNs > s.fromNs)) {
        s.fromNs = null; s.toNs = null;                     // window incomplete
    }
    return s;
}

export { FIFTEEN_MIN_NS };
