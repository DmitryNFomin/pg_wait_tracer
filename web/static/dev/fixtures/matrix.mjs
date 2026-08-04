/* Deterministic transition-matrix fixtures. Structured refusal is view-level
 * (panels.js) and intentionally N/A for the pure builder gallery. */

const CLASSES = ['CPU', 'IO', 'Lock', 'LWLock', 'IPC', 'Client', 'Timeout'];
const PREFIX = ['CPU', 'IO', 'Lock', 'LWLock', 'IPC', 'Client', 'Timeout'];

function denseMatrix() {
    const nodes = [];
    for (let i = 0; i < 24; i++) {
        const cls = CLASSES[i % CLASSES.length];
        nodes.push({ name: (i === 0 ? 'CPU*' : PREFIX[i % PREFIX.length] + ':Event' + i),
            total_ms: 100 + (24 - i) * 37, class: cls, event_id: i === 0 ? 0 : i });
    }
    const links = [];
    let total = 0;
    for (let i = 0; i < 24; i++) {
        for (let k = 1; k <= 3; k++) {
            const value = (24 - i) * (5 - k) + ((i + k) % 7);
            links.push({ source: nodes[i].name, target: nodes[(i + k * 5) % 24].name,
                value, duration_ms: value * (k + 1) * 0.7 });
            total += value;
        }
    }
    return { total, nodes, links };
}

export const states = {
    'empty': {
        description: 'No links — explicit empty matrix; refusal is a separate view-level panel.',
        tags: ['FEEDBACK'], data: { total: 0, nodes: [], links: [] }, opts: { limit: 20 },
    },
    'single-cell': {
        description: 'One directed CPU→IO cell at the minimum matrix shape.',
        tags: ['ALIGNMENT'],
        data: { total: 42, nodes: [
            { name: 'CPU*', class: 'CPU', event_id: 0, total_ms: 20 },
            { name: 'IO:DataFileRead', class: 'IO', event_id: 1, total_ms: 22 },
        ], links: [{ source: 'CPU*', target: 'IO:DataFileRead', value: 42,
            duration_ms: 123.4 }] }, opts: { limit: 20 },
    },
    'dense-top20': {
        description: '24 events collapse honestly to top 20 by incident link count; log-piecewise violet cells stay distinct from identity-colored labels.',
        tags: ['SEMANTICS', 'OCCLUSION', 'HIERARCHY', 'FEEDBACK'],
        data: denseMatrix(), opts: { limit: 20 },
    },
};
