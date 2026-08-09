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

import {
    ServerInfo, TimeRange, FilterStack, CompareState,
    serializeHashState, parseHashState,
} from './lib/state.js';
import { Transport, TransportError, CancelledError } from './lib/transport.js';
import { Camera } from './lib/camera.js';
import { StripCache } from './lib/stripcache.js';
import { ViewManager } from './lib/view-manager.js';
import { mountTable } from './lib/table.js';
import {
    classColor, fmtTime, fmtDuration, esc, fmtCount,
    nsToDatetimeLocalUTC, datetimeLocalUTCToNs, versionSkew,
} from './lib/format.js';
import { controlStatus, controlMetrics, ControlUnavailable } from './lib/control.js';
import {
    mountEscalateControl, mountMetricsPanel,
    buildComparePaneFidelity, compareFidelityHtml,
} from './lib/panels.js';
import { createActiveView } from './views/active.js';
import { createOverviewView } from './views/overview.js';
import { createEventsView } from './views/events.js';
import { createSessionsView } from './views/sessions.js';
import { createQueriesView } from './views/queries.js';
import { createHistogramView } from './views/histogram.js';
import { createTimelineView } from './views/timeline.js';
import { createTransitionsView } from './views/transitions.js';
import { createConcurrencyView } from './views/concurrency.js';
import { createWaterfallView } from './views/waterfall.js';
import { createExecScatterView } from './views/exec-scatter.js';
import { createMatrixView } from './views/matrix.js';

// ── Core services ─────────────────────────────────────────────────────────────

const server = new ServerInfo();
const timeRange = new TimeRange(server);
const filters = new FilterStack();
const compare = new CompareState();
const transport = new Transport();

let activeView = null;       // persistent AAS chart view ("active")
let vm = null;               // ViewManager for tab views

// ── Camera-lite: the AAS instrument state (Track U, U2a) ─────────────────────
// The camera is the AAS pane's viewport as pure client state (lib/camera.js);
// the strip cache is its tile store (lib/stripcache.js). The camera is
// AUTHORITATIVE BY CONVENTION (INSTRUMENT_ARCHITECTURE §5): ECharts'
// inside-dataZoom gestures are mirrored INTO it (views/active.js), and every
// app-side window change is mirrored into it via syncCamera() (suppressed so
// it never re-enters the gesture pipeline). Gestures detach the camera and
// pause live via the U0 machinery; the Live button re-attaches to NOW.
const camera = new Camera({ fromNs: 0, toNs: 0 });
// Strip fetches go through the transport's NON-SUPERSEDING send() path — a
// cache fill must bypass the single-flight channel cancel and the
// view-manager epoch discard (stale-but-useful retention is the point; §5's
// documented exception to the dashboard transport rules). Same params as the
// poll path so a strip is exactly "the aas view over a wider window".
const stripCache = new StripCache({
    fetchStrip: ({ fromNs, toNs, buckets }) => {
        const params = { from: fromNs, to: toNs, buckets,
                         filters: filters.snapshot() };
        const f = filters.filters;
        if (f.class && !f.event_id) params.detail = 'events';
        return transport.send('aas', params);
    },
});

/* The one FilterStack mutation boundary. Strip keys are intentionally
 * filter-blind, so every successful filter-set mutation invalidates the cache
 * here — no caller can remember the state change and forget its data meaning. */
function mutateFilters(mutate) {
    const changed = mutate();
    if (changed !== false) stripCache.invalidate();
    return changed;
}
let cameraSyncing = false;   // true while app state is being MIRRORED into the camera
let cameraSettleTimer = null;
const CAMERA_SETTLE_MS = 150;   // settle-refine debounce after the last gesture event

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
let lastLiveTickTo = null;
let compareEvidence = null;

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
    updateHash(false);   // P9: sort is bookmarkable state (replace, not push)
}

// ── URL hash state plumbing (Track U Phase U2, review P9) ─────────────────────
//
// The codec is pure (lib/state.js: serializeHashState/parseHashState); this
// is the WHEN. Two write modes:
//   updateHash(true)   PUSH a history entry — investigation steps (drill,
//                      breadcrumb, tab switch, zoom, live toggle, camera
//                      settle), so browser back/forward retraces them;
//   updateHash(false)  REPLACE — passive/minor changes (sort). Live-mode
//                      hashes carry live=1&span, never from/to, so the 5 s
//                      tick does not touch the URL at all.
// popstate applies the target hash; an in-flight apply coalesces to the latest
// traversal instead of dropping it. `applyingHash` suppresses re-entrant
// writes while an apply is mutating state. Deliberately hash-only: no
// router, no path pushState (roadmap: "no SPA framework/router beyond hash
// state").
let applyingHash = false;
let pendingHashApply = null;

function currentHashState() {
    const tab = vm ? vm.activeId() : null;
    const activeView = tab && vm ? vm.views[tab] : null;
    const execution = activeView && typeof activeView.getSelection === 'function'
        ? activeView.getSelection() : null;
    return {
        tab,
        live: autoRefreshOn,
        spanSecs: timeRange.liveRangeSecs,
        fromNs: timeRange.from,
        toNs: timeRange.to,
        filters: filters.snapshot(),
        sort: tab ? getSort(tab) : null,
        execution,
        compare: compare.enabled,
        baselineOffsetNs: compare.offsetNs,
    };
}

function updateHash(push) {
    if (!uiInitialized || applyingHash) return;
    const h = serializeHashState(currentHashState());
    const cur = (location.hash || '').replace(/^#/, '');
    if (h === cur) return;
    if (push) {
        // Fragment navigation: pushes a history entry (back retraces it).
        location.hash = h;
    } else {
        history.replaceState(null, '', '#' + h);
    }
}

/* Apply a parsed hash state: filters + tab + window/live + sort, then one
 * refresh. Used by the !uiInitialized hydration and by popstate (back/
 * forward). A restored live=1 RE-ANCHORS to a freshly fetched NOW — the
 * live-means-NOW rule; a live deep link can never resurrect stale time. */
async function applyHashOnce(s) {
    stopAutoRefresh();               // restarted below iff s.live
    mutateFilters(() => {
        filters.restore(s.filters);
        compare.set(s.compare, s.baselineOffsetNs);
        return true;
    });
    compareEvidence = null;
    renderCompareControl();
    const tab = (s.tab && vm.views[s.tab]) ? s.tab : vm.activeId();
    if (s.sort) tabSort[tab] = s.sort;
    else delete tabSort[tab];        // Back to a pre-sort entry undoes the sort
    setTabView(tab);
    if (s.execution && vm.views[tab] &&
        typeof vm.views[tab].selectExecution === 'function') {
        vm.views[tab].selectExecution(s.execution);
    }
    updateBreadcrumb();
    if (s.live) {
        // Fresh server clock first — same rule as the Live button (the
        // stored nowNs may be minutes stale after a paused session).
        try {
            const info = await transport.send('info');
            server.update(info);
        } catch (e) { /* anchor to last known NOW */ }
        timeRange.anchorLive(s.spanSecs || timeRange.liveRangeSecs);
        // Respect pausesLive views (transitions/concurrency): the window
        // still means NOW, but the 5 s loop stays off — exactly what
        // switching to that tab live would have done.
        if (!(vm.views[tab] && vm.views[tab].pausesLive)) {
            startAutoRefresh(timeRange.liveRangeSecs);
        }
    } else if (s.fromNs != null && s.toNs != null) {
        timeRange.set(s.fromNs, s.toNs);
    }
    updateTimeRange();
    await refresh();
    await resolveRestoredFilterLabels();
}

/* reason='hydrate' is the one normalization site for a hand-typed initial
 * hash. Traversal applies never replaceState the entry the browser selected.
 * If Back/Forward moves again while network work is in flight, only the latest
 * target matters; loop until no newer target is pending. */
async function applyHash(s, reason) {
    if (!s) return;
    const request = { state: s, reason: reason || 'traversal' };
    if (applyingHash) { pendingHashApply = request; return; }
    applyingHash = true;
    let current = request;
    let normalize = false;
    try {
        while (current) {
            pendingHashApply = null;
            await applyHashOnce(current.state);
            normalize = current.reason === 'hydrate';
            current = pendingHashApply;
        }
    } finally {
        applyingHash = false;
    }
    if (normalize) updateHash(false);
}

function initHashNavigation() {
    // Fragment traversal fires popstate (and our own pushes echo through it):
    // a hash that already matches the current state is OUR echo — skip it.
    window.addEventListener('popstate', () => {
        const raw = (location.hash || '').replace(/^#/, '');
        if (!applyingHash && raw === serializeHashState(currentHashState())) return;
        const s = parseHashState(location.hash);
        if (s) applyHash(s, 'traversal');
    });
}

// ── DOM handles (resolved once at bootstrap) ──────────────────────────────────

let chartEl, tableEl, summaryEl, tooltipEl;

// ── Shared ctx handed to every view hook ──────────────────────────────────────

function makeCtx() {
    return {
        transport, server, timeRange, filters, compare,
        // U2a: the AAS camera + strip cache (views/active.js wires gestures
        // into the camera and preloads/paints strips through the cache).
        camera, stripCache,
        echarts: window.echarts,
        // U2b: the vendored uPlot constructor for the AAS pane's default
        // renderer (views/active.js; ?renderer=echarts keeps the old path).
        uplot: window.uPlot,
        chartEl, summaryEl, tooltipEl,
        mountTable,
        setStatus,
        setCompareEvidence,
        // The ONE navigation intent surface (U2, review P3). Accepts both
        // intent shapes — filter drills and pivot intents — see the contract
        // comment above PIVOT/PIVOTS in the drill section below.
        onDrill: drill,
        onZoom: (from, to) => { stopAutoRefresh(); zoomTo(from, to); },
        onZoomOut: () => { stopAutoRefresh(); zoomOut(); },
        isLiveTick: () => autoRefreshOn && lastLiveTickTo === timeRange.to,
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

            // P9: deep-link hydration — a hash restores {tab, window|live,
            // filters, sort} instead of the boot defaults. A restored live=1
            // re-anchors to NOW inside applyHash (never to the link's time).
            const hs = parseHashState(location.hash);
            if (hs) {
                await applyHash(hs, 'hydrate');
                return;
            }

            await refresh();
            // Start in live mode. The span comes from timeRange.liveRangeSecs
            // (the ONE source of truth for the live span; initDefault set the
            // 900s = 15 min boot default) — the Live button resumes it (P4).
            startAutoRefresh(timeRange.liveRangeSecs);
            updateHash(false);   // P9: the URL always names the current state
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
    initCompareControl();
    initLiveMode();
    initMetricsButton();
    initHashNavigation();   // P9: back/forward retrace the investigation
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

async function refresh(aasMountReason) {
    await refreshActive(aasMountReason);
    await vm.refresh();
}

async function refreshActive(aasMountReason) {
    if (!activeView) return;
    const ctx = activeCtx(aasMountReason || 'deliberate');
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
function activeCtx(aasMountReason) {
    return Object.assign(makeCtx(), {
        viewId: 'active',
        // AAS y-axis hysteresis smooths live ticks only. Deliberate refreshes
        // reset it; provisional strip-cache paints neither seed nor advance it.
        aasMountReason: aasMountReason || 'deliberate',
        channel: (name) => 'active.' + name,
    });
}

/* URL restore serializes filter values, not presentation labels. Once the
 * restored window has fetched successfully, resolve the event chip from the
 * authoritative event list and repaint the projections. Until this finishes,
 * renderFilterBar's honest key=value fallback remains visible. */
async function resolveRestoredFilterLabels() {
    const restored = filters.snapshot();
    const labels = {};
    if (restored.class != null) labels.class = String(restored.class);
    if (restored.pid != null) labels.pid = 'PID ' + restored.pid;
    if (restored.query_id != null) labels.query_id = 'Query ' + restored.query_id;
    if (restored.event_id != null) {
        try {
            const data = await transport.send('top_events', {
                from: timeRange.from, to: timeRange.to, filters: {},
            });
            const row = ((data && data.rows) || []).find(r =>
                r && r.event_id === restored.event_id);
            if (row && row.name) labels.event_id = row.name;
        } catch (e) { /* key=value remains honest until a later real drill */ }
    }
    // A coalesced traversal may have advanced while the label fetch was in
    // flight. Never attach names to a different filter set.
    if (JSON.stringify(filters.snapshot()) !== JSON.stringify(restored)) return;
    if (!Object.keys(labels).length) return;
    filters.labels = Object.assign({}, filters.labels, labels);
    updateBreadcrumb();
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
    vm.register(createWaterfallView());
    vm.register(createExecScatterView());
    vm.register(createMatrixView());

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
    updateHash(true);   // P9: tab choice is shareable state ("tabs are lenses")
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

// ── Drill-down / breadcrumb / pivot intents ───────────────────────────────────
//
// U2 (review P3): ONE intent surface, `ctx.onDrill(intent)`, two intent
// shapes — both flow through FilterStack + the registries below; no view
// carries its own navigation logic or a second filter state.
//
//   FILTER DRILL (the original shape, tables + the AAS chart click):
//     { filterKey: 'class'|'event_id'|'pid'|'query_id',
//       filterValue,             // class: label string ('IO'); event_id/pid:
//                                //   number; query_id: string (uint64)
//       label }                  // human text for chip/breadcrumb
//     → pushes the filter (FilterStack.drill) and pivots to
//       PIVOT[filterKey]'s tab.
//
//   PIVOT INTENT (U2 — the cross-view wires; consumed by the app, EMITTED by
//   views; the emitting views are other slices, the registry is normative):
//     { pivot: <key>,            // one of PIVOTS below
//       filterKey?, filterValue?, label?,   // optional filter drill to apply
//                                           //   (same field contract as above)
//       removeKey?,              // key or keys to remove through the same
//                                //   app-owned FilterStack mutation boundary
//       from?, to?,              // optional ns window → zoomTo(from, to)
//       tab? }                   // optional destination override
//     Semantics: pause live (P4), apply the filter if given (recorded as a
//     normal FilterStack drill — breadcrumbs/chips/URL all follow), switch
//     to intent.tab || PIVOTS[key].tab (null keeps the current tab), then
//     zoomTo(from,to) if a window was given (zoom history records it, so
//     dblclick backs out), else plain refresh. Unknown pivot keys are loud
//     no-ops (console.error) — a typo must not silently eat a click.
//
//   Registry (destination defaults):
//     'histogram-event' events P50/P95/P99 cell → THAT event's latency
//                       distribution: filter {event_id} + histogram tab
//                       (review wire 6).
//     'heatmap-cell'    heatmap cell → event+time isolate: filter
//                       {event_id} + zoomTo(bucket window) + queries tab —
//                       "which queries drove this event, then" (wire 4).
//     'dfg-event'       DFG node → standard event drill: filter {event_id}
//                       + queries tab, identical destination to an events-
//                       table row click (wire 5; server emits event_id in
//                       node JSON).
//     'burst-zoom'      burst/peak row → pure time jump: zoomTo(burst ± pad;
//                       the EMITTER pads), no filter, current tab kept
//                       (wire 2).

const PIVOT = { class: 'events', event_id: 'queries', pid: 'timeline', query_id: 'events' };

const PIVOTS = {
    'histogram-event': { tab: 'histogram' },
    'heatmap-cell':    { tab: 'queries' },
    'dfg-event':       { tab: 'queries' },
    'burst-zoom':      { tab: null },
    'query-executions': { tab: 'waterfall' },
    'scatter-execution': { tab: 'waterfall', intentMethod: 'selectExecution' },
    'waterfall-execution': { tab: 'waterfall', intentMethod: 'selectExecution' },
    'matrix-cell':     { tab: 'queries' },
};

function drill(intent) {
    if (!intent) return;
    if (intent.pivot) { pivotDrill(intent); return; }
    stopAutoRefresh();   // P4: drilling is an investigation gesture, like zoom
    mutateFilters(() => filters.drill(intent.filterKey, intent.filterValue,
        intent.label, vm.activeId()));
    const pivot = PIVOT[intent.filterKey];
    if (pivot) setTabView(pivot);
    updateBreadcrumb();
    updateHash(true);    // P9: a drill is an investigation step — back undoes it
    refresh();
}

function pivotDrill(intent) {
    const reg = PIVOTS[intent.pivot];
    if (!reg) {
        console.error('[pgwt] unknown pivot intent:', intent.pivot, intent);
        return;
    }
    stopAutoRefresh();   // P4: every pivot is an investigation gesture
    const removeKeys = intent.removeKey == null ? [] :
        (Array.isArray(intent.removeKey) ? intent.removeKey : [intent.removeKey]);
    if (removeKeys.length || intent.filterKey) {
        mutateFilters(() => {
            let changed = false;
            for (const key of removeKeys) {
                changed = filters.removeFilter(key, vm.activeId()) || changed;
            }
            if (intent.filterKey) {
                filters.drill(intent.filterKey, intent.filterValue, intent.label,
                    vm.activeId());
                changed = true;
            }
            return changed;
        });
    }
    const tab = intent.tab !== undefined ? intent.tab : reg.tab;
    if (tab && vm.views[tab]) setTabView(tab);
    if (tab && reg.intentMethod && vm.views[tab] &&
        typeof vm.views[tab][reg.intentMethod] === 'function') {
        vm.views[tab][reg.intentMethod](intent);
    }
    updateBreadcrumb();
    if (intent.from != null && intent.to != null && intent.to > intent.from) {
        // zoomTo owns the ONE history push for this compound investigation
        // step; no never-rendered intermediate pivot entry.
        zoomTo(intent.from, intent.to);   // records zoom history + refreshes
    } else {
        updateHash(true);
        refresh();
    }
}

function drillUp(index) {
    stopAutoRefresh();   // P4: breadcrumb navigation pauses live too
    const view = mutateFilters(() => filters.drillUp(index));
    if (view) setTabView(view);
    updateBreadcrumb();
    updateHash(true);
    refresh();
}

/* U2 (wire 7): a filter-bar chip's ✕ — drop ONE dimension, keep the rest.
 * FilterStack records it as a new investigation step (append-only history),
 * so the breadcrumb/back-button can restore the removed filter. */
function removeFilter(key) {
    stopAutoRefresh();   // P4: reshaping the filter set is an investigation
    if (!mutateFilters(() => filters.removeFilter(key, vm.activeId()))) return;
    updateBreadcrumb();
    updateHash(true);
    refresh();
}

function clearFilters() {
    stopAutoRefresh();   // P4: ✕ rewinds the investigation — Live is the way back
    mutateFilters(() => { filters.clear(); });
    setTabView('overview');
    updateBreadcrumb();
    updateHash(true);
    refresh();
}

/* One render pass for every FilterStack-derived surface: the breadcrumb
 * trail, the persistent filter-bar chips, and the tab badges. They are three
 * PROJECTIONS of the same state — updating them together is what keeps
 * "tabs are lenses" true on screen (wire 7). */
function updateBreadcrumb() {
    renderBreadcrumbTrail();
    renderFilterBar();
    renderTabBadges();
}

function renderBreadcrumbTrail() {
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

/* U2 (wire 7): the persistent compact filter bar — one chip per ACTIVE
 * FilterStack entry (filters.filters), each with its own ✕. Pure projection:
 * chips render FROM the stack (labels via FilterStack.labels, falling back
 * to key=value after a URL restore) — there is no chip-side state. Lives in
 * the tab strip so the "every tab is a lens over these filters" relationship
 * is spatially explicit. */
function renderFilterBar() {
    const bar = document.getElementById('filter-bar');
    if (!bar) return;
    const entries = Object.entries(filters.filters);
    if (!entries.length) { bar.innerHTML = ''; return; }
    bar.innerHTML = entries.map(([k, v]) => {
        const label = filters.labels[k] || (k + '=' + v);
        return '<span class="fchip" title="' + esc(k + ' = ' + v) + '">' +
            dotHtml(label) + esc(label) +
            '<span class="fchip-x" data-key="' + esc(k) +
            '" title="Remove this filter">✕</span></span>';
    }).join('');
    bar.querySelectorAll('.fchip-x').forEach(x => {
        x.addEventListener('click', (e) => {
            e.stopPropagation();
            removeFilter(x.dataset.key);
        });
    });
}

/* U2 (wire 7): a dot on EVERY tab button while any filter is active — all
 * tabs are lenses over the same FilterStack, so all of them are "filtered". */
function renderTabBadges() {
    const active = Object.keys(filters.filters).length > 0;
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('filtered', active);
    });
}

function dotHtml(name) {
    const color = classColor(name);
    if (!color) return '';
    return '<span class="class-dot" style="background:' + color + '"></span>';
}

// ── Camera-lite orchestration (Track U, U2a) ──────────────────────────────────
//
// Two mirror directions, one suppression flag:
//   app → camera   syncCamera(), called from updateTimeRange() (the chokepoint
//                  every timeRange mutation already flows through) and from
//                  start/stopAutoRefresh. Runs under cameraSyncing so the
//                  camera's onChange never re-enters the gesture pipeline.
//   camera → app   onCameraChange(): a REAL gesture (zoom/pan from the chart's
//                  inside-dataZoom, mirrored in views/active.js) pauses live
//                  exactly like every other investigation gesture (U0
//                  machinery), mirrors the window into timeRange, and
//                  debounces the settle-refine (~150 ms after the last event).
//
// Settle = re-render the AAS pane from the strip cache under a freshly
// quantized axis (re-centers the 3x gesture skirt), kick the exact-strip
// fetch (deduped, non-superseding send), and bring the tab views to the new
// window. The AAS pane itself NEVER re-enters the poll path here — the server
// stays out of the gesture loop (INSTRUMENT_ARCHITECTURE §4a/§5).

/* Mirror the app's timeRange + live state into the camera. Follow mode's
 * right edge is timeRange.to — which anchorLive() just set to the server's
 * NOW, so "live means NOW" holds structurally (camera.attachFollow /
 * followTick re-anchor to exactly that edge). */
function syncCamera() {
    cameraSyncing = true;
    try {
        const sameWin = camera.fromNs === timeRange.from &&
                        camera.toNs === timeRange.to;
        if (!sameWin) {
            const isTick = autoRefreshOn && camera.mode === 'follow' &&
                timeRange.to > camera.toNs &&
                (timeRange.to - timeRange.from) === camera.span();
            // followTick for the pure live slide; setWindow for everything
            // else (span changes, jumps, restores). setWindow detaches, so
            // re-attach below whenever live mode is on.
            if (!isTick || !camera.followTick(timeRange.to)) {
                camera.setWindow(timeRange.from, timeRange.to);
            }
        }
        if (autoRefreshOn) {
            if (camera.mode !== 'follow') camera.attachFollow(timeRange.to);
        } else if (camera.mode !== 'detached') {
            camera.detach();
        }
    } finally {
        cameraSyncing = false;
    }
}

/* Camera events that were NOT app-initiated are chart gestures: pause live,
 * mirror into timeRange, settle-refine. One event per gesture step (the
 * camera fires zoom/pan/set with the detach folded in). */
function onCameraChange(ev) {
    if (cameraSyncing || !uiInitialized) return;
    if (ev.cause !== 'zoom' && ev.cause !== 'pan' && ev.cause !== 'set') return;
    // Mirror BEFORE stopAutoRefresh: its syncCamera must see the new window
    // (and then no-op) — never clobber the gesture with the old timeRange.
    timeRange.set(camera.fromNs, camera.toNs);
    stopAutoRefresh();   // P4/U0: a camera gesture is an investigation
    updateTimeRange();
    scheduleCameraSettle();
}

function scheduleCameraSettle() {
    if (cameraSettleTimer) clearTimeout(cameraSettleTimer);
    cameraSettleTimer = setTimeout(() => {
        cameraSettleTimer = null;
        settleCamera();
    }, CAMERA_SETTLE_MS);
}

function settleCamera() {
    // P9: the gesture came to rest — this window is an investigation step
    // browser-back should undo (in-gesture camera events never touch the URL;
    // only the settled window does).
    updateHash(true);
    // Instant repaint from whatever the cache holds (stale/coarse ok) under
    // the re-quantized strip axis; renderFromCamera also ensure()s the exact
    // strip, which swaps in via onStripData when it lands.
    if (activeView && activeView.renderFromCamera) {
        activeView.renderFromCamera(activeCtx('provisional'));
    }
    // The tab views are dashboard panes: they follow the window like any
    // other window change (single-flight channels supersede as usual).
    if (vm) vm.refresh();
}

/* Strip arrival: repaint the AAS pane from the cache while detached (the
 * contract says re-run get() on ANY arrival — an older overlapping strip can
 * still refine an empty view). In follow mode the 5 s poll-and-replace owns
 * the paint; arrivals only warm the cache for the first gesture. */
function onStripData() {
    if (!uiInitialized || camera.mode !== 'detached') return;
    if (activeView && activeView.renderFromCamera) {
        activeView.renderFromCamera(activeCtx('provisional'));
    }
}

camera.onChange(onCameraChange);
stripCache.onData(onStripData);

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
    // U2a: every timeRange mutation flows through here — the one place the
    // camera mirror needs to hook (suppressed, so no gesture pipeline).
    syncCamera();
    const el = document.getElementById('time-range');
    const from = fmtTime(timeRange.from);
    const to = fmtTime(timeRange.to);
    const dur = fmtDuration(timeRange.span());
    // All pgwt times are UTC — say so (UI-11).
    el.textContent = from + ' – ' + to + ' UTC (' + dur + ')';
    renderCompareHeader();
}

async function zoomTo(from, to) {
    timeRange.zoomTo(from, to);
    updateTimeRange();
    updateHash(true);   // P9: a zoom is an investigation step — back undoes it
    // U2a instant feedback: paint whatever the strip cache holds for the new
    // window RIGHT NOW (stale/coarse ok — Maps-style), before the poll below
    // swaps in the exact window data. No-op on a cache miss.
    if (activeView && activeView.renderFromCamera) {
        activeView.renderFromCamera(activeCtx('provisional'));
    }
    await refresh();
}

async function zoomOut() {
    timeRange.zoomOut();
    updateTimeRange();
    updateHash(true);   // P9: same rule as zoomTo
    // Same instant stale-paint as zoomTo (dblclick zoom-out, header button).
    if (activeView && activeView.renderFromCamera) {
        activeView.renderFromCamera(activeCtx('provisional'));
    }
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
                // U2a review F3: same freshness rule as the Live button —
                // server.nowNs can be minutes stale after a paused
                // investigation, and attachFollow is only as fresh as its
                // caller. Fetch info first; fall back to last-known NOW.
                try {
                    const info = await transport.send('info');
                    server.update(info);
                } catch (e) { /* anchor to last known NOW */ }
                timeRange.anchorLive(secs);
                updateTimeRange();
                await refresh();
                if (autoRefreshId === myId) {
                    startAutoRefresh(secs);
                    updateHash(true);   // P9: live span choice is state
                }
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

// ── Compare mode (Track U Phase U4, D1/D4/D5) ───────────────────────────────

function compareOffsetLabel(offsetNs) {
    const seconds = Math.abs(offsetNs) / 1e9;
    if (seconds % 86400 === 0) return (seconds / 86400) + ' d';
    if (seconds % 3600 === 0) return (seconds / 3600) + ' h';
    if (seconds % 60 === 0) return (seconds / 60) + ' min';
    return seconds + ' s';
}

function baselinePredatesCurrent() {
    return compare.enabled && timeRange.from + compare.offsetNs < server.fromNs;
}

function renderCompareControl() {
    const btn = document.getElementById('compare-btn');
    const exitBtn = document.getElementById('compare-exit');
    if (!btn || !exitBtn) return;
    btn.classList.toggle('active', compare.enabled);
    btn.textContent = compare.enabled
        ? 'vs ' + compareOffsetLabel(compare.offsetNs) + ' ago ▾'
        : 'Compare ▾';
    exitBtn.style.display = compare.enabled ? '' : 'none';
    renderCompareHeader();
}

function renderCompareHeader() {
    const el = document.getElementById('compare-header');
    if (!el) return;
    if (!compare.enabled) {
        el.style.display = 'none';
        el.innerHTML = '';
        return;
    }
    el.style.display = 'flex';
    let evidence = compareEvidence;
    if (baselinePredatesCurrent()) {
        evidence = buildComparePaneFidelity(null, null, { baselinePredates: true });
    }
    el.innerHTML = '<span class="compare-offset">A vs ' +
        esc(compareOffsetLabel(compare.offsetNs)) + ' ago</span>' +
        (evidence ? compareFidelityHtml(evidence) :
            '<span class="compare-note">loading baseline evidence…</span>');
}

function setCompareEvidence(dataA, dataB, predates) {
    if (!compare.enabled) {
        compareEvidence = null;
        renderCompareHeader();
        return;
    }
    compareEvidence = buildComparePaneFidelity(dataA, dataB,
        { baselinePredates: !!predates });
    renderCompareHeader();
}

function setCompareOffset(offsetNs) {
    const wasEnabled = compare.enabled;
    const changed = mutateFilters(() => compare.set(true, offsetNs));
    if (!changed) return;
    if (!wasEnabled) {
        // Entering compare starts each eligible table at the binding |Δ|
        // default, regardless of its previous ordinary-table sort.
        delete tabSort.overview;
        delete tabSort.events;
        delete tabSort.queries;
    }
    compareEvidence = null;
    renderCompareControl();
    updateHash(true);
    refresh();
}

function exitCompare() {
    if (!mutateFilters(() => compare.disable())) return;
    for (const tab of ['overview', 'events', 'queries']) {
        const s = tabSort[tab];
        if (s && (s.key === 'abs_delta_ms' || s.key === 'ratio' ||
                  s.key === 'a_ms' || s.key === 'b_ms')) delete tabSort[tab];
    }
    compareEvidence = null;
    renderCompareControl();
    updateHash(true);                 // one ✕ = one history entry
    refresh();
}

function initCompareControl() {
    const btn = document.getElementById('compare-btn');
    const menu = document.getElementById('compare-picker');
    const exitBtn = document.getElementById('compare-exit');
    if (!btn || !menu || !exitBtn) return;
    btn.addEventListener('click', (e) => {
        e.stopPropagation();
        menu.style.display = menu.style.display === 'none' ? 'block' : 'none';
    });
    menu.addEventListener('click', e => e.stopPropagation());
    document.addEventListener('click', e => {
        if (!menu.contains(e.target) && e.target !== btn) menu.style.display = 'none';
    });
    menu.querySelectorAll('[data-b-offset]').forEach(preset => {
        preset.addEventListener('click', () => {
            menu.style.display = 'none';
            setCompareOffset(Number(preset.dataset.bOffset));
        });
    });
    document.getElementById('compare-custom-apply').addEventListener('click', () => {
        const amount = Number(document.getElementById('compare-custom-value').value);
        const unit = Number(document.getElementById('compare-custom-unit').value);
        const ns = amount * unit * 1e9;
        if (!(amount > 0) || !Number.isSafeInteger(ns)) return;
        menu.style.display = 'none';
        setCompareOffset(-ns);
    });
    exitBtn.addEventListener('click', exitCompare);
    renderCompareControl();
}

// ── Live mode / auto-refresh ──────────────────────────────────────────────────

function startAutoRefresh(rangeSecs) {
    stopAutoRefresh();
    autoRefreshOn = true;
    lastLiveTickTo = null;   // attach/resume is deliberate; only the loop marks ticks
    setLiveButton(true);
    syncCamera();   // U2a: live on => camera follows (right edge = NOW)
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
                        lastLiveTickTo = timeRange.to;
                        updateTimeRange();
                    }
                    if (autoRefreshId !== myId) break;
                    await refresh('live-tick');
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
    lastLiveTickTo = null;
    autoRefreshId++;   // invalidate any running loop
    setLiveButton(false);
    // U2a: live off => the camera detaches (a later tick can never re-anchor
    // the view; camera.followTick is a strict no-op while detached).
    if (uiInitialized) syncCamera();
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
    btn.addEventListener('click', async () => {
        if (autoRefreshOn) {
            stopAutoRefresh();
            updateHash(true);   // P9: live→paused freezes the window into the URL
            return;
        }
        // P4: resume live at the CURRENT span (timeRange.liveRangeSecs is
        // the one source of truth), re-anchored to NOW — never a surprise
        // jump to a different window size.
        // U2a: "live means NOW" — fetch a FRESH server clock before the
        // re-anchor. server.nowNs goes stale while live is paused (ticks
        // stop), so anchoring to it after a long investigation would show a
        // minutes-old right edge as "live". On failure fall back to the last
        // known clock; the degraded machinery owns that visibility.
        try {
            const info = await transport.send('info');
            server.update(info);
        } catch (e) { /* anchor to last known NOW */ }
        timeRange.anchorLive(timeRange.liveRangeSecs);
        // startAutoRefresh BEFORE updateTimeRange so syncCamera sees live
        // mode and re-attaches the camera (attachFollow to the fresh NOW).
        startAutoRefresh(timeRange.liveRangeSecs);
        updateTimeRange();
        updateHash(true);   // P9: back from here returns to the paused window
        refresh();
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
    window.__pgwt = { server, timeRange, filters, compare, transport,
        // U2a: read-only camera + strip-cache handles for Playwright
        // assertions and the gate agent's gesture-to-paint instrumentation.
        camera, stripCache,
        // U2b: which AAS renderer is active + the view's read-only debug
        // snapshot (painted series/viewport/honesty-overlay geometry) so
        // tests assert renderer state without reaching into canvas pixels.
        get aasRenderer() { return activeView ? activeView.renderer : null; },
        aasDebug: () => (activeView && activeView.debug)
            ? activeView.debug() : null,
        compareSnapshot: () => ({
            enabled: compare.enabled,
            offsetNs: compare.offsetNs,
            a: { from: timeRange.from, to: timeRange.to },
            b: { from: timeRange.from + compare.offsetNs,
                 to: timeRange.to + compare.offsetNs },
            baselinePredates: baselinePredatesCurrent(),
        }),
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
