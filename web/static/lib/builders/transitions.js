/* pgwt — pure builder: transitions (directly-follows graph) -> ECharts option,
 * plus the flow-variants HTML section.
 *
 * The transitions tab's force-free DFG: nodes = wait events (size ∝ total time),
 * directed edges = transition counts, with a "Simplify" slider that hides edges
 * below a fraction of the busiest edge. This builder is library-shaped only at
 * the edge (it emits an ECharts graph option); the data->node/link mapping and
 * the grid layout math are pure and Node-testable. It owns NO chart instance and
 * touches NO DOM.
 *
 * Mapping is byte-identical to the old legacy adapter in app.js (renderDFG /
 * renderVariantSection):
 *   - nodes sorted by total_ms desc, laid out on a roughly-golden grid
 *   - node size = 15..120 by sqrt(ms/maxMs); class color; below-node label
 *   - edges filtered by `threshold`% of the max edge count; self-loops curve more
 *   - layout 'none', roam + draggable, arrow edge symbol
 */

import { classColor, esc as escHtml } from '../format.js';

/* Build the layout-independent pieces: the maximum edge count and a name->node
 * lookup. Cheap, exported so the slider re-render can reuse it. */
export function transitionsContext(data) {
    const links = (data && data.links) || [];
    const nodes = (data && data.nodes) || [];
    const maxEdgeCount = links.length ? Math.max(...links.map(l => l.value)) : 0;
    const nodeMap = {};
    nodes.forEach(n => { nodeMap[n.name] = n; });
    return { maxEdgeCount, nodeMap };
}

/* PURE: data + threshold% + container dimensions -> ECharts graph option.
 *
 * `dims` = { width, height } of the DFG container (for the grid layout); the
 * view passes the live container size, tests pass fixed dimensions. Returns
 * { option, visibleCount }. visibleCount is the number of laid-out nodes (0 =>
 * "no transitions above threshold"). */
export function buildTransitionsOption(data, threshold, dims) {
    dims = dims || {};
    const cW = dims.width || 800, cH = dims.height || 550;
    const links = (data && data.links) || [];
    const total = (data && data.total) || 0;
    const { maxEdgeCount, nodeMap } = transitionsContext(data);

    const minCount = maxEdgeCount * threshold / 100;
    const visibleLinks = links.filter(l => l.value >= minCount);
    const visibleNodes = new Set();
    visibleLinks.forEach(l => { visibleNodes.add(l.source); visibleNodes.add(l.target); });
    if (visibleNodes.size === 0) {
        return { option: null, visibleCount: 0 };
    }

    const maxNodeMs = Math.max(...[...visibleNodes].map(n => (nodeMap[n]?.total_ms || 1)));
    const maxLinkVal = Math.max(...visibleLinks.map(l => l.value));
    const sortedNodes = [...visibleNodes]
        .map(name => ({ name, ms: (nodeMap[name]?.total_ms || 0) }))
        .sort((a, b) => b.ms - a.ms);
    const n = sortedNodes.length;
    const cols = Math.ceil(Math.sqrt(n * 1.8));
    const rows = Math.ceil(n / cols);
    const padX = 80, padY = 60;
    const cellW = (cW - padX * 2) / Math.max(cols - 1, 1);
    const cellH = (cH - padY * 2) / Math.max(rows - 1, 1);

    const ecNodes = sortedNodes.map((nd, i) => {
        const info = nodeMap[nd.name] || {};
        const ms = nd.ms;
        const cls = (info.class || 'unknown').toLowerCase();
        const color = classColor(nd.name) || classColor(cls) || '#888';
        const size = Math.max(15, Math.min(120, 15 + Math.sqrt(ms / maxNodeMs) * 105));
        const timeStr = ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : ms.toFixed(0) + 'ms';
        const col = i % cols, row = Math.floor(i / cols);
        return {
            name: nd.name, x: padX + col * cellW, y: padY + row * cellH,
            symbolSize: size,
            itemStyle: { color, borderColor: color, borderWidth: 2 },
            label: {
                show: true, position: 'bottom', fontSize: 10, color: '#ccc',
                formatter: (nd.name.indexOf(':') > 0
                    ? nd.name.substring(nd.name.indexOf(':') + 1) : nd.name) +
                    '\n' + timeStr,
                lineHeight: 14,
            },
            // Tooltips render as HTML — escape the (server-derived) name too,
            // same bug class as the timeline query-text injection (UI-6).
            tooltip: { formatter: '<b>' + escHtml(nd.name) + '</b><br/>Total time: ' + timeStr },
            value: ms,
        };
    });

    const ecLinks = visibleLinks.map(l => {
        const w = Math.max(1, Math.min(10, (l.value / maxLinkVal) * 10));
        const pct = (l.value / total * 100).toFixed(1);
        const avg = l.value > 0 ? (l.duration_ms / l.value).toFixed(1) : '?';
        return {
            source: l.source, target: l.target,
            lineStyle: {
                width: w, color: '#555',
                curveness: l.source === l.target ? 0.8 : 0.3,
                opacity: 0.4 + (l.value / maxLinkVal) * 0.5,
            },
            tooltip: {
                formatter: '<b>' + escHtml(l.source) + '</b> → <b>' + escHtml(l.target) +
                    '</b><br/>' +
                    'Count: ' + l.value.toLocaleString() + ' (' + pct + '%)<br/>Avg dwell: ' +
                    avg + ' ms',
            },
            value: l.value,
        };
    });

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        tooltip: {
            backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
        },
        series: [{
            type: 'graph', layout: 'none', roam: true, draggable: true,
            data: ecNodes, links: ecLinks,
            edgeSymbol: ['none', 'arrow'], edgeSymbolSize: [0, 8],
            emphasis: { focus: 'adjacency', lineStyle: { width: 4, opacity: 1 } },
            lineStyle: { curveness: 0.3 },
        }],
    };
    return { option, visibleCount: ecNodes.length };
}

/* PURE: a variants response section ("exec" or "plan") -> HTML string. Byte-
 * identical to the old renderVariantSection. Exported for testing. */
export function buildVariantSectionHtml(vdata, title, esc) {
    if (!vdata || !vdata.variants || vdata.variants.length === 0) return '';
    const totalMs = vdata.variants.reduce((s, v) => s + v.total_ms, 0);
    let html = '<div style="padding:10px 20px">' +
        '<h3 style="color:#ccc;margin:10px 0 5px">' + title + ' Flow Patterns ' +
        '<span style="color:#666;font-size:12px">(' + vdata.total.toLocaleString() +
        ' instances → ' + vdata.num_variants + ' patterns)</span></h3>';
    vdata.variants.forEach((v, idx) => {
        const pctTime = totalMs > 0 ? (v.total_ms / totalMs * 100) : 0;
        const avgStr = v.avg_ms >= 1000 ? (v.avg_ms / 1000).toFixed(1) + 's' :
            v.avg_ms >= 1 ? v.avg_ms.toFixed(1) + 'ms' : (v.avg_ms * 1000).toFixed(0) + 'μs';
        const p95Str = v.p95_ms >= 1000 ? (v.p95_ms / 1000).toFixed(1) + 's' :
            v.p95_ms >= 1 ? v.p95_ms.toFixed(1) + 'ms' : (v.p95_ms * 1000).toFixed(0) + 'μs';
        const totalStr = v.total_ms >= 1000 ? (v.total_ms / 1000).toFixed(1) + 's' :
            v.total_ms.toFixed(0) + 'ms';
        const stepTotalMs = v.steps.reduce((s, st) => s + (st.avg_ms || 0), 0) || 1;
        let flowHtml = '<div style="display:flex;height:28px;border-radius:4px;overflow:hidden;' +
            'background:#1a1a2e;border:1px solid #2a2a4a;margin:4px 0">';
        v.steps.forEach(st => {
            const w = Math.max(2, (st.avg_ms / stepTotalMs) * 100);
            const color = classColor(st.name) || classColor(st.class) || '#888';
            const label = st.name.indexOf(':') > 0 ? st.name.substring(st.name.indexOf(':') + 1) : st.name;
            const durStr = st.avg_ms >= 1 ? st.avg_ms.toFixed(1) + 'ms' :
                st.avg_ms >= 0.001 ? (st.avg_ms * 1000).toFixed(0) + 'μs' : '';
            const loopMark = st.loop ? ' ×N' : '';
            flowHtml += '<div style="width:' + w.toFixed(1) + '%;background:' + color + ';opacity:0.8;' +
                'display:flex;align-items:center;justify-content:center;overflow:hidden;' +
                'font-size:9px;color:#fff;white-space:nowrap;padding:0 3px;min-width:2px" ' +
                'title="' + esc(st.name) + ' ' + durStr + loopMark + '">' +
                (w > 8 ? esc(label) + (loopMark ? loopMark : '') : '') +
                (w > 15 ? ' ' + durStr : '') + '</div>';
        });
        flowHtml += '</div>';
        const stepsText = v.steps.map(st => {
            const label = st.name.indexOf(':') > 0 ? st.name.substring(st.name.indexOf(':') + 1) : st.name;
            const dur = st.avg_ms >= 1 ? st.avg_ms.toFixed(1) + 'ms' : (st.avg_ms * 1000).toFixed(0) + 'μs';
            if (st.loop) return '[' + esc(label) + '(' + dur + ')…]×N';
            return esc(label) + '(' + dur + ')';
        }).join(' → ');
        let queryHtml = '';
        if (v.top_query_id) {
            const qid = v.top_query_id;
            const preview = v.query_text
                ? (v.query_text.length > 100 ? v.query_text.substring(0, 100) + '...' : v.query_text) : '';
            queryHtml = '<div style="color:#666;font-size:10px;font-family:monospace;margin-top:2px;' +
                'white-space:nowrap;overflow:hidden;text-overflow:ellipsis" title="' +
                esc(v.query_text || '') + '">' +
                '<span style="color:#888">' + qid + '</span>' +
                (preview ? ' ' + esc(preview) : '') + '</div>';
        }
        html += '<div style="background:#1e1e3a;border:1px solid #2a2a4a;border-radius:6px;padding:10px;margin:6px 0">' +
            '<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px">' +
            '<span style="color:#ccc;font-size:13px;font-weight:500">#' + (idx + 1) + '</span>' +
            '<span style="color:#888;font-size:11px">' +
            '<b style="color:#4fc3f7">' + pctTime.toFixed(1) + '%</b> of time' +
            ' · ' + v.exec_count.toLocaleString() + ' exec · ' + v.num_queries + ' queries' +
            ' · avg ' + avgStr + ' · p95 ' + p95Str + ' · total ' + totalStr +
            (v.avg_loop_n > 1.5 ? ' · ~' + v.avg_loop_n.toFixed(0) + '× loop' : '') +
            '</span></div>' + flowHtml +
            '<div style="color:#888;font-size:10px;margin-top:2px">' + stepsText + '</div>' + queryHtml +
            '</div>';
    });
    html += '</div>';
    return html;
}

/* PURE: full variants payload -> combined HTML (exec then plan). */
export function buildVariantsHtml(vdata, esc) {
    if (!vdata) return '';
    let html = '';
    if (vdata.exec) html += buildVariantSectionHtml(vdata.exec, 'Execution', esc);
    if (vdata.plan) html += buildVariantSectionHtml(vdata.plan, 'Planning', esc);
    return html;
}
