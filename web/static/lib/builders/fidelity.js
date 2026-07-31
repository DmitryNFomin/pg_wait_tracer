/* pgwt — pure builders for the fidelity-aware UI (Phase B5).
 *
 * Track A taught the daemon to run 24/7 in a cheap SAMPLED tier and escalate to
 * full-fidelity (EXACT) watchpoint capture for bounded windows. The server now
 * tags every view response with a window `fidelity` ("exact" | "sampled" |
 * "mixed") plus `sample_period_ns`, and returns an explicit
 * `{ "unavailable": "requires full-fidelity data", "fidelity": ... }` for the
 * EXACT-only views (histogram, transitions, concurrency, …) over a sampled
 * window. The control socket (proxied by pgwt-server as the `control` command)
 * exposes escalate/deescalate plus status/metrics.
 *
 * These builders turn those server facts into PLAIN render models — markArea
 * specs, panel models, annotation models — with no DOM and no ECharts. The
 * thin mount layers (the active view, the metrics/escalate panels, the
 * unavailable panel) consume them. Everything here is Node-unit-tested.
 */

/* Subtle shading colors, kept here so a snapshot test pins them and a color
 * regression is caught without a browser. */
export const SAMPLED_BAND_COLOR = 'rgba(255, 193, 7, 0.10)';   // amber wash
export const MIXED_BAND_COLOR   = 'rgba(120, 144, 255, 0.10)'; // indigo wash
export const SAMPLED_BORDER     = 'rgba(255, 193, 7, 0.45)';
export const MIXED_BORDER       = 'rgba(120, 144, 255, 0.45)';

/* Escalation annotation colors: manual vs anomaly are visually distinct. */
export const ESC_MANUAL_COLOR  = 'rgba(79, 195, 247, 0.14)';   // cyan
export const ESC_ANOMALY_COLOR = 'rgba(229, 57, 53, 0.16)';    // red
export const ESC_MANUAL_BORDER  = 'rgba(79, 195, 247, 0.7)';
export const ESC_ANOMALY_BORDER = 'rgba(229, 57, 53, 0.8)';

/* The exact string the server sends for EXACT-only views over sampled windows
 * (mirrors PGWT_UNAVAILABLE_MSG in src/compute.h). */
export const UNAVAILABLE_MSG = 'requires full-fidelity data';

/* True when a view response is the structured "needs full-fidelity data"
 * marker rather than real data. */
export function isUnavailable(data) {
    return !!(data && typeof data.unavailable === 'string');
}

/* Normalize a response's fidelity token to one of exact|sampled|mixed|none.
 * Defaults to "exact" when absent so legacy/transition-only responses (and the
 * protocol-drift fixture) keep their current, unshaded look. */
export function fidelityOf(data) {
    const f = data && data.fidelity;
    if (f === 'sampled' || f === 'mixed' || f === 'none' || f === 'exact') return f;
    return 'exact';
}

/* Human label for a fidelity token (used in the AAS legend / status). */
export function fidelityLabel(fid) {
    switch (fid) {
        case 'sampled': return 'Sampled (estimated)';
        case 'mixed':   return 'Mixed (sampled + exact)';
        case 'none':    return 'No data';
        default:        return 'Exact';
    }
}

/* ── Fidelity shading model ──────────────────────────────────────────────────
 *
 * buildFidelityShading(data, win) → {
 *   fidelity,            // the window's fidelity token
 *   bands: [{ from, to, kind }],   // time ranges to shade (ns), kind=sampled|mixed
 *   markArea,            // ECharts markArea.data spec (array of [start,end] pairs)
 *                        //   with itemStyle — fed straight into a series markArea
 *   showLegend,          // whether the sampled/exact legend chip is relevant
 * }
 *
 * The server gives a single fidelity per window. For a "mixed" window it can
 * also (optionally) provide `fidelity_ranges`: an array of
 * { from, to, fidelity } sub-ranges so the band covers only the sampled parts.
 * When that detail is absent we band the whole window with the "mixed" style —
 * "show the band only over the sampled sub-ranges IF AVAILABLE, else mark the
 * window mixed" (REWORK_PLAN B5).
 */
export function buildFidelityShading(data, win) {
    win = win || {};
    const fidelity = fidelityOf(data);
    const xMin = win.from != null ? win.from : null;
    const xMax = win.to != null ? win.to : null;

    const bands = [];

    if (fidelity === 'sampled') {
        bands.push({ from: xMin, to: xMax, kind: 'sampled' });
    } else if (fidelity === 'mixed') {
        const ranges = Array.isArray(data && data.fidelity_ranges)
            ? data.fidelity_ranges : null;
        const sampledSub = ranges
            ? ranges.filter(r => r && r.fidelity === 'sampled')
            : null;
        if (sampledSub && sampledSub.length) {
            for (const r of sampledSub) {
                bands.push({ from: r.from, to: r.to, kind: 'sampled' });
            }
        } else {
            // No sub-range detail: mark the whole window mixed.
            bands.push({ from: xMin, to: xMax, kind: 'mixed' });
        }
    }
    // exact / none → no shading.

    const markArea = bandsToMarkArea(bands);

    return {
        fidelity,
        bands,
        markArea,
        showLegend: fidelity === 'sampled' || fidelity === 'mixed',
    };
}

/* Turn shading bands into an ECharts markArea.data array. Each entry is a
 * [startPoint, endPoint] pair where the points carry xAxis coords + per-band
 * itemStyle. A null from/to means "extend to the axis edge" (ECharts treats a
 * missing xAxis on a markArea endpoint as the plot edge). */
export function bandsToMarkArea(bands) {
    if (!bands || !bands.length) return null;
    const data = [];
    for (const b of bands) {
        const fill   = b.kind === 'mixed' ? MIXED_BAND_COLOR : SAMPLED_BAND_COLOR;
        const border = b.kind === 'mixed' ? MIXED_BORDER : SAMPLED_BORDER;
        const start = { itemStyle: { color: fill,
                                     borderColor: border, borderWidth: 1,
                                     borderType: 'dashed' } };
        const end = {};
        if (b.from != null) start.xAxis = b.from;
        if (b.to != null) end.xAxis = b.to;
        data.push([start, end]);
    }
    return { silent: true, data };
}

/* ── Escalation annotation model ─────────────────────────────────────────────
 *
 * buildEscalationAnnotation(status, win) → null | {
 *   reason,          // "manual" | "anomaly" | other
 *   from, to,        // band bounds in ns (clamped to the view window), or null
 *   markArea,        // ECharts markArea spec for the OBSERVED escalation span
 *                    //   (null when the span start is unknown — never fabricate)
 *   markLine,        // a labeled vertical line at the live edge
 *   label,           // human label ("Escalated (manual)")
 * }
 *
 * `win` may carry `axisMax` — the last on-axis x value (the last bucket START,
 * which is < win.to). The mark coordinates are clamped to it: a markLine placed
 * past the axis max is dropped entirely by ECharts, so the escalation edge
 * NEVER rendered (review P2, fixed in U0). Converting the chart to a value
 * time axis so marks can sit at true timestamps is Phase U2.
 *
 * The control-socket `status` reports the CURRENTLY-open escalation window:
 * tier="escalated", escalation_seconds_remaining, and escalation_reason. The
 * daemon does not report when the window OPENED, so the client stamps
 * `observed_start_ns` onto the status the first time a poll sees the tier flip
 * to "escalated" (app.js pollDaemon). The band then covers exactly the span we
 * know is escalated: [max(win.from, observed_start_ns), win.to] — never the
 * whole view window (UI-10: shading 15 minutes for a 5-second escalation
 * implied full-fidelity capture where there is none). If the page loaded
 * mid-escalation the start is unknown; then we draw NO band — only the labeled
 * markLine at the live edge — rather than invent a duration the server didn't
 * give.
 */
export function buildEscalationAnnotation(status, win) {
    if (!status || status.tier !== 'escalated') return null;
    win = win || {};
    const reason = status.escalation_reason || 'manual';
    const isAnomaly = reason === 'anomaly';

    const fill   = isAnomaly ? ESC_ANOMALY_COLOR : ESC_MANUAL_COLOR;
    const border = isAnomaly ? ESC_ANOMALY_BORDER : ESC_MANUAL_BORDER;
    const label  = 'Escalated (' + reason + ')';

    const remainingS = +status.escalation_seconds_remaining || 0;
    const startNs = (typeof status.observed_start_ns === 'number')
        ? status.observed_start_ns : null;
    // Live edge, clamped onto the axis (win.axisMax = last on-axis x value):
    // an off-axis markLine is dropped by ECharts, not clipped.
    let to = win.to != null ? win.to : null;
    if (to != null && win.axisMax != null && to > win.axisMax) to = win.axisMax;
    // Band start: the observed escalation start, clamped into the window.
    let from = null;
    if (startNs != null) {
        from = (win.from != null) ? Math.max(win.from, startNs) : startNs;
        if (to != null && from > to) from = to;
    }

    const markArea = (from != null && to != null) ? {
        silent: true,
        data: [[
            { xAxis: from },
            { xAxis: to,
              itemStyle: { color: fill, borderColor: border,
                           borderWidth: 1, borderType: 'solid' } },
        ]],
    } : null;

    const markLine = to != null ? {
        silent: true,
        symbol: 'none',
        lineStyle: { color: border, type: 'solid', width: 1 },
        label: {
            formatter: label,
            position: 'insideStartTop',
            color: '#fff',
            backgroundColor: border,
            padding: [2, 5, 2, 5],
            fontSize: 10,
        },
        data: [{ xAxis: to }],
    } : null;

    return { reason, isAnomaly, from, to, remainingS, markArea, markLine, label };
}

/* ── Unavailable panel model ─────────────────────────────────────────────────
 *
 * buildUnavailablePanel(data, opts) → {
 *   message,           // the server's reason ("requires full-fidelity data")
 *   fidelity,          // the window's actual fidelity (sampled/none)
 *   canEscalate,       // whether the escalate affordance should be offered
 *   title, hint,       // copy for the panel
 * }
 * opts: { escalationSupported } from the daemon status (false → hide the button
 * and explain why).
 */
export function buildUnavailablePanel(data, opts) {
    opts = opts || {};
    const message = (data && data.unavailable) || UNAVAILABLE_MSG;
    const fidelity = fidelityOf(data);
    const supported = opts.escalationSupported !== false;
    return {
        message,
        fidelity,
        canEscalate: supported,
        title: 'No full-fidelity data in this window',
        hint: supported
            ? 'This view needs exact transition data. The current window is ' +
              fidelityLabel(fidelity).toLowerCase() +
              '. Escalate to capture full fidelity, then refresh.'
            : 'This view needs exact transition data, which is only captured ' +
              'while escalated. Escalation is not available (run the daemon in ' +
              '--mode tiered).',
    };
}

/* ── Daemon self-metrics panel model ─────────────────────────────────────────
 *
 * buildMetricsPanel(metrics, status) → {
 *   tier,                  // "sampled" | "escalated"
 *   rows: [{ label, value, hint }],  // events/s, samples/s, drops, overhead, anomaly, budget
 *   escalation: { active, remainingS, budgetRemainingS },
 * }
 * Pure: formats numbers to display strings, derives an overhead estimate.
 */
export function buildMetricsPanel(metrics, status) {
    metrics = metrics || {};
    status = status || {};
    const tier = metrics.tier || status.tier || 'sampled';

    const eps = num(metrics.events_per_sec);
    const sps = num(metrics.samples_per_sec);
    const drops = num(metrics.ringbuf_drops_total);
    const faults = num(metrics.sample_read_faults_total);
    const fires = num(metrics.anomaly_fires_total);
    const near = num(metrics.anomaly_near_total);
    const droppedBudget = num(metrics.anomaly_dropped_budget_total);
    const droppedCooldown = num(metrics.anomaly_dropped_cooldown_total);
    const budgetRemaining = num(metrics.escalation_budget_remaining_s);
    const budgetUnlimited = metrics.escalation_budget_unlimited === true ||
                            budgetRemaining < 0;   /* ESC-6 */
    const remaining = num(metrics.escalation_seconds_remaining);
    const active = !!metrics.escalation_active;
    const baselineAas = num(metrics.anomaly_baseline_aas);

    const overhead = estimateOverhead(eps, sps);

    const rows = [
        { label: 'Tier', value: tier === 'escalated' ? 'Escalated (full)' : 'Sampled' },
        { label: 'Events/s', value: fmtRate(eps) },
        { label: 'Samples/s', value: fmtRate(sps) },
        { label: 'Ringbuf drops', value: fmtInt(drops),
          warn: drops > 0 },
        { label: 'Sample faults', value: fmtInt(faults), warn: faults > 0 },
        { label: 'Est. overhead', value: overhead.text, hint: overhead.hint },
        { label: 'Anomaly fires', value: fmtInt(fires) },
        { label: 'Anomaly near', value: fmtInt(near) },
        { label: 'Anomaly dropped', value: fmtInt(droppedBudget + droppedCooldown),
          hint: droppedBudget + ' over-budget, ' + droppedCooldown + ' in cooldown' },
        { label: 'Baseline AAS', value: baselineAas != null ? baselineAas.toFixed(2) : '–' },
        { label: 'Budget left',
          value: budgetUnlimited ? '∞' : fmtSeconds(budgetRemaining) },
    ];

    return {
        tier,
        rows,
        escalation: {
            active,
            remainingS: remaining,
            budgetRemainingS: budgetRemaining,
        },
    };
}

/* ── Escalate control model ──────────────────────────────────────────────────
 *
 * buildEscalateControl(status) → {
 *   supported,         // escalation_supported (tiered mode)
 *   escalated,         // tier === "escalated"
 *   reason,            // current window reason
 *   remainingS,        // seconds left in window
 *   budgetRemainingS,  // rolling-hour budget left
 *   buttonLabel,       // "Escalate 60s" / "Escalated · 42s left"
 *   canEscalate,       // button enabled?
 *   canDeescalate,
 * }
 */
export function buildEscalateControl(status) {
    status = status || {};
    const supported = status.escalation_supported !== false &&
                      status.escalation_supported !== undefined;
    const escalated = status.tier === 'escalated';
    const remainingS = num(status.escalation_seconds_remaining);
    const budgetRemainingS = num(status.escalation_budget_remaining_s);
    /* ESC-6: unlimited budget = no cap. It surfaces as the explicit flag (and,
     * for older daemons, as a negative remaining sentinel). Escalation is
     * always allowed; a budget of exactly 0 = deny-all (escalation disabled). */
    const unlimited = status.escalation_budget_unlimited === true ||
                      budgetRemainingS < 0;
    const reason = status.escalation_reason || (escalated ? 'manual' : 'none');

    return {
        supported,
        escalated,
        reason,
        remainingS,
        budgetRemainingS,
        budgetUnlimited: unlimited,
        buttonLabel: escalated
            ? 'Escalated · ' + Math.ceil(remainingS) + 's left'
            : 'Escalate 60s',
        canEscalate: supported && (unlimited || budgetRemainingS > 0),
        canDeescalate: supported && escalated,
        budgetText: (unlimited ? '∞' : fmtSeconds(budgetRemainingS)) + ' budget',
    };
}

/* Interpret an escalate/deescalate control reply into a status message.
 * reply is the inner daemon response: {ok, escalated, granted_s,
 * seconds_remaining, budget_remaining_s} or {ok:false, error, budget_remaining_s}. */
export function buildEscalateResult(reply) {
    if (!reply) return { ok: false, text: 'No response from daemon' };
    if (reply.ok === false || reply.error) {
        const budget = reply.budget_remaining_s != null
            ? ' (budget ' + fmtSeconds(reply.budget_remaining_s) + ')' : '';
        return { ok: false, text: (reply.error || 'Escalation denied') + budget };
    }
    if (reply.escalated) {
        return {
            ok: true,
            text: 'Escalated for ' + Math.round(num(reply.granted_s)) + 's' +
                  ' · ' + fmtSeconds(reply.budget_remaining_s) + ' budget left',
        };
    }
    return { ok: true, text: 'De-escalated · ' +
                            fmtSeconds(reply.budget_remaining_s) + ' budget left' };
}

/* ── Number helpers (pure, exported for tests) ───────────────────────────── */

function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : 0; }

export function fmtRate(v) {
    v = num(v);
    if (v >= 1e6) return (v / 1e6).toFixed(1) + 'M';
    if (v >= 1e3) return (v / 1e3).toFixed(1) + 'K';
    return v.toFixed(v < 10 ? 1 : 0);
}

export function fmtInt(v) {
    v = num(v);
    if (v >= 1e6) return (v / 1e6).toFixed(1) + 'M';
    if (v >= 1e3) return (v / 1e3).toFixed(1) + 'K';
    return String(Math.round(v));
}

export function fmtSeconds(v) {
    v = num(v);
    if (v >= 3600) return (v / 3600).toFixed(1) + 'h';
    if (v >= 60) return (v / 60).toFixed(1) + 'm';
    return v.toFixed(0) + 's';
}

/* Rough overhead estimate from throughput. The sampled tier is ~free; the full
 * tier's cost scales with trap rate (events/s). This is a presentation-only
 * heuristic — honest, conservative, and labeled as an estimate. */
export function estimateOverhead(eventsPerSec, samplesPerSec) {
    const eps = num(eventsPerSec);
    // ~1µs per full-fidelity trap is a deliberately conservative figure.
    const trapCostNs = 1000;
    const fracOfOneCore = (eps * trapCostNs) / 1e9;  // share of one core
    const pct = fracOfOneCore * 100;
    let text;
    if (pct < 0.05) text = '<0.05%';
    else if (pct < 1) text = pct.toFixed(2) + '%';
    else text = pct.toFixed(1) + '%';
    return {
        pct,
        text,
        hint: 'estimate: ' + fmtRate(eps) + ' traps/s × ~1µs/trap',
    };
}
