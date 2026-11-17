/* pgwt — deterministic coverage-verdict fixtures (#947, #871 U3).
 *
 * The server's `coverage` object (spec-coverage-label r2 §2 Output with
 * Amendment A1) for every state the web UI distinguishes, plus one response
 * per view command in the shape the server's §3 consumers send it:
 * whole-window commands withheld as { code: 'coverage_unverified', coverage },
 * time-bucketed commands with the uncovered buckets null and the window
 * totals withheld, record-level commands with their records and the label.
 * Mirrors tests/mock_server.py's _COVERAGE_STATES (same relative ranges).
 * PURE DATA: see ./base.mjs for the determinism rules. Not a gallery
 * builder itself; ./fidelity.mjs, ./aas.mjs and ./histogram.mjs cite it.
 */

import { BASE_NS, SEC_NS, MIN_NS } from './base.mjs';

export const COV_FROM = BASE_NS;
export const COV_TO = BASE_NS + 60 * MIN_NS;
export const COV_GAP = { from: BASE_NS + 30 * MIN_NS, to: BASE_NS + 35 * MIN_NS };

function incomplete(reasons, ranges, restartCause) {
    const uncovered = ranges.map(([from, to, reason]) => ({ from, to, reason }));
    return {
        verdict: 'incomplete', reasons, covered_to: null, uncovered,
        restart_cause: restartCause || null, excludes_in_flight: false,
        idle: { verdict: 'incomplete', uncovered: uncovered.map(u => ({ ...u })) },
    };
}

const GAP = (reason) => [COV_GAP.from, COV_GAP.to, reason];

export const COVERAGE = {
    complete: {
        verdict: 'complete', reasons: [], covered_to: COV_TO, uncovered: [],
        restart_cause: null, excludes_in_flight: false,
        idle: { verdict: 'complete', uncovered: [] },
    },
    // Spec row 7b: live boot, last watermark 2 s before `to`, open waits.
    live_edge_xf: {
        verdict: 'live_edge', reasons: [], covered_to: COV_TO - 2 * SEC_NS,
        uncovered: [], restart_cause: null, excludes_in_flight: true,
        idle: { verdict: 'live_edge', uncovered: [] },
    },
    // Spec row 7a: a complete live window with in-flight time excluded.
    complete_xf: {
        verdict: 'complete', reasons: [], covered_to: COV_TO, uncovered: [],
        restart_cause: null, excludes_in_flight: true,
        idle: { verdict: 'complete', uncovered: [] },
    },
    restart_postgres: incomplete(['restart'], [GAP('restart')], 'postgres'),
    restart_daemon: incomplete(['restart'], [GAP('restart')], 'daemon'),
    restart_unknown: incomplete(['restart'], [GAP('restart')], 'unknown'),
    capture_loss: incomplete(['capture_loss'], [GAP('capture_loss')]),
    unclean_end: incomplete(['unclean_end'],
        [[COV_TO - 10 * MIN_NS, COV_TO, 'unclean_end']]),
    missing_file: incomplete(['missing_file'], [GAP('missing_file')]),
    retention: incomplete(['retention'],
        [[COV_FROM, COV_FROM + 10 * MIN_NS, 'retention']]),
    not_recorded: incomplete(['not_recorded'], [[COV_FROM, COV_TO, 'not_recorded']]),
    clock_step: incomplete(['clock_step'], [GAP('clock_step')]),
    // Spec row 3: two adjacent boots, no uncovered point (window-level rule).
    restart_no_gap: incomplete(['restart'], [], 'daemon'),
    // Two reasons in precedence order (A1 item 4).
    capture_loss_and_restart: incomplete(['capture_loss', 'restart'],
        [[COV_GAP.from, COV_GAP.from + MIN_NS, 'capture_loss'],
         [COV_GAP.from + MIN_NS, COV_GAP.to, 'restart']], 'postgres'),
    // Spec row 29: DB time complete, the idle floor far behind.
    idle_unverified: {
        verdict: 'complete', reasons: [], covered_to: COV_TO, uncovered: [],
        restart_cause: null, excludes_in_flight: false,
        idle: { verdict: 'incomplete', uncovered: [
            { from: BASE_NS + 50 * MIN_NS, to: COV_TO, reason: 'unclean_end' }] },
    },
};

export const INCOMPLETE_STATES = Object.keys(COVERAGE)
    .filter(k => COVERAGE[k].verdict === 'incomplete');

/* Every whole-window command the server withholds (spec §3 Withhold). */
export const WITHHELD_COMMANDS = ['time_model', 'top_events', 'top_sessions',
    'top_queries', 'transitions', 'fingerprints', 'lock_chains', 'interference',
    'concurrency', 'variants'];

export function withheld(state) {
    return { code: 'coverage_unverified', fidelity: 'incomplete', coverage: COVERAGE[state] };
}

function meets(t0, t1, ranges) {
    return ranges.some(r => r.from < t1 && r.to > t0);
}

/* AAS (class mode), 60 one-minute buckets of steady load; the buckets that
 * meet an uncovered range are null and max_aas is withheld. */
export function aasWindow(state) {
    const cov = COVERAGE[state];
    const holes = cov.uncovered.slice();
    if (cov.verdict === 'live_edge') holes.push({ from: cov.covered_to, to: COV_TO + MIN_NS });
    const buckets = [];
    for (let i = 0; i < 60; i++) {
        const t = BASE_NS + i * MIN_NS;
        if (meets(t, t + MIN_NS, holes)) {
            buckets.push({ t, cpu: null, io: null, lock: null, lwlock: null });
        } else {
            buckets.push({ t, cpu: 1.25, io: 0.5 + (i % 5) * 0.125, lock: (i % 7) * 0.0625, lwlock: 0.25 });
        }
    }
    const inc = cov.verdict === 'incomplete';
    return { bucket_ns: MIN_NS, max_aas: inc ? null : 2.625, buckets,
        fidelity: inc ? 'incomplete' : 'exact', coverage: cov };
}

const LAT_LABELS = ['<1', '1-2', '2-4', '4-8', '8-16', '16-32', '32-64',
    '64-128', '128-256', '256-512', '512-1K', '1K-2K',
    '2K-4K', '4K-8K', '8K-16K', '>=16K'];

/* Heatmap: 60 × 16; cells in buckets that meet an uncovered range absent;
 * max_count and total_events withheld while incomplete. */
export function heatmapWindow(state) {
    const cov = COVERAGE[state];
    const times = [];
    const cells = [];
    for (let i = 0; i < 60; i++) {
        const t = BASE_NS + i * MIN_NS;
        times.push(t);
        if (meets(t, t + MIN_NS, cov.uncovered)) continue;
        for (let j = 2; j < 9; j++) cells.push([i, j, 100 + ((i * 7 + j * 13) % 400)]);
    }
    const inc = cov.verdict === 'incomplete';
    return { bucket_ns: MIN_NS, max_count: inc ? null : 499,
        total_events: inc ? null : cells.reduce((a, c) => a + c[2], 0),
        times, labels: LAT_LABELS, cells,
        fidelity: inc ? 'incomplete' : 'exact', coverage: cov };
}

/* Time model with an Idle parent and two idle children; Idle rows null when
 * the idle verdict is unverified (the server's job; the UI masks too). */
export function timeModelWindow(state) {
    const cov = COVERAGE[state];
    if (cov.verdict === 'incomplete') return withheld(state);
    const idleNull = cov.idle.verdict === 'incomplete';
    const idle = (ms) => idleNull ? null : ms;
    return {
        fidelity: 'exact', coverage: cov, wall_ms: 3_600_000, db_time_ms: 12_500,
        idle_time_ms: idle(45_000),
        rows: [
            { indent: 0, name: 'DB Time', ms: 12_500, pct: 100, aas: 3.47 },
            { indent: 1, name: 'CPU*', ms: 7_000, pct: 56, aas: 1.94 },
            { indent: 1, name: 'IO', ms: 5_500, pct: 44, aas: 1.53 },
            { indent: 2, name: 'IO:DataFileRead', ms: 5_500, pct: 44, aas: 1.53 },
            { indent: 0, name: 'Idle', ms: idle(45_000), pct: null, aas: idle(12.5) },
            { indent: 2, name: 'Client:ClientRead', ms: idle(40_000), pct: null, aas: idle(11.1) },
            { indent: 2, name: 'Activity:CheckpointerMain', ms: idle(5_000), pct: null, aas: idle(1.39) },
        ],
    };
}

/* Top events with one idle-but-visible row (pct null). */
export function topEventsWindow(state) {
    const cov = COVERAGE[state];
    if (cov.verdict === 'incomplete') return withheld(state);
    const idleNull = cov.idle.verdict === 'incomplete';
    const idle = (v) => idleNull ? null : v;
    return {
        fidelity: 'exact', coverage: cov, db_time_ms: 12_500, has_measured_cpu: false,
        rows: [
            { name: 'IO:DataFileRead', class: 'IO', event_id: 167772161, count: 9000,
              total_ms: 5_500, avg_us: 611, p50_us: 512, p95_us: 2048, p99_us: 4096,
              max_us: 9000, p50_overflow: false, p95_overflow: false, p99_overflow: false,
              pct: 44, aas: 1.53 },
            { name: 'Client:ClientRead', class: 'Client', event_id: 100663296,
              count: idle(400), total_ms: idle(40_000), avg_us: idle(100_000),
              p50_us: idle(65_536), p95_us: idle(262_144), p99_us: idle(524_288),
              max_us: idle(900_000), p50_overflow: false, p95_overflow: false,
              p99_overflow: false, pct: null, aas: idle(11.1) },
        ],
    };
}

/* Record-level responses keep their records and carry the label. */
export function sessionTimelineWindow(state) {
    const cov = COVERAGE[state];
    return {
        fidelity: cov.verdict === 'incomplete' ? 'incomplete' : 'exact', coverage: cov,
        pids: [1001], truncated: false, total_count: 1,
        events: [{ s: BASE_NS + 5 * MIN_NS, d: 2 * SEC_NS, p: 1001,
            n: 'IO:DataFileRead', e: 0x01000015, c: 1, q: '42' }],
    };
}

export function execScatterWindow(state) {
    const cov = COVERAGE[state];
    return {
        fidelity: cov.verdict === 'incomplete' ? 'incomplete' : 'exact', coverage: cov,
        points: [{ t: String(BigInt(BASE_NS) + BigInt(30 * SEC_NS)), duration_ms: 12.5,
            pid: 5001, query_id: '701' }],
        total_count: 1, kept_count: 1, downsampled: false,
    };
}
