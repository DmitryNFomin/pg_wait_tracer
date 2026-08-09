/* pgwt — deterministic fixtures for the fidelity render-model builders
 * (lib/builders/fidelity.js).
 *
 * These builders emit plain render models (markArea specs, panel models,
 * control models), not full ECharts options — so their states name WHICH
 * builder function to exercise:
 *   states['<kebab-name>'] = {
 *     description, tags,
 *     fn: 'shading' | 'annotation' | 'unavailable' | 'metrics' | 'escalate',
 *     args: [...],   // the function's positional arguments, pure data
 *   }
 * The gallery renders shading/annotation models as a band track over the
 * window plus the model JSON, and the panel/control models as HTML cards.
 * See ./aas.mjs for the directory-wide contract and determinism rules.
 *
 * Payload shapes mirror tests/mock_server.py's DaemonState.status()/metrics()
 * and src/server.c's structured unavailable marker.
 */

import { BASE_NS, MIN_NS } from './base.mjs';

const WIN = { from: BASE_NS, to: BASE_NS + 15 * MIN_NS };
/* Last on-axis x = last bucket START (< win.to) — the very gap that made the
 * escalation markLine unrenderable pre-U0 (P2). */
const WIN_AXIS = { ...WIN, axisMax: WIN.to - MIN_NS };

/* Mirrors DaemonState.metrics() in tests/mock_server.py (escalated tier). */
const METRICS_ESCALATED = {
    ok: true, uptime_s: 123.4, events_total: 1_250_000, events_per_sec: 4200.0,
    backends_tracked: 6, samples_total: 540_000, samples_per_sec: 60.0,
    sample_read_faults_total: 0, ringbuf_drops_total: 3,
    tier: 'escalated', escalation_active: true,
    escalation_seconds_remaining: 42.0, escalation_budget_remaining_s: 180.0,
    escalation_budget_unlimited: false,
    anomaly_fires_total: 2, anomaly_near_total: 5,
    anomaly_dropped_budget_total: 1, anomaly_dropped_cooldown_total: 0,
    anomaly_baseline_aas: 1.85,
};

const STATUS_ESCALATED = {
    ok: true, mode: 'tiered', tier: 'escalated', escalation_supported: true,
    escalation_seconds_remaining: 42.0, escalation_budget_remaining_s: 180.0,
    escalation_budget_unlimited: false, escalation_reason: 'anomaly',
};

export const states = {
    'shading-sampled': {
        description: 'Sampled window → one amber band spanning the whole window.',
        tags: ['FEEDBACK', 'SEMANTICS', 'fidelity'],
        fn: 'shading',
        args: [{ fidelity: 'sampled', sample_period_ns: 100_000_000 }, WIN],
    },
    'shading-mixed-subranges': {
        description: 'Mixed window with fidelity_ranges: amber bands ONLY over the sampled sub-ranges, the exact hole unshaded.',
        tags: ['ALIGNMENT', 'SEMANTICS', 'fidelity'],
        fn: 'shading',
        args: [{
            fidelity: 'mixed',
            fidelity_ranges: [
                { from: WIN.from, to: WIN.from + 5 * MIN_NS, fidelity: 'sampled' },
                { from: WIN.from + 5 * MIN_NS, to: WIN.from + 9 * MIN_NS, fidelity: 'exact' },
                { from: WIN.from + 9 * MIN_NS, to: WIN.to, fidelity: 'sampled' },
            ],
        }, WIN],
    },
    'shading-mixed-no-detail': {
        description: 'Mixed window WITHOUT sub-range detail → whole window banded indigo (the honest fallback).',
        tags: ['FEEDBACK', 'fidelity'],
        fn: 'shading',
        args: [{ fidelity: 'mixed' }, WIN],
    },
    'annotation-observed-anomaly': {
        description: 'Anomaly escalation with an observed start: red band over the observed span + labeled edge line clamped onto the axis.',
        tags: ['ALIGNMENT', 'FEEDBACK', 'fidelity'],
        fn: 'annotation',
        args: [{
            tier: 'escalated', escalation_reason: 'anomaly',
            escalation_seconds_remaining: 41,
            observed_start_ns: WIN.from + 10 * MIN_NS,
        }, WIN_AXIS],
    },
    'annotation-mid-escalation': {
        description: 'Escalation start unknown (page loaded mid-window): markArea must be null — edge line only, never a fabricated span (UI-10).',
        tags: ['FEEDBACK', 'fidelity'],
        fn: 'annotation',
        args: [{
            tier: 'escalated', escalation_reason: 'manual',
            escalation_seconds_remaining: 12,
        }, WIN_AXIS],
    },
    'unavailable-escalatable': {
        description: 'The server\'s structured "requires full-fidelity data" refusal with the escalate affordance offered.',
        tags: ['FEEDBACK', 'fidelity'],
        fn: 'unavailable',
        args: [
            { unavailable: 'requires full-fidelity data', fidelity: 'sampled' },
            { escalationSupported: true },
        ],
    },
    'unavailable-no-daemon': {
        description: 'Same refusal without escalation support: the panel must explain why instead of offering a dead button.',
        tags: ['FEEDBACK', 'fidelity'],
        fn: 'unavailable',
        args: [
            { unavailable: 'requires full-fidelity data', fidelity: 'none' },
            { escalationSupported: false },
        ],
    },
    'metrics-escalated': {
        description: 'Daemon self-metrics panel mid-escalation: drops warn, budget counts down, anomaly counters visible.',
        tags: ['FEEDBACK', 'HIERARCHY'],
        fn: 'metrics',
        args: [METRICS_ESCALATED, STATUS_ESCALATED],
    },
    'escalate-active': {
        description: 'Escalate control while escalated: countdown label, de-escalate enabled.',
        tags: ['FEEDBACK'],
        fn: 'escalate',
        args: [STATUS_ESCALATED],
    },
    'escalate-denied-budget': {
        description: 'Budget exhausted (0 s left): escalate must be disabled, not silently no-op.',
        tags: ['FEEDBACK'],
        fn: 'escalate',
        args: [{
            ok: true, mode: 'tiered', tier: 'sampled', escalation_supported: true,
            escalation_seconds_remaining: 0, escalation_budget_remaining_s: 0,
            escalation_budget_unlimited: false, escalation_reason: 'none',
        }],
    },
    'escalate-unlimited': {
        description: 'ESC-6 unlimited budget (flag + negative sentinel): shows ∞, always escalatable.',
        tags: ['SEMANTICS'],
        fn: 'escalate',
        args: [{
            ok: true, mode: 'tiered', tier: 'sampled', escalation_supported: true,
            escalation_seconds_remaining: 0, escalation_budget_remaining_s: -1,
            escalation_budget_unlimited: true, escalation_reason: 'none',
        }],
    },
    'compare-mismatch': {
        description: 'Compare header names both evidence classes and persistently warns when A exact is compared with sampled B.',
        tags: ['FEEDBACK', 'SEMANTICS', 'fidelity', 'compare'],
        fn: 'compare',
        args: [{ fidelity: 'exact' }, { fidelity: 'sampled' }, {}],
    },
    'compare-predates': {
        description: 'Expected compare edge: B is outside trace retention, shown as a quiet note rather than an error card.',
        tags: ['FEEDBACK', 'SEMANTICS', 'compare'],
        fn: 'compare',
        args: [{ fidelity: 'exact' }, null, { baselinePredates: true }],
    },
};
