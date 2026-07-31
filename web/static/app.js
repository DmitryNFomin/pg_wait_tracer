/* pgwt — Web Investigation Client: bootstrap + router.
 *
 * B3 (complete): the old ~2000-line monolith is now fully restructured into
 * native ES modules (no build step — still embeddable via go:embed). THIS FILE
 * IS BOOTSTRAP-ONLY: WebSocket lifecycle, the time-window / live-mode controls,
 * tab routing, and view registration. It contains NO view-specific rendering and
 * NO module-level chart variables — every view (active, overview, events,
 * sessions, queries, histogram, timeline, transitions, concurrency) lives in
 * views/ as { id, requests, build (PURE), mount, enter, leave } and OWNS its own
 * ECharts instance (created on enter/mount, disposed in leave).
 *
 * State is explicit (lib/state.js): no grab-bag global mutated mid-flight.
 * Stale-response superseding is structural (lib/transport.js channels +
 * lib/view-manager.js epochs), replacing the old _refreshGen counters.
 */

import { ServerInfo, TimeRange, FilterStack } from './lib/state.js';
import { Transport, TransportError, CancelledError } from './lib/transport.js';
import { ViewManager } from './lib/view-manager.js';
import { mountTable } from './lib/table.js';
import {
    classColor, fmtTime, fmtDuration, esc, fmtCount,
    nsToDatetimeLocalUTC, datetimeLocalUTCToNs, versionSkew,
} from './lib/format.js';
import { controlStatus, controlMetrics, ControlUnavailable } from './lib/control.js';
import { mountEscalateControl, mountMetricsPanel } from './lib/panels.js';
import { createActiveView } from './views/active.js';
import { createOverviewView } from './views/overview.js';
import { createEventsView } from './views/events.js';
import { createSessionsView } from './views/sessions.js';
import { createQueriesView } from './views/queries.js';
import { createHistogramView } from './views/histogram.js';
import { createTimelineView } from './views/timeline.js';
import { createTransitionsView } from './views/transitions.js';
import { createConcurrencyView } from './views/concurrency.js';

// ── Core services ─────────────────────────────────────────────────────────────

const server = new ServerInfo();
const timeRange = new TimeRange(server);
const filters = new FilterStack();
const transport = new Transport();

let activeView = null;       // persistent AAS chart view ("active")
let vm = null;               // ViewManager for tab views

// ── Daemon control plane (B5) ─────────────────────────────────────────────────
// Latest daemon status/metrics from the control socket (proxied by pgwt-server
// as the `control` command). null until first poll; `available` flips false the
// first time the daemon is unreachable (static-trace replay) so we stop polling
// and hide the escalation UI. Views read `daemon.status` synchronously via the
// ctx hook to render the AAS escalation annotation + unavailable panels.
const daemon = { status: null, metrics: null, available: true, polled: false,
                 escalationStartNs: null };
let metricsPanelOpen = false;
let daemonPollId = 0;

// Reconnect bookkeeping (lifecycle only — not request state).
let reconnectDelay = 2000;
let reconnectTimer = null;

// One-time UI wiring guard (UI-4): chart instances, tab routing, and DOM
// listeners are created exactly once; every later WS (re)connect goes through
// the resync path instead of re-running init.
let uiInitialized = false;

// ── Degraded-transport state (UI-1) ───────────────────────────────────────────
// The bridge stays connected while the SSH/pgwt-server pipe behind it dies: the
// only signal is error envelopes on requests. This state makes that failure
// VISIBLE (red status pill + connection-lost overlay over the data panes) and
// distinct from "no data in range". While degraded, live mode stops pretending
// to tick (the window freezes); a background probe re-tries `info` and, on
// success, resyncs — server info refreshed and any live window re-anchored to
// NOW (never to stale data).
const degraded = { active: false, reason: null };
let recoverId = 0;

// Version handshake (T7 / TST-11). The Go client reports its build version +
// protocol via /session; the server reports its own in `info`. On skew we show
// a banner (warn, never refuse — a skewed pair is the normal deployment state).
// Against the mock server (no /session) these stay null and no banner shows.
let clientVersion = null;
let clientProtocol = null;

// Auto-refresh (live mode) bookkeeping.
let autoRefreshId = 0;
let autoRefreshOn = false;

// Sort state per table view. getSort returns the current { key, asc } (or null
// for server order); toggleSort cycles desc→asc on repeated clicks of the same
// column. Kept here (not in a view) so it survives tab switches, matching the
// old per-tab behavior.
const tabSort = {};   // tab -> { key, asc }
function getSort(tab) { return tabSort[tab] || null; }
function toggleSort(tab, key) {
    const cur = tabSort[tab];
    if (cur && cur.key === key) tabSort[tab] = { key, asc: !cur.asc };
    else tabSort[tab] = { key, asc: false };
}

// ── DOM handles (resolved once at bootstrap) ──────────────────────────────────

let chartEl, tableEl, summaryEl, tooltipEl;

// ── Shared ctx handed to every view hook ──────────────────────────────────────

function makeCtx() {
    return {
        transport, server, timeRange, filters,
        echarts: window.echarts,
        chartEl, summaryEl, tooltipEl,
        mountTable,
        setStatus,
        onDrill: drill,
        onZoom: (from, to) => { stopAutoRefresh(); zoomTo(from, to); },
        // Table views: per-tab sort state + a re-render hook used after a
        // header-sort click (re-runs requests/build/mount under a fresh epoch).
        getSort, toggleSort,
        refresh: () => vm.refresh(),
        // Daemon control plane (B5): views read the latest escalation status to
        // render the AAS annotation + the unavailable/escalate panels, and can
        // re-poll it after an escalate/deescalate action.
        getEscalationStatus: () => daemon.status,
        refreshEscalationStatus: () => pollDaemon(),
        onEscalationChanged: () => { renderEscalateControl(); refreshActive(); },
        // UI-1: request failures bubble here (view-manager chokepoint + the
        // active view) so a dead transport becomes visible, not "No data".
        onRequestError: onRequestError,
        // P1: view build/mount throws bubble here (view-manager) so a broken
        // view paints a per-pane error card, never a silent stale pane.
        onViewError: onViewError,
    };
}

// ── WebSocket lifecycle ───────────────────────────────────────────────────────

/* UI-12: fetch the per-session WS token from the Go server. Same-origin
 * policy is what protects it — a foreign page cannot read this response. The
 * mock server has no /session endpoint; connect without a token then. */
async function fetchSessionToken() {
    try {
        const r = await fetch('/session');
        if (!r.ok) return null;
        const j = await r.json();
        if (j) {
            // T7 / TST-11: the Go client reports its own build version +
            // protocol here so we can compare against the server's `info`.
            if (j.client_version != null) clientVersion = j.client_version;
            if (j.protocol != null) clientProtocol = j.protocol;
        }
        return (j && j.token) || null;
    } catch (e) {
        return null;
    }
}

async function connect() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    setStatus('Connecting...', 'connecting');
    const token = await fetchSessionToken();
    // Test-harness override: `?ws=<full ws url>` is used verbatim as the WS
    // endpoint (mock reachability — no Go bridge, no token). Honored only on
    // loopback: a crafted link must not point a real deployment's data plane
    // at an attacker-controlled socket (U0 review F1).
    const isLoopback = location.hostname === '127.0.0.1' ||
        location.hostname === 'localhost' || location.hostname === '[::1]';
    const wsOverride = isLoopback
        ? new URLSearchParams(location.search).get('ws') : null;
    const url = wsOverride || ('ws://' + location.host + '/ws' +
        (token ? '?token=' + encodeURIComponent(token) : ''));
    const ws = new WebSocket(url);

    ws.onopen = () => {
        transport.attach(ws);
        reconnectDelay = 2000;
        onConnected();
    };
    ws.onclose = () => {
        transport.ws = null;
        transport.rejectAll('disconnected');
        stopDaemonPoll();
        if (uiInitialized) setDegraded('disconnected');
        scheduleReconnect();
    };
    ws.onerror = () => { /* onclose follows */ };
}

function scheduleReconnect() {
    const delay = reconnectDelay;
    reconnectDelay = Math.min(delay * 2, 16000);
    setStatus('Reconnecting in ' + (delay / 1000) + 's...', 'error');
    reconnectTimer = setTimeout(connect, delay);
}

// ── Init / reconnect resync (UI-4) ────────────────────────────────────────────
//
// One-time setup (chart instances, tab routing, DOM listeners) runs exactly
// once. Every subsequent WS connection goes through the RESYNC path: refresh
// server info, re-anchor a live window to NOW, repaint — creating nothing, so
// N reconnects leak zero chart instances and zero listeners.

async function onConnected() {
    setStatus('Loading...', 'connecting');
    try {
        const info = await transport.send('info');
        server.update(info);
        renderVersionSkew();

        if (!uiInitialized) {
            timeRange.initDefault();
            initOnce();
            uiInitialized = true;
            setStatus(server.numCpus + ' CPUs', 'connected');
            updateTimeRange();

            // Poll the daemon control plane BEFORE the first paint so the AAS
            // escalation annotation + the escalate control are correct on load.
            await pollDaemon();
            startDaemonPoll();

            await refresh();
            // Start in live mode. The span comes from timeRange.liveRangeSecs
            // (the ONE source of truth for the live span; initDefault set the
            // 900s = 15 min boot default) — the Live button resumes it (P4).
            startAutoRefresh(timeRange.liveRangeSecs);
            return;
        }

        // Reconnect resync: the transport is healthy again.
        clearDegraded();
        setStatus(server.numCpus + ' CPUs', 'connected');
        if (autoRefreshOn) {
            // "Last N min" must mean NOW, never the pre-disconnect window.
            timeRange.anchorLive(timeRange.liveRangeSecs);
        }
        updateTimeRange();
        await pollDaemon();
        startDaemonPoll();
        await refresh();
    } catch (e) {
        setStatus('Error: ' + e.message, 'error');
    }
}

function initOnce() {
    initChartView();
    initChartResize();
    initTabs();
    initTimePicker();
    initLiveMode();
    initMetricsButton();
    window.addEventListener('resize', onResize);
}

// ── Degraded-transport handling (UI-1) ────────────────────────────────────────

function onRequestError(e, pane) {
    if (e instanceof CancelledError) return;
    if (e instanceof TransportError && e.transport) { setDegraded(e.message); return; }
    // P1: a command-level error from a healthy server (transport:false
    // envelope — e.g. the DUR-9 window_too_large refusal) used to be silently
    // dropped here. Make it loud and paint it into the failing pane.
    console.error('[pgwt] command error:', (pane && pane.view) || '(no pane)', e);
    if (pane) {
        renderPaneError(pane.el, {
            view: pane.view, cmd: e.cmd, code: e.code, hint: e.hint,
            maxEvents: e.maxEvents, message: e.message, retry: pane.retry,
        });
    }
}

function setDegraded(reason) {
    if (degraded.active) { degraded.reason = reason; renderDegraded(); return; }
    degraded.active = true;
    degraded.reason = reason;
    renderDegraded();
    startRecoveryProbe();
}

function clearDegraded() {
    recoverId++;   // stop any running probe
    if (!degraded.active) return;
    degraded.active = false;
    degraded.reason = null;
    renderDegraded();
}

function renderDegraded() {
    const overlay = document.getElementById('degraded-overlay');
    if (overlay) {
        overlay.style.display = degraded.active ? 'flex' : 'none';
        const reasonEl = document.getElementById('degraded-reason');
        if (reasonEl) reasonEl.textContent = degraded.reason || '';
    }
    if (degraded.active) setStatus('Connection lost — retrying', 'degraded');
}

/* While degraded with the WS still open (dead SSH behind a live bridge), probe
 * `info` until the server answers again, then resync. If the WS itself is
 * closed, the reconnect loop owns recovery (its resync clears the state). */
function startRecoveryProbe() {
    const myId = ++recoverId;
    (async function loop() {
        while (recoverId === myId && degraded.active) {
            await new Promise(r => setTimeout(r, 2500));
            if (recoverId !== myId || !degraded.active) break;
            if (!transport.isOpen()) continue;   // WS reconnect path owns this
            try {
                const info = await transport.send('info');
                if (recoverId !== myId) break;
                server.update(info);
                await onTransportRecovered();
                break;
            } catch (e) { /* still down; retry */ }
        }
    })();
}

async function onTransportRecovered() {
    clearDegraded();
    setStatus(server.numCpus + ' CPUs', 'connected');
    if (autoRefreshOn) {
        // Re-anchor the live window to NOW — never resume a stale window.
        timeRange.anchorLive(timeRange.liveRangeSecs);
    }
    updateTimeRange();
    await pollDaemon();
    await refresh();
}

// ── Per-pane error card (P1) ──────────────────────────────────────────────────
//
// A failed pane must LOOK failed — before this, broken ≡ empty ≡ stale. The
// card is PREPENDED into the pane (never clobbering a view's chart DOM, which
// shell-preserving views like the histogram reuse across refreshes) and removed
// on the next successful paint; a "Loading..." placeholder is replaced, never
// left up forever. Look mirrors the unavailable-panel: low-key, dark-theme.

/* Hook for the view-manager: a view build/mount throw becomes a visible card
 * (the console.error happens at the throw site). `pane` = { view, el, retry }. */
function onViewError(pane, err) {
    renderPaneError(pane.el, { view: pane.view, message: err && err.message,
                               retry: pane.retry });
}

/* Paint the card. info: { view, cmd, code, hint, maxEvents, message, retry }. */
function renderPaneError(el, info) {
    if (!el) return;
    clearPaneError(el);
    const loading = el.querySelector(':scope > .loading');
    if (loading) loading.remove();   // never an eternal "Loading..."

    let title;
    if (info.code === 'window_too_large') {
        // DUR-9: the server refused a PARTIAL load — say so in analyst terms.
        title = 'Window too large for full detail' +
            (info.maxEvents ? ' — over ' + fmtCount(info.maxEvents) + ' events' : '') +
            '; zoom in or pick a shorter range';
    } else {
        title = info.message || 'view failed to render';
    }
    const detail = [info.view ? 'view: ' + info.view : '',
                    info.cmd ? 'command: ' + info.cmd : '',
                    info.code ? 'code: ' + info.code : '']
        .filter(Boolean).join(' · ');

    const card = document.createElement('div');
    card.className = 'pane-error';
    card.innerHTML =
        '<div class="pane-error-title">⚠ ' + esc(title) + '</div>' +
        (detail ? '<div class="pane-error-detail">' + esc(detail) + '</div>' : '') +
        (info.hint ? '<div class="pane-error-hint">' + esc(info.hint) + '</div>' : '') +
        '<button class="pane-error-retry">Retry</button>';
    const btn = card.querySelector('.pane-error-retry');
    btn.addEventListener('click', () => {
        btn.disabled = true;
        btn.textContent = 'Retrying…';
        if (info.retry) info.retry();   // success removes the card; failure repaints it
    });
    el.prepend(card);
}

function clearPaneError(el) {
    if (!el) return;
    const card = el.querySelector(':scope > .pane-error');
    if (card) card.remove();
}

// ── Refresh orchestration ─────────────────────────────────────────────────────
//
// One refresh = (1) re-render the persistent active/AAS chart, then (2) ask the
// view-manager to refresh the active tab view. Both run under transport
// single-flight + the view-manager epoch chokepoint, so a user action mid-flight
// supersedes stale responses without any manual generation counters.

async function refresh() {
    await refreshActive();
    await vm.refresh();
}

async function refreshActive() {
    if (!activeView) return;
    const ctx = activeCtx();
    let data;
    try {
        data = await activeView.requests(ctx);
    } catch (e) {
        // Superseded: silent. Transport failure: surface the degraded state
        // (UI-1) — the current paint stays but under the connection-lost
        // overlay, never pretending to be fresh. Command-level errors paint
        // an error card into the chart pane (P1).
        onRequestError(e, { view: 'active', el: chartEl, retry: refreshActive });
        return;
    }
    let model;
    try {
        model = activeView.build(data, ctx);
    } catch (e) {
        // P1: loud + visible — never a silently stale AAS chart.
        console.error('[pgwt] view build failed:', 'active', e);
        renderPaneError(chartEl, { view: 'active', message: e && e.message,
                                   retry: refreshActive });
        return;
    }
    try {
        activeView.mount(chartEl, model, ctx);
        clearPaneError(chartEl);
    } catch (e) {
        console.error('[pgwt] view mount failed:', 'active', e);
        renderPaneError(chartEl, { view: 'active', message: e && e.message,
                                   retry: refreshActive });
    }
}

// ctx for the persistent active view: it manages its own single-flight channel
// (not tied to the tab view-manager) so tab switches never cancel the AAS fetch.
function activeCtx() {
    return Object.assign(makeCtx(), {
        viewId: 'active',
        channel: (name) => 'active.' + name,
    });
}

// ── Chart view (persistent AAS) ───────────────────────────────────────────────

function initChartView() {
    activeView = createActiveView();
    activeView.enter(activeCtx());
    // Double-click to zoom out (kept from the old chart behavior).
    chartEl.addEventListener('dblclick', (e) => { e.preventDefault(); stopAutoRefresh(); zoomOut(); });
}

function onResize() {
    if (activeView && activeView.resize) activeView.resize();
    if (vm.active && vm.active.resize) vm.active.resize();
}

// ── Tab routing via the view-manager ──────────────────────────────────────────

function initTabs() {
    vm = new ViewManager(makeCtx());
    vm.setContainer(tableEl);

    vm.register(createOverviewView());
    vm.register(createEventsView());
    vm.register(createSessionsView());
    vm.register(createQueriesView());
    vm.register(createHistogramView());
    vm.register(createTimelineView());
    vm.register(createTransitionsView());
    vm.register(createConcurrencyView());

    // P4: these views are too heavy for the 5s live loop — entering them
    // pauses live mode. Set here as a view property because the view
    // definitions (views/*.js) are outside this change's slice; follow-up:
    // declare `pausesLive` in the view objects themselves.
    vm.views['transitions'].pausesLive = true;
    vm.views['concurrency'].pausesLive = true;

    // Enter the default tab without a network refresh (init() does the first).
    vm.active = vm.views['overview'];
    vm.active.enter(vm._viewCtx());

    document.querySelectorAll('.tab').forEach(btn => {
        btn.addEventListener('click', () => switchTab(btn.dataset.tab));
    });
}

function setActiveTabButton(tab) {
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === tab);
    });
}

async function switchTab(tab) {
    const next = vm.views[tab];
    if (next && next.pausesLive) stopAutoRefresh();
    setActiveTabButton(tab);
    summaryEl.innerHTML = '';
    tableEl.innerHTML = '<div class="loading">Loading...</div>';
    // The active/AAS chart reflects the same window across tabs; refresh it too.
    await refreshActive();
    await vm.switchTo(tab);
}

// Switch the active tab WITHOUT triggering a separate refresh (used by drill
// navigation, which refreshes once afterwards). Returns nothing.
function setTabView(tab) {
    if (vm.active && vm.activeId() !== tab) {
        try { vm.active.leave(); } catch (e) { /* best-effort */ }
        vm.active = vm.views[tab];
        try { vm.active.enter(vm._viewCtx()); } catch (e) { /* best-effort */ }
    }
    setActiveTabButton(tab);
}

// ── Drill-down / breadcrumb ───────────────────────────────────────────────────

const PIVOT = { class: 'events', event_id: 'queries', pid: 'timeline', query_id: 'events' };

function drill(intent) {
    if (!intent) return;
    stopAutoRefresh();   // P4: drilling is an investigation gesture, like zoom
    filters.drill(intent.filterKey, intent.filterValue, intent.label, vm.activeId());
    const pivot = PIVOT[intent.filterKey];
    if (pivot) setTabView(pivot);
    updateBreadcrumb();
    refresh();
}

function drillUp(index) {
    stopAutoRefresh();   // P4: breadcrumb navigation pauses live too
    const view = filters.drillUp(index);
    if (view) setTabView(view);
    updateBreadcrumb();
    refresh();
}

function clearFilters() {
    stopAutoRefresh();   // P4: ✕ rewinds the investigation — Live is the way back
    filters.clear();
    setTabView('overview');
    updateBreadcrumb();
    refresh();
}

function updateBreadcrumb() {
    const el = document.getElementById('breadcrumb');
    if (filters.isEmpty()) { el.innerHTML = ''; return; }

    let html = '';
    filters.breadcrumbs.forEach((crumb, i) => {
        if (i > 0) html += '<span class="crumb-sep">›</span>';
        html += '<span class="crumb" data-idx="' + i + '">' +
            dotHtml(crumb.label) + esc(crumb.label) + '</span>';
    });

    if (Object.keys(filters.filters).length > 0) {
        if (filters.breadcrumbs.length > 0) html += '<span class="crumb-sep">›</span>';
        const label = filters.currentLabel ||
            Object.entries(filters.filters).map(([k, v]) => k + '=' + v).join(', ');
        html += '<span style="color:#4fc3f7">' + dotHtml(label) + esc(label) + '</span>';
        html += ' <span class="crumb-clear" title="Clear all filters">✕</span>';
    }
    el.innerHTML = html;

    el.querySelectorAll('.crumb').forEach(crumb => {
        crumb.addEventListener('click', () => drillUp(parseInt(crumb.dataset.idx)));
    });
    const clearBtn = el.querySelector('.crumb-clear');
    if (clearBtn) clearBtn.addEventListener('click', clearFilters);
}

function dotHtml(name) {
    const color = classColor(name);
    if (!color) return '';
    return '<span class="class-dot" style="background:' + color + '"></span>';
}

// ── Time window / zoom ────────────────────────────────────────────────────────

function setStatus(text, cls) {
    const el = document.getElementById('status');
    el.textContent = text;
    el.className = 'status ' + cls;
}

/* T7 / TST-11: reflect any client/server version skew in the header banner.
 * No-op when the client version is unknown (mock server / plain browser load
 * with no /session endpoint). */
function renderVersionSkew() {
    const el = document.getElementById('version-skew');
    if (!el) return;
    if (clientVersion == null) { el.style.display = 'none'; return; }
    const skew = versionSkew(clientVersion, clientProtocol,
                             server.serverVersion, server.protocol);
    if (!skew) { el.style.display = 'none'; return; }
    el.style.display = '';
    el.textContent = '⚠ ' + skew.short;
    el.title = skew.detail;
    el.className = 'version-skew ' + skew.level;
}

function updateTimeRange() {
    const el = document.getElementById('time-range');
    const from = fmtTime(timeRange.from);
    const to = fmtTime(timeRange.to);
    const dur = fmtDuration(timeRange.span());
    // All pgwt times are UTC — say so (UI-11).
    el.textContent = from + ' – ' + to + ' UTC (' + dur + ')';
}

async function zoomTo(from, to) {
    timeRange.zoomTo(from, to);
    updateTimeRange();
    await refresh();
}

async function zoomOut() {
    timeRange.zoomOut();
    updateTimeRange();
    await refresh();
}

// ── Chart resize handle ───────────────────────────────────────────────────────

function initChartResize() {
    const handle = document.getElementById('chart-resize');
    let startY = 0, startH = 0;
    handle.addEventListener('mousedown', (e) => {
        e.preventDefault();
        startY = e.clientY;
        startH = chartEl.offsetHeight;
        document.addEventListener('mousemove', onDrag);
        document.addEventListener('mouseup', onUp);
        document.body.style.cursor = 'ns-resize';
        document.body.style.userSelect = 'none';
    });
    function onDrag(e) {
        const h = Math.max(120, startH + e.clientY - startY);
        chartEl.style.height = h + 'px';
        if (activeView && activeView.resize) activeView.resize();
    }
    function onUp() {
        document.removeEventListener('mousemove', onDrag);
        document.removeEventListener('mouseup', onUp);
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
    }
}

// ── Time picker ───────────────────────────────────────────────────────────────

function initTimePicker() {
    const rangeBtn = document.getElementById('time-range');
    const picker = document.getElementById('time-picker');
    const zoomOutBtn = document.getElementById('zoom-out-btn');

    rangeBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        const visible = picker.style.display !== 'none';
        picker.style.display = visible ? 'none' : 'block';
        if (!visible) {
            document.getElementById('tp-from').value = nsToDatetimeLocalUTC(timeRange.from);
            document.getElementById('tp-to').value = nsToDatetimeLocalUTC(timeRange.to);
        }
    });

    document.addEventListener('click', (e) => {
        if (!picker.contains(e.target) && e.target !== rangeBtn) picker.style.display = 'none';
    });
    picker.addEventListener('click', (e) => e.stopPropagation());

    picker.querySelectorAll('.tp-quick button').forEach(btn => {
        btn.addEventListener('click', async () => {
            const secs = parseInt(btn.dataset.range);
            picker.style.display = 'none';
            picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            stopAutoRefresh();
            const myId = autoRefreshId;  // capture; only restart if still latest
            if (secs === 0) {
                await zoomTo(server.fromNs, server.toNs);
            } else {
                timeRange.anchorLive(secs);
                updateTimeRange();
                await refresh();
                if (autoRefreshId === myId) startAutoRefresh(secs);
            }
        });
    });

    document.getElementById('tp-apply').addEventListener('click', () => {
        // The inputs are defined (and labeled) as UTC; parse them as UTC so a
        // UTC+3 browser gets exactly the window it typed (UI-11).
        const fromNs = datetimeLocalUTCToNs(document.getElementById('tp-from').value);
        const toNs = datetimeLocalUTCToNs(document.getElementById('tp-to').value);
        if (fromNs == null || toNs == null || fromNs >= toNs) return;
        picker.style.display = 'none';
        picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
        stopAutoRefresh();
        zoomTo(fromNs, toNs);
    });

    zoomOutBtn.addEventListener('click', () => { stopAutoRefresh(); zoomOut(); });
}

// ── Live mode / auto-refresh ──────────────────────────────────────────────────

function startAutoRefresh(rangeSecs) {
    stopAutoRefresh();
    autoRefreshOn = true;
    setLiveButton(true);
    const myId = ++autoRefreshId;
    (async function loop() {
        await new Promise(r => setTimeout(r, 5000));
        while (autoRefreshId === myId) {
            // UI-1: while the transport is degraded, live mode must not
            // pretend to tick — the window stays frozen and nothing repaints
            // as fresh. The recovery probe re-anchors to NOW on success.
            if (!degraded.active) {
                try {
                    const info = await transport.send('info', {});
                    if (autoRefreshId !== myId) break;
                    if (info) {
                        server.update(info);
                        timeRange.anchorLive(rangeSecs);
                        updateTimeRange();
                    }
                    if (autoRefreshId !== myId) break;
                    await refresh();
                } catch (e) {
                    if (autoRefreshId === myId) onRequestError(e);
                }
            }
            if (autoRefreshId !== myId) break;
            await new Promise(r => setTimeout(r, 5000));
        }
    })();
}

function stopAutoRefresh() {
    autoRefreshOn = false;
    autoRefreshId++;   // invalidate any running loop
    setLiveButton(false);
}

function setLiveButton(on) {
    const liveBtn = document.getElementById('live-btn');
    if (!liveBtn) return;
    liveBtn.classList.toggle('active', on);
    liveBtn.textContent = on ? 'Live ●' : 'Live';
}

function initLiveMode() {
    const btn = document.getElementById('live-btn');
    if (!btn) return;
    btn.addEventListener('click', () => {
        if (autoRefreshOn) {
            stopAutoRefresh();
        } else {
            // P4: resume live at the CURRENT span (timeRange.liveRangeSecs is
            // the one source of truth), re-anchored to NOW — never a surprise
            // jump to a different window size.
            timeRange.anchorLive(timeRange.liveRangeSecs);
            updateTimeRange();
            startAutoRefresh(timeRange.liveRangeSecs);
            refresh();
        }
    });
}

// ── Daemon control plane (B5) ─────────────────────────────────────────────────
//
// One poll fetches status (+ metrics if the panel is open) over the control
// proxy. A daemon-not-running reply (static-trace replay) flips `available`
// false: we hide the escalation UI and drop to a slow re-probe cadence — the
// control plane is RE-PROBED periodically (UI-9), so a daemon (re)started
// mid-session restores the escalate UI without a page reload. When a daemon IS
// present, the escalate header control + (optionally) the metrics panel reflect
// live state, and the AAS chart re-renders so its escalation annotation tracks
// the current window.

async function pollDaemon() {
    try {
        const status = await controlStatus(transport);
        // UI-10: the daemon doesn't report when the current escalation window
        // opened, so record the moment a poll OBSERVES the sampled->escalated
        // flip. The AAS annotation shades only [observed start, live edge]; a
        // page loaded mid-escalation has no start and gets the markLine only.
        if (status && status.tier === 'escalated') {
            const prevEscalated = daemon.status && daemon.status.tier === 'escalated';
            if (!prevEscalated && daemon.status) {
                daemon.escalationStartNs = server.nowNs;
            }
            if (daemon.escalationStartNs != null) {
                status.observed_start_ns = daemon.escalationStartNs;
            }
        } else {
            daemon.escalationStartNs = null;
        }
        daemon.status = status;
        daemon.available = true;
        daemon.polled = true;
        if (metricsPanelOpen) {
            try { daemon.metrics = await controlMetrics(transport); }
            catch (e) { /* metrics optional; keep last */ }
        }
        renderEscalateControl();
        renderMetricsPanel();
    } catch (e) {
        daemon.polled = true;
        if (e instanceof ControlUnavailable) {
            // The server answered "no daemon here" — a real fact.
            daemon.available = false;
            daemon.status = null;
            daemon.escalationStartNs = null;
            renderEscalateControl();
            renderMetricsPanel();
        }
        // Transport errors (disconnect / dead bridge pipe): keep the last
        // status — the daemon may be fine; the degraded state covers the UI.
    }
}

function startDaemonPoll() {
    stopDaemonPoll();
    const myId = ++daemonPollId;
    let tick = 0;
    (async function loop() {
        while (daemonPollId === myId) {
            await new Promise(r => setTimeout(r, 5000));
            if (daemonPollId !== myId) break;
            tick++;
            if (degraded.active) continue;   // transport down: nothing to ask
            // No daemon last time we asked: keep re-probing, but slowly
            // (every 30 s), so a daemon started mid-session is picked up
            // without hammering a static-replay server (UI-9).
            if (!daemon.available && tick % 6 !== 0) continue;
            await pollDaemon();
        }
    })();
}

function stopDaemonPoll() { daemonPollId++; }

function renderEscalateControl() {
    const el = document.getElementById('escalate-control');
    const metricsBtn = document.getElementById('metrics-btn');
    if (!el) return;
    const supported = daemon.available && daemon.status &&
                      daemon.status.escalation_supported;
    if (metricsBtn) metricsBtn.style.display = supported ? '' : 'none';
    mountEscalateControl(el, Object.assign(makeCtx(), { viewId: 'control' }));
}

function renderMetricsPanel() {
    const el = document.getElementById('daemon-metrics');
    if (!el) return;
    if (metricsPanelOpen && daemon.available && daemon.metrics) {
        mountMetricsPanel(el, daemon.metrics, daemon.status);
        el.style.display = '';
    } else {
        el.style.display = 'none';
    }
}

function initMetricsButton() {
    const btn = document.getElementById('metrics-btn');
    if (!btn) return;
    btn.addEventListener('click', async () => {
        metricsPanelOpen = !metricsPanelOpen;
        btn.classList.toggle('active', metricsPanelOpen);
        if (metricsPanelOpen) {
            try { daemon.metrics = await controlMetrics(transport); }
            catch (e) { /* keep last */ }
        }
        renderMetricsPanel();
    });
}

// ── Start ─────────────────────────────────────────────────────────────────────

function boot() {
    chartEl = document.getElementById('aas-chart-container');
    tableEl = document.getElementById('table-container');
    summaryEl = document.getElementById('summary-bar');
    tooltipEl = document.getElementById('query-tooltip');

    // Debug/test handle: the UI no longer has a single mutable global `state`,
    // so expose the explicit modules for Playwright assertions (read-only use).
    window.__pgwt = { server, timeRange, filters, transport,
        get activeTab() { return vm ? vm.activeId() : null; },
        // B5: read-only accessors for Playwright assertions on the daemon state.
        daemon,
        daemonTier() { return daemon.status ? daemon.status.tier : null; },
        // T6/UI-1: read-only degraded-transport state for Playwright assertions.
        degraded };

    connect();
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
} else {
    boot();
}
