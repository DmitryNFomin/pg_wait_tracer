/* pgwt — deterministic fixtures for the AAS builder (lib/builders/aas.js).
 *
 * State contract (shared by every module in this directory):
 *   states['<kebab-name>'] = {
 *     description,            // one line, shown in the cell footer
 *     tags: [...],            // UPPERCASE = docs/VISUAL_CHECKLIST.md words this
 *                             // cell exercises; lowercase = free-form facets
 *     data, opts,             // the builder's positional arguments
 *     ticks: [{data, opts}],  // OPTIONAL: pre-recorded live-replay sequence —
 *                             // the gallery replays it through the builder +
 *                             // chart.setOption(option, true), the app's exact
 *                             // refresh path (views/active.js)
 *   }
 * Gallery cell id = gallery-<builder>-<state>; manifest id = <builder>/<state>.
 *
 * Canonical location is web/static/dev/fixtures/ because the gallery page can
 * only fetch below the static root (web/static/) that tests/mock_server.py
 * serves. tests/fixtures/*.mjs re-export these for Node-side imports.
 *
 * Input shape (from the aas builder + tests/mock_server.py): class mode =
 * { bucket_ns, max_aas, buckets: [{t, cpu, io, lock, ...}] }; event-breakdown
 * mode adds { breakdown: 'events', series: [{name, event_id}],
 * buckets: [{t, aas: [per-series]}] }. Fidelity fields ride on top.
 */

import { BASE_NS, SEC_NS, MIN_NS, r4, tri } from './base.mjs';

const CLASS_KEYS = ['cpu', 'io', 'lock', 'lwlock', 'ipc', 'client', 'timeout',
    'bufferpin', 'activity', 'extension', 'unknown'];

function classBucket(t, values) {
    const b = { t };
    for (const k of CLASS_KEYS) b[k] = values[k] || 0;
    return b;
}

function bucketTotal(b) {
    let s = 0;
    for (const k of CLASS_KEYS) s += b[k] || 0;
    return r4(s);
}

function maxTotal(buckets) {
    let m = 0;
    for (const b of buckets) m = Math.max(m, bucketTotal(b));
    return m;
}

/* ── Class-mode datasets ─────────────────────────────────────────────────── */

const B60 = MIN_NS;              // 60 s buckets (the mock's default)
const B12 = 12 * SEC_NS;         // 12 s buckets for the dense 300-bucket hour

function singlePointData() {
    const buckets = [classBucket(BASE_NS, {
        cpu: 1.2, io: 0.8, lock: 0.1, lwlock: 0.3, timeout: 0.5, extension: 0.9,
    })];
    return { bucket_ns: B60, max_aas: maxTotal(buckets), buckets };
}

/* 300 buckets, every class non-zero somewhere, plus a triangular lock storm
 * (buckets 140–169) so the dense cell has one obvious "anomaly" to eyeball. */
function denseData() {
    const buckets = [];
    for (let i = 0; i < 300; i++) {
        const storm = (i >= 140 && i < 170) ? 4.0 * (1 - Math.abs(i - 155) / 15) : 0;
        buckets.push(classBucket(BASE_NS + i * B12, {
            cpu: r4(0.8 + 1.2 * tri(i, 17)),
            io: r4(0.5 + 0.8 * tri(i + 5, 11)),
            lock: r4(0.15 + storm),
            lwlock: r4(0.25 + 0.5 * tri(i + 3, 7)),
            ipc: r4(0.03 + 0.1 * tri(i, 29)),
            client: 0,
            timeout: (i % 25 === 0) ? 0.45 : 0.3,
            bufferpin: (i % 60 === 30) ? 0.2 : 0,
            activity: 0.02,
            extension: r4(0.2 + 0.3 * tri(i + 11, 23)),
            unknown: (i % 97 === 50) ? 0.05 : 0,
        }));
    }
    return { bucket_ns: B12, max_aas: maxTotal(buckets), buckets };
}

function allZeroData() {
    const buckets = [];
    for (let i = 0; i < 60; i++) buckets.push(classBucket(BASE_NS + i * B60, {}));
    return { bucket_ns: B60, max_aas: 0, buckets };
}

/* One bucket dwarfs everything: lock spikes to 480 on a 64-vCPU host.
 * Exercises the P7 flattening (CPU line vs data sliver) at its worst. */
function oneHugeValueData() {
    const buckets = [];
    for (let i = 0; i < 60; i++) {
        buckets.push(classBucket(BASE_NS + i * B60, {
            cpu: r4(1.0 + 0.5 * tri(i, 9)),
            io: 0.6,
            lock: (i === 30) ? 480 : 0.1,
        }));
    }
    return { bucket_ns: B60, max_aas: maxTotal(buckets), buckets };
}

function sampledData() {
    const d = denseData();
    return { ...d, fidelity: 'sampled', sample_period_ns: 100_000_000 };
}

function mixedEscalationData() {
    const d = denseData();
    const from = d.buckets[0].t;
    return {
        ...d,
        fidelity: 'mixed',
        sample_period_ns: 100_000_000,
        // Sampled → exact → sampled: the exact hole is the escalation window.
        fidelity_ranges: [
            { from, to: from + 20 * MIN_NS, fidelity: 'sampled' },
            { from: from + 20 * MIN_NS, to: from + 35 * MIN_NS, fidelity: 'exact' },
            { from: from + 35 * MIN_NS, to: from + 60 * MIN_NS, fidelity: 'sampled' },
        ],
    };
}

/* ── Event-breakdown datasets ────────────────────────────────────────────── */

/* 16 events across 8 classes — the server's top-16 cap, saturated. */
const EVENTS16 = [
    { name: 'IO:DataFileRead',          event_id: 0x01000015 },
    { name: 'IO:WalSync',               event_id: 0x0100004e },
    { name: 'IO:WalWrite',              event_id: 0x01000050 },
    { name: 'IO:DataFileWrite',         event_id: 0x01000018 },
    { name: 'LWLock:WALInsert',         event_id: 0x04000007 },
    { name: 'LWLock:WALWrite',          event_id: 0x04000008 },
    { name: 'LWLock:BufferMapping',     event_id: 0x04000002 },
    { name: 'LWLock:LockManager',       event_id: 0x0400000b },
    { name: 'Lock:relation',            event_id: 0x03000000 },
    { name: 'Lock:transactionid',       event_id: 0x03000003 },
    { name: 'Lock:tuple',               event_id: 0x03000004 },
    { name: 'IPC:BufferIO',             event_id: 0x05000001 },
    { name: 'IPC:MessageQueueReceive',  event_id: 0x0500000e },
    { name: 'Client:ClientWrite',       event_id: 0x06000002 },
    { name: 'Timeout:VacuumDelay',      event_id: 0x09000005 },
    { name: 'Activity:LogicalLauncherMain', event_id: 0x02000006 },
];

/* Descending per-rank bases (loop-multiplied — Math.pow is impl-defined). */
const EVENT_BASES = (() => {
    const out = [1.2];
    for (let k = 1; k < EVENTS16.length; k++) out.push(out[k - 1] * 0.85);
    return out;
})();

function eventsData(series, nBuckets, bucketNs, valueAt) {
    const buckets = [];
    let maxAas = 0;
    for (let i = 0; i < nBuckets; i++) {
        const aas = series.map((s, k) => r4(valueAt(i, k)));
        let total = 0;
        for (const v of aas) total += v;
        maxAas = Math.max(maxAas, r4(total));
        buckets.push({ t: BASE_NS + i * bucketNs, aas });
    }
    return {
        bucket_ns: bucketNs, max_aas: maxAas, breakdown: 'events',
        series: series.map(s => ({ name: s.name, event_id: s.event_id })),
        buckets,
    };
}

function denseEventsData() {
    return eventsData(EVENTS16, 300, B12,
        (i, k) => EVENT_BASES[k] * (0.4 + 0.6 * tri(i + k * 7, 13 + (k % 5))));
}

const UNICODE_EVENTS = [
    { name: 'Extension:péngüîn_🐧',        event_id: 0x0a000101 },
    { name: 'Extension:日本語イベント',      event_id: 0x0a000102 },
    { name: 'IO:Ωmega_read',               event_id: 0x01000103 },
    { name: 'LWLock:Ελληνικά',             event_id: 0x04000104 },
    { name: 'Lock:кириллица',              event_id: 0x03000105 },
];

function unicodeNamesData() {
    return eventsData(UNICODE_EVENTS, 60, B60,
        (i, k) => (0.6 - k * 0.09) * (0.5 + 0.5 * tri(i + k * 5, 11)));
}

/* Server-derived strings are attacker-influenced (extension wait events carry
 * extension-chosen names; UI-6). Nothing here may execute or break markup. */
const HOSTILE_EVENTS = [
    { name: 'Lock:<script>document.title="pwned"</script>', event_id: 0x03000201 },
    { name: 'IO:"><img src=x onerror=alert(1)>',            event_id: 0x01000202 },
    { name: "Extension:'; DROP TABLE waits; --",            event_id: 0x0a000203 },
    { name: 'IPC:a&b<c>d"e\'f',                             event_id: 0x05000204 },
];

function hostileNamesData() {
    return eventsData(HOSTILE_EVENTS, 60, B60,
        (i, k) => (0.7 - k * 0.12) * (0.5 + 0.5 * tri(i + k * 4, 9)));
}

/* ── The live-tick replay sequence ───────────────────────────────────────────
 *
 * ~10 successive event-breakdown payloads simulating a rolling live window
 * (60 × 5 s buckets, advancing 30 s per tick) where the SERVER-SIDE TOP-N
 * RANKING CHANGES BETWEEN TICKS — src/compute.c re-ranks the top events by
 * total AAS per window and emits the series array in rank order every tick.
 * Lock:transactionid surges from outside the top-8 to rank ~1 while
 * IO:DataFileRead declines, so both membership and order churn. This is
 * exactly the instability the P2 identity fixes address: with index-keyed
 * colors/legend, stepping tick→tick makes the same event flash through
 * different colors and stack positions (STABILITY/CONTINUITY violations).
 */

const TICK_POOL = [
    // name, event_id, base, slope (per bucket), amp, phase
    { name: 'IO:DataFileRead',      event_id: 0x01000015, base: 0.95, slope: -0.0050, amp: 0.10, phase: 0 },
    { name: 'IO:WalSync',           event_id: 0x0100004e, base: 0.55, slope: 0,       amp: 0.08, phase: 2 },
    { name: 'IO:WalWrite',          event_id: 0x01000050, base: 0.30, slope: 0,       amp: 0.06, phase: 4 },
    { name: 'LWLock:WALInsert',     event_id: 0x04000007, base: 0.40, slope: 0.0040,  amp: 0.08, phase: 6 },
    { name: 'LWLock:WALWrite',      event_id: 0x04000008, base: 0.42, slope: 0,       amp: 0.05, phase: 8 },
    { name: 'LWLock:BufferMapping', event_id: 0x04000002, base: 0.36, slope: 0,       amp: 0.05, phase: 10 },
    { name: 'Lock:transactionid',   event_id: 0x03000003, base: -0.15, slope: 0.0115, amp: 0.04, phase: 12 },
    { name: 'Lock:tuple',           event_id: 0x03000004, base: 0.33, slope: 0,       amp: 0.05, phase: 14 },
    { name: 'IPC:BufferIO',         event_id: 0x05000001, base: 0.31, slope: 0,       amp: 0.04, phase: 16 },
    { name: 'Client:ClientWrite',   event_id: 0x06000002, base: 0.28, slope: 0,       amp: 0.04, phase: 3 },
    { name: 'Timeout:VacuumDelay',  event_id: 0x09000005, base: 0.26, slope: 0,       amp: 0.03, phase: 7 },
    { name: 'IO:DataFileWrite',     event_id: 0x01000018, base: 0.24, slope: 0.0008,  amp: 0.04, phase: 11 },
];

const TICK_COUNT = 10;
const TICK_TOP_N = 8;
const TICK_BUCKETS = 60;          // 60 × 5 s = a 300 s live window
const TICK_STEP_BUCKETS = 6;      // window advances 30 s per tick
const TICK_BUCKET_NS = 5 * SEC_NS;

function tickValue(ev, b) {
    return Math.max(0, ev.base + ev.slope * b + ev.amp * tri(b + ev.phase, 9));
}

function buildTicks() {
    const ticks = [];
    for (let t = 0; t < TICK_COUNT; t++) {
        const b0 = t * TICK_STEP_BUCKETS;
        // Server-side ranking: total AAS over the window, descending
        // (name tiebreak for determinism), top N — in RANK ORDER.
        const ranked = TICK_POOL
            .map(ev => {
                let total = 0;
                for (let b = b0; b < b0 + TICK_BUCKETS; b++) total += tickValue(ev, b);
                return { ev, total };
            })
            .sort((a, b) => (b.total - a.total) || (a.ev.name < b.ev.name ? -1 : a.ev.name > b.ev.name ? 1 : 0))
            .slice(0, TICK_TOP_N)
            .map(r => r.ev);

        const buckets = [];
        let maxAas = 0;
        for (let b = b0; b < b0 + TICK_BUCKETS; b++) {
            const aas = ranked.map(ev => r4(tickValue(ev, b)));
            let total = 0;
            for (const v of aas) total += v;
            maxAas = Math.max(maxAas, r4(total));
            buckets.push({ t: BASE_NS + b * TICK_BUCKET_NS, aas });
        }

        ticks.push({
            data: {
                bucket_ns: TICK_BUCKET_NS, max_aas: maxAas, breakdown: 'events',
                series: ranked.map(ev => ({ name: ev.name, event_id: ev.event_id })),
                buckets, fidelity: 'exact',
            },
            opts: {
                numCpus: 4,
                win: {
                    from: BASE_NS + b0 * TICK_BUCKET_NS,
                    to: BASE_NS + (b0 + TICK_BUCKETS) * TICK_BUCKET_NS,
                },
            },
        });
    }
    return ticks;
}

/* ── States ──────────────────────────────────────────────────────────────── */

const DENSE = denseData();
const WIN_DENSE = { from: BASE_NS, to: BASE_NS + 300 * B12 };
const WIN_60 = { from: BASE_NS, to: BASE_NS + 60 * B60 };
const COMPARE_OFFSET = -5 * MIN_NS;
const COMPARE_BASELINE = (() => {
    const buckets = DENSE.buckets.map(b => classBucket(b.t + COMPARE_OFFSET, {
        cpu: r4(b.cpu * 0.68), io: r4(b.io * 1.25),
        lock: r4(b.lock * 0.45), lwlock: r4(b.lwlock * 0.8),
        ipc: b.ipc, client: b.client, timeout: r4(b.timeout * 1.1),
        bufferpin: b.bufferpin, activity: b.activity,
        extension: r4(b.extension * 0.75), unknown: b.unknown,
    }));
    return { bucket_ns: B12, max_aas: maxTotal(buckets), buckets,
        fidelity: 'exact' };
})();
const COMPARE_STATE = {
    description: 'Compare ON: dashed B-total ghost behind A plus signed per-class A−B AAS·s geometry (final bucket provisional).',
    tags: ['ALIGNMENT', 'SEMANTICS', 'compare'],
    data: DENSE,
    opts: {
        numCpus: 8, win: WIN_DENSE,
        compareData: COMPARE_BASELINE,
        compareOffsetNs: COMPARE_OFFSET,
        compareProvisional: true,
    },
};

export const states = {
    'empty': {
        description: 'No buckets — must render the empty-state text, never a stale or blank chart.',
        tags: ['FEEDBACK'],
        data: { bucket_ns: 0, max_aas: 0, buckets: [] },
        opts: { numCpus: 4, win: WIN_60 },
    },
    'single-point': {
        description: 'One bucket. Area stack degenerates to a single column; axes must stay sane.',
        tags: ['ALIGNMENT'],
        data: singlePointData(),
        opts: { numCpus: 4, win: { from: BASE_NS, to: BASE_NS + B60 } },
    },
    'dense': {
        description: '300 buckets, all 11 classes, with a triangular Lock storm at buckets 140–169.',
        tags: ['HIERARCHY', 'OCCLUSION'],
        data: DENSE,
        opts: { numCpus: 8, win: WIN_DENSE },
    },
    'dense-events': {
        description: 'Event breakdown at the server top-16 cap: 16 events across 8 classes, 300 buckets.',
        tags: ['STABILITY', 'SEMANTICS'],
        data: denseEventsData(),
        opts: { numCpus: 8, win: WIN_DENSE },
    },
    'all-zero': {
        description: 'Buckets present but every value 0 — an idle-but-captured window, distinct from empty.',
        tags: ['FEEDBACK'],
        data: allZeroData(),
        opts: { numCpus: 4, win: WIN_60 },
    },
    'one-huge-value': {
        description: 'Lock spikes to 480 in one bucket on a 64-vCPU host — worst case for y-axis flattening (P7).',
        tags: ['HIERARCHY'],
        data: oneHugeValueData(),
        opts: { numCpus: 64, win: WIN_60 },
    },
    'unicode-names': {
        description: 'Event names with CJK/Greek/Cyrillic/emoji — legend, stack and tooltip must not garble.',
        tags: ['SEMANTICS', 'i18n'],
        data: unicodeNamesData(),
        opts: { numCpus: 4, win: WIN_60 },
    },
    'hostile-names': {
        description: 'Event names carrying <script>/quote/HTML payloads (UI-6) — must render inert.',
        tags: ['FEEDBACK', 'injection'],
        data: hostileNamesData(),
        opts: { numCpus: 4, win: WIN_60 },
    },
    'sampled': {
        description: 'Pure sampled window: amber estimated-data band must span the whole window.',
        tags: ['FEEDBACK', 'SEMANTICS', 'fidelity'],
        data: sampledData(),
        opts: { numCpus: 8, win: WIN_DENSE },
    },
    'mixed-escalation': {
        description: 'Mixed window (sampled sub-ranges shaded) with an observed anomaly-escalation band + edge line.',
        tags: ['ALIGNMENT', 'FEEDBACK', 'fidelity'],
        data: mixedEscalationData(),
        opts: {
            numCpus: 8, win: WIN_DENSE,
            escalationStatus: {
                tier: 'escalated', escalation_reason: 'anomaly',
                escalation_seconds_remaining: 42,
                observed_start_ns: BASE_NS + 20 * MIN_NS,
            },
        },
    },
    'escalated-live-edge': {
        description: 'Page loaded mid-escalation (start unknown): edge line only, NO band — never fabricate (UI-10). Before U0 this line never rendered (P2).',
        tags: ['ALIGNMENT', 'FEEDBACK', 'fidelity'],
        data: DENSE,
        opts: {
            numCpus: 8, win: WIN_DENSE,
            escalationStatus: {
                tier: 'escalated', escalation_reason: 'manual',
                escalation_seconds_remaining: 30,
            },
        },
    },
    'live-ticks': {
        description: 'Recorded 10-tick live replay: rolling 300 s window, server top-8 ranking churns (Lock:transactionid surges in, IO:DataFileRead declines). Step ticks to eyeball identity stability.',
        tags: ['STABILITY', 'CONTINUITY', 'replay'],
        data: null,   // ticks[0] is the initial render
        opts: null,
        ticks: buildTicks(),
    },
};

/* ── U2b: the uPlot-renderer cell set ────────────────────────────────────────
 * The SAME deterministic states rendered through buildUplotSpec + a real
 * uPlot mount (gallery RENDERERS['uplot-aas']), so both renderers stay
 * eyeballable side by side while the ?renderer= seam exists. Object
 * references are shared with `states` — one data source, two renderers; a
 * fixture edit can never fork them. Deliberately small: dense class + event
 * stacks, the fidelity/honesty trio (sampled band, mixed + escalation band,
 * edge-only), and the tick replay (series-set churn drives the app's real
 * rebuild-instance path). */
export const uplotStates = {
    'dense': states['dense'],
    'dense-events': states['dense-events'],
    'sampled': states['sampled'],
    'mixed-escalation': states['mixed-escalation'],
    'escalated-live-edge': states['escalated-live-edge'],
    'live-ticks': states['live-ticks'],
    'compare-ghost-diff': COMPARE_STATE,
};
