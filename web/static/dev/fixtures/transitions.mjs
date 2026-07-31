/* pgwt — deterministic fixtures for the transitions builders
 * (lib/builders/transitions.js). See ./aas.mjs for the directory-wide contract.
 *
 * DFG states carry the builder's positional args as { data, opts: { threshold,
 * dims } } (buildTransitionsOption(data, threshold, dims)). The variants state
 * instead carries { variants } for buildVariantsHtml — the gallery adapter
 * dispatches on it.
 *
 * Input shape (transitions response): { total, nodes: [{name, total_ms,
 * class}], links: [{source, target, value, duration_ms}] }.
 */

const DIMS = { width: 620, height: 460 };

/* 18 nodes across 9 classes, 27 links incl. self-loops — a rich DFG. */
function denseDfg() {
    const nodes = [
        { name: 'CPU*',                    total_ms: 5200, class: 'CPU' },
        { name: 'Client:ClientRead',       total_ms: 2600, class: 'Client' },
        { name: 'IO:DataFileRead',         total_ms: 2100, class: 'IO' },
        { name: 'Lock:transactionid',      total_ms: 1500, class: 'Lock' },
        { name: 'LWLock:WALInsert',        total_ms: 900,  class: 'LWLock' },
        { name: 'IO:WalSync',              total_ms: 800,  class: 'IO' },
        { name: 'LWLock:WALWrite',         total_ms: 640,  class: 'LWLock' },
        { name: 'Lock:tuple',              total_ms: 480,  class: 'Lock' },
        { name: 'LWLock:BufferMapping',    total_ms: 420,  class: 'LWLock' },
        { name: 'IO:WalWrite',             total_ms: 350,  class: 'IO' },
        { name: 'Timeout:VacuumDelay',     total_ms: 300,  class: 'Timeout' },
        { name: 'Lock:relation',           total_ms: 260,  class: 'Lock' },
        { name: 'IO:DataFileExtend',       total_ms: 240,  class: 'IO' },
        { name: 'LWLock:LockManager',      total_ms: 210,  class: 'LWLock' },
        { name: 'IPC:BufferIO',            total_ms: 190,  class: 'IPC' },
        { name: 'Extension:Extension',     total_ms: 150,  class: 'Extension' },
        { name: 'IPC:MessageQueueReceive', total_ms: 120,  class: 'IPC' },
        { name: 'BufferPin:BufferPin',     total_ms: 90,   class: 'BufferPin' },
    ];
    const links = [
        { source: 'CPU*', target: 'IO:DataFileRead',        value: 1400, duration_ms: 2100 },
        { source: 'IO:DataFileRead', target: 'CPU*',        value: 1350, duration_ms: 5100 },
        { source: 'CPU*', target: 'LWLock:WALInsert',       value: 800,  duration_ms: 900 },
        { source: 'LWLock:WALInsert', target: 'CPU*',       value: 780,  duration_ms: 2900 },
        { source: 'CPU*', target: 'IO:WalSync',             value: 400,  duration_ms: 800 },
        { source: 'IO:WalSync', target: 'CPU*',             value: 390,  duration_ms: 1500 },
        { source: 'CPU*', target: 'Client:ClientRead',      value: 950,  duration_ms: 2600 },
        { source: 'Client:ClientRead', target: 'CPU*',      value: 940,  duration_ms: 3600 },
        { source: 'CPU*', target: 'Lock:transactionid',     value: 220,  duration_ms: 1500 },
        { source: 'Lock:transactionid', target: 'CPU*',     value: 200,  duration_ms: 820 },
        { source: 'Lock:transactionid', target: 'LWLock:LockManager', value: 90, duration_ms: 60 },
        { source: 'LWLock:LockManager', target: 'CPU*',     value: 88,   duration_ms: 340 },
        { source: 'CPU*', target: 'Lock:tuple',             value: 130,  duration_ms: 480 },
        { source: 'Lock:tuple', target: 'CPU*',             value: 128,  duration_ms: 500 },
        { source: 'CPU*', target: 'LWLock:WALWrite',        value: 310,  duration_ms: 640 },
        { source: 'LWLock:WALWrite', target: 'IO:WalWrite', value: 160,  duration_ms: 350 },
        { source: 'IO:WalWrite', target: 'CPU*',            value: 155,  duration_ms: 620 },
        { source: 'CPU*', target: 'LWLock:BufferMapping',   value: 240,  duration_ms: 420 },
        { source: 'LWLock:BufferMapping', target: 'IPC:BufferIO', value: 110, duration_ms: 95 },
        { source: 'IPC:BufferIO', target: 'CPU*',           value: 105,  duration_ms: 380 },
        { source: 'CPU*', target: 'IO:DataFileExtend',      value: 70,   duration_ms: 240 },
        { source: 'IO:DataFileExtend', target: 'CPU*',      value: 68,   duration_ms: 280 },
        { source: 'CPU*', target: 'Timeout:VacuumDelay',    value: 40,   duration_ms: 300 },
        { source: 'Timeout:VacuumDelay', target: 'CPU*',    value: 38,   duration_ms: 190 },
        { source: 'CPU*', target: 'Lock:relation',          value: 55,   duration_ms: 260 },
        { source: 'Lock:relation', target: 'CPU*',          value: 54,   duration_ms: 230 },
        { source: 'CPU*', target: 'Extension:Extension',    value: 30,   duration_ms: 150 },
        { source: 'Extension:Extension', target: 'CPU*',    value: 29,   duration_ms: 140 },
        { source: 'CPU*', target: 'IPC:MessageQueueReceive', value: 25,  duration_ms: 120 },
        { source: 'IPC:MessageQueueReceive', target: 'CPU*', value: 24,  duration_ms: 110 },
        { source: 'CPU*', target: 'BufferPin:BufferPin',    value: 12,   duration_ms: 90 },
        { source: 'BufferPin:BufferPin', target: 'CPU*',    value: 11,   duration_ms: 85 },
        // Self-loops (curve harder in the builder).
        { source: 'CPU*', target: 'CPU*',                   value: 300,  duration_ms: 700 },
        { source: 'IO:DataFileRead', target: 'IO:DataFileRead', value: 85, duration_ms: 160 },
    ];
    let total = 0;
    for (const l of links) total += l.value;
    return { total, nodes, links };
}

const DENSE = denseDfg();

/* Mirrors tests/mock_server.py's canned variants payload, plus a hostile
 * query_text (it lands in a title attribute — same escape class as UI-6). */
const VARIANTS = {
    exec: {
        total: 45000,
        num_variants: 3,
        variants: [
            { exec_count: 30000, num_queries: 1, total_ms: 2790.0,
              avg_ms: 0.093, p95_ms: 0.30, avg_loop_n: 1,
              top_query_id: 3886912043147135675,
              steps: [
                  { name: 'CPU*', avg_ms: 0.04, class: 'cpu' },
                  { name: 'IO:DataFileRead', avg_ms: 0.03, class: 'IO' },
                  { name: 'CPU*', avg_ms: 0.023, class: 'cpu' },
              ],
              query_text: 'UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2' },
            { exec_count: 15000, num_queries: 1, total_ms: 1360.0,
              avg_ms: 0.09, p95_ms: 0.28, avg_loop_n: 4,
              top_query_id: 5371305355164922084,
              steps: [
                  { name: 'CPU*', avg_ms: 0.05, class: 'cpu' },
                  { name: 'LWLock:WALWrite', avg_ms: 0.04, class: 'LWLock', loop: true },
              ],
              query_text: 'SELECT abalance FROM pgbench_accounts WHERE aid = $1' },
            { exec_count: 120, num_queries: 1, total_ms: 900.0,
              avg_ms: 7.5, p95_ms: 21.0, avg_loop_n: 1,
              top_query_id: 6660000000000000666,
              steps: [
                  { name: 'CPU*', avg_ms: 0.5, class: 'cpu' },
                  { name: 'Lock:relation', avg_ms: 7.0, class: 'Lock' },
              ],
              query_text: 'SELECT \'<script>document.title="pwned"</script>\' FROM "users" WHERE note = \'a&b<c>\' -- 日本語' },
        ],
    },
    plan: {
        total: 45000,
        num_variants: 1,
        variants: [
            { exec_count: 45000, num_queries: 2, total_ms: 495.0,
              avg_ms: 0.011, p95_ms: 0.04, avg_loop_n: 1,
              top_query_id: 3886912043147135675,
              steps: [
                  { name: 'CPU*', avg_ms: 0.011, class: 'cpu' },
              ],
              query_text: 'UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2' },
        ],
    },
};

export const states = {
    'empty': {
        description: 'No nodes/links — option null, visibleCount 0; the view says so instead of a blank pane.',
        tags: ['FEEDBACK'],
        data: { total: 0, nodes: [], links: [] },
        opts: { threshold: 0, dims: DIMS },
    },
    'single-self-loop': {
        description: 'One node whose only transition is a self-loop — the curveness=0.8 special case alone.',
        tags: ['ALIGNMENT'],
        data: {
            total: 120,
            nodes: [{ name: 'CPU*', total_ms: 4800, class: 'CPU' }],
            links: [{ source: 'CPU*', target: 'CPU*', value: 120, duration_ms: 700 }],
        },
        opts: { threshold: 0, dims: DIMS },
    },
    'dense-dfg': {
        description: '18 nodes / 34 links across 9 classes with self-loops, threshold 0: label overlap and edge clutter at full density.',
        tags: ['OCCLUSION', 'SEMANTICS'],
        data: DENSE,
        opts: { threshold: 0, dims: DIMS },
    },
    'threshold-simplified': {
        description: 'Same dense DFG at threshold 25%: only the dominant edges/nodes survive.',
        tags: ['HIERARCHY'],
        data: DENSE,
        opts: { threshold: 25, dims: DIMS },
    },
    'hostile-names': {
        description: 'Node names carrying <script>/quote payloads and unicode — labels and tooltips must render inert (UI-6).',
        tags: ['FEEDBACK', 'injection'],
        data: {
            total: 300,
            nodes: [
                { name: 'CPU*', total_ms: 900, class: 'CPU' },
                { name: 'Lock:<script>document.title="pwned"</script>', total_ms: 700, class: 'Lock' },
                { name: 'Extension:日本語イベント🐧', total_ms: 400, class: 'Extension' },
                { name: 'IO:"><img src=x onerror=alert(1)>', total_ms: 200, class: 'IO' },
            ],
            links: [
                { source: 'CPU*', target: 'Lock:<script>document.title="pwned"</script>', value: 120, duration_ms: 700 },
                { source: 'Lock:<script>document.title="pwned"</script>', target: 'CPU*', value: 110, duration_ms: 400 },
                { source: 'CPU*', target: 'Extension:日本語イベント🐧', value: 40, duration_ms: 400 },
                { source: 'CPU*', target: 'IO:"><img src=x onerror=alert(1)>', value: 30, duration_ms: 200 },
            ],
        },
        opts: { threshold: 0, dims: DIMS },
    },
    'variants-html': {
        description: 'Flow-variant cards (exec + plan) incl. a loop variant, sub-1% steps (min-width clamp, P11) and a hostile query_text.',
        tags: ['SEMANTICS', 'injection'],
        variants: VARIANTS,
    },
};
