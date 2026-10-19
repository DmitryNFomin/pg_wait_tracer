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
 * Mapping matches the old legacy adapter in app.js (renderDFG /
 * renderVariantSection) except colors (U2, the U1-F4 backlog):
 *   - nodes sorted by total_ms desc, laid out on a roughly-golden grid;
 *     posOverrides (user-dragged coordinates, P5) win over the grid
 *   - node size = 15..120 by sqrt(ms/maxMs); eventColor identity tint (the
 *     same event = same color contract as the AAS chart); below-node label
 *   - nodes carry eventId/className (server-emitted event_id — P3 wire 5)
 *   - edges filtered by `threshold`% of the max edge count; self-loops curve more
 *   - layout 'none', roam + draggable, arrow edge symbol
 *   - `hideIdle` (issue #107, owner decision 2026-09-17 "option B"): idle-wait
 *     edges/nodes are dropped BEFORE the threshold ranking, not after. On a
 *     real OLTP workload the Client:ClientRead<->CPU* loop's edge COUNT
 *     dwarfs every analytically interesting transition (Lock/IO/Timeout) —
 *     ranking against it left every other edge below the 20%-of-max
 *     threshold, rendering sub-pixel. Kept as pure volume ranking (the
 *     owner's option B, not option A's reclassification) — just ranked
 *     against the remaining edges once the idle loop is out of the pool.
 */

import { eventColor, fmtMs, esc as escHtml } from '../format.js';

/* DEFINITIONS (contract 5, issues #436/#438 — RELEASE-PLAN.md "Transitions
 * and simplification"), recorded here because the server-side half of the
 * same contract lives at compute.c:2875 and the node/edge shapes below are
 * the client's only other home for it:
 *
 *   - An edge A->B counts EXACT records whose old_event=A, new_event=B.
 *     Edge duration sums the full SOURCE-STATE dwell (how long the process
 *     sat in A before transitioning), never target latency or the gap
 *     between two unrelated waits. A->CPU*->B is never contracted into a
 *     synthetic A->B: CPU* is a literal intermediate state the process
 *     passed through, not inferred wait-to-wait adjacency — that would be a
 *     different, invented metric (the adviser called this out by name).
 *   - CPU* is a literal intermediate state, not necessarily measured
 *     running CPU — it is whatever the sampler saw between two off-CPU
 *     waits, which can include scheduling gaps. "CPU-centered" topology is
 *     therefore the correct shape of the literal data, not a bug; #438's
 *     fix is explaining that in the Matrix view (matrix.js ~:52), not
 *     re-deriving a different metric here.
 *   - The simplification criterion stays FREQUENCY, not DB Time: after the
 *     idle exclusion below, an edge survives when
 *     `count >= max(remaining edge count) * threshold/100` (default 20%).
 *     This is why a RARE but long-dwelling edge (e.g. one catastrophic
 *     Lock:relation wait) can rank below the cutoff even though it dominates
 *     DB time — by design, not a bug to paper over with a DB-Time ranking
 *     (that would be the "different product" the adviser warned against).
 *     What #436 actually requires for a rare-but-dominant edge: NAME it
 *     instead of dropping it silently (omittedDominant below) and offer a
 *     working "show all" escape hatch (restoreAllTransitionsState).
 *   - Sample-only transition data is unavailable (see isUnavailable/#A3 at
 *     the call site) — never backfilled from CPU/wait counts elsewhere.
 */

/* Idle/client wait nodes, as far as they can reach this payload. Mirrors
 * pgwt_is_idle_event() (src/wait_event.c): Activity-class waits are already
 * excluded server-side from the DFG's node/link JSON (pgwt_is_hidden_event),
 * so Client:ClientRead is the only idle node this builder ever sees. */
const IDLE_NODE_NAMES = new Set(['Client:ClientRead']);

export function isIdleTransitionNode(name) {
    return IDLE_NODE_NAMES.has(name);
}

/* #456: a persistent, always-visible reading key for the DFG (views/
 * transitions.js renders this verbatim, unconditionally — not only when
 * something is hidden, unlike buildTransitionsDisclosure's dynamic summary
 * line below). Two things a viewer otherwise infers wrong by analogy with
 * the AAS/Events tabs, where size/color already mean "contribution to DB
 * Time": here node SIZE is source-state DWELL TIME (how long the process
 * sat in that state before transitioning — contract 5 above), while edge
 * width/the Simplify threshold both rank by TRANSITION COUNT, a different
 * quantity entirely. A big node next to thin edges is not a contradiction;
 * it is the "rare but long-dwelling" case #436's omittedDominant/
 * findDominantSourceDwellNode exist to name. */
export const TRANSITIONS_SEMANTICS_NOTE =
    'Node size represents source-state dwell time in counted transitions. ' +
    'Edges and Simplify use transition counts. This graph is not a ranking ' +
    'of window DB Time.';

/* #436 (adviser-caught): two invisible nodes placed at the exact corners of
 * the DFG's pixel box so ECharts' graph "view" coordinateSystem has no data
 * extent to auto-fit beyond that box — see the long comment on the series
 * config in buildTransitionsOption for the empirically-verified mechanism.
 * NEVER part of the real node set: excluded from `ecNodes`/`visibleCount`/
 * `sortedNodes`, carry no color/label/eventId, and are `silent` (no hover,
 * click or drag — echarts does not dispatch pointer events to silent
 * elements at all). Exported so tests can filter them out by name when
 * iterating the option's rendered node list, rather than re-deriving a
 * second guess at which entries are "real". */
const LAYOUT_ANCHOR_NAMES = ['pgwt-layout-anchor-tl', 'pgwt-layout-anchor-br'];

export function isLayoutAnchorNode(name) {
    return LAYOUT_ANCHOR_NAMES.indexOf(name) !== -1;
}

function layoutAnchorNodes(cW, cH) {
    return [
        { name: LAYOUT_ANCHOR_NAMES[0], x: 0, y: 0, symbol: 'none', symbolSize: 0, silent: true,
            label: { show: false }, tooltip: { show: false } },
        { name: LAYOUT_ANCHOR_NAMES[1], x: cW, y: cH, symbol: 'none', symbolSize: 0, silent: true,
            label: { show: false }, tooltip: { show: false } },
    ];
}

/* Drop every link touching an idle node. Returns the survivors plus how much
 * was hidden (link count + summed transition count) so the view can say so
 * instead of silently shrinking the graph. */
function filterIdleLinks(links, hideIdle) {
    if (!hideIdle) return { links, hiddenLinks: 0, hiddenValue: 0 };
    let hiddenLinks = 0, hiddenValue = 0;
    const kept = links.filter(l => {
        if (isIdleTransitionNode(l.source) || isIdleTransitionNode(l.target)) {
            hiddenLinks++;
            hiddenValue += l.value || 0;
            return false;
        }
        return true;
    });
    return { links: kept, hiddenLinks, hiddenValue };
}

/* #456: the view must distinguish an incomplete response from an empty graph
 * or a graph with no omitted waits. Return a reason for its visible error
 * panel; the pure builder enforces the same contract for other callers. */
export function transitionsDataError(data) {
    if (!data || !Array.isArray(data.nodes)) return 'missing nodes array';
    if (!Array.isArray(data.links)) return 'missing links array';
    const names = new Set();
    for (const node of data.nodes) {
        if (!node || typeof node.name !== 'string' || !node.name ||
            !Number.isFinite(node.total_ms) || node.total_ms < 0) {
            return 'invalid node name or dwell time';
        }
        if (names.has(node.name)) return 'duplicate node: ' + node.name;
        names.add(node.name);
    }
    // Check ALL endpoints, before idle/simplify filtering can conceal a missing
    // catalog entry. Links carry counts, not the complete node dwell catalog;
    // synthesizing nodes from them would silently lose omittedDominant (#456).
    for (const link of data.links) {
        if (!link || typeof link.source !== 'string' || typeof link.target !== 'string') {
            return 'invalid link endpoints';
        }
        for (const name of [link.source, link.target]) {
            if (!names.has(name)) return 'missing node: ' + name;
        }
    }
    return null;
}

/* Build the layout-independent pieces: the maximum edge count and a name->node
 * lookup. Callers may pass idle-filtered links, but must retain the complete
 * node catalog. buildTransitionsOption validates the raw links first. */
export function transitionsContext(data) {
    const error = transitionsDataError(data);
    if (error) throw new Error('Transitions data incomplete: ' + error);
    const { links, nodes } = data;
    const maxEdgeCount = links.length ? Math.max(...links.map(l => l.value)) : 0;
    const nodeMap = Object.create(null);
    nodes.forEach(n => { nodeMap[n.name] = n; });
    return { maxEdgeCount, nodeMap };
}

/* #456 (review finding): two order-sensitive tie sites in this file used to
 * keep whichever name happened to be visited FIRST on an exact duration tie
 * -- here, that was Object.keys(nodeMap)'s iteration order; at the grid
 * layout below (sortedNodes), it was the order nodes were first added to
 * the `visibleNodes` Set while walking `visibleLinks`. Both are the
 * server's / a fixture's ARRAY order, not a property of the data itself.
 * Two requests carrying the identical node set (same durations) could
 * therefore name a different "omitted dominant" node, or lay nodes out in a
 * different grid position, purely because the server returned its
 * links/nodes in a different order -- indistinguishable, from the browser,
 * from the graph actually changing between two renders of the SAME
 * underlying data (the #456 "frozen trace, different render" symptom this
 * removes one possible cause of). TOTAL order for both sites, matching the
 * server-side total-order fix already applied to events/sessions/queries
 * (compute.c:1728 cmp_event_row_desc and siblings): duration descending,
 * then numeric event identity (event_id) ascending, then name ascending as
 * a last-resort deterministic fallback for the (should not happen on a real
 * server, but must still be total) case of two nodes sharing both an ms
 * value and an event_id, or either/both missing one. */
export function compareTransitionNodesByDominance(a, b) {
    if (a.ms !== b.ms) return b.ms - a.ms;
    const aId = a.eventId, bId = b.eventId;
    if (aId != null && bId != null && aId !== bId) return aId - bId;
    if (aId != null && bId == null) return -1;   // a numeric identity beats none
    if (aId == null && bId != null) return 1;
    return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
}

/* #436/#456: among nodes NOT laid out (hidden by the threshold cutoff — idle
 * nodes are excluded here because the "Hide ClientRead" control already
 * names and counts that reason on its own, see hiddenIdleLinks/-Value), find
 * the single one with the largest total_ms. That is the "dominant-duration
 * wait" the bug report means: one whose EDGE COUNT is too low to clear the
 * frequency threshold even though its DWELL TIME dominates. Returns null
 * when nothing of the sort is hidden (nothing to name). `nodeMap` is built
 * from the full (pre-idle-filter) node list — see buildTransitionsOption.
 * Ties are broken by compareOmissionCandidates (#456) — deterministic
 * regardless of nodeMap's iteration order. */
export function findOmittedDominantNode(nodeMap, visibleNodes) {
    let best = null;
    for (const name of Object.keys(nodeMap)) {
        if (visibleNodes.has(name) || isIdleTransitionNode(name)) continue;
        const node = nodeMap[name];
        const ms = (node && node.total_ms) || 0;
        if (ms <= 0) continue;
        const eventId = node && node.event_id != null ? node.event_id : null;
        const candidate = { name, ms, eventId };
        if (!best || compareTransitionNodesByDominance(candidate, best) < 0) best = candidate;
    }
    return best ? { name: best.name, ms: best.ms } : null;
}

/* #456: the PERSISTENT counterpart to findOmittedDominantNode above. The old
 * disclosure only ever named a dominant-dwell node when the threshold/idle
 * filters hid one — a window where nothing is hidden said nothing at all
 * about which node dominates source-state dwell, even though that fact is
 * exactly as true (and exactly as easy to misread against edge width) when
 * every node is visible. Scans the SAME population as
 * findOmittedDominantNode (every non-idle node with total_ms > 0, same
 * deterministic tie-break) but over ALL of it, not just the hidden part, and
 * reports whether the winner happens to be visible or omitted right now. */
export function findDominantSourceDwellNode(nodeMap, visibleNodes) {
    let best = null;
    for (const name of Object.keys(nodeMap)) {
        if (isIdleTransitionNode(name)) continue;
        const node = nodeMap[name];
        const ms = (node && node.total_ms) || 0;
        if (ms <= 0) continue;
        const eventId = node && node.event_id != null ? node.event_id : null;
        const candidate = { name, ms, eventId };
        if (!best || compareTransitionNodesByDominance(candidate, best) < 0) best = candidate;
    }
    if (!best) return null;
    return { name: best.name, ms: best.ms, visible: visibleNodes.has(best.name) };
}

/* "Show all" (#436): the one restore action that is actually honest about
 * what it can do. It removes BOTH client-side exclusions (the frequency
 * threshold and the idle filter) so every link/node the server already
 * returned becomes visible — it does NOT, and cannot, re-fetch links the
 * server itself dropped (serverTruncated/serverTotalLinkCount below): that
 * would need a different server request this builder has no way to issue.
 * The view's button label/title says so explicitly rather than implying a
 * fuller restore than this performs. */
export function restoreAllTransitionsState() {
    return { threshold: 0, hideIdle: false };
}

const DFG_FONT_SIZE = 10;
// Conservative average glyph advance for this sans-serif at 10px — sized to
// over-, not under-, estimate (a wider guess only wastes canvas margin; a
// narrower one clips text). The Playwright live-render check in
// test_web_ui.py measures the REAL text width and is the authority; this is
// the pure-builder's best estimate so layout math stays testable without a
// browser.
const DFG_CHAR_WIDTH = DFG_FONT_SIZE * 0.65;
const DFG_LABEL_LINE_HEIGHT = 14;
const DFG_LABEL_LINES = 2;      // "<short name>\n<time string>" (see ecNodes below)
const DFG_LABEL_GAP = 6;        // ECharts' approximate symbol-to-label clearance

/* #436: the pixel footprint a laid-out node (circle + its below-node label)
 * needs so neither clips the canvas edge. `symbolSize` is the node's full
 * diameter (as built below, 15..120). Exported so the viewport-fit unit
 * tests can check the SAME estimate the layout itself uses. */
export function estimateNodeFootprint(name, symbolSize) {
    const radius = symbolSize / 2;
    const short = name.indexOf(':') > 0 ? name.substring(name.indexOf(':') + 1) : name;
    const halfWidth = Math.max(radius, (short.length * DFG_CHAR_WIDTH) / 2);
    return {
        halfWidth,
        top: radius,
        bottom: radius + DFG_LABEL_GAP + DFG_LABEL_LINES * DFG_LABEL_LINE_HEIGHT,
    };
}

function dfgSymbolSize(ms, maxNodeMs) {
    return Math.max(15, Math.min(120, 15 + Math.sqrt(ms / maxNodeMs) * 105));
}

/* #436: deterministic cell assignment for the "none"-layout DFG grid.
 * `sortedNodes` (built in buildTransitionsOption) is sorted by total_ms
 * descending, so index 0 is the single most time-significant node — often
 * the CPU* hub in a CPU-mediated topology (#438). The OLD mapping
 * (`col = i % cols, row = floor(i / cols)`) put that node at literal grid
 * cell (0,0) — a corner — which (a) gave it the LEAST label clearance of
 * any cell and (b) made every edge fanning out from it the longest possible
 * in the grid, bowing toward the far side with no margin to absorb the
 * curve. This instead ranks every cell by squared distance from the grid's
 * CENTER and assigns the lowest-index (most significant) nodes to the
 * closest cells, so the hub sits where there is headroom on every side.
 * Ties break on row then column so two runs over the same (n, cols, rows)
 * always agree. Exported for the viewport-fit unit tests. */
export function computeGridOrder(n, cols, rows) {
    const cells = [];
    const centerCol = (cols - 1) / 2, centerRow = (rows - 1) / 2;
    for (let row = 0; row < rows; row++) {
        for (let col = 0; col < cols; col++) {
            const dx = col - centerCol, dy = row - centerRow;
            cells.push({ col, row, d2: dx * dx + dy * dy });
        }
    }
    cells.sort((a, b) => a.d2 - b.d2 || a.row - b.row || a.col - b.col);
    return cells.slice(0, n).map(({ col, row }) => ({ col, row }));
}

/* PURE: data + threshold% + container dimensions -> ECharts graph option.
 *
 * `dims` = { width, height } of the DFG container (for the grid layout); the
 * view passes the live container size, tests pass fixed dimensions.
 * `posOverrides` (optional) = { nodeName: { x, y } } — user-dragged positions
 * the view read back from the live instance (P5); they beat the grid layout
 * so a refresh/re-layout never snaps a hand-placed node back.
 * `hideIdle` (optional, default false — the view defaults it ON at the call
 * site): drop Client:ClientRead edges/nodes and rank threshold/sizing against
 * the remaining edges only (issue #107).
 *
 * Returns { option, visibleCount, hiddenIdleLinks, hiddenIdleValue,
 * hiddenThresholdLinks, hiddenThresholdValue, shownLinks, totalLinks,
 * serverTruncated, serverShownLinkCount, serverTotalLinkCount,
 * omittedDominant }. visibleCount is the number of laid-out nodes (0 =>
 * nothing above threshold). The three omission causes are DISTINCT and
 * additive, never merged into one count (#436): hiddenIdleLinks/-Value is
 * the "Hide ClientRead" control; hiddenThresholdLinks/-Value is the
 * frequency simplification; serverTruncated/-ShownLinkCount/-TotalLinkCount
 * is the server's own link cap (see emit_transitions_response /
 * server.c:6017-6019) — a cause this builder can disclose but never
 * recover from (no client-side fix can see links the server did not send).
 * omittedDominant names the single largest-total_ms node hidden by the
 * threshold (findOmittedDominantNode) so a rare-but-dominant wait is named
 * instead of silently vanishing. */
export function buildTransitionsOption(data, threshold, dims, posOverrides, hideIdle) {
    const error = transitionsDataError(data);
    if (error) throw new Error('Transitions data incomplete: ' + error);
    dims = dims || {};
    const cW = dims.width || 800, cH = dims.height || 550;
    const rawLinks = (data && data.links) || [];
    const total = (data && data.total) || 0;
    const { links, hiddenLinks: hiddenIdleLinks, hiddenValue: hiddenIdleValue } =
        filterIdleLinks(rawLinks, hideIdle);
    // nodeMap is built from the FULL (pre-idle-filter) data.nodes — spread
    // below carries data.nodes through unfiltered — so findOmittedDominantNode
    // can see a node hidden by the threshold OR by hideIdle, not just the
    // survivors transitionsContext would otherwise size/color against.
    const { maxEdgeCount, nodeMap } = transitionsContext({ ...(data || {}), links });

    const minCount = maxEdgeCount * threshold / 100;
    const visibleLinks = links.filter(l => l.value >= minCount);
    const hiddenThresholdLinks = links.length - visibleLinks.length;
    const hiddenThresholdValue = links.reduce(
        (sum, l) => sum + (l.value >= minCount ? 0 : (l.value || 0)), 0);
    const visibleNodes = new Set();
    visibleLinks.forEach(l => { visibleNodes.add(l.source); visibleNodes.add(l.target); });

    const disclosure = {
        hiddenIdleLinks, hiddenIdleValue,
        hiddenThresholdLinks, hiddenThresholdValue,
        shownLinks: visibleLinks.length,
        totalLinks: rawLinks.length,
        serverTruncated: !!(data && data.truncated),
        serverShownLinkCount: Number.isFinite(Number(data && data.link_count))
            ? Number(data.link_count) : rawLinks.length,
        serverTotalLinkCount: Number.isFinite(Number(data && data.total_link_count))
            ? Number(data.total_link_count) : rawLinks.length,
        omittedDominant: findOmittedDominantNode(nodeMap, visibleNodes),
        // #456: persistent, computed regardless of whether anything is
        // actually hidden right now — see findDominantSourceDwellNode.
        dominantSourceDwell: findDominantSourceDwellNode(nodeMap, visibleNodes),
    };

    if (visibleNodes.size === 0) {
        return { option: null, visibleCount: 0, ...disclosure };
    }

    const maxNodeMs = Math.max(...[...visibleNodes].map(n => (nodeMap[n]?.total_ms || 1)));
    const maxLinkVal = Math.max(...visibleLinks.map(l => l.value));
    // #456: duration descending, numeric event identity ascending, name
    // fallback -- see compareTransitionNodesByDominance above. The old
    // `.sort((a,b) => b.ms - a.ms)` left ties ordered by [...visibleNodes]'s
    // Set-insertion order, itself driven by the server's `links` array
    // order, not by anything about the nodes themselves.
    const sortedNodes = [...visibleNodes]
        .map(name => ({ name, ms: (nodeMap[name]?.total_ms || 0),
            eventId: (nodeMap[name] && nodeMap[name].event_id != null)
                ? nodeMap[name].event_id : null }))
        .sort(compareTransitionNodesByDominance);
    const n = sortedNodes.length;
    const cols = Math.ceil(Math.sqrt(n * 1.8));
    const rows = Math.ceil(n / cols);

    // #436: size the margins to the ACTUAL node circles + labels being laid
    // out (estimateNodeFootprint), not a fixed guess — a long event name or
    // a max-size (120px) node both need more clearance than the old flat
    // 80/60. Clamped so a pathologically small container degrades (same as
    // before the fix) instead of producing a negative cell size.
    const sizes = sortedNodes.map(nd => dfgSymbolSize(nd.ms, maxNodeMs));
    const footprints = sortedNodes.map((nd, i) => estimateNodeFootprint(nd.name, sizes[i]));
    const padX = Math.min(
        Math.max(...footprints.map(f => f.halfWidth)) + 14,
        Math.max(cW / 2 - 1, 1));
    const padYTop = Math.min(
        Math.max(...footprints.map(f => f.top)) + 10,
        Math.max(cH / 2 - 1, 1));
    const padYBottom = Math.min(
        Math.max(...footprints.map(f => f.bottom)) + 10,
        Math.max(cH / 2 - 1, 1));
    const cellW = (cW - padX * 2) / Math.max(cols - 1, 1);
    const cellH = (cH - padYTop - padYBottom) / Math.max(rows - 1, 1);
    // #436: rank-0 (highest total_ms — typically the CPU* hub, #438) lands
    // on the CENTER cell instead of grid corner (0,0); see computeGridOrder.
    const order = computeGridOrder(n, cols, rows);
    const posByName = {};

    const ecNodes = sortedNodes.map((nd, i) => {
        const info = nodeMap[nd.name] || {};
        const ms = nd.ms;
        // U2 (U1-F4 backlog): eventColor, not the flat class hue — "same event
        // = same color" now holds against the AAS chart and every other view.
        const color = eventColor(info.class != null ? info.class : null, nd.name);
        const size = sizes[i];
        const timeStr = ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : ms.toFixed(0) + 'ms';
        const { col, row } = order[i];
        const ov = posOverrides ? posOverrides[nd.name] : null;
        const x = ov ? ov.x : padX + col * cellW;
        const y = ov ? ov.y : padYTop + row * cellH;
        posByName[nd.name] = { x, y };
        return {
            name: nd.name,
            x, y,
            // P3 wire 5: the server-emitted event_id rides on the node so a
            // click can become the 'dfg-event' pivot intent (0 = CPU* pseudo-
            // node, absent on servers predating the field).
            eventId: info.event_id != null ? info.event_id : null,
            className: info.class || null,
            symbolSize: size,
            itemStyle: { color, borderColor: color, borderWidth: 2 },
            label: {
                show: true, position: 'bottom', fontSize: DFG_FONT_SIZE, color: '#ccc',
                formatter: (nd.name.indexOf(':') > 0
                    ? nd.name.substring(nd.name.indexOf(':') + 1) : nd.name) +
                    '\n' + timeStr,
                lineHeight: DFG_LABEL_LINE_HEIGHT,
            },
            // Tooltips render as HTML — escape the (server-derived) name too,
            // same bug class as the timeline query-text injection (UI-6).
            tooltip: { formatter: '<b>' + escHtml(nd.name) + '</b><br/>Total time: ' + timeStr },
            value: ms,
        };
    });

    // #436: curveness bows a cross-edge's arc away from the straight line
    // between its two endpoints by (curveness * distance) — the standard
    // quadratic-Bezier-offset model ECharts' graph series uses for a plain
    // numeric curveness. A fixed 0.3 was fine while every edge was short
    // (grid-adjacent), but the center-hub layout above still has the hub
    // fan out to nodes most of the canvas away, and 0.3 * a long distance
    // can bow past whatever margin that far node's OWN cell reserved. Cap
    // curveness per edge so the bow never exceeds the smallest reserved
    // margin (padX/padYTop/padYBottom) — short edges keep the full 0.3
    // curve, long ones flatten toward straight rather than arc off-canvas.
    // Self-loops (source===target, distance 0) are a different, node-local
    // shape and keep the fixed 0.8.
    const safeMargin = Math.min(padX, padYTop, padYBottom);
    const ecLinks = visibleLinks.map(l => {
        const w = Math.max(1, Math.min(10, (l.value / maxLinkVal) * 10));
        const pct = (l.value / total * 100).toFixed(1);
        const avg = l.value > 0 ? (l.duration_ms / l.value).toFixed(1) : '?';
        const isSelf = l.source === l.target;
        let curveness = isSelf ? 0.8 : 0.3;
        if (!isSelf) {
            const p1 = posByName[l.source], p2 = posByName[l.target];
            const dist = p1 && p2 ? Math.hypot(p2.x - p1.x, p2.y - p1.y) : 0;
            if (dist > 0) curveness = Math.min(curveness, safeMargin / dist);
        }
        return {
            source: l.source, target: l.target,
            lineStyle: {
                width: w, color: '#555',
                curveness,
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
            // #436 (adviser-caught, post-review): ECharts' graph
            // series, even with layout:'none', normalizes node x/y through
            // its own "view" coordinateSystem, which by default FITS the
            // laid-out nodes' own bounding box into the series' box
            // (left/top/width/height — here the whole container), not a
            // 1:1 pixel mapping. `boundingCoords` (the geo component's
            // escape hatch) does NOT apply to graph series at all — verified
            // empirically by setting it and observing byte-identical
            // rendered coordinates with and without it. What DOES work,
            // verified against this exact vendored build
            // (web/static/vendor/echarts.min.js, ECharts 5.5.1) via
            // `chart.getModel().getSeriesByIndex(0).coordinateSystem
            // .dataToPoint([x,y])` on a real rendered instance:
            //   1. `left:0, top:0, width:cW, height:cH` pins the series' own
            //      box to the exact container rect (no ambiguous "center"
            //      default, which carries its own ~80% shrink-to-fit
            //      margin when width/height are left unset).
            //   2. Two invisible (`symbol:'none', silent:true`) anchor
            //      nodes at the box's exact top-left (0,0) and bottom-right
            //      (cW,cH) corners (layoutAnchorNodes, appended below,
            //      never part of `ecNodes`/visibleCount/sortedNodes) pin the
            //      nodes' OWN bounding box to those same exact dimensions —
            //      without this, box (1) alone still independently
            //      stretches each axis to fill it non-uniformly, distorting
            //      circles into ellipses.
            // Together, both pinned boxes are identical in origin, size AND
            // aspect ratio, so the fit computation's scale factor is
            // exactly 1 on both axes: data coordinates become pixel
            // coordinates, the invariant every padding/margin/curveness
            // computation above assumes. test_web_ui.py's live clipping
            // check now reads real `coordinateSystem.dataToPoint()` output
            // (never raw getOption() x/y) specifically because this is true
            // ONLY as a property of this series config, not of the option
            // object in isolation.
            left: 0, top: 0, width: cW, height: cH,
            data: ecNodes.concat(layoutAnchorNodes(cW, cH)), links: ecLinks,
            edgeSymbol: ['none', 'arrow'], edgeSymbolSize: [0, 8],
            emphasis: { focus: 'adjacency', lineStyle: { width: 4, opacity: 1 } },
            lineStyle: { curveness: 0.3 },
        }],
    };
    // margins: exposed (not just used internally) so the viewport-fit unit
    // tests (#436) can check laid-out nodes/labels/edge-arcs against the
    // SAME reserved space the layout computed, instead of a second,
    // independently-guessed constant that could silently drift from it.
    return { option, visibleCount: ecNodes.length, margins: { padX, padYTop, padYBottom },
        ...disclosure };
}

/* PURE: assemble the one-line DFG disclosure string (#436) — total count,
 * then the three DISTINCT omission causes (never combined into one number),
 * then the named omitted-dominant wait if there is one. `built` is
 * buildTransitionsOption's return value; `serverTotal` is data.total. Pure
 * so the text contract (which reasons appear, in what words) has a test
 * that does not require a DOM. The view assigns this string to a node's
 * `textContent`, never `innerHTML` (see renderDFG) — unlike the tooltip/
 * label formatters above, this is plain text, so a hostile node name must
 * NOT be run through escHtml here (that would show the user a literal
 * "&lt;" instead of escaping a markup sink that does not exist in this
 * path). */
export function buildTransitionsDisclosure(built, serverTotal) {
    const total = Number(serverTotal) || 0;
    const parts = [total.toLocaleString() + ' transitions'];
    if (built.hiddenIdleLinks > 0) {
        parts.push(Number(built.hiddenIdleValue).toLocaleString() +
            ' idle (ClientRead) hidden');
    }
    if (built.hiddenThresholdLinks > 0) {
        parts.push(Number(built.hiddenThresholdValue).toLocaleString() +
            ' below the simplify threshold');
    }
    if (built.serverTruncated) {
        parts.push('server returned ' + built.serverShownLinkCount.toLocaleString() +
            ' of ' + built.serverTotalLinkCount.toLocaleString() + ' links');
    }
    if (built.omittedDominant) {
        parts.push(built.omittedDominant.name + ' omitted (' +
            fmtMs(built.omittedDominant.ms) + ' total) — Show all to include it');
    }
    // #456: PERSISTENT — stated every render, not only when something is
    // hidden (unlike the omittedDominant phrase above, which only fires when
    // the dominant-dwell node happens to be one of the excluded ones). When
    // the globally dominant node IS the omitted one, omittedDominant above
    // already names it with the "Show all" cue, so this only adds a second
    // phrase for the VISIBLE case — the one buildTransitionsDisclosure used
    // to say nothing about at all.
    if (built.dominantSourceDwell && built.dominantSourceDwell.visible) {
        parts.push(built.dominantSourceDwell.name + ' is the largest source-dwell node (' +
            fmtMs(built.dominantSourceDwell.ms) + '), shown');
    }
    return parts.join(' · ');
}

/* PURE: a variant's p95 truncation marker (issue #273). The server caps the
 * p95 sample at PGWT_VARIANT_MAX_SAMPLES executions, taken in arrival order
 * (a prefix, not a random sample — biased, not merely approximate: #272/#271),
 * and reports how many it actually used in `p95_sample_n`. Returns '' when
 * the field is absent (older server — render exactly as before, no marker,
 * no crash) or when the sample covered every execution. Driven entirely by
 * p95_sample_n vs exec_count; the 10,000 cap itself is never hard-coded here. */
export function p95TruncationMarker(v) {
    if (!v || v.p95_sample_n == null || v.exec_count == null) return '';
    if (v.p95_sample_n >= v.exec_count) return '';
    const n = v.p95_sample_n.toLocaleString();
    return ' <span style="color:#d9a441;font-size:10px" title="p95 computed from the first ' +
        n + ' of ' + v.exec_count.toLocaleString() + ' executions (arrival-order sample, biased)">' +
        '(first ' + n + ')</span>';
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
            // U2 (U1-F4 backlog): variant steps use the eventColor identity
            // tint too — a step is the same event the DFG/AAS color.
            const color = eventColor(st.class != null ? st.class : null, st.name);
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
            ' · avg ' + avgStr + ' · p95 ' + p95Str + p95TruncationMarker(v) + ' · total ' + totalStr +
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

/* PURE: the variants panel's own state. `variants` measures ~5.5s against a
 * realistic 900s window on a single-threaded server (issue follow-up to
 * #291) — expensive enough that it must never fire on its own. It is now
 * ON DEMAND: the panel declares which of four states it is in, and only a
 * user click (wired in the view) ever moves it out of 'idle':
 *   'idle'    — not requested. The discoverable affordance: a button, so the
 *               user can tell data is AVAILABLE on request rather than
 *               wondering why the panel is empty (an idle panel and a
 *               genuinely-empty 'ready' one must not read the same).
 *   'pending' — the user clicked and the request is in flight: say so, so
 *               the panel reads neither as idle nor complete.
 *   'ready'   — rendered normally (identical to the old synchronous path).
 *   'error'   — the request failed or was superseded: silent, matching the
 *               pre-existing optional-failure behaviour (the old try/catch
 *               left `variants` null and rendered nothing extra). */
export function buildVariantsPanel(state, vdata, esc) {
    if (state === 'idle') {
        return '<div style="padding:10px 20px">' +
            '<button class="escalate-btn" id="dfg-load-variants" type="button">' +
                'Load flow variants</button> ' +
            '<span style="color:#666;font-size:11px">— execution/planning patterns ' +
                'for this window (can take several seconds)</span></div>';
    }
    if (state === 'pending') {
        return '<div style="padding:10px 20px;color:#888;font-size:12px">' +
            'Loading flow variants…</div>';
    }
    if (state === 'ready') return buildVariantsHtml(vdata, esc);
    return '';   // 'error' (or anything unrecognized): silent, never a stale look
}
