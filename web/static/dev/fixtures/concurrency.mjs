/* pgwt — deterministic fixtures for the concurrency builders
 * (lib/builders/concurrency.js). See ./aas.mjs for the directory-wide contract.
 *
 * Input shape (concurrency response, mirrors tests/mock_server.py):
 * { bucket_ns, peaks: [{t, t_ms, max, event}], bursts: [{timestamp_ns,
 * timestamp_ms, event, sessions, pids}] }. Peak t values are bucket STARTS
 * (server.c) — the containing-bucket burst arithmetic depends on that.
 */

import { BASE_NS, SEC_NS } from './base.mjs';

const B12 = 12 * SEC_NS;

function peak(i, bucketNs, max, event) {
    const t = BASE_NS + i * bucketNs;
    return { t, t_ms: Math.floor(t / 1e6), max, event };
}

function burst(ns, event, sessions, pids) {
    return { timestamp_ns: ns, timestamp_ms: Math.floor(ns / 1e6),
        event, sessions, pids };
}

/* 300 buckets with a storm spike at bucket 150 and a live-edge spike in the
 * FINAL bucket — the marker for the final-bucket burst used to clamp to
 * bucket 0 (P6, fixed in U0). */
function denseData() {
    const peaks = [];
    for (let i = 0; i < 300; i++) {
        let max = 1 + (i % 4);
        let event = ['LWLock:BufferMapping', 'IO:DataFileRead',
            'Lock:transactionid', 'LWLock:WALInsert'][i % 4];
        if (i === 150) { max = 9; event = 'LWLock:BufferMapping'; }
        if (i === 299) { max = 6; event = 'Lock:transactionid'; }
        peaks.push(peak(i, B12, max, event));
    }
    const bursts = [
        burst(BASE_NS + 30 * B12 + 2 * SEC_NS, 'IO:DataFileRead', 4,
            [1001, 1003, 1005, 1007]),
        burst(BASE_NS + 150 * B12 + 3 * SEC_NS, 'LWLock:BufferMapping', 8,
            [1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010]),
        // Inside the FINAL bucket: the P6 regression case, at scale.
        burst(BASE_NS + 299 * B12 + 9 * SEC_NS, 'Lock:transactionid', 6,
            [1001, 1002, 1003, 1004, 1005, 1006]),
    ];
    return { bucket_ns: B12, peaks, bursts };
}

export const states = {
    'empty': {
        description: 'No peaks — hasData false, empty tables; must not render a dead chart.',
        tags: ['FEEDBACK'],
        data: { bucket_ns: B12, peaks: [], bursts: [] },
    },
    'single-point': {
        description: 'One bucket, one burst inside it — line degenerates to a point, marker must sit on it.',
        tags: ['ALIGNMENT'],
        data: {
            bucket_ns: B12,
            peaks: [peak(0, B12, 5, 'LWLock:BufferMapping')],
            bursts: [burst(BASE_NS + 4 * SEC_NS, 'LWLock:BufferMapping', 5,
                [1001, 1002, 1003, 1004, 1005])],
        },
    },
    'dense-bursts': {
        description: '300 buckets with bursts early, mid-storm and in the FINAL bucket — the last one clamped to bucket 0 before U0 (P6).',
        tags: ['ALIGNMENT', 'OCCLUSION'],
        data: denseData(),
    },
    'burst-final-bucket': {
        description: 'Minimal P6 pin: 5 buckets, the only burst inside the last one. Marker belongs at the far RIGHT.',
        tags: ['ALIGNMENT'],
        data: {
            bucket_ns: B12,
            peaks: [
                peak(0, B12, 2, 'IO:DataFileRead'),
                peak(1, B12, 3, 'IO:DataFileRead'),
                peak(2, B12, 2, 'LWLock:WALInsert'),
                peak(3, B12, 3, 'IO:DataFileRead'),
                peak(4, B12, 5, 'Lock:transactionid'),
            ],
            bursts: [burst(BASE_NS + 4 * B12 + 9 * SEC_NS,
                'Lock:transactionid', 4, [1001, 1002, 1003, 1004])],
        },
    },
    'one-huge-peak': {
        description: 'One 400-session peak over a 1–3 floor: y-axis squashes every normal bucket flat.',
        tags: ['HIERARCHY'],
        data: {
            bucket_ns: B12,
            peaks: (() => {
                const out = [];
                for (let i = 0; i < 60; i++) {
                    out.push(peak(i, B12, i === 30 ? 400 : 1 + (i % 3),
                        i === 30 ? 'Lock:transactionid' : 'IO:DataFileRead'));
                }
                return out;
            })(),
            bursts: [],
        },
    },
    'hostile-event-names': {
        description: 'Peak/burst event names with <script>/unicode payloads — chart tooltip and both tables must escape (UI-6).',
        tags: ['FEEDBACK', 'injection'],
        data: {
            bucket_ns: B12,
            peaks: [
                peak(0, B12, 4, 'Lock:<script>document.title="pwned"</script>'),
                peak(1, B12, 6, 'Extension:日本語イベント🐧'),
                peak(2, B12, 3, 'IO:"><img src=x onerror=alert(1)>'),
            ],
            bursts: [burst(BASE_NS + SEC_NS, 'Lock:<script>document.title="pwned"</script>', 5,
                [1001, 1002, 1003, 1004, 1005])],
        },
    },
};
