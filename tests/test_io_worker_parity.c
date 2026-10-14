/* test_io_worker_parity.c — RAW-vs-SUMMARY parity for io_worker time (#376, #315)
 *
 * THE INVARIANT
 * ─────────────────────────────────────────────────────────────────────────
 * For the same capture and the same window, pgwt_compute_time_model() (raw)
 * and pgwt_compute_time_model_from_summaries() (aggregate) must report the
 * SAME Idle ms and the SAME DB Time ms. Today they do not, because the two
 * paths disagree about io_workers:
 *
 *   raw      — pgwt_compute_time_model skips a record carrying
 *              PGWT_EVENT_FLAG_IO_WORKER outright (the `continue` in its
 *              per-event loop), so io_worker time is in NEITHER DB Time nor
 *              Idle. src/compute.c states that as the contract, and
 *              docs/AAS_SEMANTICS_DECISION.md is the authority behind it.
 *   summary  — accum_event in src/summary_writer.c has no io_worker test at
 *              all, so an io_worker's Activity:IoWorkerMain lands in
 *              idle_ns and its IO waits land in the DB-Time class totals.
 *
 * This file is the GATE for that fix. It is RED on master by construction and
 * must stay red until the writer agrees with the raw path. It changes nothing
 * in src/.
 *
 * SCOPE — WHAT A GREEN RUN HERE WOULD AND WOULD NOT MEAN
 * ─────────────────────────────────────────────────────────────────────────
 * Deliberately NOT "the summary path equals the raw path on any window".
 * There is a SECOND, independent mechanism underneath: the writer buckets a
 * record WHOLE into the second in which it closes, with no pro-rating, while
 * the raw path clips every record to the window (event_window_ns in
 * src/compute.c). So ANY long record — an io_worker's whole-capture
 * Activity:IoWorkerMain, but equally a multi-minute Client:ClientRead on an
 * idle session — is counted in full by the summary path on a window that
 * holds only part of it. Excluding io_workers removes the LARGEST instance
 * of that, not the mechanism. Section 7 MEASURES the residual with a long
 * non-io_worker idle record and prints it; it is deliberately not folded
 * into the parity assertion, which is scoped to fixtures whose only
 * straddling records are io_worker ones.
 *
 * HOW THE io_worker PROPERTY IS PRODUCED — AND WHY IT IS NOT THE OLD TRAP
 * ─────────────────────────────────────────────────────────────────────────
 * A previous attempt at #315 (recorded in commit 8705412, "WHY #315 IS NOT
 * HERE") passed only because its FIXTURE pre-set PGWT_EVENT_FLAG_IO_WORKER on
 * the events it pushed into the writer. Nothing on the live path sets that
 * bit — not src/event_stream.c, not src/escalation.c — so a
 * `evt->flags & PGWT_EVENT_FLAG_IO_WORKER` guard in the writer is DEAD IN THE
 * DAEMON, and a pre-flagged fixture certifies a fix that does nothing.
 *
 * Here the bit is never written by the test. Both tiers below obtain
 * io_worker-ness the way production obtains it, from the BACKEND REGISTRY:
 *
 *   summary side — events are pushed into the REAL pgwt_summary_push_event()
 *       with flags == 0, exactly as src/event_stream.c pushes them: that push
 *       happens BEFORE the pid's category is resolved
 *       (pgwt_live_pid_cat_flag, src/map_reader.c) in the same function, and
 *       the writer is handed the raw ringbuf record either way.
 *   raw side — the same array is annotated by the REAL pgwt_tag_events()
 *       (src/compute.c) against a pid→flag table, which is what src/server.c
 *       does to every window it materialises. The table's flag comes from the
 *       production resolver pgwt_backend_type_flag() (src/sampler.h) applied
 *       to the backend type parsed out of a backends.jsonl `type` string — the
 *       same string pgwt_backend_type_name() produces and src/server.c's
 *       bm_type_to_cat_flag keys on, and the same type src/cmdline.c's
 *       pgwt_parse_cmdline resolves from "postgres: io worker".
 *
 * So the asymmetry under test is the production asymmetry: the raw path can
 * see the registry, the writer never did. assert_no_preset_flags() makes the
 * old trap STRUCTURALLY IMPOSSIBLE here: if any event handed to the writer
 * carries a category bit, this test REFUSES instead of comparing.
 *
 * TWO TIERS
 * ─────────────────────────────────────────────────────────────────────────
 *   A. UNIT COMPANION (always runs, sections 1-7). A synthetic 130 s stream
 *      through the real writer and the real reader. It is a companion, NOT
 *      the acceptance proof: a synthetic stream can only show that the two
 *      functions disagree about a shape, never that a real PG18 capture
 *      contains that shape.
 *   B. ACCEPTANCE (section 8, runs when PGWT_PARITY_TRACE or argv[1] names a
 *      trace directory). A REAL `--mode full` PG18 `io_method=worker`
 *      capture: its own backends.jsonl registry, its own
 *      daemon-written current.summary, its own current.trace. This is the
 *      tier whose numbers are the evidence for the fix. When the directory is
 *      not supplied the tier says so in one line and asserts nothing; when it
 *      IS supplied and cannot be read, it FAILS — never skips.
 *
 *      IT NEVER RUNS IN ANY GATE, and that has to be said plainly rather than
 *      left to be discovered: tests/run_all.sh and CI invoke this binary with
 *      no argument and no PGWT_PARITY_TRACE, and tests/results/demo_rehearsal/
 *      is gitignored, so box-check and CI exercise tier A only. Tier B is run
 *      BY HAND against a capture on the machine that has one, and its output
 *      is pasted into the PR as evidence (this branch:
 *      tests/results/issue376-measurement/parity-acceptance-{green,red}.txt).
 *      Honest by design -- a 26 MB capture cannot live in the repo and a tier
 *      that silently skipped would be worse -- but it means a regression of
 *      the real-capture numbers is caught by a human re-running it, not by
 *      the gate.
 *
 * WINDOW CASES — WHY ONE WINDOW WOULD HAVE MISSED IT
 * ─────────────────────────────────────────────────────────────────────────
 * The divergence is window-sensitive, because an io_worker emits ONE
 * Activity:IoWorkerMain record per pid that closes only at tracer shutdown.
 * Measured on tests/results/demo_rehearsal/trace (1851 s, PG18,
 * io_method=worker): +23.34% on Idle over the full span, +0.09% on a 900 s
 * window ending one second short of the capture end, +49.97% on the same-size
 * window that includes it. Every window case below is therefore run and
 * reported separately: a single interior window is nearly clean and would
 * have been read as agreement.
 *
 * THE THREE ADVERSARIAL QUESTIONS
 * ─────────────────────────────────────────────────────────────────────────
 *  - What input makes this check pass while the product is broken? A capture
 *    with no io_worker records, or a registry that resolves none: both paths
 *    then agree at the same number and 0 == 0 is satisfied. parity_verdict()
 *    REFUSES unless the window holds a positive io_worker total AND the
 *    registry resolved at least one io_worker pid AND at least one raw record
 *    carries the bit. A pre-flagged stream is refused for the same reason.
 *  - What single component failing makes this hang or skip rather than fail?
 *    the summary reader. pgwt_visit_summaries() over a missing/empty/
 *    unreadable directory, or a window that overlaps nothing, returns zero
 *    records and every summary total reads 0.000 ms, which a naive comparison
 *    reads as "equal". Section 6 drives all of those and requires each to be
 *    REFUSED; each case also shows what the naive comparison would have said.
 *    Nothing here sleeps, retries or polls.
 *  - What depends on timing or ordering? Two things, both pinned. (1) The
 *    writer stamps its file header from CLOCK_REALTIME and the reader treats
 *    a file as covering [header, +1 h], so a stream of small literal
 *    monotonic timestamps converts to a wall time one uptime in the past and
 *    every window query misses the file — the summary side then answers
 *    0.000 ms, which reads as success. The fixture is therefore anchored to
 *    ONE CLOCK_MONOTONIC read and every window is derived from the committed
 *    records, never from a clock. (2) The writer folds a late-arriving event
 *    into the second in progress (#277), so the stream is emitted in strict
 *    timestamp order and that order is asserted before it is pushed.
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "event_reader.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "cmdline.h"
#include "sampler.h"      /* pgwt_backend_type_flag — header-inline */
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

#define CHECK(cond, fmt, ...) do {                                        \
    tests_run++;                                                          \
    if (!(cond)) { tests_failed++;                                        \
        printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); }  \
    else printf("  ok: " fmt "\n", ##__VA_ARGS__);                        \
} while (0)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

/* Every category bit a registry lookup can produce. A fixture carrying ANY of
 * them has pre-resolved what production resolves — the 8705412 trap. */
#define CAT_BITS (PGWT_EVENT_FLAG_IO_WORKER | PGWT_EVENT_FLAG_MAINT \
                  | PGWT_EVENT_FLAG_BACKGROUND)

/* Wait-event ids by CLASS and index, never by name: the name a numeric id
 * resolves to depends on the PostgreSQL event table the host could load, and
 * nothing here depends on the names. Idle-ness is always asked of
 * pgwt_is_idle_event() at run time. */
#define EV_CPU        0u
#define EV_LOCK_A     WEI(PG_WAIT_LOCK, 0)
#define EV_IO_A       WEI(PG_WAIT_IO, 21)
#define EV_CLIENT_0   WEI(PG_WAIT_CLIENT, 0)    /* idle, VISIBLE            */
#define EV_ACTIVITY_4 WEI(PG_WAIT_ACTIVITY, 4)  /* idle AND hidden; this is
                                                 * the class IoWorkerMain is
                                                 * in on PG18 */
#define QID_A 0x1111111111111111ULL

/* ══════════════════════════════════════════════════════════════════════════
 * 0. THE BACKEND REGISTRY — the one place io_worker-ness enters this file
 *
 * Both tiers build their pid→flag table here, from backends.jsonl `type`
 * strings, through the production resolver. Tier B reads the REAL file of a
 * real capture; tier A hands the same reader the same line shape. The test
 * never names PGWT_EVENT_FLAG_IO_WORKER as an input anywhere.
 *
 * The one thing copied rather than called is the jsonl line split:
 * src/server.c's loader is static. The chain it feeds is pinned by
 * assert_registry_chain() below — the `type` string, the enum it maps to and
 * the flag that enum produces — so a rename on either side turns this red
 * rather than silently resolving nothing.
 * ══════════════════════════════════════════════════════════════════════════ */

struct registry {
    struct pgwt_pid_cat *cats;   /* sorted by pid, as src/server.c keeps it */
    int    n;
    int    n_io_workers;         /* how many resolved to an io_worker        */
};

static enum pgwt_backend_type bt_from_type_string(const char *type)
{
    for (int bt = 0; bt <= PGWT_BT_UNKNOWN; bt++)
        if (strcmp(type, pgwt_backend_type_name((enum pgwt_backend_type)bt)) == 0)
            return (enum pgwt_backend_type)bt;
    return PGWT_BT_UNKNOWN;
}

static int pid_cat_cmp(const void *a, const void *b)
{
    uint32_t pa = ((const struct pgwt_pid_cat *)a)->pid;
    uint32_t pb = ((const struct pgwt_pid_cat *)b)->pid;
    return (pa > pb) - (pa < pb);
}

static void registry_add(struct registry *reg, uint32_t pid, const char *type)
{
    struct pgwt_pid_cat *grown = realloc(reg->cats,
                                         (size_t)(reg->n + 1) * sizeof(*grown));
    if (!grown)
        return;
    reg->cats = grown;
    enum pgwt_backend_type bt = bt_from_type_string(type);
    uint32_t flag = pgwt_backend_type_flag(bt);
    reg->cats[reg->n].pid  = pid;
    reg->cats[reg->n].flag = flag;
    reg->n++;
    if (flag & PGWT_EVENT_FLAG_IO_WORKER)
        reg->n_io_workers++;
}

static void registry_seal(struct registry *reg)
{
    if (reg->n > 1)
        qsort(reg->cats, (size_t)reg->n, sizeof(*reg->cats), pid_cat_cmp);
}

static void registry_free(struct registry *reg)
{
    free(reg->cats);
    memset(reg, 0, sizeof(*reg));
}

/* Load <dir>/backends.jsonl, whose lines are {"pid":N,"type":"S"}.
 * Returns the number of entries, or -1 if the file cannot be read or holds no
 * parseable entry — an unreadable registry must REFUSE, never resolve zero
 * io_workers and let the two paths agree. */
static int registry_load_file(struct registry *reg, const char *dir)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    int parsed = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = strstr(line, "\"pid\":");
        const char *t = strstr(line, "\"type\":\"");
        if (!p || !t)
            continue;
        long pid = strtol(p + 6, NULL, 10);
        char type[64];
        t += 8;
        size_t i = 0;
        while (t[i] && t[i] != '"' && i < sizeof(type) - 1) { type[i] = t[i]; i++; }
        type[i] = '\0';
        if (pid <= 0 || i == 0)
            continue;
        registry_add(reg, (uint32_t)pid, type);
        parsed++;
    }
    fclose(f);
    registry_seal(reg);
    return parsed > 0 ? parsed : -1;
}

/* The resolver chain, pinned rather than trusted: if any link renames, the
 * registry silently resolves nothing and every comparison below agrees. */
static void assert_registry_chain(void)
{
    printf("--- 0. the io_worker resolver chain (production functions) ---\n");
    CHECK(strcmp(pgwt_backend_type_name(PGWT_BT_IO_WORKER), "io_worker") == 0,
          "pgwt_backend_type_name(PGWT_BT_IO_WORKER) is the exact "
          "backends.jsonl string src/server.c's bm_type_to_cat_flag keys on "
          "(\"%s\")", pgwt_backend_type_name(PGWT_BT_IO_WORKER));
    CHECK(bt_from_type_string("io_worker") == PGWT_BT_IO_WORKER,
          "that string maps back to PGWT_BT_IO_WORKER -- the type "
          "src/cmdline.c's pgwt_parse_cmdline resolves from "
          "\"postgres: io worker\"");
    CHECK(pgwt_backend_type_flag(PGWT_BT_IO_WORKER)
              == PGWT_EVENT_FLAG_IO_WORKER,
          "pgwt_backend_type_flag(PGWT_BT_IO_WORKER) is "
          "PGWT_EVENT_FLAG_IO_WORKER -- the daemon-side resolver "
          "pgwt_live_pid_cat_flag uses this same function");
    CHECK(pgwt_backend_type_flag(PGWT_BT_CLIENT) == 0,
          "a client backend resolves to NO category flag, so the fixture's "
          "foreground pid is foreground for the same production reason");
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE COMPARISON ITSELF, and every way it can be blind
 * ══════════════════════════════════════════════════════════════════════════ */

enum verdict { V_AGREE, V_DIVERGE, V_REFUSED };

struct parity {
    enum verdict v;
    const char  *refusal;         /* why, when V_REFUSED                    */
    double raw_idle_ms, sum_idle_ms;
    double raw_db_ms,   sum_db_ms;
    double raw_io_worker_ms;      /* cat_ms[PGWT_CAT_IO_WORKER], raw only   */
    int    records;               /* summary records in the window          */
    double idle_div_pct, db_div_pct;
};

static double div_pct(double raw, double sum)
{
    if (raw == 0.0)
        return sum == 0.0 ? 0.0 : 100.0;
    return (sum - raw) / raw * 100.0;
}

struct count_ctx { int records; uint64_t idle_ns; };

static int count_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct count_ctx *c = vctx;
    c->records++;
    c->idle_ns += rec->idle_ns;
    return 0;
}

/* The window overlap of one record, in the same [end - duration, end] shape
 * event_window_ns (src/compute.c, static) uses. This is NOT a compared
 * quantity — it is the non-vacuity measure "does this window hold any
 * io_worker time at all", busy OR idle, so it must not be read off either
 * compute path (cat_ms[PGWT_CAT_IO_WORKER] counts only the BUSY part, and a
 * window holding nothing but an io_worker's idle Activity record would
 * otherwise look empty of io_workers while being exactly the case #376 is
 * about). */
static uint64_t overlap_ns(const struct pgwt_trace_event *ev,
                           uint64_t from_ns, uint64_t to_ns)
{
    uint64_t end   = ev->timestamp_ns;
    uint64_t start = end >= ev->duration_ns ? end - ev->duration_ns : 0;
    if (start < from_ns) start = from_ns;
    if (end > to_ns)     end   = to_ns;
    return end > start ? end - start : 0;
}

/* Run BOTH paths over the same window and judge. The refusals come FIRST:
 * every one of them is a way this gate could report agreement while seeing
 * nothing, and a gate that cannot see must refuse. */
static struct parity parity_run(const char *dir,
                                const struct pgwt_trace_event *pushed,
                                int n_pushed,
                                const struct pgwt_trace_event *tagged,
                                int n_tagged,
                                const struct registry *reg,
                                uint64_t from_ns, uint64_t to_ns)
{
    struct parity p;
    memset(&p, 0, sizeof(p));

    /* R1. the 8705412 trap: nothing handed to the writer may carry a
     * category bit, or a dead `evt->flags & ...` guard in the writer greens
     * this file while the daemon is unchanged. */
    for (int i = 0; i < n_pushed; i++)
        if (pushed[i].flags & CAT_BITS) {
            p.v = V_REFUSED;
            p.refusal = "an event pushed into the writer already carried a "
                        "category flag (the 8705412 pre-flagged-fixture trap)";
            return p;
        }
    /* R2. the registry must have resolved the property under test. */
    if (!reg || reg->n == 0 || reg->n_io_workers == 0) {
        p.v = V_REFUSED;
        p.refusal = "the backend registry resolved no io_worker pid, so both "
                    "paths would agree for want of the thing being checked";
        return p;
    }
    /* R3. there must be raw events at all. */
    if (n_tagged == 0) {
        p.v = V_REFUSED;
        p.refusal = "the raw side holds no events";
        return p;
    }
    /* R4. the production tagging pass must actually have stamped the bit. */
    int flagged = 0;
    for (int i = 0; i < n_tagged; i++)
        if (tagged[i].flags & PGWT_EVENT_FLAG_IO_WORKER) { flagged++; break; }
    if (!flagged) {
        p.v = V_REFUSED;
        p.refusal = "pgwt_tag_events stamped PGWT_EVENT_FLAG_IO_WORKER on no "
                    "record -- the property is ABSENT, not wrong";
        return p;
    }
    /* R5. THE WINDOW MUST HOLD io_worker TIME. A window holding an io_worker
     * pid but none of its time cannot show the divergence, and the two paths
     * then agree for want of the thing under test. Measured off the tagged
     * raw array (see overlap_ns), not off either compute path. */
    uint64_t iow_ns = 0;
    for (int i = 0; i < n_tagged; i++)
        if (tagged[i].flags & PGWT_EVENT_FLAG_IO_WORKER)
            iow_ns += overlap_ns(&tagged[i], from_ns, to_ns);
    if (iow_ns == 0) {
        p.v = V_REFUSED;
        p.refusal = "the window holds 0 ns of io_worker time (busy or idle)";
        return p;
    }
    /* R6. THE PRODUCTION PREFLIGHT. pgwt_visit_summaries SKIPS a file it
     * cannot open or whose version it refuses and returns the records from
     * the rest, so a corrupted or stale file comes back as a plausible
     * PARTIAL window with no error anywhere — a comparison against a partial
     * summary is exactly the false negative this gate must not produce.
     * pgwt_summaries_window_current() is the same check
     * should_use_summaries() runs in src/server.c before choosing this path,
     * and an EMPTY overlap set (considered == 0) is a refusal here even
     * though it is a legitimate 1 for the server: nothing to blend means
     * nothing to compare. */
    static char preflight_msg[256];
    int considered = 0, unusable = 0;
    int current = pgwt_summaries_window_current(dir, from_ns, to_ns,
                                               &considered, &unusable);
    if (current != 1 || considered == 0 || unusable != 0) {
        snprintf(preflight_msg, sizeof(preflight_msg),
                 "the production summary preflight "
                 "(pgwt_summaries_window_current) refuses this window "
                 "(verdict %d, %d files considered, %d unusable): missing, "
                 "unreadable or wrong-version summary files",
                 current, considered, unusable);
        p.v = V_REFUSED;
        p.refusal = preflight_msg;
        return p;
    }
    /* R7. and the reader must actually yield records in THIS window. Zero
     * records is the shape that reads as 0.000 ms == 0.000 ms. */
    struct count_ctx cc;
    memset(&cc, 0, sizeof(cc));
    if (pgwt_visit_summaries(dir, from_ns, to_ns, count_visitor, &cc) < 0
        || cc.records == 0) {
        p.v = V_REFUSED;
        p.refusal = "the summary reader found no records in the window";
        return p;
    }
    p.records = cc.records;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    double wall_ms = (double)(to_ns - from_ns) / 1e6;

    struct pgwt_tm_result raw, sum;
    memset(&raw, 0, sizeof(raw));
    memset(&sum, 0, sizeof(sum));
    pgwt_compute_time_model(tagged, n_tagged, &f, from_ns, to_ns, wall_ms, &raw);
    pgwt_compute_time_model_from_summaries(dir, from_ns, to_ns, &f, wall_ms,
                                           &sum);

    p.raw_idle_ms = raw.idle_time_ms;
    p.sum_idle_ms = sum.idle_time_ms;
    p.raw_db_ms   = raw.db_time_ms;
    p.sum_db_ms   = sum.db_time_ms;
    p.raw_io_worker_ms = raw.cat_ms[PGWT_CAT_IO_WORKER];
    free(raw.rows);
    free(sum.rows);

    /* R8. and both sides must have something to compare. */
    if (p.raw_idle_ms == 0.0 && p.sum_idle_ms == 0.0 &&
        p.raw_db_ms == 0.0 && p.sum_db_ms == 0.0) {
        p.v = V_REFUSED;
        p.refusal = "every total on both sides is zero";
        return p;
    }

    p.idle_div_pct = div_pct(p.raw_idle_ms, p.sum_idle_ms);
    p.db_div_pct   = div_pct(p.raw_db_ms,   p.sum_db_ms);
    p.v = (p.raw_idle_ms == p.sum_idle_ms && p.raw_db_ms == p.sum_db_ms)
        ? V_AGREE : V_DIVERGE;
    return p;
}

static void parity_report(const char *label, const struct parity *p)
{
    if (p->v == V_REFUSED) {
        printf("    %-34s REFUSED: %s\n", label, p->refusal);
        return;
    }
    printf("    %-34s Idle raw %15.3f  summary %15.3f  (%+7.2f%%)\n",
           label, p->raw_idle_ms, p->sum_idle_ms, p->idle_div_pct);
    printf("    %-34s DBTm raw %15.3f  summary %15.3f  (%+7.2f%%)"
           "   [io_worker busy raw %.3f ms, %d records]\n",
           "", p->raw_db_ms, p->sum_db_ms, p->db_div_pct,
           p->raw_io_worker_ms, p->records);
}

/* THE assertion. Exact equality, no epsilon: both numbers are one division of
 * a sum of integer nanoseconds by 1e6, and section 2 asserts every total
 * stays below 2^53 ns so the integer-valued double sums are exact whatever
 * order they are added in. A refusal is a FAILURE, never a pass. */
static void parity_assert(const char *label, const struct parity *p)
{
    CHECK(p->v != V_REFUSED, "%s: the comparison was possible%s%s", label,
          p->v == V_REFUSED ? " -- REFUSED: " : "",
          p->v == V_REFUSED ? p->refusal : "");
    if (p->v == V_REFUSED)
        return;
    CHECK(p->raw_idle_ms == p->sum_idle_ms,
          "%s: Idle agrees (raw %.3f ms vs summary %.3f ms, %+.2f%%)",
          label, p->raw_idle_ms, p->sum_idle_ms, p->idle_div_pct);
    CHECK(p->raw_db_ms == p->sum_db_ms,
          "%s: DB Time agrees (raw %.3f ms vs summary %.3f ms, %+.2f%%)",
          label, p->raw_db_ms, p->sum_db_ms, p->db_div_pct);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 1. TIER A — the synthetic companion stream
 *
 * 130 one-second slices (over should_use_summaries' 120 s floor, so this is
 * the shape of request that really takes the summary path).
 *
 * pid 301 is a client backend: a 5 ms Lock wait (DB Time), a 7 ms on-CPU gap
 * and an 11 ms Client:ClientRead (idle, visible) per second, each placed
 * 500 ms into its second so NOTHING straddles a second boundary and nothing
 * is clipped by any window below. That matters: a straddling non-io_worker
 * record would diverge for the residual reason section 7 measures, and this
 * assertion is scoped to the io_worker mechanism.
 *
 * pids 22527-22529 are io workers (the registry says so; the test never sets
 * a flag). Each emits, per second, a 3 ms IO wait — DB Time on the summary
 * path, excluded on raw — and ONE Activity record spanning the whole capture
 * that closes in the FINAL second, which is the production shape #376
 * describes: an io_worker's Activity:IoWorkerMain closes only at tracer
 * shutdown and the writer buckets it whole into that last second.
 * ══════════════════════════════════════════════════════════════════════════ */

#define SECS 130
#define PID_CLIENT 301u
/* THE OVER-BROAD-EXCLUSION CONTROL (added with the v6 fix). src/compute.c
 * special-cases ONLY PGWT_EVENT_FLAG_IO_WORKER, so a checkpointer's or
 * bgwriter's time stays in raw DB Time and raw Idle. A writer that excluded
 * on `cat_flag != 0` rather than on the io_worker bit would delete every
 * maintenance and background record from the summary side of any >= 120 s
 * window -- and with no such pid in this fixture that mutation left this file
 * at 76/1, the 1 being an unrelated wall-base artefact rather than a
 * detection. These records are counted by BOTH paths, so the mutation now
 * breaks section 3's equality directly. */
#define PID_BG     302u
static const uint32_t IOW_PIDS[] = { 22527u, 22528u, 22529u };
#define N_IOW ((int)(sizeof(IOW_PIDS) / sizeof(IOW_PIDS[0])))
/* The io_workers start at second 65, so the first 65 seconds hold an
 * io_worker-free window. That window is section 6i: a window with no
 * io_worker time must be REFUSED, not reported as agreement. */
#define IOW_FROM_SEC 65

#define EXACT_NS_LIMIT 9007199254740992.0   /* 2^53 ns ~= 104 days */

struct fixture {
    char      dir[600];
    struct pgwt_trace_event *pushed;   /* what went into the writer (mono)  */
    struct pgwt_trace_event *tagged;   /* the same events, wall + tagged    */
    int       n;
    uint64_t  origin;
    uint64_t  first_sec, last_sec;     /* wall, from the committed records  */
    int       records;
    uint64_t  block_events;
    struct registry reg;
};

static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

static void push_ev(struct pgwt_trace_event *out, int *n, uint64_t ts,
                    uint32_t pid, uint32_t ev, uint64_t dur, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;          /* an interval ENDS at its timestamp */
    e.pid          = pid;
    e.old_event    = ev;
    e.new_event    = EV_IO_A;     /* never EXIT, never a marker */
    e.duration_ns  = dur;
    e.query_id     = qid;
    e.cpu_ns       = PGWT_CPU_NS_UNKNOWN;   /* measured-CPU rows are raw-only */
    e.flags        = 0;           /* THE POINT: production pushes no cat bit */
    out[(*n)++] = e;
}

static int ts_cmp(const void *a, const void *b)
{
    uint64_t ta = ((const struct pgwt_trace_event *)a)->timestamp_ns;
    uint64_t tb = ((const struct pgwt_trace_event *)b)->timestamp_ns;
    return (ta > tb) - (ta < tb);
}

/* Build the stream. `long_client_idle_ns` > 0 adds ONE long non-io_worker
 * Client:ClientRead closing in the final second — section 7's residual
 * measurement, zero for the parity fixture. */
static int build_stream(struct pgwt_trace_event *out, uint64_t base,
                        uint64_t long_client_idle_ns)
{
    int n = 0;
    for (int s = 0; s < SECS; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        push_ev(out, &n, sec + 500 * MS, PID_CLIENT, EV_LOCK_A,   5 * MS, QID_A);
        push_ev(out, &n, sec + 501 * MS, PID_CLIENT, EV_CPU,      7 * MS, QID_A);
        push_ev(out, &n, sec + 502 * MS, PID_CLIENT, EV_CLIENT_0, 11 * MS, 0);
        /* The BACKGROUND pid (see PID_BG): one busy Lock and one idle
         * Activity record per second, both of which must survive on BOTH
         * paths -- the control that tells "excluded" from "excluded too
         * much". */
        push_ev(out, &n, sec + 504 * MS, PID_BG, EV_LOCK_A,    4 * MS, 0);
        push_ev(out, &n, sec + 505 * MS, PID_BG, EV_ACTIVITY_4, 9 * MS, 0);
        if (s < IOW_FROM_SEC)
            continue;
        for (int w = 0; w < N_IOW; w++)
            push_ev(out, &n, sec + 503 * MS + (uint64_t)w * MS, IOW_PIDS[w],
                    EV_IO_A, 3 * MS, 0);
    }
    /* The long Activity records, opening when the io_workers start and
     * closing in the FINAL second — the production shape of #376, where an
     * io_worker's Activity:IoWorkerMain closes only at tracer shutdown and
     * the writer buckets it whole into that one second. */
    uint64_t end = base + (uint64_t)(SECS - 1) * ONE_SEC + 900 * MS;
    for (int w = 0; w < N_IOW; w++)
        push_ev(out, &n, end + (uint64_t)w * MS, IOW_PIDS[w], EV_ACTIVITY_4,
                (uint64_t)(SECS - 1 - IOW_FROM_SEC) * ONE_SEC, 0);
    if (long_client_idle_ns)
        push_ev(out, &n, end + 10 * MS, PID_CLIENT, EV_CLIENT_0,
                long_client_idle_ns, 0);
    qsort(out, (size_t)n, sizeof(*out), ts_cmp);   /* #277: emit in order */
    return n;
}

#define MAX_EV (SECS * (5 + N_IOW) + N_IOW + 2)

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

struct rec_ctx { int records; uint64_t events; uint32_t overflow;
                 uint64_t min_sec, max_sec; };

static int rec_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct rec_ctx *c = vctx;
    c->records++;
    /* events[] IS serialized (rec->total_events is an in-memory counter and
     * reads back as 0, so a conservation check on it is vacuous). */
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++)
        if (!(rec->events[e].event_id == 0 && rec->events[e].count == 0))
            c->events += rec->events[e].count;
    c->overflow += rec->events_overflow;
    if (c->min_sec == 0 || rec->second_wall_ns < c->min_sec)
        c->min_sec = rec->second_wall_ns;
    if (rec->second_wall_ns > c->max_sec) c->max_sec = rec->second_wall_ns;
    return 0;
}

/* The category the PRODUCER resolves for this pid, from the registry — the
 * summary-v6 contract (#315/#376): pgwt_summary_push_event takes a cat_flag
 * because evt->flags carries no category bit on any path that feeds the
 * writer, so the writer has to be TOLD. src/event_stream.c resolves with
 * pgwt_live_pid_cat_flag (registry + per-pid cache) and src/escalation.c does
 * the same in its capture-end flush; this is the same lookup against the same
 * table, which is what makes the push below "exactly as the daemon pushes".
 * reg == NULL means "producer resolved nothing", i.e. the PRE-v6 behaviour,
 * which is what section 3b's reachability probe wants. */
static uint32_t producer_cat_flag(const struct registry *reg, uint32_t pid)
{
    if (!reg)
        return 0;
    for (int i = 0; i < reg->n; i++)
        if (reg->cats[i].pid == pid)
            return reg->cats[i].flag;
    return 0;   /* unresolved == foreground, exactly as the daemon answers */
}

/* Push a stream through the REAL writer, checking every push. */
static long write_summaries(const char *dir, const struct pgwt_trace_event *ev,
                            int n, const struct registry *reg)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) return -1;
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) { free(w); return -1; }
    long pushed = 0;
    for (int i = 0; i < n; i++) {
        if (pgwt_summary_push_event(w, &ev[i],
                                    producer_cat_flag(reg, ev[i].pid),
                                    ev[i].old_event) != 0)
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
                         uint64_t long_client_idle_ns)
{
    memset(fx, 0, sizeof(fx[0]));
    snprintf(fx->dir, sizeof(fx->dir), "%s", fresh_dir(dirname));
    fx->origin = mono_origin();
    fx->pushed = calloc(MAX_EV, sizeof(*fx->pushed));
    fx->tagged = calloc(MAX_EV, sizeof(*fx->tagged));
    if (!fx->pushed || !fx->tagged)
        return -1;
    fx->n = build_stream(fx->pushed, fx->origin, long_client_idle_ns);

    /* THE REGISTRY IS BUILT FIRST, because summary v6 (#315/#376) made the
     * producer resolve each pid's category BEFORE the push: the writer takes
     * a cat_flag and never reads evt->flags. Writing the summaries before the
     * registry existed would mean pushing every record as "unresolved
     * foreground" — the pre-v6 behaviour — and the parity assertions would
     * stay red for a reason that is about this fixture rather than about the
     * product. Same file, same reader, same shape as before. */
    {
        char rpath[700];
        snprintf(rpath, sizeof(rpath), "%s/backends.jsonl", fx->dir);
        FILE *rf = fopen(rpath, "w");
        if (!rf)
            return -1;
        fprintf(rf, "{\"pid\":%u,\"type\":\"client\"}\n", PID_CLIENT);
        /* bgwriter -> PGWT_EVENT_FLAG_BACKGROUND via pgwt_backend_type_flag:
         * a non-zero category that must NOT be excluded anywhere. */
        fprintf(rf, "{\"pid\":%u,\"type\":\"bgwriter\"}\n", PID_BG);
        for (int w = 0; w < N_IOW; w++)
            fprintf(rf, "{\"pid\":%u,\"type\":\"io_worker\"}\n", IOW_PIDS[w]);
        fclose(rf);
        if (registry_load_file(&fx->reg, fx->dir) < 0)
            return -1;
    }

    if (write_summaries(fx->dir, fx->pushed, fx->n, &fx->reg) != fx->n)
        return -1;

    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    if (pgwt_visit_summaries(fx->dir, 0, 0, rec_visitor, &rc) < 0
        || rc.records == 0)
        return -1;
    fx->records      = rc.records;
    fx->block_events = rc.events;
    fx->first_sec    = rc.min_sec;
    fx->last_sec     = rc.max_sec;

    /* (The registry was built and loaded above, before the push — see there.)
     *
     * The raw side: the same events in the writer's wall base, annotated by
     * the REAL pgwt_tag_events against the registry — what src/server.c does
     * to every window it materialises. The writer's mono→wall offset is one
     * constant captured when it opened the file, so recovering it from the
     * records is exact and costs no second clock read. */
    uint64_t shift = fx->first_sec - fx->origin;
    memcpy(fx->tagged, fx->pushed, (size_t)fx->n * sizeof(*fx->tagged));
    for (int i = 0; i < fx->n; i++)
        fx->tagged[i].timestamp_ns += shift;
    /* #363: a REFUSED tagging pass (working-allocation failure, or more
     * distinct pids than its per-pid table holds) leaves this array PARTLY
     * rewritten, and this array is the whole raw side of every comparison
     * below. Consuming it would make the gate print a confident agreement or
     * divergence computed from a half-built oracle -- the silent-wrong-answer
     * class this file exists to catch, inside the file itself. The fixture
     * cannot be built from it, so fail the build of the fixture. */
    if (!pgwt_tag_events(fx->tagged, fx->n, fx->reg.cats, fx->reg.n))
        return -1;
    return 0;
}

static void fixture_free(struct fixture *fx)
{
    free(fx->pushed); free(fx->tagged);
    registry_free(&fx->reg);
    fx->pushed = fx->tagged = NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 2. NON-VACUITY AND THE PREMISES
 * ══════════════════════════════════════════════════════════════════════════ */

static void assert_fixture_live(const struct fixture *fx)
{
    printf("--- 2. the companion fixture is complete, in order and "
           "un-pre-flagged ---\n");
    CHECK(fx->records == SECS, "every second was committed (%d == %d)",
          fx->records, SECS);
    /* CONSERVATION, restated for summary v6 (#315/#376) and deliberately NOT
     * weakened to "some events arrived". The writer now drops an io_worker
     * record on purpose — that IS the fix — so the committed blocks hold
     * every pushed event EXCEPT those, and the two counts must add up:
     *
     *     committed + io_worker pushed == pushed
     *
     * Both terms are required to be non-zero, so neither "nothing arrived"
     * nor "nothing was excluded" can satisfy it. The two sides are still the
     * same events: the raw path drops exactly the same records (every
     * pgwt_compute_* skips PGWT_EVENT_FLAG_IO_WORKER), which is the parity
     * this file asserts in section 3. */
    int iow_pushed = 0;
    for (int i = 0; i < fx->n; i++)
        for (int w = 0; w < N_IOW; w++)
            if (fx->pushed[i].pid == IOW_PIDS[w]) { iow_pushed++; break; }
    CHECK(iow_pushed > 0,
          "the fixture pushed io_worker records (%d) -- 0 would make every "
          "comparison below vacuous", iow_pushed);
    CHECK(fx->block_events + (uint64_t)iow_pushed == (uint64_t)fx->n
          && fx->block_events > 0,
          "the committed blocks hold every pushed NON-io_worker event "
          "(%llu + %d == %d): the io_worker records are excluded at the "
          "writer (summary v6), nothing else is lost, and the two sides are "
          "still the same events rather than two samples",
          (unsigned long long)fx->block_events, iow_pushed, fx->n);

    int preset = 0;
    for (int i = 0; i < fx->n; i++)
        if (fx->pushed[i].flags & CAT_BITS) preset++;
    CHECK(preset == 0,
          "NO event handed to the writer carries a category flag (%d): a "
          "`evt->flags & PGWT_EVENT_FLAG_IO_WORKER` guard in the writer is "
          "dead in the daemon, so a pre-flagged fixture could certify a fix "
          "that does nothing (commit 8705412)", preset);

    int ordered = 1;
    for (int i = 1; i < fx->n; i++)
        if (fx->pushed[i - 1].timestamp_ns > fx->pushed[i].timestamp_ns)
            ordered = 0;
    CHECK(ordered,
          "the stream is pushed in strict timestamp order, so the writer's "
          "late-arrival rule (#277) cannot fold a record into another second");

    /* The bit exists on the raw side ONLY because pgwt_tag_events put it
     * there, from the registry. Both halves are checked: the io_worker pids
     * carry it and the client pid does not. */
    int iow_tagged = 0, client_tagged = 0;
    for (int i = 0; i < fx->n; i++) {
        int is_iow = 0;
        for (int w = 0; w < N_IOW; w++)
            if (fx->tagged[i].pid == IOW_PIDS[w]) is_iow = 1;
        if (fx->tagged[i].flags & PGWT_EVENT_FLAG_IO_WORKER) {
            if (is_iow) iow_tagged++; else client_tagged++;
        }
    }
    CHECK(iow_tagged == (SECS - IOW_FROM_SEC) * N_IOW + N_IOW,
          "pgwt_tag_events stamped PGWT_EVENT_FLAG_IO_WORKER on every record "
          "of the registry's io_worker pids (%d) -- the production resolver "
          "produced the property, not the fixture", iow_tagged);
    CHECK(client_tagged == 0,
          "and on none of the client backend's records (%d)", client_tagged);

    /* THE OVER-BROAD-EXCLUSION CONTROL (see PID_BG). The background pid's
     * records must be present on BOTH sides and carry NO io_worker bit: the
     * raw path keeps background time in DB Time and Idle, so a writer that
     * excluded every non-zero category would break section 3's equality.
     * Asserted here as a premise, so the detection is named rather than
     * incidental. */
    int bg_pushed = 0, bg_tagged_iow = 0, bg_in_blocks;
    for (int i = 0; i < fx->n; i++) {
        if (fx->pushed[i].pid != PID_BG) continue;
        bg_pushed++;
        if (fx->tagged[i].flags & PGWT_EVENT_FLAG_IO_WORKER) bg_tagged_iow++;
    }
    bg_in_blocks = bg_pushed;   /* every non-io_worker record is committed */
    CHECK(bg_pushed == SECS * 2 && bg_tagged_iow == 0,
          "the fixture carries a BACKGROUND pid (%d records, %d wrongly "
          "flagged io_worker) whose time both paths must KEEP -- without it, "
          "a writer excluding on `cat_flag != 0` instead of the io_worker bit "
          "deletes all maintenance/background time from every >= 120 s window "
          "and nothing here notices", bg_pushed, bg_tagged_iow);
    CHECK(fx->block_events >= (uint64_t)bg_in_blocks,
          "and the committed blocks are large enough to hold them (%llu >= "
          "%d)", (unsigned long long)fx->block_events, bg_in_blocks);
    CHECK(fx->reg.n_io_workers == N_IOW,
          "the registry resolved %d io_worker pids through "
          "pgwt_backend_type_flag", fx->reg.n_io_workers);

    /* The exactness premise behind `==`. */
    double total_ns = 0;
    for (int i = 0; i < fx->n; i++)
        total_ns += (double)fx->pushed[i].duration_ns;
    CHECK(total_ns < EXACT_NS_LIMIT,
          "total accumulated ns (%.0f) stays below 2^53, so integer-valued "
          "double sums are exact regardless of addition order -- which is "
          "why the assertion is `==` and not a tolerance", total_ns);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3. THE WINDOW CASES
 *
 * Three windows, because the divergence is window-sensitive and one window
 * would have missed it. Each is derived from the COMMITTED RECORDS, never
 * from a clock.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_windows(const struct fixture *fx)
{
    printf("--- 3. raw vs summary Idle and DB Time, three windows ---\n");
    struct { const char *label; uint64_t from, to; } w[] = {
        /* The final second EXCLUDED: the whole-capture Activity records close
         * in it, so this is the near-clean window a single-window test would
         * have called agreement. */
        { "A. excludes the final second", fx->first_sec, fx->last_sec },
        /* The same width, INCLUDING it. */
        { "B. 61 s ending AT capture end", fx->last_sec - 60 * ONE_SEC,
          fx->last_sec + ONE_SEC },
        /* The full span. */
        { "C. full span", fx->first_sec, fx->last_sec + ONE_SEC },
    };
    for (int i = 0; i < 3; i++) {
        struct parity p = parity_run(fx->dir, fx->pushed, fx->n, fx->tagged,
                                     fx->n, &fx->reg, w[i].from, w[i].to);
        parity_report(w[i].label, &p);
        parity_assert(w[i].label, &p);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3b. THE GATE IS SATISFIABLE — what a FIXED writer would commit
 *
 * A gate that can only ever be red is not a gate, it is a tombstone. This
 * section shows that the intended fix closes it: the same stream is pushed
 * through the same real writer with the io_worker pids' records WITHHELD —
 * which is what accum_event would accumulate if it resolved the pid's
 * category the way src/compute.c does — and the two paths are then compared
 * over the same three windows.
 *
 * This simulates the fix OUTSIDE src/ on purpose: the real change has to
 * resolve the category at the writer (the summary push in
 * src/event_stream.c runs before pgwt_live_pid_cat_flag, and that resolver
 * returns 0 for a pid whose metadata is not parsed yet), and the fix
 * direction is not this test's business. What IS this test's business is
 * that the assertion is reachable.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_fix_is_reachable(const struct fixture *fx)
{
    printf("--- 3b. the assertion is REACHABLE: a writer that withholds "
           "io_worker records ---\n");
    char dir[700];
    snprintf(dir, sizeof(dir), "%s", fresh_dir("fixed_writer"));
    struct pgwt_trace_event *kept = calloc((size_t)fx->n, sizeof(*kept));
    if (!kept) { CHECK(0, "allocation"); return; }
    int m = 0, dropped = 0;
    for (int i = 0; i < fx->n; i++) {
        int is_iow = 0;
        for (int w = 0; w < N_IOW; w++)
            if (fx->pushed[i].pid == IOW_PIDS[w]) is_iow = 1;
        if (is_iow) { dropped++; continue; }
        kept[m++] = fx->pushed[i];
    }
    CHECK(dropped > 0 && m > 0,
          "the simulated fix withholds %d io_worker records and keeps %d",
          dropped, m);
    /* reg = NULL on purpose: the io_worker records are withheld by the CALLER
     * here, so this probe is independent of the producer-side resolve. It
     * still shows the assertion is reachable — and it now shows it a second
     * way, since the real v6 writer reaches the same record set by being told
     * the category instead. */
    if (write_summaries(dir, kept, m, NULL) != m) {
        CHECK(0, "the simulated-fix writer accepted every record");
        free(kept);
        return;
    }
    /* The wall base is this writer's own, so take the window from ITS
     * records and shift the raw copy by the same constant. */
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    if (pgwt_visit_summaries(dir, 0, 0, rec_visitor, &rc) < 0
        || rc.records == 0) {
        CHECK(0, "the simulated-fix writer committed records");
        free(kept);
        return;
    }
    struct pgwt_trace_event *tagged = calloc((size_t)fx->n, sizeof(*tagged));
    if (!tagged) { free(kept); CHECK(0, "allocation"); return; }
    memcpy(tagged, fx->pushed, (size_t)fx->n * sizeof(*tagged));
    uint64_t shift = rc.min_sec - fx->origin;
    for (int i = 0; i < fx->n; i++)
        tagged[i].timestamp_ns += shift;
    /* #363: a refused pass leaves a partly-rewritten oracle; discard it
     * rather than comparing against it (see fixture_build). */
    if (!pgwt_tag_events(tagged, fx->n, fx->reg.cats, fx->reg.n)) {
        CHECK(0, "pgwt_tag_events REFUSED the simulated-fix oracle (#363): "
                 "the raw side is partly rewritten and must be discarded, "
                 "so this reachability probe cannot be run");
        free(kept); free(tagged);
        return;
    }

    struct { const char *label; uint64_t from, to; } w[] = {
        { "A'. excludes the final second", rc.min_sec, rc.max_sec },
        { "B'. 61 s ending AT capture end", rc.max_sec - 60 * ONE_SEC,
          rc.max_sec + ONE_SEC },
        { "C'. full span", rc.min_sec, rc.max_sec + ONE_SEC },
    };
    for (int i = 0; i < 3; i++) {
        struct parity p = parity_run(dir, kept, m, tagged, fx->n, &fx->reg,
                                     w[i].from, w[i].to);
        parity_report(w[i].label, &p);
        parity_assert(w[i].label, &p);
    }
    free(kept);
    free(tagged);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 4. THE HEATMAP ROUTES — the one raw aggregate that still counts io_workers
 *
 * handle_heatmap routes BOTH ways (summary >= 120 s, raw below), and
 * pgwt_compute_heatmap does NOT exclude io_worker records, so today the two
 * agree BY COUNTING THEM. The first attempt at #315 excluded io_workers from
 * pgwt_compute_heatmap while its writer guard was dead, and broke that
 * agreement: on PG18, where io_method=worker is the default, a >= 120 s and a
 * < 120 s window would have returned different latency grids.
 *
 * Note that the summary side's grid comes from events[].histogram in the
 * per-second record, so whatever a fix excludes from events[] governs this
 * route too: the two changes have to land together. This section is GREEN on
 * master and is here to stay green.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_heatmap_routes(const struct fixture *fx)
{
    printf("--- 4. handle_heatmap's two routes agree cell by cell ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    uint64_t from = fx->first_sec, to = fx->last_sec + ONE_SEC;
    struct pgwt_heatmap_result raw, sum;
    memset(&raw, 0, sizeof(raw));
    memset(&sum, 0, sizeof(sum));
    /* SECS buckets over a SECS-second window: one bucket per second. The
     * summary route can only place a record in the bucket its whole second
     * falls in, so a bucket width that is not a whole number of seconds
     * splits a second between buckets on the raw route and not on the
     * summary one — a grid disagreement that is bucket arithmetic, not
     * io_worker accounting. Measured: 70 of 960 cells differ at 60 buckets
     * over 130 s. That belongs in a bucket-alignment test, not here. */
    pgwt_compute_heatmap(fx->tagged, fx->n, &f, from, to, SECS, &raw);
    pgwt_compute_heatmap_from_summaries(fx->dir, from, to, &f, SECS, &sum);

    CHECK(raw.total_events > 0 && sum.total_events > 0,
          "both routes have events to compare (raw %llu, summary %llu) -- a "
          "zero grid would satisfy every cell comparison below",
          (unsigned long long)raw.total_events,
          (unsigned long long)sum.total_events);
    CHECK(raw.num_buckets == sum.num_buckets && raw.num_buckets > 0,
          "same grid shape (%d == %d buckets)", raw.num_buckets,
          sum.num_buckets);
    int cells = 0, mismatch = 0;
    if (raw.num_buckets == sum.num_buckets && raw.grid && sum.grid) {
        for (int b = 0; b < raw.num_buckets * HISTOGRAM_BUCKETS; b++) {
            cells++;
            if (raw.grid[b] != sum.grid[b]) mismatch++;
        }
    }
    CHECK(cells > 0 && mismatch == 0,
          "every one of %d latency cells matches (%d mismatched): if a fix "
          "excludes io_workers from events[]/the writer it must exclude them "
          "from pgwt_compute_heatmap in the SAME commit, or a >= 120 s and a "
          "< 120 s window report different grids", cells, mismatch);
    CHECK(raw.total_events == sum.total_events,
          "and the two routes count the same events (%llu == %llu)",
          (unsigned long long)raw.total_events,
          (unsigned long long)sum.total_events);
    free(raw.grid); free(raw.times); free(sum.grid); free(sum.times);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 5. MUTATION PROBES — can the comparator see one nanosecond?
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_mutation_probes(const struct fixture *fx)
{
    printf("--- 5. the comparator sees a one-nanosecond difference ---\n");
    struct parity p = parity_run(fx->dir, fx->pushed, fx->n, fx->tagged, fx->n,
                                 &fx->reg, fx->first_sec,
                                 fx->last_sec + ONE_SEC);
    CHECK(p.v != V_REFUSED, "the probe has a real comparison to perturb");
    if (p.v == V_REFUSED)
        return;
    /* Force agreement, then move one side by 1 ns and 1 ns only. */
    struct parity q = p;
    q.sum_idle_ms = q.raw_idle_ms;
    q.sum_db_ms   = q.raw_db_ms;
    CHECK(q.raw_idle_ms == q.sum_idle_ms && q.raw_db_ms == q.sum_db_ms,
          "an agreeing pair is judged as agreement");
    q.sum_idle_ms = q.raw_idle_ms + 1e-6;     /* one nanosecond, in ms */
    CHECK(!(q.raw_idle_ms == q.sum_idle_ms),
          "one nanosecond of Idle (%.9f ms) is a disagreement -- no epsilon "
          "anywhere in this file", 1e-6);
    q.sum_idle_ms = q.raw_idle_ms;
    q.sum_db_ms   = q.raw_db_ms + 1e-6;
    CHECK(!(q.raw_db_ms == q.sum_db_ms),
          "one nanosecond of DB Time is a disagreement");
}

/* ══════════════════════════════════════════════════════════════════════════
 * 6. THE BYPASS SUITE — every way THIS gate's detection can be made
 *    unreachable. Each case must come back REFUSED, and each one also shows
 *    what the naive "are the two numbers equal" comparison would have said.
 * ══════════════════════════════════════════════════════════════════════════ */

static void expect_refused(const char *what, const struct parity *p)
{
    CHECK(p->v == V_REFUSED, "%s is REFUSED%s%s", what,
          p->v == V_REFUSED ? ": " : " -- but it was not, verdict=",
          p->v == V_REFUSED ? p->refusal
                            : (p->v == V_AGREE ? "AGREE" : "DIVERGE"));
}

static void bypass_suite(const struct fixture *fx)
{
    printf("--- 6. the bypass suite: a gate that cannot see must refuse ---\n");

    /* 6a. an EMPTY summary directory: the reader finds no records and every
     * summary total is 0.000 ms. Shown first WITHOUT the refusal, because
     * that is the false negative this suite exists for. */
    const char *empty = fresh_dir("bypass_empty");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result naive;
    memset(&naive, 0, sizeof(naive));
    pgwt_compute_time_model_from_summaries(empty, fx->first_sec,
                                           fx->last_sec + ONE_SEC, &f,
                                           1000.0, &naive);
    CHECK(naive.idle_time_ms == 0.0 && naive.db_time_ms == 0.0,
          "an empty summary dir answers Idle %.3f / DB Time %.3f ms -- which "
          "a comparison against an equally empty raw side would read as "
          "AGREEMENT", naive.idle_time_ms, naive.db_time_ms);
    free(naive.rows);
    struct parity p = parity_run(empty, fx->pushed, fx->n, fx->tagged, fx->n,
                                 &fx->reg, fx->first_sec,
                                 fx->last_sec + ONE_SEC);
    expect_refused("6a. an empty summary directory", &p);

    /* 6b. a MISSING directory. */
    char gone[700];
    snprintf(gone, sizeof(gone), "%s/bypass_missing_never_created", g_base);
    p = parity_run(gone, fx->pushed, fx->n, fx->tagged, fx->n, &fx->reg,
                   fx->first_sec, fx->last_sec + ONE_SEC);
    expect_refused("6b. a missing summary directory", &p);

    /* 6c. a window that overlaps NO record. */
    p = parity_run(fx->dir, fx->pushed, fx->n, fx->tagged, fx->n, &fx->reg,
                   fx->last_sec + 3600 * ONE_SEC,
                   fx->last_sec + 3700 * ONE_SEC);
    expect_refused("6c. a window overlapping no record", &p);

    /* 6d. a summary file whose HEADER cannot be parsed. The production
     * preflight is what catches this; pgwt_visit_summaries on its own SKIPS
     * such a file and answers the window from whatever else it finds. */
    char src[700], dst[700], cmd[1600];
    snprintf(src, sizeof(src), "%s/current.summary", fx->dir);
    {
        char broken[700];
        snprintf(broken, sizeof(broken), "%s", fresh_dir("bypass_nohdr"));
        snprintf(dst, sizeof(dst), "%s/current.summary", broken);
        snprintf(cmd, sizeof(cmd),
                 "cp '%s' '%s' 2>/dev/null && : > '%s'", src, dst, dst);
        if (system(cmd) != 0) { /* reported by the refusal below */ }
        p = parity_run(broken, fx->pushed, fx->n, fx->tagged, fx->n, &fx->reg,
                       fx->first_sec, fx->last_sec + ONE_SEC);
        expect_refused("6d. a summary file with no readable header", &p);
    }
    /* 6d'. a PARTIAL commit range: header intact, the second half of the
     * blocks truncated away. The preflight cannot see this (the header is
     * fine), and no reader-side check can: a missing block is
     * indistinguishable from a second with no events. What IS pinned is the
     * direction — a short read can only make the summary side SMALLER, so it
     * cannot manufacture agreement with a complete raw side. The requirement
     * is therefore "never AGREE", which is the only safe verdict available. */
    {
        char part[700];
        snprintf(part, sizeof(part), "%s", fresh_dir("bypass_partial"));
        snprintf(dst, sizeof(dst), "%s/current.summary", part);
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s' 2>/dev/null", src, dst);
        if (system(cmd) != 0) { /* the verdict below reports it */ }
        struct stat st;
        if (stat(dst, &st) == 0 && st.st_size > 2048) {
            if (truncate(dst, st.st_size / 2) != 0) { /* same */ }
        }
        p = parity_run(part, fx->pushed, fx->n, fx->tagged, fx->n, &fx->reg,
                       fx->first_sec, fx->last_sec + ONE_SEC);
        CHECK(p.v != V_AGREE,
              "6d'. a summary file truncated to half its blocks is never read "
              "as AGREEMENT (verdict %s%s): a partial read can only shrink "
              "the summary side",
              p.v == V_REFUSED ? "REFUSED: " : "DIVERGE",
              p.v == V_REFUSED ? p.refusal : "");
    }

    /* 6e. NO REGISTRY — the case in which this gate's verdict is meaningless
     * whichever way it falls, which is why it must REFUSE rather than report.
     *
     * Before summary v6 the failure was a false AGREE: with cats absent the
     * raw path cannot know a pid is an io_worker, so it counted io_worker
     * time exactly as the writer did and the two sides matched — a gate that
     * cannot resolve its base approving itself.
     *
     * After v6 the same loss falls the other way: the summary records were
     * written by a producer that DID resolve the category, so they already
     * exclude io_worker time, while an unregistered raw side counts it. The
     * verdict becomes a false DIVERGE. Pinned below in that direction, with
     * the magnitude, so the probe keeps demonstrating "cannot see" instead of
     * silently becoming an assertion about nothing — and 6e still requires
     * the refusal. */
    struct pgwt_trace_event *untagged = calloc((size_t)fx->n,
                                               sizeof(*untagged));
    struct registry none;
    memset(&none, 0, sizeof(none));
    if (untagged) {
        memcpy(untagged, fx->pushed, (size_t)fx->n * sizeof(*untagged));
        uint64_t shift = fx->first_sec - fx->origin;
        for (int i = 0; i < fx->n; i++)
            untagged[i].timestamp_ns += shift;
        /* No registry at all. #363: still check the result -- with cats
         * absent the pass has nothing to look up, so it can only refuse on
         * its own working allocation, and a partly-rewritten array would
         * make the magnitude printed below meaningless. */
        int tag_ok = pgwt_tag_events(untagged, fx->n, NULL, 0);
        CHECK(tag_ok,
              "the registry-less tagging pass completed (#363): a refusal "
              "leaves the array partly rewritten, so the numbers below would "
              "describe a half-built oracle");
        if (!tag_ok) {
            free(untagged);
            goto skip_no_registry;
        }
        struct pgwt_tm_result r0, s0;
        memset(&r0, 0, sizeof(r0)); memset(&s0, 0, sizeof(s0));
        double wall = (double)(fx->last_sec + ONE_SEC - fx->first_sec) / 1e6;
        pgwt_compute_time_model(untagged, fx->n, &f, fx->first_sec,
                                fx->last_sec + ONE_SEC, wall, &r0);
        pgwt_compute_time_model_from_summaries(fx->dir, fx->first_sec,
                                               fx->last_sec + ONE_SEC, &f,
                                               wall, &s0);
        CHECK(r0.idle_time_ms > s0.idle_time_ms,
              "with NO registry the UNREGISTERED RAW side counts io_worker "
              "idle the v6 summary already excluded (%.3f vs %.3f ms), so the "
              "verdict is a false DIVERGE -- pre-v6 the same loss produced a "
              "false AGREE. Either way an unresolvable base makes this gate's "
              "answer meaningless, which is why it must refuse",
              r0.idle_time_ms, s0.idle_time_ms);
        free(r0.rows); free(s0.rows);
        p = parity_run(fx->dir, fx->pushed, fx->n, untagged, fx->n, &none,
                       fx->first_sec, fx->last_sec + ONE_SEC);
        expect_refused("6e. an absent/unresolvable registry", &p);
        /* 6e'. a registry that loads but resolves no io_worker: same shape,
         * reached by a renamed type string. */
        struct registry fg;
        memset(&fg, 0, sizeof(fg));
        registry_add(&fg, PID_CLIENT, "client");
        registry_add(&fg, IOW_PIDS[0], "io-worker");   /* not the real name */
        registry_seal(&fg);
        p = parity_run(fx->dir, fx->pushed, fx->n, fx->tagged, fx->n, &fg,
                       fx->first_sec, fx->last_sec + ONE_SEC);
        expect_refused("6e'. a registry resolving no io_worker", &p);
        registry_free(&fg);
        free(untagged);
    }
skip_no_registry:

    /* 6f. the THING IS ABSENT rather than wrong: a capture with no io_worker
     * records at all. Both paths agree, and that agreement means nothing. */
    struct pgwt_trace_event *noiow = calloc((size_t)fx->n, sizeof(*noiow));
    if (noiow) {
        int m = 0;
        for (int i = 0; i < fx->n; i++) {
            int is_iow = 0;
            for (int w = 0; w < N_IOW; w++)
                if (fx->tagged[i].pid == IOW_PIDS[w]) is_iow = 1;
            if (!is_iow) noiow[m++] = fx->tagged[i];
        }
        p = parity_run(fx->dir, fx->pushed, fx->n, noiow, m, &fx->reg,
                       fx->first_sec, fx->last_sec + ONE_SEC);
        expect_refused("6f. a raw side with no io_worker record", &p);
        free(noiow);
    }

    /* 6g. the 8705412 TRAP: a fixture that pre-sets the bit on what it pushes
     * into the writer. Such a stream can green a writer guard that is dead in
     * the daemon, so it is refused outright. */
    struct pgwt_trace_event *preflag = calloc((size_t)fx->n,
                                              sizeof(*preflag));
    if (preflag) {
        memcpy(preflag, fx->pushed, (size_t)fx->n * sizeof(*preflag));
        preflag[0].flags |= PGWT_EVENT_FLAG_IO_WORKER;
        p = parity_run(fx->dir, preflag, fx->n, fx->tagged, fx->n, &fx->reg,
                       fx->first_sec, fx->last_sec + ONE_SEC);
        expect_refused("6g. a pre-flagged fixture", &p);
        free(preflag);
    }

    /* 6h. an EMPTY raw side. */
    p = parity_run(fx->dir, fx->pushed, fx->n, fx->tagged, 0, &fx->reg,
                   fx->first_sec, fx->last_sec + ONE_SEC);
    expect_refused("6h. an empty raw side", &p);

    /* 6i. a window holding an io_worker PID in the registry but none of its
     * TIME — the fixture's first IOW_FROM_SEC seconds. Both paths agree
     * there (the client's events are accounted identically), which is the
     * false negative: a nearly-clean window read as proof of parity. */
    uint64_t q_from = fx->first_sec;
    uint64_t q_to   = fx->first_sec + (uint64_t)(IOW_FROM_SEC - 1) * ONE_SEC;
    struct pgwt_tm_result r1, s1;
    memset(&r1, 0, sizeof(r1)); memset(&s1, 0, sizeof(s1));
    double w1 = (double)(q_to - q_from) / 1e6;
    pgwt_compute_time_model(fx->tagged, fx->n, &f, q_from, q_to, w1, &r1);
    pgwt_compute_time_model_from_summaries(fx->dir, q_from, q_to, &f, w1, &s1);
    CHECK(r1.idle_time_ms == s1.idle_time_ms && r1.db_time_ms == s1.db_time_ms,
          "an io_worker-free window AGREES (Idle %.3f == %.3f, DB Time "
          "%.3f == %.3f) -- which is why it must be refused rather than "
          "counted as parity", r1.idle_time_ms, s1.idle_time_ms,
          r1.db_time_ms, s1.db_time_ms);
    free(r1.rows); free(s1.rows);
    p = parity_run(fx->dir, fx->pushed, fx->n, fx->tagged, fx->n, &fx->reg,
                   q_from, q_to);
    expect_refused("6i. a window with 0 ns of io_worker time", &p);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 7. THE RESIDUAL, MEASURED AND NOT ASSERTED
 *
 * The writer buckets a record whole into its closing second; the raw path
 * clips it to the window. Excluding io_workers removes the largest instance
 * of that, not the mechanism. Here is the same mechanism with a long
 * NON-io_worker idle record (a Client:ClientRead on an idle session), on a
 * window that holds only part of it. Reported as a number so a green section
 * 3 is not mistaken for "the summary path equals the raw path on any window".
 * ══════════════════════════════════════════════════════════════════════════ */

static void measure_residual_straddler(void)
{
    printf("--- 7. the RESIDUAL straddler mechanism (measured, not "
           "asserted) ---\n");
    struct fixture fx;
    if (fixture_build(&fx, "residual", 100 * ONE_SEC) != 0) {
        CHECK(0, "the residual fixture could not be built");
        return;
    }
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    /* A 10 s window ending at capture end: it holds 10 s of the 100 s
     * Client:ClientRead, and the client is NOT an io_worker, so no io_worker
     * fix can touch this. */
    uint64_t from = fx.last_sec - 9 * ONE_SEC, to = fx.last_sec + ONE_SEC;
    double wall = (double)(to - from) / 1e6;
    struct pgwt_tm_result raw, sum;
    memset(&raw, 0, sizeof(raw)); memset(&sum, 0, sizeof(sum));
    pgwt_compute_time_model(fx.tagged, fx.n, &f, from, to, wall, &raw);
    pgwt_compute_time_model_from_summaries(fx.dir, from, to, &f, wall, &sum);
    printf("    a 100 s Client:ClientRead closing in the final second, "
           "10 s window:\n"
           "      Idle raw %.3f ms, summary %.3f ms (%+.2f%%) -- the writer "
           "buckets the record WHOLE,\n"
           "      the raw path clips it. Independent of io_workers; NOT "
           "covered by section 3.\n",
           raw.idle_time_ms, sum.idle_time_ms,
           div_pct(raw.idle_time_ms, sum.idle_time_ms));
    CHECK(raw.idle_time_ms > 0.0 && sum.idle_time_ms > 0.0,
          "the residual measurement is non-vacuous (both sides positive)");
    free(raw.rows); free(sum.rows);
    fixture_free(&fx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 8. TIER B — THE ACCEPTANCE TIER: a real PG18 io_method=worker capture
 *
 * A synthetic stream cannot establish this fix; only a real capture can show
 * the shape is really there and how large it is. Point this at a trace
 * directory holding current.trace, current.summary and backends.jsonl
 * (tests/results/demo_rehearsal/trace is such a capture):
 *
 *   ./test_io_worker_parity <trace_dir>        or PGWT_PARITY_TRACE=<dir>
 *
 * The raw side is built the way src/server.c builds a window: decode every
 * block, require timestamp order, then pgwt_tag_events against the capture's
 * own registry. Two simplifications of that loader are stated rather than
 * hidden: it does not merge multiple generations/files (a single-file capture
 * has one), and it does not apply the sampled-tier phantom-EXIT drop (a
 * `--mode full` capture has no SAMPLES blocks — asserted below, so a sampled
 * capture is refused instead of silently mis-compared).
 *
 * THE SUMMARY SIDE IS RE-DERIVED, AND HERE IS WHY. The summary format carries
 * a version that records which ACCOUNTING RULE its precomputed totals were
 * written under, and this build computes only from PGWT_SUMMARY_VERSION; an
 * older file is refused by the reader on purpose (that refusal is itself a
 * shipped feature — should_use_summaries preflights it and recomputes from
 * raw). tests/results/demo_rehearsal/trace carries a v2 current.summary, so
 * its own records CANNOT be read by this build, and reading them anyway would
 * compare today's raw path against an eight-month-old accounting rule.
 *
 * So the capture's own raw events are pushed through the REAL
 * pgwt_summary_push_event() — the same function src/event_stream.c calls, in
 * timestamp order, with the flags the daemon wrote (asserted to be free of
 * category bits below). The result is the per-second records TODAY's daemon
 * would have written for that same real event stream. What this tier
 * therefore establishes is: on a real PG18 io_method=worker capture, the
 * current writer and the current raw path disagree, by this much, on these
 * windows. It does not and cannot re-measure what the v2 daemon wrote.
 * ══════════════════════════════════════════════════════════════════════════ */

static int acceptance_tier(const char *dir)
{
    printf("--- 8. ACCEPTANCE TIER: %s ---\n", dir);

    struct registry reg;
    memset(&reg, 0, sizeof(reg));
    int entries = registry_load_file(&reg, dir);
    CHECK(entries > 0,
          "the capture's own backends.jsonl was read (%d entries)", entries);
    CHECK(reg.n_io_workers > 0,
          "and it resolves %d io_worker pid(s) through "
          "pgwt_backend_type_flag -- a capture with none cannot test this",
          reg.n_io_workers);
    if (entries <= 0 || reg.n_io_workers == 0) { registry_free(&reg); return 1; }

    char tpath[700];
    snprintf(tpath, sizeof(tpath), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    memset(&r, 0, sizeof(r));
    if (pgwt_reader_open(&r, tpath) != 0) {
        CHECK(0, "the capture's current.trace could not be opened (%s)", tpath);
        registry_free(&reg);
        return 1;
    }
    CHECK(r.num_blocks > 0, "the trace holds %d blocks", r.num_blocks);

    /* Decode every block. SAMPLES blocks would need the server's phantom-EXIT
     * rule, so a capture holding any is refused rather than mis-compared. */
    int cap = 0, n = 0, samples_blocks = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        if (pgwt_reader_block_info(&r, b, &bi) == 0)
            cap += (int)bi.num_events;
    }
    struct pgwt_trace_event *ev = calloc((size_t)cap + 1, sizeof(*ev));
    if (!ev) { CHECK(0, "could not allocate %d raw events", cap);
               pgwt_reader_close(&r); registry_free(&reg); return 1; }
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int got = pgwt_reader_decode_block_info(&r, b, ev + n, cap - n, &bi);
        if (got < 0) continue;
        if (bi.block_type == PGWT_BLOCK_SAMPLES) samples_blocks++;
        n += got;
    }
    CHECK(samples_blocks == 0,
          "the capture is exact-tier only (%d SAMPLES blocks): the sampled "
          "phantom-EXIT rule in src/server.c's loader is not reproduced here, "
          "so a sampled capture must be refused, not compared",
          samples_blocks);
    CHECK(n > 0, "decoded %d raw records", n);

    int ordered = 1;
    for (int i = 1; i < n; i++)
        if (ev[i - 1].timestamp_ns > ev[i].timestamp_ns) { ordered = 0; break; }
    if (!ordered)
        qsort(ev, (size_t)n, sizeof(*ev), ts_cmp);
    printf("    (raw stream was %s on disk)\n",
           ordered ? "already time-ordered" : "sorted by timestamp");
    pgwt_reader_close(&r);

    int preset = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].flags & CAT_BITS) preset++;
    CHECK(preset == 0,
          "NO record on disk carries a category flag (%d) -- the daemon never "
          "wrote one, which is precisely why a writer-side `evt->flags` test "
          "could not have filtered io_workers", preset);

    /* ONE constant shift onto THIS machine's monotonic clock before the
     * events are pushed. The writer captures CLOCK_REALTIME/CLOCK_MONOTONIC
     * when it opens a file and the reader treats a file as covering
     * [header start, +1 h], so a stream carrying the capture box's monotonic
     * timestamps converts to a wall time one foreign uptime away: every
     * window query then misses the file and the summary side answers
     * 0.000 ms — the exact shape that reads as agreement. A single shift
     * preserves every interval and every gap, so no number below changes. */
    uint64_t rebase = mono_origin();
    uint64_t first_sec_mono = (ev[0].timestamp_ns / ONE_SEC) * ONE_SEC;
    for (int i = 0; i < n; i++)
        ev[i].timestamp_ns = ev[i].timestamp_ns - first_sec_mono + rebase;

    /* The summary side: the capture's own raw events through the REAL writer
     * (see the section header for why its own v2 file cannot be used). */
    char sdir[700];
    snprintf(sdir, sizeof(sdir), "%s", fresh_dir("acceptance_summaries"));
    /* The registry (loaded above from the capture's own backends.jsonl) is
     * what the PRODUCER resolves against — summary v6: the writer is told the
     * category, it never reads evt->flags. */
    long pushed = write_summaries(sdir, ev, n, &reg);
    CHECK(pushed == n,
          "the real writer accepted every one of the capture's %d records "
          "(%ld)", n, pushed);

    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    int visited = pgwt_visit_summaries(sdir, 0, 0, rec_visitor, &rc);
    CHECK(visited >= 0 && rc.records > 0,
          "and committed %d per-second records", rc.records);
    if (rc.records == 0) { free(ev); registry_free(&reg); return 1; }

    /* The raw side is a SECOND copy: `ev` must stay exactly as it was pushed
     * so the pre-flag refusal (R1) still inspects the real writer input. Both
     * sides land on the identical wall base, exactly as tier A does — the
     * writer's mono->wall offset is one constant captured when it opened the
     * file, recovered here from the records. */
    struct pgwt_trace_event *evw = calloc((size_t)n + 1, sizeof(*evw));
    if (!evw) { CHECK(0, "could not allocate the raw-side copy");
                free(ev); registry_free(&reg); return 1; }
    memcpy(evw, ev, (size_t)n * sizeof(*evw));
    uint64_t shift = rc.min_sec - (ev[0].timestamp_ns / ONE_SEC) * ONE_SEC;
    for (int i = 0; i < n; i++)
        evw[i].timestamp_ns += shift;

    /* #363, and THE case that matters most: this is the 4.3M-record oracle
     * the acceptance numbers are computed from, and its pid count is whatever
     * the capture had. A refusal (working-allocation failure, or more
     * distinct pids than the per-pid table holds) leaves it partly rewritten
     * -- records the pass never reached keep old_event 0 instead of
     * PGWT_WEI_NONCMD_CPU and keep their emission-time query_id -- so the
     * tier must REFUSE and report, never compare. */
    if (!pgwt_tag_events(evw, n, reg.cats, reg.n)) {
        CHECK(0, "pgwt_tag_events REFUSED the capture's %d records (#363): "
                 "the raw oracle is partly rewritten and must be discarded, "
                 "so no acceptance number can be computed from it", n);
        free(ev); free(evw);
        registry_free(&reg);
        return 1;
    }
    int tagged = 0;
    for (int i = 0; i < n; i++)
        if (evw[i].flags & PGWT_EVENT_FLAG_IO_WORKER) tagged++;
    CHECK(tagged > 0,
          "pgwt_tag_events stamped PGWT_EVENT_FLAG_IO_WORKER on %d records "
          "from the registry's io_worker pids", tagged);

    uint64_t end = rc.max_sec + ONE_SEC;
    printf("    span %.0f s, %d summary records, %d raw records\n",
           (double)(end - rc.min_sec) / 1e9, rc.records, n);

    struct { const char *label; uint64_t from, to; } w[] = {
        /* 900 s ending one second SHORT of the capture end: the near-clean
         * window a single-window test would have read as agreement. */
        { "900 s, ends 1 s before the end", end - 901 * ONE_SEC,
          end - ONE_SEC },
        /* the same width, INCLUDING the final second */
        { "900 s, ends AT capture end", end - 900 * ONE_SEC, end },
        { "full span", rc.min_sec, end },
    };
    /* WHAT TIER B ASSERTS, AND WHY IT IS NOT "raw == summary".
     *
     * Exact raw-vs-summary equality on a REAL capture is not reachable by any
     * io_worker fix, and this file's own section 7 is the proof: the writer
     * buckets every record WHOLE into the second in which it closes while the
     * raw path CLIPS it to the window (event_window_ns), so a single long
     * non-io_worker idle record diverges by +899% on a 10 s window. A real
     * capture ends with dozens of such records -- flush_open_intervals closes
     * every open interval at capture end -- and there is a second, unrelated
     * asymmetry on DB Time: pgwt_tag_events rewrites a majority-outside-command
     * we==0 interval to PGWT_WEI_NONCMD_CPU (idle) on the raw side, and the
     * writer runs no marker sweep. Measured on this capture, both BEFORE and
     * AFTER the io_worker fix: DB Time +0.16% / +0.22% / +0.09% on the three
     * windows, unchanged. Asserting total equality here would therefore be
     * asserting two other bugs fixed, and the only way to make it pass would
     * be a tolerance -- which is exactly the move that hides this class of
     * divergence.
     *
     * So tier B asserts the io_worker mechanism EXACTLY, with no epsilon, by
     * INVARIANCE: remove every io_worker record from both sides and each path
     * must report bit-identical Idle and DB Time. If the writer counted their
     * time, the summary side would move (it moved by 5,549,649.645 ms on the
     * full span before the fix); if the raw path counted it, the raw side
     * would move. Both staying put IS the parity statement, and it is immune
     * to the straddler residual because that residual is present in both runs.
     * The raw-vs-summary totals and their divergence are still REPORTED for
     * every window, and the io_worker component of that divergence is what
     * this assertion pins at zero. */
    struct pgwt_trace_event *ev_no = calloc((size_t)n + 1, sizeof(*ev_no));
    struct pgwt_trace_event *evw_no = calloc((size_t)n + 1, sizeof(*evw_no));
    if (!ev_no || !evw_no) {
        CHECK(0, "could not allocate the io_worker-free copies");
        free(ev); free(evw); free(ev_no); free(evw_no);
        registry_free(&reg);
        return 1;
    }
    int m = 0;
    struct pgwt_trace_event *removed = calloc((size_t)n + 1, sizeof(*removed));
    int n_removed = 0;
    if (!removed) {
        CHECK(0, "could not allocate the removed-record list");
        free(ev); free(evw); free(ev_no); free(evw_no);
        registry_free(&reg);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        if (producer_cat_flag(&reg, ev[i].pid) == PGWT_EVENT_FLAG_IO_WORKER) {
            removed[n_removed++] = evw[i];   /* wall base, for the windows */
            continue;
        }
        ev_no[m]  = ev[i];
        evw_no[m] = evw[i];
        m++;
    }
    CHECK(n - m > 0,
          "the capture carries io_worker records to remove (%d of %d) -- 0 "
          "would make the invariance assertion below vacuous", n - m, n);
    char sdir_no[700];
    snprintf(sdir_no, sizeof(sdir_no), "%s", fresh_dir("acceptance_noiw"));
    long pushed_no = write_summaries(sdir_no, ev_no, m, &reg);
    CHECK(pushed_no == m,
          "the writer accepted every one of the %d non-io_worker records "
          "(%ld)", m, pushed_no);
    struct rec_ctx rc_no;
    memset(&rc_no, 0, sizeof(rc_no));
    pgwt_visit_summaries(sdir_no, 0, 0, rec_visitor, &rc_no);
    CHECK(rc_no.records > 0 && rc_no.min_sec == rc.min_sec,
          "the io_worker-free run committed records on the SAME wall base "
          "(%d records, base %llu == %llu) -- a different base would compare "
          "two different windows", rc_no.records,
          (unsigned long long)rc_no.min_sec, (unsigned long long)rc.min_sec);

    int iw_sum_windows = 0;
    for (int i = 0; i < 3; i++) {
        uint64_t from = w[i].from < rc.min_sec ? rc.min_sec : w[i].from;
        /* `ev` is verbatim what went into the writer, so R1 inspects the real
         * writer input; `evw` is the server-side tagged copy. */
        struct parity p = parity_run(sdir, ev, n, evw, n, &reg, from, w[i].to);
        parity_report(w[i].label, &p);
        /* A refusal is still a failure: the window has to be comparable. */
        CHECK(p.v != V_REFUSED, "%s: the comparison was possible%s%s",
              w[i].label, p.v == V_REFUSED ? " -- REFUSED: " : "",
              p.v == V_REFUSED ? p.refusal : "");
        if (p.v == V_REFUSED)
            continue;

        /* PER-WINDOW NON-VACUITY, and it is NOT a record count. Two
         * different quantities decide whether each side can see anything:
         *
         *   iw_raw_ms  the io_worker time the RAW path would have counted in
         *              this window = Σ overlap([ts-dur, ts), [from, to))
         *              (event_window_ns' rule), so a capture-end straddler
         *              contributes to EVERY earlier window too;
         *   iw_sum_ms  what the SUMMARY side buckets INTO this window = the
         *              full duration of each record whose CLOSING SECOND
         *              falls inside it, because the writer files a record
         *              whole into the second in which it closes.
         *
         * Only iw_sum_ms can make the summary-invariance check bite, and on
         * a real capture every io_worker record closes at capture end -- so
         * a window ending earlier has iw_sum_ms == 0 and its summary check
         * passes no matter what the writer does. Counting records would have
         * hidden that (all three windows "have" 3 records). Reported per
         * window, and at least one window must have iw_sum_ms > 0 or the
         * whole tier is vacuous (checked after the loop). */
        double iw_raw_ms = 0, iw_sum_ms = 0;
        for (int r = 0; r < n_removed; r++) {
            uint64_t ts  = removed[r].timestamp_ns;
            uint64_t dur = removed[r].duration_ns;
            uint64_t st  = ts > dur ? ts - dur : 0;
            uint64_t lo  = st > from ? st : from;
            uint64_t hi  = ts < w[i].to ? ts : w[i].to;
            if (hi > lo) iw_raw_ms += (double)(hi - lo) / 1e6;
            uint64_t close_sec = (ts / ONE_SEC) * ONE_SEC;
            if (close_sec >= from && close_sec < w[i].to)
                iw_sum_ms += (double)dur / 1e6;
        }
        printf("    %-34s io_worker time in window: raw-clipped %.3f ms, "
               "summary-bucketed %.3f ms%s\n", "", iw_raw_ms, iw_sum_ms,
               iw_sum_ms == 0.0
                 ? "  <- the summary side cannot see it here, so only the RAW "
                   "invariance check bites in this window"
                 : "");
        CHECK(iw_raw_ms > 0.0 || iw_sum_ms > 0.0,
              "%s: the window actually contains io_worker time (raw %.3f ms, "
              "summary %.3f ms) -- a window containing none would make both "
              "invariance checks below pass on nothing",
              w[i].label, iw_raw_ms, iw_sum_ms);
        if (iw_sum_ms > 0.0) iw_sum_windows++;

        struct pgwt_filter f;
        memset(&f, 0, sizeof(f));
        double wall = (double)(w[i].to - from) / 1e6;
        struct pgwt_tm_result rn, sn;
        memset(&rn, 0, sizeof(rn)); memset(&sn, 0, sizeof(sn));
        pgwt_compute_time_model(evw_no, m, &f, from, w[i].to, wall, &rn);
        pgwt_compute_time_model_from_summaries(sdir_no, from, w[i].to, &f,
                                               wall, &sn);
        CHECK(sn.idle_time_ms == p.sum_idle_ms
              && sn.db_time_ms == p.sum_db_ms,
              "%s: the SUMMARY path is bit-identical with the io_worker "
              "records removed (Idle %.3f == %.3f, DB Time %.3f == %.3f) -- "
              "so it counted none of the %.3f ms this window would have "
              "bucketed", w[i].label, sn.idle_time_ms, p.sum_idle_ms,
              sn.db_time_ms, p.sum_db_ms, iw_sum_ms);
        CHECK(rn.idle_time_ms == p.raw_idle_ms
              && rn.db_time_ms == p.raw_db_ms,
              "%s: and so is the RAW path (Idle %.3f == %.3f, DB Time "
              "%.3f == %.3f) -- both paths are invariant to io_worker "
              "records, which is the parity this gate exists for",
              w[i].label, rn.idle_time_ms, p.raw_idle_ms,
              rn.db_time_ms, p.raw_db_ms);
        printf("    %-34s residual raw-vs-summary after io_workers: "
               "Idle %+.3f ms (%+.4f%%), DB Time %+.3f ms (%+.4f%%) -- the "
               "straddler + NONCMD_CPU mechanisms, NOT io_worker time; "
               "measured, not asserted (section 7)\n", "",
               p.sum_idle_ms - p.raw_idle_ms, p.idle_div_pct,
               p.sum_db_ms - p.raw_db_ms, p.db_div_pct);
        free(rn.rows); free(sn.rows);
    }

    /* The tier as a whole has to contain at least one window in which the
     * SUMMARY side buckets io_worker time, or every summary-invariance check
     * above was satisfied by a window that could not see the thing being
     * checked. On a real capture that is the window containing the final
     * second, where flush_open_intervals closes the io_workers' only
     * records. */
    CHECK(iw_sum_windows > 0,
          "at least one of the three windows buckets io_worker time on the "
          "SUMMARY side (%d do) -- otherwise the summary-invariance checks "
          "above are vacuous no matter how many io_worker RECORDS the capture "
          "holds", iw_sum_windows);

    free(ev);
    free(evw);
    free(ev_no);
    free(evw_no);
    free(removed);
    registry_free(&reg);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    printf("=== test_io_worker_parity: raw vs summary Idle/DB Time "
           "(#376, #315) ===\n");

    const char *tmp = getenv("TMPDIR");
    snprintf(g_base, sizeof(g_base), "%s/pgwt_iowp_%d",
             tmp && *tmp ? tmp : "/tmp", (int)getpid());
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }
    mkdir(g_base, 0755);

    assert_registry_chain();

    struct fixture fx;
    if (fixture_build(&fx, "parity", 0) != 0) {
        printf("  FAIL: could not build the companion fixture\n");
        tests_failed++;
    } else {
        assert_fixture_live(&fx);
        test_windows(&fx);
        test_fix_is_reachable(&fx);
        test_heatmap_routes(&fx);
        test_mutation_probes(&fx);
        bypass_suite(&fx);
        fixture_free(&fx);
    }
    measure_residual_straddler();

    const char *trace = argc > 1 ? argv[1] : getenv("PGWT_PARITY_TRACE");
    if (trace && *trace) {
        acceptance_tier(trace);
    } else {
        printf("--- 8. ACCEPTANCE TIER: not requested ---\n"
               "    A real PG18 io_method=worker capture is the evidence for "
               "this fix; the\n"
               "    sections above are a unit companion. Run with a trace "
               "directory:\n"
               "      PGWT_PARITY_TRACE=tests/results/demo_rehearsal/trace "
               "./test_io_worker_parity\n");
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
