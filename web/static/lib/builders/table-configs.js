/* pgwt — table column configs for the migrated table views.
 *
 * Each config is { columns, rowClass?, onClick? } in the shape lib/table.js
 * consumes. They are declarative and pure (cell formatters return HTML
 * strings), so buildTableModel() over a config is Node-testable. onClick here
 * is a *descriptor* — it returns the drill intent { filterKey, filterValue,
 * label } rather than performing navigation, keeping the config free of app
 * wiring. The view translates the intent into filters.drill(). A column may
 * additionally carry `intent(row) -> intent|null` — a per-CELL drill
 * descriptor (U2 wire 6: the events percentile cells emit the
 * 'histogram-event' pivot); lib/table.js marks such cells drillable and the
 * view routes them through ctx.onDrill like every other intent.
 *
 * Used by the migrated overview, events, sessions and queries views (the
 * drill-down tables all share lib/table.js via these configs).
 */

import {
    WAIT_CLASSES, classColor, classFilterName, eventColor,
    fmtMs, fmtUs, fmtCount, fmtPct, fmtAas, esc,
} from '../format.js';
import { dot, pctBar, stackedBar, eventStackedBar, createStableOrder } from '../table.js';

import { backendActivityHtml } from './backend-span.js';

const _dot = (name) => dot(name, classColor);

function fmtSignedMs(ms) {
    if (!Number.isFinite(ms)) return '—';
    if (ms === 0) return fmtMs(0);
    return (ms > 0 ? '+' : '−') + fmtMs(Math.abs(ms));
}

function fmtRatio(r) {
    if (r.status) return '<span class="delta-status ' + r.status + '">' +
        r.status + '</span>';
    if (!(r.ratio > 0) || !Number.isFinite(r.ratio)) return '—';
    return r.ratio.toFixed(2) + '×';
}

const deltaColumns = [
    { key: 'a_ms', label: 'A', cls: 'num', format: r => r._a ? fmtMs(r.a_ms) : '—' },
    { key: 'b_ms', label: 'B', cls: 'num', sortable: false,
      format: r => r._b ? fmtMs(r.b_ms) : '—' },
    { key: 'abs_delta_ms', label: 'Δ', cls: 'num', format: r =>
        '<span class="delta-value ' + (r.delta_ms >= 0 ? 'grew' : 'shrank') + '">' +
        fmtSignedMs(r.delta_ms) + '</span>' },
    { key: 'ratio', label: '×', cls: 'num', format: fmtRatio },
];

/* U2 wire 6 (review P3): the events table's percentile cells drill into the
 * latency-distribution view for THAT event. The intent is the app's pivot
 * shape ({pivot:'histogram-event'} + a normal event_id filter drill); it is
 * null — cell not drillable — when the value itself is null (sampled-only
 * row: no exact durations, no distribution to show, FID-3) or for CPU*
 * (event_id 0 is not a wait event; the histogram is a wait-latency view and
 * an event_id=0 filter means "unfiltered" server-side). */
function histogramIntent(key) {
    return (r) => {
        if (!(r.event_id > 0) || r[key] == null) return null;
        return { pivot: 'histogram-event', filterKey: 'event_id',
                 filterValue: r.event_id, label: r.name };
    };
}

/* Percentile cell: the value, wrapped in a visible drill affordance when the
 * cell is drillable (same predicate as histogramIntent).
 *
 * SEMANTICS (issue #103): the server derives P50/P95/P99 from a 16-bucket
 * latency histogram whose LAST bucket is open-ended (every wait of
 * 16.384 ms or more). A percentile that lands there is a LOWER BOUND, not a value —
 * printing it bare produced rows reading "Avg 1.8s / P50=P95=P99=16.4ms",
 * a mean 100x above the stated P99. Such a cell renders ">=16.4ms" and says
 * why in its tooltip; the exact tail lives in Max. The flag is data-driven
 * (`<pctl>_overflow` per row, src/server.c handle_top_events) because it
 * cannot be inferred here: bucket 14 (8.2..16.4 ms) reports the same 16384
 * as the open-ended bucket 15.
 *
 * The number itself is unchanged, so the column still SORTS on the bound
 * (row.p95_us stays 16384): the sort key must mean what the cell shows, and
 * ranking every overflow row at +inf would both tie them arbitrarily and
 * order them against a number the user cannot see. Max is the column that
 * ranks the true tail.
 *
 * #437 (contract item 4): the tooltip carries the exact agreed sentence,
 * verbatim. The pre-#437 wording ("the true value is higher") wrongly
 * EXCLUDED equality — a percentile sitting exactly at the bucket boundary is
 * not "higher than itself", its actual value is unavailable. The same
 * sentence is also shown BESIDE the table (buildOverflowNote/overflowNoteHtml
 * below), not only on hover: a tooltip is undiscoverable until someone
 * hovers the exact cell, and this fact changes how every percentile in the
 * table should be read.
 *
 * #454: #437 only qualified the OPEN-ended bucket. Every OTHER (finite)
 * bucket is exactly as approximate — hist_percentile() (src/compute.c:1718)
 * ALWAYS returns a bucket's upper edge, never the exact observed duration,
 * for every one of the 16 buckets, not just the last. The live capture that
 * opened #454 was a Count=1 row: Max ~1.0 ms (the one exact observation) next
 * to P50/P95/P99 all reading the bucket-10 edge 2048 us (~2.0 ms) bare — a
 * percentile printed BIGGER than the row's own exact Max, with nothing
 * marking it as a rounded bound rather than a measurement. Finite buckets
 * now render "<=" (vs #437's ">=" for the open-ended tail) so the two
 * directions of approximation read as what they are: a finite bucket's true
 * value is AT OR BELOW its edge, the open bucket's true value is AT OR
 * ABOVE 16.384 ms. Bucket 14 (finite, 8.2..16.384 ms) and bucket 15
 * (open-ended) can report the identical 16384 number; only the qualifier
 * (driven by `<pctl>_overflow`, same as #437) tells them apart. */
export const PCTL_OVERFLOW_SENTENCE =
    'Percentiles marked ≥16.4 ms fall in the open-ended ≥16.384 ms ' +
    'histogram bucket; they are lower bounds, and their actual values are ' +
    'unavailable.';

export const PCTL_FINITE_BOUND_SENTENCE =
    'Percentiles marked ≤ are the upper edge of the histogram bucket the ' +
    'value fell in, not the exact observed duration; a rounded bound can ' +
    'read above Max when few samples exist. The exact tail lives in Max.';

function pctlCell(key) {
    const intent = histogramIntent(key);
    const overflowKey = key.replace(/_us$/, '_overflow');
    return (r) => {
        if (r[key] == null) return fmtUs(r[key]);   // '—': no distribution
        const overflow = r[overflowKey] === true;
        const v = (overflow ? '≥' : '≤') + fmtUs(r[key]);
        const drillable = !!intent(r);
        const sentence = overflow ? PCTL_OVERFLOW_SENTENCE : PCTL_FINITE_BOUND_SENTENCE;
        const title = sentence + (drillable
            ? ' Click to see its latency distribution.' : '');
        /* Drillable cells keep the inline drill affordance; a bound on a
         * non-drillable row gets the quieter .pctl-overflow underline
         * (style.css, mirrored in dev/gallery.html) so it still reads as
         * inspectable rather than as flat text. #454: every percentile is
         * now a bound (finite or open-ended), so this span is no longer
         * conditional on overflow -- the plain, unqualified return path is
         * gone. */
        return '<span class="' + (drillable ? 'cell-drill' : 'pctl-overflow') +
               '"' + (drillable ? ' style="border-bottom:1px dotted #778"' : '') +
               ' title="' + esc(title) + '">' + v + '</span>';
    };
}

/* #435/#425 (contract item 1): Idle is categorically excluded from DB Time
 * (docs/AAS_SEMANTICS_DECISION.md:10/:43/:171) — not a row whose share
 * happens to compute to zero. src/server.c:3533 currently serializes that
 * exclusion as a numeric 0 (Events' handler already emits null for the same
 * fact, server.c:3661 — a recorded CONTRADICTION, src/ owner's to fix, not
 * this file's); this predicate makes the client-side presentation rule hold
 * regardless of which shape arrives, so the "meaningful 0%" bug cannot come
 * back if the server-side 0 reappears after a refactor. Matches the
 * existing idle-row finder in views/overview.js's buildSummary(). */
function isIdleRow(r) {
    return r.indent === 0 && typeof r.name === 'string' && r.name.indexOf('Idle') >= 0;
}

const IDLE_PCT_HTML =
    '<span class="idle-excluded" title="Excluded from DB Time — not a ' +
    'measured zero share of it.">—</span>';

/* #453: a row's %DB Time arrives as the server's PGWT_PCT_DB_IDLE sentinel,
 * which the wire serializes as JSON `null` (src/server.c:4069 for Overview's
 * time-model rows -- parent AND every indent-2 child alike, since
 * emit_idle_rows in src/compute.c stamps the SAME sentinel on the Idle
 * parent, its named children (e.g. "IO:DataFileRead" parked while idle) and
 * the "Other (background)" remainder row; src/server.c:3661/1865/3031 send
 * the identical sentinel for Events' idle-but-visible rows, e.g.
 * Client:ClientRead). `pct == null` is therefore the one wire-level signal
 * that is true for "excluded from DB Time" everywhere it can occur, and
 * nowhere else (a real class/event with a genuine 0% share sends numeric
 * 0, never null) -- unlike `isIdleRow` above, which only matches the
 * Overview PARENT row by name/indent and silently let the parent's own
 * children, and every row in the Events tab, fall through to the plain
 * formatter below.
 *
 * `|| isIdleRow(r)` keeps the SAME defense isIdleRow exists for above
 * (the %DB Time column, two lines up, cannot drop it either): a parent row
 * can still arrive as the legacy numeric-0 shape (today's canned
 * tests/mock_server.py time_model, matching the documented server.c:3533
 * contradiction) rather than null. `pct == null` alone would read that row
 * as a genuine, included, zero-sharing row and print its idle occupancy
 * bare under "AAS" -- precisely the bug #453 exists to remove. */
function isExcludedFromDbTime(r) {
    return r.pct == null || isIdleRow(r);
}

const AAS_EXCLUDED_TITLE = 'Excluded from DB Time: this is idle occupancy ' +
    '(time ÷ window), not an active-session average — no session is ' +
    '"active" while idle.';

/* #453 (GIF-visible fix): the pre-fix cell printed the bare rate (e.g.
 * "24.62") under the "AAS" header with ONLY a hover tooltip relabeling it --
 * a still frame (GIF, screenshot) shows nothing but a number nineteen times
 * the real active-session load, indistinguishable from real AAS. The em-dash
 * is now the headline glyph (matches the %DB Time treatment, and is visible
 * with no interaction); the occupancy figure, when non-null, still prints
 * beside it, labeled, so the information is not lost -- just never printed
 * bare under an "AAS" header. */
function idleAasCellHtml(aas) {
    const dash = '<span class="idle-excluded" title="' +
        esc(AAS_EXCLUDED_TITLE) + '">—</span>';
    const v = fmtAas(aas);
    if (v === '—') return dash;
    return dash + ' <span class="idle-occ">(idle occ. ' + v + ')</span>';
}

export const overviewConfig = {
    columns: [
        { key: 'name', label: 'Stat Name', format: (r) => {
            if (r.indent >= 1) return _dot(r.name) + esc(r.name);
            return esc(r.name);
        }},
        { key: 'ms', label: 'Time', cls: 'num', format: (r) => fmtMs(r.ms) },
        { key: 'pct', label: '%DB Time', cls: 'num', format: (r) => {
            if (r.indent === 0 && r.name === 'DB Time') return fmtPct(r.pct);
            // #435: never a meaningful "0%" for idle -- it is not applicable,
            // not a measured zero share of DB Time.
            if (isIdleRow(r)) return IDLE_PCT_HTML;
            /* U2 (U1-review F4): event rows (indent 2) take the event's
             * identity tint so "same event = same color" holds against the
             * AAS breakdown and the wait-profile bars; class rows (indent 1)
             * keep the flat class hue — they ARE the class. */
            const color = (r.indent >= 2 ? eventColor(null, r.name)
                                         : classColor(r.name)) || '#4fc3f7';
            return pctBar(r.pct, color);
        }},
        { key: 'aas', label: 'AAS', cls: 'num', format: (r) => {
            // #453: the column header says "AAS" for every row, but any row
            // excluded from DB Time (Idle's parent row AND its own
            // indent-2 children -- "Other (background)", parked idle
            // events) has no active-session average: no session is
            // "active" while idle. A hover-only relabel does not survive a
            // still frame (GIF/screenshot), so the cell itself renders the
            // same em-dash the %DB Time column uses, with the occupancy
            // rate (when present) printed visibly beside it, not hidden
            // behind a tooltip.
            if (isExcludedFromDbTime(r)) return idleAasCellHtml(r.aas);
            return fmtAas(r.aas);
        }},
    ],
    rowClass: (r) => {
        let c = '';
        // #187: "CPU (waiting for a core)" is indent 1 like every other
        // class row, but onClick below never drills it (no server-side
        // class_name, no Events-table rows) — 'clickable' would paint a
        // cursor/hover affordance for a click that always does nothing.
        if (r.indent === 1) {
            c += ' indent-1';
            if (classFilterName(r.name) != null) c += ' clickable';
        }
        if (r.indent === 2) c += ' indent-2';
        return c;
    },
    onClick: (r) => {
        if (r.indent !== 1) return null;
        // #187: classFilterName strips the "*"/"(running)" display
        // qualifiers down to the server's class_name identity, and returns
        // null for "CPU (waiting for a core)" — a computed residual with no
        // server-side class_name and no Events-table rows to show.
        const cls = classFilterName(r.name);
        if (cls == null) return null;
        return { filterKey: 'class', filterValue: cls, label: cls };
    },
};

/* #440 (contract item 6): apply the refresh-ordering/near-tie hysteresis
 * (lib/table.js createStableOrder) to Overview's indent-1 "class" sibling
 * group, and independently to each sibling group's indent-2 children —
 * "preserving hierarchy" means a group's children move WITH it and are
 * reordered only against their OWN siblings, never promoted into a
 * different group or reshuffled relative to the indent-0 bookends.
 *
 * DB Time (always first) and Idle (wherever the server places it, normally
 * last — #435: it is excluded from DB Time, not a competing "class") are
 * indent-0 and are deliberately EXCLUDED from the RANKED pool: they are
 * structural bookends, not entries competing on contribution, and Idle's
 * own `ms` is routinely larger than any one class's (idle time often
 * dwarfs any single wait class) — ranking it in would float it to the top,
 * which is not what "preserving hierarchy" means here. Idle's own indent-2
 * children (e.g. Client:ClientRead) are still real rows that need a
 * parent, though, so bookends carry a children group exactly like classes
 * do -- they are just never themselves re-ranked.
 *
 * Bug found by review: an earlier version routed every indent-2 row to
 * "whichever class group was pushed last" (classes[classes.length-1]),
 * which is only correct while the CURRENT parent is in fact a class. The
 * moment an indent-0 row (Idle) follows a class, that check is still true
 * (classes is non-empty) even though the class is no longer the active
 * parent -- Idle's children attached to the PRECEDING class instead (wrong
 * parent, wrong position: rendered before Idle, under the wrong name), and
 * if no class had been seen at all yet, the same stale check (`classes.
 * length` falsy) silently DROPPED the row instead of attaching it
 * anywhere. Fixed below by tracking the single most-recently-seen group
 * explicitly (`active`), reassigned on every indent-0 OR indent-1 row:
 * children always attach to whichever parent is actually current, and a
 * row with truly no parent yet is never dropped (last-resort `pre` entry). */
function splitOverviewRows(rows) {
    const pre = [];                 // [{ row, children }] indent-0, before any indent-1
    const classes = [];             // [{ row, children }] indent-1 (the RANKED pool)
    const post = [];                // [{ row, children }] indent-0, after the first indent-1
    let active = null;              // the group indent-2 rows currently attach to
    for (const r of rows || []) {
        if (r.indent === 1) {
            active = { row: r, children: [] };
            classes.push(active);
        } else if (r.indent === 0) {
            active = { row: r, children: [] };
            (classes.length ? post : pre).push(active);
        } else if (active) {
            active.children.push(r);
        } else {
            // Defensive only -- real server output always opens with an
            // indent-0 DB Time row first. Never silently drop a real row.
            pre.push({ row: r, children: [] });
        }
    }
    return { pre, classes, post };
}

/* PURE: flatten a list of { row, children } groups, applying (and lazily
 * creating/pruning) a per-group-identity child hysteresis tracker from
 * `childOrders` -- shared between the class pool and the pre/post bookends
 * so Idle's children get exactly the same nested-hysteresis treatment a
 * class's children get, keyed by the SAME map (row names are unique across
 * the whole table). */
function flattenOverviewGroups(groups, childOrders, out) {
    const seenNames = new Set(groups.map((g) => g.row.name));
    for (const name of Array.from(childOrders.keys())) {
        if (!seenNames.has(name)) childOrders.delete(name);
    }
    for (const g of groups) {
        out.push(g.row);
        if (!g.children.length) continue;
        let tracker = childOrders.get(g.row.name);
        if (!tracker) {
            tracker = createStableOrder({ idKey: (r) => r.name, valueKey: (r) => r.ms });
            childOrders.set(g.row.name, tracker);
        }
        for (const kid of tracker.apply(g.children)) out.push(kid);
    }
}

/* createOverviewOrder() returns { apply(rows)->orderedRows, reset() } — one
 * instance per mounted Overview table (view-local state, like
 * views/waterfall.js's `selected`); callers reset() it on an explicit
 * re-sort or a window/filter change (contract item 6). Child trackers are
 * created lazily per class identity and pruned when that class is no
 * longer present, so a class that comes back later starts that nested
 * group fresh rather than resuming stale history. */
export function createOverviewOrder() {
    const classOrder = createStableOrder({ idKey: (r) => r.name, valueKey: (r) => r.ms });
    const childOrders = new Map();   // row name (class OR bookend) -> createStableOrder()

    function reset() {
        classOrder.reset();
        childOrders.clear();
    }

    function apply(rows) {
        const { pre, classes, post } = splitOverviewRows(rows);
        const classByName = new Map(classes.map((g) => [g.row.name, g]));
        const orderedClassRows = classOrder.apply(classes.map((g) => g.row));
        // Re-attach each ranked row to ITS OWN children group (classOrder
        // only reorders rows, never their nested children) -- this is what
        // "preserving hierarchy" means: a class moves, but keeps its kids.
        const orderedClassGroups = orderedClassRows.map((r) => classByName.get(r.name));

        // ONE prune pass over every group that exists this tick (pre +
        // classes + post combined) -- pruning per-subset would wipe a
        // bookend's (or a class's) tracker just because it is not in
        // WHICHEVER subset happened to be flattened first.
        const out = [];
        flattenOverviewGroups(
            pre.concat(orderedClassGroups, post), childOrders, out);
        return out;
    }

    return { apply, reset };
}

export const eventsConfig = {
    columns: [
        { key: 'name', label: 'Wait Event', format: (r) => _dot(r.name) + esc(r.name) },
        { key: 'count', label: 'Count', cls: 'num', format: (r) => fmtCount(r.count) },
        { key: 'total_ms', label: 'Total', cls: 'num', format: (r) => fmtMs(r.total_ms) },
        { key: 'avg_us', label: 'Avg', cls: 'num', format: (r) => fmtUs(r.avg_us) },
        { key: 'p50_us', label: 'P50', cls: 'num', format: pctlCell('p50_us'),
          intent: histogramIntent('p50_us') },
        { key: 'p95_us', label: 'P95', cls: 'num', format: pctlCell('p95_us'),
          intent: histogramIntent('p95_us') },
        { key: 'p99_us', label: 'P99', cls: 'num', format: pctlCell('p99_us'),
          intent: histogramIntent('p99_us') },
        { key: 'max_us', label: 'Max', cls: 'num', format: (r) => fmtUs(r.max_us) },
        { key: 'pct', label: '%DB', cls: 'num', format: (r) => {
            /* Idle-but-visible events (e.g. Client:ClientRead) have time but
             * no meaningful share of DB Time; the server sends pct=null so we
             * render "—" instead of a bogus bar. */
            if (r.pct == null) return '<span style="color:#555">—</span>';
            /* U2 (U1-review F4): identity tint, not the flat class hue. */
            const color = eventColor(null, r.name) || '#4fc3f7';
            return pctBar(r.pct, color);
        }},
        { key: 'aas', label: 'AAS', cls: 'num', format: (r) => {
            // #453: same idle-exclusion rule as Overview's AAS column, same
            // pct==null signal (this table already uses it for the %DB
            // column two lines up) -- the pre-fix cell printed the bare
            // idle-occupancy rate (e.g. "19.62") under "AAS" with no visible
            // marker at all.
            if (isExcludedFromDbTime(r)) return idleAasCellHtml(r.aas);
            return fmtAas(r.aas);
        }},
    ],
    rowClass: () => 'clickable',
    onClick: (r) => ({ filterKey: 'event_id', filterValue: r.event_id, label: r.name }),
};

const PCTL_KEYS = ['p50_us', 'p95_us', 'p99_us'];

/* #437/#454: the disclosure beside the table, not only on hover. PURE — the
 * view (views/events.js) renders whatever this returns next to the mounted
 * table, the same spot it already uses for the percentile-basis footnote
 * (lib/panels.js's buildPercentileBasis/percentileBasisHtml), so a reader
 * sees it without hovering any one cell. Null only when the page has NO
 * percentile data at all (every p50/p95/p99 null -- e.g. a sampled-only
 * window, FID-3), since #454 made every finite percentile a bound too: there
 * is no longer a case with real percentile numbers and nothing to disclose.
 * Two independent facts, so both sentences can appear together (a page can
 * have both a finite-bucket row and an overflowing one, as the live #454
 * capture did). */
export function buildOverflowNote(rows) {
    rows = rows || [];
    let hasOverflow = false;
    let hasFiniteBound = false;
    for (const r of rows) {
        for (const key of PCTL_KEYS) {
            if (r[key] == null) continue;
            const overflowKey = key.replace(/_us$/, '_overflow');
            if (r[overflowKey] === true) hasOverflow = true;
            else hasFiniteBound = true;
        }
    }
    if (!hasOverflow && !hasFiniteBound) return null;
    const texts = [];
    if (hasFiniteBound) texts.push(PCTL_FINITE_BOUND_SENTENCE);
    if (hasOverflow) texts.push(PCTL_OVERFLOW_SENTENCE);
    return { text: texts.join(' ') };
}

export function overflowNoteHtml(model) {
    if (!model) return '';
    return '<div class="pctl-overflow-banner">' + esc(model.text) + '</div>';
}

export const sessionsConfig = {
    columns: [
        { key: 'pid', label: 'PID', format: (r) => r.pid },
        { key: 'type', label: 'Backend', format: (r) => esc(r.type || 'unknown') },
        { key: 'user', label: 'User', format: (r) => esc(r.user || '') },
        { key: 'db', label: 'Database', format: (r) => esc(r.db || '') },
        { key: 'db_time_ms', label: 'DB Time', cls: 'num', format: (r) => fmtMs(r.db_time_ms) },
        // #187: was a hardcoded literal that silently went stale when the
        // owner recoloured 'cpu' (dark green for "running"); classColor('cpu')
        // stays in sync with WAIT_CLASSES by construction.
        { key: 'cpu_pct', label: 'CPU%', cls: 'num', format: (r) => pctBar(r.cpu_pct, classColor('cpu') || 'rgb(28,120,62)') },
        { key: 'wait_pct', label: 'Wait%', cls: 'num', format: (r) => {
            const color = classColor(r.top_wait) || '#F44336';
            return pctBar(r.wait_pct, color);
        }},
        { key: 'top_wait', label: 'Top Wait', format: (r) => _dot(r.top_wait) + esc(r.top_wait) },
    ],
    rowClass: () => 'clickable',
    onClick: (r) => ({ filterKey: 'pid', filterValue: r.pid, label: 'PID ' + r.pid }),
};

const queriesOnClick = (r) => ({
    filterKey: 'query_id', filterValue: r.query_id,
    label: r.text ? r.text.substring(0, 40) : 'Query ' + r.query_id,
});

/* #441 (contract item 3): `count` counts non-idle WAIT **and CPU** records
 * (src/compute.c handle_top_queries) — a "Waits" label overstates what it
 * is. It is still meaningful (every exact row's full-duration average
 * below is over this SAME population), so it keeps a name, just an honest
 * one: "Wait/CPU intervals" for exact rows, where a continuation never adds
 * a new observation; "Observations" over sampled data, where each sample is
 * one, not an interval boundary. */
function queriesCountColumn(sampled) {
    return { key: 'count', label: sampled ? 'Observations' : 'Wait/CPU intervals',
        cls: 'num', format: (r) => fmtCount(r.count) };
}

/* #441: this average is full-duration-exact-only (src/server.c:3829/
 * compute.c:1741) over the SAME mixed wait+CPU population as `count` above
 * — "Avg Wait" is wrong twice: it is not waits-only, and it is not a
 * meaningful average once interval count only exists ("sampled continuations
 * add no new observations") rather than a fully exact duration. Never
 * invented by dividing DB Time by a different count (that would silently
 * swap in a number this aggregate does not actually have). */
function queriesAvgColumn(sampled) {
    return { key: 'avg_us', label: 'Avg interval', cls: 'num',
        format: (r) => sampled ? '—' : fmtUs(r.avg_us) };
}

/* #439 (contract item 1/3): `pct` divides by query-ATTRIBUTED active time
 * (src/compute.c:1691), excluding query_id-0 (unattributed) work — a
 * narrower, DIFFERENT denominator than Overview's total DB Time. "%DB"
 * implied the same denominator as every other tab; this name says which
 * one it actually is. The excluded time is disclosed separately, beside
 * the table (buildUnattributedNote/unattributedNoteHtml below), not folded
 * into this number and not silently dropped. */
const QUERIES_PCT_LABEL = '% attributed DB Time';

/* PURE: builds the queries table config for a given response fidelity
 * ('exact'|'sampled'|'mixed'|'none', see fidelity.js fidelityOf()). Only
 * `count`/`avg_us` depend on it; every other column is identical, so the
 * default export below (fidelity 'exact') is what the gallery and any
 * caller that does not have a live response still get. */
export function makeQueriesConfig(fidelity) {
    const sampled = fidelity === 'sampled' || fidelity === 'mixed';
    return {
        columns: [
            { key: 'query_id', label: 'Query ID', cls: 'sticky-col sticky-col-0', format: (r) =>
                '<span class="query-id">' + r.query_id + '</span>' },
            { key: 'text', label: 'Query Text', cls: 'sticky-col sticky-col-1', format: (r) => {
                if (!r.text) return '<span style="color:#555">—</span>';
                const truncated = esc(r.text.substring(0, 80)) +
                    (r.text.length > 80 ? '...' : '');
                return '<span class="qt-hover" data-fulltext="' +
                    esc(r.text).replace(/"/g, '&quot;') + '">' +
                    _dot(r.top_wait) + truncated + '</span>';
            }},
            { key: '_executions', label: 'Waterfall', sortable: false,
              cls: 'query-executions-cell',
              format: () => '<span class="cell-drill query-executions-action" ' +
                  'title="Open individual executions">Executions →</span>',
              intent: (r) => ({ pivot: 'query-executions', filterKey: 'query_id',
                  filterValue: r.query_id,
                  label: r.text ? r.text.substring(0, 40) : 'Query ' + r.query_id }) },
            { key: 'total_ms', label: 'DB Time', cls: 'num', format: (r) => fmtMs(r.total_ms) },
            /* Reviewer finding: `Closed · 0.4 s` renders directly beside
             * `DB Time 4.2 s`, and nothing in the cell or the header said one
             * is an OBSERVED WALL-CLOCK SPAN of one backend's activity while
             * the other is a SUMMED wait + CPU total across every execution.
             * Read as "another time next to DB Time" it trades #455's missing
             * 714 s for a new ambiguity, so the qualifier is in the header
             * text itself -- not left to be inferred from the drill's
             * Measurement/Completion wording. */
            { key: 'backend_activity_span', sortable: false,
              label: 'Backend activity <span class="col-qualifier" title="' +
                  'One backend&#39;s observed wall-clock activity span, from ' +
                  'CMD_START/CMD_END timestamps. NOT comparable to DB Time, ' +
                  'which sums wait and CPU time across every execution of ' +
                  'this query, and never an average.">(observed span)</span>',
              cls: 'backend-activity-cell', format: (r) => backendActivityHtml(r) },
            { key: 'pct', label: QUERIES_PCT_LABEL, cls: 'num', format: (r) => fmtPct(r.pct) },
            { key: 'classes', label: 'Wait Profile', format: (r) =>
                r.events ? eventStackedBar(r.events, r.total_ms, eventColor, fmtMs)
                         : stackedBar(r.classes, r.total_ms, WAIT_CLASSES, fmtMs) },
            { key: 'exec_count', label: 'Execs', cls: 'num', format: (r) =>
                r.exec_count != null ? fmtCount(r.exec_count) : '—' },
            { key: 'plan_count', label: 'Plans', cls: 'num', format: (r) =>
                r.plan_count != null ? fmtCount(r.plan_count) : '—' },
            { key: 'plan_ratio', label: 'Plan%', cls: 'num', format: (r) =>
                (r.exec_count > 0 && r.plan_count != null)
                    ? (r.plan_count / r.exec_count * 100).toFixed(0) + '%' : '—' },
            { key: 'avg_exec_ms', label: 'Avg Exec', cls: 'num', format: (r) =>
                r.avg_exec_ms != null ? fmtMs(r.avg_exec_ms) : '—' },
            { key: 'p95_exec_ms', label: 'p95 Exec', cls: 'num', format: (r) =>
                r.p95_exec_ms != null ? fmtMs(r.p95_exec_ms) : '—' },
            { key: 'p99_exec_ms', label: 'p99 Exec', cls: 'num', format: (r) =>
                r.p99_exec_ms != null ? fmtMs(r.p99_exec_ms) : '—' },
            { key: 'avg_plan_ms', label: 'Avg Plan', cls: 'num', format: (r) =>
                r.avg_plan_ms != null ? fmtMs(r.avg_plan_ms) : '—' },
            { key: 'p95_plan_ms', label: 'p95 Plan', cls: 'num', format: (r) =>
                r.p95_plan_ms != null ? fmtMs(r.p95_plan_ms) : '—' },
            { key: 'p99_plan_ms', label: 'p99 Plan', cls: 'num', format: (r) =>
                r.p99_plan_ms != null ? fmtMs(r.p99_plan_ms) : '—' },
            queriesCountColumn(sampled),
            queriesAvgColumn(sampled),
        ],
        rowClass: () => 'clickable',
        onClick: queriesOnClick,
    };
}

/* Default/back-compat export: the gallery and any caller without a live
 * fidelity tag get the EXACT-tier config (today's behavior unchanged for
 * every existing use). views/queries.js calls makeQueriesConfig(fidelityOf
 * (data)) instead, so the live tab's headers track the real response. */
export const queriesConfig = makeQueriesConfig('exact');

/* #455: `total_ms` ("DB Time") sums EVERY wait/CPU interval attributed to
 * the query, including one taken before Exec ever starts -- a relation
 * lock acquired during parse analysis (src/ parser, before Plan/Exec) is
 * real DB Time but invisible to any Exec/Plan column, because those are
 * timed from the EXEC_START/EXEC_END (PLAN_START/PLAN_END) markers alone.
 * A query whose only cost is such a lock shows a large, correct "DB Time"
 * beside a tiny, ALSO correct "Avg Exec" -- two numbers that are each
 * individually honest but read as contradictory without this disclosure.
 * STATIC (not data-dependent): both column families are always present in
 * every fidelity tier (makeQueriesConfig only branches the LAST two
 * columns), so the scope gap exists on every row, not just an overflowing
 * one -- unlike buildOverflowNote/buildUnattributedNote above, there is no
 * "nothing to disclose this tick" case. Never used to manufacture a
 * latency number: the measured Avg Exec stays exactly what the server
 * measured, scoped by this sentence, not replaced by DB Time / Execs. */
export const QUERIES_SCOPE_NOTE_TEXT =
    'DB Time includes waits acquired before execution starts (e.g. a ' +
    'relation lock taken during parse analysis); Avg/p95/p99 Exec and ' +
    'Plan measure executor/planner time only and do not include it.';

export function queriesScopeNoteHtml() {
    return '<div class="exec-scope-note">' + esc(QUERIES_SCOPE_NOTE_TEXT) + '</div>';
}

/* #439 beside-table disclosure: how much foreground time no query claimed
 * (query_id 0), separate from the attributed total the table's own %
 * column (QUERIES_PCT_LABEL) divides by. PURE — same slot pattern as
 * buildOverflowNote above. `unattributed_available: false` (the
 * per-second-summary fast path, src/server.c) means the server genuinely
 * cannot say — that is reported as such, never silently as "none". */
export function buildUnattributedNote(data) {
    if (!data) return null;
    if (data.unattributed_available === false) {
        return { text: 'Unattributed foreground time is not available for ' +
            'this window (per-second summary tier has no unattributed bucket).' };
    }
    const ms = data.unattributed_ms;
    if (typeof ms !== 'number' || !Number.isFinite(ms) || ms < 0) return null;
    const count = typeof data.unattributed_count === 'number'
        ? data.unattributed_count : null;
    if (ms === 0 && (count == null || count === 0)) return null;
    const attributed = typeof data.db_time_ms === 'number' ? data.db_time_ms : null;
    const total = attributed != null ? attributed + ms : null;
    const pctText = (total != null && total > 0)
        ? ' (' + (ms / total * 100).toFixed(1) + '% of foreground DB Time)' : '';
    const countText = count != null
        ? ', ' + fmtCount(count) + ' occurrence' + (count === 1 ? '' : 's') : '';
    return { text: fmtMs(ms) + pctText + ' of foreground DB Time is not ' +
        'attributed to any query (query_id 0)' + countText + '.' };
}

export function unattributedNoteHtml(model) {
    if (!model) return '';
    return '<div class="unattributed-note">' + esc(model.text) + '</div>';
}

/* D3 compare-mode configs: identity + A | B | Δ | ×. The source entity's
 * normal drill identity rides on each joined delta row, so drills still act
 * on A exactly as they do outside compare. */
export const compareOverviewConfig = {
    columns: [{ key: '_entityLabel', label: 'Stat Name', sortable: false, format: r => {
        const dotName = r.indent >= 1 ? _dot(r.name) : '';
        return dotName + esc(r.name);
    }}].concat(deltaColumns),
    rowClass: r => {
        let c = r.indent === 1 ? ' indent-1 clickable' : '';
        if (r.indent === 2) c += ' indent-2';
        return c;
    },
    onClick: overviewConfig.onClick,
};

export const compareEventsConfig = {
    columns: [{ key: '_entityLabel', label: 'Wait Event', sortable: false, format: r =>
        _dot(r.name) + esc(r.name) }].concat(deltaColumns),
    rowClass: () => 'clickable',
    onClick: eventsConfig.onClick,
};

export const compareQueriesConfig = {
    columns: [
        { key: '_entityLabel', label: 'Query ID', sortable: false,
          cls: 'sticky-col sticky-col-0',
          format: r => '<span class="query-id">' + esc(r.query_id) + '</span>' },
        { key: 'text', label: 'Query Text', sortable: false,
          cls: 'sticky-col sticky-col-1',
          format: r => {
              if (!r.text) return '<span style="color:#555">—</span>';
              const truncated = esc(r.text.substring(0, 80)) +
                  (r.text.length > 80 ? '...' : '');
              return '<span class="qt-hover" data-fulltext="' +
                  esc(r.text).replace(/"/g, '&quot;') + '">' +
                  _dot(r.top_wait) + truncated + '</span>';
          }},
    ].concat(deltaColumns),
    rowClass: () => 'clickable',
    onClick: queriesConfig.onClick,
};
