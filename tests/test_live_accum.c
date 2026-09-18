/* test_live_accum.c — live-view interval accounting (issue #97).
 *
 * The multi-window system_event view showed CPU* at 128-142% of DB Time
 * (tests/test_multi_window.py "Non-idle top-level %DB", PG18 gate box). Root
 * cause: the closed-record path filed a client backend's NON-COMMAND on-CPU
 * record (we==0, command gate closed at emission) under the CPU* row while
 * routing its time to the idle Activity bucket — so the CPU* row grew by
 * time DB Time never contained. Both live paths (closed record and open
 * state_map stretch) now fold through pgwt_accum_add_interval with ONE
 * classification (pgwt_live_effective_event), and the ring delta saturates
 * instead of wrapping when an open stretch closes under a different label.
 *
 * Pure: links map_reader.c (-DPGWT_SERVER, BPF-free core) + snapshot.c.
 * No daemon, no PostgreSQL, no root. */
#include "map_reader.h"
#include "snapshot.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

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
     * bounded by the reclassified stretch's wall (2 s here) and is #98's
     * (gate read at emission) — the roadmap entry states it. */
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

int main(void)
{
    test_effective_event();
    test_closed_noncmd_cpu_row();
    test_open_interval();
    test_ring_delta();
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
