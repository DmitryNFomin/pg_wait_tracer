/* pgwt — thin DOM mount layer for the fidelity-aware panels (Phase B5).
 *
 * The models come from lib/builders/fidelity.js (pure, Node-tested); this file
 * is the ONLY place that touches the DOM for them. Three panels:
 *
 *   - mountUnavailablePanel: the "no full-fidelity data in this window —
 *     escalate to capture" state for EXACT-only views over sampled windows.
 *   - mountEscalateControl: the header "Escalate 60s" button + budget/window
 *     readout, with a de-escalate affordance while active.
 *   - mountMetricsPanel: the collapsible daemon self-metrics panel.
 *
 * All three share an escalate() action wired through lib/control.js. Escalation
 * is operable from the browser over the same WebSocket (pgwt-server proxies the
 * daemon control socket as the `control` command).
 *
 * U2 (review P10) adds two PURE, Node-tested pane-chrome builders consumed by
 * the table views: the per-pane fidelity badge (buildPaneFidelity /
 * paneFidelityBadgeHtml) and the events percentile-basis footnote
 * (buildPercentileBasis / percentileBasisHtml). They return models / HTML
 * strings only; the views own the insertion points.
 */

import {
    buildUnavailablePanel, buildEscalateControl, buildMetricsPanel,
    buildEscalateResult, fidelityOf, fidelityLabel,
    SAMPLED_BAND_COLOR, MIXED_BAND_COLOR, SAMPLED_BORDER, MIXED_BORDER,
} from './builders/fidelity.js';
import {
    controlEscalate, controlDeescalate, ControlUnavailable,
} from './control.js';
import { esc, fmtCount } from './format.js';

/* ── Per-pane fidelity badge (U2, review P10) ────────────────────────────────
 *
 * The Trust-Milestone fidelity story used to stop at the AAS chart: in a
 * sampled/mixed window the tables' DB-time/AAS/count numbers are SCALED
 * ESTIMATES rendered pixel-identically to exact ones. Every table pane now
 * carries a compact chip that reuses the AAS chip's model surface
 * (fidelityOf/fidelityLabel) and its exact colors (the SAMPLED/MIXED band
 * fills + dashed borders from builders/fidelity.js), so "amber-dash =
 * sampled estimate" reads the same everywhere. Hidden for exact windows —
 * the common case stays noise-free, matching the AAS chip.
 *
 * buildPaneFidelity(data) -> null | { fidelity, label, fill, border, title }
 * (pure, Node-tested); paneFidelityBadgeHtml(model) -> HTML string (inline
 * styles: pane chrome ships with the component, no stylesheet dependency). */
export function buildPaneFidelity(data) {
    const fid = fidelityOf(data);
    if (fid !== 'sampled' && fid !== 'mixed') return null;
    const sampled = fid === 'sampled';
    return {
        fidelity: fid,
        label: fidelityLabel(fid),
        fill: sampled ? SAMPLED_BAND_COLOR : MIXED_BAND_COLOR,
        border: sampled ? SAMPLED_BORDER : MIXED_BORDER,
        title: sampled
            ? 'This window is sampled-tier: counts and times in this pane ' +
              'are scaled estimates, not exact measurements.'
            : 'This window mixes sampled and exact capture: numbers in this ' +
              'pane may combine scaled estimates with exact measurements.',
    };
}

export function paneFidelityBadgeHtml(model) {
    if (!model) return '';
    return '<div class="pane-fidelity-badge" title="' + esc(model.title) +
        '" style="display:inline-flex;align-items:center;gap:6px;' +
        'margin:0 0 6px 2px;padding:2px 8px;border:1px dashed ' + model.border +
        ';border-radius:3px;background:' + model.fill +
        ';font-size:11px;color:#aaa">' +
        '<span style="width:8px;height:8px;border-radius:50%;background:' +
        model.border + '"></span>' + esc(model.label) + '</div>';
}

/* ── Percentile-basis footnote (U2, review P10) ──────────────────────────────
 *
 * The events table's Avg/P50/P95/P99/Max come from EXACT-captured events
 * only (src/compute.c hist_percentile over exact_count; sampled durations
 * would be fabrications ≈ the sample period, FID-3). Over a mostly-sampled
 * window those columns describe a small exact subset — this footnote states
 * that basis whenever the window is not fully exact.
 *
 * Counts ("N of M"): rows would need both `count` (total, ASH-scaled) and
 * `exact_count` (the percentile basis). `exact_count` exists in
 * struct pgwt_event_row and gates the null-vs-number emit
 * (src/server.c:1916) but is NOT itself emitted — a recorded SERVER GAP
 * (one cjson_add_uint64 per row). Until it ships, the footnote is
 * qualitative; when every row carries both fields the same builder renders
 * the exact "N of M". Never fabricated client-side. */
export function buildPercentileBasis(data) {
    const fid = fidelityOf(data);
    if (fid === 'exact' || fid === 'none') return null;
    const rows = (data && data.rows) || [];
    if (!rows.length) return null;

    let exact = 0, total = 0, haveCounts = true, anyPctl = false;
    for (const r of rows) {
        if (r.p50_us != null) anyPctl = true;
        if (typeof r.exact_count === 'number' && typeof r.count === 'number') {
            exact += r.exact_count;
            total += r.count;
        } else {
            haveCounts = false;
        }
    }
    if (haveCounts && total > 0) {
        return {
            exact, total,
            text: 'Percentiles from exact-captured events only: ' +
                  fmtCount(exact) + ' of ' + fmtCount(total) + ' waits',
        };
    }
    return {
        exact: null, total: null,
        text: anyPctl
            ? 'Avg/P50/P95/P99/Max are from exact-captured events only — ' +
              'sampled-tier waits are not included in them'
            : 'Avg/P50/P95/P99/Max need exact-captured events; this window ' +
              'has none (sampled tier)',
    };
}

export function percentileBasisHtml(model) {
    if (!model) return '';
    return '<div class="pctl-basis-note" style="color:#777;font-size:11px;' +
        'font-style:italic;margin:6px 2px 0">' + esc(model.text) + '</div>';
}

/* Render the unavailable/escalate panel into `el` for an EXACT-only view whose
 * response was {"unavailable": ...}. `data` is that response; ctx supplies the
 * transport (for escalate) + escalation status + a refresh hook. */
export function mountUnavailablePanel(el, data, ctx) {
    const status = ctx.getEscalationStatus ? ctx.getEscalationStatus() : null;
    const model = buildUnavailablePanel(data, {
        escalationSupported: status ? status.escalation_supported : true,
    });
    if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';

    el.innerHTML =
        '<div class="unavailable-panel">' +
        '  <div class="unavailable-icon">◐</div>' +
        '  <div class="unavailable-title">' + esc(model.title) + '</div>' +
        '  <div class="unavailable-hint">' + esc(model.hint) + '</div>' +
        (model.canEscalate
            ? '  <button class="escalate-btn" id="unavail-escalate">Escalate 60s</button>' +
              '  <div class="unavailable-status" id="unavail-status"></div>'
            : '') +
        '</div>';

    if (model.canEscalate) {
        const btn = el.querySelector('#unavail-escalate');
        const statusEl = el.querySelector('#unavail-status');
        btn.addEventListener('click', async () => {
            await doEscalate(ctx, btn, statusEl, /*refreshAfter=*/true);
        });
    }
}

/* Shared escalate action: disables the button, calls control, shows the result,
 * refreshes the daemon status, and (optionally) re-runs the current view so an
 * exact-only view repaints once data is being captured. */
async function doEscalate(ctx, btn, statusEl, refreshAfter) {
    if (btn) { btn.disabled = true; btn.textContent = 'Escalating…'; }
    try {
        const reply = await controlEscalate(ctx.transport, 60, 'manual');
        const res = buildEscalateResult(reply);
        if (statusEl) {
            statusEl.textContent = res.text;
            statusEl.className = 'unavailable-status ' + (res.ok ? 'ok' : 'err');
        }
        if (ctx.refreshEscalationStatus) await ctx.refreshEscalationStatus();
        // Re-render the escalate header control if present.
        if (ctx.onEscalationChanged) ctx.onEscalationChanged();
        // Give the daemon a beat to start writing transition blocks, then
        // refresh the view so it can repaint with exact data.
        if (refreshAfter && ctx.refresh) {
            setTimeout(() => { try { ctx.refresh(); } catch (e) { /* best-effort */ } }, 800);
        }
    } catch (e) {
        if (statusEl) {
            statusEl.className = 'unavailable-status err';
            statusEl.textContent = (e instanceof ControlUnavailable)
                ? 'Escalation unavailable: ' + e.message
                : 'Escalation failed: ' + (e && e.message);
        }
    } finally {
        if (btn) { btn.disabled = false; btn.textContent = 'Escalate 60s'; }
    }
}

/* Render the header escalate control into `el`. Reads the daemon status from
 * ctx (getEscalationStatus) and re-renders itself after each action. */
export function mountEscalateControl(el, ctx) {
    if (!el) return;
    const status = ctx.getEscalationStatus ? ctx.getEscalationStatus() : null;
    const model = buildEscalateControl(status || {});

    if (!model.supported) {
        // Not tiered (or no daemon): hide the control entirely.
        el.innerHTML = '';
        el.style.display = 'none';
        return;
    }
    el.style.display = '';

    const cls = model.escalated ? 'escalate-btn active' : 'escalate-btn';
    el.innerHTML =
        '<button class="' + cls + '" id="hdr-escalate"' +
            (model.canEscalate || model.escalated ? '' : ' disabled') + '>' +
            esc(model.buttonLabel) + '</button>' +
        (model.canDeescalate
            ? '<button class="deescalate-btn" id="hdr-deescalate" title="Stop full-fidelity capture">Stop</button>'
            : '') +
        '<span class="escalate-budget" title="Full-fidelity seconds left this hour">' +
            esc(model.budgetText) + '</span>';

    const escBtn = el.querySelector('#hdr-escalate');
    if (escBtn && !model.escalated) {
        escBtn.addEventListener('click', async () => {
            await doEscalate(ctx, escBtn, null, /*refreshAfter=*/true);
            mountEscalateControl(el, ctx);   // re-render with new status
        });
    }
    const deBtn = el.querySelector('#hdr-deescalate');
    if (deBtn) {
        deBtn.addEventListener('click', async () => {
            deBtn.disabled = true;
            try {
                await controlDeescalate(ctx.transport);
                if (ctx.refreshEscalationStatus) await ctx.refreshEscalationStatus();
                if (ctx.refresh) ctx.refresh();
            } catch (e) { /* best-effort; status poll will correct */ }
            mountEscalateControl(el, ctx);
        });
    }
}

/* Render the daemon self-metrics panel into `el`. `metrics` is the control
 * `metrics` reply; `status` the `status` reply. */
export function mountMetricsPanel(el, metrics, status) {
    if (!el) return;
    if (!metrics) { el.innerHTML = ''; el.style.display = 'none'; return; }
    el.style.display = '';
    const model = buildMetricsPanel(metrics, status);

    let rows = '';
    for (const r of model.rows) {
        rows +=
            '<div class="dm-row' + (r.warn ? ' warn' : '') + '"' +
                (r.hint ? ' title="' + esc(r.hint) + '"' : '') + '>' +
            '<span class="dm-label">' + esc(r.label) + '</span>' +
            '<span class="dm-value">' + esc(String(r.value)) + '</span>' +
            '</div>';
    }
    el.innerHTML =
        '<div class="daemon-metrics-title">Daemon</div>' +
        '<div class="daemon-metrics-grid">' + rows + '</div>';
}
