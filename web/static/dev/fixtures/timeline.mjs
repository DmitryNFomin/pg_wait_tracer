/* pgwt — deterministic fixtures for the session-timeline builder
 * (lib/builders/timeline.js). See ./aas.mjs for the directory-wide contract.
 *
 * Input shape (session_timeline response): { events: [{s, d, p, n, e, c, q}],
 * pids: [...], truncated, total_count }; opts = { from, to } (view window).
 * Class index c indexes WAIT_CLASSES (0=cpu 1=io 2=lock 3=lwlock 4=ipc
 * 5=client 6=timeout 7=bufferpin 8=activity 9=extension 10=unknown).
 */

import { BASE_NS, SEC_NS, MIN_NS } from './base.mjs';

/* A representative event name per class index (c ↔ n must stay consistent). */
const CLASS_EVENTS = [
    { c: 0,  n: 'CPU*',                    e: 0x00000000 },
    { c: 1,  n: 'IO:DataFileRead',         e: 0x01000015 },
    { c: 2,  n: 'Lock:relation',           e: 0x03000000 },
    { c: 3,  n: 'LWLock:WALInsert',        e: 0x04000007 },
    { c: 4,  n: 'IPC:BufferIO',            e: 0x05000001 },
    { c: 5,  n: 'Client:ClientRead',       e: 0x06000000 },
    { c: 6,  n: 'Timeout:VacuumDelay',     e: 0x09000005 },
    { c: 7,  n: 'BufferPin:BufferPin',     e: 0x07000000 },
    { c: 8,  n: 'Activity:CheckpointerMain', e: 0x02000001 },
    { c: 9,  n: 'Extension:Extension',     e: 0x0a000000 },
    { c: 10, n: 'Unknown:Unknown',         e: 0x0b000000 },
];

const WIN5 = { from: BASE_NS, to: BASE_NS + 5 * MIN_NS };

/* 50 PIDs × 8 waits each = 400 bars over a 5-minute window; classes, offsets
 * and durations are pure functions of (pid index, wait index). */
function dense50Pids() {
    const pids = [];
    const events = [];
    for (let pi = 0; pi < 50; pi++) {
        const pid = 4000 + pi;
        pids.push(pid);
        for (let j = 0; j < 8; j++) {
            const cls = CLASS_EVENTS[(pi + j * 3) % 11];
            const startOff = ((pi * 37 + j * 613) % 280) * SEC_NS;
            const dur = (((pi * 7 + j * 13) % 40) + 1) * 250_000_000; // 0.25–10 s
            events.push({
                s: BASE_NS + startOff, d: dur, p: pid,
                n: cls.n, e: cls.e, c: cls.c,
                q: String(3886912043147135675n + BigInt((pi * 8 + j) % 5)),
            });
        }
    }
    return { truncated: true, total_count: 1200, pids, events };
}

/* The #106 shape, from the live UI smoke: ONE PID row carrying 3036 spans over
 * a 60 s window — ~57 % Timeout, ~43 % CPU. Drawn per span they fused into a
 * solid block that alpha-composited to rgb(181,199,48), a yellow-green in no
 * palette; bucketed per pixel column they are a stack of the two real class
 * colors, with the density banner saying so. Pure integer arithmetic only
 * (see base.mjs). */
function denseOneRow() {
    const events = [];
    const n = 3036;
    for (let i = 0; i < n; i++) {
        // 4 of every 7 spans are Timeout (57.1 %), 3 are CPU (42.9 %).
        const cls = (i % 7) < 4 ? CLASS_EVENTS[6] : CLASS_EVENTS[0];
        // ~19.7 ms apart with a deterministic 0–6 ms wobble, so columns carry
        // uneven counts instead of a perfect comb.
        const s = BASE_NS + i * 19_762_845 + ((i * 7919) % 6_000_000);
        const d = 4_000_000 + ((i * 104_729) % 26_000_000);   // 4–30 ms
        events.push({ s, d, p: 9001, n: cls.n, e: cls.e, c: cls.c,
            q: '3886912043147135675' });
    }
    return { truncated: true, total_count: 9812, pids: [9001], events };
}

export const states = {
    'empty': {
        description: 'No events — the view paints its placeholder; hasData must be false.',
        tags: ['FEEDBACK'],
        data: { events: [], pids: [], truncated: false, total_count: 0 },
        opts: WIN5,
    },
    'single-point': {
        description: 'One PID, one wait bar in the middle of the window.',
        tags: ['ALIGNMENT'],
        data: {
            truncated: false, total_count: 1, pids: [1001],
            events: [{ s: BASE_NS + 2 * MIN_NS, d: 30 * SEC_NS, p: 1001,
                n: 'IO:DataFileRead', e: 0x01000015, c: 1, q: '3886912043147135675' }],
        },
        opts: WIN5,
    },
    'dense-50pids': {
        description: '50 PIDs × 8 waits (400 bars, truncated from 1200): row labels, band packing and the truncation flags at density.',
        tags: ['OCCLUSION', 'FEEDBACK'],
        data: dense50Pids(),
        opts: WIN5,
    },
    'dense-one-row': {
        description: '3036 spans on ONE PID row over 60 s (#106): above 2 spans per painted px the builder buckets per pixel column into class-share stacks — every fill a real palette color, with the banner stating the density and what zooming restores. Drawn per span these fused into one block whose composited pixels named no class.',
        tags: ['OCCLUSION', 'SEMANTICS', 'FEEDBACK'],
        data: denseOneRow(),
        // No width here: the gallery renderer passes the cell's real canvas
        // width, because the aggregation buckets into PIXEL columns.
        opts: { from: BASE_NS, to: BASE_NS + 60 * SEC_NS },
    },
    'pre-window-start': {
        description: 'Long waits that began BEFORE the window: drawn bars clamp to the left edge (P6), tooltips keep the raw start.',
        tags: ['ALIGNMENT'],
        data: {
            truncated: false, total_count: 3, pids: [1001, 1002],
            events: [
                // Started 10 min before the window, ends inside it.
                { s: BASE_NS - 10 * MIN_NS, d: 12 * MIN_NS, p: 1001,
                  n: 'Lock:relation', e: 0x03000000, c: 2, q: '5371305355164922084' },
                // Started before, ends after: spans the whole window.
                { s: BASE_NS - MIN_NS, d: 8 * MIN_NS, p: 1002,
                  n: 'Client:ClientRead', e: 0x06000000, c: 5, q: '0' },
                // Fully inside, for contrast.
                { s: BASE_NS + 3 * MIN_NS, d: 40 * SEC_NS, p: 1002,
                  n: 'CPU*', e: 0, c: 0, q: '5371305355164922084' },
            ],
        },
        opts: WIN5,
    },
    'micro-vs-macro': {
        description: 'A 5 µs wait next to a 5 s wait on one row: the 1 px floor renders both — µs must not read as ms (P11).',
        tags: ['SEMANTICS'],
        data: {
            truncated: false, total_count: 3, pids: [1001],
            events: [
                { s: BASE_NS + MIN_NS, d: 5_000, p: 1001,
                  n: 'LWLock:WALInsert', e: 0x04000007, c: 3, q: '42' },
                { s: BASE_NS + 2 * MIN_NS, d: 5 * SEC_NS, p: 1001,
                  n: 'IO:DataFileRead', e: 0x01000015, c: 1, q: '42' },
                { s: BASE_NS + 4 * MIN_NS, d: 5_000_000, p: 1001,   // 5 ms
                  n: 'LWLock:WALInsert', e: 0x04000007, c: 3, q: '42' },
            ],
        },
        opts: WIN5,
    },
    'hostile-sql': {
        description: 'Query field carrying <script>/quote payloads and a unicode event name — tooltips must escape (UI-6).',
        tags: ['FEEDBACK', 'injection'],
        data: {
            truncated: false, total_count: 2, pids: [666],
            events: [
                { s: BASE_NS + MIN_NS, d: 20 * SEC_NS, p: 666,
                  n: 'Lock:relation', e: 0x03000000, c: 2,
                  q: '<script>document.title="pwned"</script> UNION SELECT "a\'b" -- ' },
                { s: BASE_NS + 3 * MIN_NS, d: 15 * SEC_NS, p: 666,
                  n: 'Extension:日本語イベント🐧', e: 0x0a000102, c: 9,
                  q: "'; DROP TABLE pgbench_accounts; --" },
            ],
        },
        opts: WIN5,
    },
};
