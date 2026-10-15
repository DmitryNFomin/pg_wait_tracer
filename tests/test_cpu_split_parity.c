/* test_cpu_split_parity.c — RAW-vs-SUMMARY parity for the CPU ROWS (#408)
 *
 * THE INVARIANT
 * ─────────────────────────────────────────────────────────────────────────
 * For the same capture and the same window, pgwt_compute_time_model() (raw)
 * and pgwt_compute_time_model_from_summaries() (aggregate) must report the
 * SAME CPU rows: "CPU (running)" and "CPU (waiting for a core)", both
 * present, both equal, and the same DB Time and Idle underneath them.
 *
 * Before summary v7 they could not, for two independent reasons, and this
 * file holds both down:
 *
 *   A. THE SPLIT WAS NOT IN THE FILE. The raw path splits CPU whenever any
 *      record carried a measured cpu_ns. The summary record stored neither
 *      the measured sum nor the count, so the summary path reported
 *      has_measured_cpu = 0 and emitted ONE combined "CPU*" row: the
 *      runqueue-wait row simply disappeared on every window of 120 s or more
 *      (every window the demo shows). Section 3.
 *   B. THE NON-COMMAND RELABEL WAS NOT APPLIED. A foreground we == 0
 *      interval outside a command is post/between-command idle time, not CPU
 *      (docs/AAS_SEMANTICS_DECISION.md). Both raw paths file it under
 *      PGWT_WEI_NONCMD_CPU; the writer saw the raw ringbuf id, so that time
 *      sat in the summary's CPU class and inside DB Time while raw counted it
 *      as Idle. On the PG18 demo capture that was the WHOLE DB Time
 *      divergence at the full span (+20,033.044 ms) and +3.54% on the CPU row
 *      (tests/results/issue408-measurement). Section 4.
 *
 * HOW THE PROPERTIES ARE PRODUCED — AND WHY NEITHER IS PRE-COOKED
 * ─────────────────────────────────────────────────────────────────────────
 * The same discipline as tests/test_io_worker_parity.c, for the same reason
 * (commit 8705412: a fixture that pre-set the property greened a writer
 * guard that was DEAD in the daemon):
 *   - the relabel is never written into the fixture. The stream carries
 *     old_event == 0 and real CMD_START/CMD_END markers, and BOTH sides
 *     resolve the relabel through the production classifier: the raw side via
 *     pgwt_tag_events() (what src/server.c runs over every raw window), the
 *     summary side via the eff_event the OFFLINE PRODUCER resolves with the
 *     same function before the push (what src/event_stream.c does on the live
 *     path with pgwt_live_effective_event). Section 4c is the reachability
 *     probe: a producer that resolves NOTHING (PGWT_SUMMARY_EFF_UNRESOLVED)
 *     must make this comparison go RED, or the parameter is dead code.
 *   - cpu_ns is a real per-record measurement with a real runqueue remainder
 *     (gap 50 ms, cpu 30 ms), so the two CPU rows are distinct numbers rather
 *     than a total that happens to match.
 *
 * THE THREE ADVERSARIAL QUESTIONS
 * ─────────────────────────────────────────────────────────────────────────
 *  - What input makes this pass while the product is broken? A fixture whose
 *    every record carries cpu_ns == PGWT_CPU_NS_UNKNOWN: both paths then
 *    report has_measured_cpu = 0 and ONE "CPU*" row, and the comparison is
 *    satisfied having never seen a split (section 5a drives exactly that and
 *    requires a REFUSAL). A fixture with no non-command gap satisfies the
 *    relabel assertions the same way (5b). And a window the summary reader
 *    finds no record in reads 0.000 == 0.000 (5c/5d).
 *  - What single component failing makes this skip rather than fail? The
 *    summary reader: a missing directory, a refused version or a
 *    non-overlapping window all return zero records, every summary total
 *    reads 0.000 ms and a naive comparison calls that agreement. Every
 *    comparison here runs the production preflight
 *    (pgwt_summaries_window_current, the same call should_use_summaries
 *    makes) and requires records, a measured record COUNT and a non-zero
 *    relabelled total before it compares anything. Nothing sleeps or retries.
 *  - What depends on timing or ordering? Two things, both pinned. The writer
 *    stamps its header from CLOCK_REALTIME and the reader treats a file as
 *    covering [header, +1 h], so the fixture is anchored to ONE
 *    CLOCK_MONOTONIC read and every window is derived from the COMMITTED
 *    records, never from a second clock read. And the writer folds a
 *    late-arriving event into the second in progress (#277), so the stream is
 *    emitted in strict timestamp order (asserted before it is pushed).
 *
 * Server build, no BPF, no live PG; ~0.2 s.
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "cmdline.h"
#include "sampler.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static int tests_run = 0;
static int tests_failed = 0;
static int expect_fail = 0;      /* section 5: a red result is the point */

#define CHECK(cond, fmt, ...) do {                                        \
    tests_run++;                                                          \
    if (!(cond)) { tests_failed++;                                        \
        printf("  %s(line %d): " fmt "\n",                                \
               expect_fail ? "red-as-designed" : "FAIL",                  \
               __LINE__, ##__VA_ARGS__); }                                \
    else printf("  ok: " fmt "\n", ##__VA_ARGS__);                        \
} while (0)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

/* >= 120 s, the width at which src/server.c routes to the summary path. */
#define SECS 130

#define PID_CLIENT 41001u
#define EV_CPU      0u
#define EV_LOCK_A   WEI(PG_WAIT_LOCK, 0)
#define EV_IO_A     WEI(PG_WAIT_IO, 21)
#define QID_A 0x2222222222222222ULL

/* One in-command gap: 50 ms wall of which 30 ms measured on-CPU, so the
 * runqueue remainder is 20 ms per second and the two CPU rows can never be
 * each other by accident. */
#define GAP_WALL (50 * MS)
#define GAP_CPU  (30 * MS)
/* One gap OUTSIDE the command window: the relabel's subject. */
#define NONCMD_WALL (20 * MS)

struct fixture {
    char dir[600];
    struct pgwt_trace_event *pushed;   /* what the writer was given (mono)  */
    struct pgwt_trace_event *tagged;   /* the raw oracle (wall + relabelled)*/
    uint32_t *eff;                     /* the producer's resolved ids       */
    int n;
    uint64_t origin, first_sec, last_sec;
    int records;
    int relabelled;                    /* records pgwt_tag_events rewrote   */
    struct pgwt_pid_cat *cats;
    int n_cats;
};

static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

static void push_ev(struct pgwt_trace_event *out, int *n, uint64_t ts,
                    uint32_t ev, uint64_t dur, uint64_t cpu_ns, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;          /* an interval ENDS at its timestamp */
    e.pid          = PID_CLIENT;
    e.old_event    = ev;
    e.new_event    = EV_IO_A;     /* never EXIT, never a marker */
    e.duration_ns  = dur;
    e.query_id     = qid;
    e.cpu_ns       = cpu_ns;
    e.flags        = 0;           /* production pushes no category bit */
    out[(*n)++] = e;
}

static void push_marker(struct pgwt_trace_event *out, int *n, uint64_t ts,
                        uint32_t marker)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid          = PID_CLIENT;
    e.old_event    = marker;
    e.new_event    = marker;
    e.cpu_ns       = PGWT_CPU_NS_UNKNOWN;
    out[(*n)++] = e;
}

static int ts_cmp(const void *a, const void *b)
{
    uint64_t ta = ((const struct pgwt_trace_event *)a)->timestamp_ns;
    uint64_t tb = ((const struct pgwt_trace_event *)b)->timestamp_ns;
    return (ta > tb) - (ta < tb);
}

/* `measured` 0 builds the all-UNKNOWN variant (section 5a); `noncmd` 0 drops
 * the out-of-command gap (section 5b). Both are the shapes in which the
 * assertions below would be satisfied by absence. */
static int build_stream(struct pgwt_trace_event *out, uint64_t base,
                        int measured, int noncmd)
{
    int n = 0;
    for (int s = 0; s < SECS; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        /* THE INTERVALS ARE CONTIGUOUS, which is not decoration: both command
         * gates (pgwt_tag_events offline, pgwt_live_cmd_gate_* live) BANK the
         * command-open time since the pid's previous record and charge it to
         * the next one, because "records of one pid are contiguous in an
         * exact span". A fixture with gaps between its records hands the
         * majority rule banked time from minutes ago and every interval looks
         * in-command -- the relabel then never fires and this file's
         * section 2 refuses the fixture (which is how this shape was found).
         *
         *   [100,150) we==0, in command   -> CPU, 30 of 50 ms measured
         *   [150,160) Lock,  in command   -> a wait that also spun on-CPU
         *   CMD_END at 161 ms
         *   [161,181) we==0, NO command   -> the relabel's subject: raw
         *                                    files it under
         *                                    PGWT_WEI_NONCMD_CPU = idle */
        push_marker(out, &n, sec + 100 * MS, PGWT_MARKER_CMD_START);
        push_ev(out, &n, sec + 150 * MS, EV_CPU, GAP_WALL,
                measured ? GAP_CPU : PGWT_CPU_NS_UNKNOWN, QID_A);
        push_ev(out, &n, sec + 160 * MS, EV_LOCK_A, 10 * MS,
                measured ? 1 * MS : PGWT_CPU_NS_UNKNOWN, QID_A);
        push_marker(out, &n, sec + 161 * MS, PGWT_MARKER_CMD_END);
        if (noncmd)
            push_ev(out, &n, sec + 181 * MS, EV_CPU, NONCMD_WALL,
                    measured ? NONCMD_WALL : PGWT_CPU_NS_UNKNOWN, 0);
    }
    qsort(out, (size_t)n, sizeof(*out), ts_cmp);   /* #277: emit in order */
    return n;
}

#define MAX_EV (SECS * 5 + 8)

static char g_base[300];

static const char *fresh_dir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base, name);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", buf);
    if (system(cmd) != 0) { /* ignore */ }
    mkdir(buf, 0755);
    return buf;
}

struct rec_ctx { int records; uint64_t min_sec, max_sec;
                 uint64_t cpu_measured_ns, cpu_unknown_ns,
                          cpu_measured_records; };

static int rec_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct rec_ctx *c = vctx;
    c->records++;
    if (c->min_sec == 0 || rec->second_wall_ns < c->min_sec)
        c->min_sec = rec->second_wall_ns;
    if (rec->second_wall_ns > c->max_sec) c->max_sec = rec->second_wall_ns;
    c->cpu_measured_ns      += rec->cpu_measured_ns;
    c->cpu_unknown_ns       += rec->cpu_unknown_ns;
    c->cpu_measured_records += rec->cpu_measured_records;
    return 0;
}

/* The registry: one client backend, resolved through the production chain so
 * the fixture never names a category flag. A client resolves to NO flag,
 * which is what makes it a FOREGROUND pid -- the only category the
 * non-command relabel applies to. */
static int registry_build(struct fixture *fx)
{
    char rpath[700];
    snprintf(rpath, sizeof(rpath), "%s/backends.jsonl", fx->dir);
    FILE *rf = fopen(rpath, "w");
    if (!rf) return -1;
    fprintf(rf, "{\"pid\":%u,\"type\":\"%s\"}\n", PID_CLIENT,
            pgwt_backend_type_name(PGWT_BT_CLIENT));
    fclose(rf);
    fx->cats = calloc(1, sizeof(*fx->cats));
    if (!fx->cats) return -1;
    fx->cats[0].pid  = PID_CLIENT;
    fx->cats[0].flag = pgwt_backend_type_flag(PGWT_BT_CLIENT);
    fx->n_cats = 1;
    return 0;
}

/* The OFFLINE PRODUCER's resolve: pgwt_tag_events on a throwaway copy, which
 * is the same classification src/server.c applies to every raw window and the
 * offline equivalent of pgwt_live_effective_event. `force_unresolved` is
 * section 4c's probe: a producer that resolves nothing. */
static int resolve_eff(struct fixture *fx, int force_unresolved)
{
    struct pgwt_trace_event *probe = calloc((size_t)fx->n + 1, sizeof(*probe));
    if (!probe) return -1;
    memcpy(probe, fx->pushed, (size_t)fx->n * sizeof(*probe));
    if (!pgwt_tag_events(probe, fx->n, fx->cats, fx->n_cats)) {
        free(probe);
        return -1;          /* #363: a partly-rewritten oracle is discarded */
    }
    fx->relabelled = 0;
    for (int i = 0; i < fx->n; i++) {
        fx->eff[i] = force_unresolved ? PGWT_SUMMARY_EFF_UNRESOLVED
                                      : probe[i].old_event;
        if (probe[i].old_event != fx->pushed[i].old_event)
            fx->relabelled++;
    }
    free(probe);
    return 0;
}

static long write_summaries(const struct fixture *fx)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) return -1;
    if (pgwt_summary_writer_init(w, fx->dir, 24, NULL) != 0) {
        free(w);
        return -1;
    }
    long pushed = 0;
    for (int i = 0; i < fx->n; i++) {
        if (pgwt_summary_push_event(w, &fx->pushed[i],
                                    /* the producer's category: a client
                                     * backend resolves to 0 */
                                    fx->cats[0].flag, fx->eff[i]) != 0)
            break;
        pushed++;
    }
    pgwt_summary_flush(w);
    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
    return pushed;
}

static int fixture_build(struct fixture *fx, const char *dirname,
                         int measured, int noncmd, int force_unresolved)
{
    memset(fx, 0, sizeof(fx[0]));
    snprintf(fx->dir, sizeof(fx->dir), "%s", fresh_dir(dirname));
    fx->origin = mono_origin();
    fx->pushed = calloc(MAX_EV, sizeof(*fx->pushed));
    fx->tagged = calloc(MAX_EV, sizeof(*fx->tagged));
    fx->eff    = calloc(MAX_EV, sizeof(*fx->eff));
    if (!fx->pushed || !fx->tagged || !fx->eff) return -1;
    fx->n = build_stream(fx->pushed, fx->origin, measured, noncmd);
    for (int i = 1; i < fx->n; i++)
        if (fx->pushed[i - 1].timestamp_ns > fx->pushed[i].timestamp_ns)
            return -1;      /* #277: the fold would move time between seconds */
    if (registry_build(fx) != 0) return -1;
    if (resolve_eff(fx, force_unresolved) != 0) return -1;
    if (write_summaries(fx) != fx->n) return -1;

    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    if (pgwt_visit_summaries(fx->dir, 0, 0, rec_visitor, &rc) < 0
        || rc.records == 0)
        return -1;
    fx->records   = rc.records;
    fx->first_sec = rc.min_sec;
    fx->last_sec  = rc.max_sec;

    /* The raw oracle: the same events on the writer's WALL base (the
     * mono->wall offset is one constant captured when the file opened, so
     * recovering it from the records is exact), annotated by the REAL
     * pgwt_tag_events -- which is where the relabel comes from on this side. */
    uint64_t shift = fx->first_sec - fx->origin;
    memcpy(fx->tagged, fx->pushed, (size_t)fx->n * sizeof(*fx->tagged));
    for (int i = 0; i < fx->n; i++)
        fx->tagged[i].timestamp_ns += shift;
    if (!pgwt_tag_events(fx->tagged, fx->n, fx->cats, fx->n_cats))
        return -1;
    return 0;
}

static void fixture_free(struct fixture *fx)
{
    free(fx->pushed); free(fx->tagged); free(fx->eff); free(fx->cats);
    memset(fx, 0, sizeof(*fx));
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE COMPARISON, and every way it can be blind
 * ══════════════════════════════════════════════════════════════════════════ */

enum verdict { V_AGREE, V_DIVERGE, V_REFUSED };

struct parity {
    enum verdict v;
    const char *refusal;
    double raw_run_ms, sum_run_ms;      /* "CPU (running)"               */
    double raw_wait_ms, sum_wait_ms;    /* "CPU (waiting for a core)"    */
    double raw_star_ms, sum_star_ms;    /* "CPU*", -1 when absent        */
    double raw_db_ms, sum_db_ms, raw_idle_ms, sum_idle_ms;
    int raw_measured, sum_measured;
    int records;
};

static double row_ms(const struct pgwt_tm_result *r, const char *name)
{
    for (int i = 0; i < r->num_rows; i++)
        if (strcmp(r->rows[i].name, name) == 0) return r->rows[i].time_ms;
    return -1.0;
}

static uint64_t overlap_ns(const struct pgwt_trace_event *ev,
                           uint64_t from_ns, uint64_t to_ns)
{
    uint64_t end   = ev->timestamp_ns;
    uint64_t start = end >= ev->duration_ns ? end - ev->duration_ns : 0;
    if (start < from_ns) start = from_ns;
    if (end > to_ns)     end   = to_ns;
    return end > start ? end - start : 0;
}

static struct parity parity_run(const struct fixture *fx,
                                uint64_t from_ns, uint64_t to_ns)
{
    struct parity p;
    memset(&p, 0, sizeof(p));
    static char msg[256];

    /* R1. the raw side must exist at all. */
    if (fx->n == 0) {
        p.v = V_REFUSED; p.refusal = "the raw side holds no events";
        return p;
    }
    /* R2. THE MEASURED SPLIT MUST BE PRESENT IN THE WINDOW. An all-UNKNOWN
     * window makes both paths emit one "CPU*" row and agree, having never
     * exercised the split -- the property would be ABSENT, not equal. */
    uint64_t gap_ns = 0, meas_ns = 0;
    int meas_recs = 0;
    for (int i = 0; i < fx->n; i++) {
        if (fx->tagged[i].old_event != 0) continue;   /* CPU class only */
        uint64_t ov = overlap_ns(&fx->tagged[i], from_ns, to_ns);
        if (!ov) continue;
        gap_ns += ov;
        if (fx->tagged[i].cpu_ns != PGWT_CPU_NS_UNKNOWN) {
            meas_ns += fx->tagged[i].cpu_ns;
            meas_recs++;
        }
    }
    if (meas_recs == 0) {
        p.v = V_REFUSED;
        p.refusal = "no record in this window carries a measured cpu_ns: the "
                    "two CPU rows are UNAVAILABLE here, not equal";
        return p;
    }
    /* R3. and the runqueue remainder must be non-zero, or "CPU (running)"
     * equals the gap total and a reader that dropped the residual entirely
     * would still match. */
    if (meas_ns >= gap_ns) {
        p.v = V_REFUSED;
        p.refusal = "measured CPU covers the whole gap total, so the runqueue "
                    "row is 0 and could not show a difference";
        return p;
    }
    /* R4. THE PRODUCTION PREFLIGHT -- the same call should_use_summaries
     * makes. pgwt_visit_summaries SKIPS a file it cannot open or whose
     * version it refuses and answers from the rest, so without this a stale
     * or corrupt file comes back as a plausible PARTIAL window. */
    int considered = 0, unusable = 0;
    int current = pgwt_summaries_window_current(fx->dir, from_ns, to_ns,
                                               &considered, &unusable);
    if (current != 1 || considered == 0 || unusable != 0) {
        snprintf(msg, sizeof(msg),
                 "the production summary preflight refuses this window "
                 "(verdict %d, %d considered, %d unusable)",
                 current, considered, unusable);
        p.v = V_REFUSED; p.refusal = msg;
        return p;
    }
    /* R5. and the reader must yield records AND a measured-record count in
     * THIS window: cpu_measured_records == 0 is exactly the v6 shape, which
     * reads as "this window cannot split CPU" rather than as an error. */
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    if (pgwt_visit_summaries(fx->dir, from_ns, to_ns, rec_visitor, &rc) < 0
        || rc.records == 0) {
        p.v = V_REFUSED; p.refusal = "the summary reader found no records";
        return p;
    }
    if (rc.cpu_measured_records == 0) {
        p.v = V_REFUSED;
        p.refusal = "the summary records carry no measured-CPU count, so the "
                    "summary side cannot split and 'equal' would be vacuous";
        return p;
    }
    p.records = rc.records;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    double wall_ms = (double)(to_ns - from_ns) / 1e6;
    struct pgwt_tm_result raw, sum;
    memset(&raw, 0, sizeof(raw));
    memset(&sum, 0, sizeof(sum));
    pgwt_compute_time_model(fx->tagged, fx->n, &f, from_ns, to_ns, wall_ms,
                            &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, from_ns, to_ns, &f,
                                           wall_ms, &sum);
    p.raw_run_ms  = row_ms(&raw, "CPU (running)");
    p.sum_run_ms  = row_ms(&sum, "CPU (running)");
    p.raw_wait_ms = row_ms(&raw, "CPU (waiting for a core)");
    p.sum_wait_ms = row_ms(&sum, "CPU (waiting for a core)");
    p.raw_star_ms = row_ms(&raw, "CPU*");
    p.sum_star_ms = row_ms(&sum, "CPU*");
    p.raw_db_ms   = raw.db_time_ms;
    p.sum_db_ms   = sum.db_time_ms;
    p.raw_idle_ms = raw.idle_time_ms;
    p.sum_idle_ms = sum.idle_time_ms;
    p.raw_measured = raw.has_measured_cpu;
    p.sum_measured = sum.has_measured_cpu;
    free(raw.rows);
    free(sum.rows);

    /* R6. both sides must have something to compare. */
    if (p.raw_db_ms == 0.0 && p.sum_db_ms == 0.0) {
        p.v = V_REFUSED; p.refusal = "DB Time is zero on both sides";
        return p;
    }
    p.v = (p.raw_run_ms == p.sum_run_ms && p.raw_wait_ms == p.sum_wait_ms
           && p.raw_db_ms == p.sum_db_ms && p.raw_idle_ms == p.sum_idle_ms)
        ? V_AGREE : V_DIVERGE;
    return p;
}

static void parity_report(const char *label, const struct parity *p)
{
    if (p->v == V_REFUSED) {
        printf("    %-28s REFUSED: %s\n", label, p->refusal);
        return;
    }
    printf("    %-28s run raw %12.3f sum %12.3f | wait raw %12.3f "
           "sum %12.3f\n", label, p->raw_run_ms, p->sum_run_ms,
           p->raw_wait_ms, p->sum_wait_ms);
    printf("    %-28s DB  raw %12.3f sum %12.3f | Idle raw %12.3f "
           "sum %12.3f  [%d records]\n", "", p->raw_db_ms, p->sum_db_ms,
           p->raw_idle_ms, p->sum_idle_ms, p->records);
}

/* THE assertion. Exact equality, no epsilon: both sides divide a sum of
 * integer nanoseconds by 1e6, and section 2 pins that the fixture's own
 * arithmetic is exact in doubles. */
static void parity_assert(const char *label, const struct parity *p)
{
    parity_report(label, p);
    CHECK(p->v != V_REFUSED, "%s: the comparison was possible", label);
    if (p->v == V_REFUSED) return;
    CHECK(p->raw_measured == 1 && p->sum_measured == 1,
          "%s: BOTH paths report has_measured_cpu (raw %d, summary %d)",
          label, p->raw_measured, p->sum_measured);
    CHECK(p->sum_run_ms >= 0 && p->sum_wait_ms >= 0,
          "%s: the summary path emits BOTH CPU rows (run %.3f, wait %.3f) -- "
          "the #408 symptom is one row and a missing runqueue signal",
          label, p->sum_run_ms, p->sum_wait_ms);
    CHECK(p->sum_star_ms < 0 && p->raw_star_ms < 0,
          "%s: neither path emits the combined \"CPU*\" row when the split is "
          "available", label);
    CHECK(p->raw_run_ms == p->sum_run_ms,
          "%s: CPU (running) agrees exactly (%.6f)", label, p->raw_run_ms);
    CHECK(p->raw_wait_ms == p->sum_wait_ms,
          "%s: CPU (waiting for a core) agrees exactly (%.6f)",
          label, p->raw_wait_ms);
    CHECK(p->raw_wait_ms > 0,
          "%s: and the runqueue row is NON-ZERO (%.6f) -- a zero would make "
          "the equality above vacuous", label, p->raw_wait_ms);
    CHECK(p->raw_db_ms == p->sum_db_ms,
          "%s: DB Time agrees exactly (%.6f) -- the non-command relabel is "
          "what moved it", label, p->raw_db_ms);
    CHECK(p->raw_idle_ms == p->sum_idle_ms,
          "%s: Idle agrees exactly (%.6f)", label, p->raw_idle_ms);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 2. THE PREMISES — the fixture must contain what the assertions read
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_premises(const struct fixture *fx)
{
    printf("--- 2. the fixture holds both properties under test ---\n");
    CHECK(fx->records >= SECS - 1,
          "the writer committed %d per-second records for a %d s stream",
          fx->records, SECS);
    CHECK((double)(fx->last_sec + ONE_SEC - fx->first_sec) / 1e9 >= 120.0,
          "the span is %.0f s -- at or above the 120 s width at which "
          "src/server.c routes to the summary path",
          (double)(fx->last_sec + ONE_SEC - fx->first_sec) / 1e9);
    CHECK(fx->relabelled > 0,
          "pgwt_tag_events relabelled %d records to PGWT_WEI_NONCMD_CPU -- "
          "the relabel is produced by the PRODUCTION classifier, never "
          "written into the fixture", fx->relabelled);
    int preflagged = 0, measured = 0;
    for (int i = 0; i < fx->n; i++) {
        if (fx->pushed[i].flags & (PGWT_EVENT_FLAG_IO_WORKER
                                   | PGWT_EVENT_FLAG_MAINT
                                   | PGWT_EVENT_FLAG_BACKGROUND))
            preflagged++;
        if (fx->pushed[i].old_event == PGWT_WEI_NONCMD_CPU)
            preflagged++;
        if (fx->pushed[i].cpu_ns != PGWT_CPU_NS_UNKNOWN) measured++;
    }
    CHECK(preflagged == 0,
          "NOTHING handed to the writer carries a category bit or a "
          "pre-relabelled id (%d) -- the 8705412 pre-cooked-fixture trap",
          preflagged);
    CHECK(measured > 0, "%d records carry a measured cpu_ns", measured);
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    pgwt_visit_summaries(fx->dir, 0, 0, rec_visitor, &rc);
    CHECK(rc.cpu_measured_records > 0,
          "and the FILE carries the measured-record count (%llu) and the "
          "measured total (%llu ns) -- v7's new bytes, read back through the "
          "real reader", (unsigned long long)rc.cpu_measured_records,
          (unsigned long long)rc.cpu_measured_ns);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3/4. THE WINDOW CASES
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_windows(const struct fixture *fx)
{
    printf("--- 3. raw vs summary over three windows ---\n");
    uint64_t end = fx->last_sec + ONE_SEC;
    /* The FULL span first: no record straddles either edge there, so it is
     * the window on which #379 (the writer buckets a long record whole into
     * its closing second) contributes nothing and the CPU rows must match
     * exactly. The interior windows are second-aligned for the same reason --
     * every record in this fixture closes inside one second. */
    struct parity p = parity_run(fx, fx->first_sec, end);
    parity_assert("full span", &p);
    p = parity_run(fx, fx->first_sec + 5 * ONE_SEC, end - 5 * ONE_SEC);
    parity_assert("interior 120 s", &p);
    p = parity_run(fx, end - 125 * ONE_SEC, end);
    parity_assert("125 s to the end", &p);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 5. BYPASS SUITE: every way this gate can stop seeing
 * ══════════════════════════════════════════════════════════════════════════ */

/* Run a real assertion against a crafted bad input so its red output is
 * VISIBLE: the lines are labelled red-as-designed and the failure counter is
 * restored, because these show what the naive comparison would have said --
 * they are not failures of the product. */
#define PROVOKE(stmt) do {                                                 \
    int saved = tests_failed; expect_fail = 1;                             \
    stmt;                                                                  \
    expect_fail = 0; provoked += tests_failed - saved; tests_failed = saved; \
} while (0)

static int provoked = 0;

static void bypass_suite(void)
{
    printf("--- 5. bypass suite (a gate that cannot see must refuse) ---\n");

    /* 5a. ALL-UNKNOWN cpu_ns: both paths emit one "CPU*" row and agree at the
     * same number. This is the shape in which #408 is fully present and a
     * naive comparison is green. */
    {
        struct fixture fx;
        int ok = fixture_build(&fx, "unknown_cpu", 0, 1, 0) == 0;
        CHECK(ok, "5a. a fixture whose every record is cpu_ns UNKNOWN builds");
        if (ok) {
            struct parity p = parity_run(&fx, fx.first_sec,
                                         fx.last_sec + ONE_SEC);
            parity_report("5a all-UNKNOWN", &p);
            CHECK(p.v == V_REFUSED,
                  "5a. REFUSED (%s) -- the split is unavailable, not equal",
                  p.refusal ? p.refusal : "");
            /* ... and what the naive comparison would have said. */
            PROVOKE(parity_assert("5a if it were trusted", &p));
        }
        fixture_free(&fx);
    }

    /* 5b. NO out-of-command gap: the relabel has nothing to act on, so DB
     * Time matches whether or not the writer applies it. The CPU rows still
     * compare (the split IS present), and this case is reported rather than
     * asserted away -- it is why section 2 requires relabelled > 0. */
    {
        struct fixture fx;
        int ok = fixture_build(&fx, "no_noncmd", 1, 0, 0) == 0;
        CHECK(ok, "5b. a fixture with no out-of-command gap builds");
        if (ok) {
            CHECK(fx.relabelled == 0,
                  "5b. and pgwt_tag_events relabels nothing in it (%d) -- the "
                  "relabel assertion would be satisfied by ABSENCE",
                  fx.relabelled);
        }
        fixture_free(&fx);
    }

    /* 5c. THE REACHABILITY PROBE, and the most important case here: a
     * producer that resolves NOTHING (PGWT_SUMMARY_EFF_UNRESOLVED for every
     * record) must make the comparison go RED. If it does not, the eff_event
     * parameter is dead code and the v7 relabel is decoration -- which is
     * exactly the N7 failure recorded in tests/test_agg_raw_crosscheck.c,
     * where a writer-side guard greened a test while doing nothing in the
     * daemon. */
    {
        struct fixture fx;
        int ok = fixture_build(&fx, "unresolved", 1, 1, 1) == 0;
        CHECK(ok, "5c. a fixture whose producer resolves nothing builds");
        if (ok) {
            struct parity p = parity_run(&fx, fx.first_sec,
                                         fx.last_sec + ONE_SEC);
            parity_report("5c producer unresolved", &p);
            CHECK(p.v == V_DIVERGE,
                  "5c. the comparison DIVERGES when the producer resolves "
                  "nothing -- the eff_event parameter is load-bearing, not "
                  "decoration");
            CHECK(p.sum_db_ms > p.raw_db_ms,
                  "5c. and it diverges in the #408 DIRECTION: summary DB Time "
                  "%.3f over raw %.3f, i.e. non-command CPU counted as load",
                  p.sum_db_ms, p.raw_db_ms);
            CHECK(p.sum_run_ms >= 0 && p.sum_wait_ms >= 0,
                  "5c. while the SPLIT still works (run %.3f, wait %.3f) -- "
                  "the two halves of v7 are independent", p.sum_run_ms,
                  p.sum_wait_ms);
            PROVOKE(parity_assert("5c if it were trusted", &p));
        }
        fixture_free(&fx);
    }

    /* 5d. A WINDOW THE SUMMARY READER FINDS NOTHING IN: every summary total
     * reads 0.000 ms, which a naive comparison calls agreement. */
    {
        struct fixture fx;
        if (fixture_build(&fx, "window_miss", 1, 1, 0) == 0) {
            uint64_t far = fx.last_sec + 3600 * ONE_SEC;
            struct parity p = parity_run(&fx, far, far + 130 * ONE_SEC);
            parity_report("5d window misses records", &p);
            CHECK(p.v == V_REFUSED,
                  "5d. REFUSED (%s)", p.refusal ? p.refusal : "");
        }
        fixture_free(&fx);
    }

    /* 5e. A MISSING SUMMARY DIRECTORY: same zero, different cause. */
    {
        struct fixture fx;
        if (fixture_build(&fx, "missing_dir", 1, 1, 0) == 0) {
            char cmd[800];
            snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fx.dir);
            if (system(cmd) != 0) { /* ignore */ }
            struct parity p = parity_run(&fx, fx.first_sec,
                                         fx.last_sec + ONE_SEC);
            parity_report("5e summary dir removed", &p);
            CHECK(p.v == V_REFUSED,
                  "5e. REFUSED (%s)", p.refusal ? p.refusal : "");
        }
        fixture_free(&fx);
    }

    /* 5f. A WRONG-VERSION FILE IN THE WINDOW. v6 files exist on every box
     * running the previous daemon, and the whole v7 contract is that they are
     * REFUSED rather than reinterpreted -- a v6 record ends 40 bytes before a
     * v7 one, so reading it as v7 is a parse off the end. The preflight the
     * server runs must refuse the window, which is what sends the request to
     * the raw path. */
    {
        struct fixture fx;
        if (fixture_build(&fx, "old_version", 1, 1, 0) == 0) {
            /* Patch the on-disk header version to 6 the way a previous
             * daemon would have written it: same magic, older rule. */
            char path[800];
            snprintf(path, sizeof(path), "%s/current.summary", fx.dir);
            FILE *fp = fopen(path, "r+b");
            int patched = 0;
            if (fp) {
                struct pgwt_trace_file_header hdr;
                if (fread(&hdr, sizeof(hdr), 1, fp) == 1
                    && hdr.magic == PGWT_SUMMARY_MAGIC) {
                    hdr.version = PGWT_SUMMARY_VERSION - 1;
                    if (fseek(fp, 0, SEEK_SET) == 0
                        && fwrite(&hdr, sizeof(hdr), 1, fp) == 1)
                        patched = 1;
                }
                fclose(fp);
            }
            CHECK(patched, "5f. a summary file was rewritten as v%d",
                  PGWT_SUMMARY_VERSION - 1);
            if (patched) {
                struct parity p = parity_run(&fx, fx.first_sec,
                                             fx.last_sec + ONE_SEC);
                parity_report("5f v6 file in the window", &p);
                CHECK(p.v == V_REFUSED,
                      "5f. REFUSED (%s) -- an older-rule file is never "
                      "reinterpreted", p.refusal ? p.refusal : "");
            }
        }
        fixture_free(&fx);
    }

    /* 5g. A TRUNCATED v7 RECORD. The measured-CPU scalars are the LAST bytes
     * of the payload, so a short block is precisely the input that would
     * leave them at 0 -- i.e. silently reproduce the v6 behaviour for part of
     * a window. pgwt_summary_deserialize must refuse it. Driven at the
     * function, because pgwt_visit_summaries skips an undecodable block. */
    {
        struct pgwt_summary_accum *acc = calloc(1, sizeof(*acc));
        uint8_t *buf = malloc(PGWT_SUMMARY_SERIALIZE_MAX);
        CHECK(acc && buf, "5g. buffers");
        if (acc && buf) {
            acc->class_ns[PGWT_CLASS_CPU] = 50 * MS;
            acc->cpu_measured_ns = 30 * MS;
            acc->cpu_measured_records = 1;
            size_t sz = pgwt_summary_serialize(acc, buf,
                                               PGWT_SUMMARY_SERIALIZE_MAX);
            CHECK(sz > 40, "5g. a record serializes to %zu bytes", sz);
            struct pgwt_summary_accum *back = calloc(1, sizeof(*back));
            if (back && sz > 40) {
                CHECK(pgwt_summary_deserialize(buf, sz, back,
                                               PGWT_SUMMARY_VERSION) == 0,
                      "5g. the full record decodes (the control)");
                CHECK(back->cpu_measured_ns == 30 * MS
                      && back->cpu_measured_records == 1,
                      "5g. and round-trips the v7 scalars (%llu ns, %llu "
                      "records)",
                      (unsigned long long)back->cpu_measured_ns,
                      (unsigned long long)back->cpu_measured_records);
                memset(back, 0, sizeof(*back));
                CHECK(pgwt_summary_deserialize(buf, sz - 1, back,
                                               PGWT_SUMMARY_VERSION) != 0,
                      "5g. one byte short is REFUSED, not decoded with the "
                      "scalars left at 0");
                memset(back, 0, sizeof(*back));
                CHECK(pgwt_summary_deserialize(buf, sz - 40, back,
                                               PGWT_SUMMARY_VERSION) != 0,
                      "5g. a payload missing the whole v7 trailer (a v6-shaped "
                      "record) is REFUSED");
            }
            free(back);
        }
        free(acc); free(buf);
    }

    /* 5h. AN ILLEGAL eff_event. The parameter may express exactly one
     * rewrite; anything else must be refused and counted, or a caller could
     * refile arbitrary time into an arbitrary class through it. */
    {
        const char *dir = fresh_dir("illegal_eff");
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
              "5h. writer opens");
        if (w && w->enabled) {
            uint64_t sec = mono_origin();
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = sec + 300 * MS;
            e.pid = PID_CLIENT;
            e.old_event = EV_CPU;
            e.new_event = EV_IO_A;
            e.duration_ns = 50 * MS;
            e.cpu_ns = 30 * MS;
            pgwt_summary_push_event(w, &e, 0, EV_LOCK_A);  /* not legal */
            CHECK(w->eff_rejected_events_total == 1,
                  "5h. an eff_event that is neither the record's id nor "
                  "PGWT_WEI_NONCMD_CPU is REFUSED and counted (%llu)",
                  (unsigned long long)w->eff_rejected_events_total);
            CHECK(w->accum.class_ns[PGWT_CLASS_CPU] == 50 * MS,
                  "5h. and the record keeps its own class (%llu ns in CPU) "
                  "rather than the one the caller asked for",
                  (unsigned long long)w->accum.class_ns[PGWT_CLASS_CPU]);
            e.timestamp_ns = sec + 400 * MS;
            pgwt_summary_push_event(w, &e, 0, PGWT_SUMMARY_EFF_UNRESOLVED);
            CHECK(w->eff_unresolved_events_total == 1,
                  "5h. and an UNRESOLVED push is counted too (%llu) -- the v6 "
                  "behaviour stays visible instead of silent",
                  (unsigned long long)w->eff_unresolved_events_total);
            pgwt_summary_close(w);
            pgwt_summary_destroy(w);
        }
        free(w);
    }
}

int main(void)
{
    printf("=== CPU-split raw-vs-summary parity (#408) ===\n\n");
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_cpu_split_%d", (int)getpid());
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_base, g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("--- 1. the fixture ---\n");
    struct fixture fx;
    int built = fixture_build(&fx, "main", 1, 1, 0) == 0;
    CHECK(built, "the fixture builds (writer, registry, reader, raw oracle)");
    if (built) {
        test_premises(&fx);
        test_windows(&fx);
    }
    fixture_free(&fx);

    bypass_suite();

    /* The PROVOKE lines are red BY DESIGN -- they print what the naive
     * comparison would have said about an input this gate refuses. They are
     * excluded from the verdict, and the count is printed so a reader can
     * see they actually fired: zero would mean the crafted inputs stopped
     * being detectable and the refusals are no longer being demonstrated. */
    printf("\n%d expected-red lines fired (each a shape where a naive "
           "comparison reads agreement)\n", provoked);
    if (provoked == 0) {
        printf("  FAIL: no expected-red line fired -- the bypass cases are "
               "no longer reaching the assertions they are meant to trip\n");
        tests_failed++;
    }
    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
