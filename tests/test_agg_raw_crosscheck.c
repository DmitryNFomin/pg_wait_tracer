/* test_agg_raw_crosscheck.c — the AGGREGATE-vs-RAW differential gate.
 *
 * WHY THIS EXISTS, AND WHY IT COMES BEFORE THE THING IT PROTECTS
 * ─────────────────────────────────────────────────────────────────────────
 * pgwt-server answers the same UI question two ways: from pre-aggregated
 * per-second summary records (~50 ms at any window width) or by materialising
 * every raw event in the window (3-5 s at millions of events).
 * should_use_summaries() (src/server.c) picks: >= 120 s goes to summaries,
 * anything shorter is forced raw, and ANY pid filter is forced raw.
 *
 * The failure mode that matters is the two paths silently DISAGREEING. A
 * wrong-but-fast answer is worse than a slow one because nothing reveals it:
 * the response carries "fidelity":"exact" either way, and the only way a user
 * notices is by resizing the window past 120 s and watching a number jump.
 * This project has already shipped exactly that bug once (FID-2,
 * docs/ROADMAP_AND_STATUS.md:579 — the summary path stamped "exact" on data
 * the sampler never fed).
 *
 * So this file is the gate that every aggregate path must pass, built against
 * the pairs that exist TODAY so that the next one lands into an existing gate
 * rather than next to one.
 *
 * HOW THE TWO PATHS ARE DRIVEN, AND WHY THAT IS HONEST
 * ─────────────────────────────────────────────────────────────────────────
 * Not through the router. should_use_summaries() cannot give both answers for
 * one request BY CONSTRUCTION: the window-width gate is a strict either/or and
 * the pid gate is one-way. Manipulating it (two requests with the clock moved,
 * a test-only flag in src/server.c) would mean comparing two DIFFERENT
 * requests, or shipping a flag whose only caller is this test.
 *
 * Instead both compute entry points are called DIRECTLY — pgwt_compute_X() and
 * pgwt_compute_X_from_summaries() — with the identical (from_ns, to_ns,
 * filter, wall_ms, num_buckets) tuple. That matches the server's handlers on
 * either side of the `if (should_use_summaries(...))` branch: they pass the
 * same `&req->filter` and the same `wall_ms` to both, and add JSON
 * serialisation and the fidelity label, not arithmetic.
 *
 * ONE DIFFERENCE, stated rather than glossed: the handlers hand the summary
 * call the DEFAULTED window (`req->from_ns ? req->from_ns :
 * srv->earliest_wall_ns`, same for `to`) and the raw call `req->from_ns` /
 * `req->to_ns` verbatim — which for a whole-capture request is 0/0, i.e.
 * "unbounded". Those are the same question in two encodings as long as
 * earliest/latest bracket the data. handle_heatmap is the exception and
 * passes the defaulted window to both. This file passes a real, explicit,
 * second-aligned window to both calls, which is the stricter case: it
 * exercises the raw path's window clipping instead of bypassing it with 0/0.
 *
 * What this test therefore does NOT cover, and what no C unit test can: a
 * handler passing a DIFFERENT window or filter to the two calls, or routing
 * a filter shape the summary path cannot honour. (Three handlers already
 * guard for that — see the `hole` strings in PAIRS below, which name which
 * guard defends which divergence.) That layer is covered by
 * tests/test_data_*.py, which drive the real pgwt-server binary.
 *
 * SAME COMMITTED BLOCKS, SAME WINDOW
 * ─────────────────────────────────────────────────────────────────────────
 * The summary side reads the .summary.lz4 files that the REAL writer
 * (src/summary_writer.c) committed; the raw side reads the same event array
 * that was pushed into that writer. Three things pin "same":
 *   - every push is checked for acceptance, so nothing is silently dropped;
 *   - sum(record.total_events) over the committed blocks must equal the number
 *     of events pushed (markers/EXIT excluded, of which there are none here);
 *   - the window is derived from the records that were actually written
 *     (window_from_records), never from a clock.
 * The raw events carry MONOTONIC timestamps and the records carry WALL
 * seconds, so the raw side gets a second copy of the array shifted by the
 * writer's own constant mono->wall offset, recovered from the records
 * (wall_shift). Both sides then see the identical wall window and the raw path
 * is exercised WITH its window clipping on, not with from=to=0.
 *
 * WHAT "AGREE" MEANS — THE FIELD CLASSIFICATION
 * ─────────────────────────────────────────────────────────────────────────
 * There is NO blanket epsilon in this file. A blanket epsilon is how this
 * class of bug hides: it turns "these two numbers are the same" into "these
 * two numbers are close", and 0.5% of a 900 s window is 4.5 seconds of
 * misaccounted DB Time.
 *
 * BIT-EXACT (asserted with ==, no tolerance). Every ms / us / AAS / % number
 * on both paths is ONE arithmetic expression over sums of integer
 * nanoseconds:
 *     time_ms = (sum of integer ns) / 1e6
 *     pct     = a_ms / b_ms * 100.0        (same expression, both paths)
 *     aas     = a_ms / wall_ms             (same expression, both paths)
 * The two paths sum the same integers in a DIFFERENT ORDER (raw: per event;
 * summary: per second), and some accumulators are `double` rather than
 * uint64_t (struct class_accum in compute.c). Integer-valued double addition
 * is associative and exact while every partial sum stays below 2^53, so the
 * order does not matter and the result is bit-identical — and that PREMISE is
 * asserted, not assumed: assert_exact_ns_premise() checks every total against
 * 2^53 ns (= 104 days of wait time in one window). Division by 1e6 / by a
 * shared denominator of two bit-identical doubles is one correctly-rounded
 * operation on identical operands, so it too is bit-identical. Hence `==`.
 *
 * BOUNDED: none. No field in these six result structs needs a tolerance, and
 * the moment one does it means the paths disagree. If a future field genuinely
 * cannot be exact (a sampled estimator, say), its bound belongs here with its
 * derivation — never a tuned constant.
 *
 * NOT COMPARABLE (excluded, each with its reason and its source line). Every
 * one of these is a NAMED HOLE in this gate, not a silent one:
 *
 *  N1. pgwt_tm_result.cat_ms[], .io_worker_busy_pct, .cpu_ms, .offcpu_ms,
 *      .has_measured_cpu, .cpu_clamped_ms, .wait_gap_cpu_ms
 *      — raw path only; the summary records carry no category or measured-CPU
 *      decomposition, so the summary path leaves them 0 (documented at
 *      src/compute.h:239-253). The SERVER does not emit them on the summary
 *      path either, so there is no user-visible disagreement; comparing them
 *      would compare "a number" against "deliberately absent".
 *
 *  N2. pgwt_queries_result.unattributed_ms / .unattributed_count /
 *      .backfilled_ms — raw path only, by the same rule, and the server marks
 *      the response `unattributed_available: false` on the summary path
 *      (src/compute.h:374-381).
 *
 *  N3. pgwt_aas_bucket.cat_aas[] / .offcpu_aas — raw path only (same reason as
 *      N1); aas_summary_visitor never writes them.
 *
 *  N4. pgwt_session_row.top_wait / .top_wait_id and
 *      pgwt_query_row.top_wait / .top_wait_id — TWO DIFFERENT QUANTITIES, not
 *      an accuracy question:
 *        raw     (src/compute.c:1621-1627) = argmax over the window of the
 *                pid's TOTAL time per event;
 *        summary (src/summary_writer.c:303-306 + src/compute.c:2501-2504) =
 *                the single LONGEST INDIVIDUAL wait, because the writer keeps
 *                `dur > ss->top_wait_ns` per second and the reader keeps the
 *                max over seconds.
 *      MEASURED counterexample (a 130 s probe, pid 301): 130 x 10 ms of Lock
 *      (1300 ms total, 10 ms max) against 1 x 40 ms of IO (40 ms total, 40 ms
 *      max) — raw reports top_wait = Lock:relation, the summary path reports
 *      IO:DataFileRead, with both agreeing on db_time_ms 1340.000. Reachable
 *      unfiltered. Asserting equality would be green only by fixture choice, so this
 *      gate asserts instead the strongest FIX-PROOF invariant: the summary's
 *      top wait must be an event the session/query ACTUALLY had in the window
 *      with non-zero raw time, and must never be an idle event or 0. That
 *      catches a summary path naming an event that is not there, which is the
 *      failure worth catching, and it stays green whichever definition the
 *      product settles on. See FINDING 1 in the report.
 *
 *  N5. top_sessions under a class / event / query filter —
 *      ts_summary_visitor (src/compute.c:2487-2491) looks at f->pid ONLY: it
 *      ignores class_name, event_id and query_id entirely, while the raw path
 *      applies all of them via pgwt_filter_matches. Not an accuracy gap, a
 *      missing feature, and REACHABLE (should_use_summaries blocks only pid).
 *      Compared UNFILTERED here; see FINDING 2.
 *
 *  N6. heatmap under a query filter — hm_summary_visitor consults
 *      summary_event_matches_filter (class/event) but there is no per-query
 *      histogram in the records, so query_id is ignored. Also reachable. See
 *      FINDING 3.
 *
 *  N7. events carrying PGWT_EVENT_FLAG_IO_WORKER are absent from the fixture.
 *      The raw paths exclude io_worker records from DB Time / Top Events /
 *      Top Sessions / Top Queries (src/compute.c:1077, 1440, 1575, 1685);
 *      src/summary_writer.c's accum_event has NO io_worker check at all, so
 *      they enter class_ns, events[], sessions[] and queries[]. There is no
 *      fix-proof invariant to assert here — one path counts the time and the
 *      other does not — so the fixture stays io_worker-free (asserted in
 *      assert_fixture_live, so the hole cannot be quietly widened) and the
 *      divergence is reported rather than hidden inside a tolerance.
 *      MEASURED on a 130 s probe with one io_worker burning 40 ms/s: raw DB
 *      Time 1300.000 ms vs summary 6500.000 ms (+400%), the io_worker also
 *      appearing as a Top Sessions row (5200.000 ms) and a Top Events row
 *      that exist only on the summary side. REACHABLE: no handler guards it.
 *
 *  N8. the WINDOW END BOUND. pgwt_visit_summaries (src/summary_reader.c:474)
 *      drops a record only when `rec_ns > to_wall_ns`, so the record whose
 *      second starts exactly AT the window end is included in full, while
 *      every other window consumer is half-open (event_window_ns clips at
 *      to_ns; pgwt_compute_heatmap drops `ev_ts >= to_ns`). A summary-path
 *      answer over [T, T+W) therefore covers W+1 seconds, and the extra
 *      second lands on the LAST bucket of any chart. The main 130 s window
 *      below ends one second past the last record, so it is unaffected;
 *      section 5h measures the effect on an interior window and asserts it
 *      FIX-PROOF (the summary total must be exactly the in-window slice or
 *      exactly those two slices -- nothing else). Reported, not fixed:
 *      src/summary_reader.c is product code and this branch is the gate.
 *
 *  N9/N10/N11. three filter-shaped divergences the registry below names in
 *      full, each with the measured numbers: Top Events under a query filter
 *      loses the whole CPU* row, Top Queries under a class filter reports an
 *      unfiltered count, Top Queries under an event-only filter reports
 *      unfiltered per-class columns. All three are reachable
 *      (should_use_summaries blocks only pid filters).
 *
 * ROW ORDER is also not comparable: both paths qsort by total descending and
 * neither sort is stable, so equal totals may come back in either order.
 * Rows are therefore matched by IDENTITY (name+indent / event_id / pid /
 * query_id) and the match is required to be a bijection — so a row present on
 * one side only still fails. The fixture additionally has no ties at all, and
 * assert_no_ties() checks that premise, because the per-class top-5 sub-event
 * selection in both time-model paths would otherwise be free to pick
 * different events.
 *
 * NON-VACUITY — WHAT STOPS THIS PASSING FOR THE WRONG REASON
 * ─────────────────────────────────────────────────────────────────────────
 * "0 == 0" satisfies every assertion above. A pgwt_visit_summaries() that
 * finds no files returns 0 records and every summary total is 0; if the raw
 * side were also empty the whole file would be green and blind. So before any
 * agreement is asserted:
 *   - the committed blocks must exist and be complete (records == SECS,
 *     sum(total_events) == events pushed, events_overflow == 0);
 *   - the fixture's own expected DB Time / Idle / event count are literals
 *     computed from the slice table, independent of BOTH paths, and both
 *     paths must match them;
 *   - every diff_*() refuses (counts a mismatch) when EITHER side has zero
 *     rows or a zero total — a comparison with nothing in it is a FAILURE,
 *     never a skip;
 *   - section 5 drives every way the comparison can be starved (no directory,
 *     a writer that wrote nothing, an unreadable file, a window with no
 *     overlap, a one-second window, an empty registry, an unreadable
 *     compute.h) and requires each to be REFUSED, and shows for the empty
 *     case that the naive comparison would have said "equal".
 *
 * TIMING AND ORDERING
 * ─────────────────────────────────────────────────────────────────────────
 * Every timestamp, duration, pid and query id below is a literal. The single
 * real clock read is mono_origin(), needed because the writer stamps its file
 * header with CLOCK_REALTIME and the reader's file-level window filter treats
 * a file as covering [header start, +1 h]: a stream of small literal monotonic
 * values converts to a wall time one machine-uptime in the past, every window
 * query misses the file, and the summary side answers 0.000 ms — which an
 * agreement test reads as success. (test_idle_summary.c hit exactly that.)
 * No assertion depends on the value of that read; section 5c pins the
 * zero-records shape so it cannot come back silently.
 *
 * SECTION MAP
 *   1. PAIR COVERAGE — every pgwt_compute_*_from_summaries in src/compute.h
 *      must be cross-checked here. This is the part that makes the derived-
 *      tier plan safe: a new aggregate path turns this test RED until it has
 *      a differential. A header it cannot read or parse REFUSES.
 *   2. the fixture, its non-vacuity and its premises.
 *   3. the differentials, per command x filter.
 *   4. MUTATION PROBES — one perturbed field at a time in a COPY of the
 *      summary-side result, proving each diff_*() can actually see a 1 ns /
 *      1-count / 1-cell / 1-row difference. A comparator that returned 0
 *      unconditionally would pass section 3 and fail every one of these.
 *   5. the bypass suite (above), plus 5h's measurement of N8.
 *
 * ENVIRONMENT INDEPENDENCE. Which wait events count as idle is derived from
 * resolved NAMES (src/idle_rule.c), because the Timeout ids invert between
 * PG13 and PG18 -- so the same fixture classifies differently on a host with
 * a PostgreSQL event table than on one without. Nothing here hardcodes that
 * classification: every expected total calls pgwt_is_idle_event() itself, and
 * every other assertion is raw-vs-summary, where both sides use the same
 * predicate. The fixture's DB-Time/Idle split therefore differs between this
 * Mac and the gate box, and every assertion holds on both.
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                        \
    tests_run++;                                                          \
    if (!(cond)) { tests_failed++;                                        \
        printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); }  \
} while (0)

#define MS      1000000ULL
#define US      1000ULL
#define ONE_SEC 1000000000ULL

/* The exactness premise (see the header): integer-valued double arithmetic is
 * exact below 2^53. 2^53 ns = 9007 s ~= 104 days of accumulated wait time in a
 * single window. Asserted, not assumed. */
#define EXACT_NS_LIMIT 9007199254740992.0

/* Wait-event ids, named by CLASS and index rather than by wait-event name on
 * purpose: the name a numeric id resolves to depends on the PostgreSQL event
 * table the host could load (and the Timeout ids INVERT between PG13 and
 * PG18), so a name in a macro here would be wrong on some tier. Nothing in
 * this file depends on the names: the fixture needs a spread of CLASSES, and
 * the idle/hidden classification is always asked of pgwt_is_idle_event() /
 * pgwt_is_hidden_event() at run time. */
#define EV_LOCK_A    WEI(PG_WAIT_LOCK, 0)
#define EV_IO_A      WEI(PG_WAIT_IO, 21)
#define EV_IO_B      WEI(PG_WAIT_IO, 60)      /* a second IO row */
#define EV_LWLOCK_A  WEI(PG_WAIT_LWLOCK, 7)
#define EV_IPC_A     WEI(PG_WAIT_IPC, 20)
#define EV_TIMEOUT_2 WEI(PG_WAIT_TIMEOUT, 2)
#define EV_TIMEOUT_1 WEI(PG_WAIT_TIMEOUT, 1)
#define EV_CLIENT_0  WEI(PG_WAIT_CLIENT, 0)
#define EV_ACTIVITY_4 WEI(PG_WAIT_ACTIVITY, 4)  /* hidden on every tier */
#define EV_CPU       0u                          /* on-CPU gap */

#define QID_A 0x1111111111111111ULL
#define QID_B 0x2222222222222222ULL

/* ── scratch dirs ──────────────────────────────────────────────────────── */

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

/* ══════════════════════════════════════════════════════════════════════════
 * 2. THE FIXTURE
 *
 * SECS one-second slices (> the 120 s floor should_use_summaries applies, so
 * this is the shape of request that actually takes the summary path).
 *
 * Every event is placed 500 ms into its second and the longest duration is
 * 400 ms, so NO event straddles a second boundary and none is clipped by the
 * window. A straddler would be split between two summary seconds while the
 * raw path keeps it whole, which turns a bit-exact comparison into a tolerance
 * argument; a clipped first/last event would make the raw total legitimately
 * smaller. Both are real product behaviours and both belong in
 * tests/test_window_clip.c, not in a path-vs-path differential.
 *
 * Durations are base_us * (1 + s % 8): eight distinct values per event id, so
 * the latency histogram spans several buckets and p50/p95/p99 are genuinely
 * different numbers (one of them lands in the open-ended >= 16.384 ms bucket,
 * exercising the *_overflow flags).
 * ══════════════════════════════════════════════════════════════════════════ */

#define SECS 130

struct ev_spec {
    uint32_t pid;
    uint32_t ev;
    uint64_t base_us;    /* duration = base_us * (1 + s % 8) */
    uint64_t qid;
};

/* No two specs share (ev) so per-event totals are distinct; the per-class and
 * per-pid totals below are distinct too (assert_no_ties checks it). */
static const struct ev_spec slice[] = {
    { 101, EV_CPU,        7000, QID_A },  /* on-CPU gap                   */
    { 101, EV_LOCK_A,     5000, QID_A },  /* Lock                         */
    { 102, EV_IO_A,       3100, QID_B },  /* IO                           */
    { 102, EV_IO_B,        430, QID_B },  /* IO, a second row in the class */
    { 103, EV_LWLOCK_A,     91, QID_A },  /* LWLock                       */
    { 103, EV_IPC_A,         3, QID_A },  /* IPC, sub-microsecond         */
    { 104, EV_TIMEOUT_2,  1700, QID_B },  /* Timeout                      */
    { 101, EV_CLIENT_0,  11000, QID_A },  /* Client -- idle but VISIBLE   */
    /* qid 0: a checkpointer never reports a query id, and giving it one
     * makes the #128 deferred per-query resolver file this time under
     * another query -- a fixture artefact, not product behaviour. */
    { 105, EV_TIMEOUT_1,  2300, 0     },  /* Timeout pacing, no query id  */
    { 106, EV_ACTIVITY_4, 4700, 0     },  /* Activity -- idle AND hidden  */
};
#define SLICE_N ((int)(sizeof(slice) / sizeof(slice[0])))
#define MAX_EV  (SECS * SLICE_N)

struct fixture {
    char      dir[600];
    struct pgwt_trace_event *mono;   /* what was pushed into the writer */
    struct pgwt_trace_event *wall;   /* the same events, wall timestamps */
    int       n;
    uint64_t  from_ns, to_ns;        /* wall window, second-aligned */
    double    wall_ms;
    int       records;
    uint64_t  block_events;          /* sum of events[].count over records */
    uint32_t  overflow;              /* sum of record.events_overflow */
    int       max_events, max_sessions, max_queries;  /* widest second */
    /* Expected totals, derived from `slice` alone — from NEITHER path. */
    double    exp_db_ms;
    double    exp_idle_ms;
    uint64_t  exp_events;
};

static uint64_t spec_dur_ns(int spec, int s)
{
    return slice[spec].base_us * (1 + (uint64_t)(s % 8)) * US;
}

/* THE MONOTONIC ORIGIN — a clock READ, not a timing dependency. See the
 * header's TIMING note for why it cannot be a literal. */
static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

static int build_stream(struct pgwt_trace_event *out, uint64_t base_mono)
{
    int n = 0;
    for (int s = 0; s < SECS; s++) {
        uint64_t sec = base_mono + (uint64_t)s * ONE_SEC;
        for (int i = 0; i < SLICE_N; i++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            /* Each interval ENDS at its timestamp. 500 ms in, max duration
             * 400 ms => every interval starts inside its own second. */
            e.timestamp_ns = sec + 500 * MS + (uint64_t)i * MS;
            e.pid          = slice[i].pid;
            e.old_event    = slice[i].ev;
            e.new_event    = EV_IO_A;  /* never EXIT, never a marker */
            e.duration_ns  = spec_dur_ns(i, s);
            e.query_id     = slice[i].qid;
            /* PGWT_CPU_NS_UNKNOWN throughout: the measured-CPU decomposition
             * is raw-path-only (N1), so leaving it unmeasured keeps the two
             * time-model ROW SETS comparable (no Off-CPU* row on either
             * side) instead of forcing a whole-row exclusion. */
            e.cpu_ns       = PGWT_CPU_NS_UNKNOWN;
            out[n++] = e;
        }
    }
    return n;
}

/* ── reading the committed blocks back directly ───────────────────────── */

struct rec_ctx {
    int      records;
    uint64_t total_events;
    uint32_t overflow;
    uint64_t min_sec, max_sec;
    int      max_sessions, max_queries, max_events;
};

static int rec_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct rec_ctx *c = vctx;
    c->records++;
    /* NOT rec->total_events: that field is an in-memory accumulator counter
     * and is NOT part of the serialized record (see the serialize layout in
     * src/summary_writer.h), so it reads back as 0 and a conservation check
     * built on it would be vacuously satisfied. events[] IS serialized and
     * carries every non-marker, non-EXIT event including the idle and hidden
     * ones, so summing its counts is the honest "did every event I pushed
     * reach the committed blocks" measure. */
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        c->total_events += se->count;
    }
    c->overflow     += rec->events_overflow;
    if (c->min_sec == 0 || rec->second_wall_ns < c->min_sec)
        c->min_sec = rec->second_wall_ns;
    if (rec->second_wall_ns > c->max_sec) c->max_sec = rec->second_wall_ns;
    if (rec->num_sessions > c->max_sessions) c->max_sessions = rec->num_sessions;
    if (rec->num_queries  > c->max_queries)  c->max_queries  = rec->num_queries;
    if (rec->num_events   > c->max_events)   c->max_events   = rec->num_events;
    return 0;
}

/* Write a stream through the REAL writer, checking every push. Returns the
 * number of events the writer accepted, or -1. */
static long write_summaries(const char *dir, const struct pgwt_trace_event *ev,
                            int n)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) return -1;
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) { free(w); return -1; }
    long pushed = 0;
    for (int i = 0; i < n; i++) {
        if (pgwt_summary_push_event(w, &ev[i]) != 0) {
            printf("  FAIL: writer refused event %d\n", i);
            tests_failed++;
            break;
        }
        pushed++;
    }
    pgwt_summary_flush(w);          /* close the last, still-open second */
    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
    return pushed;
}

/* Build the fixture: write the stream, derive the window from the records that
 * were actually committed, and shift a copy of the events into the same wall
 * time base. Returns 0 on success. */
static int fixture_build(struct fixture *fx, const char *dirname)
{
    memset(fx, 0, sizeof(*fx));
    snprintf(fx->dir, sizeof(fx->dir), "%s", fresh_dir(dirname));

    uint64_t origin = mono_origin();
    fx->mono = calloc(MAX_EV, sizeof(*fx->mono));
    fx->wall = calloc(MAX_EV, sizeof(*fx->wall));
    if (!fx->mono || !fx->wall) return -1;
    fx->n = build_stream(fx->mono, origin);

    long pushed = write_summaries(fx->dir, fx->mono, fx->n);
    if (pushed != fx->n) {
        printf("  FAIL: writer accepted %ld of %d events\n", pushed, fx->n);
        tests_failed++;
        return -1;
    }

    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    if (pgwt_visit_summaries(fx->dir, 0, 0, rec_visitor, &rc) < 0 ||
        rc.records == 0) {
        printf("  FAIL: no summary records were committed to %s\n", fx->dir);
        tests_failed++;
        return -1;
    }
    fx->records      = rc.records;
    fx->block_events = rc.total_events;
    fx->overflow     = rc.overflow;
    fx->max_events   = rc.max_events;
    fx->max_sessions = rc.max_sessions;
    fx->max_queries  = rc.max_queries;
    fx->from_ns      = rc.min_sec;
    fx->to_ns        = rc.max_sec + ONE_SEC;
    fx->wall_ms      = (double)(fx->to_ns - fx->from_ns) / 1e6;

    /* The writer's mono->wall offset is a single constant captured when it
     * opened the file (mono_to_wall in src/summary_writer.c), so recovering it
     * from the records is exact — and it keeps this file free of a second
     * clock read. */
    uint64_t shift = fx->from_ns - origin;
    memcpy(fx->wall, fx->mono, (size_t)fx->n * sizeof(*fx->wall));
    for (int i = 0; i < fx->n; i++)
        fx->wall[i].timestamp_ns += shift;

    /* Expected totals from the slice table alone. Summed as INTEGER
     * nanoseconds and divided once, exactly as both compute paths do: summing
     * per-event `dur/1e6` doubles instead gives a number that prints the same
     * to six places and is not bit-equal, which would turn this independent
     * cross-check into a tolerance argument. */
    uint64_t db_ns = 0, idle_ns = 0;
    for (int s = 0; s < SECS; s++)
        for (int i = 0; i < SLICE_N; i++) {
            uint64_t d = spec_dur_ns(i, s);
            if (pgwt_is_idle_event(slice[i].ev)) idle_ns += d;
            else                                 db_ns   += d;
            fx->exp_events++;
        }
    fx->exp_db_ms   = (double)db_ns / 1e6;
    fx->exp_idle_ms = (double)idle_ns / 1e6;
    return 0;
}

static void fixture_free(struct fixture *fx)
{
    free(fx->mono); free(fx->wall);
    fx->mono = fx->wall = NULL;
}

/* ── non-vacuity and the premises the == assertions rest on ───────────── */

static void assert_fixture_live(const struct fixture *fx)
{
    printf("--- 2. the fixture is non-empty, complete and tie-free ---\n");
    CHECK(fx->records == SECS, "every second was committed (%d == %d)",
          fx->records, SECS);
    CHECK(fx->block_events == (uint64_t)fx->n,
          "the committed blocks hold EVERY pushed event (%llu == %d) -- "
          "the raw and summary sides are the same events, not two samples",
          (unsigned long long)fx->block_events, fx->n);
    CHECK(fx->overflow == 0,
          "no per-second event table overflowed (%u) -- an overflow would "
          "make the summary side legitimately short",
          fx->overflow);
    CHECK(fx->to_ns - fx->from_ns == (uint64_t)SECS * ONE_SEC,
          "the window is exactly %d s (%llu ns)", SECS,
          (unsigned long long)(fx->to_ns - fx->from_ns));
    CHECK(fx->to_ns - fx->from_ns >= 120 * ONE_SEC,
          "and wider than should_use_summaries' 120 s floor, so this is the "
          "shape of request that really takes the summary path");
    CHECK(fx->exp_db_ms > 0 && fx->exp_idle_ms > 0 && fx->exp_events > 0,
          "the fixture has DB Time (%.3f ms), Idle (%.3f ms) and events "
          "(%llu) by construction", fx->exp_db_ms, fx->exp_idle_ms,
          (unsigned long long)fx->exp_events);
    /* BREADTH, read off the committed records. A single pid / single event /
     * single query fixture would compare one row per command and would never
     * reach the per-event, per-session or per-query tables these paths
     * actually diverge in. (max_queries is 2, not 3: query_id 0 is the
     * "unused slot" sentinel in the per-second query table, so the
     * checkpointer's and the Activity pid's unattributed time is not a
     * counted query on either path.) */
    CHECK(fx->max_events >= 8 && fx->max_sessions >= 5 && fx->max_queries >= 2,
          "each committed second holds many distinct events (%d), sessions "
          "(%d) and queries (%d)",
          fx->max_events, fx->max_sessions, fx->max_queries);
    /* N7's premise, checked rather than trusted. The fixture must stay
     * io_worker-free: src/summary_writer.c's accum_event has no io_worker
     * check, so such a record enters class_ns / events[] / sessions[] /
     * queries[] while every raw path excludes it -- measured +400% on summary
     * DB Time against raw for a 130 s stream with one io_worker. Adding one
     * here would not reveal that; it would just make every comparison below
     * red and invite someone to loosen them. The exclusion is the named hole;
     * this is what stops it from being quietly widened. */
    int io_worker_events = 0;
    for (int i = 0; i < fx->n; i++)
        if (fx->mono[i].flags & PGWT_EVENT_FLAG_IO_WORKER) io_worker_events++;
    CHECK(io_worker_events == 0,
          "the fixture carries no PGWT_EVENT_FLAG_IO_WORKER records (%d) -- "
          "see hole N7", io_worker_events);

    /* The exactness premise. */
    CHECK((fx->exp_db_ms + fx->exp_idle_ms) * 1e6 < EXACT_NS_LIMIT,
          "total accumulated ns (%.0f) stays below 2^53, so integer-valued "
          "double sums are exact regardless of addition order",
          (fx->exp_db_ms + fx->exp_idle_ms) * 1e6);
}

/* Ties would let the two paths' unstable qsorts, and the per-class top-5
 * sub-event cut, pick different rows — a disagreement that is not a bug.
 * The fixture has none; this checks that rather than trusting it. */
static void assert_no_ties(const struct fixture *fx)
{
    double per_ev[SLICE_N];
    memset(per_ev, 0, sizeof(per_ev));
    for (int s = 0; s < SECS; s++)
        for (int i = 0; i < SLICE_N; i++)
            per_ev[i] += (double)spec_dur_ns(i, s);
    int ties = 0;
    for (int i = 0; i < SLICE_N; i++)
        for (int j = i + 1; j < SLICE_N; j++)
            if (per_ev[i] == per_ev[j]) ties++;
    CHECK(ties == 0, "no two event ids have equal total time (%d ties)", ties);
    (void)fx;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3. THE DIFFERENTIALS
 *
 * Each diff_*() returns the number of DISAGREEING fields. It counts a
 * mismatch (never "equal") when either side is empty — see the non-vacuity
 * note in the header. `v` controls printing so the mutation probes in
 * section 4 can run the same comparator quietly.
 * ══════════════════════════════════════════════════════════════════════════ */

#define D(fmt, ...) do { if (v) printf("    DIFF %s: " fmt "\n", lbl, \
                                       ##__VA_ARGS__); n++; } while (0)

/* Bit-exact double comparison. No tolerance: see the header. */
static int dne(double a, double b) { return !(a == b); }

/* ── time_model ───────────────────────────────────────────────────────── */

static const struct pgwt_tm_row *tm_find(const struct pgwt_tm_result *r,
                                         const char *name, int indent)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].indent == indent && strcmp(r->rows[i].name, name) == 0)
            return &r->rows[i];
    return NULL;
}

static int diff_time_model(const char *lbl, const struct pgwt_tm_result *raw,
                           const struct pgwt_tm_result *sum, int v)
{
    int n = 0;
    if (raw->num_rows <= 0 || sum->num_rows <= 0)
        D("one side has no rows (raw %d, summary %d) -- an empty comparison "
          "is a FAILURE, not a pass", raw->num_rows, sum->num_rows);
    if (!(raw->db_time_ms > 0) || !(sum->db_time_ms > 0))
        D("one side has no DB Time (raw %.6f, summary %.6f)",
          raw->db_time_ms, sum->db_time_ms);

    if (dne(raw->db_time_ms, sum->db_time_ms))
        D("db_time_ms %.9f vs %.9f", raw->db_time_ms, sum->db_time_ms);
    if (dne(raw->idle_time_ms, sum->idle_time_ms))
        D("idle_time_ms %.9f vs %.9f", raw->idle_time_ms, sum->idle_time_ms);
    if (dne(raw->aas, sum->aas))
        D("aas %.12f vs %.12f", raw->aas, sum->aas);
    if (dne(raw->wall_ms, sum->wall_ms))
        D("wall_ms %.9f vs %.9f", raw->wall_ms, sum->wall_ms);
    if (dne(raw->idle_children_excess_ms, 0.0) ||
        dne(sum->idle_children_excess_ms, 0.0))
        D("idle_children_excess_ms must be 0 on both (%.9f / %.9f)",
          raw->idle_children_excess_ms, sum->idle_children_excess_ms);

    if (raw->num_rows != sum->num_rows)
        D("num_rows %d vs %d", raw->num_rows, sum->num_rows);
    /* Bijection on (name, indent): a row on one side only fails either here
     * or in the reverse pass below. */
    for (int i = 0; i < raw->num_rows; i++) {
        const struct pgwt_tm_row *a = &raw->rows[i];
        const struct pgwt_tm_row *b = tm_find(sum, a->name, a->indent);
        if (!b) { D("row '%s'(indent %d) absent from the summary path",
                    a->name, a->indent); continue; }
        if (dne(a->time_ms, b->time_ms))
            D("row '%s' time_ms %.9f vs %.9f", a->name, a->time_ms, b->time_ms);
        if (dne(a->pct_db_time, b->pct_db_time))
            D("row '%s' pct_db_time %.9f vs %.9f", a->name,
              a->pct_db_time, b->pct_db_time);
        if (dne(a->aas, b->aas))
            D("row '%s' aas %.12f vs %.12f", a->name, a->aas, b->aas);
    }
    for (int i = 0; i < sum->num_rows; i++) {
        const struct pgwt_tm_row *b = &sum->rows[i];
        if (!tm_find(raw, b->name, b->indent))
            D("row '%s'(indent %d) absent from the raw path",
              b->name, b->indent);
    }
    return n;
}

/* ── top_events ───────────────────────────────────────────────────────── */

static const struct pgwt_event_row *ev_find(const struct pgwt_events_result *r,
                                            uint32_t eid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == eid) return &r->rows[i];
    return NULL;
}

static int diff_top_events(const char *lbl, const struct pgwt_events_result *raw,
                           const struct pgwt_events_result *sum, int v)
{
    int n = 0;
    if (raw->num_rows <= 0 || sum->num_rows <= 0)
        D("one side has no rows (raw %d, summary %d)",
          raw->num_rows, sum->num_rows);
    if (dne(raw->db_time_ms, sum->db_time_ms))
        D("db_time_ms %.9f vs %.9f", raw->db_time_ms, sum->db_time_ms);
    if (raw->num_rows != sum->num_rows)
        D("num_rows %d vs %d", raw->num_rows, sum->num_rows);

    uint64_t total_count = 0;
    for (int i = 0; i < raw->num_rows; i++) {
        const struct pgwt_event_row *a = &raw->rows[i];
        const struct pgwt_event_row *b = ev_find(sum, a->event_id);
        total_count += a->count;
        if (!b) { D("event 0x%x ('%s') absent from the summary path",
                    a->event_id, a->name); continue; }
        if (strcmp(a->name, b->name) != 0)
            D("event 0x%x name '%s' vs '%s'", a->event_id, a->name, b->name);
        if (a->count != b->count)
            D("event '%s' count %llu vs %llu", a->name,
              (unsigned long long)a->count, (unsigned long long)b->count);
        if (a->exact_count != b->exact_count)
            D("event '%s' exact_count %llu vs %llu", a->name,
              (unsigned long long)a->exact_count,
              (unsigned long long)b->exact_count);
        if (dne(a->total_ms, b->total_ms))
            D("event '%s' total_ms %.9f vs %.9f", a->name,
              a->total_ms, b->total_ms);
        if (dne(a->avg_us, b->avg_us))
            D("event '%s' avg_us %.9f vs %.9f", a->name, a->avg_us, b->avg_us);
        if (a->has_latency_dist != b->has_latency_dist)
            D("event '%s' has_latency_dist %d vs %d", a->name,
              a->has_latency_dist, b->has_latency_dist);
        if (dne(a->max_us, b->max_us))
            D("event '%s' max_us %.9f vs %.9f", a->name, a->max_us, b->max_us);
        if (dne(a->p50_us, b->p50_us) || dne(a->p95_us, b->p95_us) ||
            dne(a->p99_us, b->p99_us))
            D("event '%s' percentiles (%.3f/%.3f/%.3f) vs (%.3f/%.3f/%.3f)",
              a->name, a->p50_us, a->p95_us, a->p99_us,
              b->p50_us, b->p95_us, b->p99_us);
        if (a->p50_overflow != b->p50_overflow ||
            a->p95_overflow != b->p95_overflow ||
            a->p99_overflow != b->p99_overflow)
            D("event '%s' overflow flags (%d,%d,%d) vs (%d,%d,%d)", a->name,
              a->p50_overflow, a->p95_overflow, a->p99_overflow,
              b->p50_overflow, b->p95_overflow, b->p99_overflow);
        if (dne(a->pct_db, b->pct_db))
            D("event '%s' pct_db %.9f vs %.9f", a->name, a->pct_db, b->pct_db);
        if (dne(a->aas, b->aas))
            D("event '%s' aas %.12f vs %.12f", a->name, a->aas, b->aas);
    }
    for (int i = 0; i < sum->num_rows; i++)
        if (!ev_find(raw, sum->rows[i].event_id))
            D("event 0x%x ('%s') absent from the raw path",
              sum->rows[i].event_id, sum->rows[i].name);
    if (total_count == 0)
        D("the compared rows carry no events at all (count sum 0)");
    return n;
}

/* ── top_sessions ─────────────────────────────────────────────────────── */

static const struct pgwt_session_row *se_find(
    const struct pgwt_sessions_result *r, uint32_t pid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].pid == pid) return &r->rows[i];
    return NULL;
}

static int diff_top_sessions(const char *lbl,
                             const struct pgwt_sessions_result *raw,
                             const struct pgwt_sessions_result *sum, int v)
{
    int n = 0;
    if (raw->num_rows <= 0 || sum->num_rows <= 0)
        D("one side has no rows (raw %d, summary %d)",
          raw->num_rows, sum->num_rows);

    double total = 0;
    for (int i = 0; i < raw->num_rows; i++) {
        const struct pgwt_session_row *a = &raw->rows[i];
        const struct pgwt_session_row *b = se_find(sum, a->pid);
        total += a->db_time_ms;
        if (!b) { D("pid %u absent from the summary path", a->pid); continue; }
        if (dne(a->db_time_ms, b->db_time_ms))
            D("pid %u db_time_ms %.9f vs %.9f", a->pid,
              a->db_time_ms, b->db_time_ms);
        if (dne(a->cpu_pct, b->cpu_pct))
            D("pid %u cpu_pct %.9f vs %.9f", a->pid, a->cpu_pct, b->cpu_pct);
        if (dne(a->wait_pct, b->wait_pct))
            D("pid %u wait_pct %.9f vs %.9f", a->pid, a->wait_pct, b->wait_pct);
        /* top_wait_id: N4 -- two different definitions. The fix-proof
         * invariant instead: whatever the summary path names must be a
         * non-idle event the pid really had. Checked by the caller, which has
         * the raw per-pid event list. */
    }
    /* A row present on ONE side only is a failure UNLESS it carries no DB
     * Time: the summary writer opens a session slot for every event including
     * idle ones (src/summary_writer.c find_or_insert_session), while the raw
     * path skips idle records outright, so an idle-only pid legitimately
     * appears with db_time_ms == 0 on the summary side only. Phrased as an
     * invariant rather than as "expect exactly one extra row" so it stays
     * green whichever way that is settled. */
    for (int i = 0; i < sum->num_rows; i++) {
        const struct pgwt_session_row *b = &sum->rows[i];
        if (se_find(raw, b->pid)) continue;
        if (b->db_time_ms != 0.0)
            D("pid %u carries %.9f ms on the summary path and does not exist "
              "on the raw path", b->pid, b->db_time_ms);
    }
    if (!(total > 0))
        D("the compared sessions carry no DB Time at all");
    return n;
}

/* ── top_queries ──────────────────────────────────────────────────────── */

static const struct pgwt_query_row *q_find(const struct pgwt_queries_result *r,
                                           uint64_t qid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].query_id == qid) return &r->rows[i];
    return NULL;
}

static int diff_top_queries(const char *lbl,
                            const struct pgwt_queries_result *raw,
                            const struct pgwt_queries_result *sum, int v)
{
    int n = 0;
    if (raw->num_rows <= 0 || sum->num_rows <= 0)
        D("one side has no rows (raw %d, summary %d)",
          raw->num_rows, sum->num_rows);
    if (dne(raw->db_time_ms, sum->db_time_ms))
        D("db_time_ms %.9f vs %.9f", raw->db_time_ms, sum->db_time_ms);
    if (raw->num_rows != sum->num_rows)
        D("num_rows %d vs %d", raw->num_rows, sum->num_rows);

    double total = 0;
    for (int i = 0; i < raw->num_rows; i++) {
        const struct pgwt_query_row *a = &raw->rows[i];
        const struct pgwt_query_row *b = q_find(sum, a->query_id);
        total += a->total_ms;
        if (!b) { D("query 0x%llx absent from the summary path",
                    (unsigned long long)a->query_id); continue; }
        if (a->count != b->count)
            D("query 0x%llx count %llu vs %llu",
              (unsigned long long)a->query_id,
              (unsigned long long)a->count, (unsigned long long)b->count);
        if (dne(a->total_ms, b->total_ms))
            D("query 0x%llx total_ms %.9f vs %.9f",
              (unsigned long long)a->query_id, a->total_ms, b->total_ms);
        if (dne(a->avg_us, b->avg_us))
            D("query 0x%llx avg_us %.9f vs %.9f",
              (unsigned long long)a->query_id, a->avg_us, b->avg_us);
        if (dne(a->pct_db, b->pct_db))
            D("query 0x%llx pct_db %.9f vs %.9f",
              (unsigned long long)a->query_id, a->pct_db, b->pct_db);
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            if (dne(a->class_ms[c], b->class_ms[c]))
                D("query 0x%llx class_ms[%s] %.9f vs %.9f",
                  (unsigned long long)a->query_id, pgwt_class_names[c],
                  a->class_ms[c], b->class_ms[c]);
    }
    for (int i = 0; i < sum->num_rows; i++)
        if (!q_find(raw, sum->rows[i].query_id))
            D("query 0x%llx absent from the raw path",
              (unsigned long long)sum->rows[i].query_id);
    if (!(total > 0))
        D("the compared queries carry no time at all");
    return n;
}

/* ── aas ──────────────────────────────────────────────────────────────── */

static int diff_aas(const char *lbl, const struct pgwt_aas_result *raw,
                    const struct pgwt_aas_result *sum, int v)
{
    int n = 0;
    if (raw->num_buckets <= 0 || sum->num_buckets <= 0)
        D("one side has no buckets (raw %d, summary %d)",
          raw->num_buckets, sum->num_buckets);
    if (raw->num_buckets != sum->num_buckets)
        D("num_buckets %d vs %d", raw->num_buckets, sum->num_buckets);
    if (raw->bucket_ns != sum->bucket_ns)
        D("bucket_ns %llu vs %llu", (unsigned long long)raw->bucket_ns,
          (unsigned long long)sum->bucket_ns);
    if (dne(raw->max_aas, sum->max_aas))
        D("max_aas %.12f vs %.12f", raw->max_aas, sum->max_aas);

    int nb = raw->num_buckets < sum->num_buckets ? raw->num_buckets
                                                 : sum->num_buckets;
    double total = 0;
    for (int i = 0; i < nb; i++) {
        if (raw->buckets[i].start_ns != sum->buckets[i].start_ns)
            D("bucket %d start_ns %llu vs %llu", i,
              (unsigned long long)raw->buckets[i].start_ns,
              (unsigned long long)sum->buckets[i].start_ns);
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            total += raw->buckets[i].class_aas[c];
            if (dne(raw->buckets[i].class_aas[c], sum->buckets[i].class_aas[c]))
                D("bucket %d class_aas[%s] %.12f vs %.12f", i,
                  pgwt_class_names[c], raw->buckets[i].class_aas[c],
                  sum->buckets[i].class_aas[c]);
        }
    }
    if (!(total > 0)) D("every compared AAS bucket is zero");
    return n;
}

/* ── heatmap ──────────────────────────────────────────────────────────── */

static int diff_heatmap(const char *lbl, const struct pgwt_heatmap_result *raw,
                        const struct pgwt_heatmap_result *sum, int v)
{
    int n = 0;
    if (raw->num_buckets <= 0 || sum->num_buckets <= 0)
        D("one side has no buckets (raw %d, summary %d)",
          raw->num_buckets, sum->num_buckets);
    if (raw->total_events == 0 || sum->total_events == 0)
        D("one side has no events (raw %llu, summary %llu)",
          (unsigned long long)raw->total_events,
          (unsigned long long)sum->total_events);
    if (raw->num_buckets != sum->num_buckets)
        D("num_buckets %d vs %d", raw->num_buckets, sum->num_buckets);
    if (raw->bucket_ns != sum->bucket_ns)
        D("bucket_ns %llu vs %llu", (unsigned long long)raw->bucket_ns,
          (unsigned long long)sum->bucket_ns);
    if (raw->total_events != sum->total_events)
        D("total_events %llu vs %llu", (unsigned long long)raw->total_events,
          (unsigned long long)sum->total_events);
    if (raw->max_count != sum->max_count)
        D("max_count %llu vs %llu", (unsigned long long)raw->max_count,
          (unsigned long long)sum->max_count);

    int nb = raw->num_buckets < sum->num_buckets ? raw->num_buckets
                                                 : sum->num_buckets;
    for (int i = 0; i < nb; i++) {
        if (raw->times[i] != sum->times[i])
            D("bucket %d times %llu vs %llu", i,
              (unsigned long long)raw->times[i],
              (unsigned long long)sum->times[i]);
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
            size_t k = (size_t)i * HISTOGRAM_BUCKETS + b;
            if (raw->grid[k] != sum->grid[k])
                D("cell [t=%d, lat=%d] %llu vs %llu", i, b,
                  (unsigned long long)raw->grid[k],
                  (unsigned long long)sum->grid[k]);
        }
    }
    return n;
}

#undef D

/* ══════════════════════════════════════════════════════════════════════════
 * the per-command runners: compute BOTH paths over the identical tuple
 * ══════════════════════════════════════════════════════════════════════════ */

#define AAS_BUCKETS 13   /* 130 s / 13 = 10 s exactly: no partial last bucket,
                          * and >= the 1 s floor the summary paths apply, so
                          * both sides agree on bucket_ns by derivation rather
                          * than by luck. */

static void free_tm(struct pgwt_tm_result *r) { free(r->rows); }
static void free_ev(struct pgwt_events_result *r) { free(r->rows); }
static void free_se(struct pgwt_sessions_result *r) { free(r->rows); }
static void free_q(struct pgwt_queries_result *r) { free(r->rows); }
static void free_aas(struct pgwt_aas_result *r) { free(r->buckets);
                                                  free(r->event_aas); }
static void free_hm(struct pgwt_heatmap_result *r) { free(r->grid);
                                                     free(r->times); }

static void run_time_model(const char *lbl, const struct fixture *fx,
                           const struct pgwt_filter *f)
{
    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, f, fx->from_ns, fx->to_ns,
                            fx->wall_ms, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, fx->from_ns, fx->to_ns, f,
                                           fx->wall_ms, &sum);
    int d = diff_time_model(lbl, &raw, &sum, 1);
    CHECK(d == 0, "time_model [%s]: %d disagreeing field(s)", lbl, d);
    CHECK(diff_time_model(lbl, &raw, &raw, 0) == 0,
          "time_model [%s]: the comparator agrees with itself", lbl);
    free_tm(&raw); free_tm(&sum);
}

static void run_top_events(const char *lbl, const struct fixture *fx,
                           const struct pgwt_filter *f)
{
    struct pgwt_events_result raw, sum;
    pgwt_compute_top_events(fx->wall, fx->n, f, fx->from_ns, fx->to_ns,
                            fx->wall_ms, &raw);
    pgwt_compute_top_events_from_summaries(fx->dir, fx->from_ns, fx->to_ns, f,
                                           fx->wall_ms, &sum);
    int d = diff_top_events(lbl, &raw, &sum, 1);
    CHECK(d == 0, "top_events [%s]: %d disagreeing field(s)", lbl, d);
    free_ev(&raw); free_ev(&sum);
}

/* N4's fix-proof invariant: the summary path's top wait must be an event the
 * pid really had, with non-zero raw time, and never idle or 0. */
static void check_session_top_wait(const struct fixture *fx,
                                   const struct pgwt_sessions_result *sum)
{
    int checked = 0;
    for (int i = 0; i < sum->num_rows; i++) {
        const struct pgwt_session_row *b = &sum->rows[i];
        if (b->top_wait_id == 0) continue;   /* "CPU*" / nothing selected */
        struct pgwt_filter pf;
        memset(&pf, 0, sizeof(pf));
        pf.pid = b->pid;
        struct pgwt_events_result er;
        pgwt_compute_top_events(fx->wall, fx->n, &pf, fx->from_ns, fx->to_ns,
                                fx->wall_ms, &er);
        const struct pgwt_event_row *r = ev_find(&er, b->top_wait_id);
        CHECK(r != NULL && r->total_ms > 0,
              "pid %u: the summary path's top wait 0x%x ('%s') is an event the "
              "pid really had, with non-zero raw time", b->pid,
              b->top_wait_id, b->top_wait);
        CHECK(!pgwt_is_idle_event(b->top_wait_id),
              "pid %u: the summary path's top wait is not an IDLE event "
              "(0x%x '%s')", b->pid, b->top_wait_id, b->top_wait);
        checked++;
        free_ev(&er);
    }
    CHECK(checked > 0,
          "the top-wait invariant was applied to at least one session (%d)",
          checked);
}

static void run_top_sessions(const char *lbl, const struct fixture *fx,
                             const struct pgwt_filter *f)
{
    struct pgwt_sessions_result raw, sum;
    pgwt_compute_top_sessions(fx->wall, fx->n, f, fx->from_ns, fx->to_ns,
                              fx->wall_ms, &raw);
    pgwt_compute_top_sessions_from_summaries(fx->dir, fx->from_ns, fx->to_ns, f,
                                             fx->wall_ms, &sum);
    int d = diff_top_sessions(lbl, &raw, &sum, 1);
    CHECK(d == 0, "top_sessions [%s]: %d disagreeing field(s)", lbl, d);
    check_session_top_wait(fx, &sum);
    free_se(&raw); free_se(&sum);
}

static void run_top_queries(const char *lbl, const struct fixture *fx,
                            const struct pgwt_filter *f)
{
    struct pgwt_queries_result raw, sum;
    pgwt_compute_top_queries(fx->wall, fx->n, f, fx->from_ns, fx->to_ns,
                             fx->wall_ms, &raw);
    pgwt_compute_top_queries_from_summaries(fx->dir, fx->from_ns, fx->to_ns, f,
                                            fx->wall_ms, &sum);
    int d = diff_top_queries(lbl, &raw, &sum, 1);
    CHECK(d == 0, "top_queries [%s]: %d disagreeing field(s)", lbl, d);
    free_q(&raw); free_q(&sum);
}

static void run_aas(const char *lbl, const struct fixture *fx,
                    const struct pgwt_filter *f)
{
    struct pgwt_aas_result raw, sum;
    pgwt_compute_aas(fx->wall, fx->n, f, fx->from_ns, fx->to_ns, AAS_BUCKETS,
                     0, AAS_MAX_EVENT_SERIES, &raw);
    pgwt_compute_aas_from_summaries(fx->dir, fx->from_ns, fx->to_ns, f,
                                    AAS_BUCKETS, &sum);
    int d = diff_aas(lbl, &raw, &sum, 1);
    CHECK(d == 0, "aas [%s]: %d disagreeing field(s)", lbl, d);
    free_aas(&raw); free_aas(&sum);
}

static void run_heatmap(const char *lbl, const struct fixture *fx,
                        const struct pgwt_filter *f)
{
    struct pgwt_heatmap_result raw, sum;
    pgwt_compute_heatmap(fx->wall, fx->n, f, fx->from_ns, fx->to_ns,
                         AAS_BUCKETS, &raw);
    pgwt_compute_heatmap_from_summaries(fx->dir, fx->from_ns, fx->to_ns, f,
                                        AAS_BUCKETS, &sum);
    int d = diff_heatmap(lbl, &raw, &sum, 1);
    CHECK(d == 0, "heatmap [%s]: %d disagreeing field(s)", lbl, d);
    free_hm(&raw); free_hm(&sum);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 1. PAIR COVERAGE — the part that makes the derived-tier plan safe.
 *
 * Every pgwt_compute_*_from_summaries() declared in src/compute.h must appear
 * below. Adding a new aggregate path without a differential turns this RED.
 * A header this cannot read or parse REFUSES: a gate that cannot see must
 * never approve.
 * ══════════════════════════════════════════════════════════════════════════ */

/* `filters` is a bitmask of which filter shapes the pair is compared under.
 * A 0 bit is a NAMED HOLE (N5/N6 in the header), never an omission. */
#define F_NONE  1
#define F_CLASS 2
#define F_EVENT 4
#define F_QUERY 8

struct pair {
    const char *summary_fn;   /* exactly as declared in src/compute.h */
    void (*run)(const char *, const struct fixture *,
                const struct pgwt_filter *);
    int  filters;
    const char *hole;         /* why the missing filters are missing */
};

static const struct pair PAIRS[] = {
    { "pgwt_compute_time_model_from_summaries",   run_time_model,
      F_NONE | F_CLASS | F_EVENT | F_QUERY, NULL },
    { "pgwt_compute_top_events_from_summaries",   run_top_events,
      F_NONE | F_CLASS | F_EVENT,
      "N9: under a QUERY filter the summary path reads the per-query "
      "top_events[] list, which (a) is never given event_id 0, so the CPU* "
      "row is absent and db_time_ms is short by the query's whole CPU time "
      "(src/summary_writer.c accum_query_add: `if (old_ev != 0)`) and (b) "
      "carries no histogram and no max, so the latency columns come back as "
      "'absent' (has_latency_dist = 0, which the server renders as null -- "
      "that part is deliberate, #103). Measured on this fixture: raw DB Time "
      "7002.426 ms vs summary 2949.426 ms, -57.9%. REACHABLE: "
      "handle_top_events (src/server.c:3596) adds no query guard, unlike "
      "handle_top_sessions and handle_heatmap" },
    { "pgwt_compute_top_sessions_from_summaries", run_top_sessions,
      F_NONE,
      "N5: ts_summary_visitor (src/compute.c) reads f->pid ONLY -- "
      "class_name, event_id and query_id are ignored. handle_top_sessions "
      "(src/server.c:3688) guards only `query_id == 0`, so a CLASS- or "
      "EVENT-filtered Sessions tab over a >= 120 s window gets UNFILTERED "
      "numbers" },
    { "pgwt_compute_top_queries_from_summaries",  run_top_queries,
      F_NONE | F_QUERY,
      "N10/N11: under a CLASS filter the summary path filters total_ns but "
      "adds the query's UNFILTERED sq->count (260 raw vs 390 summary here), "
      "and under an EVENT-only filter it accumulates EVERY class into "
      "class_ms (class_ms[cpu] 0 raw vs 4053.000 summary). NOT reachable: "
      "handle_top_queries (src/server.c:3741,3762) forces raw whenever "
      "class_name or event_id is set, so the server only ever takes the two "
      "shapes compared here -- unfiltered and query-filtered" },
    { "pgwt_compute_aas_from_summaries",          run_aas,
      F_NONE | F_CLASS | F_EVENT | F_QUERY, NULL },
    { "pgwt_compute_heatmap_from_summaries",      run_heatmap,
      F_NONE | F_CLASS | F_EVENT,
      "N6: the per-second records carry no per-query histogram, so "
      "hm_summary_visitor cannot honour a query filter. NOT reachable: "
      "handle_heatmap (src/server.c:3968) guards `query_id == 0`" },
};
#define NPAIRS ((int)(sizeof(PAIRS) / sizeof(PAIRS[0])))

/* Scan `text` for identifiers matching pgwt_compute_<x>_from_summaries.
 * Returns the count found; names are written into out[] (deduplicated). */
static int scan_summary_fns(const char *text, char out[][96], int max)
{
    int n = 0;
    const char *p = text;
    const char *needle = "pgwt_compute_";
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p;
        while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
        size_t len = (size_t)(q - p);
        const size_t tail = sizeof("_from_summaries") - 1;
        /* strncmp, not strcmp: `q` points at the character AFTER the
         * identifier, which is not a NUL in a slurped header, so strcmp would
         * compare past the suffix and never match. That bug made the parse
         * return 0 names -- and the "nf > 0" refusal in test_pair_coverage is
         * the only thing that stopped it reading as full coverage. */
        if (len < sizeof(out[0]) && len > tail &&
            strncmp(q - tail, "_from_summaries", tail) == 0) {
            char buf[96];
            memcpy(buf, p, len);
            buf[len] = '\0';
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (strcmp(out[i], buf) == 0) { dup = 1; break; }
            if (!dup && n < max) snprintf(out[n++], sizeof(out[0]), "%s", buf);
        }
        p = q;
    }
    return n;
}

/* Read a whole file. Returns malloc'd NUL-terminated text, or NULL. */
static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static void test_pair_coverage(void)
{
    printf("--- 1. every summary path in src/compute.h is cross-checked "
           "here ---\n");
    /* Run from tests/ (both `make -C tests check` and tests/run_all.sh do),
     * with a fallback for a repo-root invocation. */
    const char *cands[] = { "../src/compute.h", "src/compute.h" };
    char *text = NULL;
    const char *used = NULL;
    for (int i = 0; i < 2 && !text; i++) {
        text = slurp(cands[i]);
        if (text) used = cands[i];
    }
    /* REFUSE, never approve: with no header there is nothing to compare the
     * registry against, and "nothing missing" would be a false negative. */
    CHECK(text != NULL,
          "src/compute.h is readable (tried ../src/compute.h and "
          "src/compute.h) -- a coverage gate that cannot read the source "
          "must FAIL, not report full coverage");
    if (!text) return;

    char found[64][96];
    int nf = scan_summary_fns(text, found, 64);
    free(text);
    CHECK(nf > 0, "the parse found at least one pgwt_compute_*_from_summaries "
                  "in %s (found %d) -- an empty parse must never read as "
                  "'everything is covered'", used, nf);
    if (nf == 0) return;
    CHECK(NPAIRS > 0, "the registry is non-empty (%d pairs)", NPAIRS);

    /* Every declared summary path must be in the registry. */
    for (int i = 0; i < nf; i++) {
        int seen = 0;
        for (int j = 0; j < NPAIRS; j++)
            if (strcmp(PAIRS[j].summary_fn, found[i]) == 0) { seen = 1; break; }
        CHECK(seen, "%s() is declared in src/compute.h but has NO raw-vs-"
                    "summary differential in this file -- add one before the "
                    "path ships", found[i]);
    }
    /* And every registry entry must still exist, so a renamed or deleted path
     * does not leave a differential silently testing nothing. */
    for (int j = 0; j < NPAIRS; j++) {
        int seen = 0;
        for (int i = 0; i < nf; i++)
            if (strcmp(PAIRS[j].summary_fn, found[i]) == 0) { seen = 1; break; }
        CHECK(seen, "this file cross-checks %s() but src/compute.h no longer "
                    "declares it", PAIRS[j].summary_fn);
    }
    printf("    %d summary path(s) declared, %d cross-checked\n", nf, NPAIRS);
    for (int j = 0; j < NPAIRS; j++)
        if (PAIRS[j].hole)
            printf("    HOLE %s: %s\n", PAIRS[j].summary_fn, PAIRS[j].hole);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3. run the matrix
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_matrix(const struct fixture *fx)
{
    printf("--- 3. raw vs summary over the same committed blocks ---\n");

    struct pgwt_filter f_none, f_class, f_event, f_query;
    memset(&f_none,  0, sizeof(f_none));
    memset(&f_class, 0, sizeof(f_class));
    memset(&f_event, 0, sizeof(f_event));
    memset(&f_query, 0, sizeof(f_query));
    snprintf(f_class.class_name, sizeof(f_class.class_name), "io");
    f_event.event_id = EV_LOCK_A;
    f_query.query_id = QID_A;

    for (int j = 0; j < NPAIRS; j++) {
        if (PAIRS[j].filters & F_NONE)
            PAIRS[j].run("unfiltered", fx, &f_none);
        if (PAIRS[j].filters & F_CLASS)
            PAIRS[j].run("class=io", fx, &f_class);
        if (PAIRS[j].filters & F_EVENT)
            PAIRS[j].run("event=Lock[0]", fx, &f_event);
        if (PAIRS[j].filters & F_QUERY)
            PAIRS[j].run("query=A", fx, &f_query);
    }
}

/* Both paths must also match the fixture's OWN expected totals, which come
 * from the slice table and from neither path. Two paths that agree with each
 * other and disagree with the fixture is the one shape mutual comparison
 * cannot see. */
static void test_against_the_fixture(const struct fixture *fx)
{
    printf("--- 3b. both paths match the fixture's independent totals ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                            fx->wall_ms, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, fx->from_ns, fx->to_ns, &f,
                                           fx->wall_ms, &sum);
    CHECK(raw.db_time_ms == fx->exp_db_ms,
          "raw DB Time %.6f == fixture %.6f ms", raw.db_time_ms, fx->exp_db_ms);
    CHECK(sum.db_time_ms == fx->exp_db_ms,
          "summary DB Time %.6f == fixture %.6f ms", sum.db_time_ms,
          fx->exp_db_ms);
    CHECK(raw.idle_time_ms == fx->exp_idle_ms,
          "raw Idle %.6f == fixture %.6f ms", raw.idle_time_ms,
          fx->exp_idle_ms);
    CHECK(sum.idle_time_ms == fx->exp_idle_ms,
          "summary Idle %.6f == fixture %.6f ms", sum.idle_time_ms,
          fx->exp_idle_ms);
    free_tm(&raw); free_tm(&sum);

    struct pgwt_heatmap_result hraw, hsum;
    pgwt_compute_heatmap(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                         AAS_BUCKETS, &hraw);
    pgwt_compute_heatmap_from_summaries(fx->dir, fx->from_ns, fx->to_ns, &f,
                                        AAS_BUCKETS, &hsum);
    /* The hidden class (Activity) is excluded from the heatmap on both paths,
     * so the expected population is every event minus the Activity ones. */
    uint64_t exp_visible = 0;
    for (int i = 0; i < SLICE_N; i++)
        if (!pgwt_is_hidden_event(slice[i].ev)) exp_visible += SECS;
    CHECK(hraw.total_events == exp_visible,
          "raw heatmap population %llu == fixture %llu",
          (unsigned long long)hraw.total_events,
          (unsigned long long)exp_visible);
    CHECK(hsum.total_events == exp_visible,
          "summary heatmap population %llu == fixture %llu",
          (unsigned long long)hsum.total_events,
          (unsigned long long)exp_visible);
    free_hm(&hraw); free_hm(&hsum);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 4. MUTATION PROBES
 *
 * Section 3 is satisfied by a comparator that returns 0 unconditionally. Each
 * probe below perturbs ONE field in a COPY of the summary-side result and
 * requires the same comparator to see it. The mutations are in copies, never
 * in the shipped tree, so they stay valid when the product changes.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_mutation_probes(const struct fixture *fx)
{
    printf("--- 4. each comparator can actually go red ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    const double ONE_NS_MS = 1e-6;   /* the smallest real misaccounting */

    /* 4a. time_model: one nanosecond of DB Time, and a deleted row. */
    {
        struct pgwt_tm_result raw, sum;
        pgwt_compute_time_model(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                                fx->wall_ms, &raw);
        pgwt_compute_time_model_from_summaries(fx->dir, fx->from_ns, fx->to_ns,
                                               &f, fx->wall_ms, &sum);
        CHECK(diff_time_model("probe", &raw, &sum, 0) == 0,
              "4a: unperturbed time_model agrees (the probe's baseline)");

        struct pgwt_tm_result m = sum;
        m.db_time_ms += ONE_NS_MS;
        CHECK(diff_time_model("probe", &raw, &m, 0) > 0,
              "4a: ONE NANOSECOND of extra DB Time is caught");

        m = sum; m.idle_time_ms += ONE_NS_MS;
        CHECK(diff_time_model("probe", &raw, &m, 0) > 0,
              "4a: one nanosecond of extra Idle is caught");

        m = sum;
        struct pgwt_tm_row *rows = calloc((size_t)sum.num_rows, sizeof(*rows));
        memcpy(rows, sum.rows, (size_t)sum.num_rows * sizeof(*rows));
        m.rows = rows;
        m.num_rows = sum.num_rows;
        rows[sum.num_rows - 1].time_ms += ONE_NS_MS;
        CHECK(diff_time_model("probe", &raw, &m, 0) > 0,
              "4a: one nanosecond on a single ROW is caught");
        /* A row that is absent rather than wrong: restore every value and
         * drop only the last row from the count. */
        memcpy(rows, sum.rows, (size_t)sum.num_rows * sizeof(*rows));
        m.num_rows = sum.num_rows - 1;
        CHECK(diff_time_model("probe", &raw, &m, 0) > 0,
              "4a: a MISSING row is caught (absent, not wrong)");
        free(rows);
        free_tm(&raw); free_tm(&sum);
    }

    /* 4b. top_events: count, total, percentile bucket, overflow flag. */
    {
        struct pgwt_events_result raw, sum;
        pgwt_compute_top_events(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                                fx->wall_ms, &raw);
        pgwt_compute_top_events_from_summaries(fx->dir, fx->from_ns, fx->to_ns,
                                               &f, fx->wall_ms, &sum);
        CHECK(diff_top_events("probe", &raw, &sum, 0) == 0,
              "4b: unperturbed top_events agrees (the probe's baseline)");
        struct pgwt_event_row *rows = calloc((size_t)sum.num_rows,
                                             sizeof(*rows));
        struct pgwt_events_result m = sum;
        m.rows = rows;
#define MUT_EV(stmt, what) do {                                            \
            memcpy(rows, sum.rows, (size_t)sum.num_rows * sizeof(*rows));  \
            m.num_rows = sum.num_rows;                                     \
            stmt;                                                          \
            CHECK(diff_top_events("probe", &raw, &m, 0) > 0,               \
                  "4b: " what " is caught");                               \
        } while (0)
        MUT_EV(rows[0].count += 1,            "a single extra event COUNT");
        MUT_EV(rows[0].total_ms += ONE_NS_MS, "one nanosecond of event time");
        MUT_EV(rows[0].exact_count += 1,      "one extra exact_count");
        MUT_EV(rows[0].p95_us *= 2.0,         "a p95 from the wrong bucket");
        MUT_EV(rows[0].p99_overflow ^= 1,     "a flipped p99_overflow flag");
        MUT_EV(rows[0].max_us += 1.0,         "a one-microsecond max_us drift");
        MUT_EV(rows[0].aas += 1e-12,          "a 1e-12 AAS drift");
        MUT_EV(rows[0].name[0] = 'Z',         "a renamed event");
        MUT_EV(m.num_rows = sum.num_rows - 1, "a MISSING event row");
        MUT_EV(m.db_time_ms += ONE_NS_MS,     "one nanosecond of DB Time");
#undef MUT_EV
        free(rows);
        free_ev(&raw); free_ev(&sum);
    }

    /* 4c. top_sessions. */
    {
        struct pgwt_sessions_result raw, sum;
        pgwt_compute_top_sessions(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                                  fx->wall_ms, &raw);
        pgwt_compute_top_sessions_from_summaries(fx->dir, fx->from_ns,
                                                 fx->to_ns, &f, fx->wall_ms,
                                                 &sum);
        CHECK(diff_top_sessions("probe", &raw, &sum, 0) == 0,
              "4c: unperturbed top_sessions agrees (the probe's baseline)");
        struct pgwt_session_row *rows = calloc((size_t)sum.num_rows,
                                               sizeof(*rows));
        struct pgwt_sessions_result m = sum;
        m.rows = rows;
#define MUT_SE(stmt, what) do {                                            \
            memcpy(rows, sum.rows, (size_t)sum.num_rows * sizeof(*rows));  \
            m.num_rows = sum.num_rows;                                     \
            stmt;                                                          \
            CHECK(diff_top_sessions("probe", &raw, &m, 0) > 0,             \
                  "4c: " what " is caught");                               \
        } while (0)
        MUT_SE(rows[0].db_time_ms += ONE_NS_MS, "one nanosecond of session time");
        MUT_SE(rows[0].cpu_pct += 1e-12,        "a 1e-12 cpu_pct drift");
        MUT_SE(rows[0].wait_pct += 1e-12,       "a 1e-12 wait_pct drift");
        /* A session that is PRESENT ON ONE SIDE ONLY with real time on it --
         * the shape the zero-time carve-out must not let through. */
        MUT_SE(rows[0].pid = 999999,            "a session that exists only on "
                                                "the summary path, carrying time");
        /* Drop the BUSIEST row (index 0 -- the result is sorted descending),
         * not the last: the summary side legitimately carries extra
         * zero-time idle-only pids at the tail, so truncating the array
         * would remove one of those and correctly change nothing. */
        MUT_SE((memmove(rows, rows + 1,
                        (size_t)(sum.num_rows - 1) * sizeof(*rows)),
                m.num_rows = sum.num_rows - 1),
               "a MISSING session row (the busiest one)");
#undef MUT_SE
        free(rows);
        free_se(&raw); free_se(&sum);
    }

    /* 4d. top_queries. */
    {
        struct pgwt_queries_result raw, sum;
        pgwt_compute_top_queries(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                                 fx->wall_ms, &raw);
        pgwt_compute_top_queries_from_summaries(fx->dir, fx->from_ns, fx->to_ns,
                                                &f, fx->wall_ms, &sum);
        CHECK(diff_top_queries("probe", &raw, &sum, 0) == 0,
              "4d: unperturbed top_queries agrees (the probe's baseline)");
        struct pgwt_query_row *rows = calloc((size_t)sum.num_rows,
                                             sizeof(*rows));
        struct pgwt_queries_result m = sum;
        m.rows = rows;
#define MUT_Q(stmt, what) do {                                             \
            memcpy(rows, sum.rows, (size_t)sum.num_rows * sizeof(*rows));  \
            m.num_rows = sum.num_rows;                                     \
            stmt;                                                          \
            CHECK(diff_top_queries("probe", &raw, &m, 0) > 0,              \
                  "4d: " what " is caught");                               \
        } while (0)
        MUT_Q(rows[0].total_ms += ONE_NS_MS, "one nanosecond of query time");
        MUT_Q(rows[0].count += 1,            "one extra query count");
        MUT_Q(rows[0].class_ms[PGWT_CLASS_IO] += ONE_NS_MS,
              "one nanosecond in a single class column");
        MUT_Q(rows[0].query_id ^= 1ULL,      "a query present on one side only");
        MUT_Q(m.num_rows = sum.num_rows - 1, "a MISSING query row");
#undef MUT_Q
        free(rows);
        free_q(&raw); free_q(&sum);
    }

    /* 4e. aas. */
    {
        struct pgwt_aas_result raw, sum;
        pgwt_compute_aas(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                         AAS_BUCKETS, 0, AAS_MAX_EVENT_SERIES, &raw);
        pgwt_compute_aas_from_summaries(fx->dir, fx->from_ns, fx->to_ns, &f,
                                        AAS_BUCKETS, &sum);
        CHECK(diff_aas("probe", &raw, &sum, 0) == 0,
              "4e: unperturbed aas agrees (the probe's baseline)");
        struct pgwt_aas_bucket *bk = calloc((size_t)sum.num_buckets,
                                            sizeof(*bk));
        struct pgwt_aas_result m = sum;
        m.buckets = bk;
#define MUT_AAS(stmt, what) do {                                            \
            memcpy(bk, sum.buckets, (size_t)sum.num_buckets * sizeof(*bk)); \
            m.num_buckets = sum.num_buckets;                                \
            stmt;                                                           \
            CHECK(diff_aas("probe", &raw, &m, 0) > 0,                       \
                  "4e: " what " is caught");                                \
        } while (0)
        /* One nanosecond inside one 10 s bucket is 1e-10 of AAS. */
        MUT_AAS(bk[0].class_aas[PGWT_CLASS_IO] += 1e-10,
                "one nanosecond inside ONE bucket's ONE class");
        MUT_AAS(bk[sum.num_buckets - 1].class_aas[PGWT_CLASS_LOCK] += 1e-10,
                "the same in the LAST bucket (not just the first)");
        MUT_AAS(bk[0].start_ns += 1,       "a one-nanosecond bucket shift");
        MUT_AAS(m.num_buckets = sum.num_buckets - 1, "a MISSING bucket");
        MUT_AAS(m.bucket_ns += 1,          "a different bucket width");
#undef MUT_AAS
        free(bk);
        free_aas(&raw); free_aas(&sum);
    }

    /* 4f. heatmap. */
    {
        struct pgwt_heatmap_result raw, sum;
        pgwt_compute_heatmap(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                             AAS_BUCKETS, &raw);
        pgwt_compute_heatmap_from_summaries(fx->dir, fx->from_ns, fx->to_ns, &f,
                                            AAS_BUCKETS, &sum);
        CHECK(diff_heatmap("probe", &raw, &sum, 0) == 0,
              "4f: unperturbed heatmap agrees (the probe's baseline)");
        size_t cells = (size_t)sum.num_buckets * HISTOGRAM_BUCKETS;
        uint64_t *grid = calloc(cells, sizeof(*grid));
        struct pgwt_heatmap_result m = sum;
        m.grid = grid;
#define MUT_HM(stmt, what) do {                                      \
            memcpy(grid, sum.grid, cells * sizeof(*grid));           \
            m.total_events = sum.total_events;                       \
            m.max_count = sum.max_count;                             \
            stmt;                                                    \
            CHECK(diff_heatmap("probe", &raw, &m, 0) > 0,            \
                  "4f: " what " is caught");                         \
        } while (0)
        MUT_HM(grid[0] += 1,                 "one extra count in cell [0,0]");
        MUT_HM(grid[cells - 1] += 1,         "one extra count in the LAST cell");
        MUT_HM(m.total_events += 1,          "a one-event population drift");
        MUT_HM(m.max_count += 1,             "a one-count max_count drift");
#undef MUT_HM
        free(grid);
        free_hm(&raw); free_hm(&sum);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 5. THE BYPASS SUITE — every way this gate's detection can be made
 *    UNREACHABLE. Each case must be REFUSED, never read as agreement.
 * ══════════════════════════════════════════════════════════════════════════ */

static void bypass_empty_dir(void)
{
    printf("  5a. an EMPTY trace dir\n");
    const char *dir = fresh_dir("bypass_empty");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    uint64_t from = 1700000000ULL * ONE_SEC;
    uint64_t to   = from + (uint64_t)SECS * ONE_SEC;

    struct pgwt_tm_result raw, sum;
    struct pgwt_trace_event none;
    memset(&none, 0, sizeof(none));
    pgwt_compute_time_model(&none, 0, &f, from, to, 130000.0, &raw);
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, 130000.0, &sum);

    /* FIRST, show the vacuous pass is real: both sides ARE equal here. */
    CHECK(raw.db_time_ms == 0.0 && sum.db_time_ms == 0.0,
          "both paths answer 0 ms, so a naive equality check PASSES "
          "(raw %.6f, summary %.6f) -- this is the shape the non-vacuity "
          "gate exists for", raw.db_time_ms, sum.db_time_ms);
    /* THEN, the comparator must refuse it. */
    CHECK(diff_time_model("empty", &raw, &sum, 0) > 0,
          "5a: the comparator REFUSES an empty comparison instead of "
          "reporting agreement");
    free_tm(&raw); free_tm(&sum);

    /* And fixture_build itself must refuse to produce a fixture from it. */
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    int nrec = pgwt_visit_summaries(dir, 0, 0, rec_visitor, &rc);
    CHECK(rc.records == 0, "no records in an empty dir (visit returned %d)",
          nrec);
    CHECK(rc.min_sec == 0 && rc.max_sec == 0,
          "so window_from_records finds no bounds and fixture_build REFUSES "
          "to produce a fixture (min %llu, max %llu)",
          (unsigned long long)rc.min_sec, (unsigned long long)rc.max_sec);
}

static void bypass_missing_dir(void)
{
    printf("  5b. a trace dir that DOES NOT EXIST\n");
    char dir[700];
    snprintf(dir, sizeof(dir), "%s/bypass_absent", g_base);
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &rc);
    CHECK(rc.records == 0, "no records (%d)", rc.records);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_events_result raw, sum;
    struct pgwt_trace_event none;
    memset(&none, 0, sizeof(none));
    pgwt_compute_top_events(&none, 0, &f, 0, 0, 1.0, &raw);
    pgwt_compute_top_events_from_summaries(dir, 1, 2, &f, 1.0, &sum);
    CHECK(diff_top_events("absent", &raw, &sum, 0) > 0,
          "5b: the comparator refuses a missing directory");
    free_ev(&raw); free_ev(&sum);
}

static void bypass_window_no_overlap(const struct fixture *fx)
{
    printf("  5c. records exist but the WINDOW MISSES THEM\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    /* One hour before the data. The reader's file-level filter rejects the
     * file, pgwt_visit_summaries returns zero records, and every summary
     * total is 0 -- the exact shape that read as success in the first
     * version of tests/test_idle_summary.c. */
    uint64_t from = fx->from_ns - 7200ULL * ONE_SEC;
    uint64_t to   = from + (uint64_t)SECS * ONE_SEC;
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    pgwt_visit_summaries(fx->dir, from, to, rec_visitor, &rc);
    CHECK(rc.records == 0, "the window sees no records (%d)", rc.records);

    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, &f, from, to, fx->wall_ms, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, from, to, &f, fx->wall_ms,
                                           &sum);
    CHECK(diff_time_model("no-overlap", &raw, &sum, 0) > 0,
          "5c: a window with no data is REFUSED, not reported as agreement");
    free_tm(&raw); free_tm(&sum);
}

static void bypass_unreadable_file(const struct fixture *fx)
{
    printf("  5d. a summary file the reader cannot decode (visit_summaries "
           "SKIPS it)\n");
    /* Copy the fixture, then truncate one file. pgwt_visit_summaries skips a
     * file it cannot open and returns the rest -- a plausible PARTIAL answer
     * with no error anywhere. The conservation check is what sees it. */
    const char *dir = fresh_dir("bypass_corrupt");
    char cmd[1600];
    /* Copy everything: an unrotated writer leaves `current.summary` plus its
     * .meta, not a dated *.summary.lz4. */
    snprintf(cmd, sizeof(cmd), "cp '%s'/* '%s'/", fx->dir, dir);
    int cp = system(cmd);
    CHECK(cp == 0, "the fixture's summary files were copied (status %d)", cp);

    struct rec_ctx before;
    memset(&before, 0, sizeof(before));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &before);
    CHECK(before.records == fx->records,
          "the copy reads back whole first (%d == %d)", before.records,
          fx->records);

    /* Truncate every copied summary to nothing. */
    snprintf(cmd, sizeof(cmd),
             "for g in '%s'/*; do : > \"$g\"; done", dir);
    if (system(cmd) != 0) { /* ignore */ }

    struct rec_ctx after;
    memset(&after, 0, sizeof(after));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &after);
    CHECK(after.records < before.records,
          "the damaged copy yields FEWER records (%d < %d) and no error",
          after.records, before.records);
    CHECK(after.total_events != (uint64_t)fx->n,
          "so the conservation check (sum(total_events) == events pushed) "
          "FAILS: %llu != %d -- which is what stops a partial read from being "
          "compared as if it were complete",
          (unsigned long long)after.total_events, fx->n);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                            fx->wall_ms, &raw);
    pgwt_compute_time_model_from_summaries(dir, fx->from_ns, fx->to_ns, &f,
                                           fx->wall_ms, &sum);
    CHECK(diff_time_model("corrupt", &raw, &sum, 0) > 0,
          "5d: and the comparator sees the shortfall as a disagreement");
    free_tm(&raw); free_tm(&sum);
}

/* DB Time of one slice, in integer ns, from the slice table alone. */
static uint64_t slice_db_ns(int s)
{
    uint64_t ns = 0;
    for (int i = 0; i < SLICE_N; i++)
        if (!pgwt_is_idle_event(slice[i].ev)) ns += spec_dur_ns(i, s);
    return ns;
}

static void bypass_one_second_window(const struct fixture *fx)
{
    printf("  5e. a ONE-SECOND window -- narrow enough to be nearly empty, "
           "which is where a trivial comparison looks green\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    /* The LAST second of the capture. Deliberately not an interior second:
     * see 5h, which measures why. */
    uint64_t from = fx->to_ns - ONE_SEC;
    uint64_t to   = fx->to_ns;
    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, &f, from, to, 1000.0, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, from, to, &f, 1000.0, &sum);
    int d = diff_time_model("1 s window", &raw, &sum, 1);
    CHECK(d == 0, "5e: a 1 s window agrees too (%d disagreeing field(s))", d);
    CHECK(raw.db_time_ms > 0 && sum.db_time_ms > 0,
          "5e: and it is NOT empty (raw %.6f, summary %.6f ms)",
          raw.db_time_ms, sum.db_time_ms);
    CHECK(raw.db_time_ms == (double)slice_db_ns(SECS - 1) / 1e6,
          "5e: and it is the LAST slice's DB Time, not some other second "
          "(%.6f == %.6f ms)", raw.db_time_ms,
          (double)slice_db_ns(SECS - 1) / 1e6);
    free_tm(&raw); free_tm(&sum);
}

/* ── 5h. N8: the WINDOW END BOUND differs between the two paths ─────────
 *
 * pgwt_visit_summaries (src/summary_reader.c:474) skips a record only when
 * `rec_ns > to_wall_ns`, so the record whose second STARTS exactly at the
 * window end is INCLUDED -- a whole extra second, in full. Every other window
 * consumer is half-open: the raw path's event_window_ns clips at to_ns and
 * pgwt_compute_heatmap drops `ev_ts >= to_ns`. So a summary-path answer over
 * [T, T+W) covers W+1 seconds, and the extra second is placed at bucket index
 * (to-from)/bucket_ns, which clamps onto the LAST bucket -- so the last point
 * of an AAS chart is inflated by one whole second's load.
 *
 * It is invisible on a live "last 15 min" request because to_ns is ~now and
 * that second has not been flushed yet; it is NOT invisible on any historical
 * window, nor on this file's own interior windows.
 *
 * ASSERTED FIX-PROOF AND SHARP: the summary total must be EITHER exactly the
 * one slice inside the window (correct, half-open) OR exactly those two slices
 * (today's inclusive bound). Any third value -- a partial second, a different
 * second, a scaling error -- fails. The two candidates are asserted distinct
 * first, so the `||` cannot be satisfied vacuously.
 */
static void bypass_end_bound(const struct fixture *fx)
{
    printf("  5h. N8: the summary path's window END is INCLUSIVE\n");
    const int K = 10;                     /* an interior second */
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    uint64_t from = fx->from_ns + (uint64_t)K * ONE_SEC;
    uint64_t to   = from + ONE_SEC;

    uint64_t in_ns   = slice_db_ns(K);
    uint64_t next_ns = slice_db_ns(K + 1);
    CHECK(in_ns > 0 && next_ns > 0 && in_ns != next_ns,
          "the in-window and next-second DB Times are both non-zero and "
          "DIFFERENT (%llu vs %llu ns), so the two candidate answers below "
          "are distinguishable", (unsigned long long)in_ns,
          (unsigned long long)next_ns);

    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, &f, from, to, 1000.0, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, from, to, &f, 1000.0, &sum);

    double want_half_open = (double)in_ns / 1e6;
    double want_inclusive = (double)(in_ns + next_ns) / 1e6;
    CHECK(raw.db_time_ms == want_half_open,
          "the RAW path is half-open: %.6f == %.6f ms",
          raw.db_time_ms, want_half_open);
    CHECK(sum.db_time_ms == want_half_open || sum.db_time_ms == want_inclusive,
          "the SUMMARY path is either half-open (%.6f) or includes exactly "
          "the second at to_ns (%.6f) -- it answered %.6f ms",
          want_half_open, want_inclusive, sum.db_time_ms);
    printf("      raw %.6f ms, summary %.6f ms (%s; excess %.6f ms = %.1f%% "
           "of the window)\n", raw.db_time_ms, sum.db_time_ms,
           sum.db_time_ms == want_half_open ? "half-open, agreeing"
                                            : "END-INCLUSIVE, see N8",
           sum.db_time_ms - raw.db_time_ms,
           raw.db_time_ms > 0
               ? (sum.db_time_ms - raw.db_time_ms) / raw.db_time_ms * 100.0
               : 0.0);
    free_tm(&raw); free_tm(&sum);
}

static void bypass_coverage_blind(void)
{
    printf("  5f. the coverage gate's own blind spots\n");
    char found[8][96];
    /* A header with no summary declarations at all must parse to ZERO, so the
     * "nf > 0" refusal in test_pair_coverage fires rather than the loop
     * finding nothing missing and approving. */
    CHECK(scan_summary_fns("int pgwt_compute_time_model(void);\n", found, 8)
          == 0,
          "5f: a header with no *_from_summaries declaration parses to 0, "
          "which test_pair_coverage REFUSES");
    CHECK(scan_summary_fns("", found, 8) == 0, "5f: empty text parses to 0");
    CHECK(slurp("/nonexistent/compute.h") == NULL,
          "5f: an unreadable header returns NULL, which test_pair_coverage "
          "REFUSES");
    CHECK(slurp("/dev/null") == NULL,
          "5f: a zero-length header is refused too (not read as 'no paths "
          "declared, nothing to cover')");
    /* A truncated declaration must NOT be mistaken for a summary path... */
    CHECK(scan_summary_fns("void pgwt_compute_aas_from_summ(void);", found, 8)
          == 0, "5f: a near-miss name is not accepted as a covered path");
    /* ...and a real one must be found even when it appears only once. */
    CHECK(scan_summary_fns("void pgwt_compute_zz_from_summaries(int);",
                           found, 8) == 1 &&
          strcmp(found[0], "pgwt_compute_zz_from_summaries") == 0,
          "5f: a single new declaration IS found -- so a new aggregate path "
          "with no differential turns this test red");
    /* Dedup must not hide a second distinct name. */
    CHECK(scan_summary_fns("pgwt_compute_a_from_summaries "
                           "pgwt_compute_a_from_summaries "
                           "pgwt_compute_b_from_summaries", found, 8) == 2,
          "5f: duplicates collapse but distinct names do not");
}

static void bypass_absent_not_wrong(const struct fixture *fx)
{
    printf("  5g. the thing being checked is ABSENT rather than wrong\n");
    /* A filter that matches nothing. Both paths answer "no rows", which every
     * field-by-field loop satisfies vacuously -- so the comparator must count
     * it as a mismatch rather than as agreement. */
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    f.event_id = WEI(PG_WAIT_EXTENSION, 4242);   /* not in the fixture */
    struct pgwt_events_result raw, sum;
    pgwt_compute_top_events(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                            fx->wall_ms, &raw);
    pgwt_compute_top_events_from_summaries(fx->dir, fx->from_ns, fx->to_ns, &f,
                                           fx->wall_ms, &sum);
    CHECK(raw.num_rows == 0 && sum.num_rows == 0,
          "both paths return no rows for an event the fixture never had "
          "(raw %d, summary %d)", raw.num_rows, sum.num_rows);
    CHECK(diff_top_events("absent-filter", &raw, &sum, 0) > 0,
          "5g: 'both empty' is REFUSED, not counted as agreement");
    free_ev(&raw); free_ev(&sum);
}

static void test_bypass_suite(const struct fixture *fx)
{
    printf("--- 5. the bypass suite: every way this gate could be blind ---\n");
    bypass_empty_dir();
    bypass_missing_dir();
    bypass_window_no_overlap(fx);
    bypass_unreadable_file(fx);
    bypass_one_second_window(fx);
    bypass_coverage_blind();
    bypass_absent_not_wrong(fx);
    bypass_end_bound(fx);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_agg_raw_crosscheck: summary vs raw, same blocks, "
           "same window ===\n");

    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_crosscheck_%d", (int)getpid());
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }
    mkdir(g_base, 0755);

    test_pair_coverage();

    struct fixture fx;
    if (fixture_build(&fx, "fixture") == 0) {
        assert_fixture_live(&fx);
        assert_no_ties(&fx);
        test_matrix(&fx);
        test_against_the_fixture(&fx);
        test_mutation_probes(&fx);
        test_bypass_suite(&fx);
    } else {
        printf("  FAIL: could not build the fixture -- nothing was compared\n");
        tests_failed++;
    }
    fixture_free(&fx);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
