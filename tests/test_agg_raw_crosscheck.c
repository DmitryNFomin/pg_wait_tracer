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
 *      STILL OPEN as a product decision (#319), and now PINNED ON BOTH
 *      SIDES by section 6e rather than only described: that case builds its
 *      own 130 s stream (pid 301: 130 x 10 ms of Lock = 1300 ms total,
 *      10 ms max, against 1 x 40 ms of IO = 40 ms total, 40 ms max) and
 *      asserts raw names Lock, the summary path names IO, and both agree on
 *      db_time_ms 1340.000 — so it is a definition difference, not a
 *      counting one. "Largest total" is the right answer, but it is NOT
 *      derivable from the record: struct pgwt_summary_session carries one
 *      (top_wait_id, top_wait_ns) pair per second, and no sequence of
 *      per-second (id, max) pairs determines a window-wide argmax of
 *      TOTALS. Fixing it needs per-session per-event totals in the record,
 *      and any BOUNDED version of that re-introduces silent inexactness.
 *      The main fixture CANNOT express the divergence at all (every
 *      duration is base_us * (1 + s % 8), so totals and maxima rank
 *      identically for every pid), which is why 6e has its own stream; the
 *      invariant still asserted here is the fix-proof one — the summary's
 *      top wait must be an event the session/query ACTUALLY had in the
 *      window with non-zero raw time, never idle, never 0.
 *
 *  N5. top_sessions under a class / event / query filter — the per-second
 *      record has NO per-session breakdown by any of them
 *      (struct pgwt_summary_session is {pid, db_time_ns, cpu_ns,
 *      top_wait_id, top_wait_ns}), so the summary path cannot answer such a
 *      request at all. FIXED (#318) in the only two places it can be:
 *      handle_top_sessions now forces the RAW path for a class- or
 *      event-filtered request as well as a query-filtered one, and
 *      ts_summary_visitor REFUSES the filter (no rows) instead of returning
 *      the UNFILTERED numbers under a filtered label, which is what it used
 *      to do — reachable, since should_use_summaries blocks only pid.
 *      Compared UNFILTERED here; the refusal and its non-vacuity are
 *      section 6d.
 *
 *  N6. heatmap under a query filter — hm_summary_visitor consults
 *      summary_event_matches_filter (class/event) but there is no per-query
 *      histogram in the records, so query_id is ignored. Also reachable. See
 *      FINDING 3.
 *
 *  N7. events carrying PGWT_EVENT_FLAG_IO_WORKER are absent from the
 *      fixture, and the summary records still COUNT io_worker time. OPEN:
 *      issue #315. The raw paths exclude io_worker records from DB Time, Top
 *      Events, Top Sessions, Top Queries and AAS (src/compute.c:1077, 1440,
 *      1575, 1685 and the AAS loop); src/summary_writer.c has no io_worker
 *      check anywhere, so they enter class_ns, idle_ns, events[], sessions[]
 *      and queries[]. MEASURED on a 130 s probe with one io_worker at
 *      40 ms/s: raw DB Time 1300.000 ms vs summary 6500.000 ms (+400%), plus
 *      an io_worker Top Sessions row (5200.000 ms) and a Top Events row that
 *      exist only on the summary side. REACHABLE: no handler guards it.
 *      There is no fix-proof invariant to assert -- one path counts the time
 *      and the other does not -- so the fixture stays io_worker-free
 *      (asserted in assert_fixture_live, so the hole cannot be quietly
 *      widened) and the divergence is reported rather than hidden inside a
 *      tolerance.
 *
 *      WHY THE ONE-LINE WRITER FIX DOES NOT WORK, recorded here so the fix
 *      branch does not rediscover it: `evt->flags` is EMPTY of category bits
 *      on the live path. The only producers of PGWT_EVENT_FLAG_IO_WORKER are
 *      src/server.c's bm_type_to_cat_flag (applied by pgwt_tag_events at raw
 *      LOAD time, server-side, long after the summary was written) and
 *      src/sampler.c (whose events never reach the summary writer). Nothing
 *      in src/event_stream.c or src/escalation.c sets it. So a
 *      `evt->flags & PGWT_EVENT_FLAG_IO_WORKER` test inside
 *      pgwt_summary_push_event is DEAD IN THE DAEMON, and a fixture that
 *      pre-sets the bit greens it while testing an input shape production
 *      never produces -- which is exactly what happened on the first attempt
 *      at this. The daemon-side resolver is pgwt_live_pid_cat_flag
 *      (src/map_reader.c), and using it needs real work: the summary push at
 *      event_stream.c:96 runs BEFORE the resolve at :161, and the resolver
 *      returns 0 for a pid whose metadata is not yet parsed, so an
 *      io_worker's first events slip through regardless. Any test for the
 *      fix must push UNFLAGGED events from an io_worker pid through the
 *      daemon-side path.
 *
 *      NOTE for whoever takes #315: pgwt_compute_heatmap does NOT exclude
 *      io_workers, so both heatmap paths currently agree by counting them.
 *      Excluding them at the writer alone therefore MOVES the divergence to
 *      the latency grid instead of removing it -- and handle_heatmap really
 *      does route both ways (summary at src/server.c:3984, raw at :4001), so
 *      a >= 120 s and a < 120 s window would disagree. The two changes have
 *      to land together.
 *  N8. the WINDOW END BOUND — FIXED (#316). pgwt_visit_summaries used to
 *      drop a record only when `rec_ns > to_wall_ns`, so the record whose
 *      second starts exactly AT the window end was included in full while
 *      every other window consumer is half-open (event_window_ns clips at
 *      to_ns; pgwt_compute_heatmap drops `ev_ts >= to_ns`). A summary answer
 *      over [T, T+W) therefore covered W+1 seconds, and because
 *      aas_summary_visitor clamps an over-range bucket index to the last
 *      bucket, the extra second landed entirely on the LAST bar of any
 *      chart. Invisible on a live "last 15 min" (that second is not flushed
 *      yet), never invisible on a historical window. The main 130 s window
 *      below ends one second past the last record and so never showed it;
 *      5h measures an interior window and now asserts plain EQUALITY, and
 *      section 6a pins the bound in the three shapes that break
 *      independently (interior, ending ON the last record's second — which
 *      is the one that reaches the reader's block-level prune — and zero
 *      width) plus the start bound, which must stay INCLUSIVE.
 *
 *  N9. Top Events under a QUERY filter used to lose the whole CPU* row
 *      (measured raw 7002.426 ms vs summary 2949.426 ms, -57.9%), because
 *      accum_query_add guarded its per-query top_events[] filing with
 *      `if (old_ev != 0)`. FIXED (#317) at the writer, and section 6c
 *      compares the two paths row by row on that filter. What REMAINS a
 *      named hole is only the LATENCY columns: the per-query list carries no
 *      histogram and no max, so has_latency_dist is 0 there, which the
 *      server renders as null — deliberate, #103. That is why the registry
 *      entry below still omits F_QUERY (diff_top_events compares the latency
 *      columns too); 6c compares everything else and asserts the latency
 *      columns are ABSENT on the summary side and PRESENT on raw, so
 *      "absent" cannot quietly become "zero".
 *
 *  N10/N11. two filter-shaped divergences the registry below names in full
 *      with the measured numbers: Top Queries under a class filter reports
 *      an unfiltered count, and under an event-only filter reports
 *      unfiltered per-class columns. NOT reachable — handle_top_queries
 *      forces raw for both — so they stay reported rather than fixed.
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
 *   5. the bypass suite (above), plus 5h's measurement of the #316 bound.
 *   6. THE NAMED DIVERGENCES (#316, #317, #318 fixed; #319 pinned), one
 *      case each so none can come back silently. #315 is NOT here: it is
 *      still open, for the reason N7 now records in full. Each case asserts
 *      its own non-vacuity premise FIRST, because all of them are "a number
 *      that should be smaller" and a fixture that does not contain the thing
 *      satisfies that for free. #319 needs a stream the main fixture cannot
 *      express and builds its own (`mini`).
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
    uint64_t  origin;                /* the ONE clock read (mono_origin) */
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
    fx->origin = origin;
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
     * io_worker-free: src/summary_writer.c has no io_worker check, so such a
     * record enters class_ns / idle_ns / events[] / sessions[] / queries[]
     * while every raw path except the heatmap excludes it -- measured +400%
     * on summary DB Time against raw for a 130 s stream with one io_worker.
     * Adding one here would not reveal that; it would just make every
     * comparison below red and invite someone to loosen them.
     *
     * It is ALSO what stops #315 being "fixed" by a fixture rather than by
     * the product. A pre-flagged io_worker event greens a
     * `evt->flags & PGWT_EVENT_FLAG_IO_WORKER` guard in the writer that is
     * DEAD IN THE DAEMON (nothing on the live path sets that bit -- see N7),
     * so a fixture allowed to set it can certify a fix that does nothing.
     * The exclusion is the named hole; this is what stops it from being
     * quietly widened OR quietly closed. */
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
      "top_events[] list, which carries no histogram and no max -- so the "
      "LATENCY columns come back as 'absent' (has_latency_dist = 0, which "
      "the server renders as null). Deliberate, #103, and the only part of "
      "this hole left: the missing CPU* row (accum_query_add's "
      "`if (old_ev != 0)`, measured raw 7002.426 ms vs summary 2949.426 ms, "
      "-57.9%) is FIXED in #317. F_QUERY stays off here only because "
      "diff_top_events compares the latency columns too; section 6c compares "
      "every other column on that filter row by row AND asserts the latency "
      "columns absent on the summary side and present on raw" },
    { "pgwt_compute_top_sessions_from_summaries", run_top_sessions,
      F_NONE,
      "N5: the per-second record has no per-session breakdown by query, "
      "class or event at all (struct pgwt_summary_session is {pid, "
      "db_time_ns, cpu_ns, top_wait_id, top_wait_ns}), so no filter but pid "
      "is answerable from summaries and pid is already forced raw. #318: "
      "ts_summary_visitor used to read f->pid ONLY and silently return "
      "UNFILTERED numbers for the other three, and handle_top_sessions "
      "guarded only `query_id == 0`. Now the handler forces raw for all "
      "three and the visitor REFUSES them (no rows) as the fail-safe -- "
      "section 6d, which also checks the refusal is of the FILTER and not of "
      "everything" },
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
 * FIXED (#316): the reader's record filter is `rec_ns >= to_wall_ns`, so this
 * now asserts plain EQUALITY with the in-window slice. The inclusive-bound
 * answer is still computed and named in the message, so a regression prints
 * what it regressed TO rather than only that two numbers differ; the two
 * candidates are asserted distinct first, so "equal to the right one" cannot
 * be satisfied by accident. Section 6a pins the same bound in the three
 * shapes that can each break independently (interior, on the last record's
 * second, zero width) plus the start bound that must stay inclusive.
 */
static void bypass_end_bound(const struct fixture *fx)
{
    printf("  5h. #316: the summary path's window END is EXCLUSIVE, like "
           "every other consumer\n");
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
    CHECK(sum.db_time_ms == want_half_open,
          "the SUMMARY path is half-open too (%.6f) and does NOT include the "
          "second at to_ns (which would read %.6f) -- it answered %.6f ms",
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

/* ══════════════════════════════════════════════════════════════════════════
 * 6. THE NAMED DIVERGENCES (#316, #317, #318 — fixed; #319 — pinned)
 *
 * Section 5h and the PAIRS holes above REPORTED these; this section PINS
 * them, one case each, so none can come back silently. Three are now fixed
 * and asserted equal; #319 is a definition question the product has not
 * settled, so both definitions are pinned exactly instead of one being
 * asserted "correct" — the day it is decided, exactly one CHECK below has to
 * flip, and the test says which.
 *
 * #315 (io_worker time on the summary path) is deliberately ABSENT. It is
 * still open and N7 in the header records why the obvious writer-side fix is
 * dead in the daemon: nothing on the live path sets the io_worker flag, so a
 * fixture that pre-sets it tests an input shape production never produces.
 * That mistake was made and reverted on this branch; the fixture stays
 * io_worker-free and assert_fixture_live keeps it that way.
 *
 * Each case carries its own NON-VACUITY premise, because every one of these
 * is "a number that should be smaller" and zero is smaller than everything:
 * an io_worker fix is satisfied by a fixture with no io_workers, a CPU-row fix
 * by a query with no CPU, and an end-bound fix by a window that already ends
 * past the data. The premise is asserted first in each case, so the case
 * cannot pass by not exercising the thing it is named after.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── a SECOND fixture, from a caller-built stream ──────────────────────
 *
 * fixture_build() above is hardcoded to `slice`, which is correct for the
 * matrix (one broad, io_worker-free, tie-free stream) and useless for cases
 * that need a stream shaped around one divergence. This builds the same thing
 * from any literal event array: real writer, every push checked, window
 * derived from the records that were actually committed, and a wall-shifted
 * copy for the raw side. */
struct mini {
    char      dir[600];
    struct pgwt_trace_event *wall;
    int       n;
    uint64_t  from_ns, to_ns;
    double    wall_ms;
    int       records;
    uint64_t  block_events;   /* sum of events[].count over the records */
    uint32_t  overflow;
};

static int mini_build(struct mini *m, const char *dirname, uint64_t origin,
                      const struct pgwt_trace_event *ev, int n)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->dir, sizeof(m->dir), "%s", fresh_dir(dirname));
    m->n = n;

    /* Every push checked: "the writer ACCEPTED it and deliberately did not
     * account it" and "the writer REFUSED it" are different fixes, and only
     * the first is the one being asserted below. */
    long pushed = write_summaries(m->dir, ev, n);
    if (pushed != n) {
        printf("  FAIL: mini writer accepted %ld of %d events\n", pushed, n);
        tests_failed++;
        return -1;
    }

    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    if (pgwt_visit_summaries(m->dir, 0, 0, rec_visitor, &rc) < 0 ||
        rc.records == 0) {
        printf("  FAIL: no summary records were committed to %s\n", m->dir);
        tests_failed++;
        return -1;
    }
    m->records      = rc.records;
    m->block_events = rc.total_events;
    m->overflow     = rc.overflow;
    m->from_ns      = rc.min_sec;
    m->to_ns        = rc.max_sec + ONE_SEC;
    m->wall_ms      = (double)(m->to_ns - m->from_ns) / 1e6;

    m->wall = calloc((size_t)n, sizeof(*m->wall));
    if (!m->wall) { tests_failed++; return -1; }
    uint64_t shift = m->from_ns - origin;
    memcpy(m->wall, ev, (size_t)n * sizeof(*m->wall));
    for (int i = 0; i < n; i++)
        m->wall[i].timestamp_ns += shift;
    return 0;
}

static void mini_free(struct mini *m) { free(m->wall); m->wall = NULL; }

/* ── 6a. #316: the window end is HALF-OPEN on both paths ───────────────
 *
 * pgwt_visit_summaries used to drop a record only on `rec_ns > to_wall_ns`,
 * so the second STARTING at the window end was included in full and a summary
 * answer over [T, T+W) covered W+1 seconds. 5h above measured it; this pins
 * the fix in the three shapes that can each be got wrong independently:
 *
 *   - an INTERIOR one-second window (the shape 5h measured);
 *   - a window ending on the LAST record's second, which is also the shape
 *     that exercises the block-level prune (`block_index[b].timestamp_ns`)
 *     rather than only the per-record filter — a fix applied to one and not
 *     the other passes the first case and fails this one;
 *   - a ZERO-WIDTH window, which must see NOTHING. Under the old inclusive
 *     bound it saw exactly one record, so this is the sharpest statement of
 *     the bug that exists: half-open means from == to is empty.
 *
 * And the FROM side must stay INCLUSIVE: `>=` applied to the wrong bound
 * would shift every window by one second and still give a one-second answer
 * for a one-second request, so the interior case asserts the answer is slice
 * K specifically, with slices K-1, K and K+1 asserted distinct first. */
static void div316_half_open(const struct fixture *fx)
{
    printf("  6a. #316: both paths treat the window end as EXCLUSIVE\n");
    const int K = 10;
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    uint64_t prev_ns = slice_db_ns(K - 1);
    uint64_t in_ns   = slice_db_ns(K);
    uint64_t next_ns = slice_db_ns(K + 1);
    /* NON-VACUITY: if the three neighbouring seconds carried the same DB
     * Time, "it answered slice K" would also be true of an answer that was
     * really slice K-1 or K+1, and an off-by-one-second window would read as
     * correct. */
    CHECK(prev_ns > 0 && in_ns > 0 && next_ns > 0 &&
          prev_ns != in_ns && in_ns != next_ns && prev_ns != next_ns,
          "slices %d, %d and %d carry three DIFFERENT non-zero DB Times "
          "(%llu / %llu / %llu ns), so 'the window answered slice %d' is a "
          "statement that can be wrong", K - 1, K, K + 1,
          (unsigned long long)prev_ns, (unsigned long long)in_ns,
          (unsigned long long)next_ns, K);

    /* (1) interior one-second window */
    uint64_t from = fx->from_ns + (uint64_t)K * ONE_SEC;
    uint64_t to   = from + ONE_SEC;
    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(fx->wall, fx->n, &f, from, to, 1000.0, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, from, to, &f, 1000.0, &sum);
    CHECK(raw.db_time_ms == (double)in_ns / 1e6,
          "6a: the raw path answers slice %d exactly (%.6f == %.6f ms)", K,
          raw.db_time_ms, (double)in_ns / 1e6);
    CHECK(sum.db_time_ms == (double)in_ns / 1e6,
          "6a: and so does the SUMMARY path -- not slice %d+%d together "
          "(%.6f would be %.6f ms); answered %.6f ms", K, K + 1,
          (double)(in_ns + next_ns) / 1e6, (double)(in_ns + next_ns) / 1e6,
          sum.db_time_ms);
    CHECK(diff_time_model("6a interior 1 s", &raw, &sum, 1) == 0,
          "6a: and every other time-model field agrees on that window too");
    free_tm(&raw); free_tm(&sum);

    /* (2) a window ending ON the last record's second: that record is at
     * to_ns and must be excluded. This is also the case where the reader's
     * BLOCK-level prune sits exactly on the boundary. */
    uint64_t all_but_last = 0;
    for (int s = 0; s < SECS - 1; s++)
        all_but_last += slice_db_ns(s);
    uint64_t last = slice_db_ns(SECS - 1);
    CHECK(last > 0 && all_but_last > 0,
          "the last slice (%llu ns) and the preceding %d (%llu ns) are both "
          "non-zero", (unsigned long long)last, SECS - 1,
          (unsigned long long)all_but_last);
    uint64_t to2 = fx->from_ns + (uint64_t)(SECS - 1) * ONE_SEC;
    double wall2 = (double)(to2 - fx->from_ns) / 1e6;
    pgwt_compute_time_model(fx->wall, fx->n, &f, fx->from_ns, to2, wall2, &raw);
    pgwt_compute_time_model_from_summaries(fx->dir, fx->from_ns, to2, &f,
                                           wall2, &sum);
    CHECK(sum.db_time_ms == (double)all_but_last / 1e6,
          "6a: a window ending ON the last record's second EXCLUDES it "
          "(%.6f == %.6f ms, and would be %.6f with it)", sum.db_time_ms,
          (double)all_but_last / 1e6, (double)(all_but_last + last) / 1e6);
    CHECK(diff_time_model("6a ends on last second", &raw, &sum, 1) == 0,
          "6a: and the two paths agree on it");
    free_tm(&raw); free_tm(&sum);

    /* (3) zero width: half-open means empty. Asserted at the visitor, which
     * is where the bound lives -- the compute wrappers return early on a
     * zero-length range, so they cannot see this either way. */
    struct rec_ctx rc;
    memset(&rc, 0, sizeof(rc));
    uint64_t z = fx->from_ns + (uint64_t)K * ONE_SEC;
    pgwt_visit_summaries(fx->dir, z, z, rec_visitor, &rc);
    CHECK(rc.records == 0,
          "6a: a ZERO-WIDTH window [T, T) visits no records (%d) -- under the "
          "old inclusive bound it visited exactly the record at T",
          rc.records);

    /* (4) and the FROM bound is still INCLUSIVE: a window starting exactly on
     * a record's second must contain it. Without this, changing `>` to `>=`
     * on the wrong comparison would pass (1)-(3) by shifting every window one
     * second earlier. */
    memset(&rc, 0, sizeof(rc));
    pgwt_visit_summaries(fx->dir, z, z + ONE_SEC, rec_visitor, &rc);
    CHECK(rc.records == 1,
          "6a: and [T, T+1s) visits exactly the ONE record at T (%d) -- the "
          "start bound stays inclusive", rc.records);
}

/* ── 6c. #317: a query's CPU time is in its per-query event list ────────
 *
 * accum_query_add guarded the per-query top_events[] filing with
 * `if (old_ev != 0)`, so event 0 -- CPU -- was never filed, and the Events
 * tab drilled into one query over a >= 120 s window lost the whole CPU* row:
 * measured raw 7002.426 ms vs summary 2949.426 ms, -57.9%, on this fixture.
 * REACHABLE: handle_top_events has no query guard.
 *
 * The null LATENCY columns on this path are a different thing and are
 * deliberate (#103): the per-query list carries no histogram and no max. That
 * is asserted here explicitly rather than skipped, so "absent" cannot quietly
 * become "zero" or "wrong". */
static void div317_query_cpu(const struct fixture *fx)
{
    printf("  6c. #317: the per-query event list includes CPU (event 0)\n");

    /* Independent expectation for QID_A's CPU time and its event count. */
    uint64_t exp_cpu_ns = 0;
    uint64_t exp_cpu_count = 0;
    for (int s = 0; s < SECS; s++)
        for (int i = 0; i < SLICE_N; i++)
            if (slice[i].qid == QID_A && slice[i].ev == EV_CPU) {
                exp_cpu_ns += spec_dur_ns(i, s);
                exp_cpu_count++;
            }
    /* NON-VACUITY: a query with no CPU time would satisfy every assertion
     * below while the product was still dropping the row. */
    CHECK(exp_cpu_ns > 0 && exp_cpu_count > 0,
          "6c: the filtered query really has CPU time (%.6f ms over %llu "
          "intervals) -- a query without any would pass this case while the "
          "CPU row was still being dropped", (double)exp_cpu_ns / 1e6,
          (unsigned long long)exp_cpu_count);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    f.query_id = QID_A;

    struct pgwt_events_result raw, sum;
    pgwt_compute_top_events(fx->wall, fx->n, &f, fx->from_ns, fx->to_ns,
                            fx->wall_ms, &raw);
    pgwt_compute_top_events_from_summaries(fx->dir, fx->from_ns, fx->to_ns, &f,
                                           fx->wall_ms, &sum);

    /* The CPU row specifically, on both sides, with the independent total. */
    const struct pgwt_event_row *rr = ev_find(&raw, EV_CPU);
    const struct pgwt_event_row *rs = ev_find(&sum, EV_CPU);
    CHECK(rr != NULL, "6c: the raw path has a CPU row under the query filter");
    CHECK(rs != NULL,
          "6c: and so does the SUMMARY path -- the row this fix restores");
    if (rr && rs) {
        CHECK(rr->total_ms == (double)exp_cpu_ns / 1e6 &&
              rs->total_ms == (double)exp_cpu_ns / 1e6,
              "6c: both CPU rows carry the query's whole CPU time "
              "(raw %.6f, summary %.6f, expected %.6f ms)",
              rr->total_ms, rs->total_ms, (double)exp_cpu_ns / 1e6);
        CHECK(rr->count == exp_cpu_count && rs->count == exp_cpu_count,
              "6c: and the same interval count (raw %llu, summary %llu, "
              "expected %llu)", (unsigned long long)rr->count,
              (unsigned long long)rs->count,
              (unsigned long long)exp_cpu_count);
    }

    /* db_time_ms was short by exactly that row. */
    CHECK(raw.db_time_ms == sum.db_time_ms,
          "6c: and the query's DB Time now agrees (raw %.6f == summary %.6f "
          "ms; the gap used to be the CPU row, %.6f ms)",
          raw.db_time_ms, sum.db_time_ms, (double)exp_cpu_ns / 1e6);
    CHECK(raw.db_time_ms > 0,
          "6c: on a non-empty comparison (%.6f ms)", raw.db_time_ms);

    /* Row sets, counts and the DB-Time columns are compared field by field
     * here rather than via diff_top_events, because that comparator also
     * compares the latency columns -- which are deliberately absent on this
     * path (#103) and are asserted as absent just below. */
    CHECK(raw.num_rows == sum.num_rows && raw.num_rows > 0,
          "6c: the same number of rows on both paths (%d == %d, non-zero)",
          raw.num_rows, sum.num_rows);
    int bij = 0, agreed = 0, sum_claims_latency = 0;
    for (int i = 0; i < raw.num_rows; i++) {
        const struct pgwt_event_row *a = &raw.rows[i];
        const struct pgwt_event_row *b = ev_find(&sum, a->event_id);
        if (!b) {
            printf("    DIFF 6c: event 0x%x ('%s') absent from the summary "
                   "path\n", a->event_id, a->name);
            continue;
        }
        bij++;
        if (a->count != b->count || dne(a->total_ms, b->total_ms) ||
            dne(a->pct_db, b->pct_db) || dne(a->aas, b->aas) ||
            dne(a->avg_us, b->avg_us) || a->exact_count != b->exact_count)
            printf("    DIFF 6c: event '%s' count %llu/%llu total_ms "
                   "%.9f/%.9f pct_db %.9f/%.9f aas %.12f/%.12f\n", a->name,
                   (unsigned long long)a->count, (unsigned long long)b->count,
                   a->total_ms, b->total_ms, a->pct_db, b->pct_db,
                   a->aas, b->aas);
        else
            agreed++;
        if (b->has_latency_dist) sum_claims_latency++;
    }
    CHECK(bij == raw.num_rows,
          "6c: every raw row has a summary counterpart (%d of %d)",
          bij, raw.num_rows);
    CHECK(agreed == raw.num_rows,
          "6c: and every one agrees on count, total, pct_db, avg and AAS "
          "(%d of %d)", agreed, raw.num_rows);
    for (int i = 0; i < sum.num_rows; i++)
        CHECK(ev_find(&raw, sum.rows[i].event_id) != NULL,
              "6c: summary row 0x%x ('%s') exists on the raw path too",
              sum.rows[i].event_id, sum.rows[i].name);
    /* The #103 hole, pinned: absent on the summary path, present on raw. */
    CHECK(sum_claims_latency == 0,
          "6c: the summary path reports NO latency distribution under a query "
          "filter (%d rows claimed one) -- deliberate, #103: the per-query "
          "list carries no histogram and no max",
          sum_claims_latency);
    int lat_raw = 0;
    for (int i = 0; i < raw.num_rows; i++)
        if (raw.rows[i].has_latency_dist) lat_raw++;
    CHECK(lat_raw > 0,
          "6c: while the raw path does report one (%d rows) -- so the "
          "assertion above is about a real difference, not about both paths "
          "being empty", lat_raw);

    free_ev(&raw); free_ev(&sum);
}

/* ── 6d. #318: Top Sessions cannot honour a class or event filter ───────
 *
 * ts_summary_visitor reads f->pid ONLY, so a class- or event-filtered
 * Sessions tab over a >= 120 s window used to come back UNFILTERED while the
 * UI said one class was selected. The issue proposed honouring the filter in
 * the visitor instead, on the stated grounds that "the summary record does
 * carry per-event totals per session" -- it does NOT:
 * struct pgwt_summary_session is {pid, db_time_ns, cpu_ns, top_wait_id,
 * top_wait_ns} and has no per-event field at all. So the fix is two-part:
 * handle_top_sessions forces the RAW path for a class- or event-filtered
 * request (the precedent handle_top_queries already sets), and
 * ts_summary_visitor REFUSES such a filter -- no rows -- as the fail-safe
 * underneath it.
 *
 * A C unit test cannot reach the handler (that layer is
 * tests/test_data_*.py), which is exactly why the refusal matters and is what
 * this case asserts: even if the guard were edited away, the answer would be
 * visibly EMPTY rather than plausibly wrong. The refusal is also checked to
 * be a refusal of the FILTER specifically -- the unfiltered request must
 * still answer, and still agree with raw. */
static void div318_session_filters(const struct fixture *fx)
{
    printf("  6d. #318: the summary Top Sessions path REFUSES a class or "
           "event filter it cannot honour\n");

    struct pgwt_filter f_none, f_class, f_event;
    memset(&f_none,  0, sizeof(f_none));
    memset(&f_class, 0, sizeof(f_class));
    memset(&f_event, 0, sizeof(f_event));
    snprintf(f_class.class_name, sizeof(f_class.class_name), "io");
    f_event.event_id = EV_LOCK_A;

    struct pgwt_sessions_result rn, rc_, re, sn, sc, se;
    pgwt_compute_top_sessions(fx->wall, fx->n, &f_none, fx->from_ns, fx->to_ns,
                              fx->wall_ms, &rn);
    pgwt_compute_top_sessions(fx->wall, fx->n, &f_class, fx->from_ns,
                              fx->to_ns, fx->wall_ms, &rc_);
    pgwt_compute_top_sessions(fx->wall, fx->n, &f_event, fx->from_ns,
                              fx->to_ns, fx->wall_ms, &re);
    pgwt_compute_top_sessions_from_summaries(fx->dir, fx->from_ns, fx->to_ns,
                                             &f_none, fx->wall_ms, &sn);
    pgwt_compute_top_sessions_from_summaries(fx->dir, fx->from_ns, fx->to_ns,
                                             &f_class, fx->wall_ms, &sc);
    pgwt_compute_top_sessions_from_summaries(fx->dir, fx->from_ns, fx->to_ns,
                                             &f_event, fx->wall_ms, &se);

    /* NON-VACUITY: the filters must actually change the RAW answer, or
     * "the summary answer is unfiltered" would be indistinguishable from
     * "the filter matched everything". */
    CHECK(rn.num_rows > 0 && rc_.num_rows > 0 && re.num_rows > 0,
          "6d: all three raw answers have rows (%d / %d / %d)",
          rn.num_rows, rc_.num_rows, re.num_rows);
    CHECK(rc_.num_rows != rn.num_rows || re.num_rows != rn.num_rows,
          "6d: and the class/event filters really change the raw row set "
          "(unfiltered %d, class=io %d, event=Lock %d)",
          rn.num_rows, rc_.num_rows, re.num_rows);

    /* The summary path REFUSES both filters: no rows, rather than the
     * unfiltered answer wearing a filtered label. */
    CHECK(sc.num_rows == 0,
          "6d: the summary path returns NO rows under a class filter (%d) -- "
          "it used to return the UNFILTERED %d rows", sc.num_rows,
          sn.num_rows);
    CHECK(se.num_rows == 0,
          "6d: and none under an event filter (%d)", se.num_rows);

    /* NON-VACUITY of the refusal itself: it must be a refusal of the FILTER,
     * not a path that returns nothing whatever it is asked. Without this,
     * deleting the body of ts_summary_visitor would satisfy the two CHECKs
     * above. */
    CHECK(sn.num_rows > 0,
          "6d: while the UNFILTERED summary request still returns rows (%d) "
          "-- so this is a refusal of the filter, not a path that answers "
          "nothing at all", sn.num_rows);
    CHECK(diff_top_sessions("6d unfiltered", &rn, &sn, 1) == 0,
          "6d: and that unfiltered answer still agrees with raw field by "
          "field");

    /* And the gate can SEE a filtered request being answered from summaries,
     * so the guard in handle_top_sessions is not an argument from code
     * reading. A refusal is a disagreement here, which is the point: it is
     * loud, and the handler is what keeps it off the wire. */
    CHECK(diff_top_sessions("6d class=io", &rc_, &sc, 0) > 0,
          "6d: comparing raw class=io against the summary path's answer is a "
          "DISAGREEMENT, not agreement -- so a handler that routed this to "
          "summaries could not do it quietly");
    CHECK(diff_top_sessions("6d event=Lock", &re, &se, 0) > 0,
          "6d: same under an event filter");

    free_se(&rn); free_se(&rc_); free_se(&re);
    free_se(&sn); free_se(&sc); free_se(&se);
}

/* ── 6e. #319: top_wait means two different things -- BOTH pinned ───────
 *
 *   raw     = argmax over the window of the session's per-EVENT TOTAL
 *             ("where did this session's time go")
 *   summary = the single LONGEST INDIVIDUAL wait (the writer keeps
 *             `dur > ss->top_wait_ns` per second; the reader keeps the max
 *             over seconds)
 *
 * NOT FIXED, and deliberately so: "largest total" is the right answer (every
 * other total in the product is a window total, and the column sits beside DB
 * Time where a DBA reads it as "where the time went"), but it is NOT
 * derivable from the record. struct pgwt_summary_session carries ONE
 * (top_wait_id, top_wait_ns) pair per second, and no sequence of per-second
 * (id, max) pairs determines the window-wide argmax of totals -- the fixture
 * below is the counterexample: every second's longest wait is the IO one, in
 * the only second that has it, while Lock has 32x the total. Making the
 * summary path exact needs per-session per-event totals in the record, and
 * any BOUNDED version of that (a top-8 list, like the per-query one) would
 * re-introduce silent inexactness, which is the bug class this whole file
 * exists for. So the decision is the owner's, and meanwhile BOTH definitions
 * are pinned exactly: neither side can drift, and when it is decided exactly
 * one CHECK here flips.
 *
 * The main fixture CANNOT express this: every one of its durations is
 * base_us * (1 + s % 8), so per-event totals and per-event maxima rank
 * identically for every pid and the two definitions agree by construction.
 * That is precisely why it needs its own stream. */
#define TW_SECS   130
#define TW_PID    301u
#define TW_LOCK_MS 10
#define TW_IO_MS   40

static void div319_top_wait(uint64_t origin)
{
    printf("  6e. #319: top_wait is 'largest total' on raw and 'longest "
           "single wait' on the summary path -- both pinned\n");

    /* BUILT IN TIMESTAMP ORDER, and that is load-bearing, not tidiness.
     * pgwt_summary_push_event implements #277's late-arrival rule: an event
     * whose second has already been flushed is FOLDED into the oldest second
     * still open. Appending the single IO event after all 130 Lock events
     * therefore moved it out of second TW_SECS/2 and into the LAST second --
     * counts conserved, so nothing here went red, but the one event that
     * distinguishes the two top_wait definitions ended up in the last
     * record. That silently neutered a mutation probe (a reader keeping the
     * LAST second's top wait instead of the max over seconds still answered
     * IO, so the pin below could not see the drift). Emitting in timestamp
     * order puts the IO wait in an interior second, where it belongs, and
     * the probe goes red as it should. */
    int n = 0;
    struct pgwt_trace_event *ev = calloc(TW_SECS + 1, sizeof(*ev));
    if (!ev) { tests_failed++; return; }
    const int io_sec = TW_SECS / 2;
    for (int s = 0; s < TW_SECS; s++) {
        ev[n].timestamp_ns = origin + (uint64_t)s * ONE_SEC + 500 * MS;
        ev[n].pid          = TW_PID;
        ev[n].old_event    = EV_LOCK_A;
        ev[n].new_event    = EV_IO_A;
        ev[n].duration_ns  = TW_LOCK_MS * MS;
        ev[n].query_id     = QID_A;
        ev[n].cpu_ns       = PGWT_CPU_NS_UNKNOWN;
        n++;
        if (s == io_sec) {
            /* ONE long IO wait, 200 ms later in the SAME second (so it does
             * not straddle: 500 ms + 200 ms + 40 ms < 1 s). */
            ev[n].timestamp_ns = origin + (uint64_t)s * ONE_SEC + 700 * MS;
            ev[n].pid          = TW_PID;
            ev[n].old_event    = EV_IO_A;
            ev[n].new_event    = EV_LOCK_A;
            ev[n].duration_ns  = TW_IO_MS * MS;
            ev[n].query_id     = QID_A;
            ev[n].cpu_ns       = PGWT_CPU_NS_UNKNOWN;
            n++;
        }
    }
    CHECK(io_sec > 0 && io_sec < TW_SECS - 1,
          "6e: the IO wait is in an INTERIOR second (%d of 0..%d), not the "
          "last one -- otherwise 'the max over seconds' and 'the last "
          "second' are the same answer and the pin below cannot tell them "
          "apart", io_sec, TW_SECS - 1);

    uint64_t lock_total = (uint64_t)TW_SECS * TW_LOCK_MS * MS;
    uint64_t lock_max   = (uint64_t)TW_LOCK_MS * MS;
    uint64_t io_total   = (uint64_t)TW_IO_MS * MS;
    uint64_t io_max     = (uint64_t)TW_IO_MS * MS;

    /* NON-VACUITY: the two definitions must pick DIFFERENT events here, or
     * "raw says Lock and the summary says IO" would be a statement that could
     * not fail. */
    CHECK(lock_total > io_total && io_max > lock_max,
          "6e: Lock has the larger TOTAL (%.6f > %.6f ms) and IO the larger "
          "SINGLE wait (%.6f > %.6f ms), so the two definitions disagree on "
          "this stream", (double)lock_total / 1e6, (double)io_total / 1e6,
          (double)io_max / 1e6, (double)lock_max / 1e6);

    struct mini m;
    if (mini_build(&m, "fixture_top_wait", origin, ev, n) != 0) {
        free(ev);
        return;
    }
    free(ev);
    CHECK(m.records == TW_SECS, "6e: every second was committed (%d == %d)",
          m.records, TW_SECS);
    CHECK(m.block_events == (uint64_t)n,
          "6e: and the blocks hold every pushed event (%llu == %d)",
          (unsigned long long)m.block_events, n);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result raw, sum;
    pgwt_compute_top_sessions(m.wall, m.n, &f, m.from_ns, m.to_ns, m.wall_ms,
                              &raw);
    pgwt_compute_top_sessions_from_summaries(m.dir, m.from_ns, m.to_ns, &f,
                                             m.wall_ms, &sum);

    const struct pgwt_session_row *a = se_find(&raw, TW_PID);
    const struct pgwt_session_row *b = se_find(&sum, TW_PID);
    CHECK(a != NULL && b != NULL,
          "6e: the pid is a row on both paths (raw %s, summary %s)",
          a ? "yes" : "NO", b ? "yes" : "NO");

    /* This is NOT a counting error: the totals agree exactly. */
    CHECK(a && b && a->db_time_ms == b->db_time_ms &&
          a->db_time_ms == (double)(lock_total + io_total) / 1e6,
          "6e: both paths agree on the session's DB Time (%.6f / %.6f, "
          "expected %.6f ms) -- the top_wait difference is a DEFINITION "
          "difference, not a counting one", a ? a->db_time_ms : -1.0,
          b ? b->db_time_ms : -1.0, (double)(lock_total + io_total) / 1e6);

    /* Both definitions, pinned. Flip exactly one of these when the product
     * decides (see the note above). */
    CHECK(a && a->top_wait_id == EV_LOCK_A,
          "6e: the RAW path names the session's LARGEST TOTAL (Lock, 0x%x); "
          "it named 0x%x", EV_LOCK_A, a ? a->top_wait_id : 0);
    CHECK(b && b->top_wait_id == EV_IO_A,
          "6e: the SUMMARY path names the LONGEST SINGLE WAIT (IO, 0x%x); it "
          "named 0x%x. NOT a bug being left in place: see the note above -- "
          "the per-second record cannot carry the other answer. When this is "
          "decided, this is the assertion that changes", EV_IO_A,
          b ? b->top_wait_id : 0);
    CHECK(a && b && a->top_wait_id != b->top_wait_id,
          "6e: and they really are different events here, so neither "
          "assertion above is satisfied by the other definition");

    free_se(&raw); free_se(&sum);
    mini_free(&m);
}

static void test_divergences(const struct fixture *fx, uint64_t origin)
{
    printf("--- 6. the five named divergences (#315-#319) ---\n");
    div316_half_open(fx);
    div317_query_cpu(fx);
    div318_session_filters(fx);
    div319_top_wait(origin);
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
        test_divergences(&fx, fx.origin);
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
