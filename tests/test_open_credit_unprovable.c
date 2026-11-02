/* test_open_credit_unprovable.c — #709: uncredited must never double-tell
 * a credited interval after forget_open destroys the pid's watermark.
 *
 * §1 makes the production ordering reachable through the public writer API:
 * six seconds credited, then a lifecycle forget, then the gap's closing trace
 * record. The invariant compares uncredited with gap wall minus credit from
 * credit_open_second, a DIFFERENT code path from the close-time add. The
 * parent tree reports 6000 ms uncredited for 0 ms never-credited wall. The
 * new counter must MOVE, charge the record exactly once across the pair,
 * and measure the same evidence loss's excess CPU-class wall read off DISK.
 * §2 keeps the original #379 residual alive with no notes at all; it passes
 * on BOTH trees and kills a fix that routes all gaps to the new counter.
 * §3 bypasses: (a) forget with no credit, (b) a reused pid's record entirely
 * above the dropped watermark, (c) no forget and a matching note, (d) a
 * sub-second record, (e) a non-CPU record, (f) no slot for the pid, and (g)
 * missing/disabled output which the reader must REFUSE, never sum as zero.
 *
 * Server build, no BPF, no live PG. Every timestamp is a literal offset
 * from the pinned BASE_MONO; every credit is driven by an explicit flush,
 * so scheduling and the real clock cannot decide an assertion. */
#define _GNU_SOURCE
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define PID_B        1002u
#define BASE_MONO    (10000ULL * S)

static const uint32_t EV_LOCK_REL = (PG_WAIT_LOCK << 24) | 0;

/* ── fixture plumbing, copied only where used from open_cpu_accounting ── */

static const char *fresh_dir(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof(buf), "/tmp/pgwt_unprov_%s_%d", name, (int)getpid());
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", buf, buf);
    if (system(cmd) != 0)
        return NULL;
    return buf;
}

/* The longest fixture spans fourteen seconds. Overflow must REFUSE rather
 * than shorten the disk sum and let a partial read green the invariant. */
#define RB_MAX_ROWS 256
struct readback {
    uint64_t class_ns[RB_MAX_ROWS][PGWT_NUM_CLASSES];
    int n;
    int overflow;
};

static int rb_visit(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct readback *rb = vctx;
    if (rb->n >= RB_MAX_ROWS) {
        rb->overflow = 1;
        return 1;
    }
    memcpy(rb->class_ns[rb->n++], rec->class_ns, sizeof(rec->class_ns));
    return 0;
}

/* No records is -1, never a successful empty sum: a gate that sums zero
 * records agrees with everything. §3g makes this refusal itself go red if
 * missing or disabled output is ever accepted as evidence. */
static int read_back(const char *dir, struct readback *rb)
{
    rb->n = 0;
    rb->overflow = 0;
    int rc = pgwt_visit_summaries(dir, 0, 0, rb_visit, rb);
    if (rc <= 0 || rb->overflow || rb->n == 0)
        return -1;
    return rb->n;
}

static uint64_t rb_sum_class(const struct readback *rb, int cls)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->class_ns[i][cls];
    return t;
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
}

static void wr_free(struct wr *x)
{
    pgwt_summary_destroy(x->w);
    free(x->w);
    x->w = NULL;
}

/* One tick of a producer that keeps a pid's on-CPU note fresh, exactly as
 * src/map_reader.c's state_map scan does for every live backend: note what
 * is open WITH the CPU observation it already computed for the live view,
 * then let the writer seal whatever seconds have completed.
 *
 * `prefix` is the CUMULATIVE measured CPU over [start, now), clamped to that
 * prefix's wall, which is what the producer has. PGWT_CPU_NS_UNKNOWN = the
 * tier has no measurement. */
static void tick_cpu(struct wr *x, uint64_t now, uint32_t pid, uint64_t start,
                     uint32_t cat, uint32_t eff_we, uint64_t prefix)
{
    struct pgwt_summary_open_cpu obs = { .eff_we = eff_we,
                                         .cpu_prefix_ns = prefix };
    pgwt_summary_note_open_ex(x->w, pid, 0, start, cat, now, &obs);
    pgwt_summary_flush_completed(x->w, now);
}

/* Anchor the file with [0 s, 1 s) Lock. It contributes no CPU-class wall,
 * so the disk sum below measures only the gap, not an unrelated seed. */
static void seed_file(struct wr *x)
{
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x->w, &seed, 0, seed.old_event);
}

/* The shared probe: [1 s, 7 s) at 60% CPU. Ticks at s + 200 ms for
 * s = 2..7 credit one second into each of seconds 1..6, leaving the live
 * watermark at 7 s. No assertion derives credit from the closing add site. */
static void credit_gap(struct wr *x)
{
    seed_file(x);
    for (int s = 2; s <= 7; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(x, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 (now - (BASE_MONO + 1 * S)) * 3 / 5);
    }
}

static void close_gap(struct wr *x)
{
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 7 * S, 6 * S, 0, EV_LOCK_REL,
             0, 3600 * MS);
    pgwt_summary_push_event(x->w, &cl, 0, cl.old_event);
}

/* ── §1: the invariant, with the measured overlapping credit as control ─ */

static void test_invariant(void)
{
    struct wr x;
    if (wr_open(&x, "invariant") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    credit_gap(&x);
    pgwt_summary_forget_open(x.w, PID_A);
    close_gap(&x);
    const uint64_t gap_wall = 6 * S;
    uint64_t credited = x.w->open_cpu_credited_ns_total;
    uint64_t uncred = x.w->cpu_open_uncredited_ns_total;
    uint64_t unprov = x.w->cpu_open_credit_unprovable_ns_total;

    /* REACHABILITY: the overlap is made to happen, never asserted absent.
     * If six seconds were not credited, this leg is measuring nothing and
     * the uncredited == 0 control could green a dead credit path. */
    CHECK(credited == 6 * S,
          "REACHABILITY: the gap really WAS credited (%llu ms)",
          (unsigned long long)(credited / MS));
    uint64_t never_credited = gap_wall - credited;
    /* THE CONTROL: credit_open_second accumulates credited, a DIFFERENT
     * code path from the residual add. This fails when credited and
     * uncredited overlap: origin/master reads 6000 ms where the gap's
     * never-credited part is 0 ms. Deleting all credit cannot green it
     * because the reachability check above would then be red. */
    CHECK(uncred == never_credited,
          "INVARIANT: uncredited = %llu ms, never-credited = %llu ms",
          (unsigned long long)(uncred / MS),
          (unsigned long long)(never_credited / MS));
    /* The new counter must demonstrably MOVE on the evidence-loss input.
     * A counter which cannot move reads as evidence of health, which is why
     * 1d5fde74 deleted sampler_ticks_missed_total and #697 deleted
     * sampler_ticks_rated_total. A silent deletion of the residual is red. */
    CHECK(unprov == 6 * S,
          "UNPROVABLE MOVES: the forgotten gap charges %llu ms",
          (unsigned long long)(unprov / MS));
    /* Exactly ONE counter owns the record. Red input: adding the whole
     * duration to both counters instead of splitting the claim. */
    CHECK(uncred + unprov == 6 * S,
          "ONCE: uncredited + unprovable = %llu ms",
          (unsigned long long)((uncred + unprov) / MS));
    wr_close(&x);
    struct readback rb = { 0 };
    int n = read_back(x.dir, &rb);
    /* A lost file or decoder failure must fail before any disk sum: zero
     * records would otherwise make a conservation gate agree with nothing. */
    CHECK(n > 0, "invariant: records read back (%d)", n);
    if (n > 0) {
        uint64_t disk_cpu_wall = rb_sum_class(&rb, PGWT_CLASS_CPU);
        /* This counter measures exactly the wall the SAME evidence loss
         * double counts on disk: 12000 ms for a 6000 ms gap here. This
         * identity also holds on a tree that later fixes that over-count:
         * there already > 0, so both sides become 0. It pins the relation
         * without pinning the defect's current value. A counter unrelated
         * to the wall at risk, or a broken disk read, makes it go red. */
        CHECK(unprov == disk_cpu_wall - gap_wall,
              "DISK RISK: unprovable %llu ms = disk CPU %llu ms - gap 6000 ms",
              (unsigned long long)(unprov / MS),
              (unsigned long long)(disk_cpu_wall / MS));
    }
    wr_free(&x);
}

/* ── §2: the original #379 residual must remain observable ───────────── */

static void test_non_vacuity(void)
{
    struct wr x;
    if (wr_open(&x, "nonvacuity") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    seed_file(&x);
    close_gap(&x);                   /* NO notes at all for the gap */
    uint64_t credited = x.w->open_cpu_credited_ns_total;
    uint64_t never_credited = 6 * S - credited;
    /* With no notes, any credit is invented. This leg passes on BOTH trees
     * by design: it catches a fix that over-broadens the split and leaves
     * uncredited dead by routing every gap into the new counter. */
    CHECK(credited == 0, "NON-VACUITY: no notes means no credit (%llu ms)",
          (unsigned long long)(credited / MS));
    /* The literal wall minus the independent credit path must remain the
     * whole gap; a fixture that accidentally credits anything is red. */
    CHECK(never_credited == 6 * S,
          "NON-VACUITY: never-credited wall = %llu ms",
          (unsigned long long)(never_credited / MS));
    /* Deleting the #379 residual or moving every gap to unprovable fails
     * here even though it could make §1's uncredited == 0 look healthy. */
    CHECK(x.w->cpu_open_uncredited_ns_total == 6 * S,
          "NON-VACUITY: uncredited still MOVES (%llu ms)",
          (unsigned long long)(x.w->cpu_open_uncredited_ns_total / MS));
    /* No evidence ever existed to lose. Treating the absence of a note as
     * destroyed proof would launder genuine uncredited wall and go red. */
    CHECK(x.w->cpu_open_credit_unprovable_ns_total == 0,
          "NON-VACUITY: unprovable stays zero (%llu ms)",
          (unsigned long long)(x.w->cpu_open_credit_unprovable_ns_total / MS));
    wr_close(&x);
    wr_free(&x);
}

/* ── §3a-f: each way the split's detection could become unreachable ──── */

static void test_bypass(void)
{
    struct wr a;
    if (wr_open(&a, "emptydrop") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    seed_file(&a);
    struct pgwt_summary_open_cpu obs = { .eff_we = 0, .cpu_prefix_ns = 0 };
    pgwt_summary_note_open_ex(a.w, PID_A, 0, BASE_MONO + 1 * S, 0,
                              BASE_MONO + 1 * S, &obs);
    pgwt_summary_forget_open(a.w, PID_A);   /* watermark 0: no flush */
    close_gap(&a);
    /* a. THE EVIDENCE IS ABSENT RATHER THAN WRONG. Kills a bool
     * watermark_dropped flag: a forget that destroyed nothing must not
     * launder a genuinely never-credited gap out of uncredited. */
    CHECK(a.w->cpu_open_credit_unprovable_ns_total == 0 &&
          a.w->cpu_open_uncredited_ns_total == 6 * S,
          "BYPASS a: zero watermark lost no proof (unprovable %llu ms, "
          "uncredited %llu ms)",
          (unsigned long long)(a.w->cpu_open_credit_unprovable_ns_total / MS),
          (unsigned long long)(a.w->cpu_open_uncredited_ns_total / MS));
    wr_close(&a);
    wr_free(&a);

    struct wr b;
    if (wr_open(&b, "abovedrop") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    credit_gap(&b);
    pgwt_summary_forget_open(b.w, PID_A);
    struct pgwt_trace_event later =
        mkev(PID_A, BASE_MONO + 14 * S, 6 * S, 0, EV_LOCK_REL,
             0, 3600 * MS);
    pgwt_summary_push_event(b.w, &later, 0, later.old_event);
    /* b. A POST-DROP RECORD ENTIRELY ABOVE THE DROPPED WATERMARK. This is
     * the reused pid's un-noted [8 s, 14 s) gap, above the dropped 7 s.
     * Kills the same flag implementation and pins start <, rather than
     * merely whether a forget happened at any point in the pid's history. */
    CHECK(b.w->cpu_open_credit_unprovable_ns_total == 0 &&
          b.w->cpu_open_uncredited_ns_total == 6 * S,
          "BYPASS b: above the dropped mark (unprovable %llu ms, "
          "uncredited %llu ms)",
          (unsigned long long)(b.w->cpu_open_credit_unprovable_ns_total / MS),
          (unsigned long long)(b.w->cpu_open_uncredited_ns_total / MS));
    wr_close(&b);
    wr_free(&b);

    struct wr c;
    if (wr_open(&c, "normal") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    credit_gap(&c);
    close_gap(&c);                   /* matching start; NO forget */
    /* c. THE NORMAL PATH IS UNTOUCHED. already > 0 must keep BOTH counters
     * zero. Removing the already gate would count this fully credited close
     * as a residual even while the matching note still proves its credit. */
    CHECK(c.w->cpu_open_uncredited_ns_total == 0 &&
          c.w->cpu_open_credit_unprovable_ns_total == 0,
          "BYPASS c: matching note keeps both counters zero "
          "(uncredited %llu ms, unprovable %llu ms)",
          (unsigned long long)(c.w->cpu_open_uncredited_ns_total / MS),
          (unsigned long long)(c.w->cpu_open_credit_unprovable_ns_total / MS));
    wr_close(&c);
    struct readback rb = { 0 };
    int n = read_back(c.dir, &rb);
    /* A missing output must fail before summing; otherwise a broken writer
     * or reader could turn the normal-path control into an empty agreement. */
    CHECK(n > 0, "normal path: records read back (%d)", n);
    if (n > 0) {
        uint64_t disk_cpu_wall = rb_sum_class(&rb, PGWT_CLASS_CPU);
        /* Red input: any change that stops subtracting the interval's OWN
         * credit gives 12 s here. With proof intact disk wall is exactly
         * the gap's 6 s, so naming uncertainty cannot disturb subtraction. */
        CHECK(disk_cpu_wall == 6 * S,
              "BYPASS c: disk CPU wall is exactly the gap (%llu ms)",
              (unsigned long long)(disk_cpu_wall / MS));
    }
    wr_free(&c);

    struct wr d;
    if (wr_open(&d, "short") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    credit_gap(&d);
    pgwt_summary_forget_open(d.w, PID_A);
    struct pgwt_trace_event short_rec =
        mkev(PID_A, BASE_MONO + 7 * S, 500 * MS, 0, EV_LOCK_REL,
             0, 300 * MS);
    pgwt_summary_push_event(d.w, &short_rec, 0, short_rec.old_event);
    /* d. A SUB-SECOND GAP starts at 6.5 s, below the dropped 7 s mark.
     * Neither counter moves: kills an unconditional add that loses the
     * over-one-second test when introducing the evidence-loss branch. */
    CHECK(d.w->cpu_open_uncredited_ns_total == 0 &&
          d.w->cpu_open_credit_unprovable_ns_total == 0,
          "BYPASS d: sub-second gap moves neither counter "
          "(uncredited %llu ms, unprovable %llu ms)",
          (unsigned long long)(d.w->cpu_open_uncredited_ns_total / MS),
          (unsigned long long)(d.w->cpu_open_credit_unprovable_ns_total / MS));
    wr_close(&d);
    wr_free(&d);

    struct wr e;
    if (wr_open(&e, "lock") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    credit_gap(&e);
    pgwt_summary_forget_open(e.w, PID_A);
    struct pgwt_trace_event lock =
        mkev(PID_A, BASE_MONO + 7 * S, 6 * S, EV_LOCK_REL, 0,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(e.w, &lock, 0, lock.old_event);
    /* e. NOT CPU CLASS. The Lock span starts below the dropped watermark
     * and exceeds one second, but neither CPU counter may move. Kills a
     * fix that drops the class test while routing the whole duration. */
    CHECK(e.w->cpu_open_uncredited_ns_total == 0 &&
          e.w->cpu_open_credit_unprovable_ns_total == 0,
          "BYPASS e: Lock moves neither CPU counter "
          "(uncredited %llu ms, unprovable %llu ms)",
          (unsigned long long)(e.w->cpu_open_uncredited_ns_total / MS),
          (unsigned long long)(e.w->cpu_open_credit_unprovable_ns_total / MS));
    wr_close(&e);
    wr_free(&e);

    struct wr f;
    if (wr_open(&f, "noslot") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    credit_gap(&f);                  /* only PID_A has ever been noted */
    pgwt_summary_forget_open(f.w, PID_A);
    struct pgwt_trace_event other =
        mkev(PID_B, BASE_MONO + 7 * S, 6 * S, 0, EV_LOCK_REL,
             0, 3600 * MS);
    pgwt_summary_push_event(f.w, &other, 0, other.old_event);
    /* f. NO SLOT AT ALL. No evidence ever existed for PID_B to lose, so
     * the original #379 residual must not migrate. Kills o == NULL being
     * treated as unprovable, or another pid's lost proof being consulted. */
    CHECK(f.w->cpu_open_uncredited_ns_total == 6 * S &&
          f.w->cpu_open_credit_unprovable_ns_total == 0,
          "BYPASS f: never-noted PID_B stays uncredited "
          "(uncredited %llu ms, unprovable %llu ms)",
          (unsigned long long)(f.w->cpu_open_uncredited_ns_total / MS),
          (unsigned long long)(f.w->cpu_open_credit_unprovable_ns_total / MS));
    wr_close(&f);
    wr_free(&f);
}

/* ── §3g: refuse missing output rather than approve an empty sum ─────── */

static void test_bypass_reader(void)
{
    /* A directory known to be absent, not a shared fixed path that another
     * run could populate: accepting its empty result greens every disk sum
     * without ever observing a record. */
    const char *missing = fresh_dir("missing");
    if (!missing || rmdir(missing) != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    struct readback rb = { 0 };
    CHECK(read_back(missing, &rb) == -1,
          "BYPASS g: missing directory makes read_back REFUSE (-1)");

    struct wr x;
    if (wr_open(&x, "disabled") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    /* No public enable/disable setter exists. Removing the directory before
     * the first push exercises the public failure path that sets enabled
     * false; a private-field assignment would bypass that production path. */
    if (rmdir(x.dir) != 0) {
        printf("  FAIL: setup\n"); failures++; wr_free(&x); return;
    }
    seed_file(&x);
    close_gap(&x);
    pgwt_summary_flush_completed(x.w, BASE_MONO + 8 * S + 200 * MS);
    wr_close(&x);
    /* The refused first open must really disable the writer; otherwise the
     * empty read below could be testing an unrelated failure to write. */
    CHECK(!x.w->enabled,
          "BYPASS g: public first-open failure disables the writer");
    /* A disabled writer produces no records. Returning zero as success is
     * red here, so no accounting leg can approve over zero disk records. */
    CHECK(read_back(x.dir, &rb) == -1,
          "BYPASS g: disabled writer makes read_back REFUSE (-1)");
    wr_free(&x);
}

int main(void)
{
    printf("=== test_open_credit_unprovable (#709 / summary v%d) ===\n",
           PGWT_SUMMARY_VERSION);
    printf("-- #709 1: invariant and the credited-overlap control --\n");
    test_invariant();
    printf("-- #709 2: non-vacuity (uncredited must still move) --\n");
    test_non_vacuity();
    printf("-- #709 3a-f: bypass — evidence, normal path, duration, class, pid --\n");
    test_bypass();
    printf("-- #709 3g: bypass — the reader must refuse --\n");
    test_bypass_reader();

    printf("\nchecks=%d failures=%d\n", checks, failures);
    printf("=== test_open_credit_unprovable: %s ===\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
