/* test_straddler_accounting.c — #379: a long interval's duration must land in
 * the seconds it OCCUPIED, not whole in the second it closed in.
 *
 * WHAT IS ASSERTED, and why each assertion can go red
 * ────────────────────────────────────────────────────────────────────────
 * The product rule (summary v8) is a WATERMARK: a producer tells the writer
 * what each pid has open (pgwt_summary_note_open) and the writer credits
 * each completed second's share as that second is written; the closing
 * record then contributes only the remainder. So the gates here are:
 *
 *  §1 PLACEMENT. A 10 s Client:ClientRead closing at t0+10 must put 1 s of
 *     idle in EACH of the ten seconds it covered. Red input: charging it
 *     whole to the closing second (the v7 rule, i.e. this fix reverted) puts
 *     10 s in one second and 0 in nine.
 *  §2 CONSERVATION, which §1 alone does not give. The sum over every record
 *     must equal the interval's duration EXACTLY — once, not 1+2+...+10,
 *     which is what the live view's open-stretch rule would produce in a
 *     persistent writer (pgwt_live_account_open_stretch is correct only
 *     because d->accum is rebuilt every tick). Red input: crediting without
 *     a watermark; crediting again at close.
 *  §3 OCCURRENCE QUANTITIES STAY WHOLE AND ONCE. count == 1, histogram has
 *     exactly one observation, and it is in the 10 s bucket — not ten 1 s
 *     ones. Red input: spreading count/histogram with the duration.
 *  §4 EITHER ORDER of (tick, close). A tick that runs PAST the close must
 *     not make the total exceed the duration, and a close that arrives
 *     before any tick must still account for the whole interval.
 *  §5 THE QUANTITIES THE RULE REFUSES TO SPREAD: queries[] (open-interval
 *     query attribution is unresolved at credit time, #128/#324) and an
 *     on-CPU gap (no cpu_ns exists until close, so spreading its wall would
 *     clamp v7's measured-CPU split to zero). Both must keep the v7 answer,
 *     and the CPU one must be COUNTED.
 *
 * THE BYPASS SUITE (§6) — every way this gate could pass while the product
 * is broken, each made to fail:
 *   a. NO PRODUCER NOTES. The writer must then behave exactly as v7. This is
 *      the reachability probe: if §1 still passed with every note removed,
 *      the credit path would be dead code and the gate would be measuring
 *      nothing. It must go RED.
 *   b. A STALE NOTE. A producer that stops refreshing (a pid that died
 *      without an EXIT record) must stop accruing, within
 *      PGWT_SUMMARY_OPEN_STALE_NS — a watermark that credits forever
 *      fabricates idle time out of a dead backend.
 *   c. AN EXIT. pgwt_summary_forget_open must retire the pid exactly.
 *   d. A MISMATCHED NOTE (the producer's open interval is not the one the
 *      record closes — overlapping intervals on one pid). The record must be
 *      charged WHOLE and never double counted.
 *   e. AN IO_WORKER pid. v6 keeps its time out of the model; the open path
 *      must refuse it too, or v6's exclusion is relocated rather than kept.
 *   f. THE TABLE FULL. Refusal must be counted, and the un-noted pid must
 *      degrade to v7 rather than to nothing.
 *   g. A WINDOW WITH NO RECORDS and a MISSING DIRECTORY: the read-back must
 *      refuse, so a run that measured nothing cannot report agreement.
 *   h. THE CREDIT ARRIVING LATE (first note after the seconds were written).
 *      The slice that can no longer be placed must ride on the closing
 *      record — never vanish — and be counted.
 *
 * Server build (no BPF, no live PG), ~0.2 s. */
#define _GNU_SOURCE
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond, fmt, ...) do {                                        \
    checks++;                                                             \
    if (!(cond)) {                                                        \
        failures++;                                                       \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                       \
    } else {                                                              \
        printf("  ok:   " fmt "\n", ##__VA_ARGS__);                       \
    }                                                                     \
} while (0)

#define S  1000000000ULL
#define MS 1000000ULL

#define PID_A        1001u
#define PID_IOW      1002u
#define BASE_MONO    (10000ULL * S)      /* second-aligned synthetic clock */

static const uint32_t EV_CLIENT_READ = (PG_WAIT_CLIENT << 24) | 0;
static const uint32_t EV_LOCK_REL    = (PG_WAIT_LOCK   << 24) | 0;
static const uint32_t EV_IO_READ     = (PG_WAIT_IO     << 24) | 1;

/* ── fixture plumbing ─────────────────────────────────────────────────── */

static const char *fresh_dir(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof(buf), "/tmp/pgwt_straddle_%s_%d", name, (int)getpid());
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", buf, buf);
    if (system(cmd) != 0)
        return NULL;
    return buf;
}

struct sec_row {
    uint64_t wall;
    uint64_t idle_ns;
    uint64_t class_ns[PGWT_NUM_CLASSES];
    uint64_t ev_total_ns;      /* events[] total for the event under test */
    uint64_t ev_count;
    uint64_t ev_max_ns;
    uint64_t ev_hist[HISTOGRAM_BUCKETS];
    uint64_t sess_db_ns;
    uint64_t query_total_ns;
    uint64_t cpu_measured_ns;
    uint64_t cpu_unknown_ns;
    uint64_t cpu_clamped_ns;
};

struct readback {
    struct sec_row rows[4096];
    int n;
    uint32_t want_event;
    uint32_t want_pid;
};

static int rb_visit(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct readback *rb = vctx;
    if (rb->n >= 4096)
        return 1;
    struct sec_row *r = &rb->rows[rb->n++];
    memset(r, 0, sizeof(*r));
    r->wall = rec->second_wall_ns;
    r->idle_ns = rec->idle_ns;
    memcpy(r->class_ns, rec->class_ns, sizeof(r->class_ns));
    r->cpu_measured_ns = rec->cpu_measured_ns;
    r->cpu_unknown_ns  = rec->cpu_unknown_ns;
    r->cpu_clamped_ns  = rec->cpu_clamped_ns;
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        if (rec->events[i].event_id != rb->want_event)
            continue;
        r->ev_total_ns = rec->events[i].total_ns;
        r->ev_count    = rec->events[i].count;
        r->ev_max_ns   = rec->events[i].max_ns;
        memcpy(r->ev_hist, rec->events[i].histogram, sizeof(r->ev_hist));
    }
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (rec->sessions[i].pid == rb->want_pid)
            r->sess_db_ns = rec->sessions[i].db_time_ns;
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++)
        if (rec->queries[i].query_id != 0)
            r->query_total_ns += rec->queries[i].total_ns;
    return 0;
}

/* Read every record back. Returns the count, or -1 — never 0-as-success:
 * "no records" must fail the caller, not read as agreement (bypass §6g). */
static int read_back(const char *dir, struct readback *rb)
{
    rb->n = 0;
    int rc = pgwt_visit_summaries(dir, 0, 0, rb_visit, rb);
    if (rc <= 0)
        return -1;
    return rb->n;
}

static uint64_t rb_sum_idle(const struct readback *rb)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].idle_ns;
    return t;
}
static uint64_t rb_sum_class(const struct readback *rb, int cls)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].class_ns[cls];
    return t;
}
static uint64_t rb_sum_ev(const struct readback *rb)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].ev_total_ns;
    return t;
}
static uint64_t rb_sum_ev_count(const struct readback *rb)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].ev_count;
    return t;
}
static uint64_t rb_sum_query(const struct readback *rb)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].query_total_ns;
    return t;
}
/* How many distinct seconds carry any of the event's time. */
static int rb_seconds_with_ev(const struct readback *rb)
{
    int n = 0;
    for (int i = 0; i < rb->n; i++) if (rb->rows[i].ev_total_ns) n++;
    return n;
}

static struct pgwt_trace_event mkev(uint32_t pid, uint64_t ts, uint64_t dur,
                                    uint32_t old_ev, uint32_t new_ev,
                                    uint64_t qid, uint64_t cpu)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.pid = pid;
    e.timestamp_ns = ts;
    e.duration_ns = dur;
    e.old_event = old_ev;
    e.new_event = new_ev;
    e.query_id = qid;
    e.cpu_ns = cpu;
    return e;
}

struct wr {
    struct pgwt_summary_writer *w;
    char dir[512];
};

static int wr_open(struct wr *x, const char *name)
{
    const char *d = fresh_dir(name);
    if (!d) return -1;
    snprintf(x->dir, sizeof(x->dir), "%s", d);
    x->w = calloc(1, sizeof(*x->w));
    if (!x->w) return -1;
    if (pgwt_summary_writer_init(x->w, x->dir, 24, NULL) != 0)
        return -1;
    pgwt_summary_pin_clock(x->w, BASE_MONO, BASE_MONO);
    return 0;
}

static void wr_close(struct wr *x)
{
    pgwt_summary_flush(x->w);
    pgwt_summary_close(x->w);
    pgwt_summary_destroy(x->w);
    free(x->w);
    x->w = NULL;
}

/* One tick of a producer that keeps a pid's note fresh, exactly as
 * src/map_reader.c does for every live backend on every display tick. */
static void tick(struct wr *x, uint64_t now, uint32_t pid, uint32_t we,
                 uint64_t start, uint32_t cat)
{
    if (we != 0xFFFFFFFFu)
        pgwt_summary_note_open(x->w, pid, we, start, cat, now);
    pgwt_summary_flush_completed(x->w, now);
}

/* ── §1-§3: one 10 s idle interval, ticked every second ──────────────── */

static void test_placement_and_conservation(int with_notes)
{
    struct wr x;
    const char *tag = with_notes ? "placed" : "nonotes";
    if (wr_open(&x, tag) != 0) { printf("  FAIL: setup\n"); failures++; return; }

    /* A prior 1 s record anchors the file at second 0 and gives the pid a
     * closed interval before the long one, exactly as a real stream does. */
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, EV_CLIENT_READ,
             77, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);

    /* [1 s, 11 s): a 10 s Client:ClientRead. Ten ticks while it is open. */
    for (int s = 2; s <= 11; s++)
        tick(&x, BASE_MONO + s * S + 200 * MS, PID_A,
             with_notes ? EV_CLIENT_READ : 0xFFFFFFFFu, BASE_MONO + 1 * S, 0);

    struct pgwt_trace_event close_rec =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, EV_CLIENT_READ, EV_LOCK_REL,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &close_rec, 0, close_rec.old_event);
    wr_close(&x);

    struct readback rb = { .want_event = EV_CLIENT_READ, .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "%s: records read back (%d)", tag, n);
    if (n <= 0) return;

    uint64_t total = rb_sum_ev(&rb);
    CHECK(total == 10 * S,
          "%s: CONSERVATION sum(events[ClientRead].total_ns) = %llu ms "
          "(exactly the 10000 ms duration, once)",
          tag, (unsigned long long)(total / MS));
    CHECK(rb_sum_idle(&rb) == 10 * S,
          "%s: CONSERVATION sum(idle_ns) = %llu ms",
          tag, (unsigned long long)(rb_sum_idle(&rb) / MS));

    int spread = rb_seconds_with_ev(&rb);
    if (with_notes) {
        CHECK(spread == 10,
              "PLACEMENT: the 10 s wait appears in %d distinct seconds "
              "(the ten it occupied), not 1", spread);
        for (int i = 0; i < rb.n; i++) {
            uint64_t sec = (rb.rows[i].wall - BASE_MONO) / S;
            if (sec >= 1 && sec <= 10)
                CHECK(rb.rows[i].ev_total_ns == 1 * S,
                      "PLACEMENT: second %llu carries 1000 ms of it (%llu ms)",
                      (unsigned long long)sec,
                      (unsigned long long)(rb.rows[i].ev_total_ns / MS));
        }
    } else {
        /* §6a, the reachability probe: with every note withheld the writer
         * MUST fall back to v7 — all 10 s in the closing second. If this
         * ever read 10, the credit path would not be the thing §1 measures. */
        CHECK(spread == 1,
              "BYPASS a (no producer notes): the whole 10 s lands in ONE "
              "second (the v7 rule) — %d seconds carry it", spread);
    }

    CHECK(rb_sum_ev_count(&rb) == 1,
          "%s: ONCE — events[].count == %llu over the whole file",
          tag, (unsigned long long)rb_sum_ev_count(&rb));
    uint64_t maxns = 0, hist_obs = 0;
    int hist_bucket = -1;
    for (int i = 0; i < rb.n; i++) {
        if (rb.rows[i].ev_max_ns > maxns) maxns = rb.rows[i].ev_max_ns;
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
            if (rb.rows[i].ev_hist[b]) {
                hist_obs += rb.rows[i].ev_hist[b];
                hist_bucket = b;
            }
    }
    CHECK(maxns == 10 * S,
          "%s: WHOLE — max_ns = %llu ms (the longest single wait, not 1000)",
          tag, (unsigned long long)(maxns / MS));
    CHECK(hist_obs == 1 && hist_bucket == HISTOGRAM_BUCKETS - 1,
          "%s: WHOLE — one histogram observation, in the top bucket "
          "(obs=%llu bucket=%d)", tag, (unsigned long long)hist_obs,
          hist_bucket);
}

/* ── §4: the tick and the close in either order ──────────────────────── */

static void test_tick_close_order(void)
{
    /* (a) a tick that runs PAST the close: the writer is ticked at t=12 s
     *     while the record that ended at 11 s has not been drained yet. */
    struct wr x;
    if (wr_open(&x, "past") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, EV_CLIENT_READ,
             77, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 13; s++)      /* two ticks beyond the real close */
        tick(&x, BASE_MONO + s * S + 200 * MS, PID_A, EV_CLIENT_READ,
             BASE_MONO + 1 * S, 0);
    struct pgwt_trace_event close_rec =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, EV_CLIENT_READ, EV_LOCK_REL,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &close_rec, 0, close_rec.old_event);
    wr_close(&x);
    struct readback rb = { .want_event = EV_CLIENT_READ, .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "tick-past-close: records read back (%d)", n);
    if (n > 0)
        CHECK(rb_sum_ev(&rb) == 12 * S,
              "ORDER: a tick running past the close credits the OPEN "
              "stretch and no more — %llu ms for 10 s closed + 2 s still "
              "open, never 10 s twice",
              (unsigned long long)(rb_sum_ev(&rb) / MS));

    /* (b) the close FIRST, with no tick at all in between: the v7 answer,
     *     whole and exactly once. */
    struct wr y;
    if (wr_open(&y, "closefirst") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    pgwt_summary_note_open(y.w, PID_A, EV_CLIENT_READ, BASE_MONO + 1 * S, 0,
                           BASE_MONO + 1 * S);
    pgwt_summary_push_event(y.w, &close_rec, 0, close_rec.old_event);
    wr_close(&y);
    struct readback rb2 = { .want_event = EV_CLIENT_READ, .want_pid = PID_A };
    int n2 = read_back(y.dir, &rb2);
    CHECK(n2 > 0, "close-before-any-tick: records read back (%d)", n2);
    if (n2 > 0)
        CHECK(rb_sum_ev(&rb2) == 10 * S,
              "ORDER: close before any tick accounts for the whole 10 s "
              "exactly once (%llu ms)",
              (unsigned long long)(rb_sum_ev(&rb2) / MS));
}

/* ── §5: what the rule refuses to spread ─────────────────────────────── */

static void test_refusals(void)
{
    /* (a) QUERIES. The same 10 s wait, with a query id: the per-query total
     *     must stay whole-at-close (open-interval attribution is unresolved
     *     at credit time, and Queries always raw-loads, #324). */
    struct wr x;
    if (wr_open(&x, "queries") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, EV_LOCK_REL,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 11; s++)
        tick(&x, BASE_MONO + s * S + 200 * MS, PID_A, EV_LOCK_REL,
             BASE_MONO + 1 * S, 0);
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, EV_LOCK_REL, EV_LOCK_REL,
             4242, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    wr_close(&x);
    struct readback rb = { .want_event = EV_LOCK_REL, .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "queries: records read back (%d)", n);
    if (n > 0) {
        CHECK(rb_sum_class(&rb, PGWT_CLASS_LOCK) == 11 * S,
              "REFUSAL(queries): the class total is still spread and exact "
              "(%llu ms = 1 s seed + 10 s wait)",
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_LOCK) / MS));
        /* 11 s, not 10: #128's deferral parks the seed record's 1 s on the
         * pid until a command reports an id, so query 4242 is credited both
         * records' FULL durations at close. Which is the point -- neither is
         * spread, and the per-query total is the v7 answer exactly. */
        CHECK(rb_sum_query(&rb) == 11 * S,
              "REFUSAL(queries): queries[] keeps the WHOLE durations at "
              "close (%llu ms = the 10 s wait + the 1 s #128 defers onto "
              "it), never a per-second credit",
              (unsigned long long)(rb_sum_query(&rb) / MS));
    }

    /* (b) AN ON-CPU GAP. A 10 s we==0 interval with a measured cpu_ns must
     *     keep the v7 whole-at-close behaviour, or its measurement would be
     *     clamped away; and the refusal must be COUNTED. */
    struct wr y;
    if (wr_open(&y, "cpugap") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 11; s++)
        tick(&y, BASE_MONO + s * S + 200 * MS, PID_A, 0, BASE_MONO + 1 * S, 0);
    struct pgwt_trace_event cpu =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 6 * S);
    pgwt_summary_push_event(y.w, &cpu, 0, cpu.old_event);
    uint64_t refused = y.w->open_cpu_refused_total;
    uint64_t uncredited = y.w->cpu_open_uncredited_ns_total;
    wr_close(&y);
    struct readback rb2 = { .want_event = 0, .want_pid = PID_A };
    int n2 = read_back(y.dir, &rb2);
    CHECK(n2 > 0, "cpu gap: records read back (%d)", n2);
    if (n2 > 0) {
        uint64_t m = 0, clamped = 0;
        for (int i = 0; i < rb2.n; i++) {
            m += rb2.rows[i].cpu_measured_ns;
            clamped += rb2.rows[i].cpu_clamped_ns;
        }
        CHECK(m == 6 * S && clamped == 0,
              "REFUSAL(cpu): v7's measured split survives — cpu_measured "
              "%llu ms, clamped %llu ms (spreading the wall would have "
              "clamped all 6000 ms away)",
              (unsigned long long)(m / MS), (unsigned long long)(clamped / MS));
        CHECK(rb_sum_class(&rb2, PGWT_CLASS_CPU) == 10 * S,
              "REFUSAL(cpu): the gap's wall is still accounted exactly once "
              "(%llu ms)",
              (unsigned long long)(rb_sum_class(&rb2, PGWT_CLASS_CPU) / MS));
    }
    CHECK(refused > 0,
          "REFUSAL(cpu): counted — open_cpu_refused_total = %llu",
          (unsigned long long)refused);
    CHECK(uncredited == 10 * S,
          "REFUSAL(cpu): the residual #379 leaves open is MEASURED — "
          "cpu_open_uncredited_ns_total = %llu ms",
          (unsigned long long)(uncredited / MS));
}

/* ── §6b-§6h: the rest of the bypass suite ───────────────────────────── */

static void test_bypass(void)
{
    /* b. A STALE NOTE: the producer notes once and then stops (a pid that
     *    died without an EXIT record). Crediting must stop. */
    struct wr x;
    if (wr_open(&x, "stale") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, EV_CLIENT_READ,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    pgwt_summary_note_open(x.w, PID_A, EV_CLIENT_READ, BASE_MONO + 1 * S, 0,
                           BASE_MONO + 1 * S);
    /* Another pid keeps the clock moving for 20 s without refreshing PID_A. */
    for (int s = 2; s <= 21; s++)
        pgwt_summary_flush_completed(x.w, BASE_MONO + s * S + 200 * MS);
    uint64_t credited = x.w->open_credited_ns_total;
    uint64_t expired = x.w->open_notes_expired_total;
    wr_close(&x);
    CHECK(credited <= 4 * S,
          "BYPASS b (stale note): crediting stopped at %llu ms, within the "
          "3 s staleness bound — a dead backend cannot accrue idle forever",
          (unsigned long long)(credited / MS));
    CHECK(expired > 0,
          "BYPASS b: the expiry is COUNTED (open_notes_expired_total = %llu)",
          (unsigned long long)expired);

    /* c. AN EXIT retires the note exactly, without waiting for staleness. */
    struct wr y;
    if (wr_open(&y, "exit") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    pgwt_summary_note_open(y.w, PID_A, EV_CLIENT_READ, BASE_MONO + 1 * S, 0,
                           BASE_MONO + 1 * S);
    pgwt_summary_forget_open(y.w, PID_A);
    for (int s = 2; s <= 5; s++)
        pgwt_summary_flush_completed(y.w, BASE_MONO + s * S + 200 * MS);
    uint64_t after_exit = y.w->open_credited_ns_total;
    wr_close(&y);
    CHECK(after_exit == 0,
          "BYPASS c (exit): nothing credited after forget_open (%llu ms)",
          (unsigned long long)(after_exit / MS));

    /* d. A MISMATCHED NOTE: the note describes a different interval from the
     *    one the record closes (two overlapping intervals on one pid — the
     *    shape that made the parity fixture's 89,000 ms IO read 49,499 ms
     *    before the identity check existed). Charge it WHOLE, once. */
    struct wr z;
    if (wr_open(&z, "mismatch") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(z.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 11; s++)
        tick(&z, BASE_MONO + s * S + 200 * MS, PID_A, EV_CLIENT_READ,
             BASE_MONO + (uint64_t)(s - 1) * S, 0);   /* a NEW interval each tick */
    struct pgwt_trace_event longrec =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, EV_IO_READ, EV_LOCK_REL,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(z.w, &longrec, 0, longrec.old_event);
    uint64_t mism = z.w->open_note_mismatch_total;
    wr_close(&z);
    struct readback rb = { .want_event = EV_IO_READ, .want_pid = PID_A };
    struct readback rbc = { .want_event = EV_CLIENT_READ, .want_pid = PID_A };
    int n = read_back(z.dir, &rb);
    int nc = read_back(z.dir, &rbc);
    CHECK(n > 0 && nc > 0, "mismatch: records read back (%d)", n);
    if (n > 0 && nc > 0)
        /* NO DOUBLE COUNT is the invariant, not "the record keeps all of
         * it": the bogus notes really did place 10 s of wall over the same
         * span, so the closing record may only contribute what the pid's
         * monotone watermark did NOT already place. The wall is therefore
         * accounted exactly once across the two labels -- the attribution is
         * wrong (that is the producer's bug, and it is counted below), the
         * arithmetic is not. */
        CHECK(rb_sum_ev(&rb) + rb_sum_ev(&rbc) == 10 * S,
              "BYPASS d (mismatched note): the 10 s of wall is accounted "
              "exactly ONCE across both labels (%llu ms + %llu ms), never "
              "twice", (unsigned long long)(rb_sum_ev(&rb) / MS),
              (unsigned long long)(rb_sum_ev(&rbc) / MS));
    CHECK(mism > 0,
          "BYPASS d: the mismatch is COUNTED (open_note_mismatch_total = %llu)",
          (unsigned long long)mism);

    /* e. AN IO_WORKER pid is refused by the open path, as v6 refuses its
     *    records — otherwise v6's exclusion is relocated, not kept. */
    struct wr iw;
    if (wr_open(&iw, "iow") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(iw.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 11; s++)
        tick(&iw, BASE_MONO + s * S + 200 * MS, PID_IOW, EV_CLIENT_READ,
             BASE_MONO + 1 * S, PGWT_EVENT_FLAG_IO_WORKER);
    uint64_t iw_credited = iw.w->open_credited_ns_total;
    uint64_t iw_refused = iw.w->open_io_worker_refused_total;
    wr_close(&iw);
    CHECK(iw_credited == 0 && iw_refused > 0,
          "BYPASS e (io_worker): nothing credited (%llu ms), refusal counted "
          "(%llu)", (unsigned long long)iw_credited,
          (unsigned long long)iw_refused);

    /* f. THE TABLE FULL: every slot taken by distinct pids. The refusal is
     *    counted and the un-noted pid degrades to v7, not to nothing. */
    struct wr fu;
    if (wr_open(&fu, "full") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(fu.w, &seed, 0, seed.old_event);
    for (int i = 0; i < SUMMARY_MAX_OPEN + 8; i++)
        pgwt_summary_note_open(fu.w, 20000u + (uint32_t)i, EV_CLIENT_READ,
                               BASE_MONO + 1 * S, 0, BASE_MONO + 1 * S);
    uint64_t full = fu.w->open_table_full_total;
    wr_close(&fu);
    CHECK(full > 0,
          "BYPASS f (table full): refusal counted (open_table_full_total = "
          "%llu)", (unsigned long long)full);

    /* g. A MISSING DIRECTORY must make the read-back REFUSE, so a run that
     *    measured nothing cannot be mistaken for agreement. */
    struct readback empty = { .want_event = EV_CLIENT_READ, .want_pid = PID_A };
    CHECK(read_back("/tmp/pgwt_straddle_does_not_exist", &empty) < 0,
          "BYPASS g (no such trace dir): the read-back REFUSES rather than "
          "returning an empty, agreeing answer");

    /* h. A LATE FIRST NOTE: the interval's early seconds were written before
     *    the producer said anything. That slice must ride on the closing
     *    record — never vanish — and be counted. */
    struct wr lt;
    if (wr_open(&lt, "late") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(lt.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 8; s++)            /* seconds written, no note yet */
        pgwt_summary_flush_completed(lt.w, BASE_MONO + s * S + 200 * MS);
    for (int s = 9; s <= 11; s++)
        tick(&lt, BASE_MONO + s * S + 200 * MS, PID_A, EV_CLIENT_READ,
             BASE_MONO + 1 * S, 0);
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, EV_CLIENT_READ, EV_LOCK_REL,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(lt.w, &cl, 0, cl.old_event);
    uint64_t late = lt.w->open_credit_late_ns_total;
    wr_close(&lt);
    struct readback rbl = { .want_event = EV_CLIENT_READ, .want_pid = PID_A };
    int nl = read_back(lt.dir, &rbl);
    CHECK(nl > 0, "late note: records read back (%d)", nl);
    if (nl > 0)
        CHECK(rb_sum_ev(&rbl) == 10 * S,
              "BYPASS h (late note): the whole 10 s is still accounted "
              "(%llu ms) — the un-creditable slice rides on the closing "
              "record instead of vanishing",
              (unsigned long long)(rb_sum_ev(&rbl) / MS));
    CHECK(late > 0,
          "BYPASS h: the late slice is COUNTED (open_credit_late_ns_total = "
          "%llu ms)", (unsigned long long)(late / MS));
}

int main(void)
{
    printf("=== test_straddler_accounting (#379, summary v%d) ===\n",
           PGWT_SUMMARY_VERSION);

    printf("\n-- 1-3. placement, conservation, occurrence quantities --\n");
    test_placement_and_conservation(1);

    printf("\n-- 6a. BYPASS: no producer notes (the reachability probe) --\n");
    test_placement_and_conservation(0);

    printf("\n-- 4. the tick and the close in either order --\n");
    test_tick_close_order();

    printf("\n-- 5. what the rule refuses to spread --\n");
    test_refusals();

    printf("\n-- 6b-h. the rest of the bypass suite --\n");
    test_bypass();

    printf("\n%d checks, %d failures\n", checks, failures);
    printf("=== test_straddler_accounting: %s ===\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
