/* Node unit tests for the fidelity-aware pure builders (lib/builders/fidelity.js).
 *
 * Phase B5. Proves the markArea shading model, the unavailable-panel model, the
 * escalation-annotation model (manual vs anomaly), the metrics-panel model, and
 * the escalate-control model — all without a browser, so a dropped band, a
 * wrong color, or a mis-derived overhead is caught in milliseconds.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    isUnavailable, fidelityOf, fidelityLabel,
    buildFidelityShading, bandsToMarkArea,
    buildEscalationAnnotation, buildUnavailablePanel,
    buildMetricsPanel, buildEscalateControl, buildEscalateResult,
    estimateOverhead, fmtRate, fmtInt, fmtSeconds,
    SAMPLED_BAND_COLOR, MIXED_BAND_COLOR, ESC_ANOMALY_COLOR, ESC_MANUAL_COLOR,
    UNAVAILABLE_MSG,
} from '../../web/static/lib/builders/fidelity.js';

const WIN = { from: 1000, to: 2000 };

// ── fidelity token helpers ───────────────────────────────────────────────────

test('fidelityOf: defaults to exact; passes through known tokens', () => {
    assert.equal(fidelityOf(null), 'exact');
    assert.equal(fidelityOf({}), 'exact');
    assert.equal(fidelityOf({ fidelity: 'sampled' }), 'sampled');
    assert.equal(fidelityOf({ fidelity: 'mixed' }), 'mixed');
    assert.equal(fidelityOf({ fidelity: 'bogus' }), 'exact');
});

test('isUnavailable: only true for the structured marker', () => {
    assert.equal(isUnavailable({ unavailable: UNAVAILABLE_MSG }), true);
    assert.equal(isUnavailable({ rows: [] }), false);
    assert.equal(isUnavailable(null), false);
});

test('fidelityLabel: human strings per token', () => {
    assert.match(fidelityLabel('sampled'), /Sampled/);
    assert.match(fidelityLabel('mixed'), /Mixed/);
    assert.equal(fidelityLabel('exact'), 'Exact');
});

// ── fidelity shading ─────────────────────────────────────────────────────────

test('shading: exact window → no bands, no markArea, no legend', () => {
    const s = buildFidelityShading({ fidelity: 'exact' }, WIN);
    assert.equal(s.fidelity, 'exact');
    assert.deepEqual(s.bands, []);
    assert.equal(s.markArea, null);
    assert.equal(s.showLegend, false);
});

test('shading: sampled window → one full-extent sampled band over the window', () => {
    const s = buildFidelityShading({ fidelity: 'sampled' }, WIN);
    assert.equal(s.bands.length, 1);
    assert.deepEqual(s.bands[0], { from: 1000, to: 2000, kind: 'sampled' });
    assert.equal(s.showLegend, true);
    // markArea endpoints carry xAxis coords + sampled fill color.
    const pair = s.markArea.data[0];
    assert.equal(pair[0].xAxis, 1000);
    assert.equal(pair[1].xAxis, 2000);
    assert.equal(pair[0].itemStyle.color, SAMPLED_BAND_COLOR);
});

test('shading: mixed window WITHOUT sub-ranges → whole window marked mixed', () => {
    const s = buildFidelityShading({ fidelity: 'mixed' }, WIN);
    assert.equal(s.bands.length, 1);
    assert.equal(s.bands[0].kind, 'mixed');
    assert.equal(s.markArea.data[0][0].itemStyle.color, MIXED_BAND_COLOR);
});

test('shading: mixed window WITH sub-ranges → bands only over sampled parts', () => {
    const data = {
        fidelity: 'mixed',
        fidelity_ranges: [
            { from: 1000, to: 1300, fidelity: 'sampled' },
            { from: 1300, to: 1700, fidelity: 'exact' },
            { from: 1700, to: 2000, fidelity: 'sampled' },
        ],
    };
    const s = buildFidelityShading(data, WIN);
    assert.equal(s.bands.length, 2);            // only the two sampled sub-ranges
    assert.deepEqual(s.bands.map(b => [b.from, b.to]), [[1000, 1300], [1700, 2000]]);
    assert.ok(s.bands.every(b => b.kind === 'sampled'));
});

test('bandsToMarkArea: null for empty; dashed border per kind', () => {
    assert.equal(bandsToMarkArea([]), null);
    const ma = bandsToMarkArea([{ from: 1, to: 2, kind: 'sampled' }]);
    assert.equal(ma.silent, true);
    assert.equal(ma.data[0][0].itemStyle.borderType, 'dashed');
});

// ── escalation annotation ────────────────────────────────────────────────────

test('escalation annotation: null when not escalated', () => {
    assert.equal(buildEscalationAnnotation({ tier: 'sampled' }, WIN), null);
    assert.equal(buildEscalationAnnotation(null, WIN), null);
});

test('escalation annotation: manual vs anomaly use distinct colors + labels', () => {
    const manual = buildEscalationAnnotation(
        { tier: 'escalated', escalation_reason: 'manual',
          escalation_seconds_remaining: 42, observed_start_ns: 1500 }, WIN);
    assert.equal(manual.reason, 'manual');
    assert.equal(manual.isAnomaly, false);
    assert.match(manual.label, /manual/);
    assert.equal(manual.markArea.data[0][1].itemStyle.color, ESC_MANUAL_COLOR);

    const anomaly = buildEscalationAnnotation(
        { tier: 'escalated', escalation_reason: 'anomaly',
          escalation_seconds_remaining: 30, observed_start_ns: 1500 },
        { ...WIN, axisMax: 1900 });
    assert.equal(anomaly.isAnomaly, true);
    assert.match(anomaly.label, /anomaly/);
    assert.equal(anomaly.markArea.data[0][1].itemStyle.color, ESC_ANOMALY_COLOR);
    // FLIPPED in U0 (review P2): this used to pin the markLine at win.to
    // (2000) — but the x-axis ends at the last bucket START, so an off-axis
    // markLine was DROPPED by ECharts and the edge never rendered. It now
    // clamps to axisMax, the last on-axis x value, and actually paints.
    assert.equal(anomaly.markLine.data[0].xAxis, 1900);
});

/* UI-10: the band must cover only the escalation window actually observed —
 * never the whole view window (a 15-min window with a 5 s escalation used to
 * shade all 15 min, implying full-fidelity capture where there is none). */
test('escalation annotation: band spans [observed start, live edge], not the window', () => {
    const a = buildEscalationAnnotation(
        { tier: 'escalated', escalation_reason: 'manual',
          escalation_seconds_remaining: 42, observed_start_ns: 1600 }, WIN);
    assert.equal(a.from, 1600, 'band starts where escalation was observed to start');
    assert.equal(a.to, 2000, 'band ends at the live edge');
    assert.equal(a.markArea.data[0][0].xAxis, 1600);
    assert.equal(a.markArea.data[0][1].xAxis, 2000);
});

test('escalation annotation: observed start clamps to the view window', () => {
    const a = buildEscalationAnnotation(
        { tier: 'escalated', escalation_reason: 'manual',
          escalation_seconds_remaining: 42, observed_start_ns: 5 }, WIN);
    assert.equal(a.from, WIN.from, 'start clamped to the visible window');
});

test('escalation annotation: unknown start -> markLine only, NO fabricated band', () => {
    const a = buildEscalationAnnotation(
        { tier: 'escalated', escalation_reason: 'manual',
          escalation_seconds_remaining: 42 }, { ...WIN, axisMax: 1900 });
    assert.notEqual(a, null, 'annotation still present');
    assert.equal(a.markArea, null, 'no band when the start is unknown');
    // FLIPPED in U0 (review P2): was pinned at win.to (2000, off-axis, never
    // rendered); now clamped to axisMax so the markLine actually paints.
    assert.equal(a.markLine.data[0].xAxis, 1900, 'live-edge markLine remains, on-axis');
    assert.match(a.label, /manual/);
});

test('escalation annotation: no axisMax -> live edge stays at win.to (no clamp)', () => {
    const a = buildEscalationAnnotation(
        { tier: 'escalated', escalation_reason: 'manual',
          escalation_seconds_remaining: 42, observed_start_ns: 1500 }, WIN);
    assert.equal(a.to, 2000);
    assert.equal(a.markLine.data[0].xAxis, 2000);
});

// ── unavailable panel ────────────────────────────────────────────────────────

test('unavailable panel: offers escalate when supported, with the server message', () => {
    const m = buildUnavailablePanel(
        { unavailable: UNAVAILABLE_MSG, fidelity: 'sampled' },
        { escalationSupported: true });
    assert.equal(m.message, UNAVAILABLE_MSG);
    assert.equal(m.fidelity, 'sampled');
    assert.equal(m.canEscalate, true);
    assert.match(m.hint, /Escalate/);
});

test('unavailable panel: hides escalate when escalation unsupported, explains why', () => {
    const m = buildUnavailablePanel(
        { unavailable: UNAVAILABLE_MSG, fidelity: 'sampled' },
        { escalationSupported: false });
    assert.equal(m.canEscalate, false);
    assert.match(m.hint, /not available|tiered/);
});

// ── metrics panel ────────────────────────────────────────────────────────────

test('metrics panel: derives rows incl. tier, rates, drops (warn), overhead, budget', () => {
    const metrics = {
        tier: 'escalated', events_per_sec: 4200, samples_per_sec: 60,
        ringbuf_drops_total: 3, sample_read_faults_total: 0,
        anomaly_fires_total: 2, anomaly_near_total: 5,
        anomaly_dropped_budget_total: 1, anomaly_dropped_cooldown_total: 0,
        anomaly_baseline_aas: 1.85, escalation_budget_remaining_s: 240,
        escalation_seconds_remaining: 30, escalation_active: true,
    };
    const m = buildMetricsPanel(metrics, { tier: 'escalated' });
    assert.equal(m.tier, 'escalated');
    const byLabel = Object.fromEntries(m.rows.map(r => [r.label, r]));
    assert.equal(byLabel['Events/s'].value, '4.2K');
    assert.equal(byLabel['Samples/s'].value, '60');
    assert.equal(byLabel['Ringbuf drops'].value, '3');
    assert.equal(byLabel['Ringbuf drops'].warn, true);
    assert.equal(byLabel['Anomaly fires'].value, '2');
    assert.match(byLabel['Est. overhead'].value, /%/);
    assert.equal(byLabel['Budget left'].value, '4.0m');
    assert.equal(m.escalation.active, true);
});

test('metrics panel: missing fields → safe zeros, no NaN', () => {
    const m = buildMetricsPanel({}, {});
    const byLabel = Object.fromEntries(m.rows.map(r => [r.label, r]));
    assert.equal(byLabel['Events/s'].value, '0.0');
    assert.equal(byLabel['Ringbuf drops'].warn, false);
    assert.equal(byLabel['Budget left'].value, '0s');
});

// ── escalate control ─────────────────────────────────────────────────────────

test('escalate control: sampled tier → "Escalate 60s", can escalate within budget', () => {
    const m = buildEscalateControl({
        escalation_supported: true, tier: 'sampled',
        escalation_budget_remaining_s: 300, escalation_seconds_remaining: 0,
    });
    assert.equal(m.supported, true);
    assert.equal(m.escalated, false);
    assert.equal(m.buttonLabel, 'Escalate 60s');
    assert.equal(m.canEscalate, true);
    assert.equal(m.canDeescalate, false);
});

test('escalate control: escalated tier → shows seconds left + deescalate', () => {
    const m = buildEscalateControl({
        escalation_supported: true, tier: 'escalated',
        escalation_reason: 'anomaly',
        escalation_budget_remaining_s: 240, escalation_seconds_remaining: 42,
    });
    assert.equal(m.escalated, true);
    assert.match(m.buttonLabel, /42s left/);
    assert.equal(m.canDeescalate, true);
    assert.equal(m.reason, 'anomaly');
});

test('escalate control: zero budget → cannot escalate', () => {
    const m = buildEscalateControl({
        escalation_supported: true, tier: 'sampled',
        escalation_budget_remaining_s: 0,
    });
    assert.equal(m.canEscalate, false);
});

test('escalate control: zero budget = deny-all → cannot escalate (ESC-6)', () => {
    // 0 now means escalation DISABLED — the control must not offer escalate.
    const m = buildEscalateControl({
        escalation_supported: true, tier: 'sampled',
        escalation_budget_remaining_s: 0, escalation_budget_unlimited: false,
    });
    assert.equal(m.canEscalate, false);
    assert.equal(m.budgetUnlimited, false);
});

test('escalate control: unlimited budget → can escalate, ∞ budget (ESC-6)', () => {
    const m = buildEscalateControl({
        escalation_supported: true, tier: 'sampled',
        escalation_budget_remaining_s: -1, escalation_budget_unlimited: true,
    });
    assert.equal(m.budgetUnlimited, true);
    assert.equal(m.canEscalate, true);
    assert.match(m.budgetText, /∞/);
});

test('escalate control: unsupported daemon → not supported (hide control)', () => {
    assert.equal(buildEscalateControl({}).supported, false);
    assert.equal(buildEscalateControl({ escalation_supported: false }).supported, false);
});

test('escalate result: ok grant, denial with budget, deescalate', () => {
    assert.match(buildEscalateResult(
        { ok: true, escalated: true, granted_s: 60, budget_remaining_s: 240 }).text,
        /Escalated for 60s/);
    const denied = buildEscalateResult(
        { ok: false, error: 'over budget', budget_remaining_s: 0 });
    assert.equal(denied.ok, false);
    assert.match(denied.text, /over budget/);
    assert.match(buildEscalateResult(
        { ok: true, escalated: false, budget_remaining_s: 240 }).text, /De-escalated/);
});

// ── number helpers ───────────────────────────────────────────────────────────

test('format helpers', () => {
    assert.equal(fmtRate(4200), '4.2K');
    assert.equal(fmtRate(2_500_000), '2.5M');
    assert.equal(fmtRate(7), '7.0');
    assert.equal(fmtInt(1500), '1.5K');
    assert.equal(fmtSeconds(240), '4.0m');
    assert.equal(fmtSeconds(7200), '2.0h');
    assert.equal(fmtSeconds(45), '45s');
});

test('estimateOverhead: conservative, labeled, scales with trap rate', () => {
    const low = estimateOverhead(100, 60);
    assert.equal(low.text, '<0.05%');
    const high = estimateOverhead(2_000_000, 60);   // 2M traps/s × 1µs = 2 cores
    assert.ok(high.pct > 100);
    assert.match(high.hint, /traps\/s/);
});
