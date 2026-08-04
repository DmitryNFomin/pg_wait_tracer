/* Deterministic exec_scatter fixtures. Refusal is a view-level panels.js
 * state and therefore deliberately N/A in this pure-builder module. */

import { BASE_NS, SEC_NS } from './base.mjs';

const t = (n) => String(BigInt(BASE_NS) + BigInt(n));

function densePoints() {
    const out = [];
    for (let i = 0; i < 360; i++) {
        const band = i % 3;
        out.push({ t: t(i * 10 * SEC_NS),
            duration_ms: band === 0 ? 0.08 + (i % 7) * 0.02
                : band === 1 ? 4 + (i % 11) * 0.7 : 120 + (i % 13) * 18,
            pid: 5000 + (i % 24), query_id: String(700 + (i % 5)) });
    }
    return out;
}

export const states = {
    'empty': {
        description: 'No executions — explicit empty state.', tags: ['FEEDBACK'],
        data: { points: [], total_count: 0, kept_count: 0, downsampled: false },
    },
    'single-point': {
        description: 'One completed execution on time/log axes.', tags: ['ALIGNMENT'],
        data: { points: [{ t: t(30 * SEC_NS), duration_ms: 12.5,
            pid: 5001, query_id: '701' }], total_count: 1, kept_count: 1,
            downsampled: false },
    },
    'in-progress-excluded': {
        description: 'One open null-duration execution is excluded from the log axis beside one completed point; the note says exactly one.',
        tags: ['FEEDBACK'],
        data: { points: [
            { t: t(10 * SEC_NS), duration_ms: null, pid: 5001, query_id: '701' },
            { t: t(20 * SEC_NS), duration_ms: 2.4, pid: 5002, query_id: '702' },
        ], total_count: 2, kept_count: 2, downsampled: false },
    },
    'dense-downsampled': {
        description: '360 retained points across three latency modes; payload honestly says 360 of 9,000 after server downsampling.',
        tags: ['SEMANTICS', 'OCCLUSION', 'FEEDBACK'],
        data: { points: densePoints(), total_count: 9000, kept_count: 360,
            downsampled: true },
    },
};
