/* pgwt — the one shared table / drill-down component.
 *
 * Every table view (overview, events, sessions, queries) describes itself with
 * a `config` (columns + rowClass + onClick) and hands the component its data
 * rows. The component is split into:
 *
 *   buildTableModel(config, rows, sort)  PURE: data -> { headers, rows[] } with
 *                                        cells pre-formatted to HTML strings and
 *                                        sort already applied. Node-testable.
 *   mountTable(el, config, model, opts)  thin: paint the model, wire header
 *                                        sort + row-click + query tooltip.
 *
 * A `config` column is: { key, label, cls?, format(row)->html }.
 * config.rowClass(row)->string and config.onClick(row) are optional.
 *
 * The HTML produced is identical to the old renderTable() in app.js so the
 * existing Playwright cell-value tests keep passing.
 */

import { esc, fmtCount } from './format.js';

/* Pure: apply sort, format every cell, produce a render model.
 *
 * U2 (review P3, wire 6): a column may carry `intent(row) -> intent|null` — a
 * per-CELL drill descriptor (same shape contract as config.onClick, including
 * the pivot form). Cells whose intent is non-null get the `drillable` class
 * and keep the intent on the cell model; mountTable wires them to
 * opts.onCellDrill with stopPropagation so the row-level drill does not also
 * fire. */
export function buildTableModel(config, rows, sort) {
    rows = rows ? rows.slice() : [];

    // Sort (callers pass null sort to keep server order, e.g. overview).
    if (sort && sort.key) {
        const asc = sort.asc;
        const key = sort.key;
        rows.sort((a, b) => {
            const va = a[key], vb = b[key];
            // Missing compare ratios (new/gone, or a real zero divisor) stay
            // at the end in BOTH directions. They are not +/-Infinity.
            if (va == null && vb != null) return 1;
            if (vb == null && va != null) return -1;
            if (va == null && vb == null) return 0;
            if (typeof va === 'number' && typeof vb === 'number')
                return asc ? va - vb : vb - va;
            return asc ? String(va).localeCompare(String(vb))
                       : String(vb).localeCompare(String(va));
        });
    }

    const headers = config.columns.map(col => ({
        key: col.key,
        label: col.label,
        cls: col.cls || '',
        sortable: col.sortable !== false,
        arrow: (sort && sort.key === col.key)
            ? (sort.asc ? ' ▲' : ' ▼') : '',
    }));

    const modelRows = rows.map(row => ({
        cls: config.rowClass ? config.rowClass(row) : '',
        row,
        cells: config.columns.map(col => {
            const intent = col.intent ? col.intent(row) : null;
            return {
                cls: (col.cls || '') + (intent ? ' drillable' : ''),
                html: col.format(row),
                intent,
            };
        }),
    }));

    return { headers, rows: modelRows };
}

/* ── #440: refresh ordering + near-tie hysteresis (contract item 6) ─────────
 *
 * A live table re-sorted by raw value on every tick jitters: two rows whose
 * values are within noise of each other swap rank every refresh, which reads
 * as random motion rather than as a real change. The agreed policy (NOT an
 * existing repository rule before this — draft, see the 1.05x constant
 * below): the FIRST render for a given identity set sorts by descending
 * value then stable identity; every later tick keeps the previous order
 * except where a row's current value is STRICTLY GREATER than 1.05x its
 * predecessor's — applied as deterministic adjacent (bubble) passes,
 * left-to-right, until no such inversion remains, so the result does not
 * depend on how many passes happen to run. A departed identity is dropped;
 * a new one is appended (ranked against the other newcomers by
 * value/identity) BEFORE the passes run, so a newcomer can still rise past
 * a weaker survivor in the same tick it appears.
 *
 * This is NOT folded into buildTableModel()/the `sort` param above: an
 * explicit column sort (the `sort` argument) is a request for the EXACT
 * displayed order and must never be dampened by hysteresis (docs/
 * VISUAL_CHECKLIST.md:23) -- callers reset() this tracker on an explicit
 * re-sort or a window/filter change, and only feed its output through
 * buildTableModel with sort=null (server/stable order) on every other tick.
 * Selection/clicks must key off the row's identity (whatever idKey(row)
 * returns), never its index in this order -- an index is exactly what a
 * hysteresis pass is free to change under a selected row.
 *
 * createStableOrder({ idKey(row)->string, valueKey(row)->number, ratio? })
 * returns { apply(rows)->orderedRows, reset() }. Pure except for its own
 * internal "previous tick" memory, which is exactly the state this policy
 * needs to carry — there is no hidden global, one instance per table. */
export const STABLE_ORDER_PASS_RATIO = 1.05;

function stableOrderValue(valueKey, row) {
    const v = row ? valueKey(row) : null;
    return typeof v === 'number' && Number.isFinite(v) ? v : -Infinity;
}

function stableOrderSortNewcomers(rows, idKey, valueKey) {
    return rows.slice().sort((a, b) => {
        const va = stableOrderValue(valueKey, a), vb = stableOrderValue(valueKey, b);
        if (va !== vb) return vb - va;
        return String(idKey(a)).localeCompare(String(idKey(b)));
    });
}

export function createStableOrder(opts) {
    const idKey = opts.idKey;
    const valueKey = opts.valueKey;
    const ratio = opts.ratio != null ? opts.ratio : STABLE_ORDER_PASS_RATIO;
    let prevIds = null;   // null = "no prior tick" (first render, or just reset())

    function reset() { prevIds = null; }

    function apply(rows) {
        rows = rows || [];
        if (!rows.length) { prevIds = []; return []; }
        const byId = new Map(rows.map(r => [idKey(r), r]));

        if (prevIds === null) {
            const ordered = stableOrderSortNewcomers(rows, idKey, valueKey);
            prevIds = ordered.map(idKey);
            return ordered;
        }

        const survivorIds = prevIds.filter((id) => byId.has(id));
        const survivorSet = new Set(survivorIds);
        const newcomers = stableOrderSortNewcomers(
            rows.filter((r) => !survivorSet.has(idKey(r))), idKey, valueKey)
            .map(idKey);
        const order = survivorIds.concat(newcomers);

        let changed = true;
        while (changed) {
            changed = false;
            for (let i = 0; i < order.length - 1; i++) {
                const predVal = stableOrderValue(valueKey, byId.get(order[i]));
                const curVal = stableOrderValue(valueKey, byId.get(order[i + 1]));
                if (curVal > predVal * ratio) {
                    const tmp = order[i]; order[i] = order[i + 1]; order[i + 1] = tmp;
                    changed = true;
                }
            }
        }

        prevIds = order;
        return order.map((id) => byId.get(id));
    }

    return { apply, reset };
}

/* Shared gate for the flat (non-hierarchical) drill-down tables
 * (Events/Sessions/Queries — Overview's hierarchical case has its own
 * wrapper, table-configs.js createOverviewOrder): an EXPLICIT column sort
 * (`sort.key` set — the user clicked a header) is a request for the exact
 * displayed order and bypasses the tracker entirely, never dampened by
 * hysteresis; the default (no explicit sort) view runs through it. Every
 * view's build() calls this the same way, so "a view stopped using the
 * tracker" is a one-line diff to spot and a one-line call to restore. */
export function applyDefaultOrder(tracker, rows, sort) {
    if (sort && sort.key) return rows || [];
    return tracker.apply(rows || []);
}

/* #474: the reset key a table view's createOrderResetGate call should use.
 * During a live tick (ctx.isLiveTick(), app.js:322 -- true while the
 * window is following "now") the window's absolute edges advance on every
 * refresh tick by design; keying the reset on those edges (the pre-#474
 * bug) resets the hysteresis tracker every tick, discarding it entirely in
 * the one mode it exists for. The key is instead {live:true, spanSecs} --
 * stable while live mode just slides forward, and only a genuine SPAN
 * change (the user picks a different "last N min") produces a new key.
 * Outside a live tick (manual navigation, zoom, or live mode off) the key
 * is the raw window, unchanged from pre-#474 behavior -- any edge move
 * there IS a real navigation and must reset. */
export function orderResetKey(ctx) {
    if (ctx.isLiveTick()) {
        return { live: true, spanSecs: ctx.timeRange.liveRangeSecs };
    }
    return { from: ctx.timeRange.from, to: ctx.timeRange.to };
}

/* Shared "reset on window/filter change or explicit re-sort" gate (contract
 * item 6): returns a function a view calls every build() with whatever
 * facts should force a fresh first-render ranking (window, filters, the
 * CURRENT sort state) -- resets the tracker exactly when that fact set
 * changes since the previous call, never on an ordinary same-window
 * refresh tick. One instance per mounted view (view-local state, same
 * family as views/waterfall.js's `selected`). */
export function createOrderResetGate() {
    let lastKey = null;
    return (tracker, keyParts) => {
        const key = JSON.stringify(keyParts);
        if (key !== lastKey) { tracker.reset(); lastKey = key; }
    };
}

/* ── Truncation row (U2, review P10) ─────────────────────────────────────────
 *
 * The summary tier caps distinct entities per second-record
 * (SUMMARY_MAX_EVENTS/QUERIES/SESSIONS, src/summary_writer.h) and silently
 * drops the tail — the tables must not present a capped list as complete.
 * Field names follow the session_timeline precedent (`truncated` +
 * `total_count`, src/server.c:2506): when a table response says
 * truncated:true we append a muted final row; with a numeric total_count we
 * can say how many rows were dropped, otherwise only THAT something was
 * ("… more not shown" — never an invented N).
 *
 * SERVER GAP (recorded, not worked around): as of U2 the table handlers
 * (top_events/top_sessions/top_queries/time_model, src/server.c) emit
 * neither field, and summary_writer.c does not count table-full drops
 * (find_or_insert_* returns NULL silently) — so today this renders nothing.
 * The wiring is the honest client half of the contract; the server half is
 * a per-handler `truncated`/`total_count` emit plus a drop counter in the
 * summary accumulator. */
export function buildTruncationRow(data) {
    if (!data || data.truncated !== true) return null;
    const shown = Array.isArray(data.rows) ? data.rows.length : 0;
    const total = (typeof data.total_count === 'number' && isFinite(data.total_count))
        ? data.total_count : null;
    if (total != null && total > shown) {
        const n = total - shown;
        return { omitted: n, text: '… ' + fmtCount(n) + ' more below threshold' };
    }
    return { omitted: null, text: '… more not shown' };
}

function tableHtml(model, truncation) {
    let html = '<table><thead><tr>';
    for (const h of model.headers) {
        html += '<th class="' + h.cls + '"' +
                (h.sortable ? ' data-sort="' + h.key + '"' : '') + '>' +
                h.label + h.arrow + '</th>';
    }
    html += '</tr></thead><tbody>';
    for (let ri = 0; ri < model.rows.length; ri++) {
        const mr = model.rows[ri];
        html += '<tr class="' + mr.cls + '" data-row="' + ri + '">';
        for (let ci = 0; ci < mr.cells.length; ci++) {
            const c = mr.cells[ci];
            html += c.intent
                ? '<td class="' + c.cls + '" data-ci="' + ci +
                  '" style="cursor:pointer">' + c.html + '</td>'
                : '<td class="' + c.cls + '">' + c.html + '</td>';
        }
        html += '</tr>';
    }
    if (truncation) {
        html += '<tr class="truncation-row"><td colspan="' +
            model.headers.length +
            '" style="color:#666;font-style:italic;font-size:11px">' +
            esc(truncation.text) + '</td></tr>';
    }
    html += '</tbody></table>';
    return html;
}

/* Mount a table into `el`. opts:
 *   { sort, onSort(key), onRowClick(row), onCellDrill(intent), truncation,
 *     tooltipEl }
 * `truncation` is a buildTruncationRow() model (or null) — rendered as a
 * muted, non-clickable final row. */
export function mountTable(el, config, model, opts) {
    opts = opts || {};
    el.innerHTML = tableHtml(model, opts.truncation);

    if (opts.onSort) {
        el.querySelectorAll('th[data-sort]').forEach(th => {
            th.addEventListener('click', () => opts.onSort(th.dataset.sort));
        });
    }

    if (opts.onRowClick) {
        el.querySelectorAll('tr.clickable').forEach(tr => {
            tr.addEventListener('click', () => {
                const idx = parseInt(tr.dataset.row);
                const mr = model.rows[idx];
                if (mr) opts.onRowClick(mr.row);
            });
        });
    }

    // U2 wire 6: per-cell drills (events percentile cells → histogram pivot).
    // stopPropagation keeps the row-level drill from also firing.
    if (opts.onCellDrill) {
        el.querySelectorAll('td.drillable').forEach(td => {
            td.addEventListener('click', (e) => {
                e.stopPropagation();
                const tr = td.closest('tr');
                const mr = tr && model.rows[parseInt(tr.dataset.row)];
                const cell = mr && mr.cells[parseInt(td.dataset.ci)];
                if (cell && cell.intent) opts.onCellDrill(cell.intent);
            });
        });
    }

    // Query-text hover tooltip (queries view).
    const tip = opts.tooltipEl;
    if (tip) {
        el.querySelectorAll('.qt-hover').forEach(qt => {
            qt.addEventListener('mouseenter', (e) => {
                const text = qt.getAttribute('data-fulltext');
                if (!text || text.length <= 120) return;
                tip.textContent = text;
                tip.style.display = 'block';
                positionTooltip(tip, e);
            });
            qt.addEventListener('mousemove', (e) => {
                if (tip.style.display === 'block') positionTooltip(tip, e);
            });
            qt.addEventListener('mouseleave', () => { tip.style.display = 'none'; });
        });
    }
}

function positionTooltip(tip, e) {
    const pad = 12;
    let x = e.clientX + pad;
    let y = e.clientY + pad;
    const rect = tip.getBoundingClientRect();
    if (x + rect.width > window.innerWidth - pad) x = e.clientX - rect.width - pad;
    if (y + rect.height > window.innerHeight - pad) y = e.clientY - rect.height - pad;
    tip.style.left = x + 'px';
    tip.style.top = y + 'px';
}

/* Small shared cell renderers used by multiple table configs (kept here so the
 * configs themselves are declarative). All return HTML strings. */

export function dot(name, classColor) {
    const color = classColor(name);
    if (!color) return '';
    return '<span class="class-dot" style="background:' + color + '"></span>';
}

export function pctBar(pct, color) {
    if (pct == null || isNaN(pct)) return '—';   // null/NaN: em-dash like fmtMs (P1)
    const w = Math.min(Math.max(pct, 0), 100);
    const c = color || '#4fc3f7';
    return '<div class="pct-bar">' +
        '<div class="pct-fill" style="width:' + w.toFixed(1) + '%;background:' + c + '"></div>' +
        '<span>' + pct.toFixed(1) + '%</span></div>';
}

/* ── Stacked composition bars (U2, review P11 "table bar truth") ─────────────
 *
 * The pre-U2 bars had three documented lies:
 *   1. Segments below the display threshold vanished into ONE gray "Other"
 *      that conflated skipped-known entries with genuinely-unlisted time —
 *      two different facts, now two labeled segments (below/other).
 *   2. CSS `min-width:2px` per segment let widths sum past 100% and flexbox
 *      silently renormalized, so displayed proportions depended on the
 *      container's pixel width. Widths are now pre-normalized HERE
 *      (deterministic, container-independent; inline `flex:0 0 w%` +
 *      `min-width:0` defeat the CSS re-flex) and every title carries the
 *      TRUE percentage.
 *   3. Adjacent same-class segments fused into one block — a 1px inset
 *      separator now divides neighbors (box-shadow: zero layout impact, so
 *      the width math stays exact).
 */

/* Every displayed segment gets at least this display width (the honest
 * successor of the CSS min-width:2px — ~2px at the 120–300px bar range).
 * Deficit is taken from segments above the floor, proportionally. */
export const BAR_MIN_SEG_PCT = 1.5;
/* Listed entries below the display threshold, folded + counted. */
export const BAR_BELOW_COLOR = '#4a4a5c';
/* Time NOT covered by the listed entries at all (tail beyond the server's
 * top list / unattributed) — deliberately distinct from BAR_BELOW_COLOR. */
export const BAR_OTHER_COLOR = '#33334a';
const BAR_SEPARATOR = 'box-shadow:inset -1px 0 0 rgba(18,18,32,0.9);';

/* PURE: items [{name, ms, color}] + total(ms) -> segment models
 *   [{ kind: 'item'|'below'|'other', color, pct, width, title }]
 * pct = the TRUE share; width = the pre-normalized display width (sums to
 * ≤100 with the floor applied). opts: { threshold, fmt, noun, otherLabel }.
 * Exported for unit tests. */
export function buildBarSegments(items, total, opts) {
    opts = opts || {};
    const threshold = opts.threshold != null ? opts.threshold : 0.5;
    const fmt = opts.fmt || String;
    const noun = opts.noun || 'entries';
    const otherLabel = opts.otherLabel || 'Other (not in the listed entries)';
    if (!items || !items.length || !total || total <= 0) return [];

    const segs = [];
    const below = [];
    let listedPct = 0;
    for (const it of items) {
        if (!it || !(it.ms > 0)) continue;    // absent ≠ "below threshold"
        const pct = it.ms / total * 100;
        listedPct += pct;
        if (pct < threshold) { below.push(pct); continue; }
        segs.push({
            kind: 'item', color: it.color, pct,
            title: it.name + ': ' + fmt(it.ms) + ' (' + pct.toFixed(1) + '%)',
        });
    }

    if (below.length) {
        const pct = below.reduce((a, b) => a + b, 0);
        segs.push({
            kind: 'below', color: BAR_BELOW_COLOR, pct,
            title: below.length + ' ' + noun + ' below the ' + threshold +
                   '% display threshold: ' + pct.toFixed(1) + '% combined',
        });
    }

    // The gap between the listed entries and the row total: time the list
    // does not cover (NOT the same fact as "listed but small" above).
    const otherPct = 100 - Math.min(listedPct, 100);
    if (otherPct > 0.05) {
        segs.push({
            kind: 'other', color: BAR_OTHER_COLOR, pct: otherPct,
            title: otherLabel + ': ' + otherPct.toFixed(1) + '%',
        });
    }

    normalizeSegWidths(segs);
    return segs;
}

/* Deterministic width pass: scale into a 100% track if the true shares
 * overflow it, clamp everything to the BAR_MIN_SEG_PCT floor, and take the
 * deficit proportionally from the segments above the floor. No browser
 * flexing is left to renegotiate these numbers. */
function normalizeSegWidths(segs) {
    const MIN = BAR_MIN_SEG_PCT;
    if (!segs.length) return;
    if (segs.length * MIN >= 100) {          // degenerate: floors alone overflow
        for (const s of segs) s.width = Math.round(10000 / segs.length) / 100;
        return;
    }
    const totalPct = segs.reduce((a, s) => a + s.pct, 0);
    const scale = totalPct > 100 ? 100 / totalPct : 1;
    let deficit = 0, excess = 0;
    for (const s of segs) {
        s.width = s.pct * scale;
        if (s.width < MIN) deficit += MIN - s.width;
        else excess += s.width - MIN;
    }
    const f = excess > 0 ? Math.max(0, (excess - deficit) / excess) : 0;
    for (const s of segs) {
        s.width = s.width < MIN ? MIN : MIN + (s.width - MIN) * f;
        s.width = Math.round(s.width * 100) / 100;
    }
}

function barHtml(segs) {
    if (!segs.length) return '';
    let html = '<div class="stacked-bar">';
    for (let i = 0; i < segs.length; i++) {
        const s = segs[i];
        const sep = i < segs.length - 1 ? BAR_SEPARATOR : '';
        html += '<div class="bar-seg' +
                (s.kind !== 'item' ? ' bar-seg-' + s.kind : '') +
                '" style="flex:0 0 ' + s.width.toFixed(2) + '%;min-width:0;' +
                sep + 'background:' + s.color + '" title="' + esc(s.title) +
                '"></div>';
    }
    return html + '</div>';
}

export function stackedBar(classes, total, WAIT_CLASSES, fmtMs) {
    const items = [];
    if (classes) {
        for (let i = 0; i < WAIT_CLASSES.length && i < classes.length; i++) {
            items.push({ name: WAIT_CLASSES[i].label, ms: classes[i],
                         color: WAIT_CLASSES[i].color });
        }
    }
    return barHtml(buildBarSegments(items, total, {
        threshold: 0.5, fmt: fmtMs, noun: 'classes',
        otherLabel: 'Unattributed (outside the listed classes)',
    }));
}

export function eventStackedBar(events, total, eventColor, fmtMs) {
    // U1 identity color service: a deterministic per-event tint of the
    // class hue (was class color, or an index-keyed palette slot).
    const items = (events || []).map(e => ({
        name: e.name, ms: e.ms, color: eventColor(null, e.name),
    }));
    return barHtml(buildBarSegments(items, total, {
        threshold: 0.3, fmt: fmtMs, noun: 'events',
        otherLabel: 'Other events (beyond the listed top events)',
    }));
}
