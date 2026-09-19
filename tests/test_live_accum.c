/* test_live_accum.c — live-view interval accounting (issues #97, #98).
 *
 * #97: the multi-window system_event view showed CPU* at 128-142% of DB
 * Time (tests/test_multi_window.py "Non-idle top-level %DB", PG18 gate box).
 * Root cause: the closed-record path filed a client backend's NON-COMMAND
 * on-CPU record (we==0, command gate closed at emission) under the CPU* row
 * while routing its time to the idle Activity bucket — so the CPU* row grew
 * by time DB Time never contained. Both live paths (closed record and open
 * state_map stretch) now fold through pgwt_accum_add_interval with ONE
 * classification (pgwt_live_effective_event), and the ring delta saturates
 * instead of wrapping when an open stretch closes under a different label.
 *
 * #98: "in-command" itself was the gate value at EMISSION, which is always
 * clear when a waitless statement's run closes (STATE_IDLE precedes the
 * ClientRead) — the server/CLI CPU ratio of 4.6-7.1x. The live paths now
 * sweep the CMD_START/CMD_END markers with the server's majority rule
 * (tests 5-8; test 8 diffs the live sweep against compute.c
 * pgwt_tag_events on one generated stream).
 *
 * Pure: links map_reader.c (-DPGWT_SERVER, BPF-free core) + snapshot.c +
 * compute.c. No daemon, no PostgreSQL, no root. */
#include "map_reader.h"
#include "snapshot.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "compute.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define MS(x) ((uint64_t)(x) * 1000000ULL)
#define IO_WALSYNC   WEI(PG_WAIT_IO, 7)      /* any IO-class id */
#define LW_WALWRITE  WEI(PG_WAIT_LWLOCK, 3)  /* any LWLock-class id */

static uint64_t sys_row(const struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == we)
            return acc->system_events[i].total_ns;
    return 0;
}

static uint64_t sys_count(const struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == we)
            return acc->system_events[i].count;
    return 0;
}

static uint64_t snap_row(const struct pgwt_snapshot *s, uint32_t we)
{
    for (int i = 0; i < s->num_events; i++)
        if (s->events[i].wait_event == we)
            return s->events[i].total_ns;
    return 0;
}

/* Σ of the NON-IDLE system rows — what the view's "% DB" column sums over
 * (idle rows render "—"). Must equal DB Time exactly when every interval
 * is accounted at wall (the closed-record model). */
static uint64_t sys_nonidle_sum(const struct pgwt_accumulator *acc)
{
    uint64_t sum = 0;
    for (int i = 0; i < acc->num_system_events; i++)
        if (!pgwt_is_idle_event(acc->system_events[i].wait_event))
            sum += acc->system_events[i].total_ns;
    return sum;
}

static double pct_db(uint64_t row_ns, uint64_t db_ns)
{
    return db_ns ? 100.0 * (double)row_ns / (double)db_ns : 0.0;   /* output.c */
}

/* Closed trace record from a foreground (client) backend, gate active. */
static struct pgwt_live_interval closed_fg(uint32_t pid, uint32_t we,
                                           uint64_t wall, int cmd_open,
                                           uint64_t qid)
{
    struct pgwt_live_interval iv = {
        .pid = pid, .we = we, .wall_ns = wall, .cpu_ns = wall,
        .query_id = qid, .cat_flag = 0,
        .cmd_gate_active = true, .cmd_open = cmd_open != 0, .closed = true,
    };
    return iv;
}

/* ── 1. The classification both paths share ────────────────────────── */
static void test_effective_event(void)
{
    printf("--- pgwt_live_effective_event ---\n");
    /* Client backend, gate maintained, outside a command: idle non-command
     * CPU (the T2 decision table). */
    CHECK(pgwt_live_effective_event(0, 0, true, false) == PGWT_WEI_NONCMD_CPU,
          "client we==0 outside a command -> NONCMD_CPU");
    CHECK(pgwt_is_idle_event(PGWT_WEI_NONCMD_CPU),
          "NONCMD_CPU is idle (excluded from DB Time)");
    CHECK(pgwt_is_hidden_event(PGWT_WEI_NONCMD_CPU),
          "NONCMD_CPU is hidden from event lists (like other Activity)");
    /* In-command CPU stays CPU*. */
    CHECK(pgwt_live_effective_event(0, 0, true, true) == 0,
          "client we==0 inside a command -> CPU*");
    /* Gate unavailable (no on_report_activity probe): legacy ungated CPU. */
    CHECK(pgwt_live_effective_event(0, 0, false, false) == 0,
          "gate inactive -> we==0 stays CPU*");
    /* Background / maintenance / io_worker processes never report activity
     * states: their we==0 unambiguously means working. */
    CHECK(pgwt_live_effective_event(0, PGWT_EVENT_FLAG_BACKGROUND, true, false) == 0,
          "background we==0 -> CPU*");
    CHECK(pgwt_live_effective_event(0, PGWT_EVENT_FLAG_MAINT, true, false) == 0,
          "autovacuum we==0 -> CPU*");
    CHECK(pgwt_live_effective_event(0, PGWT_EVENT_FLAG_IO_WORKER, true, false) == 0,
          "io_worker we==0 -> raw (row-visible, load-excluded elsewhere)");
    /* Waits are never rewritten. */
    CHECK(pgwt_live_effective_event(IO_WALSYNC, 0, true, false) == IO_WALSYNC,
          "a wait outside a command is still that wait");
    CHECK(pgwt_live_effective_event(PG_WAIT_CLIENT_READ, 0, true, false) == PG_WAIT_CLIENT_READ,
          "ClientRead is untouched");
}

/* ── 2. The #97 reproduction: pgbench-shaped closed records ────────── */
static void test_closed_noncmd_cpu_row(void)
{
    printf("--- closed records: non-command CPU must not sit in the CPU* row ---\n");
    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);

    /* One pgbench client: idle read, a waitless UPDATE whose on-CPU run ends
     * at the next ClientRead (gate already closed at emission -> non-command
     * per the live gate), the commit's in-command CPU, and its WAL wait. */
    struct pgwt_live_interval seq[] = {
        closed_fg(100, PG_WAIT_CLIENT_READ, MS(10), 0, 0),
        closed_fg(100, 0,          MS(3), 0, 0xABC),   /* non-command CPU */
        closed_fg(100, 0,          MS(1), 1, 0xABC),   /* in-command CPU  */
        closed_fg(100, IO_WALSYNC, MS(1), 1, 0xABC),
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        pgwt_accum_add_interval(acc, &seq[i]);

    uint64_t db = acc->tm.db_time_ns;
    uint64_t cpu_row = sys_row(acc, 0);
    uint64_t noncmd_row = sys_row(acc, PGWT_WEI_NONCMD_CPU);

    /* The inputs are chosen so that the SUM of all we==0 records (3 + 1 ms)
     * exceeds DB Time (1 + 1 ms): a CPU* row that carried every we==0
     * record regardless of the gate would print > 100% here. This pins the
     * shape of the input, not a comparison against another build. */
    uint64_t all_we0 = MS(3) + MS(1);
    CHECK(pct_db(all_we0, db) > 100.0,
          "inputs: Σ we==0 records = %.1f%% of DB Time (> 100, the #97 shape)",
          pct_db(all_we0, db));

    CHECK(db == MS(2), "DB Time = in-command CPU + WAL wait = 2 ms (got %llu ns)",
          (unsigned long long)db);
    CHECK(cpu_row == MS(1), "CPU* row = in-command CPU only (got %llu ns)",
          (unsigned long long)cpu_row);
    CHECK(cpu_row == acc->tm.cpu_time_ns,
          "CPU* row == time-model CPU (row %llu vs tm %llu)",
          (unsigned long long)cpu_row, (unsigned long long)acc->tm.cpu_time_ns);
    CHECK(pct_db(cpu_row, db) <= 100.0,
          "CPU* %%DB = %.1f%% (<= 100)", pct_db(cpu_row, db));
    CHECK(noncmd_row == MS(3),
          "non-command CPU filed under Activity:NonCommandCpu (got %llu ns)",
          (unsigned long long)noncmd_row);
    CHECK(acc->tm.activity_time_ns == MS(10) + MS(3),
          "Activity bucket = ClientRead + non-command CPU (got %llu ns)",
          (unsigned long long)acc->tm.activity_time_ns);
    CHECK(sys_nonidle_sum(acc) == db,
          "Σ non-idle rows == DB Time (%llu vs %llu)",
          (unsigned long long)sys_nonidle_sum(acc), (unsigned long long)db);
    CHECK(acc->tm.io_time_ns == MS(1), "IO class carries the WAL wait");

    /* Per-pid and per-query rows agree with the system rows. */
    struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, 100);
    CHECK(pa != NULL, "pid entry exists");
    if (pa) {
        CHECK(pa->cpu_time_ns == MS(1) && pa->db_time_ns == MS(2) &&
              pa->wait_time_ns == MS(1),
              "per-pid load: cpu %llu db %llu wait %llu",
              (unsigned long long)pa->cpu_time_ns,
              (unsigned long long)pa->db_time_ns,
              (unsigned long long)pa->wait_time_ns);
        uint64_t es_noncmd = 0, es_cpu = 0;
        for (int i = 0; i < pa->num_events; i++) {
            if (pa->events[i].wait_event == PGWT_WEI_NONCMD_CPU)
                es_noncmd = pa->events[i].total_ns;
            if (pa->events[i].wait_event == 0)
                es_cpu = pa->events[i].total_ns;
        }
        CHECK(es_noncmd == MS(3) && es_cpu == MS(1),
              "per-pid rows split the same way (noncmd %llu, cpu %llu)",
              (unsigned long long)es_noncmd, (unsigned long long)es_cpu);
    }
    uint64_t qe_cpu = 0, qe_noncmd = 0;
    for (int i = 0; i < acc->num_query_events; i++) {
        if (acc->query_events[i].query_id != 0xABC) continue;
        if (acc->query_events[i].wait_event == 0)
            qe_cpu = acc->query_events[i].total_ns;
        if (acc->query_events[i].wait_event == PGWT_WEI_NONCMD_CPU)
            qe_noncmd = acc->query_events[i].total_ns;
    }
    CHECK(qe_cpu == MS(1) && qe_noncmd == MS(3),
          "per-query rows split the same way (cpu %llu, noncmd %llu)",
          (unsigned long long)qe_cpu, (unsigned long long)qe_noncmd);

    /* Closed records feed the histograms; count is per record. */
    CHECK(sys_count(acc, 0) == 1 && sys_count(acc, PGWT_WEI_NONCMD_CPU) == 1,
          "row counts are per record");
    free(acc);
}

/* ── 3. The open stretch classifies exactly like the record it becomes ── */
static void test_open_interval(void)
{
    printf("--- open state_map stretch ---\n");
    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);

    /* Client inside a command: measured CPU to the CPU* row and tm.cpu, wall
     * to DB Time (the off-CPU remainder is DB Time minus CPU*). */
    struct pgwt_live_interval in_cmd = {
        .pid = 200, .we = 0, .wall_ns = MS(5), .cpu_ns = MS(3),
        .query_id = 0x1, .cat_flag = 0,
        .cmd_gate_active = true, .cmd_open = true, .closed = false,
    };
    pgwt_accum_add_interval(acc, &in_cmd);
    CHECK(sys_row(acc, 0) == MS(3) && acc->tm.cpu_time_ns == MS(3),
          "in-command open stretch: CPU* row/tm = measured 3 ms");
    CHECK(acc->tm.db_time_ns == MS(5), "DB Time = wall 5 ms");
    CHECK(pct_db(sys_row(acc, 0), acc->tm.db_time_ns) <= 100.0,
          "measured CPU never exceeds its wall");

    /* Client between commands: idle, invisible to DB Time. */
    struct pgwt_live_interval between = in_cmd;
    between.pid = 201; between.wall_ns = MS(2); between.cpu_ns = MS(2);
    between.cmd_open = false; between.query_id = 0;
    pgwt_accum_add_interval(acc, &between);
    CHECK(sys_row(acc, PGWT_WEI_NONCMD_CPU) == MS(2),
          "between-command open stretch -> NonCommandCpu row (wall)");
    CHECK(acc->tm.db_time_ns == MS(5) && acc->tm.cpu_time_ns == MS(3),
          "…and DB Time / CPU untouched");
    CHECK(acc->tm.activity_time_ns == MS(2), "…Activity bucket +2 ms");

    /* io_worker: visible in the rows, never in the time model or load. */
    struct pgwt_live_interval iow = {
        .pid = 300, .we = 0, .wall_ns = MS(4), .cpu_ns = MS(4),
        .query_id = 0x2, .cat_flag = PGWT_EVENT_FLAG_IO_WORKER,
        .cmd_gate_active = true, .cmd_open = false, .closed = false,
    };
    pgwt_accum_add_interval(acc, &iow);
    CHECK(sys_row(acc, 0) == MS(3) + MS(4), "io_worker CPU visible in the CPU* row");
    CHECK(acc->tm.cpu_time_ns == MS(3) && acc->tm.db_time_ns == MS(5),
          "io_worker CPU excluded from the time model");
    struct pgwt_pid_accum *pw = pgwt_find_pid_accum(acc, 300);
    CHECK(pw && pw->cpu_time_ns == 0 && pw->db_time_ns == 0,
          "io_worker per-pid load stays 0");
    CHECK(acc->num_query_events == 1, "io_worker never attributes to a query");

    /* Background process (checkpointer) on CPU: always active. */
    struct pgwt_live_interval bg = {
        .pid = 400, .we = 0, .wall_ns = MS(1), .cpu_ns = MS(1),
        .cat_flag = PGWT_EVENT_FLAG_BACKGROUND,
        .cmd_gate_active = true, .cmd_open = false, .closed = false,
    };
    pgwt_accum_add_interval(acc, &bg);
    CHECK(acc->tm.cpu_time_ns == MS(4) && acc->tm.db_time_ns == MS(6),
          "background we==0 counts as CPU* / DB Time");

    /* Open stretches do not touch the histograms (no closed duration). */
    uint64_t hist = 0;
    for (int i = 0; i < acc->num_system_events; i++)
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
            hist += acc->system_events[i].histogram[b];
    CHECK(hist == 0, "open stretches leave histograms empty (got %llu)",
          (unsigned long long)hist);
    free(acc);
}

/* ── 4. Multi-window delta of ring snapshots ───────────────────────── */
static void test_ring_delta(void)
{
    printf("--- ring delta ---\n");
    struct pgwt_ring ring;
    CHECK(pgwt_ring_init(&ring, 4) == 0, "ring init");
    struct pgwt_accumulator *closed = calloc(1, sizeof(*closed));   /* event_accum */
    struct pgwt_accumulator *view = calloc(1, sizeof(*view));       /* d->accum   */
    pgwt_accum_init(closed);

    /* Tick 1: cumulative closed records so far + one straddling open
     * in-command run (5 s wall, 4 s on-CPU: 1 s runqueue). */
    struct pgwt_live_interval c1 = closed_fg(1, IO_WALSYNC, MS(200), 1, 0);
    struct pgwt_live_interval c2 = closed_fg(1, 0, MS(300), 1, 0);
    struct pgwt_live_interval c3 = closed_fg(1, 0, MS(150), 0, 0);   /* noncmd */
    pgwt_accum_add_interval(closed, &c1);
    pgwt_accum_add_interval(closed, &c2);
    pgwt_accum_add_interval(closed, &c3);
    memcpy(view, closed, sizeof(*view));
    struct pgwt_live_interval open1 = {
        .pid = 2, .we = 0, .wall_ns = MS(5000), .cpu_ns = MS(4000),
        .cmd_gate_active = true, .cmd_open = true, .closed = false,
    };
    pgwt_accum_add_interval(view, &open1);
    pgwt_ring_push(&ring, view);

    /* Tick 2: more closed records; pid 2's run is still open (10 s, 8 s CPU). */
    struct pgwt_live_interval c4 = closed_fg(1, 0, MS(100), 1, 0);
    struct pgwt_live_interval c5 = closed_fg(1, LW_WALWRITE, MS(50), 1, 0);
    struct pgwt_live_interval c6 = closed_fg(1, 0, MS(400), 0, 0);   /* noncmd */
    pgwt_accum_add_interval(closed, &c4);
    pgwt_accum_add_interval(closed, &c5);
    pgwt_accum_add_interval(closed, &c6);
    memcpy(view, closed, sizeof(*view));
    struct pgwt_live_interval open2 = open1;
    open2.wall_ns = MS(10000); open2.cpu_ns = MS(8000);
    pgwt_accum_add_interval(view, &open2);
    pgwt_ring_push(&ring, view);

    struct pgwt_snapshot *d = calloc(1, sizeof(*d));
    CHECK(pgwt_ring_delta(&ring, 1, d) == 0, "delta over one tick");
    /* Window: closed in-command CPU 100 + LWLock 50 + open CPU (8000-4000)
     * measured over wall (10000-5000). */
    CHECK(d->tm.db_time_ns == MS(100) + MS(50) + MS(5000),
          "window DB Time = %llu ns", (unsigned long long)d->tm.db_time_ns);
    CHECK(snap_row(d, 0) == MS(100) + MS(4000),
          "window CPU* row = %llu ns", (unsigned long long)snap_row(d, 0));
    CHECK(snap_row(d, 0) == d->tm.cpu_time_ns, "window CPU* row == window tm.cpu");
    double p = pct_db(snap_row(d, 0), d->tm.db_time_ns);
    CHECK(p <= 100.0, "window CPU* %%DB = %.1f%% (<= 100)", p);
    CHECK(snap_row(d, PGWT_WEI_NONCMD_CPU) == MS(400),
          "window NonCommandCpu = the closed non-command record");
    CHECK(d->tm.activity_time_ns == MS(400), "…and the Activity bucket agrees");
    /* The pre-fix quantity for this window: CPU* row would also have carried
     * the 400 ms non-command record -> (100+4000+400)/(5150) = 87% here, but
     * with pgbench's DB Time dominated by short WAL waits the same term is
     * what pushed the column to 128-142%. Assert the identity instead: the
     * non-idle rows partition the window's DB Time up to the off-CPU
     * remainder of the open run (1000 ms runqueue here). */
    uint64_t nonidle = 0;
    for (int i = 0; i < d->num_events; i++)
        if (!pgwt_is_idle_event(d->events[i].wait_event))
            nonidle += d->events[i].total_ns;
    CHECK(nonidle + MS(1000) == d->tm.db_time_ns,
          "Σ non-idle window rows + off-CPU remainder == window DB Time "
          "(%llu + 1000ms vs %llu)", (unsigned long long)nonidle,
          (unsigned long long)d->tm.db_time_ns);

    /* Tick 3: pid 2's run CLOSES at its ClientRead with the gate already
     * closed -> the record is non-command. The open stretch the previous
     * snapshots carried under CPU* / DB Time is now filed under NonCommandCpu
     * / Activity, so CPU* and DB Time go DOWN across the window. The delta
     * must saturate — never wrap to ~1.8e19 ns. */
    struct pgwt_live_interval c7 = closed_fg(2, 0, MS(12000), 0, 0);
    pgwt_accum_add_interval(closed, &c7);
    memcpy(view, closed, sizeof(*view));
    pgwt_ring_push(&ring, view);
    CHECK(pgwt_ring_delta(&ring, 1, d) == 0, "delta after the reclassifying close");
    CHECK(d->tm.db_time_ns == 0, "DB Time delta saturates at 0 (got %llu)",
          (unsigned long long)d->tm.db_time_ns);
    CHECK(d->tm.cpu_time_ns == 0, "CPU delta saturates at 0 (got %llu)",
          (unsigned long long)d->tm.cpu_time_ns);
    CHECK(snap_row(d, 0) == 0, "CPU* row delta saturates at 0 (got %llu)",
          (unsigned long long)snap_row(d, 0));
    CHECK(snap_row(d, PGWT_WEI_NONCMD_CPU) == MS(12000),
          "the run shows up whole under NonCommandCpu (got %llu)",
          (unsigned long long)snap_row(d, PGWT_WEI_NONCMD_CPU));
    CHECK(d->tm.activity_time_ns == MS(12000), "…and in the Activity bucket");
    for (int i = 0; i < d->num_events; i++)
        CHECK(d->events[i].total_ns < (1ULL << 62) && d->events[i].count < (1ULL << 62),
              "no wrapped counter in the delta (event 0x%x)", d->events[i].wait_event);
    /* The fail-safe is not silent: every clamped field is counted (the
     * daemon folds it into metrics ring_delta_clamps_total). Here: tm.db,
     * tm.cpu, CPU* row count + total_ns = 4. */
    CHECK(d->clamped_fields == 4, "clamped_fields = %u (expected 4)",
          d->clamped_fields);

    /* Tick 4-5, the MIXED window (what the clamp does NOT repair): pid 3
     * has an open in-command run of 2 s (1.5 s on-CPU) in snapshot A; in
     * snapshot B it has closed gate-clear (NonCommandCpu, 2.5 s) while pid
     * 1 added a 3 s WAL wait. DB Time still goes UP over the window (by
     * 3 s - 2 s = 1 s > 0, so it is not clamped), but the IO row's 3 s is
     * measured against it: 300% of the window's DB Time. This residual is
     * bounded by the reclassified stretch's wall (2 s here). Since #98 a
     * stretch is classified by the marker majority rule at the tick AND at
     * its close, so this reclassification is the rare majority flip of a
     * still-growing stretch (test 6 shows the common case no longer flips);
     * the clamp and its metric stay as the fail-safe. */
    memcpy(view, closed, sizeof(*view));
    struct pgwt_live_interval open3 = {
        .pid = 3, .we = 0, .wall_ns = MS(2000), .cpu_ns = MS(1500),
        .cmd_gate_active = true, .cmd_open = true, .closed = false,
    };
    pgwt_accum_add_interval(view, &open3);
    pgwt_ring_push(&ring, view);                       /* snapshot A */
    struct pgwt_live_interval c8 = closed_fg(1, IO_WALSYNC, MS(3000), 1, 0);
    struct pgwt_live_interval c9 = closed_fg(3, 0, MS(2500), 0, 0);   /* gate-clear */
    pgwt_accum_add_interval(closed, &c8);
    pgwt_accum_add_interval(closed, &c9);
    memcpy(view, closed, sizeof(*view));
    pgwt_ring_push(&ring, view);                       /* snapshot B */
    CHECK(pgwt_ring_delta(&ring, 1, d) == 0, "mixed-window delta");
    CHECK(d->tm.db_time_ns == MS(3000) - MS(2000),
          "window DB Time = wait added - reclassified wall = %llu ns (> 0, not clamped)",
          (unsigned long long)d->tm.db_time_ns);
    CHECK(snap_row(d, IO_WALSYNC) == MS(3000), "IO row delta = the 3 s wait");
    double pio = pct_db(snap_row(d, IO_WALSYNC), d->tm.db_time_ns);
    CHECK(pio > 100.0 && pio == 300.0,   /* exact: 100 * 3e9 / 1e9 */
          "residual: a WAIT row can still read > 100%% of the window (%.0f%%), "
          "bounded by the reclassified stretch", pio);
    CHECK(d->tm.cpu_time_ns == 0 && snap_row(d, 0) == 0,
          "CPU delta clamped at 0 for the reclassified run");
    CHECK(d->clamped_fields == 3, "clamped_fields = %u (tm.cpu, CPU* count, CPU* total)",
          d->clamped_fields);
    CHECK(snap_row(d, PGWT_WEI_NONCMD_CPU) == MS(2500),
          "…the run is whole under NonCommandCpu");

    free(d);
    free(view);
    free(closed);
    pgwt_ring_free(&ring);
}

/* ═══ Issue #98: the marker-driven command gate ═══════════════════════════
 *
 * The live gate used to be the value BPF read AT EMISSION of the closing
 * record. PostgreSQL reports STATE_IDLE (CMD_END) before the post-command
 * ClientRead begins, so the record that closes a waitless statement's
 * whole on-CPU run always carried a CLEAR gate — the run was filed as
 * non-command, and only CPU preceding an in-command wait counted (~83% of
 * pgbench's CPU dropped live; the 4.6-7.1x server/CLI CPU ratio). The live
 * paths now sweep the CMD_START/CMD_END markers and apply the server's
 * majority rule (compute.c pgwt_tag_events). */

/* A per-pid live stream driver: markers feed the gate, records are
 * classified (consumed) and folded through pgwt_accum_add_interval exactly
 * as event_stream.c does. `emit_flag` is what BPF stamped at emission — the
 * OLD rule, kept only to show the shape of the input. */
struct stream {
    struct pgwt_accumulator *acc;
    struct pgwt_pid_accum *pa;
    uint32_t pid;
    uint64_t we0_total;          /* Σ wall of every we==0 record */
    uint64_t we0_emit_open;      /* … of which stamped CMD_OPEN at emission */
    int      records;
};

static void stream_init(struct stream *s, struct pgwt_accumulator *acc,
                        uint32_t pid)
{
    memset(s, 0, sizeof(*s));
    s->acc = acc;
    s->pid = pid;
    s->pa = pgwt_get_or_create_pid(acc, pid);
}

static void stream_marker(struct stream *s, uint32_t marker, uint64_t ts)
{
    pgwt_live_cmd_gate_marker(&s->pa->cmd_gate, marker, ts);
}

/* Closed record [t0, t1) of `we`; emit_open = the emission-time gate. */
static bool stream_record(struct stream *s, uint32_t we, uint64_t t0,
                          uint64_t t1, int emit_open, uint64_t qid)
{
    bool in_cmd = false;
    pgwt_live_cmd_gate_classify(&s->pa->cmd_gate, true, t0, t1,
                                emit_open != 0, &in_cmd);
    struct pgwt_live_interval iv = {
        .pid = s->pid, .we = we, .wall_ns = t1 - t0, .cpu_ns = t1 - t0,
        .query_id = qid, .cat_flag = 0,
        .cmd_gate_active = true, .cmd_open = in_cmd, .closed = true,
        .pa = s->pa,
    };
    pgwt_accum_add_interval(s->acc, &iv);
    s->records++;
    if (we == 0) {
        s->we0_total += t1 - t0;
        if (emit_open)
            s->we0_emit_open += t1 - t0;
    }
    return in_cmd;
}

#define US(x) ((uint64_t)(x) * 1000ULL)

/* ── 5. The #98 reproduction: pgbench-shaped stream WITH its markers ──── */
static void test_cmd_gate_waitless_statement(void)
{
    printf("--- #98: marker-driven gate, waitless / waiting / trailing statements ---\n");
    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    struct stream s;
    stream_init(&s, acc, 100);

    /* Idle read. */
    stream_record(&s, PG_WAIT_CLIENT_READ, MS(0), MS(10), 0, 0);

    /* (a) A WAITLESS statement: the query message ends ClientRead at 10 ms,
     * STATE_RUNNING (CMD_START) at 10.2, the whole UPDATE runs on CPU,
     * STATE_IDLE (CMD_END) at 13.9, and the next ClientRead begins at 14 —
     * which is when the we==0 record [10, 14) is emitted, gate ALREADY
     * CLEAR. 3.7 of its 4 ms overlap the command: in-command. */
    stream_marker(&s, PGWT_MARKER_CMD_START, MS(10) + US(200));
    stream_marker(&s, PGWT_MARKER_CMD_END,   MS(13) + US(900));
    bool a = stream_record(&s, 0, MS(10), MS(14), 0, 0xA);
    CHECK(a, "(a) waitless statement's whole on-CPU run is in-command");
    stream_record(&s, PG_WAIT_CLIENT_READ, MS(14), MS(20), 0, 0);

    /* (b) A statement WITH a wait: CPU [20, 21) is closed by the WALSync
     * wait (gate open at emission — the only CPU the old rule counted),
     * the wait [21, 22), then the commit's trailing CPU [22, 22.5) closed
     * by the next ClientRead after CMD_END at 22.4: 0.4 of 0.5 ms
     * in-command. */
    stream_marker(&s, PGWT_MARKER_CMD_START, MS(20) + US(100));
    bool b1 = stream_record(&s, 0, MS(20), MS(21), 1, 0xB);
    bool b2 = stream_record(&s, IO_WALSYNC, MS(21), MS(22), 1, 0xB);
    stream_marker(&s, PGWT_MARKER_CMD_END, MS(22) + US(400));
    bool b3 = stream_record(&s, 0, MS(22), MS(22) + US(500), 0, 0xB);
    CHECK(b1 && b2 && b3, "(b) CPU before the wait, the wait, and the trailing "
          "CPU are all in-command (%d %d %d)", b1, b2, b3);
    stream_record(&s, PG_WAIT_CLIENT_READ, MS(22) + US(500), MS(30), 0, 0);

    /* (c) A short command followed by a long post-command on-CPU run:
     * CMD_START 30.1, CMD_END 31, ClientRead at 34 → 0.9 of 4 ms overlap:
     * the majority rule says NON-command — same verdict as the server. */
    stream_marker(&s, PGWT_MARKER_CMD_START, MS(30) + US(100));
    stream_marker(&s, PGWT_MARKER_CMD_END,   MS(31));
    bool c = stream_record(&s, 0, MS(30), MS(34), 0, 0xC);
    CHECK(!c, "(c) a run mostly after its command is non-command (majority)");
    stream_record(&s, PG_WAIT_CLIENT_READ, MS(34), MS(40), 0, 0);

    /* The input's shape — the OLD (emission-time) rule would have kept only
     * the 1 ms of CPU that preceded the in-command wait: the #98 ratio. */
    CHECK(s.we0_emit_open == MS(1) && s.we0_total == MS(9) + US(500),
          "input shape: %.1f ms of %.1f ms we==0 was stamped CMD_OPEN at "
          "emission (the old rule's CPU*)",
          s.we0_emit_open / 1e6, s.we0_total / 1e6);

    uint64_t cpu_row = sys_row(acc, 0);
    uint64_t noncmd_row = sys_row(acc, PGWT_WEI_NONCMD_CPU);
    CHECK(cpu_row == MS(4) + MS(1) + US(500),
          "CPU* row = 4 + 1 + 0.5 ms in-command CPU (got %.2f ms)", cpu_row / 1e6);
    CHECK(noncmd_row == MS(4),
          "NonCommandCpu row = the 4 ms post-command run (got %.2f ms)",
          noncmd_row / 1e6);
    CHECK(acc->tm.cpu_time_ns == cpu_row, "time-model CPU == CPU* row");
    CHECK(acc->tm.db_time_ns == cpu_row + MS(1),
          "DB Time = CPU* + WAL wait = %.2f ms", acc->tm.db_time_ns / 1e6);
    CHECK(acc->tm.activity_time_ns == MS(10) + MS(6) + (MS(7) + US(500)) +
                                      MS(6) + MS(4),
          "Activity = ClientReads + non-command CPU (got %.2f ms)",
          acc->tm.activity_time_ns / 1e6);
    CHECK(sys_nonidle_sum(acc) == acc->tm.db_time_ns,
          "Σ non-idle rows == DB Time");
    /* Per-query rows: the waitless statement's CPU lands on ITS query. */
    uint64_t qa = 0, qc_noncmd = 0;
    for (int i = 0; i < acc->num_query_events; i++) {
        if (acc->query_events[i].query_id == 0xA && acc->query_events[i].wait_event == 0)
            qa = acc->query_events[i].total_ns;
        if (acc->query_events[i].query_id == 0xC &&
            acc->query_events[i].wait_event == PGWT_WEI_NONCMD_CPU)
            qc_noncmd = acc->query_events[i].total_ns;
    }
    CHECK(qa == MS(4), "query A's row carries its 4 ms CPU (got %.2f ms)", qa / 1e6);
    CHECK(qc_noncmd == MS(4), "query C's post-command run is its NonCommandCpu row");
    free(acc);
}

/* ── 6. Straddling command at the window edge: the OPEN stretch ────────── */
static void test_cmd_gate_open_stretch(void)
{
    printf("--- #98: open stretch peeks the gate; its close consumes it ---\n");
    struct pgwt_ring ring;
    CHECK(pgwt_ring_init(&ring, 4) == 0, "ring init");
    struct pgwt_accumulator *closed = calloc(1, sizeof(*closed));
    struct pgwt_accumulator *view = calloc(1, sizeof(*view));
    pgwt_accum_init(closed);
    struct stream s;
    stream_init(&s, closed, 7);

    /* ClientRead ends at 40 ms; CMD_START at 40.1; the statement is still
     * on CPU at the tick (now = 45 ms). state_map: last_ts = 40, we = 0,
     * cmd_open = 1. */
    stream_record(&s, PG_WAIT_CLIENT_READ, MS(0), MS(40), 0, 0);
    stream_marker(&s, PGWT_MARKER_CMD_START, MS(40) + US(100));
    struct pgwt_live_cmd_gate before = s.pa->cmd_gate;

    /* Tick A: copy (as pgwt_accum_copy_used does) and add the open stretch
     * through the same classify (peek). */
    memcpy(view, closed, sizeof(*view));
    struct pgwt_pid_accum *vpa = pgwt_find_pid_accum(view, 7);
    CHECK(vpa != NULL, "view copy carries the pid entry (and its gate)");
    bool in_cmd = false;
    bool by_markers = pgwt_live_cmd_gate_classify(&vpa->cmd_gate, false,
                                                  MS(40), MS(45), false, &in_cmd);
    CHECK(by_markers && in_cmd, "open stretch [40, 45) is in-command by markers "
          "(state_map's instantaneous gate is not consulted)");
    CHECK(memcmp(&vpa->cmd_gate, &before, sizeof(before)) == 0,
          "peek does not advance the sweep");
    struct pgwt_live_interval open1 = {
        .pid = 7, .we = 0, .wall_ns = MS(5), .cpu_ns = MS(4) + US(800),
        .query_id = 0x7, .cmd_gate_active = true, .cmd_open = in_cmd,
        .closed = false, .pa = vpa,
    };
    pgwt_accum_add_interval(view, &open1);
    CHECK(view->tm.cpu_time_ns == MS(4) + US(800) && view->tm.db_time_ns == MS(5),
          "tick A: CPU* = measured 4.8 ms, DB Time = wall 5 ms");
    pgwt_ring_push(&ring, view);

    /* The command ends at 46 (CMD_END), the ClientRead begins at 46.2: the
     * record [40, 46.2) is emitted gate-clear. Consumed: 5.9 of 6.2 ms
     * in-command → CPU*, the same label the open stretch had. */
    stream_marker(&s, PGWT_MARKER_CMD_END, MS(46));
    bool closed_in_cmd = stream_record(&s, 0, MS(40), MS(46) + US(200), 0, 0x7);
    CHECK(closed_in_cmd, "the closing record is in-command too");
    CHECK(s.pa->cmd_gate.banked_ns == 0 && !s.pa->cmd_gate.open,
          "consume banks the closed run and resets");

    /* Tick B: no open stretch (pid is in ClientRead now; a wait, so the
     * has_closed_data guard applies) — snapshot the closed accumulator. */
    memcpy(view, closed, sizeof(*view));
    pgwt_ring_push(&ring, view);
    struct pgwt_snapshot *d = calloc(1, sizeof(*d));
    CHECK(pgwt_ring_delta(&ring, 1, d) == 0, "delta A→B");
    CHECK(d->clamped_fields == 0,
          "no clamp: the stretch closed under the SAME label it was shown "
          "under (clamped_fields = %u)", d->clamped_fields);
    CHECK(d->tm.db_time_ns == MS(6) + US(200) - MS(5),
          "window DB Time = the closed wall beyond the open stretch (%.2f ms)",
          d->tm.db_time_ns / 1e6);
    CHECK(snap_row(d, PGWT_WEI_NONCMD_CPU) == 0,
          "nothing moved to NonCommandCpu across the window");

    /* A straddling stretch that the gate CLOSED mid-way, still open at the
     * tick: CMD_START 50.1, CMD_END 51, on CPU since 50, now = 56 → 0.9 of
     * 6 ms: non-command at the tick — and at its close (same rule). */
    stream_record(&s, PG_WAIT_CLIENT_READ, MS(46) + US(200), MS(50), 0, 0);
    stream_marker(&s, PGWT_MARKER_CMD_START, MS(50) + US(100));
    stream_marker(&s, PGWT_MARKER_CMD_END,   MS(51));
    in_cmd = true;
    pgwt_live_cmd_gate_classify(&s.pa->cmd_gate, false, MS(50), MS(56), true, &in_cmd);
    CHECK(!in_cmd, "open post-command run: non-command at the tick");
    CHECK(!stream_record(&s, 0, MS(50), MS(57), 0, 0x8),
          "…and non-command when it closes (no reclassification)");

    free(d);
    free(view);
    free(closed);
    pgwt_ring_free(&ring);
}

/* ── 7. Marker-less fallback: explicit, never a silent guess ───────────── */
static void test_cmd_gate_fallback(void)
{
    printf("--- #98: unmarked pid / no state / gate inactive ---\n");
    struct pgwt_live_cmd_gate g = {0};
    bool in_cmd = false;

    /* Unmarked pid (no CMD marker ever seen): the server's seen_cmd rule —
     * CPU stays CPU; the caller counts live_cpu_unmarked_ns_total. */
    CHECK(!pgwt_live_cmd_gate_classify(&g, true, MS(0), MS(5), false, &in_cmd)
          && in_cmd, "unmarked pid: not decided by markers, in-command");
    CHECK(g.anchor_ns == 0 && g.banked_ns == 0 && !g.seen,
          "…and the sweep stays untouched until the first marker");
    /* First marker is a CMD_END (attached mid-command): nothing to bank,
     * but from here on the pid is marked and post-command CPU is idle. */
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(5) + US(500));
    CHECK(g.seen && !g.open && g.banked_ns == 0, "CMD_END first: seen, nothing banked");
    CHECK(pgwt_live_cmd_gate_classify(&g, true, MS(5), MS(9), true, &in_cmd)
          && !in_cmd, "after it, a we==0 record is non-command even though "
          "the emission flag said open");
    /* Plan/exec/escalation markers never touch the gate. */
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_EXEC_START, MS(9));
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_ESCALATE_START, MS(9));
    CHECK(!g.open && g.banked_ns == 0, "non-command markers are ignored");
    /* Duplicate CMD_START (a lost CMD_END) keeps the first anchor; a
     * CMD_END without an open window banks nothing. */
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(10));
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(12));
    CHECK(g.open && g.anchor_ns == MS(10), "duplicate CMD_START keeps the anchor");
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(13));
    pgwt_live_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(14));
    CHECK(!g.open && g.banked_ns == MS(3), "duplicate CMD_END banks once (3 ms)");

    /* No per-pid state at all (accumulator full): the emission-time gate
     * is the explicit fallback (counted as live_cpu_gate_fallback_total). */
    CHECK(!pgwt_live_cmd_gate_classify(NULL, true, MS(0), MS(1), true, &in_cmd)
          && in_cmd, "no state: falls back to the emission flag (open)");
    CHECK(!pgwt_live_cmd_gate_classify(NULL, true, MS(0), MS(1), false, &in_cmd)
          && !in_cmd, "no state: falls back to the emission flag (clear)");

    /* Majority rule corner cases, identical to pgwt_tag_events. */
    CHECK(pgwt_live_cmd_majority(MS(2), MS(4), false), "exactly half: in-command");
    CHECK(!pgwt_live_cmd_majority(MS(2) - 1, MS(4), true), "just under half: not");
    CHECK(pgwt_live_cmd_majority(0, 0, true) && !pgwt_live_cmd_majority(0, 0, false),
          "zero-length interval takes the instantaneous gate");

    /* Gate inactive on the daemon: the classification is bypassed entirely
     * (pgwt_live_effective_event keeps we==0 as CPU*) — the header/status
     * says "ungated". */
    CHECK(pgwt_live_effective_event(0, 0, false, false) == 0,
          "gate inactive: we==0 stays CPU* regardless of the sweep");

    /* Open-stretch peek clips to the stretch's wall when banked time
     * predates it (state_map's last_ts ahead of the last drained record). */
    struct pgwt_live_cmd_gate lag = { .seen = 1, .open = 0, .banked_ns = MS(9) };
    CHECK(pgwt_live_cmd_gate_peek(&lag, MS(100), MS(103)) == MS(3),
          "peek clips banked time to the stretch wall");
}

/* ── 8. Cross-check: the live sweep == the server's pgwt_tag_events ────── */

/* compute.c's only foreign symbol (summary streaming) — unused here. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns; (void)visitor; (void)ctx;
    return -1;
}

static uint32_t lcg_state = 0x9E3779B9u;
static uint32_t lcg(uint32_t n)   /* deterministic pseudo-random in [0, n) */
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 8) % n;
}

static void test_cross_check_tag_events(void)
{
    printf("--- #98: live sweep vs compute.c pgwt_tag_events on one stream ---\n");
    enum { NPIDS = 3, MAXEV = 6000 };
    struct pgwt_trace_event *ev = calloc(MAXEV, sizeof(*ev));
    int n = 0;
    uint64_t t[NPIDS] = { US(0), US(300), US(700) };   /* per-pid clocks */
    int mid_cmd[NPIDS] = { 0, 0, 0 };                   /* pid 2 attaches mid-command */
    mid_cmd[2] = 1;

    /* Generate interleaved per-pid streams of pgbench-like statements:
     * ClientRead → [CMD_START] → CPU (→ wait → CPU)* → [CMD_END] → ClientRead,
     * with random phase offsets so commands start after the run begins and
     * end before/after it ends, plus lost markers and back-to-back
     * commands. Each pid's records are time-ordered; pids interleave. */
    while (n < MAXEV - 16) {
        int p = lcg(NPIDS);
        uint32_t pid = 100 + p;
        #define EMIT(_we, _dur, _flags) do { \
            ev[n++] = (struct pgwt_trace_event){ .timestamp_ns = t[p], .pid = pid, \
                .old_event = (_we), .new_event = 0, .flags = (_flags), \
                .duration_ns = (_dur), .cpu_ns = (_dur) }; } while (0)
        #define MARK(_m) do { \
            ev[n++] = (struct pgwt_trace_event){ .timestamp_ns = t[p], .pid = pid, \
                .old_event = (_m), .new_event = (_m), .duration_ns = 0 }; } while (0)

        if (!mid_cmd[p]) {
            t[p] += US(50 + lcg(2000));  EMIT(PG_WAIT_CLIENT_READ, US(50 + lcg(2000)) , 0);
            /* command opens a little after the on-CPU run began */
            uint64_t run_start = t[p];
            t[p] += US(lcg(40));
            if (lcg(10) != 0) MARK(PGWT_MARKER_CMD_START);   /* 10%: lost START */
            (void)run_start;
        }
        mid_cmd[p] = 0;
        int waits = lcg(3);              /* 0-2 mid-command waits */
        for (int w = 0; w < waits; w++) {
            t[p] += US(20 + lcg(500));   EMIT(0, US(20 + lcg(500)), PGWT_EVENT_FLAG_CMD_OPEN);
            t[p] += US(10 + lcg(300));   EMIT(IO_WALSYNC, US(10 + lcg(300)), PGWT_EVENT_FLAG_CMD_OPEN);
        }
        /* trailing CPU: the command ends somewhere inside it */
        uint64_t cpu_start = t[p];
        uint64_t cpu_len = US(20 + lcg(3000));
        uint64_t end_off = lcg(4) == 0 ? cpu_len * 3 / 4 : cpu_len - US(lcg(20));
        if (end_off > cpu_len) end_off = cpu_len;
        t[p] = cpu_start + end_off;
        if (lcg(12) != 0) MARK(PGWT_MARKER_CMD_END);        /* 8%: lost END */
        t[p] = cpu_start + cpu_len;
        EMIT(0, cpu_len, 0);   /* closes at the next ClientRead: gate clear */
        if (lcg(6) == 0)       /* back-to-back command without a ClientRead */
            mid_cmd[p] = 1;
        #undef EMIT
        #undef MARK
    }

    /* Sort by timestamp (stable for equal stamps: insertion order) so the
     * array is the merged, time-ordered stream the server tags. */
    for (int i = 1; i < n; i++) {
        struct pgwt_trace_event x = ev[i];
        int j = i - 1;
        while (j >= 0 && ev[j].timestamp_ns > x.timestamp_ns) { ev[j + 1] = ev[j]; j--; }
        ev[j + 1] = x;
    }

    /* Server side. */
    struct pgwt_trace_event *srv = calloc(n, sizeof(*srv));
    memcpy(srv, ev, n * sizeof(*ev));
    pgwt_tag_events(srv, n, NULL, 0);

    /* Live side: the same stream through the sweep, record by record. */
    struct pgwt_live_cmd_gate gates[NPIDS];
    memset(gates, 0, sizeof(gates));
    int records = 0, disagreements = 0, noncmd_live = 0, unmarked = 0;
    for (int i = 0; i < n; i++) {
        struct pgwt_live_cmd_gate *g = &gates[ev[i].pid - 100];
        if (PGWT_IS_MARKER(ev[i].old_event)) {
            pgwt_live_cmd_gate_marker(g, ev[i].old_event, ev[i].timestamp_ns);
            continue;
        }
        uint64_t t1 = ev[i].timestamp_ns, t0 = t1 - ev[i].duration_ns;
        bool in_cmd = false;
        bool by_markers = pgwt_live_cmd_gate_classify(g, true, t0, t1,
            (ev[i].flags & PGWT_EVENT_FLAG_CMD_OPEN) != 0, &in_cmd);
        uint32_t live_we = pgwt_live_effective_event(ev[i].old_event, 0, true, in_cmd);
        records++;
        if (!by_markers) unmarked++;
        if (live_we == PGWT_WEI_NONCMD_CPU) noncmd_live++;
        if (live_we != srv[i].old_event) {
            if (disagreements < 5)
                printf("  disagree #%d: pid %u we=0x%x [%llu,%llu) server=0x%x live=0x%x\n",
                       i, ev[i].pid, ev[i].old_event,
                       (unsigned long long)t0, (unsigned long long)t1,
                       srv[i].old_event, live_we);
            disagreements++;
        }
    }
    CHECK(records > 2000, "generated %d records (%d events)", records, n);
    CHECK(noncmd_live > 50 && noncmd_live < records / 2,
          "the stream exercises both verdicts (%d non-command records)", noncmd_live);
    CHECK(unmarked > 0, "…and the unmarked prefix of the mid-command pid (%d records)",
          unmarked);
    CHECK(disagreements == 0,
          "live classification == pgwt_tag_events on every record (%d disagreements)",
          disagreements);
    free(srv);
    free(ev);
}

int main(void)
{
    test_effective_event();
    test_closed_noncmd_cpu_row();
    test_open_interval();
    test_ring_delta();
    test_cmd_gate_waitless_statement();
    test_cmd_gate_open_stretch();
    test_cmd_gate_fallback();
    test_cross_check_tag_events();
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
