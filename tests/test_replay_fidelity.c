/* test_replay_fidelity.c — T1/FID-5: --replay is fidelity-aware.
 *
 * Before the fix, pgwt_replay_events consumed old_event/duration_ns
 * blindly: decoded SAMPLES records (old_event = 0, duration = 0) inflated
 * the CPU bucket's count while contributing zero time — a tiered trace
 * replayed as a near-idle database — and markers created junk "Unknown"
 * entries in every accumulator table.
 *
 * This test writes a synthetic trace (transitions with exec + escalation
 * markers, then a SAMPLES block) through the real writer, decodes it with
 * the real reader, replays it through the real accumulation, and asserts:
 *   - exact waits accumulate exactly (count, total, histogram);
 *   - each sample is worth one sample_period_ns of estimated time for
 *     counts / time model / per-query totals;
 *   - samples contribute NOTHING to histograms/min/max (no fabricated
 *     latencies) and do not inflate the CPU bucket;
 *   - markers are skipped everywhere (no marker-id event entries, no
 *     PGWT_ESC_PACK query ids);
 *   - the replay stats report what was accumulated (for the loud notice).
 *
 * Links the BPF-free accumulator core (map_reader.c with -DPGWT_SERVER)
 * plus the reader and replay_accum.c — no bpftool needed.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "map_reader.h"
#include "compute.h"
#include "summary_reader.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* compute.c's summary-stream entry point is unused by this raw comparison. */
int pgwt_visit_summaries(const char *dir, uint64_t from, uint64_t to,
                         pgwt_summary_visitor visitor, void *ctx)
{
    (void)dir; (void)from; (void)to; (void)visitor; (void)ctx;
    return -1;
}

#define IO_READ  0x0A000015u   /* IO:DataFileRead */
#define LOCK_REL 0x03000000u   /* Lock:relation */

/* An unclassified flag bit standing in for a category a future build adds.
 * The inverse mask in replay_accum.c must route it to the total refusal
 * rather than the back-fill. */
#define FUTURE_CAT_FLAG 0x2000u

static int failures = 0;
/* #531 bypass suite: EXPECT_RED runs a probe that MUST register a failure,
 * so the probe's own PASS/FAIL lines are noise. `quiet` silences them; it
 * never silences a failure COUNT, which is what EXPECT_RED inspects. */
static int quiet = 0;

#define CHECK(cond, msg) do { \
    if (cond) { if (!quiet) printf("  PASS: %s\n", msg); } \
    else      { if (!quiet) printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

#define CHECK_EQ_U64(got, want, msg) do { \
    uint64_t g_ = (got), w_ = (want); \
    if (g_ == w_) { if (!quiet) printf("  PASS: %s\n", msg); } \
    else { if (!quiet) printf("  FAIL: %s (got %llu, want %llu)\n", msg, \
                  (unsigned long long)g_, (unsigned long long)w_); \
           failures++; } \
} while (0)

/* The gate's own detection test. `stmt` is an assertion block that is
 * SUPPOSED to go red; EXPECT_RED fails if it stays green, and absorbs the
 * failures it provoked so they do not count against the suite. Every defect
 * this repo keeps finding is a FALSE negative -- a gate that could not see
 * -- so each way of blinding the CPU*-row check gets one of these. */
#define EXPECT_RED(desc, stmt) do { \
    int before_ = failures; \
    quiet = 1; { stmt; } quiet = 0; \
    int raised_ = failures - before_; \
    failures = before_; \
    if (raised_ > 0) printf("  PASS: %s (gate raised %d)\n", desc, raised_); \
    else { printf("  FAIL: %s (gate stayed GREEN -- it cannot see)\n", desc); \
           failures++; } \
} while (0)

static struct pgwt_trace_event ev(uint32_t pid, uint64_t ts, uint64_t dur,
                                  uint32_t old_e, uint32_t new_e,
                                  uint64_t qid)
{
    struct pgwt_trace_event e = {0};
    e.pid = pid;
    e.timestamp_ns = ts;
    e.duration_ns = dur;
    e.old_event = old_e;
    e.new_event = new_e;
    e.query_id = qid;
    return e;
}

static const struct pgwt_event_stats *
find_sys(const struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == we &&
            acc->system_events[i].count > 0)
            return &acc->system_events[i];
    return NULL;
}

static const struct pgwt_event_stats *
find_pid_ev(struct pgwt_accumulator *acc, uint32_t pid, uint32_t we)
{
    struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, pid);
    if (!pa) return NULL;
    for (int i = 0; i < pa->num_events; i++)
        if (pa->events[i].wait_event == we && pa->events[i].count > 0)
            return &pa->events[i];
    return NULL;
}

static const struct pgwt_query_event_stats *
find_query_row(struct pgwt_accumulator *acc, uint64_t qid, uint32_t we)
{
    for (int i = 0; i < acc->num_query_events; i++)
        if (acc->query_events[i].query_id == qid &&
            acc->query_events[i].wait_event == we)
            return &acc->query_events[i];
    return NULL;
}

/* The rendering finder must reject invisible rows (#531). Clipped-time
 * assertions use the raw row lookup explicitly when count == 0 is intended. */
static const struct pgwt_query_event_stats *
find_query_ev(struct pgwt_accumulator *acc, uint64_t qid, uint32_t we)
{
    const struct pgwt_query_event_stats *q = find_query_row(acc, qid, we);
    return q && q->count > 0 ? q : NULL;
}

#define FIX_QID 700ULL
#define ROW_ABSENT UINT64_MAX   /* "there must be no CPU* row at all" */
#define OCC_UNSET  UINT64_MAX   /* "the occurrence columns stay untouched" */

/* #531: every per-event table that renders a "CPU*" row -- system_event,
 * session_event (per-PID), query_event -- must carry the MEASURED on-CPU ns,
 * the same figure src/output.c prints as time_model's CPU*. One replay
 * invocation printing that one label twice with two different numbers is the
 * defect; src/output.c:print_offcpu_row_wide (#498) then stacks the off-CPU
 * remainder on a row that already holds the whole wall, so system_event's
 * %DB column sums past 100%.
 *
 * The assertions are ABSOLUTE (4 ms measured inside a 10 ms wall), not
 * "the rows agree": agreement is satisfied by both rows being wall, which is
 * precisely the state before #495 and before this change. `row_total` is the
 * expected window-clipped total; `row_occ` the expected min/max, a SEPARATE
 * axis the half-window case pins at 4 ms while the total is 2 ms -- a fix
 * that only moves total_ns leaves max/min and the histogram on wall.
 *
 * A row that ought to exist and does not is a FAILURE, never a skipped
 * `if (row)`: that vacuous shape is how this family of test goes blind. */
static void check_cpu_rows(struct pgwt_accumulator *acc, uint32_t pid,
                           uint64_t row_total, uint64_t row_occ)
{
    const struct pgwt_event_stats *se = find_sys(acc, 0);
    const struct pgwt_event_stats *pe = find_pid_ev(acc, pid, 0);
    const struct pgwt_query_event_stats *qe = find_query_ev(acc, FIX_QID, 0);

    if (row_total == ROW_ABSENT) {
        CHECK(se == NULL, "no system_event CPU* row for a wait-labelled record");
        CHECK(pe == NULL, "no session_event CPU* row for a wait-labelled record");
        CHECK(qe == NULL, "no query_event CPU* row for a wait-labelled record");
        return;
    }

    CHECK(se != NULL, "system_event CPU* row present");
    CHECK(pe != NULL, "session_event CPU* row present");
    CHECK(qe != NULL, "query_event CPU* row present");
    if (se) {
        CHECK_EQ_U64(se->total_ns, row_total, "system_event CPU* total (ns)");
        CHECK_EQ_U64(se->total_ns, acc->tm.cpu_time_ns,
                     "system_event CPU* == time_model CPU* in one invocation");
        if (row_occ == OCC_UNSET) {
            CHECK_EQ_U64(se->max_ns, 0, "CPU* max unset for a sample");
            CHECK_EQ_U64(se->min_ns, UINT64_MAX, "CPU* min unset for a sample");
        } else {
            CHECK_EQ_U64(se->max_ns, row_occ, "system_event CPU* max (ns)");
            CHECK_EQ_U64(se->min_ns, row_occ, "system_event CPU* min (ns)");
        }
    }
    if (pe)
        CHECK_EQ_U64(pe->total_ns, row_total, "session_event CPU* total (ns)");
    if (qe)
        CHECK_EQ_U64(qe->total_ns, row_total, "query_event CPU* total (ns)");
}

/* #495: absolute CPU values catch full-wall replay; conservation alone
 * cannot, since off-CPU is calculated as wall minus CPU. Each isolated
 * case gets a fresh accumulator and a record decoded from the real writer. */
static void check_cpu_replay(const struct pgwt_trace_event *e,
                              const struct pgwt_block_info *bi,
                              uint64_t from, uint64_t to,
                              uint64_t cpu, uint64_t offcpu, uint64_t db,
                              uint64_t row_total, uint64_t row_occ)
{
    struct pgwt_accumulator *acc = malloc(sizeof(*acc));
    if (!acc) { perror("malloc"); exit(1); }
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, e, 1, from, to, bi, NULL);
    pgwt_replay_finish(acc);
    CHECK_EQ_U64(acc->tm.cpu_time_ns, cpu, "time model CPU (ns)");
    CHECK_EQ_U64(acc->tm.offcpu_time_ns, offcpu, "time model off-CPU (ns)");
    CHECK_EQ_U64(acc->tm.db_time_ns, db, "time model DB wall (ns)");
    struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, e->pid);
    CHECK(pa != NULL, "record's PID accumulated");
    if (pa) {
        CHECK_EQ_U64(pa->cpu_time_ns, cpu, "per-PID CPU agrees (ns)");
        CHECK_EQ_U64(pa->db_time_ns, db, "per-PID DB keeps wall (ns)");
    }
    if (e->old_event == IO_READ) {
        CHECK_EQ_U64(acc->tm.io_time_ns, db, "IO keeps full wall (ns)");
        if (pa)
            CHECK_EQ_U64(pa->wait_time_ns, db, "per-PID wait keeps wall (ns)");
        const struct pgwt_event_stats *io = find_sys(acc, IO_READ);
        CHECK(io != NULL, "system_event IO row present");
        if (io)
            CHECK_EQ_U64(io->total_ns, db,
                         "system_event IO row keeps full wall (ns)");
    }
    check_cpu_rows(acc, e->pid, row_total, row_occ);
    free(acc);
}

/* Re-stamp every per-event CPU* row, leaving the time model alone. Used only
 * by the bypass suite below, to build the exact shape of the #531 defect
 * (per-event rows on wall, time model on measured) without reverting the
 * product -- so the gate's detection is proved on this tree, not inferred. */
static void stamp_cpu_rows(struct pgwt_accumulator *acc, uint32_t pid,
                           uint64_t total, uint64_t occ, uint64_t count)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == 0) {
            acc->system_events[i].total_ns = total;
            acc->system_events[i].min_ns = occ;
            acc->system_events[i].max_ns = occ;
            acc->system_events[i].count = count;
        }
    struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, pid);
    if (pa)
        for (int i = 0; i < pa->num_events; i++)
            if (pa->events[i].wait_event == 0) {
                pa->events[i].total_ns = total;
                pa->events[i].min_ns = occ;
                pa->events[i].max_ns = occ;
                pa->events[i].count = count;
            }
    for (int i = 0; i < acc->num_query_events; i++)
        if (acc->query_events[i].wait_event == 0 &&
            acc->query_events[i].query_id == FIX_QID) {
            acc->query_events[i].total_ns = total;
            acc->query_events[i].count = count;
        }
}

/* #531 BYPASS SUITE -- every way the CPU*-row check could be made unable to
 * see, each one asserted to go RED rather than quietly approve. True
 * positives were never the broken path in this repo; false negatives were:
 * a guard that passed because something else in the range changed, a probe
 * that passed against a working dependency, an implication guard that was
 * vacuous because its premise was false. So: the row absent instead of
 * wrong, nothing replayed at all, the row carrying wall, only total_ns
 * corrected, the row present but invisible to the finder, and an UNKNOWN
 * measurement offered in place of a measured one. */
static void test_cpu_row_bypass(const struct pgwt_trace_event *measured,
                                const struct pgwt_trace_event *wait_labelled,
                                const struct pgwt_block_info *bi)
{
    const uint64_t MS = 1000000ULL;
    struct pgwt_accumulator *acc = malloc(sizeof(*acc));
    if (!acc) { perror("malloc"); exit(1); }

    printf("CASE #531 bypass: the CPU* row is ABSENT, not wrong\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, wait_labelled, 1, 0, 0, bi, NULL);
    pgwt_replay_finish(acc);
    EXPECT_RED("an absent CPU* row refuses, never approves",
               check_cpu_rows(acc, wait_labelled->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: nothing was replayed at all\n");
    pgwt_accum_init(acc);
    EXPECT_RED("an empty accumulator refuses",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: per-event rows carry WALL (the defect itself)\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, measured, 1, 0, 0, bi, NULL);
    pgwt_replay_finish(acc);
    stamp_cpu_rows(acc, measured->pid, 10 * MS, 10 * MS, 1);
    EXPECT_RED("wall in the per-event CPU* rows goes red",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: total_ns corrected, occurrence left on wall\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, measured, 1, 0, 0, bi, NULL);
    pgwt_replay_finish(acc);
    stamp_cpu_rows(acc, measured->pid, 4 * MS, 10 * MS, 1);
    EXPECT_RED("min/max left on wall goes red",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: the row exists but is invisible (count == 0)\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, measured, 1, 0, 0, bi, NULL);
    pgwt_replay_finish(acc);
    stamp_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS, 0);
    CHECK(find_query_row(acc, FIX_QID, 0) != NULL,
          "invisible query row physically exists");
    CHECK(find_query_ev(acc, FIX_QID, 0) == NULL,
          "query finder itself rejects count-zero rows");
    EXPECT_RED("a CPU* row the finder cannot resolve refuses",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: UNKNOWN is not accepted in place of measured\n");
    struct pgwt_trace_event unknown = *measured;
    unknown.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, &unknown, 1, 0, 0, bi, NULL);
    pgwt_replay_finish(acc);
    EXPECT_RED("an UNKNOWN cpu column cannot satisfy the measured assertion",
               check_cpu_rows(acc, unknown.pid, 4 * MS, 4 * MS));
    /* ...and the legacy whole-gap row is still asserted, positively. */
    check_cpu_rows(acc, unknown.pid, 10 * MS, 10 * MS);

    free(acc);
}

static void test_cpu_replay(bool measured)
{
    char dir[] = "/tmp/pgwt_replay_cpu_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }
    const uint64_t BASE = 10000000000000ULL;
    const uint64_t MS = 1000000ULL;
    const uint64_t PERIOD = 100 * MS;
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0) {
        fprintf(stderr, "CPU writer init failed\n");
        exit(1);
    }
    w.cpu_measured = measured;
    /* FIX_QID so the query_event table's CPU* row is exercised too: it is a
     * third per-event view of the same record and renders the same label. */
    struct pgwt_trace_event trans[] = {
        ev(2000, BASE + 10 * MS, 10 * MS, 0, IO_READ, FIX_QID),
        ev(2001, BASE + 30 * MS, 10 * MS, 0, IO_READ, FIX_QID),
        ev(2002, BASE + 50 * MS, 10 * MS, 0, IO_READ, FIX_QID),
        ev(2003, BASE + 70 * MS, 10 * MS, IO_READ, 0, FIX_QID),
    };
    trans[0].cpu_ns = 4 * MS;
    trans[1].cpu_ns = 0;
    trans[2].cpu_ns = 12 * MS;
    trans[3].cpu_ns = 4 * MS;
    int count = measured ? 4 : 1;
    for (int i = 0; i < count; i++)
        CHECK(pgwt_writer_push_event(&w, &trans[i]) == 0,
              "CPU fixture interval written");
    if (measured) {
        struct pgwt_trace_event sample = ev(2004, BASE + 90 * MS, 0,
                                            0, 0, FIX_QID);
        CHECK(pgwt_writer_push_samples(&w, &sample, 1, PERIOD) == 0,
              "CPU fixture sample written");
    }
    CHECK(pgwt_writer_close(&w) == 0, "CPU fixture writer closed");
    pgwt_writer_destroy(&w);

    char path[600];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0) {
        fprintf(stderr, "CPU fixture reader open failed\n");
        exit(1);
    }
    /* cpu_measured=false still writes v3, with UNKNOWN in its CPU column;
     * v2 decoding supplies the same sentinel without a column. */
    CHECK_EQ_U64(r.header.version, 3, "real writer emits v3 CPU column");
    CHECK_EQ_U64(r.num_blocks, measured ? 2 : 1,
                 "CPU fixture has expected interval/sample blocks");
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    struct pgwt_accumulator *mixed = malloc(sizeof(*mixed));
    if (!buf || !mixed) { perror("malloc"); exit(1); }
    pgwt_accum_init(mixed);
    int intervals_seen = 0, samples_seen = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf,
                                              PGWT_BLOCK_EVENTS, &bi);
        CHECK(n > 0, "CPU fixture block decoded");
        if (n <= 0) continue;
        pgwt_replay_events(mixed, buf, n, 0, 0, &bi, NULL);
        if (bi.block_type == PGWT_BLOCK_TRANSITIONS) {
            intervals_seen += n;
            CHECK_EQ_U64(n, count, "all CPU fixture intervals decoded");
            if (n != count) continue;
            if (!measured) {
                printf("CASE #495: UNKNOWN keeps legacy whole-gap CPU\n");
                CHECK_EQ_U64(buf[0].cpu_ns, PGWT_CPU_NS_UNKNOWN,
                             "unmeasured writer stamps UNKNOWN, not zero");
                check_cpu_replay(&buf[0], &bi, 0, 0, 10 * MS, 0, 10 * MS,
                                 10 * MS, 10 * MS);
                continue;
            }
            CHECK_EQ_U64(buf[0].cpu_ns, 4 * MS, "measured CPU column decoded");
            printf("CASE #495: measured 4 ms CPU in 10 ms wall\n");
            check_cpu_replay(&buf[0], &bi, 0, 0, 4 * MS, 6 * MS, 10 * MS,
                             4 * MS, 4 * MS);
            printf("CASE #495: same interval, half-window overlap\n");
            /* total and occurrence are SEPARATE axes: 2 ms of clipped
             * measured CPU, whose occurrence still measured 4 ms. */
            check_cpu_replay(&buf[0], &bi, BASE + 5 * MS, BASE + 10 * MS,
                             2 * MS, 3 * MS, 5 * MS, 2 * MS, 4 * MS);
            printf("CASE #495: measured zero is not UNKNOWN\n");
            CHECK_EQ_U64(buf[1].cpu_ns, 0, "measured zero survives writer");
            check_cpu_replay(&buf[1], &bi, 0, 0, 0, 10 * MS, 10 * MS, 0, 0);
            printf("CASE #495: CPU skew clamps to wall\n");
            CHECK_EQ_U64(buf[2].cpu_ns, 12 * MS, "skew survives writer");
            check_cpu_replay(&buf[2], &bi, 0, 0, 10 * MS, 0, 10 * MS,
                             10 * MS, 10 * MS);
            printf("CASE #495: measured wait spin keeps its IO label\n");
            CHECK_EQ_U64(buf[3].cpu_ns, 4 * MS, "wait spin survives writer");
            check_cpu_replay(&buf[3], &bi, 0, 0, 0, 0, 10 * MS,
                             ROW_ABSENT, ROW_ABSENT);
            test_cpu_row_bypass(&buf[0], &buf[3], &bi);
        } else if (bi.block_type == PGWT_BLOCK_SAMPLES) {
            samples_seen += n;
            CHECK_EQ_U64(n, 1, "one CPU sample decoded");
            if (n != 1) continue;
            printf("CASE #495: CPU sample keeps one period of estimated CPU\n");
            /* The original no-phantom-CPU assertion covers Lock samples.
             * A real new_event==0 sample DOES reach the CPU bucket. */
            CHECK_EQ_U64(buf[0].new_event, 0, "sample observes CPU");
            CHECK_EQ_U64(buf[0].cpu_ns, PGWT_CPU_NS_UNKNOWN,
                         "sample has no measured CPU");
            check_cpu_replay(&buf[0], &bi, 0, 0, PERIOD, 0, PERIOD,
                             PERIOD, OCC_UNSET);
        }
    }
    pgwt_replay_finish(mixed);
    CHECK_EQ_U64(intervals_seen, count, "interval checks were reached");
    CHECK_EQ_U64(samples_seen, measured ? 1 : 0, "sample checks were reached");
    if (measured) {
        uint64_t pid_cpu = 0;
        for (int i = 0; i < mixed->num_pids; i++)
            pid_cpu += mixed->pids[i].cpu_time_ns;
        CHECK_EQ_U64(mixed->tm.cpu_time_ns, 14 * MS + PERIOD,
                     "mixed CPU = 4 + 0 + clamped 10 ms + one period");
        CHECK_EQ_U64(mixed->tm.offcpu_time_ns, 16 * MS,
                     "mixed off-CPU = 6 + 10 ms");
        CHECK_EQ_U64(pid_cpu, 14 * MS + PERIOD,
                     "mixed per-PID CPU sum is measured, not wall");
        CHECK_EQ_U64(pid_cpu, mixed->tm.cpu_time_ns,
                     "mixed per-PID CPU sum agrees with time model");
        /* #531: the same absolute figure in the per-event table, over the
         * whole fixture. 14 ms + one period, not the 30 ms + period of wall. */
        const struct pgwt_event_stats *mix_cpu = find_sys(mixed, 0);
        CHECK(mix_cpu != NULL, "mixed system_event CPU* row present");
        if (mix_cpu)
            CHECK_EQ_U64(mix_cpu->total_ns, 14 * MS + PERIOD,
                         "mixed system_event CPU* total is measured, not wall");
        const struct pgwt_event_stats *mix_io = find_sys(mixed, IO_READ);
        CHECK(mix_io != NULL, "mixed system_event IO row present");
        if (mix_io)
            CHECK_EQ_U64(mix_io->total_ns, 10 * MS,
                         "mixed system_event IO row still carries wall");
    }
    pgwt_reader_close(&r);
    free(buf);
    free(mixed);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
}

static uint64_t query_total(const struct pgwt_accumulator *acc)
{
    uint64_t ns = 0;
    for (int i = 0; i < acc->num_query_events; i++)
        ns += acc->query_events[i].total_ns;  /* includes UNATTRIBUTED */
    return ns;
}

/* #612: exercise the actual zero-id branch, through the trace codec, and
 * compare query accounting with the live interval/marker contract. */
static void test_parse_phase_replay(void)
{
    const uint64_t MS = 1000000ULL, BASE = 10000000000000ULL;
    char dir[] = "/tmp/pgwt_replay_parse_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); failures++; return; }
    struct pgwt_trace_event trace[] = {
        /* CMD_START's stale id must never resolve this command's wait. */
        ev(123, BASE, 0, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 999),
        ev(123, BASE + 10 * MS, 10 * MS, LOCK_REL, 0, 0),
        ev(123, BASE + 12 * MS, 0, PGWT_MARKER_EXEC_START,
           PGWT_MARKER_EXEC_START, 700),
        ev(123, BASE + 16 * MS, 4 * MS, IO_READ, 0, 700),
        /* A trailing zero-id record falls back to this command's id. */
        ev(123, BASE + 19 * MS, 3 * MS, LOCK_REL, 0, 0),
        ev(123, BASE + 20 * MS, 0, PGWT_MARKER_CMD_END,
           PGWT_MARKER_CMD_END, 0),
        ev(123, BASE + 21 * MS, 0, PGWT_MARKER_CMD_START,
           PGWT_MARKER_CMD_START, 700),
        /* No resolving marker or exit: replay completion must flush it. */
        ev(123, BASE + 26 * MS, 5 * MS, LOCK_REL, 0, 0),
    };
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0) {
        failures++; return;
    }
    for (size_t i = 0; i < sizeof(trace) / sizeof(trace[0]); i++) {
        CHECK_EQ_U64(pgwt_writer_push_event(&w, &trace[i]), 0,
                     "parse fixture record written");
        if (i == 1 || i == 2)
            CHECK_EQ_U64(pgwt_writer_publish(&w), 0,
                         "parse wait and resolving marker in separate blocks");
    }
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    char path[600];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0) { failures++; return; }
    CHECK(r.num_blocks >= 2, "parse fixture crosses a block boundary");
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    struct pgwt_accumulator *live = malloc(sizeof(*live));
    struct pgwt_accumulator *replay = malloc(sizeof(*replay));
    struct pgwt_accumulator *bounded = malloc(sizeof(*bounded));
    struct pgwt_accumulator *clipped = malloc(sizeof(*clipped));
    if (!buf || !live || !replay || !bounded || !clipped) {
        perror("malloc");
        free(buf); free(live); free(replay); free(bounded); free(clipped);
        pgwt_reader_close(&r);
        failures++;
        return;
    }
    pgwt_accum_init(live);
    pgwt_accum_init(replay);
    pgwt_accum_init(bounded);
    pgwt_accum_init(clipped);
    int zero_waits = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf, PGWT_BLOCK_EVENTS, &bi);
        CHECK(n > 0, "parse fixture block decoded");
        if (n <= 0) continue;
        pgwt_replay_events(replay, buf, n, 0, 0, &bi, NULL);
        pgwt_replay_events(clipped, buf, n, BASE + 3 * MS,
                           BASE + 7 * MS, &bi, NULL);
        for (int i = 0; i < n; i++) {
            const struct pgwt_trace_event *e = &buf[i];
            struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(live, e->pid);
            if (PGWT_IS_MARKER(e->old_event)) {
                pgwt_live_qattr_marker(live, pa, e->old_event, e->query_id);
            } else {
                if (e->old_event == LOCK_REL && e->query_id == 0)
                    zero_waits++;
                struct pgwt_live_interval iv = {
                    .pid = e->pid, .we = e->old_event,
                    .wall_ns = e->duration_ns, .cpu_ns = e->duration_ns,
                    .query_id = e->query_id, .closed = true, .pa = pa,
                };
                pgwt_accum_add_interval(live, &iv);
            }
        }
    }
    /* #586: marker is in an older, non-overlapping block. The only
     * admitted wait has qid 0; no in-window id can mask a missing seed. */
    CHECK(pgwt_replay_file(&r, bounded, BASE + 17 * MS, BASE + 19 * MS,
          buf, PGWT_BLOCK_EVENTS, NULL, NULL, NULL) == 0, "seeded replay scans older blocks");
    CHECK_EQ_U64(zero_waits, 3, "codec preserves actual zero-id parse waits");
    pgwt_live_qattr_pid_exit(live, 123);
    struct pgwt_pid_accum *pending = pgwt_find_pid_accum(replay, 123);
    CHECK(pending && pending->qattr.npending == 1,
          "only the unresolved end-of-stream wait remains pending");
    if (pending) CHECK_EQ_U64(pending->qattr.pending[0].total_ns, 5 * MS,
                             "earlier waits were resolved during the streaming pass");
    CHECK(pgwt_replay_check_finished(replay) == -1,
          "forgotten finalization loudly refuses query output");
    pgwt_replay_finish(replay);
    CHECK(pgwt_replay_check_finished(replay) == 0, "finalized replay passes completeness check");
    pgwt_replay_finish(bounded);
    pgwt_replay_finish(clipped);
    const struct pgwt_query_event_stats *lq = find_query_ev(live, 700, LOCK_REL);
    const struct pgwt_query_event_stats *rq = find_query_ev(replay, 700, LOCK_REL);
    const struct pgwt_query_event_stats *uq = find_query_ev(replay, 0, LOCK_REL);
    const struct pgwt_query_event_stats *bq = find_query_ev(bounded, 700, LOCK_REL);
    CHECK(lq && rq, "live and replay both resolve zero-id waits to query 700");
    if (lq && rq) {
        CHECK_EQ_U64(lq->total_ns, 13 * MS, "live resolves parse and trailing waits");
        CHECK_EQ_U64(rq->total_ns, lq->total_ns, "replay resolves to the live query");
        CHECK_EQ_U64(rq->count, 2, "deferred wait occurrences preserved");
        CHECK_EQ_U64(rq->min_ns, 3 * MS, "deferred wait minimum preserved");
        CHECK_EQ_U64(rq->max_ns, 10 * MS, "deferred wait maximum preserved");
    }
    CHECK(uq != NULL, "unresolved end-of-stream wait is visible");
    if (uq) CHECK_EQ_U64(uq->total_ns, 5 * MS, "unresolved time is UNATTRIBUTED");
    /* Compare the same raw window with the REAL server seeded tag pass,
     * #586. The pre-window EXEC_START supplies rule 2, with no in-window
     * resolving id after the zero-id wait to make the seed test vacuous. */
    struct pgwt_trace_event server[] = { trace[4] };
    struct pgwt_trace_event unseeded = trace[4];
    CHECK(pgwt_tag_events_window(&unseeded, 1, NULL, 0, BASE + 19 * MS),
          "unseeded control tag pass succeeds");
    CHECK_EQ_U64(unseeded.query_id, 0, "left-edge comparison requires the seed to pass");
    struct pgwt_qid_seed seed = { .pid = 123, .query_id = 700 };
    CHECK(pgwt_tag_events_window_seeded(server, 1, NULL, 0,
          BASE + 19 * MS, &seed, 1), "server seeded tag pass succeeds");
    CHECK_EQ_U64(server[0].query_id, 700, "server attributes left-edge wait to 700");
    CHECK(bq != NULL, "replay seeds left-edge attribution from pre-window markers (#586)");
    if (bq) {
        CHECK_EQ_U64(bq->query_id, server[0].query_id, "replay and server agree on query");
        CHECK_EQ_U64(bq->total_ns, server[0].timestamp_ns - (BASE + 17 * MS), "replay and server agree on wait time");
        CHECK_EQ_U64(bq->total_ns, 2 * MS, "left-edge wait has the expected duration");
    }
    CHECK(find_query_ev(bounded, 0, LOCK_REL) == NULL,
          "seeded wait does not leak into UNATTRIBUTED");
    CHECK(find_query_ev(replay, 999, LOCK_REL) == NULL,
          "CMD_START's stale id never receives parse time");
    CHECK_EQ_U64(query_total(live), 22 * MS, "live fixture accounts for every wait");
    CHECK_EQ_U64(query_total(replay), query_total(live),
                 "conservation: replay query plus unattributed time equals live total");
    CHECK_EQ_U64(query_total(bounded), 2 * MS,
                 "bounded conservation: all selected parse time remains accounted");
    const struct pgwt_query_event_stats *cq =
        find_query_row(clipped, 0, LOCK_REL);
    CHECK(cq != NULL, "clipped parse wait has an unattributed row");
    if (cq) {
        CHECK_EQ_U64(cq->total_ns, 4 * MS, "deferred time respects both window edges");
        CHECK_EQ_U64(cq->count, 0, "close outside window adds no occurrence");
        CHECK_EQ_U64(cq->min_ns, UINT64_MAX, "close outside window adds no minimum");
        CHECK_EQ_U64(cq->max_ns, 0, "close outside window adds no maximum");
    }
    CHECK_EQ_U64(query_total(clipped), 4 * MS,
                 "clipped conservation includes time without an occurrence");
    pgwt_replay_finish(replay);
    CHECK_EQ_U64(query_total(replay), query_total(live), "finish is idempotent");
    pgwt_reader_close(&r);
    free(buf);
    free(live);
    free(replay);
    free(bounded);
    free(clipped);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
}

/* Commit order: A's CMD_START, B's command, then A's lagging wait/end.
 * Sorting only each block (or each replay call) assigns A's wait to 900. */
static void test_replay_commit_order(void)
{
    const uint64_t MS = 1000000ULL, BASE = 10000000000000ULL;
    char dir[] = "/tmp/pgwt_replay_order_XXXXXX";
    if (!mkdtemp(dir)) { failures++; return; }
    struct pgwt_trace_event command_b[] = {
        ev(123, BASE + 200 * MS, 0, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 0),
        ev(123, BASE + 200 * MS, 0, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 900),
    };
    struct pgwt_trace_event command_a[] = {
        ev(123, BASE + 90 * MS, 0, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 0),
        ev(123, BASE + 100 * MS, 10 * MS, LOCK_REL, 0, 0),
        ev(123, BASE + 110 * MS, 0, PGWT_MARKER_CMD_END, PGWT_MARKER_CMD_END, 0),
    };
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0) { failures++; return; }
    CHECK(pgwt_writer_push_event(&w, &command_a[0]) == 0, "A's start written first");
    CHECK(pgwt_writer_publish(&w) == 0, "commit A's start before the overlapping block");
    for (int i = 0; i < 2; i++)
        CHECK(pgwt_writer_push_event(&w, &command_b[i]) == 0, "early-committed B written");
    CHECK(pgwt_writer_publish(&w) == 0, "commit B before lagging A");
    /* Samples interleave both commands and carry different block weights.
     * B's zero-id sample also makes its equal-timestamp marker order matter. */
    struct pgwt_trace_event sample_a = ev(123, BASE + 105 * MS, 0, 0, IO_READ, 0);
    struct pgwt_trace_event sample_b = ev(123, BASE + 205 * MS, 0, 0, IO_READ, 0);
    CHECK(pgwt_writer_push_samples(&w, &sample_a, 1, 2 * MS) == 0,
          "A's sample block has a 2 ms period");
    CHECK(pgwt_writer_push_samples(&w, &sample_b, 1, 3 * MS) == 0,
          "B's sample block has a 3 ms period");
    for (int i = 1; i < 3; i++)
        CHECK(pgwt_writer_push_event(&w, &command_a[i]) == 0, "late-committed A written");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    char path[600];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0) { failures++; return; }
    CHECK(!r.block_index_sorted, "fixture really has reversed commit order");
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    struct pgwt_accumulator *a = calloc(1, sizeof(*a));
    if (!buf || !a) { failures++; free(buf); free(a); pgwt_reader_close(&r); return; }
    CHECK(pgwt_replay_file(&r, a, 0, 0, buf, PGWT_BLOCK_EVENTS, NULL, NULL, NULL) == 0,
          "replay unordered blocks through the actual file scanner");
    pgwt_replay_finish(a);
    const struct pgwt_query_event_stats *uq = find_query_ev(a, 0, LOCK_REL);
    CHECK(uq && uq->total_ns == 10 * MS, "A's 10 ms stays UNATTRIBUTED");
    CHECK(find_query_ev(a, 900, LOCK_REL) == NULL, "B never inherits A's wait");
    const struct pgwt_query_event_stats *sa = find_query_ev(a, 0, IO_READ);
    const struct pgwt_query_event_stats *sb = find_query_ev(a, 900, IO_READ);
    CHECK(sa && sa->count == 1 && sa->total_ns == 2 * MS &&
          sa->min_ns == UINT64_MAX && sa->max_ns == 0,
          "file merge keeps A's sample period and no occurrence latencies");
    CHECK(sb && sb->count == 1 && sb->total_ns == 3 * MS &&
          sb->min_ns == UINT64_MAX && sb->max_ns == 0,
          "file merge keeps B's sample period and stable tied marker order");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 3 * MS, "only B's own sample backfills to B");
    CHECK_EQ_U64(query_total(a), 15 * MS, "unordered mixed replay conserves every contribution");
    pgwt_reader_close(&r);
    free(buf);
    free(a);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
}

static void test_replay_unattributed_categories(void)
{
    const struct pgwt_block_info bi = { .block_type = PGWT_BLOCK_TRANSITIONS };
    struct pgwt_accumulator *a = calloc(1, sizeof(*a));
    if (!a) { failures++; return; }
    /* Activity-class events are idle without any version/name registry. */
    const uint32_t idle = WEI(PG_WAIT_ACTIVITY, 0);
    struct pgwt_trace_event arr[] = {
        ev(101, 1000, 11, LOCK_REL, 0, 0),
        ev(102, 2000, 23, IO_READ, 0, 0),
        ev(102, 3000, 41, IO_READ, 0, 4242),
        ev(103, 4000, 37, LOCK_REL, 0, 0),
        ev(104, 5000, 53, LOCK_REL, 0, 0),
        ev(104, 6000, 67, LOCK_REL, 0, 4242),
        ev(105, 7000, 71, IO_READ, 0, 0),
        ev(106, 8000, 83, idle, 0, 0),
    };
    arr[1].flags = arr[2].flags = PGWT_EVENT_FLAG_BACKGROUND;
    arr[3].flags = PGWT_EVENT_FLAG_MAINT;
    /* Deliberately under-count an unknown category: its 53 ns must never
     * borrow the later statement's id, even on this same foreground-like pid. */
    arr[4].flags = FUTURE_CAT_FLAG;
    arr[6].flags = PGWT_EVENT_FLAG_IO_WORKER;
    arr[7].flags = PGWT_EVENT_FLAG_BACKGROUND;

    printf("CASE #665: replay zero-id categories and total refusal\n");
    pgwt_replay_events(a, arr, 1, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    const struct pgwt_query_event_stats *q = find_query_ev(a, 0, LOCK_REL);
    CHECK(q && q->count == 1 && q->total_ns == 11 &&
          q->min_ns == 11 && q->max_ns == 11,
          "foreground without a later id retains its exact 11 ns in qid 0 (#128)");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 11, "foreground contributes 11 unattributed ns");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "foreground without an id never backfills");
    EXPECT_RED("missing foreground qid-0 row goes red", {
        a->num_query_events = 0;
        CHECK(find_query_ev(a, 0, LOCK_REL) != NULL, "foreground qid-0 row present");
        a->num_query_events = 1;
    });

    pgwt_accum_init(a);
    pgwt_replay_events(a, &arr[1], 2, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    q = find_query_ev(a, 0, IO_READ);
    CHECK(q && q->count == 1 && q->total_ns == 23 &&
          q->min_ns == 23 && q->max_ns == 23,
          "background retains its exact 23 ns in qid 0 despite a later real id");
    q = find_query_ev(a, 4242, IO_READ);
    CHECK(q && q->count == 1 && q->total_ns == 41 &&
          q->min_ns == 41 && q->max_ns == 41,
          "same background pid's query 4242 contains only its own 41 ns");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 23, "background contributes 23 unattributed ns");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "background zero-id work never backfills");
    EXPECT_RED("lost background time goes red", {
        a->query_events[0].total_ns = 0;
        q = find_query_ev(a, 0, IO_READ);
        CHECK(q && q->total_ns == 23, "background qid 0 retains 23 ns");
        a->query_events[0].total_ns = 23;
    });

    pgwt_accum_init(a);
    pgwt_replay_events(a, &arr[3], 1, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    q = find_query_ev(a, 0, LOCK_REL);
    CHECK(q && q->count == 1 && q->total_ns == 37 &&
          q->min_ns == 37 && q->max_ns == 37,
          "maintenance retains its exact 37 ns and one occurrence in qid 0");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 37, "maintenance contributes 37 unattributed ns");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "maintenance zero-id work never backfills");

    pgwt_accum_init(a);
    pgwt_replay_events(a, &arr[4], 2, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK(find_query_row(a, 0, LOCK_REL) == NULL, "unknown category creates no qid-0 row");
    q = find_query_ev(a, 4242, LOCK_REL);
    CHECK(q && q->count == 1 && q->total_ns == 67 &&
          q->min_ns == 67 && q->max_ns == 67,
          "unknown category never adds its 53 ns to the later query's 67 ns");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 0, "unknown category moves no unattributed counter");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "unknown category moves no backfilled counter");
    EXPECT_RED("borrowing an unknown category's time goes red", {
        a->query_events[0].total_ns = 53 + 67;
        q = find_query_ev(a, 4242, LOCK_REL);
        CHECK(q && q->total_ns == 67, "query 4242 contains only its own 67 ns");
        a->query_events[0].total_ns = 67;
    });

    pgwt_accum_init(a);
    pgwt_replay_events(a, &arr[6], 1, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK(find_query_row(a, 0, IO_READ) == NULL, "IO worker creates no qid-0 IO row");
    CHECK_EQ_U64(a->num_query_events, 0, "IO worker creates no query rows at all");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 0, "IO worker moves no unattributed counter");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "IO worker moves no backfilled counter");

    pgwt_accum_init(a);
    CHECK(pgwt_is_idle_event(idle), "Activity fixture is idle under the load rule");
    pgwt_replay_events(a, &arr[7], 1, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK(find_query_row(a, 0, idle) == NULL, "idle background creates no qid-0 row");
    CHECK_EQ_U64(a->num_query_events, 0, "idle background creates no query rows");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 0, "idle background moves no unattributed counter");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "idle background moves no backfilled counter");

    pgwt_accum_init(a);
    pgwt_replay_events(a, arr, sizeof(arr) / sizeof(arr[0]), 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    /* Independent literal budget: unknown 53, IO-worker 71 and idle 83 ns
     * are intentionally excluded from query accounting. */
    CHECK_EQ_U64(query_total(a), 11 + 23 + 41 + 37 + 67,
                 "query totals conserve the independent per-record ns budget");
    CHECK_EQ_U64(a->qattr_unattributed_ns, 11 + 23 + 37,
                 "only foreground, background and maintenance zero-id ns are unattributed");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "combined fixture never borrows a query id");
    free(a);
}

static void test_replay_resolver_edges(void)
{
    const uint64_t MS = 1000000ULL, BASE = 10000000000000ULL;
    const struct pgwt_block_info bi = { .block_type = PGWT_BLOCK_TRANSITIONS };
    struct pgwt_accumulator *a = calloc(1, sizeof(*a));
    if (!a) { failures++; return; }
    /* Same timestamp: captured CMD_START precedes EXEC_START. An unstable
     * sort can leave a false reset between the id and the zero-id wait. */
    struct pgwt_trace_event ties[] = {
        ev(123, BASE, 0, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 999),
        ev(123, BASE, 0, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 700),
        ev(123, BASE + 10 * MS, 10 * MS, LOCK_REL, 0, 0),
    };
    /* Direct calls consume the ordered stream supplied by the file sort. */
    pgwt_replay_events(a, ties, 3, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    const struct pgwt_query_event_stats *q = find_query_ev(a, 700, LOCK_REL);
    CHECK(q && q->total_ns == 10 * MS, "ordered stream preserves tied marker order");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 10 * MS, "stable marker order supplies rule 2");

    pgwt_accum_init(a);
    struct pgwt_trace_event io[] = {
        ev(123, BASE + MS, MS, LOCK_REL, 0, 900),
        ev(123, BASE + 2 * MS, MS, LOCK_REL, 0, 0),
    };
    io[0].flags = PGWT_EVENT_FLAG_IO_WORKER;
    pgwt_replay_events(a, io, 2, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK(find_query_ev(a, 900, LOCK_REL) == NULL, "io-worker nonzero id is structurally query-less");
    q = find_query_ev(a, 0, LOCK_REL);
    CHECK(q && q->total_ns == MS, "io-worker id never seeds a foreground wait");
    CHECK_EQ_U64(a->qattr_backfilled_ns, 0, "io-worker id cannot trigger a backfill");

    pgwt_accum_init(a);
    for (uint32_t pid = 1; pid <= MAX_BACKENDS; pid++)
        pgwt_get_or_create_pid(a, pid);
    CHECK_EQ_U64(a->num_pids, MAX_BACKENDS, "fixture fills the pid table");
    struct pgwt_trace_event dropped = ev(MAX_BACKENDS + 1, BASE + MS, MS, LOCK_REL, 0, 0);
    pgwt_replay_events(a, &dropped, 1, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK_EQ_U64(query_total(a), 0, "full pid table matches live's drop of zero-id time");
    CHECK_EQ_U64(a->tm.db_time_ns, MS, "full pid table still retains system time");

    pgwt_accum_init(a);
    struct pgwt_trace_event overflow[PGWT_QATTR_PENDING_MAX + 2];
    for (int i = 0; i < PGWT_QATTR_PENDING_MAX + 1; i++)
        overflow[i] = ev(123, BASE + (i + 1) * MS, MS, LOCK_REL + i, 0, 0);
    overflow[PGWT_QATTR_PENDING_MAX + 1] = ev(123, BASE + 100 * MS, 0,
        PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 700);
    pgwt_replay_events(a, overflow, PGWT_QATTR_PENDING_MAX + 2, 0, 0, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK_EQ_U64(a->qattr_pending_overflow, 1, "resolver API owns the pending overflow policy");
    CHECK_EQ_U64(a->qattr_unattributed_ns, PGWT_QATTR_PENDING_MAX * MS,
                 "overflow spills all full-list time exactly once");
    q = find_query_ev(a, 700, LOCK_REL + PGWT_QATTR_PENDING_MAX);
    CHECK(q && q->count == 1 && q->min_ns == MS && q->max_ns == MS && q->total_ns == MS,
          "first post-overflow slot has only its own occurrence stats");
    CHECK_EQ_U64(query_total(a), (PGWT_QATTR_PENDING_MAX + 1) * MS,
                 "overflow conserves every deferred interval");
    pgwt_accum_init(a);
    pgwt_replay_events(a, ties, 2, BASE + MS, BASE + 2 * MS, &bi, NULL);
    pgwt_replay_finish(a);
    CHECK_EQ_U64(a->num_pids, 1, "seed markers use the bounded per-pid resolver");
    CHECK_EQ_U64(a->pids[0].num_events, 0, "seed-only markers create no event statistics");
    CHECK_EQ_U64(a->num_query_events, 0, "seed-only markers never fabricate query rows");

    pgwt_accum_init(a);
    struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(a, 123);
    pgwt_live_qattr_record(a, pa, 0, LOCK_REL, MS, 0, true);
    CHECK(pgwt_replay_check_finished(a) == -1,
          "residual resolver aggregates also loudly refuse unfinished output");
    pgwt_replay_finish(a);
    CHECK(pgwt_replay_check_finished(a) == 0, "finish clears residual resolver aggregates");

    free(a);
}

/* Cross-file timestamps reverse across a reboot. Read REAL patched file
 * headers so the test covers per-file clocks in wall-ordered replay. */
static void test_replay_wall_clocks(void)
{
    const uint64_t S = 1000000000ULL, WALL = 1700000000ULL * S;
    struct pgwt_accumulator *a = calloc(1, sizeof(*a));
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    if (!a || !buf) { failures++; free(a); free(buf); return; }
    /* Reboot reversal, then a clock-anchor step with overlapping mono
     * ranges. The latter would interleave two commands under raw sorting. */
    for (int scenario = 0; scenario < 2; scenario++) {
        pgwt_accum_init(a);
        for (int f = 0; f < 2; f++) {
            char dir[] = "/tmp/pgwt_replay_clock_XXXXXX";
            if (!mkdtemp(dir)) { failures++; break; }
            uint64_t mono = (f ? (scenario ? 3601 : 5) : 3600) * S;
            struct pgwt_trace_event records[] = {
                ev(123, mono, 0, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 0),
                ev(123, mono + S, 0, PGWT_MARKER_EXEC_START,
                   PGWT_MARKER_EXEC_START, f ? 900 : 700),
                ev(123, mono + 2 * S, S, LOCK_REL, 0, 0),
            };
            struct pgwt_event_writer w;
            CHECK(pgwt_writer_init(&w, dir, 18, 24, NULL) == 0, "clock fixture writer opened");
            for (int i = 0; i < 3; i++)
                CHECK(pgwt_writer_push_event(&w, &records[i]) == 0, "clock fixture record written");
            pgwt_writer_close(&w);
            pgwt_writer_destroy(&w);
            char path[600];
            snprintf(path, sizeof(path), "%s/current.trace", dir);
            FILE *fp = fopen(path, "r+b");
            struct pgwt_trace_file_header h;
            CHECK(fp && fread(&h, sizeof(h), 1, fp) == 1, "clock fixture header read");
            if (fp) {
                h.start_time_ns = WALL + f * 10 * S;
                h.clock_offset_ns = mono;
                rewind(fp);
                CHECK(fwrite(&h, sizeof(h), 1, fp) == 1, "per-file clock anchor written");
                fclose(fp);
            }
            struct pgwt_event_reader r;
            CHECK(pgwt_reader_open(&r, path) == 0, "clock fixture reader opened");
            CHECK(pgwt_replay_file(&r, a, 0, 0, buf, PGWT_BLOCK_EVENTS, NULL, NULL, NULL) == 0,
                  "different clock domains admitted through file scanner");
            pgwt_reader_close(&r);
            char cmd[700];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
            if (system(cmd) != 0) { /* best effort */ }
        }
        pgwt_replay_finish(a);
        CHECK(pgwt_replay_check_finished(a) == 0, "cross-clock replay is complete");
        const struct pgwt_query_event_stats *q = find_query_ev(a, 700, LOCK_REL);
        CHECK(q && q->total_ns == S, "pre-reboot wait retains pre-reboot query");
        q = find_query_ev(a, 900, LOCK_REL);
        CHECK(q && q->total_ns == S, "post-reboot recycled pid retains its own query");
        CHECK_EQ_U64(a->qattr_backfilled_ns, 2 * S, "cross-clock attribution counts only correct backfills");
    }
    free(a); free(buf);
}

/* The documented two-hour window at 100 statements/s: 2.88M markers.
 * It must succeed without any window-sized attribution queue or spool. */
static void test_replay_two_hours(void)
{
    const uint64_t S = 1000000000ULL, BASE = 10000000000000ULL;
    const int commands = 2 * 60 * 60 * 100;
    char dir[] = "/tmp/pgwt_replay_two_hours_XXXXXX";
    if (!mkdtemp(dir)) { failures++; return; }
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0) { failures++; return; }
    int written = 0;
    for (int i = 0; i < commands; i++) {
        uint64_t t = BASE + (uint64_t)i * (S / 100);
        struct pgwt_trace_event records[] = {
            ev(123, t, 0, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 999),
            ev(123, t + 1, 0, PGWT_MARKER_PLAN_START, PGWT_MARKER_PLAN_START, 700),
            ev(123, t + 2, 0, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 700),
            ev(123, t + 3, 1, LOCK_REL, 0, 0),
            ev(123, t + 4, 0, PGWT_MARKER_CMD_END, PGWT_MARKER_CMD_END, 0),
        };
        int rc = 0;
        for (int j = 0; j < 5; j++) rc |= pgwt_writer_push_event(&w, &records[j]);
        if (rc) { failures++; break; }
        written++;
    }
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    CHECK_EQ_U64(written, commands, "two-hour 100 TPS fixture written through real codec");
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    struct pgwt_accumulator *a = calloc(1, sizeof(*a));
    char path[600];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (!buf || !a || pgwt_reader_open(&r, path) != 0) {
        failures++; free(buf); free(a); return;
    }
    struct pgwt_replay_stats st = {0};
    uint64_t decoded = 0;
    CHECK(pgwt_replay_file(&r, a, BASE, BASE + 2 * 60 * 60 * S,
          buf, PGWT_BLOCK_EVENTS, NULL, &st, &decoded) == 0,
          "documented two-hour window succeeds beyond the former marker cap");
    pgwt_reader_close(&r);
    pgwt_replay_finish(a);
    CHECK(pgwt_replay_check_finished(a) == 0, "two-hour streaming replay completes");
    CHECK_EQ_U64(decoded, (uint64_t)commands * 5, "all two-hour records decoded");
    CHECK_EQ_U64(st.markers_skipped, (uint64_t)commands * 4, "all 2.88M markers processed");
    const struct pgwt_query_event_stats *q = find_query_ev(a, 700, LOCK_REL);
    CHECK(q && q->count == (uint64_t)commands && q->total_ns == (uint64_t)commands,
          "all 720000 zero-id waits backfill with exact time and occurrences");
    CHECK_EQ_U64(a->num_pids, 1, "entire window needs one resolver");
    CHECK_EQ_U64(a->pids[0].qattr.npending, 0, "no pending aggregates survive completion");
    free(a); free(buf);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
}

int main(void)
{
    char dir[] = "/tmp/pgwt_replay_fid_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    const uint64_t BASE = 10000000000000ULL;   /* 10000 s */
    const uint64_t MS = 1000000ULL;
    const uint64_t PERIOD = 100 * MS;          /* 100 ms */

    /* ── Write the trace ─────────────────────────────────── */
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0) {
        fprintf(stderr, "writer init failed\n");
        return 1;
    }

    struct pgwt_trace_event trans[] = {
        /* escalation START marker (pid 0, ESC_PACK payload) */
        ev(0, BASE, 0, PGWT_MARKER_ESCALATE_START, PGWT_MARKER_ESCALATE_START,
           PGWT_ESC_PACK(60, 1)),
        /* exec markers around two real IO waits of 10 ms */
        ev(1000, BASE + 1 * MS, 0, PGWT_MARKER_EXEC_START,
           PGWT_MARKER_EXEC_START, 100),
        ev(1000, BASE + 20 * MS, 10 * MS, IO_READ, 0, 100),
        ev(1000, BASE + 40 * MS, 10 * MS, IO_READ, 0, 100),
        ev(1000, BASE + 41 * MS, 0, PGWT_MARKER_EXEC_END,
           PGWT_MARKER_EXEC_END, 100),
        /* escalation END marker */
        ev(0, BASE + 50 * MS, 0, PGWT_MARKER_ESCALATE_END,
           PGWT_MARKER_ESCALATE_END, PGWT_ESC_PACK(60, 2)),
    };
    for (size_t i = 0; i < sizeof(trans) / sizeof(trans[0]); i++)
        pgwt_writer_push_event(&w, &trans[i]);

    /* 5 Lock samples for pid 1001 / query 200 at 100 ms period */
    struct pgwt_trace_event smp[5];
    for (int i = 0; i < 5; i++) {
        smp[i] = ev(1001, BASE + 100 * MS + (uint64_t)i * PERIOD, 0,
                    0, LOCK_REL, 200);
    }
    pgwt_writer_push_samples(&w, smp, 5, PERIOD);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    /* ── Read + replay ───────────────────────────────────── */
    char trace_path[600];
    snprintf(trace_path, sizeof(trace_path), "%s/current.trace", dir);

    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, trace_path) != 0) {
        fprintf(stderr, "reader open failed\n");
        return 1;
    }
    CHECK(r.num_blocks >= 2, "trace has transitions + samples blocks");

    struct pgwt_accumulator *acc = malloc(sizeof(*acc));
    pgwt_accum_init(acc);
    struct pgwt_replay_stats st = {0};
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));

    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf,
                                              PGWT_BLOCK_EVENTS, &bi);
        if (n <= 0) continue;
        pgwt_replay_events(acc, buf, n, 0, 0, &bi, &st);
    }
    pgwt_replay_finish(acc);
    pgwt_reader_close(&r);

    /* ── Assertions ──────────────────────────────────────── */

    /* Replay stats (drive the loud fidelity notice) */
    CHECK_EQ_U64(st.transitions, 2, "2 exact transitions accumulated");
    CHECK_EQ_U64(st.samples, 5, "5 samples accumulated");
    CHECK_EQ_U64(st.markers_skipped, 4, "4 markers skipped");
    CHECK_EQ_U64(st.sample_period_ns, PERIOD, "sample period reported");

    /* Exact IO wait: full stats including histogram */
    const struct pgwt_event_stats *io = find_sys(acc, IO_READ);
    CHECK(io != NULL, "IO:DataFileRead entry exists");
    if (io) {
        CHECK_EQ_U64(io->count, 2, "IO count = 2");
        CHECK_EQ_U64(io->total_ns, 20 * MS, "IO total = 20 ms (exact)");
        uint64_t hist_total = 0;
        for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
            hist_total += io->histogram[i];
        CHECK_EQ_U64(hist_total, 2, "IO histogram fed by exact records");
        CHECK_EQ_U64(io->max_ns, 10 * MS, "IO max = 10 ms");
    }

    /* Sampled Lock wait: ASH-estimated time, NO histogram/min/max */
    const struct pgwt_event_stats *lk = find_sys(acc, LOCK_REL);
    CHECK(lk != NULL, "Lock:relation entry exists (samples visible)");
    if (lk) {
        CHECK_EQ_U64(lk->count, 5, "Lock count = 5 samples");
        CHECK_EQ_U64(lk->total_ns, 5 * PERIOD,
                     "Lock total = 5 x period (ASH estimate, not 0)");
        uint64_t hist_total = 0;
        for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
            hist_total += lk->histogram[i];
        CHECK_EQ_U64(hist_total, 0,
                     "Lock histogram EMPTY (no fabricated latencies)");
        CHECK_EQ_U64(lk->max_ns, 0, "Lock max not fabricated");
    }

    /* Samples must NOT be misread as CPU (old_event = 0 on the wire) */
    const struct pgwt_event_stats *cpu = find_sys(acc, 0);
    CHECK(cpu == NULL, "no phantom CPU entries from sample records");

    /* Markers create no event entries and no phantom query ids */
    int marker_entries = 0;
    for (int i = 0; i < acc->num_system_events; i++)
        if (PGWT_IS_MARKER(acc->system_events[i].wait_event) &&
            acc->system_events[i].count > 0)
            marker_entries++;
    CHECK_EQ_U64(marker_entries, 0, "no marker entries in system events");

    int esc_qids = 0;
    uint64_t q100_ns = 0, q200_ns = 0;
    for (int i = 0; i < acc->num_query_events; i++) {
        const struct pgwt_query_event_stats *qe = &acc->query_events[i];
        if (qe->count == 0) continue;
        if (qe->query_id == PGWT_ESC_PACK(60, 1) ||
            qe->query_id == PGWT_ESC_PACK(60, 2))
            esc_qids++;
        if (qe->query_id == 100) q100_ns += qe->total_ns;
        if (qe->query_id == 200) q200_ns += qe->total_ns;
    }
    CHECK_EQ_U64(esc_qids, 0, "no PGWT_ESC_PACK phantom query ids");
    CHECK_EQ_U64(q100_ns, 20 * MS, "query 100 total = exact 20 ms");
    CHECK_EQ_U64(q200_ns, 5 * PERIOD, "query 200 total = 5 x period");

    /* Time model: lock class = sampled estimate, io class = exact */
    CHECK_EQ_U64(acc->tm.lock_time_ns, 5 * PERIOD,
                 "time model Lock = 500 ms (sampled, was 0 before fix)");
    CHECK_EQ_U64(acc->tm.io_time_ns, 20 * MS, "time model IO = 20 ms");
    CHECK_EQ_U64(acc->tm.db_time_ns, 20 * MS + 5 * PERIOD,
                 "time model DB time = exact + sampled");

    free(buf);
    free(acc);

    /* cleanup */
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }

    test_cpu_replay(true);
    test_cpu_replay(false);
    test_parse_phase_replay();
    test_replay_commit_order();
    test_replay_unattributed_categories();
    test_replay_resolver_edges();
    test_replay_wall_clocks();
    test_replay_two_hours();

    printf("%s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
