/* pgwt — deterministic fixtures for the declarative table configs
 * (lib/builders/table-configs.js + lib/table.js buildTableModel/mountTable).
 *
 * States name their config by string (the fixture stays pure data; the gallery
 * maps 'overview'|'events'|'sessions'|'queries' to the imported config):
 *   states['<kebab-name>'] = { description, tags, config, rows, sort }
 * See ./aas.mjs for the directory-wide contract. Row shapes mirror
 * tests/mock_server.py's canned time_model/top_events/top_sessions/top_queries.
 */

export const states = {
    'overview': {
        description: 'The time-model table: indents, class dots, pct bars, the Idle row at pct 0.',
        tags: ['HIERARCHY'],
        config: 'overview',
        sort: null,   // overview keeps server order
        rows: [
            { indent: 0, name: 'DB Time',  ms: 12500, pct: 100.0, aas: 3.47 },
            { indent: 1, name: 'CPU*',     ms: 4800,  pct: 38.4,  aas: 1.33 },
            { indent: 1, name: 'IO',       ms: 3200,  pct: 25.6,  aas: 0.89 },
            { indent: 2, name: 'IO:DataFileRead', ms: 2100, pct: 16.8, aas: 0.58 },
            { indent: 2, name: 'IO:WalSync',      ms: 800,  pct: 6.4,  aas: 0.22 },
            { indent: 1, name: 'Lock',     ms: 1500,  pct: 12.0,  aas: 0.42 },
            { indent: 1, name: 'LWLock',   ms: 1200,  pct: 9.6,   aas: 0.33 },
            { indent: 0, name: 'Idle',     ms: 45000, pct: 0,     aas: 0.0 },
        ],
    },
    'events-null-latency': {
        description: 'Events table with an idle-but-visible row (pct null → "—") and a sampled-gated row (null percentiles → "—", FID-3).',
        tags: ['FEEDBACK', 'SEMANTICS', 'fidelity'],
        config: 'events',
        sort: { key: 'total_ms', asc: false },
        rows: [
            { name: 'CPU*', event_id: 0, class: 'CPU',
              count: 250000, total_ms: 4800, avg_us: 19.2, p50_us: 12,
              p95_us: 45, p99_us: 120, max_us: 5000, pct: 38.4, aas: 1.33 },
            { name: 'IO:DataFileRead', event_id: 0x01000015, class: 'IO',
              count: 85000, total_ms: 2100, avg_us: 24.7, p50_us: 15,
              p95_us: 80, p99_us: 250, max_us: 12000, pct: 16.8, aas: 0.58 },
            // Idle-but-visible (Client:ClientRead): pct is null by design.
            { name: 'Client:ClientRead', event_id: 0x06000000, class: 'Client',
              count: 42000, total_ms: 39000, avg_us: 928.6, p50_us: 400,
              p95_us: 3800, p99_us: 12000, max_us: 90000, pct: null, aas: 0.0 },
            // Sampled-gated (FID-3): latency columns are null over sampled data.
            { name: 'Lock:relation', event_id: 0x03000000, class: 'Lock',
              count: 12000, total_ms: 1500, avg_us: null, p50_us: null,
              p95_us: null, p99_us: null, max_us: null, pct: 12.0, aas: 0.42 },
        ],
    },
    'sessions-unicode': {
        description: 'Sessions table incl. background workers (empty user/db) and unicode user/database names.',
        tags: ['SEMANTICS', 'i18n'],
        config: 'sessions',
        sort: { key: 'db_time_ms', asc: false },
        rows: [
            { pid: 1001, type: 'client', user: 'postgres', db: 'testdb',
              db_time_ms: 5200, cpu_pct: 45.0, wait_pct: 55.0,
              top_wait: 'IO:DataFileRead', top_wait_id: 0x01000015 },
            { pid: 1002, type: 'client', user: 'аналитик', db: '日本語データベース',
              db_time_ms: 3800, cpu_pct: 38.0, wait_pct: 62.0,
              top_wait: 'Lock:relation', top_wait_id: 0x03000000 },
            { pid: 4870, type: 'checkpointer', user: '', db: '',
              db_time_ms: 800, cpu_pct: 10.0, wait_pct: 90.0,
              top_wait: 'Timeout:CheckpointWriteDelay', top_wait_id: 0x09000000 },
        ],
    },
    'queries-hostile-sql': {
        description: 'Queries table with hostile SQL (script/quotes, >120 chars → hover tooltip), unicode SQL, null lifecycle stats and sub-1% stacked-bar segments (P11).',
        tags: ['FEEDBACK', 'SEMANTICS', 'injection'],
        config: 'queries',
        sort: { key: 'total_ms', asc: false },
        rows: [
            { query_id: '3886912043147135675',
              text: 'UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2',
              total_ms: 4200, pct: 33.6, count: 45000, avg_us: 93.3,
              top_wait: 'IO:DataFileRead', top_wait_id: 0x01000015,
              exec_count: 45000, plan_count: 45000,
              avg_exec_ms: 0.092, p95_exec_ms: 0.31, p99_exec_ms: 1.2,
              avg_plan_ms: 0.011, p95_plan_ms: 0.04, p99_plan_ms: 0.09,
              classes: [2000, 1200, 500, 300, 0, 0, 100, 0, 0, 100, 0],
              events: [
                  { name: 'CPU*', id: 0, ms: 2000 },
                  { name: 'IO:DataFileRead', id: 0x01000015, ms: 1200 },
                  { name: 'Lock:relation', id: 0x03000000, ms: 500 },
                  { name: 'LWLock:WALWrite', id: 0x04000008, ms: 300 },
                  // Sub-1% segments: exercise the min-width clamp (P11).
                  { name: 'Extension:Extension', id: 0x0a000000, ms: 12 },
                  { name: 'Timeout:PgSleep', id: 0x09000002, ms: 8 },
              ] },
            { query_id: '-6660000000000000666',
              text: 'SELECT \'<script>document.title="pwned"</script>\' AS x, "col&<>" FROM "users" '
                  + 'WHERE note = \'a"b\'\'c<d>e\' AND payload = \'<img src=x onerror=alert(1)>\' '
                  + '-- a very long hostile comment to push the text past the 120-char hover-tooltip threshold',
              total_ms: 2800, pct: 22.4, count: 4500, avg_us: 622.2,
              top_wait: 'Lock:relation', top_wait_id: 0x03000000,
              exec_count: null, plan_count: null,
              avg_exec_ms: null, p95_exec_ms: null, p99_exec_ms: null,
              avg_plan_ms: null, p95_plan_ms: null, p99_plan_ms: null,
              classes: [1000, 800, 900, 0, 0, 0, 100, 0, 0, 0, 0],
              events: null },
            { query_id: '5371305355164922084',
              text: 'SELECT "名前", "цена" FROM "商品" WHERE "名前" = $1 -- unicode identifiers 🐧',
              total_ms: 1100, pct: 8.8, count: 9000, avg_us: 122.2,
              top_wait: 'IO:DataFileRead', top_wait_id: 0x01000015,
              exec_count: 9000, plan_count: 120,
              avg_exec_ms: 0.11, p95_exec_ms: 0.4, p99_exec_ms: 0.9,
              avg_plan_ms: 0.02, p95_plan_ms: 0.05, p99_plan_ms: 0.07,
              classes: [500, 500, 0, 0, 0, 0, 100, 0, 0, 0, 0],
              events: null },
            { query_id: '0', text: null,   // no captured text → "—"
              total_ms: 400, pct: 3.2, count: 800, avg_us: 500.0,
              top_wait: 'CPU*', top_wait_id: 0,
              classes: [400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
              events: null },
        ],
    },
    'compare-delta': {
        description: 'Compare events: |Δ|-ranked rows with new/gone labels and one honest below-change-floor row.',
        tags: ['HIERARCHY', 'SEMANTICS', 'compare'],
        config: 'compare-events',
        sort: { key: 'abs_delta_ms', asc: false },
        compare: {
            kind: 'events',
            a: { db_time_ms: 10000, fidelity: 'exact', rows: [
                { event_id: 1, name: 'IO:DataFileRead', class: 'IO', total_ms: 3200 },
                { event_id: 2, name: 'Lock:new_in_A', class: 'Lock', total_ms: 700 },
                { event_id: 3, name: 'IPC:below_floor', class: 'IPC', total_ms: 140 },
            ] },
            b: { db_time_ms: 10000, fidelity: 'exact', rows: [
                { event_id: 1, name: 'IO:DataFileRead', class: 'IO', total_ms: 1200 },
                { event_id: 3, name: 'IPC:below_floor', class: 'IPC', total_ms: 100 },
                { event_id: 4, name: 'Client:gone_from_A', class: 'Client', total_ms: 500 },
            ] },
        },
    },
};
