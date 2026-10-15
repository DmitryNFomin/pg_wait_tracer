/* test_open_cpu_accounting.c — #424 / summary v9: an OPEN on-CPU interval
 * must be credited to the seconds it occupies WITHOUT destroying v7's
 * measured-CPU split.
 *
 * WHY THIS TEST EXISTS, AND WHAT MAKES EACH ASSERTION ABLE TO GO RED
 * ─────────────────────────────────────────────────────────────────────────
 * v8 refused on-CPU intervals outright, so an in-flight pure-CPU statement
 * was ABSENT from the CPU rows of the summaries path until it ended. v8's
 * reason was real: a per-second credit has no cpu_ns, so spreading the wall
 * while the whole measurement arrived at close would clamp the entire
 * measurement into cpu_clamped_ns and leave ZERO measured. v9 credits the
 * gap from a producer-supplied CPU OBSERVATION
 * (pgwt_summary_note_open_ex + struct pgwt_summary_open_cpu) and must
 * therefore satisfy that constraint, not bypass it.
 *
 *  §1 PLACEMENT AND SPLIT, TOGETHER. A 10 s on-CPU gap at 60% CPU must put
 *     1 s of wall in EACH of the ten seconds it covered AND still report
 *     6000 ms of MEASURED CPU with ZERO clamped. Red inputs, each
 *     demonstrated: the v8 rule (gap refused) puts the wall in one second
 *     and leaves nine empty; "credit the wall, clamp the measurement at
 *     close" reports ~480 ms measured and ~5520 ms clamped.
 *  §2 CONSERVATION, with the two sides coming from DIFFERENT places. The
 *     wall summed off DISK equals the fixture's literal duration; the
 *     measured CPU summed off DISK plus the clamp equals the fixture's
 *     literal cpu_ns. Neither side is a writer counter, so the identity
 *     cannot be satisfied by comparing one sum with itself.
 *  §3 EITHER ORDER of (tick, close), including a tick that credits PAST the
 *     close. The pid's whole timeline must stay conserved to the nanosecond;
 *     where attribution shifts by one tick the shift is bounded and the
 *     over-credited measurement is COUNTED.
 *  §4 CLASSIFICATION CHANGES — the subtlest piece. A we == 0 gap is CPU or
 *     (foreground, outside a command) idle, and the producer's answer is a
 *     MAJORITY over the open prefix, so it CAN FLIP before close. Written
 *     seconds are immutable, so the answer is FROZEN at the first credit and
 *     used for every later credit and for the closing record. The gate: the
 *     whole interval's time sits under ONE event id, and the divergence from
 *     the producer's final answer is counted in nanoseconds. Red input:
 *     persisting each tick's answer splits one interval across two ids.
 *  §5 THE UNKNOWN TIER. No measurement at all must file the whole wall in
 *     cpu_unknown_ns and nothing in cpu_measured_ns.
 *  §6 A TIER FLIP. Credits that went to the unknown tier must not have the
 *     closing cpu_ns added to the measured one: CPU* is measured + unknown,
 *     so that double counts the credited wall. Red input: 16 s of CPU* on a
 *     10 s gap.
 *  §7 NON-VACUITY. A fixture with NO open CPU must be unchanged: every
 *     record BIT-IDENTICAL between the v8 entry point and the v9 one with an
 *     observation attached (which a wait must ignore), and every v9 counter
 *     zero.
 *
 * THE BYPASS SUITE (§8) — one leg per way this gate's DETECTION could be
 * made unreachable, which is the failure mode that actually ships. Each is
 * demonstrated failing before the fix.
 *   a. NO OBSERVATION (the v8 entry point). Must degrade to v8 EXACTLY.
 *      This is the reachability probe: if §1 still passed with the
 *      observation withheld, §1 would not be measuring the credit path.
 *   b. A STALE observation — a pid that died without an EXIT record must
 *      stop accruing within PGWT_SUMMARY_OPEN_STALE_NS.
 *   c. AN EXIT. forget_open must retire the on-CPU slot exactly.
 *   d. A MISMATCHED note (the open gap is not the one the record closes).
 *      Charge the record WHOLE, never twice, and COUNT the disagreement.
 *   e. AN IO_WORKER pid: outside the model on the open path too.
 *   f. AN UNPARSEABLE FIELD: an eff_we that is neither 0 nor
 *      PGWT_WEI_NONCMD_CPU must be refused to the raw id and counted, never
 *      used to refile time into an arbitrary class.
 *   g. THE TABLE FULL: refusal counted, the un-noted pid degrades to v8.
 *   h. NO RECORDS / NO DIRECTORY: the read-back must REFUSE. A gate that
 *      sums zero records agrees with everything.
 *   i. THE WRONG VERSION ON DISK: a v8-stamped file must be refused, so
 *      evidence gathered against the old accounting rule cannot green a v9
 *      run.
 *   j. A GAP THAT NEVER SPANS A SECOND: nothing to credit, and the answer
 *      must be the v8 one. This is the input that makes a careless
 *      "conservation" check pass while placement is broken.
 *   k. A PURE OPEN-CREDIT SECOND MUST SURVIVE THE ROUND TRIP. The id-0
 *      events[] row such a second creates has time and no count; if the
 *      serializer skips it while the block header counts it, the reader
 *      walks a count it was handed and the record fails to decode — a
 *      silently dropped second, not a cosmetic loss.
 *   l. A DISABLED WRITER must produce no records, and the harness must FAIL
 *      on that rather than report agreement over an empty file.
 *
 * Server build (no BPF, no live PG), ~0.3 s. Every timestamp is a literal
 * offset from a pinned clock anchor: nothing here reads a real clock, so no
 * assertion depends on scheduling. */
#define _GNU_SOURCE
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <dirent.h>
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
#define PID_B        1002u
#define PID_IOW      1003u
#define BASE_MONO    (10000ULL * S)      /* second-aligned synthetic clock */

static const uint32_t EV_LOCK_REL  = (PG_WAIT_LOCK   << 24) | 0;
static const uint32_t EV_NONCMD    = PGWT_WEI_NONCMD_CPU;

/* ── fixture plumbing ─────────────────────────────────────────────────── */

static const char *fresh_dir(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof(buf), "/tmp/pgwt_opencpu_%s_%d", name, (int)getpid());
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
    uint64_t cpu_measured_ns;
    uint64_t cpu_unknown_ns;
    uint64_t cpu_clamped_ns;
    uint64_t cpu_measured_records;
    /* events[] for the two ids an on-CPU gap can carry */
    uint64_t ev_cpu_total_ns,    ev_cpu_count,    ev_cpu_max_ns;
    uint64_t ev_noncmd_total_ns, ev_noncmd_count, ev_noncmd_max_ns;
    uint64_t ev_cpu_hist[HISTOGRAM_BUCKETS];
    uint64_t sess_db_ns;
    uint64_t sess_cpu_ns;
    uint64_t blob_hash;           /* FNV-1a of the re-serialized record */
};

/* Enough for every fixture here (the longest writes ~40 seconds) with room
 * to spare, and small enough that the five readbacks one test function holds
 * are ~430 KB of stack rather than 6.5 MB -- a test that dies on a stack
 * overflow is a gate that could not run, which this file's own §8 says is a
 * failure and not a skip. Overflow REFUSES (see rb_visit / read_back) rather
 * than truncating, so growing a fixture past the cap can never silently
 * shorten the sums every assertion is built on. */
#define RB_MAX_ROWS 256

struct readback {
    struct sec_row rows[RB_MAX_ROWS];
    int n;
    int overflow;
    uint32_t want_pid;
    uint8_t *blob;
};

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int rb_visit(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct readback *rb = vctx;
    if (rb->n >= RB_MAX_ROWS) {
        rb->overflow = 1;
        return 1;
    }
    struct sec_row *r = &rb->rows[rb->n++];
    memset(r, 0, sizeof(*r));
    r->wall = rec->second_wall_ns;
    r->idle_ns = rec->idle_ns;
    memcpy(r->class_ns, rec->class_ns, sizeof(r->class_ns));
    r->cpu_measured_ns      = rec->cpu_measured_ns;
    r->cpu_unknown_ns       = rec->cpu_unknown_ns;
    r->cpu_clamped_ns       = rec->cpu_clamped_ns;
    r->cpu_measured_records = rec->cpu_measured_records;
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        const struct pgwt_summary_event *e = &rec->events[i];
        if (e->event_id == 0) {
            /* An EMPTY slot and the id-0 (CPU) row are both event_id 0; the
             * empty one is all-zero, so summing is exact either way. */
            r->ev_cpu_total_ns += e->total_ns;
            r->ev_cpu_count    += e->count;
            if (e->max_ns > r->ev_cpu_max_ns) r->ev_cpu_max_ns = e->max_ns;
            for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
                r->ev_cpu_hist[b] += e->histogram[b];
        } else if (e->event_id == EV_NONCMD) {
            r->ev_noncmd_total_ns += e->total_ns;
            r->ev_noncmd_count    += e->count;
            if (e->max_ns > r->ev_noncmd_max_ns)
                r->ev_noncmd_max_ns = e->max_ns;
        }
    }
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (rec->sessions[i].pid == rb->want_pid) {
            r->sess_db_ns  = rec->sessions[i].db_time_ns;
            r->sess_cpu_ns = rec->sessions[i].cpu_ns;
        }
    /* BIT-IDENTITY witness (§7): the record's own serialized payload, run
     * back through the shipping serializer. Comparing hashes compares the
     * bytes, not a hand-picked subset of fields. */
    if (rb->blob) {
        size_t n = pgwt_summary_serialize(rec, rb->blob,
                                          PGWT_SUMMARY_SERIALIZE_MAX);
        r->blob_hash = n ? fnv1a(rb->blob, n) : 0;
    }
    return 0;
}

/* Read every record back. Returns the count, or -1 — never 0-as-success:
 * "no records" must FAIL the caller, not read as agreement (bypass §8h). */
static int read_back(const char *dir, struct readback *rb)
{
    rb->n = 0;
    rb->overflow = 0;
    if (!rb->blob)
        rb->blob = malloc(PGWT_SUMMARY_SERIALIZE_MAX);
    int rc = pgwt_visit_summaries(dir, 0, 0, rb_visit, rb);
    if (rc <= 0 || rb->overflow)
        return -1;
    return rb->n;
}

#define RB_SUM(rb, field) ({                                              \
    uint64_t _t = 0;                                                      \
    for (int _i = 0; _i < (rb)->n; _i++) _t += (rb)->rows[_i].field;      \
    _t; })

static uint64_t rb_sum_class(const struct readback *rb, int cls)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].class_ns[cls];
    return t;
}
static int rb_seconds_with_cpu_class(const struct readback *rb)
{
    int n = 0;
    for (int i = 0; i < rb->n; i++)
        if (rb->rows[i].class_ns[PGWT_CLASS_CPU]) n++;
    return n;
}
static int rb_seconds_with_measured(const struct readback *rb)
{
    int n = 0;
    for (int i = 0; i < rb->n; i++) if (rb->rows[i].cpu_measured_records) n++;
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
}

/* Rewrite the ACCOUNTING VERSION in every summary header under `dir`
 * (bypass §8i). Same helper as tests/test_idle_summary.c's. */
static int set_version(const char *dir, uint32_t version)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 8 || strcmp(e->d_name + len - 8, ".summary") != 0)
            continue;
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *fp = fopen(path, "r+b");
        if (!fp) continue;
        struct pgwt_trace_file_header hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
            hdr.magic == PGWT_SUMMARY_MAGIC) {
            hdr.version = version;
            if (fseek(fp, 0, SEEK_SET) == 0 &&
                fwrite(&hdr, sizeof(hdr), 1, fp) == 1)
                n++;
        }
        fclose(fp);
    }
    closedir(d);
    return n;
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

/* The same tick through the v8 entry point: no observation at all. */
static void tick_cpu_v8(struct wr *x, uint64_t now, uint32_t pid,
                        uint64_t start, uint32_t cat)
{
    pgwt_summary_note_open(x->w, pid, 0, start, cat, now);
    pgwt_summary_flush_completed(x->w, now);
}

/* A 60%-CPU producer: measured CPU over a prefix of `elapsed` ns. */
static uint64_t cpu60(uint64_t elapsed) { return elapsed * 3 / 5; }

/* ── The §1/§2 fixture, parameterised so §8a can withhold the observation ─
 *
 * pid A: a 1 s Lock record closing at t=1 s (which anchors the file and
 * gives the pid a closed interval first, as a real stream does), then an
 * on-CPU gap [1 s, 11 s) running at 60% CPU, ticked once a second from
 * t=2.2 s to t=10.2 s, closing at t=11 s with cpu_ns = 6000 ms. */
static void run_gap_fixture(struct wr *x, int with_observation)
{
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0,
             0, PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x->w, &seed, 0, seed.old_event);

    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        if (with_observation)
            tick_cpu(x, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                     cpu60(now - (BASE_MONO + 1 * S)));
        else
            tick_cpu_v8(x, now, PID_A, BASE_MONO + 1 * S, 0);
    }

    struct pgwt_trace_event close_rec =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 6 * S);
    pgwt_summary_push_event(x->w, &close_rec, 0, close_rec.old_event);
}

/* ── §1 + §2 ──────────────────────────────────────────────────────────── */

static void test_placement_split_conservation(void)
{
    struct wr x;
    if (wr_open(&x, "placed") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    run_gap_fixture(&x, 1);

    /* Writer counters snapshotted BEFORE close, so the close's own
     * contribution is not folded into the open-credit totals. */
    uint64_t c_credited = x.w->open_cpu_credited_ns_total;
    uint64_t c_measured = x.w->open_cpu_measured_ns_total;
    uint64_t c_refused  = x.w->open_cpu_refused_total;
    uint64_t c_over     = x.w->open_cpu_overcredit_ns_total;
    uint64_t c_diverge  = x.w->open_cpu_class_divergent_ns_total;
    uint64_t c_tier     = x.w->open_cpu_tier_mismatch_total;
    uint64_t c_uncred   = x.w->cpu_open_uncredited_ns_total;
    wr_close(&x);

    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "placed: records read back (%d)", n);
    if (n <= 0) { wr_free(&x); return; }

    /* §1 PLACEMENT. Ten seconds, 1000 ms each. */
    CHECK(rb_seconds_with_cpu_class(&rb) == 10,
          "PLACEMENT: the 10 s on-CPU gap appears in %d distinct seconds "
          "(the ten it occupied), not 1",
          rb_seconds_with_cpu_class(&rb));
    for (int i = 0; i < rb.n; i++) {
        uint64_t sec = (rb.rows[i].wall - BASE_MONO) / S;
        if (sec >= 1 && sec <= 10)
            CHECK(rb.rows[i].class_ns[PGWT_CLASS_CPU] == 1 * S,
                  "PLACEMENT: second %llu carries 1000 ms of CPU wall "
                  "(%llu ms)", (unsigned long long)sec,
                  (unsigned long long)(rb.rows[i].class_ns[PGWT_CLASS_CPU]
                                       / MS));
    }

    /* §1 THE SPLIT SURVIVES — the whole point of v8's refusal. */
    uint64_t measured = RB_SUM(&rb, cpu_measured_ns);
    uint64_t clamped  = RB_SUM(&rb, cpu_clamped_ns);
    uint64_t unknown  = RB_SUM(&rb, cpu_unknown_ns);
    CHECK(measured == 6000 * MS,
          "SPLIT: cpu_measured_ns = %llu ms (the closing record's whole "
          "6000 ms measurement, placed across the seconds it was burned in)",
          (unsigned long long)(measured / MS));
    /* The two naive fixes, with the numbers this fixture actually produces
     * when each is applied (tests/results/issue424-v9/red-*.log):
     *   - credit the wall and leave the measurement to the close
     *     ("wall-only-credit"): measured 0 ms, clamped 6000 ms. v8's
     *     prediction, exactly.
     *   - credit both but clamp the closing remainder against the remaining
     *     wall rather than the interval's ("naive-clamp-at-close"): measured
     *     5520 ms, clamped 480 ms -- the tail silently disappears. */
    CHECK(clamped == 0,
          "SPLIT: cpu_clamped_ns = %llu ms — crediting the wall and leaving "
          "the measurement to the close clamps all 6000 ms away, and "
          "clamping the remainder against the remaining wall loses the "
          "480 ms tail", (unsigned long long)(clamped / MS));
    CHECK(unknown == 0,
          "SPLIT: cpu_unknown_ns = %llu ms (this tier HAS a measurement)",
          (unsigned long long)(unknown / MS));
    /* has_measured_cpu follows cpu_measured_records, so a sub-window made
     * only of open-credit seconds must still carry one. */
    CHECK(rb_seconds_with_measured(&rb) == 11,
          "SPLIT: %d seconds carry a CPU OBSERVATION (the ten open-credit "
          "ones plus the closing record), so a window made only of "
          "still-open seconds reports has_measured_cpu = 1 instead of "
          "discarding the split it was handed",
          rb_seconds_with_measured(&rb));

    /* §2 CONSERVATION. Left sides come off DISK, right sides are the
     * fixture's literals — never one sum compared with itself. */
    CHECK(rb_sum_class(&rb, PGWT_CLASS_CPU) == 10 * S,
          "CONSERVATION(wall): sum over records = %llu ms == the gap's "
          "10000 ms, exactly once",
          (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_CPU) / MS));
    CHECK(measured + clamped == 6 * S,
          "CONSERVATION(measured): sum(cpu_measured) + sum(cpu_clamped) = "
          "%llu ms == the closing record's cpu_ns",
          (unsigned long long)((measured + clamped) / MS));
    CHECK(RB_SUM(&rb, ev_cpu_total_ns) == 10 * S,
          "CONSERVATION(events): events[CPU].total_ns = %llu ms",
          (unsigned long long)(RB_SUM(&rb, ev_cpu_total_ns) / MS));
    CHECK(RB_SUM(&rb, sess_cpu_ns) == 10 * S &&
          RB_SUM(&rb, sess_db_ns) == 11 * S,
          "CONSERVATION(sessions): cpu_ns %llu ms, db_time %llu ms "
          "(10 s gap + the 1 s Lock seed)",
          (unsigned long long)(RB_SUM(&rb, sess_cpu_ns) / MS),
          (unsigned long long)(RB_SUM(&rb, sess_db_ns) / MS));

    /* OCCURRENCE QUANTITIES STAY WHOLE AND ONCE (v8's rule, unchanged). */
    CHECK(RB_SUM(&rb, ev_cpu_count) == 1,
          "ONCE: events[CPU].count = %llu over the whole file, not ten "
          "one-second observations",
          (unsigned long long)RB_SUM(&rb, ev_cpu_count));
    uint64_t hist_obs = 0;
    int hist_bucket = -1;
    for (int i = 0; i < rb.n; i++)
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
            if (rb.rows[i].ev_cpu_hist[b]) {
                hist_obs += rb.rows[i].ev_cpu_hist[b];
                hist_bucket = b;
            }
    uint64_t maxns = 0;
    for (int i = 0; i < rb.n; i++)
        if (rb.rows[i].ev_cpu_max_ns > maxns) maxns = rb.rows[i].ev_cpu_max_ns;
    CHECK(hist_obs == 1 && hist_bucket == HISTOGRAM_BUCKETS - 1,
          "ONCE: exactly %llu histogram observation, in the top (10 s) "
          "bucket (%d) — not ten one-second ones",
          (unsigned long long)hist_obs, hist_bucket);
    CHECK(maxns == 10 * S,
          "ONCE: events[CPU].max_ns = %llu ms — the longest SINGLE gap",
          (unsigned long long)(maxns / MS));

    /* The counters, which are the only way a silent degradation is visible. */
    CHECK(c_credited == 10 * S,
          "COUNTED: open_cpu_credited_ns_total = %llu ms",
          (unsigned long long)(c_credited / MS));
    CHECK(c_measured == 5520 * MS,
          "COUNTED: open_cpu_measured_ns_total = %llu ms (the measurement "
          "placed while OPEN; the 480 ms tail rides the closing record)",
          (unsigned long long)(c_measured / MS));
    CHECK(c_refused == 0 && c_over == 0 && c_diverge == 0 && c_tier == 0 &&
          c_uncred == 0,
          "COUNTED: no refusal (%llu), no over-credit (%llu ms), no "
          "classification divergence (%llu ms), no tier flip (%llu), no "
          "uncredited residual (%llu ms)",
          (unsigned long long)c_refused, (unsigned long long)(c_over / MS),
          (unsigned long long)(c_diverge / MS), (unsigned long long)c_tier,
          (unsigned long long)(c_uncred / MS));
    wr_free(&x);
}

/* ── §2b COHERENT PER-SECOND OBSERVATIONS ─────────────────────────────── */

/* How many records credited open CPU (events[CPU].count == 0: nothing
 * CLOSED in them) report MORE measured CPU than their own CPU wall. Must be
 * zero. The closing record is deliberately exempt and is excluded by the
 * count test: its remainder may exceed the tail of wall it carries, which
 * the v9 note states and §1 relies on.
 *
 * This is the assertion the per-second CAP exists for, and the only one that
 * can see it: without min(prefix, wall credited so far) the totals still
 * come out right -- the producer's observation is always NEWER than the
 * second being sealed, so the extra CPU is real, just not in this second --
 * and only the per-second shape goes wrong. A second claiming 1200 ms of CPU
 * inside 1000 ms of wall makes the reader's residual (DB - CPU* - waits)
 * negative and shows up as phantom clamp, which is how an incoherent record
 * reaches a user. */
static int rb_incoherent_seconds(const struct readback *rb)
{
    int bad = 0;
    for (int i = 0; i < rb->n; i++) {
        if (rb->rows[i].ev_cpu_count != 0) continue;
        if (rb->rows[i].cpu_measured_ns + rb->rows[i].cpu_unknown_ns >
            rb->rows[i].class_ns[PGWT_CLASS_CPU])
            bad++;
    }
    return bad;
}

static void test_coherence(void)
{
    /* A gap at 100% CPU is where the cap binds: at tick (k+1).2 s the
     * producer has measured k+0.2 s of CPU while only k s of wall has been
     * sealed, so an uncapped credit puts 1200 ms of CPU into a 1000 ms
     * second. Gap [1 s, 11 s), cpu_ns = 10 s at close. */
    struct wr x;
    if (wr_open(&x, "coherent") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&x, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 now - (BASE_MONO + 1 * S));      /* 100% CPU */
    }
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 10 * S);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    wr_close(&x);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "coherent: records read back (%d)", n);
    if (n > 0) {
        CHECK(rb_incoherent_seconds(&rb) == 0,
              "COHERENT: %d open-credit seconds claim more measured CPU than "
              "their own wall — a 100%% CPU gap is where an uncapped credit "
              "puts 1200 ms of CPU in a 1000 ms second, which makes the "
              "reader's DB - CPU* - waits residual negative",
              rb_incoherent_seconds(&rb));
        CHECK(RB_SUM(&rb, cpu_measured_ns) + RB_SUM(&rb, cpu_clamped_ns)
                  == 10 * S,
              "COHERENT: and the total is still exact — measured %llu ms + "
              "clamped %llu ms == the 10000 ms measurement",
              (unsigned long long)(RB_SUM(&rb, cpu_measured_ns) / MS),
              (unsigned long long)(RB_SUM(&rb, cpu_clamped_ns) / MS));
        CHECK(rb_sum_class(&rb, PGWT_CLASS_CPU) == 10 * S &&
              rb_seconds_with_cpu_class(&rb) == 10,
              "COHERENT: wall %llu ms across %d seconds",
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_CPU) / MS),
              rb_seconds_with_cpu_class(&rb));
    }
    wr_free(&x);
}

/* ── §3 EITHER ORDER ──────────────────────────────────────────────────── */

static void test_orders(void)
{
    /* (a) ONE tick, then a close three seconds later: the writer seals the
     *     two un-ticked seconds at the close and the measurement still
     *     totals exactly. Gap [1 s, 4 s), 60% CPU, cpu_ns = 1800 ms. */
    struct wr x;
    if (wr_open(&x, "order_a") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    tick_cpu(&x, BASE_MONO + 2 * S + 200 * MS, PID_A, BASE_MONO + 1 * S, 0, 0,
             cpu60(1 * S + 200 * MS));
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 4 * S, 3 * S, 0, EV_LOCK_REL, 0, 1800 * MS);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    wr_close(&x);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "order_a: records read back (%d)", n);
    if (n > 0) {
        CHECK(rb_sum_class(&rb, PGWT_CLASS_CPU) == 3 * S &&
              rb_seconds_with_cpu_class(&rb) == 3,
              "ORDER(one tick, late close): 3000 ms of wall over %d seconds",
              rb_seconds_with_cpu_class(&rb));
        CHECK(RB_SUM(&rb, cpu_measured_ns) + RB_SUM(&rb, cpu_clamped_ns)
                  == 1800 * MS,
              "ORDER(one tick, late close): measured %llu ms + clamped "
              "%llu ms == the 1800 ms measurement",
              (unsigned long long)(RB_SUM(&rb, cpu_measured_ns) / MS),
              (unsigned long long)(RB_SUM(&rb, cpu_clamped_ns) / MS));
    }
    wr_free(&x);

    /* (b) A TICK THAT CREDITS PAST THE CLOSE. The producer is one tick
     *     behind: it still reports the gap [1 s, 4 s) as open at t=5.2 s, so
     *     seconds 2, 3 AND 4 are credited under it before the closing record
     *     (which ends at 4 s) arrives. The pid's TIMELINE must still be
     *     conserved to the nanosecond -- the fourth second is pre-charged
     *     against the next interval -- and the measurement over-credited
     *     past the record's own cpu_ns must be COUNTED, never hidden. */
    struct wr y;
    if (wr_open(&y, "order_b") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    tick_cpu(&y, BASE_MONO + 2 * S + 200 * MS, PID_A, BASE_MONO + 1 * S, 0, 0,
             cpu60(1 * S + 200 * MS));
    tick_cpu(&y, BASE_MONO + 5 * S + 200 * MS, PID_A, BASE_MONO + 1 * S, 0, 0,
             cpu60(4 * S + 200 * MS));
    struct pgwt_trace_event cl2 =
        mkev(PID_A, BASE_MONO + 4 * S, 3 * S, 0, EV_LOCK_REL, 0, 1800 * MS);
    pgwt_summary_push_event(y.w, &cl2, 0, cl2.old_event);
    /* The NEXT interval on the same pid: a 3 s Lock wait [4 s, 7 s). Its
     * note pre-charges the second the previous tick credited past the
     * close, which is what keeps the timeline exact. */
    for (int s = 6; s <= 6; s++)
        pgwt_summary_note_open(y.w, PID_A, EV_LOCK_REL, BASE_MONO + 4 * S, 0,
                               BASE_MONO + (uint64_t)s * S + 200 * MS),
        pgwt_summary_flush_completed(y.w,
                               BASE_MONO + (uint64_t)s * S + 200 * MS);
    struct pgwt_trace_event cl3 =
        mkev(PID_A, BASE_MONO + 7 * S, 3 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(y.w, &cl3, 0, cl3.old_event);
    uint64_t over = y.w->open_cpu_overcredit_ns_total;
    wr_close(&y);
    struct readback rb2 = { .want_pid = PID_A };
    int n2 = read_back(y.dir, &rb2);
    CHECK(n2 > 0, "order_b: records read back (%d)", n2);
    if (n2 > 0) {
        uint64_t cpu_wall  = rb_sum_class(&rb2, PGWT_CLASS_CPU);
        uint64_t lock_wall = rb_sum_class(&rb2, PGWT_CLASS_LOCK);
        /* 1 s seed Lock + 3 s gap + 3 s Lock wait = 7 s of pid timeline. */
        CHECK(cpu_wall + lock_wall == 7 * S,
              "ORDER(tick past close): the pid's whole timeline is "
              "conserved — CPU %llu ms + Lock %llu ms = 7000 ms, no "
              "nanosecond counted twice or lost",
              (unsigned long long)(cpu_wall / MS),
              (unsigned long long)(lock_wall / MS));
        CHECK(cpu_wall == 4 * S,
              "ORDER(tick past close): attribution moved by ONE tick — the "
              "CPU label holds %llu ms of the 3000 ms gap, the overlap "
              "staying filed under the interval the tick believed was open "
              "(v8 residual 2, unchanged)",
              (unsigned long long)(cpu_wall / MS));
        CHECK(over == 720 * MS,
              "ORDER(tick past close): the measurement credited beyond the "
              "closing record's own cpu_ns is COUNTED — "
              "open_cpu_overcredit_ns_total = %llu ms",
              (unsigned long long)(over / MS));
    }
    wr_free(&y);
}

/* ── §3c ROTATION ACROSS A GAP ────────────────────────────────────────── */

static void test_rotation_mid_gap(void)
{
    /* An on-CPU gap that is still open when the hourly file rotates. The
     * open table, the per-PID watermark and the per-interval FREEZE all live
     * on the WRITER, not on the file, so the credits must continue into the
     * new file under the SAME frozen label and the total must stay exact --
     * if any of it were file-scoped, the gap would be re-frozen (possibly
     * under the other label) or re-credited from its start.
     *
     * Rotation is forced by zeroing w->current_hour rather than by waiting
     * for a wall-clock hour: pgwt_summary_check_rotation compares the hour
     * it last saw against localtime(), so a mismatched cached value triggers
     * the real rotation path deterministically. Nothing here reads a clock
     * that the assertions depend on. */
    struct wr x;
    if (wr_open(&x, "rotate") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&x, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 cpu60(now - (BASE_MONO + 1 * S)));
        if (s == 5) {
            x.w->current_hour = -1;             /* force the hour to differ */
            CHECK(pgwt_summary_check_rotation(x.w) == 0,
                  "ROTATION: the file rotated mid-gap");
        }
    }
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 6 * S);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    uint64_t credited = x.w->open_cpu_credited_ns_total;
    wr_close(&x);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "rotation: records read back (%d)", n);
    if (n > 0) {
        /* The rotation discards at most the one second whose accumulator was
         * open when it fired, so the wall is 9000 or 10000 ms -- never more
         * (re-crediting from the start would give ~14000) and never under 9. */
        uint64_t wall = rb_sum_class(&rb, PGWT_CLASS_CPU);
        CHECK(wall >= 9 * S && wall <= 10 * S,
              "ROTATION: the gap's wall is %llu ms across the two files — at "
              "most the 10000 ms it lasted, and at least the 9000 ms the "
              "rotation could not have discarded. Re-crediting from the "
              "interval's start in the new file would read ~14000",
              (unsigned long long)(wall / MS));
        CHECK(RB_SUM(&rb, ev_noncmd_total_ns) == 0,
              "ROTATION: the frozen label survived the file change (0 ms "
              "under the other id, %llu ms under CPU)",
              (unsigned long long)(RB_SUM(&rb, ev_cpu_total_ns) / MS));
        CHECK(RB_SUM(&rb, cpu_measured_ns) + RB_SUM(&rb, cpu_clamped_ns)
                  <= 6 * S,
              "ROTATION: measured %llu ms + clamped %llu ms never exceeds "
              "the 6000 ms the record measured",
              (unsigned long long)(RB_SUM(&rb, cpu_measured_ns) / MS),
              (unsigned long long)(RB_SUM(&rb, cpu_clamped_ns) / MS));
        CHECK(rb_incoherent_seconds(&rb) == 0,
              "ROTATION: every open-credit second is still coherent (%d are "
              "not)", rb_incoherent_seconds(&rb));
    }
    CHECK(credited >= 9 * S && credited <= 10 * S,
          "ROTATION: COUNTED — open_cpu_credited_ns_total = %llu ms",
          (unsigned long long)(credited / MS));
    wr_free(&x);
}

/* ── §4 CLASSIFICATION CHANGES (the subtle one) ───────────────────────── */

static void test_classification_freeze(void)
{
    /* The gap [1 s, 11 s) starts OUTSIDE a command, so the producer's
     * majority answer is PGWT_WEI_NONCMD_CPU (idle). A command starts
     * part-way through and by t=7 s the majority has flipped to in-command,
     * so the producer starts saying 0 (CPU) -- and the closing record's own
     * eff_event is 0 too.
     *
     * Seconds 1..5 are ALREADY ON DISK as idle by then and cannot be
     * rewritten (#277). The rule: freeze at the first credit, apply it to
     * every later credit AND to the closing record, and count the
     * divergence. The gate is that the interval's 10 s sits under ONE id. */
    struct wr x;
    if (wr_open(&x, "classflip") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&x, now, PID_A, BASE_MONO + 1 * S, 0,
                 s < 7 ? EV_NONCMD : 0u,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 6 * S);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    uint64_t diverge = x.w->open_cpu_class_divergent_ns_total;
    wr_close(&x);

    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "classflip: records read back (%d)", n);
    if (n > 0) {
        CHECK(RB_SUM(&rb, ev_noncmd_total_ns) == 10 * S &&
              RB_SUM(&rb, ev_cpu_total_ns) == 0,
              "FREEZE: the whole 10 s sits under ONE event id — "
              "NonCmdCPU %llu ms, CPU %llu ms. Persisting each tick's answer "
              "would split one interval across both, leaving immutable "
              "seconds that disagree with each other and with the record",
              (unsigned long long)(RB_SUM(&rb, ev_noncmd_total_ns) / MS),
              (unsigned long long)(RB_SUM(&rb, ev_cpu_total_ns) / MS));
        CHECK(RB_SUM(&rb, ev_noncmd_count) == 1 &&
              RB_SUM(&rb, ev_cpu_count) == 0,
              "FREEZE: the single occurrence is counted on the SAME row as "
              "the time (NonCmdCPU count %llu, CPU count %llu) — a row with "
              "time and no count is what splitting them produces",
              (unsigned long long)RB_SUM(&rb, ev_noncmd_count),
              (unsigned long long)RB_SUM(&rb, ev_cpu_count));
        CHECK(RB_SUM(&rb, idle_ns) == 10 * S &&
              rb_sum_class(&rb, PGWT_CLASS_CPU) == 0,
              "FREEZE: an idle-classified gap is Idle for its whole length "
              "(%llu ms) and contributes NOTHING to the CPU class (%llu ms), "
              "exactly as the raw paths file post/between-command time",
              (unsigned long long)(RB_SUM(&rb, idle_ns) / MS),
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_CPU) / MS));
        CHECK(RB_SUM(&rb, cpu_measured_ns) == 0 &&
              RB_SUM(&rb, cpu_unknown_ns) == 0 &&
              RB_SUM(&rb, cpu_clamped_ns) == 0,
              "FREEZE: an idle gap touches NEITHER CPU scalar (measured "
              "%llu, unknown %llu, clamped %llu)",
              (unsigned long long)RB_SUM(&rb, cpu_measured_ns),
              (unsigned long long)RB_SUM(&rb, cpu_unknown_ns),
              (unsigned long long)RB_SUM(&rb, cpu_clamped_ns));
    }
    CHECK(diverge == 10 * S,
          "FREEZE: the cost of freezing is MEASURED, not assumed — "
          "open_cpu_class_divergent_ns_total = %llu ms (the producer's "
          "answer at close was CPU, the frozen one was idle)",
          (unsigned long long)(diverge / MS));
    wr_free(&x);
}

/* ── §5 the UNKNOWN tier, §6 a tier flip ──────────────────────────────── */

static void test_tiers(void)
{
    /* §5 No measurement anywhere: the whole wall is CPU* via the UNKNOWN
     *    tier and nothing claims to be measured. */
    struct wr x;
    if (wr_open(&x, "unknown") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++)
        tick_cpu(&x, BASE_MONO + (uint64_t)s * S + 200 * MS, PID_A,
                 BASE_MONO + 1 * S, 0, 0, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    uint64_t unk_counter = x.w->open_cpu_unknown_ns_total;
    wr_close(&x);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "unknown tier: records read back (%d)", n);
    if (n > 0) {
        CHECK(RB_SUM(&rb, cpu_unknown_ns) == 10 * S &&
              RB_SUM(&rb, cpu_measured_ns) == 0,
              "TIER(unknown): the whole 10 s wall is CPU* via the UNKNOWN "
              "tier (%llu ms) and the measured tier stays empty (%llu ms)",
              (unsigned long long)(RB_SUM(&rb, cpu_unknown_ns) / MS),
              (unsigned long long)RB_SUM(&rb, cpu_measured_ns));
        CHECK(RB_SUM(&rb, cpu_measured_records) == 0,
              "TIER(unknown): no CPU OBSERVATION is claimed (%llu), so the "
              "reader reports has_measured_cpu = 0 — an ABSENT measurement "
              "must not read as a measured zero",
              (unsigned long long)RB_SUM(&rb, cpu_measured_records));
        CHECK(rb_seconds_with_cpu_class(&rb) == 10,
              "TIER(unknown): still placed across %d seconds",
              rb_seconds_with_cpu_class(&rb));
    }
    CHECK(unk_counter == 10 * S,
          "TIER(unknown): COUNTED — open_cpu_unknown_ns_total = %llu ms",
          (unsigned long long)(unk_counter / MS));
    wr_free(&x);

    /* §6 A TIER FLIP: the credits had no measurement, the closing record
     *    does. CPU* is measured + unknown, so adding the closing cpu_ns to
     *    the measured tier would count the already-credited wall twice --
     *    16 s of CPU* on a 10 s gap. The tier is frozen with the class. */
    struct wr y;
    if (wr_open(&y, "tierflip") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++)
        tick_cpu(&y, BASE_MONO + (uint64_t)s * S + 200 * MS, PID_A,
                 BASE_MONO + 1 * S, 0, 0, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_trace_event cl2 =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 6 * S);
    pgwt_summary_push_event(y.w, &cl2, 0, cl2.old_event);
    uint64_t mismatch = y.w->open_cpu_tier_mismatch_total;
    wr_close(&y);
    struct readback rb2 = { .want_pid = PID_A };
    int n2 = read_back(y.dir, &rb2);
    CHECK(n2 > 0, "tier flip: records read back (%d)", n2);
    if (n2 > 0)
        CHECK(RB_SUM(&rb2, cpu_measured_ns) + RB_SUM(&rb2, cpu_unknown_ns)
                  == 10 * S,
              "TIER(flip): CPU* = measured %llu ms + unknown %llu ms = "
              "%llu ms, at most the gap's own 10000 ms of wall — not 16000",
              (unsigned long long)(RB_SUM(&rb2, cpu_measured_ns) / MS),
              (unsigned long long)(RB_SUM(&rb2, cpu_unknown_ns) / MS),
              (unsigned long long)((RB_SUM(&rb2, cpu_measured_ns)
                                    + RB_SUM(&rb2, cpu_unknown_ns)) / MS));
    CHECK(mismatch == 1,
          "TIER(flip): COUNTED — open_cpu_tier_mismatch_total = %llu",
          (unsigned long long)mismatch);
    wr_free(&y);
}

/* ── §7 NON-VACUITY: no open CPU => bit-identical records ─────────────── */

/* A wait-only fixture: a 10 s Lock wait, ticked every second. Driven either
 * through the v8 entry point or through the v9 one WITH an observation
 * attached, which a wait must ignore completely. */
static void run_wait_fixture(struct wr *x, int via_ex)
{
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, EV_LOCK_REL, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x->w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 11; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        if (via_ex) {
            /* A deliberately WRONG observation. If a single nanosecond of it
             * reaches a wait's accounting, the hashes below diverge. */
            struct pgwt_summary_open_cpu obs = { .eff_we = EV_NONCMD,
                                                 .cpu_prefix_ns = 9 * S };
            pgwt_summary_note_open_ex(x->w, PID_A, EV_LOCK_REL,
                                      BASE_MONO + 1 * S, 0, now, &obs);
        } else {
            pgwt_summary_note_open(x->w, PID_A, EV_LOCK_REL,
                                   BASE_MONO + 1 * S, 0, now);
        }
        pgwt_summary_flush_completed(x->w, now);
    }
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, EV_LOCK_REL, EV_LOCK_REL, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x->w, &cl, 0, cl.old_event);
}

static void test_non_vacuity(void)
{
    struct wr a, b;
    if (wr_open(&a, "nv_v8") != 0 || wr_open(&b, "nv_v9") != 0) {
        printf("  FAIL: setup\n"); failures++; return;
    }
    run_wait_fixture(&a, 0);
    run_wait_fixture(&b, 1);
    uint64_t cpu_counters_b =
        b.w->open_cpu_credited_ns_total + b.w->open_cpu_measured_ns_total +
        b.w->open_cpu_unknown_ns_total + b.w->open_cpu_eff_rejected_total +
        b.w->open_cpu_class_divergent_ns_total +
        b.w->open_cpu_tier_mismatch_total + b.w->open_cpu_overcredit_ns_total +
        b.w->open_cpu_refused_total;
    wr_close(&a);
    wr_close(&b);

    struct readback ra = { .want_pid = PID_A }, rbk = { .want_pid = PID_A };
    int na = read_back(a.dir, &ra);
    int nb = read_back(b.dir, &rbk);
    CHECK(na > 0 && nb > 0, "non-vacuity: records read back (%d / %d)",
          na, nb);
    if (na > 0 && nb > 0) {
        int same = (na == nb);
        for (int i = 0; same && i < na; i++)
            same = (ra.rows[i].blob_hash == rbk.rows[i].blob_hash) &&
                   ra.rows[i].blob_hash != 0;
        CHECK(same,
              "NON-VACUITY: a fixture with NO open on-CPU interval is "
              "BIT-IDENTICAL through the v9 entry point — all %d record "
              "payloads hash equal", na);
        CHECK(rb_sum_class(&ra, PGWT_CLASS_LOCK) == 11 * S,
              "NON-VACUITY: and the v8 answer itself is still right "
              "(%llu ms = 1 s seed + 10 s wait)",
              (unsigned long long)(rb_sum_class(&ra, PGWT_CLASS_LOCK) / MS));
    }
    CHECK(cpu_counters_b == 0,
          "NON-VACUITY: every v9 on-CPU counter is zero (%llu) — a wait's "
          "note cannot reach the CPU path even with an observation attached",
          (unsigned long long)cpu_counters_b);
    wr_free(&a);
    wr_free(&b);
}

/* ── §8 THE BYPASS SUITE ──────────────────────────────────────────────── */

static void test_bypass_no_observation(void)
{
    /* a. THE REACHABILITY PROBE. The same §1 fixture with the observation
     *    withheld must give the v8 answer: the whole wall in the closing
     *    second, the measurement whole and unclamped there, and the refusal
     *    counted. If this read "10 seconds", §1 would not be measuring the
     *    credit path at all. */
    struct wr x;
    if (wr_open(&x, "noobs") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    run_gap_fixture(&x, 0);
    uint64_t refused   = x.w->open_cpu_refused_total;
    uint64_t credited  = x.w->open_cpu_credited_ns_total;
    uint64_t uncredit  = x.w->cpu_open_uncredited_ns_total;
    wr_close(&x);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "no observation: records read back (%d)", n);
    if (n > 0) {
        CHECK(rb_seconds_with_cpu_class(&rb) == 1 &&
              rb_sum_class(&rb, PGWT_CLASS_CPU) == 10 * S,
              "BYPASS a (no CPU observation): the whole 10000 ms lands in "
              "ONE second (the v8 rule) — %d seconds carry it",
              rb_seconds_with_cpu_class(&rb));
        CHECK(RB_SUM(&rb, cpu_measured_ns) == 6 * S &&
              RB_SUM(&rb, cpu_clamped_ns) == 0,
              "BYPASS a: v7's split is untouched — measured %llu ms, "
              "clamped %llu ms",
              (unsigned long long)(RB_SUM(&rb, cpu_measured_ns) / MS),
              (unsigned long long)(RB_SUM(&rb, cpu_clamped_ns) / MS));
    }
    CHECK(refused > 0 && credited == 0,
          "BYPASS a: the refusal is COUNTED (open_cpu_refused_total = %llu) "
          "and nothing was credited (%llu ms)",
          (unsigned long long)refused, (unsigned long long)credited);
    CHECK(uncredit == 10 * S,
          "BYPASS a: the residual is MEASURED — cpu_open_uncredited_ns_total "
          "= %llu ms", (unsigned long long)(uncredit / MS));
    wr_free(&x);
}

static void test_bypass_lifecycle(void)
{
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);

    /* b. A STALE OBSERVATION. The producer notes once at t=2.2 s and then
     *    stops (SIGKILL, a dropped map entry). Crediting must stop within
     *    PGWT_SUMMARY_OPEN_STALE_NS -- a watermark that credits forever
     *    fabricates CPU out of a dead backend -- and the expiry is counted. */
    struct wr x;
    if (wr_open(&x, "stale") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    tick_cpu(&x, BASE_MONO + 2 * S + 200 * MS, PID_A, BASE_MONO + 1 * S, 0, 0,
             cpu60(1 * S + 200 * MS));
    for (int s = 3; s <= 40; s++)
        pgwt_summary_flush_completed(x.w, BASE_MONO + (uint64_t)s * S);
    uint64_t credited = x.w->open_cpu_credited_ns_total;
    uint64_t expired  = x.w->open_notes_expired_total;
    wr_close(&x);
    CHECK(credited > 0 && credited <= 4 * S,
          "BYPASS b (stale observation): crediting stopped at %llu ms, "
          "within the 3 s staleness bound — a dead backend cannot accrue "
          "CPU forever", (unsigned long long)(credited / MS));
    CHECK(expired == 1,
          "BYPASS b: the expiry is COUNTED (open_notes_expired_total = %llu)",
          (unsigned long long)expired);
    wr_free(&x);

    /* c. AN EXIT. forget_open must retire the on-CPU slot exactly -- the
     *    staleness rule is the fail-safe for the pids nobody tells us
     *    about, this is the exact one. */
    struct wr y;
    if (wr_open(&y, "exit") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    tick_cpu(&y, BASE_MONO + 2 * S + 200 * MS, PID_A, BASE_MONO + 1 * S, 0, 0,
             cpu60(1 * S + 200 * MS));
    uint64_t before = y.w->open_cpu_credited_ns_total;
    pgwt_summary_forget_open(y.w, PID_A);
    for (int s = 3; s <= 12; s++)
        pgwt_summary_flush_completed(y.w, BASE_MONO + (uint64_t)s * S);
    uint64_t after = y.w->open_cpu_credited_ns_total;
    wr_close(&y);
    CHECK(after == before,
          "BYPASS c (exit): nothing credited after forget_open (%llu ms "
          "before, %llu ms after)",
          (unsigned long long)(before / MS), (unsigned long long)(after / MS));
    wr_free(&y);

    /* d. A MISMATCHED OBSERVATION. The producer's open gap starts at 1 s;
     *    the record that arrives closes a DIFFERENT gap [5 s, 8 s). The
     *    record must be charged WHOLE (the v8 answer for the cases the
     *    identity check refuses) and the disagreement COUNTED -- an on-CPU
     *    slot also has we == 0, so without the `|| cpu_slot` clause this
     *    would be the one uncounted disagreement in the writer. */
    struct wr z;
    if (wr_open(&z, "mismatch") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(z.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 7; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&z, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    struct pgwt_trace_event odd =
        mkev(PID_A, BASE_MONO + 8 * S, 3 * S, 0, EV_LOCK_REL, 0, 1 * S);
    pgwt_summary_push_event(z.w, &odd, 0, odd.old_event);
    uint64_t mism = z.w->open_note_mismatch_total;
    wr_close(&z);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(z.dir, &rb);
    CHECK(n > 0, "mismatch: records read back (%d)", n);
    if (n > 0) {
        /* Credits placed 1 s in each of seconds 1..6 under the gap the
         * producer described; the mismatched record adds its own 3 s minus
         * only what the monotone watermark proves was inside its span
         * ([5 s, 8 s) overlaps the watermark at 7 s, so 2 s). Nothing is
         * counted twice, which is the invariant -- the exact split is
         * attribution. */
        CHECK(rb_sum_class(&rb, PGWT_CLASS_CPU) == 7 * S,
              "BYPASS d (mismatched observation): the wall is accounted "
              "exactly ONCE (%llu ms = 6 s credited + the 1 s of the "
              "record the watermark had not already placed), never twice",
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_CPU) / MS));
    }
    CHECK(mism == 1,
          "BYPASS d: the disagreement is COUNTED "
          "(open_note_mismatch_total = %llu)", (unsigned long long)mism);
    wr_free(&z);
}

static void test_bypass_refusals(void)
{
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);

    /* e. AN IO_WORKER pid. v6 keeps its time out of the model; the open CPU
     *    path must refuse it too, or v6's exclusion is relocated rather
     *    than kept. */
    struct wr x;
    if (wr_open(&x, "iow") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&x, now, PID_IOW, BASE_MONO + 1 * S,
                 PGWT_EVENT_FLAG_IO_WORKER, 0,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    uint64_t iow_refused = x.w->open_io_worker_refused_total;
    uint64_t iow_credit  = x.w->open_cpu_credited_ns_total;
    wr_close(&x);
    CHECK(iow_refused > 0 && iow_credit == 0,
          "BYPASS e (io_worker): nothing credited (%llu ms), refusal counted "
          "(%llu)", (unsigned long long)iow_credit,
          (unsigned long long)iow_refused);
    wr_free(&x);

    /* f. AN UNPARSEABLE FIELD. A producer passing an eff_we that is neither
     *    0 nor PGWT_WEI_NONCMD_CPU is a bug; accepting it would let a
     *    caller refile time into an arbitrary class through plumbing. It
     *    must fall back to the RAW id (CPU) and be counted. */
    struct wr y;
    if (wr_open(&y, "badeff") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(y.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&y, now, PID_A, BASE_MONO + 1 * S, 0, EV_LOCK_REL,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 11 * S, 10 * S, 0, EV_LOCK_REL, 0, 6 * S);
    pgwt_summary_push_event(y.w, &cl, 0, cl.old_event);
    uint64_t rejected = y.w->open_cpu_eff_rejected_total;
    wr_close(&y);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(y.dir, &rb);
    CHECK(n > 0, "bad eff_we: records read back (%d)", n);
    if (n > 0)
        CHECK(rb_sum_class(&rb, PGWT_CLASS_CPU) == 10 * S &&
              rb_sum_class(&rb, PGWT_CLASS_LOCK) == 1 * S,
              "BYPASS f (illegal eff_we): the gap stayed in the CPU class "
              "(%llu ms) and NOTHING was refiled into Lock beyond the 1 s "
              "seed (%llu ms)",
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_CPU) / MS),
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_LOCK) / MS));
    CHECK(rejected == 9,
          "BYPASS f: every illegal note is COUNTED "
          "(open_cpu_eff_rejected_total = %llu over 9 ticks)",
          (unsigned long long)rejected);
    wr_free(&y);

    /* g. THE TABLE FULL. Fill every slot, then note one more pid: the
     *    refusal must be counted and the un-noted pid must degrade to v8
     *    rather than to nothing. */
    struct wr z;
    if (wr_open(&z, "full") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    pgwt_summary_push_event(z.w, &seed, 0, seed.old_event);
    for (int i = 0; i < SUMMARY_MAX_OPEN + 8; i++) {
        struct pgwt_summary_open_cpu obs = { .eff_we = 0,
                                             .cpu_prefix_ns = 100 * MS };
        pgwt_summary_note_open_ex(z.w, 20000u + (uint32_t)i, 0,
                                  BASE_MONO + 1 * S, 0,
                                  BASE_MONO + 2 * S, &obs);
    }
    uint64_t full = z.w->open_table_full_total;
    wr_close(&z);
    CHECK(full >= 8,
          "BYPASS g (table full): the refusal is COUNTED "
          "(open_table_full_total = %llu)", (unsigned long long)full);
    wr_free(&z);
}

static void test_bypass_reader(void)
{
    /* h. NO RECORDS AND NO DIRECTORY. A read-back that returns zero records
     *    agrees with every assertion in this file, so the harness must
     *    REFUSE rather than report agreement. */
    struct readback rb = { .want_pid = PID_A };
    CHECK(read_back("/tmp/pgwt_opencpu_definitely_absent_dir", &rb) < 0,
          "BYPASS h (no such trace dir): the read-back REFUSES rather than "
          "returning an empty, agreeing answer");
    const char *empty = fresh_dir("emptydir");
    if (empty) {
        struct readback rb2 = { .want_pid = PID_A };
        CHECK(read_back(empty, &rb2) < 0,
              "BYPASS h (empty trace dir): the read-back REFUSES");
    }

    /* i. THE WRONG VERSION ON DISK. Evidence gathered under the v8
     *    accounting rule must not green a v9 run: a file whose header says
     *    v8 is refused outright, which is what turns a stale capture into a
     *    RAW RECOMPUTE instead of a plausible partial answer. */
    struct wr x;
    if (wr_open(&x, "oldver") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    for (int s = 2; s <= 6; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&x, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    wr_close(&x);
    struct readback ok = { .want_pid = PID_A };
    CHECK(read_back(x.dir, &ok) > 0,
          "wrong version: the v%d file reads back first (%d records), so the "
          "refusal below is about the VERSION and not about an unreadable "
          "fixture", PGWT_SUMMARY_VERSION, ok.n);
    /* Stamp every header down one version in place. */
    int stamped = set_version(x.dir, PGWT_SUMMARY_VERSION - 1);
    struct readback rb3 = { .want_pid = PID_A };
    CHECK(stamped > 0 && read_back(x.dir, &rb3) < 0,
          "BYPASS i (v%d file): the reader REFUSES a file written under the "
          "previous accounting rule instead of summing it as if it were v%d",
          PGWT_SUMMARY_VERSION - 1, PGWT_SUMMARY_VERSION);
    wr_free(&x);

    /* l. A DISABLED WRITER. The one shape in which every loop in this file
     *    iterates zero times. It must FAIL the read-back, not pass. */
    struct wr y;
    if (wr_open(&y, "disabled") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    y.w->enabled = false;
    for (int s = 2; s <= 6; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&y, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    struct readback rb4 = { .want_pid = PID_A };
    CHECK(read_back(y.dir, &rb4) < 0 &&
          y.w->open_cpu_credited_ns_total == 0,
          "BYPASS l (writer disabled): no records and nothing credited, and "
          "the read-back REFUSES rather than agreeing over an empty file");
    wr_free(&y);
}

static void test_bypass_short_gap_and_roundtrip(void)
{
    /* j. A GAP THAT NEVER SPANS A SECOND BOUNDARY. There is nothing to
     *    credit, so the answer must be the v8 one exactly. This is the
     *    input that makes a careless conservation check pass -- 0 credited
     *    plus the whole duration at close equals the whole duration -- while
     *    multi-second placement is broken, which is why §1 asserts the
     *    second COUNT and not just the total. */
    struct wr x;
    if (wr_open(&x, "shortgap") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event seed =
        mkev(PID_A, BASE_MONO + 1 * S, 100 * MS, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(x.w, &seed, 0, seed.old_event);
    /* A 200 ms on-CPU gap wholly inside second 1, noted once. */
    tick_cpu(&x, BASE_MONO + 1 * S + 250 * MS, PID_A, BASE_MONO + 1100 * MS,
             0, 0, 120 * MS);
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 1300 * MS, 200 * MS, 0, EV_LOCK_REL, 0,
             120 * MS);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    uint64_t credited = x.w->open_cpu_credited_ns_total;
    wr_close(&x);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "short gap: records read back (%d)", n);
    if (n > 0)
        CHECK(credited == 0 &&
              rb_sum_class(&rb, PGWT_CLASS_CPU) == 200 * MS &&
              RB_SUM(&rb, cpu_measured_ns) == 120 * MS &&
              RB_SUM(&rb, cpu_clamped_ns) == 0,
              "BYPASS j (sub-second gap): nothing credited (%llu), the v8 "
              "answer exactly — wall %llu ms, measured %llu ms, clamped "
              "%llu ms", (unsigned long long)credited,
              (unsigned long long)(rb_sum_class(&rb, PGWT_CLASS_CPU) / MS),
              (unsigned long long)(RB_SUM(&rb, cpu_measured_ns) / MS),
              (unsigned long long)(RB_SUM(&rb, cpu_clamped_ns) / MS));
    wr_free(&x);

    /* k. A PURE OPEN-CREDIT SECOND MUST SURVIVE THE ROUND TRIP. Such a
     *    second's events[] row for the CPU id (0) has TIME and NO COUNT. If
     *    the serializer skips it while the block header counts it, the
     *    reader walks a count it was handed and the record fails to decode
     *    -- a silently dropped second, not a cosmetic loss. The witness is
     *    a second with class_ns[CPU] but zero events[CPU].count whose
     *    events[] time still matches. */
    struct wr y;
    if (wr_open(&y, "roundtrip") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    struct pgwt_trace_event s2 =
        mkev(PID_A, BASE_MONO + 1 * S, 1 * S, EV_LOCK_REL, 0, 0,
             PGWT_CPU_NS_UNKNOWN);
    pgwt_summary_push_event(y.w, &s2, 0, s2.old_event);
    for (int s = 2; s <= 10; s++) {
        uint64_t now = BASE_MONO + (uint64_t)s * S + 200 * MS;
        tick_cpu(&y, now, PID_A, BASE_MONO + 1 * S, 0, 0,
                 cpu60(now - (BASE_MONO + 1 * S)));
    }
    uint64_t open_records = y.w->open_credit_records_total;
    uint64_t written = y.w->total_records_written;
    wr_close(&y);
    struct readback rb2 = { .want_pid = PID_A };
    int n2 = read_back(y.dir, &rb2);
    CHECK(n2 > 0, "round trip: records read back (%d)", n2);
    CHECK(open_records >= 8,
          "ROUND TRIP: %llu seconds were materialised as open-credit-only "
          "records (nothing closed in them, a gap was open)",
          (unsigned long long)open_records);
    if (n2 > 0) {
        CHECK((uint64_t)n2 == written,
              "ROUND TRIP: every one of the %llu records written decoded "
              "(%d read) — a desynchronised block header would lose whole "
              "seconds here, not a field",
              (unsigned long long)written, n2);
        int pure = 0;
        int pure_time_ok = 1;
        for (int i = 0; i < rb2.n; i++) {
            if (rb2.rows[i].class_ns[PGWT_CLASS_CPU] == 0) continue;
            if (rb2.rows[i].ev_cpu_count != 0) continue;
            pure++;
            if (rb2.rows[i].ev_cpu_total_ns !=
                rb2.rows[i].class_ns[PGWT_CLASS_CPU])
                pure_time_ok = 0;
        }
        CHECK(pure >= 8 && pure_time_ok,
              "ROUND TRIP: %d seconds carry CPU time with NO count, and "
              "events[CPU].total_ns survived the round trip in every one of "
              "them", pure);
    }
    wr_free(&y);
}

int main(void)
{
    printf("=== test_open_cpu_accounting (#424 / summary v%d) ===\n",
           PGWT_SUMMARY_VERSION);
    printf("-- 1/2: placement, the measured split, conservation --\n");
    test_placement_split_conservation();
    printf("-- 2b: coherent per-second CPU observations --\n");
    test_coherence();
    printf("-- 3: either order of (tick, close) --\n");
    test_orders();
    printf("-- 3c: rotation while the gap is open --\n");
    test_rotation_mid_gap();
    printf("-- 4: classification changes before close --\n");
    test_classification_freeze();
    printf("-- 5/6: the unknown tier and a tier flip --\n");
    test_tiers();
    printf("-- 7: non-vacuity (no open CPU => bit-identical) --\n");
    test_non_vacuity();
    printf("-- 8a: bypass — the observation withheld --\n");
    test_bypass_no_observation();
    printf("-- 8b-d: bypass — lifecycle --\n");
    test_bypass_lifecycle();
    printf("-- 8e-g: bypass — refusals --\n");
    test_bypass_refusals();
    printf("-- 8h,i,l: bypass — the reader must refuse --\n");
    test_bypass_reader();
    printf("-- 8j,k: bypass — sub-second gap, and the round trip --\n");
    test_bypass_short_gap_and_roundtrip();

    printf("\n%d checks, %d failures\n", checks, failures);
    printf("=== test_open_cpu_accounting: %s ===\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
