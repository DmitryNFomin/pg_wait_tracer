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
        description: 'The time-model table: indents, class dots, pct bars. #187: DB Time now includes the "CPU (waiting for a core)" row (mirrors tests/mock_server.py). #435: Idle renders "—" (excluded from DB Time) in %DB Time, never a measured 0%. #453: Idle\'s AAS cell (parent AND its own "Other (background)" child) renders the SAME visible em-dash, with the idle-occupancy rate printed labeled beside it -- never the bare rate under an "AAS" header, which a still frame cannot tell from real active-session load.',
        tags: ['HIERARCHY'],
        config: 'overview',
        sort: null,   // overview keeps server order
        rows: [
            { indent: 0, name: 'DB Time',  ms: 12760, pct: 100.0, aas: 1.28 },
            { indent: 1, name: 'CPU (running)', ms: 4800, pct: 37.6, aas: 1.33 },
            { indent: 1, name: 'CPU (waiting for a core)', ms: 260, pct: 2.0, aas: 0.07 },
            { indent: 1, name: 'IO',       ms: 3200,  pct: 25.1,  aas: 0.89 },
            { indent: 2, name: 'IO:DataFileRead', ms: 2100, pct: 16.5, aas: 0.58 },
            { indent: 2, name: 'IO:WalSync',      ms: 800,  pct: 6.3,  aas: 0.22 },
            { indent: 1, name: 'Lock',     ms: 1500,  pct: 11.8,  aas: 0.42 },
            { indent: 1, name: 'LWLock',   ms: 1200,  pct: 9.4,   aas: 0.33 },
            // #453 repro shape: Idle's occupancy (24.62) is nineteen times
            // DB Time's real AAS (1.28) above -- the exact #453 scenario.
            { indent: 0, name: 'Idle',     ms: 888000, pct: null,  aas: 24.62 },
            { indent: 2, name: 'Other (background)', ms: 147600, pct: null, aas: 4.10 },
        ],
    },
    'events-null-latency': {
        description: 'Events table with an idle-but-visible row (pct null → "—") and a sampled-gated row (null percentiles → "—", FID-3). #453: Client:ClientRead\'s AAS cell is the SAME visible em-dash as its %DB cell, with the idle-occupancy rate (19.62) printed labeled beside it -- not the bare rate under an "AAS" header.',
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
              p95_us: 3800, p99_us: 12000, max_us: 90000, pct: null, aas: 19.62 },
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
    /* Deliberately LAST in this file (issue #122): #grid is a wrapping
     * flexbox, so inserting a cell re-wraps its row and re-rasterizes every
     * row-mate baseline after it. Appended here it spares
     * gallery/table-configs-queries-hostile-sql (5 changed cells -> 4 when
     * this state moved from mid-file to the end); the four that remain are
     * the compare cells the gallery renders last. */
    'events-overflow-pctl': {
        description: 'Events table where percentiles saturate the latency histogram\'s OPEN-ENDED top bucket (#103/#437): Avg/Max are seconds, so P50/P95/P99 must read ">=16.4ms" bounds, never exact values, with the beside-table disclosure sentence shown next to the table (not only on hover). #454: every OTHER (finite) bucket is a bound too -- LWLock:LockManager\'s P99 reports the SAME 16384 number as an overflow row but p99_overflow is false, so it renders "<=16.4ms" (bound, not exact) while the true overflow rows render ">=16.4ms" -- the flag, never the number, decides WHICH qualifier, but every finite percentile now carries one. The last row (LWLock:WALWrite) is fully inside the histogram and still gets the "<=" qualifier on every percentile cell -- #454 removed the old "entirely inside the histogram = unchanged" exemption, since hist_percentile() (src/compute.c:1718) always returns a bucket edge, not an exact value, for every bucket. CPU* overflows too: discloses the bound, but no histogram drill (event_id 0).',
        tags: ['SEMANTICS', 'FEEDBACK'],
        config: 'events',
        sort: { key: 'total_ms', asc: false },
        rows: [
            // The shape found by the live UI smoke (PG17, --mode full):
            // every percentile pinned at the top bucket next to a 1.8 s mean.
            { name: 'Lock:relation', event_id: 0x03000000, class: 'Lock',
              count: 900, total_ms: 1620, avg_us: 1800000, p50_us: 16384,
              p95_us: 16384, p99_us: 16384, max_us: 2600000,
              p50_overflow: true, p95_overflow: true, p99_overflow: true,
              pct: 35.8, aas: 0.45 },
            { name: 'Timeout:CheckpointWriteDelay', event_id: 0x09000000,
              class: 'Timeout', count: 1200, total_ms: 1202, avg_us: 100200,
              p50_us: 16384, p95_us: 16384, p99_us: 16384, max_us: 105400,
              p50_overflow: true, p95_overflow: true, p99_overflow: true,
              pct: 26.6, aas: 0.33 },
            // Partial: only the tail leaves the histogram — P50/P95 are
            // finite bucket edges (#454: "<=", not exact), P99 overflows ">=".
            { name: 'IO:DataFileRead', event_id: 0x01000015, class: 'IO',
              count: 85000, total_ms: 900, avg_us: 24.7, p50_us: 15,
              p95_us: 8192, p99_us: 16384, max_us: 42000,
              p50_overflow: false, p95_overflow: false, p99_overflow: true,
              pct: 19.9, aas: 0.25 },
            // #437/#454 finite-bucket CONTROL: bucket 14 (8.2..16.384ms)
            // reports the same 16384 edge value as the open-ended bucket 15
            // -- the comment on pctlCell() above explains why that number
            // alone cannot distinguish them. p99_overflow:false here means
            // this 16384 is the EXACT edge of a FINITE bucket, so it renders
            // "<=16.4ms" (a bound, #454), never the open row's ">=16.4ms" --
            // same number, different qualifier, decided only by the flag.
            { name: 'LWLock:LockManager', event_id: 0x04000012, class: 'LWLock',
              count: 12000, total_ms: 700, avg_us: 58.3, p50_us: 40,
              p95_us: 9000, p99_us: 16384, max_us: 16384,
              p50_overflow: false, p95_overflow: false, p99_overflow: false,
              pct: 15.5, aas: 0.19 },
            // CPU* overflows too: discloses the bound, but no histogram drill.
            { name: 'CPU*', event_id: 0, class: 'CPU',
              count: 250000, total_ms: 500, avg_us: 19.2, p50_us: 12,
              p95_us: 45, p99_us: 16384, max_us: 90000,
              p50_overflow: false, p95_overflow: false, p99_overflow: true,
              pct: 11.1, aas: 0.14 },
            // Entirely inside the histogram: #454 removed the "unchanged by
            // #103" exemption here -- every cell still gets its own "<="
            // bound qualifier, since hist_percentile() never returns an
            // exact value for any bucket, finite or open-ended.
            { name: 'LWLock:WALWrite', event_id: 0x04000008, class: 'LWLock',
              count: 30000, total_ms: 300, avg_us: 40.0, p50_us: 25,
              p95_us: 100, p99_us: 400, max_us: 8000,
              p50_overflow: false, p95_overflow: false, p99_overflow: false,
              pct: 6.6, aas: 0.08 },
        ],
    },
};
