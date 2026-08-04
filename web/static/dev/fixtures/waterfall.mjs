/* Deterministic execution_detail fixtures for builders/waterfall.js.
 * Structured full_fidelity_required refusal is intentionally N/A here: it is
 * a view-level panels.js state, already covered by fidelity fixtures/UI tests. */

import { BASE_NS, SEC_NS } from './base.mjs';

const QID = '3886912043147135675';
const s = (n) => String(BigInt(BASE_NS) + BigInt(n));

function ev(i, lane) {
    const defs = [
        [0, 'CPU*'], [0x01000015, 'IO:DataFileRead'],
        [0x04000007, 'LWLock:WALInsert'], [0x03000001, 'Lock:relation'],
        [0x06000000, 'Client:ClientRead'],
    ];
    const d = defs[(i + lane) % defs.length];
    const start = (lane * 7 + i * 13) * 11_000_000;
    const dur = ((i * 17 + lane * 5) % 23 + 1) * 900_000;
    return { we: d[0], name: d[1], start_ns: s(start), dur_ns: String(dur),
        cpu_ns: i % 3 === 0 ? String(Math.floor(dur * 3 / 5)) : null };
}

function events(n, lane) {
    return Array.from({ length: n }, (_, i) => ev(i, lane));
}

const DENSE = {
    query_id: QID,
    leader: { pid: 4100, query_id: QID, events: events(24, 0),
        total_count: 24, truncated: false },
    workers: [
        { pid: 4106, events: events(18, 1), total_count: 18, truncated: false },
        { pid: 4108, events: events(20, 2), total_count: 20, truncated: false },
    ],
    plan: { start_ns: s(-35_000_000), end_ns: s(-5_000_000) },
    total_count: 62, kept_count: 62, truncated: false,
};

const EXEC_END = s(6 * SEC_NS);

export const states = {
    'empty': {
        description: 'Execution marker exists but no wait/CPU spans were retained; refusal is view-level, not fabricated here.',
        tags: ['FEEDBACK'],
        data: { query_id: QID,
            leader: { pid: 4100, query_id: QID, events: [], total_count: 0, truncated: false },
            workers: [], plan: null, total_count: 0, kept_count: 0, truncated: false },
        opts: { executionStart: s(0), executionEnd: EXEC_END },
    },
    'single-point': {
        description: 'One leader lane and one 5 µs event: the custom-series 1 px floor remains visible.',
        tags: ['ALIGNMENT'],
        data: { query_id: QID,
            leader: { pid: 4100, query_id: QID, events: [
                { we: 0x04000007, name: 'LWLock:WALInsert', start_ns: s(SEC_NS),
                  dur_ns: '5000', cpu_ns: null },
            ], total_count: 1, truncated: false },
            workers: [], plan: null, total_count: 1, kept_count: 1, truncated: false },
        opts: { executionStart: s(0), executionEnd: EXEC_END },
    },
    'dense-plan-3lanes': {
        description: 'Leader + two workers (3 lanes), 62 spans, measured CPU on a subset, and a pre-execution plan band.',
        tags: ['OCCLUSION', 'SEMANTICS'],
        data: DENSE,
        opts: { executionStart: s(0), executionEnd: EXEC_END },
        executions: { rows: [{ pid: 4100, query_id: QID, start_ns: s(0),
            end_ns: EXEC_END, duration_ms: 6000, plan_ms: 30, n_events: 62,
            n_workers: 2, in_progress: false }], total_count: 1, truncated: false },
    },
    'in-progress-no-plan': {
        description: 'Open execution has no fabricated end/duration in its selector row; detail is bounded by the loaded window, with no plan band.',
        tags: ['FEEDBACK'],
        data: { query_id: QID,
            leader: { pid: 4200, query_id: QID, events: events(7, 0),
                total_count: 7, truncated: false },
            workers: [], plan: null, total_count: 7, kept_count: 7, truncated: false },
        opts: { executionStart: s(0), executionEnd: EXEC_END },
        executions: { rows: [{ pid: 4200, query_id: QID, start_ns: s(0),
            end_ns: null, duration_ms: null, plan_ms: null, n_events: 7,
            n_workers: 0, in_progress: true }], total_count: 1, truncated: false },
    },
    'truncated': {
        description: 'Payload states 12 of 80 spans retained and one lane states 4 of 51; the UI repeats only those server counts.',
        tags: ['FEEDBACK'],
        data: { query_id: QID,
            leader: { pid: 4300, query_id: QID, events: events(8, 0),
                total_count: 29, truncated: true },
            workers: [{ pid: 4306, events: events(4, 1), total_count: 51, truncated: true }],
            plan: { start_ns: s(-20_000_000), end_ns: s(-2_000_000) },
            total_count: 80, kept_count: 12, truncated: true },
        opts: { executionStart: s(0), executionEnd: EXEC_END },
        executions: { rows: [], total_count: 140, truncated: true },
    },
};
