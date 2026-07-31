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

import { esc } from './format.js';

/* Pure: apply sort, format every cell, produce a render model. */
export function buildTableModel(config, rows, sort) {
    rows = rows ? rows.slice() : [];

    // Sort (callers pass null sort to keep server order, e.g. overview).
    if (sort && sort.key) {
        const asc = sort.asc;
        const key = sort.key;
        rows.sort((a, b) => {
            const va = a[key], vb = b[key];
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
        arrow: (sort && sort.key === col.key)
            ? (sort.asc ? ' ▲' : ' ▼') : '',
    }));

    const modelRows = rows.map(row => ({
        cls: config.rowClass ? config.rowClass(row) : '',
        row,
        cells: config.columns.map(col => ({
            cls: col.cls || '',
            html: col.format(row),
        })),
    }));

    return { headers, rows: modelRows };
}

function tableHtml(model) {
    let html = '<table><thead><tr>';
    for (const h of model.headers) {
        html += '<th class="' + h.cls + '" data-sort="' + h.key + '">' +
                h.label + h.arrow + '</th>';
    }
    html += '</tr></thead><tbody>';
    for (let ri = 0; ri < model.rows.length; ri++) {
        const mr = model.rows[ri];
        html += '<tr class="' + mr.cls + '" data-row="' + ri + '">';
        for (const c of mr.cells) {
            html += '<td class="' + c.cls + '">' + c.html + '</td>';
        }
        html += '</tr>';
    }
    html += '</tbody></table>';
    return html;
}

/* Mount a table into `el`. opts:
 *   { sort, onSort(key), onRowClick(row), tooltipEl } */
export function mountTable(el, config, model, opts) {
    opts = opts || {};
    el.innerHTML = tableHtml(model);

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

export function stackedBar(classes, total, WAIT_CLASSES, fmtMs) {
    if (!classes || !total || total <= 0) return '';
    let html = '<div class="stacked-bar">';
    let shownPct = 0;
    for (let i = 0; i < WAIT_CLASSES.length && i < classes.length; i++) {
        const pct = classes[i] / total * 100;
        if (pct < 0.5) continue;
        shownPct += pct;
        html += '<div class="bar-seg" style="width:' + pct.toFixed(1) +
                '%;background:' + WAIT_CLASSES[i].color + '" title="' +
                WAIT_CLASSES[i].label + ': ' + fmtMs(classes[i]) +
                ' (' + pct.toFixed(1) + '%)"></div>';
    }
    if (shownPct < 99.5) {
        const otherPct = 100 - shownPct;
        html += '<div class="bar-seg" style="width:' + otherPct.toFixed(1) +
                '%;background:#444" title="Other: ' + otherPct.toFixed(1) + '%"></div>';
    }
    html += '</div>';
    return html;
}

export function eventStackedBar(events, total, EVENT_PALETTE, classColor, fmtMs) {
    if (!events || !events.length || !total || total <= 0) return '';
    let html = '<div class="stacked-bar">';
    let shownPct = 0;
    for (let i = 0; i < events.length; i++) {
        const pct = events[i].ms / total * 100;
        if (pct < 0.3) continue;
        shownPct += pct;
        const color = classColor(events[i].name) || EVENT_PALETTE[i % EVENT_PALETTE.length];
        html += '<div class="bar-seg" style="width:' + pct.toFixed(1) +
                '%;background:' + color + '" title="' +
                esc(events[i].name) + ': ' + fmtMs(events[i].ms) +
                ' (' + pct.toFixed(1) + '%)"></div>';
    }
    if (shownPct < 99.5) {
        const otherPct = 100 - shownPct;
        html += '<div class="bar-seg" style="width:' + otherPct.toFixed(1) +
                '%;background:#444" title="Other small events: ' +
                otherPct.toFixed(1) + '%"></div>';
    }
    html += '</div>';
    return html;
}
