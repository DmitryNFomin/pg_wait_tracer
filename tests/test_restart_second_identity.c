/* test_restart_second_identity.c — #284: does a daemon restart INSIDE a
 * second make the second-keyed reader report more time than elapsed?
 *
 * THE CLAIM UNDER TEST, which was n=0 (a reading of the code, no fixture).
 * A restart inside second S leaves S written twice: the outgoing daemon's
 * pgwt_summary_close force-flushes the in-progress second, the incoming one
 * writes the remainder of the same second as its own first record, and
 * pgwt_visit_summaries sums every record it finds. #284 called that an
 * over-count.
 *
 * ANSWER (this file is the measurement, not a fix): they tile for closed
 * records, so #284 is not the over-count it was filed as. The two
 * under-counts it did expose are #632, which is also where any fix goes.
 *
 * It is NOT a duplicate and neither record is a superset: the outgoing
 * partial holds the records that CLOSED in [sec_start, t_death], the
 * incoming one holds [t_attach, sec_end]. Summing disjoint partials is what
 * a half-open, second-keyed reader should do. The only question is whether
 * the two partials TILE or OVERLAP, and this file answers it by measurement:
 * one event schedule, replayed three ways —
 *
 *   (1) writer A, force-closed (pgwt_summary_close) at t_death inside S;
 *   (2) writer B, attaching at t_attach inside S, in the SAME directory, so
 *       the product's own recover_current_summary archives A's file exactly
 *       as the incoming daemon does;
 *   (3) ONE writer over the whole schedule — the ground truth.
 *
 * WHAT IT MEASURES (all numbers nanosecond-exact; no tolerance anywhere)
 * ─────────────────────────────────────────────────────────────────────────
 * §1 THE ANSWER, measured on this fixture: the restart second IS written
 *    twice (2 records, one per file, 5 records against the single writer's
 *    4) and it is NOT over-counted. The CLOSED-record terms tile EXACTLY:
 *    12 of 12 records counted once in both runs, and per second the closed
 *    IO time is identical ns for ns. #284's over-count does not exist.
 *    The OPEN-interval credit term does not tile — it is SHORT. For a 3 s
 *    Lock wait open across the restart second the split reads 2411 ms where
 *    the single writer reads 3011 ms: a DEFICIT of exactly 600 ms =
 *    t_attach - sec_start(S), all of it inside the restart second
 *    (403 ms + 600 ms == 1003 ms), with every other second identical. The
 *    error is in the OPPOSITE direction to the one filed, and it is pinned
 *    as an exact identity (split == gt - DARK), never as a tolerance.
 *    WHICH TERM, pinned by varying the instants rather than by arithmetic:
 *    the deficit tracks t_attach (attach at +100 ms -> 100 ms; at +600 ms ->
 *    600 ms) and does NOT move when t_death moves from +50 ms to +500 ms
 *    into S (600 ms both). So the slice [sec_start, t_death) that the
 *    outgoing daemon still had a FRESH NOTE for contributes nothing:
 *    pgwt_summary_close force-flushes the partial second without crediting
 *    what is open in it, and the incoming writer starts at its attach
 *    instant. That is the whole mechanism.
 * §2 THE BOUNDARY CONTROL. A handover exactly ON a second boundary.
 *    (i) nothing open across it: the identity holds EXACTLY (2204 ms both,
 *        every second identical) with credit live on BOTH sides (an interval
 *        that opens and closes inside A, another inside B) — so the
 *        two-file sum mechanism itself tiles, and §1's divergence is
 *        attributable to the credit term alone. §1 asserts its own restart
 *        is NOT boundary-aligned, so the suite cannot silently degenerate
 *        into this easy case.
 *    (ii) an interval open ACROSS the boundary: 2004 ms vs 3004 ms, short by
 *        exactly one second — the second the outgoing writer COMPLETED but
 *        never sealed, because PGWT_SUMMARY_FLUSH_LAG_NS means its last tick
 *        at T could only seal seconds ending before T - LAG. A second,
 *        distinct under-count; still never an over-count.
 * §3 TWO FILES WITH NO SHARED SECOND, separated by a gap: every second in
 *    exactly one record, all five pushed records readable once (5 ms total),
 *    and the seconds inside the gap ABSENT rather than fabricated.
 * §4 PERTURBATION (this gate must be able to go red). One nanosecond of
 *    closed-record duration and one nanosecond of the incoming writer's
 *    CREDITED RANGE each make §2's exact-identity predicate FALSE, with the
 *    delta measured as exactly 1 ns; and moving t_attach by one nanosecond
 *    moves §1's deficit by exactly one, so `deficit == dark` is a
 *    nanosecond gate rather than a coincidence.
 * §5 WHAT THE PRODUCER PASSES FOR AN INHERITED START, verified rather than
 *    assumed: src/map_reader.c:661 passes BPF's `sval.last_ts`, and after a
 *    restart src/backend.c's preseed_state_map seeds `last_ts = attach_ts`
 *    (src/backend.c:253, from be->attach_ts stamped with now_ns()). So the
 *    inherited start is CLAMPED to attach by the producer — the writer never
 *    sees the true pre-restart start. §5 measures what the unclamped
 *    alternative would do to this very fixture: pgwt_summary_note_open_ex
 *    stores start_ns unclamped and earliest_open_second floors it, so the
 *    incoming writer seals TWO seconds that were entirely before it existed
 *    and the sum EXCEEDS the truth: 4511 ms vs 3011 ms, +1500 ms. That is a
 *    pinned measurement of today's writer and the reason the clamp is
 *    load-bearing; the writer-side clamp that would remove the hazard
 *    (max(start_mono_ns, the writer's anchor) at note time) belongs to #632,
 *    and the instruction for replacing this leg when it lands sits at the
 *    assertion itself.
 * §6 BYPASS SUITE — every way this measurement could fail to SEE (false
 *    negatives, which is the failure mode here, not false positives):
 *    a. a read that saw nothing (0 == 0 agrees with anything): read_secs
 *       returns -1 for rc <= 0, and the ground-truth side is checked against
 *       LITERALS counted by hand off the schedule, never only against the
 *       other read;
 *    b. a writer that wrote nothing (disabled): the read must refuse;
 *    c. the restart-second shape ABSENT: §1 asserts second S really is in
 *       two records, one per file, and in exactly one in the single run;
 *    d. the thing being checked absent rather than wrong: with NO producer
 *       notes the identity holds trivially — asserted, together with the
 *       proof that that run is a DIFFERENT measurement (0 ns credited, 3 ms
 *       in the restart second instead of 1003 ms), so a writer that silently
 *       stopped crediting could not green §1;
 *    e. a PARTIAL set of files (the outgoing archive deleted): the
 *       record-count gate goes red (2 != 5) instead of the sum quietly
 *       agreeing with the survivor;
 *    f. an unparseable file: the read refuses;
 *    g. a missing directory: the read refuses;
 *    h. a closed record lost in the dark window: records_in_gap() refuses
 *       the probe, because such a record adds its duration to the deficit
 *       and is indistinguishable from missing credit (it happened while this
 *       file was being written — a 602 ms "deficit" that was 600 ms of
 *       credit plus two dropped 1 ms records).
 *
 * TIMING. Nothing here reads a clock to decide anything: every tick is a
 * nanosecond literal passed to pgwt_summary_flush_completed, and both
 * writers PIN the same synthetic clock anchor (pgwt_summary_pin_clock) so
 * one monotonic second maps to one wall second in both files — without that
 * the "same second written twice" would not even collide on
 * second_wall_ns. No sleeps, no retries, no tolerances.
 *
 * Server build (no BPF, no live PG), ~0.1 s.
 */
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

static int checks;
static int failures;

#define CHECK(cond, fmt, ...) do {                                        \
    checks++;                                                             \
    if (!(cond)) {                                                        \
        failures++;                                                       \
        printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__);         \
    } else {                                                              \
        printf("  ok:   " fmt "\n", ##__VA_ARGS__);                       \
    }                                                                     \
} while (0)

#define S  1000000000ULL
#define MS 1000000ULL

/* Second-aligned synthetic monotonic base; the wall anchor is pinned to the
 * same number in every writer (see the TIMING note above). */
#define BASE      (20000ULL * S)
#define GRID_STEP (100 * MS)
#define GRID_END  (BASE + 6 * S)

#define PID_SHORT 1001u
#define PID_LONG  1002u
#define SHORT_DUR (1 * MS)

static char g_base[300];

static const char *fresh_dir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base, name);
    char cmd[760];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", buf, buf);
    if (system(cmd) != 0)
        return NULL;
    return buf;
}

/* ── the schedule ─────────────────────────────────────────────────────── */

struct sched_event {            /* a CLOSED record, in ts order */
    uint64_t ts;
    uint32_t pid;
    uint32_t we;
    uint64_t dur;
};

struct sched_open {             /* an interval the producer notes while open */
    uint64_t start;             /* true start (BPF's last_ts) */
    uint64_t end;               /* ts of its closing record */
    uint32_t pid;
    uint32_t we;
};

struct sched {
    const struct sched_event *ev;
    int nev;
    const struct sched_open  *op;
    int nop;
};

/* Replay [from_t, to_t] of the schedule into one writer.
 *
 * `from_t` is this writer's ATTACH instant, which is what makes the two
 * producer models expressible: with `clamp_inherited` the note for an
 * interval that was already open at attach carries max(true_start, from_t)
 * — what src/backend.c's preseed_state_map actually seeds into BPF's
 * last_ts — and without it the true pre-restart start (§5).
 *
 * `note_bias_ns` and `dur_bias_ns` are the §4 perturbations: one nanosecond
 * added to EVERY note this writer makes (a shift of its credited range —
 * biasing only the first note would be undone by the next tick, which
 * pgwt_summary_note_open reads as a different interval), and to its first
 * closed record's duration.
 *
 * Order inside one grid step is fixed and identical in every run: the
 * records due are pushed, then the open notes are refreshed, then the tick
 * runs. No decision is taken from a clock. */
static void replay(struct pgwt_summary_writer *w, const struct sched *s,
                   uint64_t from_t, uint64_t to_t, int clamp_inherited,
                   uint64_t note_bias_ns, uint64_t dur_bias_ns)
{
    int ei = 0;
    while (ei < s->nev && s->ev[ei].ts < from_t)
        ei++;
    int biased_ev = 0;

    uint64_t now = BASE + GRID_STEP;
    while (now < from_t)
        now += GRID_STEP;
    for (; now <= to_t; now += GRID_STEP) {
        while (ei < s->nev && s->ev[ei].ts <= now) {
            if (s->ev[ei].ts > to_t)
                break;
            const struct sched_event *e = &s->ev[ei];
            struct pgwt_trace_event rec;
            memset(&rec, 0, sizeof(rec));
            rec.pid = e->pid;
            rec.timestamp_ns = e->ts;
            rec.duration_ns = e->dur;
            rec.old_event = e->we;
            rec.new_event = 0;
            rec.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            if (dur_bias_ns && !biased_ev) {
                rec.duration_ns += dur_bias_ns;
                biased_ev = 1;
            }
            pgwt_summary_push_event(w, &rec, 0, rec.old_event);
            ei++;
        }
        for (int i = 0; i < s->nop; i++) {
            uint64_t start = s->op[i].start;
            if (clamp_inherited && start < from_t)
                start = from_t;
            if (start > now || now >= s->op[i].end)
                continue;
            start += note_bias_ns;
            pgwt_summary_note_open(w, s->op[i].pid, s->op[i].we, start, 0,
                                   now);
        }
        pgwt_summary_flush_completed(w, now);
    }
}

static struct pgwt_summary_writer *new_writer(const char *dir)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w)
        exit(1);
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0)
        exit(1);
    pgwt_summary_pin_clock(w, BASE, BASE);
    return w;
}

static void close_writer(struct pgwt_summary_writer *w)
{
    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
}

/* ── read-back, per second ────────────────────────────────────────────── */

#define MAX_SEC 32

struct secs {
    int      n;
    uint64_t wall[MAX_SEC];
    uint64_t total_ns[MAX_SEC];      /* sigma class_ns over every record */
    uint64_t io_ns[MAX_SEC];         /* the closed short records' class   */
    uint64_t idle_ns[MAX_SEC];
    uint64_t events[MAX_SEC];        /* sigma events[].count              */
    int      records[MAX_SEC];       /* how many RECORDS carry this second */
    int      nrecords;
    int      overflow;
};

static int secs_visit(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct secs *t = vctx;
    t->nrecords++;
    int k = -1;
    for (int i = 0; i < t->n; i++)
        if (t->wall[i] == rec->second_wall_ns)
            k = i;
    if (k < 0) {
        if (t->n >= MAX_SEC) {
            t->overflow = 1;
            return 1;
        }
        k = t->n++;
        t->wall[k] = rec->second_wall_ns;
    }
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        t->total_ns[k] += rec->class_ns[i];
    t->io_ns[k] += rec->class_ns[PGWT_CLASS_IO];
    t->idle_ns[k] += rec->idle_ns;
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++)
        if (rec->events[i].event_id != 0)
            t->events[k] += rec->events[i].count;
    t->records[k]++;
    return 0;
}

/* Returns the number of RECORDS visited, or -1. Never 0-as-success: a read
 * that saw nothing must fail its caller, never read as agreement (§6a). */
static int read_secs(const char *dir, struct secs *t)
{
    memset(t, 0, sizeof(*t));
    int rc = pgwt_visit_summaries(dir, 0, 0, secs_visit, t);
    if (rc <= 0 || t->overflow)
        return -1;
    return rc;
}

static int sec_index(const struct secs *t, uint64_t wall)
{
    for (int i = 0; i < t->n; i++)
        if (t->wall[i] == wall)
            return i;
    return -1;
}
static uint64_t sec_total(const struct secs *t, uint64_t wall)
{
    int i = sec_index(t, wall);
    return i < 0 ? 0 : t->total_ns[i];
}
static uint64_t sec_io(const struct secs *t, uint64_t wall)
{
    int i = sec_index(t, wall);
    return i < 0 ? 0 : t->io_ns[i];
}
static uint64_t sec_events(const struct secs *t, uint64_t wall)
{
    int i = sec_index(t, wall);
    return i < 0 ? 0 : t->events[i];
}
static int sec_records(const struct secs *t, uint64_t wall)
{
    int i = sec_index(t, wall);
    return i < 0 ? 0 : t->records[i];
}
static uint64_t all_total(const struct secs *t)
{
    uint64_t v = 0;
    for (int i = 0; i < t->n; i++)
        v += t->total_ns[i];
    return v;
}
static uint64_t all_events(const struct secs *t)
{
    uint64_t v = 0;
    for (int i = 0; i < t->n; i++)
        v += t->events[i];
    return v;
}
static uint64_t all_idle(const struct secs *t)
{
    uint64_t v = 0;
    for (int i = 0; i < t->n; i++)
        v += t->idle_ns[i];
    return v;
}

/* ── the two runs ─────────────────────────────────────────────────────── */

struct run_out {
    struct secs t;
    int         nrec;              /* read_secs' return, -1 on refusal */
    uint64_t    credited_ns;       /* the ground-truth writer's open credit */
};

/* (3) ONE writer over the whole schedule. */
static void run_single(const char *tag, const struct sched *s,
                       struct run_out *out, int clamp, uint64_t note_bias,
                       uint64_t dur_bias)
{
    const char *dir = fresh_dir(tag);
    if (!dir) { failures++; out->nrec = -1; return; }
    struct pgwt_summary_writer *w = new_writer(dir);
    replay(w, s, BASE, GRID_END, clamp, note_bias, dur_bias);
    out->credited_ns = w->open_credited_ns_total;
    close_writer(w);
    out->nrec = read_secs(dir, &out->t);
}

/* (1)+(2) A dies at t_death, B attaches at t_attach, SAME directory. */
static void run_split(const char *tag, const struct sched *s,
                      uint64_t t_death, uint64_t t_attach,
                      struct run_out *out, int clamp, uint64_t note_bias,
                      uint64_t dur_bias)
{
    const char *dir = fresh_dir(tag);
    if (!dir) { failures++; out->nrec = -1; return; }
    struct pgwt_summary_writer *a = new_writer(dir);
    replay(a, s, BASE, t_death, 1, 0, 0);
    close_writer(a);                      /* the outgoing force-flush */

    /* B opens current.summary with "wbx" in the same directory, so
     * recover_current_summary archives A's file — the product's own restart
     * path, not a hand-made copy. */
    struct pgwt_summary_writer *b = new_writer(dir);
    replay(b, s, t_attach, GRID_END, clamp, note_bias, dur_bias);
    out->credited_ns = b->open_credited_ns_total;
    close_writer(b);

    out->nrec = read_secs(dir, &out->t);
}

/* ── §1: the restart second ───────────────────────────────────────────── */

/* The schedule: eleven 1 ms IO records on PID_SHORT spread over four
 * seconds, and ONE 3 s Lock wait on PID_LONG open from BASE+0.5 s across
 * the restart second and closing at BASE+3.5 s. Nothing is scheduled inside
 * [t_death, t_attach] — the daemon is down there, and a record in that gap
 * would make the two runs' CLOSED sets differ for a reason that has nothing
 * to do with the question. records_in_gap() proves it rather than assuming
 * it, and it measures the gap from the outgoing writer's LAST TICK (the grid
 * is 100 ms), which is why second S's pre-restart record sits exactly on a
 * grid step. */
static const struct sched_event ev_main[] = {
    { BASE +  100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE +  200 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE +  300 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 1100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 1200 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 1300 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 2000 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 2700 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 2800 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 3100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 3200 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 3500 * MS, PID_LONG,  (PG_WAIT_LOCK << 24) | 0, 3 * S     },
};
static const struct sched_open op_main[] = {
    { BASE + 500 * MS, BASE + 3500 * MS, PID_LONG, (PG_WAIT_LOCK << 24) | 0 },
};
static const struct sched sched_main = {
    ev_main, (int)(sizeof(ev_main) / sizeof(ev_main[0])),
    op_main, (int)(sizeof(op_main) / sizeof(op_main[0])),
};

#define SEC(n)  (BASE + (uint64_t)(n) * S)
#define RESTART_SEC SEC(2)

/* How many CLOSED records fall inside [t_death, t_attach] -- the window in
 * which no daemon is running. It must be ZERO for a deficit measurement to
 * mean anything: a record lost in the gap adds its duration to the deficit
 * and is indistinguishable from missing open-interval credit. MEASURED
 * during development: a probe whose gap swallowed two 1 ms records reported
 * a 602 ms deficit where the credit term was 600 ms. */
static int records_in_gap(const struct sched *s, uint64_t t_death,
                          uint64_t t_attach)
{
    /* The OUTGOING writer's real cutoff is its last tick at or before
     * t_death, not t_death itself: replay() pushes a record on the first
     * grid step at or after its timestamp, so a record between that tick and
     * t_attach belongs to neither writer. Using t_death here instead would
     * make this guard miss exactly the case it exists for -- it did, during
     * development, and the two records it missed showed up as 2 ms of
     * "missing credit". */
    uint64_t cutoff = BASE + ((t_death - BASE) / GRID_STEP) * GRID_STEP;
    int n = 0;
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].ts > cutoff && s->ev[i].ts < t_attach)
            n++;
    return n;
}

/* Deficit of the split run against the single-writer ground truth, in ns.
 * Positive means the split reports LESS than the truth. */
static int64_t deficit(uint64_t t_death, uint64_t t_attach)
{
    struct run_out gt, sp;
    CHECK(records_in_gap(&sched_main, t_death, t_attach) == 0,
          "1 probe (death +%llu ms, attach +%llu ms): no closed record is "
          "lost in the dark window, so the deficit is the credit term alone",
          (unsigned long long)((t_death - RESTART_SEC) / MS),
          (unsigned long long)((t_attach - RESTART_SEC) / MS));
    run_single("vary_gt", &sched_main, &gt, 1, 0, 0);
    run_split("vary_sp", &sched_main, t_death, t_attach, &sp, 1, 0, 0);
    if (gt.nrec <= 0 || sp.nrec <= 0) {
        failures++;
        checks++;
        printf("  FAIL: deficit probe could not read back (gt=%d split=%d)\n",
               gt.nrec, sp.nrec);
        return 0;
    }
    CHECK(all_events(&sp.t) == all_events(&gt.t),
          "1 probe: both runs carry the same %llu closed records",
          (unsigned long long)all_events(&gt.t));
    return (int64_t)all_total(&gt.t) - (int64_t)all_total(&sp.t);
}

static void test_restart_second(void)
{
    const uint64_t t_death  = BASE + 2500 * MS;
    const uint64_t t_attach = BASE + 2600 * MS;
    const uint64_t dark     = t_attach - RESTART_SEC;     /* 600 ms */

    printf("--- 1. the restart second: do the two partials tile? ---\n");

    /* The fixture's own preconditions, asserted not assumed. */
    CHECK(t_death % S != 0 && t_attach % S != 0,
          "1 the restart is INSIDE a second (death +%llu ms, attach +%llu ms "
          "into it), not the easy boundary case of section 2",
          (unsigned long long)((t_death % S) / MS),
          (unsigned long long)((t_attach % S) / MS));
    CHECK(records_in_gap(&sched_main, t_death, t_attach) == 0,
          "1 no closed record falls inside the dark window, so the two runs "
          "see the SAME closed set and only the credit term can differ");

    struct run_out gt, sp;
    run_single("gt", &sched_main, &gt, 1, 0, 0);
    run_split("split", &sched_main, t_death, t_attach, &sp, 1, 0, 0);

    CHECK(gt.nrec == 4, "1 ground truth: 4 records, one per second (%d)",
          gt.nrec);
    CHECK(sp.nrec == 5,
          "1 split: 5 records across TWO files — the restart second is "
          "written twice (%d)", sp.nrec);
    if (gt.nrec <= 0 || sp.nrec <= 0)
        return;

    /* §6a/§6c NON-VACUITY: ground truth checked against a LITERAL counted by
     * hand off the schedule, and the shape under test proved present. */
    CHECK(sec_total(&gt.t, RESTART_SEC) == 1 * S + 3 * SHORT_DUR,
          "1 ground truth for the restart second = %llu ms "
          "(1000 ms of open Lock + three 1 ms IO records)",
          (unsigned long long)(sec_total(&gt.t, RESTART_SEC) / MS));
    CHECK(all_total(&gt.t) == 3 * S + 11 * SHORT_DUR,
          "1 ground truth total = %llu ms (3000 ms Lock + eleven 1 ms IO)",
          (unsigned long long)(all_total(&gt.t) / MS));
    CHECK(gt.credited_ns > 0,
          "1 the open-credit term is LIVE in the ground truth (%llu ms "
          "credited) — with it absent this whole section would be vacuous",
          (unsigned long long)(gt.credited_ns / MS));
    CHECK(sec_records(&sp.t, RESTART_SEC) == 2,
          "1 the restart second IS present in two records, one per file (%d)",
          sec_records(&sp.t, RESTART_SEC));
    CHECK(sec_records(&gt.t, RESTART_SEC) == 1,
          "1 ... and in exactly one record in the single-writer run (%d)",
          sec_records(&gt.t, RESTART_SEC));
    CHECK(all_idle(&gt.t) == 0 && all_idle(&sp.t) == 0,
          "1 no time is filed as idle in either run (%llu / %llu ns), so "
          "class_ns is the whole of it",
          (unsigned long long)all_idle(&gt.t),
          (unsigned long long)all_idle(&sp.t));

    /* THE ANSWER, part 1: the CLOSED records tile EXACTLY, even though the
     * second is written twice. This is #284's claim, and it is false. */
    CHECK(all_events(&sp.t) == 12 && all_events(&gt.t) == 12,
          "1 TILE: every one of the 12 closed records is counted exactly "
          "once in both runs (split %llu, single %llu)",
          (unsigned long long)all_events(&sp.t),
          (unsigned long long)all_events(&gt.t));
    CHECK(sec_events(&sp.t, RESTART_SEC) == sec_events(&gt.t, RESTART_SEC) &&
          sec_events(&gt.t, RESTART_SEC) == 3,
          "1 TILE: the twice-written second carries 3 closed records in both "
          "runs (split %llu, single %llu)",
          (unsigned long long)sec_events(&sp.t, RESTART_SEC),
          (unsigned long long)sec_events(&gt.t, RESTART_SEC));
    for (int i = 0; i < gt.t.n; i++)
        CHECK(sec_io(&sp.t, gt.t.wall[i]) == gt.t.io_ns[i],
              "1 TILE: second %d's closed IO time is identical "
              "(%llu ns both)", i,
              (unsigned long long)gt.t.io_ns[i]);

    /* THE ANSWER, part 2: the OPEN-interval credit does not tile — it is
     * short by exactly the part of the restart second that preceded the
     * incoming daemon's attach. A DEFICIT, not the filed over-count. */
    CHECK(all_total(&sp.t) < all_total(&gt.t),
          "1 the split reports LESS than the truth (%llu ms vs %llu ms) — "
          "#284's over-count does not occur",
          (unsigned long long)(all_total(&sp.t) / MS),
          (unsigned long long)(all_total(&gt.t) / MS));
    CHECK(all_total(&gt.t) - all_total(&sp.t) == dark,
          "1 DEFICIT = %llu ns, exactly t_attach - sec_start(S) = %llu ns",
          (unsigned long long)(all_total(&gt.t) - all_total(&sp.t)),
          (unsigned long long)dark);
    CHECK(sec_total(&sp.t, RESTART_SEC) + dark ==
          sec_total(&gt.t, RESTART_SEC),
          "1 and the whole deficit is in the restart second: %llu ns + "
          "%llu ns == %llu ns",
          (unsigned long long)sec_total(&sp.t, RESTART_SEC),
          (unsigned long long)dark,
          (unsigned long long)sec_total(&gt.t, RESTART_SEC));
    for (int i = 0; i < gt.t.n; i++)
        if (gt.t.wall[i] != RESTART_SEC)
            CHECK(sec_total(&sp.t, gt.t.wall[i]) == gt.t.total_ns[i],
                  "1 second %d is IDENTICAL across the restart (%llu ns)", i,
                  (unsigned long long)gt.t.total_ns[i]);

    /* WHICH TERM, measured by varying the two instants rather than by
     * arithmetic on the literals above.
     *   - the deficit tracks t_attach: two attach instants, two deficits;
     *   - it does NOT depend on how long the outgoing writer survived into
     *     S: two t_death values, one deficit. So the [sec_start, t_death)
     *     slice the outgoing writer still had a fresh note for contributes
     *     nothing — pgwt_summary_close force-flushes the partial second
     *     without crediting what is open in it. */
    int64_t d_attach_early = deficit(BASE + 2050 * MS, BASE + 2100 * MS);
    int64_t d_attach_late  = deficit(BASE + 2050 * MS, BASE + 2600 * MS);
    int64_t d_death_late   = deficit(BASE + 2500 * MS, BASE + 2600 * MS);
    CHECK(d_attach_early == 100 * (int64_t)MS,
          "1 attach at +100 ms into S: deficit 100 ms (%lld ns)",
          (long long)d_attach_early);
    CHECK(d_attach_late == 600 * (int64_t)MS,
          "1 attach at +600 ms into S: deficit 600 ms (%lld ns) — it TRACKS "
          "the attach instant", (long long)d_attach_late);
    CHECK(d_death_late == d_attach_late,
          "1 moving t_death from +50 ms to +500 ms into S leaves the deficit "
          "unchanged (%lld vs %lld ns): the outgoing writer's observation of "
          "the partial second contributes NOTHING",
          (long long)d_death_late, (long long)d_attach_late);

    /* §6d: the thing being checked ABSENT rather than wrong. With no
     * producer notes there is no credit term, the identity holds trivially,
     * and that must not be mistaken for §1 passing. */
    struct sched no_notes = sched_main;
    no_notes.nop = 0;
    struct run_out gt0, sp0;
    run_single("gt_nonotes", &no_notes, &gt0, 1, 0, 0);
    run_split("sp_nonotes", &no_notes, t_death, t_attach, &sp0, 1, 0, 0);
    CHECK(gt0.nrec > 0 && sp0.nrec > 0 &&
          all_total(&sp0.t) == all_total(&gt0.t),
          "1 ABSENT CASE: with no notes at all the identity holds exactly "
          "(%llu ns both) — closed records alone always tile",
          (unsigned long long)all_total(&gt0.t));
    CHECK(gt0.credited_ns == 0 &&
          sec_total(&gt0.t, RESTART_SEC) != sec_total(&gt.t, RESTART_SEC),
          "1 ... and that run is NOT the one section 1 measures: 0 ns "
          "credited and the restart second holds %llu ns instead of %llu ns "
          "(v7 puts the whole wait in its closing second, which is why the "
          "TOTAL is no use as a non-vacuity check here), so a writer that "
          "silently stopped crediting could not green this section",
          (unsigned long long)sec_total(&gt0.t, RESTART_SEC),
          (unsigned long long)sec_total(&gt.t, RESTART_SEC));
}

/* ── §2: the boundary control ─────────────────────────────────────────── */

/* (i) Nothing open ACROSS the handover, but the credit path live on BOTH
 *     sides: a 1.1 s Lock wait that opens and closes inside A's half, and
 *     another inside B's half. */
static const struct sched_event ev_bnd[] = {
    { BASE +  100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 1100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 1400 * MS, PID_LONG,  (PG_WAIT_LOCK << 24) | 0, 1100 * MS },
    { BASE + 2100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 3100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    { BASE + 3400 * MS, PID_LONG,  (PG_WAIT_LOCK << 24) | 0, 1100 * MS },
};
static const struct sched_open op_bnd[] = {
    { BASE +  300 * MS, BASE + 1400 * MS, PID_LONG, (PG_WAIT_LOCK << 24) | 0 },
    { BASE + 2300 * MS, BASE + 3400 * MS, PID_LONG, (PG_WAIT_LOCK << 24) | 0 },
};
static const struct sched sched_bnd = {
    ev_bnd, (int)(sizeof(ev_bnd) / sizeof(ev_bnd[0])),
    op_bnd, (int)(sizeof(op_bnd) / sizeof(op_bnd[0])),
};

static void test_boundary_handover(void)
{
    printf("--- 2. handover exactly ON a second boundary ---\n");
    const uint64_t T = SEC(2);

    struct run_out gt, sp;
    run_single("bnd_gt", &sched_bnd, &gt, 1, 0, 0);
    run_split("bnd_sp", &sched_bnd, T, T, &sp, 1, 0, 0);
    CHECK(gt.nrec > 0 && sp.nrec > 0,
          "2 both runs read back (single %d records, split %d)", gt.nrec,
          sp.nrec);
    if (gt.nrec <= 0 || sp.nrec <= 0)
        return;
    CHECK(gt.credited_ns > 0,
          "2 the credit path is LIVE on both sides (%llu ms credited in the "
          "single-writer run)", (unsigned long long)(gt.credited_ns / MS));
    CHECK(all_total(&gt.t) == 2 * 1100 * MS + 4 * SHORT_DUR,
          "2 ground truth total = %llu ns (two 1100 ms waits + four 1 ms "
          "records), checked against the schedule, not against the split",
          (unsigned long long)all_total(&gt.t));

    int shared = 0;
    for (int i = 0; i < sp.t.n; i++)
        if (sp.t.records[i] > 1)
            shared++;
    CHECK(shared == 0,
          "2 no second is written twice: the handover is on the boundary "
          "(%d seconds in >1 record)", shared);
    CHECK(all_total(&sp.t) == all_total(&gt.t),
          "2 IDENTITY HOLDS EXACTLY: %llu ns both, to the nanosecond",
          (unsigned long long)all_total(&sp.t));
    CHECK(sp.t.n == gt.t.n, "2 the same seconds are present (%d vs %d)",
          sp.t.n, gt.t.n);
    for (int i = 0; i < gt.t.n; i++)
        CHECK(sec_total(&sp.t, gt.t.wall[i]) == gt.t.total_ns[i],
              "2 second %d identical (%llu ns)", i,
              (unsigned long long)gt.t.total_ns[i]);

    /* (ii) The same boundary handover, with an interval open ACROSS it. The
     * identity does NOT hold, and the missing term is a different one: the
     * second the outgoing writer COMPLETED but never sealed, because
     * PGWT_SUMMARY_FLUSH_LAG_NS means its last tick at T could only seal
     * seconds that ended before T - LAG. Still an under-count. */
    static const struct sched_event ev_x[] = {
        { BASE +  100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE + 1100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE + 2100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE + 3100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE + 3500 * MS, PID_LONG,  (PG_WAIT_LOCK << 24) | 0, 3 * S     },
    };
    static const struct sched_open op_x[] = {
        { BASE + 500 * MS, BASE + 3500 * MS, PID_LONG,
          (PG_WAIT_LOCK << 24) | 0 },
    };
    const struct sched sx = { ev_x, 5, op_x, 1 };

    struct run_out gx, sx_out;
    run_single("bndx_gt", &sx, &gx, 1, 0, 0);
    run_split("bndx_sp", &sx, T, T, &sx_out, 1, 0, 0);
    CHECK(gx.nrec > 0 && sx_out.nrec > 0,
          "2ii both runs read back (%d / %d records)", gx.nrec, sx_out.nrec);
    if (gx.nrec <= 0 || sx_out.nrec <= 0)
        return;
    CHECK(all_total(&sx_out.t) < all_total(&gx.t),
          "2ii with an interval open ACROSS the boundary the split is still "
          "SHORT, never long (%llu ns vs %llu ns)",
          (unsigned long long)all_total(&sx_out.t),
          (unsigned long long)all_total(&gx.t));
    CHECK(all_total(&gx.t) - all_total(&sx_out.t) == 1 * S,
          "2ii and it is short by exactly one second (%llu ns): the second "
          "the outgoing writer completed but could not seal within "
          "PGWT_SUMMARY_FLUSH_LAG_NS of its last tick",
          (unsigned long long)(all_total(&gx.t) - all_total(&sx_out.t)));
}

/* ── §3: two files, no shared second, with a gap between them ─────────── */

static void test_disjoint_files(void)
{
    printf("--- 3. two files with no shared second ---\n");
    static const struct sched_event ev_d[] = {
        { BASE +  100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE +  200 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE + 1100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        /* seconds 2 and 3: the daemon is DOWN, nothing is recorded */
        { BASE + 4100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
        { BASE + 5100 * MS, PID_SHORT, (PG_WAIT_IO << 24) | 1, SHORT_DUR },
    };
    const struct sched sd = { ev_d, 5, NULL, 0 };

    struct run_out sp;
    run_split("disjoint", &sd, BASE + 1500 * MS, BASE + 4000 * MS, &sp, 1, 0,
              0);
    CHECK(sp.nrec == 4,
          "3 four records across the two files (%d)", sp.nrec);
    if (sp.nrec <= 0)
        return;
    int shared = 0;
    for (int i = 0; i < sp.t.n; i++)
        if (sp.t.records[i] > 1)
            shared++;
    CHECK(shared == 0, "3 every second appears in exactly ONE record (%d "
          "shared)", shared);
    CHECK(all_events(&sp.t) == 5,
          "3 all five records are readable, each once (%llu)",
          (unsigned long long)all_events(&sp.t));
    CHECK(all_total(&sp.t) == 5 * SHORT_DUR,
          "3 nothing dropped and nothing invented: %llu ns == 5 x 1 ms",
          (unsigned long long)all_total(&sp.t));
    CHECK(sec_index(&sp.t, SEC(2)) < 0 && sec_index(&sp.t, SEC(3)) < 0,
          "3 the seconds in the gap are ABSENT, not fabricated as zero-filled "
          "records");
}

/* ── §4: the perturbation — this gate must be able to go red ──────────── */

static void test_perturbation(void)
{
    printf("--- 4. perturbation: one nanosecond must break it ---\n");
    const uint64_t T = SEC(2);

    struct run_out gt, sp_dur, sp_note;
    run_single("pert_gt", &sched_bnd, &gt, 1, 0, 0);
    /* (a) one nanosecond of CLOSED-record duration in writer B. */
    run_split("pert_dur", &sched_bnd, T, T, &sp_dur, 1, 0, 1);
    /* (b) one nanosecond on writer B's CREDITED RANGE (its note start). */
    run_split("pert_note", &sched_bnd, T, T, &sp_note, 1, 1, 0);
    CHECK(gt.nrec > 0 && sp_dur.nrec > 0 && sp_note.nrec > 0,
          "4 all three runs read back (%d / %d / %d)", gt.nrec, sp_dur.nrec,
          sp_note.nrec);
    if (gt.nrec <= 0 || sp_dur.nrec <= 0 || sp_note.nrec <= 0)
        return;

    CHECK(!(all_total(&sp_dur.t) == all_total(&gt.t)),
          "4a the section-2 identity predicate is FALSE when one closed "
          "record is 1 ns longer (%llu vs %llu ns)",
          (unsigned long long)all_total(&sp_dur.t),
          (unsigned long long)all_total(&gt.t));
    CHECK(all_total(&sp_dur.t) - all_total(&gt.t) == 1,
          "4a ... and the delta is exactly 1 ns (%llu)",
          (unsigned long long)(all_total(&sp_dur.t) - all_total(&gt.t)));

    CHECK(!(all_total(&sp_note.t) == all_total(&gt.t)),
          "4b the same predicate is FALSE when the incoming writer's "
          "credited range starts 1 ns later (%llu vs %llu ns)",
          (unsigned long long)all_total(&sp_note.t),
          (unsigned long long)all_total(&gt.t));
    CHECK(all_total(&gt.t) - all_total(&sp_note.t) == 1,
          "4b ... and the delta is exactly 1 ns (%llu)",
          (unsigned long long)(all_total(&gt.t) - all_total(&sp_note.t)));

    /* (c) section 1's DEFICIT predicate, perturbed the same way: moving the
     * attach instant by one nanosecond must move the deficit by one. */
    int64_t d0 = deficit(BASE + 2500 * MS, BASE + 2600 * MS);
    int64_t d1 = deficit(BASE + 2500 * MS, BASE + 2600 * MS + 1);
    CHECK(d1 - d0 == 1,
          "4c the section-1 deficit moves by exactly 1 ns when the attach "
          "instant does (%lld -> %lld ns), so `deficit == dark` is a "
          "nanosecond gate and not a coincidence",
          (long long)d0, (long long)d1);
}

/* ── §5: what an UNCLAMPED inherited start would do ───────────────────── */

static void test_unclamped_inherited_start(void)
{
    printf("--- 5. the inherited start: clamped (real) vs unclamped ---\n");
    const uint64_t t_death  = BASE + 2500 * MS;
    const uint64_t t_attach = BASE + 2600 * MS;

    struct run_out gt, cl, un;
    run_single("inh_gt", &sched_main, &gt, 1, 0, 0);
    run_split("inh_cl", &sched_main, t_death, t_attach, &cl, 1, 0, 0);
    /* clamp = 0: the incoming writer is handed the interval's TRUE
     * pre-restart start, which is what pgwt_summary_note_open_ex stores
     * unclamped and earliest_open_second then floors to a second. */
    run_split("inh_un", &sched_main, t_death, t_attach, &un, 0, 0, 0);
    CHECK(gt.nrec > 0 && cl.nrec > 0 && un.nrec > 0,
          "5 all three runs read back (%d / %d / %d)", gt.nrec, cl.nrec,
          un.nrec);
    if (gt.nrec <= 0 || cl.nrec <= 0 || un.nrec <= 0)
        return;

    CHECK(all_total(&cl.t) < all_total(&gt.t),
          "5 CLAMPED (what src/backend.c's preseed_state_map seeds into "
          "BPF's last_ts, which src/map_reader.c passes verbatim): the sum "
          "stays BELOW the truth, %llu ns vs %llu ns",
          (unsigned long long)all_total(&cl.t),
          (unsigned long long)all_total(&gt.t));

    /* PINNED MEASUREMENT of today's writer, not a requirement: it is why
     * the producer's clamp is load-bearing, and it is a regression guard on
     * preseed_state_map seeding last_ts = attach_ts. It deliberately does
     * NOT assert the clamped behaviour a writer-side fix would give, because
     * nothing implements that yet.
     * INSTRUCTION FOR WHOEVER DOES #632: the writer-side anchor clamp
     * (max(start_mono_ns, the writer's own anchor) in
     * pgwt_summary_note_open_ex) makes this leg go red. When it lands,
     * replace this CHECK and the `un != cl` one below with
     * `all_total(&un.t) == all_total(&cl.t)` — an unclamped note must then
     * be indistinguishable from a clamped one — and keep the pre_anchor
     * leg, inverted to `pre_anchor == 0`. Do not simply delete them: they
     * are the only coverage of the inherited-start hazard. */
    CHECK(all_total(&un.t) > all_total(&gt.t),
          "5 UNCLAMPED: the incoming writer seals seconds BEFORE IT EXISTED "
          "and the sum EXCEEDS the truth by %lld ns (%llu vs %llu) — a "
          "larger defect than the one #284 filed, and the reason the "
          "producer's attach clamp is load-bearing",
          (long long)((int64_t)all_total(&un.t) - (int64_t)all_total(&gt.t)),
          (unsigned long long)all_total(&un.t),
          (unsigned long long)all_total(&gt.t));
    int pre_anchor = 0;
    for (int i = 0; i < un.t.n; i++)
        if (un.t.wall[i] < RESTART_SEC && un.t.records[i] > 1)
            pre_anchor++;
    CHECK(pre_anchor > 0,
          "5 UNCLAMPED: %d second(s) ENTIRELY before the restart second are "
          "written twice — the incoming writer credits seconds the outgoing "
          "one already closed", pre_anchor);
    CHECK(all_total(&un.t) != all_total(&cl.t),
          "5 the two producer models are distinguishable here (%llu vs %llu "
          "ns), so section 1 is measuring the clamped one on purpose",
          (unsigned long long)all_total(&un.t),
          (unsigned long long)all_total(&cl.t));
}

/* ── §6: bypass suite — every way this could fail to SEE ──────────────── */

static void test_bypass_suite(void)
{
    printf("--- 6. bypass suite (false negatives) ---\n");

    /* 6b. A writer that wrote nothing. Sum == sum would be 0 == 0. */
    {
        const char *dir = fresh_dir("disabled");
        struct pgwt_summary_writer *w = new_writer(dir);
        w->enabled = false;
        replay(w, &sched_main, BASE, GRID_END, 1, 0, 0);
        CHECK(w->total_records_written == 0,
              "6b disabled writer wrote nothing");
        close_writer(w);
        struct secs t;
        int rc = read_secs(dir, &t);
        CHECK(rc < 0, "6b ... and the read REFUSES rather than returning an "
              "empty agreement (rc=%d)", rc);
    }

    /* 6e. One of the two files missing. The record-count gate must go red
     *     instead of the sum quietly agreeing with the survivor. */
    {
        const char *dir = fresh_dir("onefile");
        struct pgwt_summary_writer *a = new_writer(dir);
        replay(a, &sched_main, BASE, BASE + 2500 * MS, 1, 0, 0);
        close_writer(a);
        struct pgwt_summary_writer *b = new_writer(dir);
        replay(b, &sched_main, BASE + 2600 * MS, GRID_END, 1, 0, 0);
        close_writer(b);
        struct secs full;
        int rc_full = read_secs(dir, &full);
        CHECK(rc_full == 5, "6e both files present: 5 records (%d)", rc_full);
        char cmd[760];
        snprintf(cmd, sizeof(cmd), "rm -f '%s'/*.summary.lz4", dir);
        CHECK(system(cmd) == 0, "6e the archived (outgoing) file is removed");
        struct secs part;
        int rc_part = read_secs(dir, &part);
        CHECK(rc_part > 0 && rc_part != 5,
              "6e ... and the read now returns a DIFFERENT record count "
              "(%d != 5), so the count gate fails instead of the sum "
              "agreeing with the survivor", rc_part);
        CHECK(rc_part > 0 && all_total(&part) < all_total(&full),
              "6e the surviving sum is strictly smaller (%llu < %llu ns)",
              (unsigned long long)all_total(&part),
              (unsigned long long)all_total(&full));
    }

    /* 6f. An unparseable file. */
    {
        const char *dir = fresh_dir("garbage");
        char path[700];
        snprintf(path, sizeof(path), "%s/current.summary", dir);
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "6f garbage file created");
        if (f) {
            unsigned char junk[256];
            memset(junk, 0xA5, sizeof(junk));
            size_t nw = fwrite(junk, 1, sizeof(junk), f);
            fclose(f);
            CHECK(nw == sizeof(junk), "6f garbage written");
        }
        struct secs t;
        int rc = read_secs(dir, &t);
        CHECK(rc < 0, "6f bad magic: the read refuses (rc=%d)", rc);
    }

    /* 6g. A missing directory. */
    {
        char dir[700];
        snprintf(dir, sizeof(dir), "%s/no-such-dir", g_base);
        struct secs t;
        int rc = read_secs(dir, &t);
        CHECK(rc < 0, "6g missing directory: the read refuses (rc=%d)", rc);
    }
}

int main(void)
{
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_restart2nd_XXXXXX");
    if (!mkdtemp(g_base)) {
        perror("mkdtemp");
        return 1;
    }
    printf("=== test_restart_second_identity (#284) ===\n");
    printf("scratch: %s\n", g_base);

    test_restart_second();
    test_boundary_handover();
    test_disjoint_files();
    test_perturbation();
    test_unclamped_inherited_start();
    test_bypass_suite();

    char cmd[760];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failures\n", checks, failures);
    printf("=== test_restart_second_identity: %s ===\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
