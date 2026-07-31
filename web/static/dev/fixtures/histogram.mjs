/* pgwt — deterministic fixtures for the latency heatmap builder
 * (lib/builders/histogram.js). See ./aas.mjs for the directory-wide contract.
 *
 * Input shape (heatmap response, mirrors tests/mock_server.py): { bucket_ns,
 * max_count, times: [ns...], labels: [latency bands], cells: [[timeIdx,
 * latIdx, count], ...] }.
 */

import { BASE_NS, MIN_NS } from './base.mjs';

/* The server's 16 log2 latency bands (µs), verbatim from the mock. */
const LAT_LABELS = ['<1', '1-2', '2-4', '4-8', '8-16', '16-32', '32-64',
    '64-128', '128-256', '256-512', '512-1K', '1K-2K',
    '2K-4K', '4K-8K', '8K-16K', '>=16K'];

function times(n) {
    const out = [];
    for (let i = 0; i < n; i++) out.push(BASE_NS + i * MIN_NS);
    return out;
}

/* Full 60×16 grid: a latency ridge that drifts upward over time (the
 * distribution-shift question this EXACT-tier view exists to answer). */
function denseData() {
    const cells = [];
    let maxCount = 0;
    let total = 0;
    for (let i = 0; i < 60; i++) {
        const ridge = 3 + (((i / 10) | 0) % 4);   // band 3 → 6 and back
        for (let j = 0; j < 16; j++) {
            const count = Math.max(0, 4000 - Math.abs(j - ridge) * 900
                - Math.abs(i - 30) * 40);
            if (count > 0) {
                cells.push([i, j, count]);
                maxCount = Math.max(maxCount, count);
                total += count;
            }
        }
    }
    return { bucket_ns: MIN_NS, max_count: maxCount, total_events: total,
        times: times(60), labels: LAT_LABELS, cells };
}

/* One cell at 50 000 over a uniform ≤16 floor: the P8 killer — a linear ramp
 * collapses every other cell into the darkest stops. */
function oneHotCellData() {
    const cells = [];
    let total = 0;
    for (let i = 0; i < 60; i++) {
        for (let j = 0; j < 16; j++) {
            const count = (i === 30 && j === 12) ? 50_000 : 5 + ((i + j) % 12);
            cells.push([i, j, count]);
            total += count;
        }
    }
    return { bucket_ns: MIN_NS, max_count: 50_000, total_events: total,
        times: times(60), labels: LAT_LABELS, cells };
}

export const states = {
    'empty': {
        description: 'No cells — hasData false; the view paints "No data for selected event/range".',
        tags: ['FEEDBACK'],
        data: { bucket_ns: MIN_NS, max_count: 0, total_events: 0,
            times: [], labels: LAT_LABELS, cells: [] },
    },
    'single-cell': {
        description: 'One cell in one time bucket — visualMap and axes at minimum data.',
        tags: ['ALIGNMENT'],
        data: { bucket_ns: MIN_NS, max_count: 42, total_events: 42,
            times: times(1), labels: LAT_LABELS, cells: [[0, 4, 42]] },
    },
    'dense': {
        description: 'Full 60×16 grid with a latency ridge drifting bands 3→6 over the hour.',
        tags: ['SEMANTICS', 'OCCLUSION'],
        data: denseData(),
    },
    'one-hot-cell': {
        description: 'One 50 000-count cell over a ≤16 floor: heavy-tail worst case for the linear rainbow ramp (P8).',
        tags: ['SEMANTICS', 'HIERARCHY'],
        data: oneHotCellData(),
    },
};
