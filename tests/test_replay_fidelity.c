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
 * plus the daemon-flavor event_reader.c — no bpftool needed.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "map_reader.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define IO_READ  0x0A000015u   /* IO:DataFileRead */
#define LOCK_REL 0x03000000u   /* Lock:relation */

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
find_query_ev(struct pgwt_accumulator *acc, uint64_t qid, uint32_t we)
{
    for (int i = 0; i < acc->num_query_events; i++)
        if (acc->query_events[i].query_id == qid &&
            acc->query_events[i].wait_event == we &&
            acc->query_events[i].count > 0)
            return &acc->query_events[i];
    return NULL;
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
    EXPECT_RED("an absent CPU* row refuses, never approves",
               check_cpu_rows(acc, wait_labelled->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: nothing was replayed at all\n");
    pgwt_accum_init(acc);
    EXPECT_RED("an empty accumulator refuses",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: per-event rows carry WALL (the defect itself)\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, measured, 1, 0, 0, bi, NULL);
    stamp_cpu_rows(acc, measured->pid, 10 * MS, 10 * MS, 1);
    EXPECT_RED("wall in the per-event CPU* rows goes red",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: total_ns corrected, occurrence left on wall\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, measured, 1, 0, 0, bi, NULL);
    stamp_cpu_rows(acc, measured->pid, 4 * MS, 10 * MS, 1);
    EXPECT_RED("min/max left on wall goes red",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: the row exists but is invisible (count == 0)\n");
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, measured, 1, 0, 0, bi, NULL);
    stamp_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS, 0);
    EXPECT_RED("a CPU* row the finder cannot resolve refuses",
               check_cpu_rows(acc, measured->pid, 4 * MS, 4 * MS));

    printf("CASE #531 bypass: UNKNOWN is not accepted in place of measured\n");
    struct pgwt_trace_event unknown = *measured;
    unknown.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, &unknown, 1, 0, 0, bi, NULL);
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

    printf("%s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
