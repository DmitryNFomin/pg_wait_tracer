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
        { name: 'CPU*',                    idle: false, total_ms: 5200, class: 'CPU' },
        { name: 'Client:ClientRead',       idle: true, total_ms: 2600, class: 'Client' },
        { name: 'IO:DataFileRead',         idle: false, total_ms: 2100, class: 'IO' },
        { name: 'Lock:transactionid',      idle: false, total_ms: 1500, class: 'Lock' },
        { name: 'LWLock:WALInsert',        idle: false, total_ms: 900,  class: 'LWLock' },
        { name: 'IO:WalSync',              idle: false, total_ms: 800,  class: 'IO' },
        { name: 'LWLock:WALWrite',         idle: false, total_ms: 640,  class: 'LWLock' },
        { name: 'Lock:tuple',              idle: false, total_ms: 480,  class: 'Lock' },
        { name: 'LWLock:BufferMapping',    idle: false, total_ms: 420,  class: 'LWLock' },
        { name: 'IO:WalWrite',             idle: false, total_ms: 350,  class: 'IO' },
        { name: 'Timeout:VacuumDelay',     idle: false, total_ms: 300,  class: 'Timeout' },
        { name: 'Lock:relation',           idle: false, total_ms: 260,  class: 'Lock' },
        { name: 'IO:DataFileExtend',       idle: false, total_ms: 240,  class: 'IO' },
        { name: 'LWLock:LockManager',      idle: false, total_ms: 210,  class: 'LWLock' },
        { name: 'IPC:BufferIO',            idle: false, total_ms: 190,  class: 'IPC' },
        { name: 'Extension:Extension',     idle: false, total_ms: 150,  class: 'Extension' },
        { name: 'IPC:MessageQueueReceive', idle: false, total_ms: 120,  class: 'IPC' },
        { name: 'BufferPin:BufferPin',     idle: false, total_ms: 90,   class: 'BufferPin' },
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

/* #436 ("threshold-simplified", extended per contract 5): the dense DFG
 * plus
 *   - a RARE-but-LONGEST-duration Lock:relation — the exact "a rare but
 *     long Lock:relation disappears" case the issue names. Its edge COUNT
 *     stays the original 55/54 (well below the 25%-of-1400 threshold, so it
 *     is still excluded by the frequency criterion — that criterion is
 *     unchanged, see transitions.js ~:18), but its DURATION is bumped to
 *     the largest in the graph, so omittedDominant must name it instead of
 *     dropping it silently.
 *   - CPU-mediated paths (already true of DENSE: every edge routes through
 *     CPU*) and pacing idle (DENSE's existing Client:ClientRead<->CPU*
 *     loop) — exercised together by setting hideIdle:true below.
 *   - server-side link-cap truncation (truncated/link_count/
 *     total_link_count), the THIRD independent omission cause, declared
 *     directly on the payload.
 * One state exercising idle exclusion + threshold exclusion + server
 * truncation + the named omitted-dominant wait together, as the contract
 * asks for, instead of three separate fixtures that never interact. */
function thresholdSimplifiedData() {
    const nodes = DENSE.nodes.map(n => n.name === 'Lock:relation'
        ? { ...n, total_ms: 52000 }   // now the single largest total_ms in the graph
        : { ...n });
    const links = DENSE.links.map(l => {
        const isLockRelationEdge =
            (l.source === 'CPU*' && l.target === 'Lock:relation') ||
            (l.source === 'Lock:relation' && l.target === 'CPU*');
        if (!isLockRelationEdge) return { ...l };
        // Count (value) is UNCHANGED — still rare — only the dwell grows.
        return { ...l, duration_ms: l.source === 'CPU*' ? 52000 : 50000 };
    });
    return {
        total: DENSE.total, nodes, links,
        truncated: true, link_count: links.length, total_link_count: links.length + 3,
    };
}
const THRESHOLD_SIMPLIFIED = thresholdSimplifiedData();

/* Idle-loop-dominant DFG (issue #107): shaped from the real box-check
 * EPHEMERAL capture that filed the issue — a pgbench + lock-contention + IO
 * workload where the Client:ClientRead<->CPU* loop's TRANSITION COUNT
 * dwarfs every other edge (every wait cycle passes through it), while
 * Lock:relation/Timeout:PgSleep/IO:DataFileRead transitions are rare by
 * comparison though they are the entire point of the tab. node total_ms
 * mirror the issue's own reported numbers (ClientRead 915.3s, CPU* 10.0s,
 * Lock:relation 28.7%/Timeout 68% of DB time on that capture). */
function idleDominantDfg() {
    const nodes = [
        { name: 'CPU*',              idle: false, total_ms: 10000,  class: 'CPU' },
        { name: 'Client:ClientRead', idle: true, total_ms: 915300, class: 'Client' },
        { name: 'Lock:relation',     idle: false, total_ms: 28700,  class: 'Lock' },
        { name: 'Timeout:PgSleep',   idle: false, total_ms: 68000,  class: 'Timeout' },
        { name: 'IO:DataFileRead',   idle: false, total_ms: 5200,   class: 'IO' },
    ];
    const links = [
        { source: 'Client:ClientRead', target: 'CPU*', value: 48000, duration_ms: 915300 },
        { source: 'CPU*', target: 'Client:ClientRead', value: 47950, duration_ms: 910000 },
        { source: 'CPU*', target: 'Lock:relation',     value: 600,   duration_ms: 28700 },
        { source: 'Lock:relation', target: 'CPU*',     value: 595,   duration_ms: 27000 },
        { source: 'CPU*', target: 'Timeout:PgSleep',   value: 300,   duration_ms: 68000 },
        { source: 'Timeout:PgSleep', target: 'CPU*',   value: 298,   duration_ms: 66000 },
        { source: 'CPU*', target: 'IO:DataFileRead',   value: 200,   duration_ms: 5200 },
        { source: 'IO:DataFileRead', target: 'CPU*',   value: 198,   duration_ms: 5000 },
    ];
    let total = 0;
    for (const l of links) total += l.value;
    return { total, nodes, links };
}
const IDLE_DOMINANT = idleDominantDfg();

/* Mirrors tests/mock_server.py's canned variants payload, plus a hostile
 * query_text (it lands in a title attribute — same escape class as UI-6). */
const VARIANTS = {
    exec: {
        total: 45000,
        num_variants: 3,
        variants: [
            { exec_count: 30000, num_queries: 1, total_ms: 2790.0,
              avg_ms: 0.093, p95_ms: 0.30, p95_sample_n: 10000, avg_loop_n: 1,
              top_query_id: 3886912043147135675,
              steps: [
                  { name: 'CPU*', avg_ms: 0.04, class: 'cpu' },
                  { name: 'IO:DataFileRead', avg_ms: 0.03, class: 'IO' },
                  { name: 'CPU*', avg_ms: 0.023, class: 'cpu' },
              ],
              query_text: 'UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2' },
            { exec_count: 15000, num_queries: 1, total_ms: 1360.0,
              avg_ms: 0.09, p95_ms: 0.28, p95_sample_n: 10000, avg_loop_n: 4,
              top_query_id: 5371305355164922084,
              steps: [
                  { name: 'CPU*', avg_ms: 0.05, class: 'cpu' },
                  { name: 'LWLock:WALWrite', avg_ms: 0.04, class: 'LWLock', loop: true },
              ],
              query_text: 'SELECT abalance FROM pgbench_accounts WHERE aid = $1' },
            { exec_count: 120, num_queries: 1, total_ms: 900.0,
              avg_ms: 7.5, p95_ms: 21.0, p95_sample_n: 120, avg_loop_n: 1,
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
              avg_ms: 0.011, p95_ms: 0.04, p95_sample_n: 10000, avg_loop_n: 1,
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
            nodes: [{ name: 'CPU*', idle: false, total_ms: 4800, class: 'CPU' }],
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
        description: '#436: dense DFG at threshold 25% with hideIdle on — exercises all three ' +
            'omission causes together (idle-excluded ClientRead loop, threshold-excluded edges, ' +
            'server link-cap truncation) plus the rare-but-longest-duration Lock:relation the ' +
            'issue names: its count (55/54) stays below the cutoff so it is still omitted, but ' +
            'its 52.0s/50.0s dwell is the largest in the graph — omittedDominant must name it, ' +
            'and the disclosure line / "Show all" restore geometry are checked against this state.',
        tags: ['HIERARCHY', 'FEEDBACK'],
        data: THRESHOLD_SIMPLIFIED,
        opts: { threshold: 25, dims: DIMS, hideIdle: true },
    },
    'idle-loop-dominant': {
        description: 'Issue #107, hideIdle OFF (the pre-fix default): the real-OLTP-shaped ClientRead<->CPU* loop dominates edge COUNT at the default 20% threshold, so Lock:relation/Timeout:PgSleep/IO:DataFileRead all drop below it — exactly the bug report ("only the ClientRead<->CPU* loop").',
        tags: ['HIERARCHY', 'SEMANTICS'],
        data: IDLE_DOMINANT,
        opts: { threshold: 20, dims: DIMS, hideIdle: false },
    },
    'idle-loop-hidden': {
        description: 'Issue #107 fix, hideIdle ON (the new default): the idle loop is removed from the ranking pool BEFORE the threshold runs, so Lock:relation/Timeout:PgSleep/IO:DataFileRead all clear it and render.',
        tags: ['HIERARCHY', 'FEEDBACK'],
        data: IDLE_DOMINANT,
        opts: { threshold: 20, dims: DIMS, hideIdle: true },
    },
    /* #436 adviser-review negative control: six nodes with IDENTICAL
     * total_ms (so every symbolSize and footprint is identical too — no
     * single dominant/huge node can mask the effect) at DIMS (620x460),
     * threshold 0 (nothing hidden). The builder's PRE-transform-pin math
     * read this as fitting; the live, fully-rendered coordinates did not
     * (measured via chart.getModel().getSeriesByIndex(0).coordinateSystem
     * .dataToPoint() against ECharts 5.5.1, web/static/vendor/echarts.min.js
     * -- the bottom row landed ~5% further from center than its data
     * coordinates, past the container's bottom edge) until the graph
     * series pinned its own view box (left/top/width/height) AND its data
     * bounding box (the two invisible layoutAnchorNodes) to make that
     * mapping the identity transform. tests/test_dfg_geometry.py renders
     * this cell for real and checks the RENDERED coordinates — a check a
     * pure option-object inspection cannot perform. */
    'six-equal-duration': {
        description: '#436 adviser control: six equal-total_ms nodes at 620x460 — the real ' +
            'rendered position differs from the pre-transform data position whenever the ' +
            'graph series is not pinned to an identity view transform; see ' +
            'tests/test_dfg_geometry.py for the render-based check.',
        tags: ['ALIGNMENT'],
        data: (() => {
            const names = ['CPU*', 'A:Event', 'B:Event', 'C:Event', 'D:Event', 'E:Event'];
            const nodes = names.map(name => ({ name, total_ms: 1000, class: 'CPU', idle: false }));
            const links = [];
            for (let i = 1; i < names.length; i++) {
                links.push({ source: 'CPU*', target: names[i], value: 100, duration_ms: 1 });
                links.push({ source: names[i], target: 'CPU*', value: 100, duration_ms: 1 });
            }
            return { total: links.reduce((s, l) => s + l.value, 0), nodes, links };
        })(),
        opts: { threshold: 0, dims: DIMS },
    },
    'hostile-names': {
        description: 'Node names carrying <script>/quote payloads and unicode — labels and tooltips must render inert (UI-6).',
        tags: ['FEEDBACK', 'injection'],
        data: {
            total: 300,
            nodes: [
                { name: 'CPU*', idle: false, total_ms: 900, class: 'CPU' },
                { name: 'Lock:<script>document.title="pwned"</script>', idle: false, total_ms: 700, class: 'Lock' },
                { name: 'Extension:日本語イベント🐧', idle: false, total_ms: 400, class: 'Extension' },
                { name: 'IO:"><img src=x onerror=alert(1)>', idle: false, total_ms: 200, class: 'IO' },
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
