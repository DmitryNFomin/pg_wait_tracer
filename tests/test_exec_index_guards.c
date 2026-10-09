/* test_exec_index_guards.c — the two poison guards the executions index
 * needs before anything can be wired onto it (paint-latency Phase 3).
 *
 * THE DEFECT, stated as the input that triggers it
 * ------------------------------------------------
 * src/exec_index.h writes PGWT_EXEC_INDEX_NOT_INDEXED (-1) into n_events,
 * n_workers and matches_event_filter, because those three are per-event,
 * per-filter, window-CLIPPED quantities a BOUNDARY index cannot carry, and
 * "absence must never read as zero".
 *
 * -1 IS TRUTHY IN C. src/server.c's execution_matches_request ended in
 *
 *     if ((req->filter.class_name[0] || req->filter.event_id) &&
 *         !row->matches_event_filter)
 *         return 0;
 *     return 1;
 *
 * so a row carrying -1 PASSED an event filter it had never been tested
 * against: a class-filtered Waterfall or Scatter painting unfiltered
 * executions, with nothing in the response to say so. The input that
 * triggers it is exactly one row: matches_event_filter = -1 with a filter
 * whose class_name or event_id is set. §2 holds a VERBATIM COPY of that
 * deleted predicate and asserts the differential, so the bug goes red here
 * on the same fixture rather than being argued about.
 *
 * The second guard: n_events / n_workers are rendered verbatim as the
 * Waterfall table's "Leader events" / "Workers" cells, so -1 reaches the
 * screen as the string "-1" in a column of counts.
 * pgwt_execution_counts_known() is what serialize_execution_row() refuses
 * on; its JS mirror (executionCountsKnown / fmtExecutionCount in
 * web/static/lib/builders/waterfall.js) is pinned by
 * tests/web_unit/waterfall.test.mjs.
 *
 * §3 is the BYPASS SUITE: every way these two predicates could be made
 * unreachable or satisfied without checking anything. A gate that cannot
 * see must refuse, never approve — so the undecidable verdict is asserted
 * to be neither 1 nor 0, and the fixtures that are BLIND to the original
 * bug are asserted to be blind, so nobody mistakes one of them for
 * coverage.
 *
 * Pure: links compute.c with -DPGWT_SERVER, same shape as
 * test_executions_attribution. No daemon, no PostgreSQL, no root. */
#include "compute.h"
#include "exec_index.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do {                                   \
    tests_run++;                                                     \
    if (cond) { tests_passed++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } \
    else      { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); }  \
} while (0)

/* compute.c's only foreign symbol under -DPGWT_SERVER. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

/* ── the predicate that shipped, copied verbatim ──────────────────────────
 * Not paraphrased: this is src/server.c's execution_matches_request as it
 * stood at f78ab4c, with only the struct wiring changed (it read
 * req->filter; here it takes the filter directly). Its presence is what
 * makes §2 a differential instead of an opinion. */
static int old_execution_matches_request(const struct pgwt_execution *row,
                                         const struct pgwt_filter *f)
{
    if (f->pid && row->pid != f->pid)
        return 0;
    if (f->query_id && row->query_id != f->query_id)
        return 0;
    if ((f->class_name[0] || f->event_id) &&
        !row->matches_event_filter)
        return 0;
    return 1;
}

/* An index-shaped row: boundary fields real, the three per-event fields
 * poisoned exactly as pgwt_exec_index_query() writes them. */
static struct pgwt_execution index_row(void)
{
    struct pgwt_execution r;
    memset(&r, 0, sizeof(r));
    r.pid = 4242;
    r.query_id = 0x5151515151515151ULL;
    r.start_ns = 1000;
    r.end_ns = 2000;
    r.n_events = PGWT_EXEC_INDEX_NOT_INDEXED;
    r.n_workers = PGWT_EXEC_INDEX_NOT_INDEXED;
    r.matches_event_filter = PGWT_EXEC_INDEX_NOT_INDEXED;
    return r;
}

/* A raw-path row: every field computed. */
static struct pgwt_execution raw_row(int n_events, int n_workers, int matches)
{
    struct pgwt_execution r = index_row();
    r.n_events = n_events;
    r.n_workers = n_workers;
    r.matches_event_filter = matches;
    return r;
}

static struct pgwt_filter filter_none(void)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    return f;
}

static struct pgwt_filter filter_class(const char *cls)
{
    struct pgwt_filter f = filter_none();
    snprintf(f.class_name, sizeof(f.class_name), "%s", cls);
    return f;
}

static struct pgwt_filter filter_event(uint32_t event_id)
{
    struct pgwt_filter f = filter_none();
    f.event_id = event_id;
    return f;
}

/* ── §1 the two sentinels must be the same number ─────────────────────── */

static void test_sentinels_agree(void)
{
    /* compute.h names the tri-state; exec_index.h produces it. Two headers,
     * one number. If they ever diverge, every `< 0` test below still works
     * but the DOCUMENTATION lies, and the next reader hand-writes `== -1`
     * against the wrong constant. */
    CHECK(PGWT_EXEC_COUNT_UNKNOWN == PGWT_EXEC_INDEX_NOT_INDEXED,
          "PGWT_EXEC_COUNT_UNKNOWN (%d) == PGWT_EXEC_INDEX_NOT_INDEXED (%d)",
          PGWT_EXEC_COUNT_UNKNOWN, PGWT_EXEC_INDEX_NOT_INDEXED);
    CHECK(PGWT_EXEC_COUNT_UNKNOWN < 0,
          "the sentinel is negative, so no count can collide with it");
}

/* ── §2 the differential: the deleted predicate vs the shipped one ─────── */

static void test_class_filter_on_a_poisoned_row(void)
{
    struct pgwt_execution row = index_row();
    struct pgwt_filter f = filter_class("Lock");

    /* THE BUG, on its own fixture. */
    CHECK(old_execution_matches_request(&row, &f) == 1,
          "deleted predicate KEPT a not-computed row under a class filter "
          "(returned %d) -- the defect this test exists for",
          old_execution_matches_request(&row, &f));
    CHECK(pgwt_execution_filter_verdict(&row, &f) == -1,
          "shipped predicate reports UNDECIDABLE (got %d)",
          pgwt_execution_filter_verdict(&row, &f));
    /* Undecidable is NOT a drop either. A gate that silently answered 0
     * would shorten the tab instead of widening it: still a silent wrong
     * answer, just in the other direction. */
    CHECK(pgwt_execution_filter_verdict(&row, &f) != 0,
          "UNDECIDABLE is not collapsed to 'drop'");
}

static void test_event_id_filter_on_a_poisoned_row(void)
{
    struct pgwt_execution row = index_row();
    struct pgwt_filter f = filter_event(0x01000007u);

    CHECK(old_execution_matches_request(&row, &f) == 1,
          "deleted predicate KEPT a not-computed row under an event_id filter");
    CHECK(pgwt_execution_filter_verdict(&row, &f) == -1,
          "shipped predicate reports UNDECIDABLE for event_id too");
}

static void test_both_dimensions_set(void)
{
    struct pgwt_execution row = index_row();
    struct pgwt_filter f = filter_class("IO");
    f.event_id = 0x02000003u;
    CHECK(pgwt_execution_filter_verdict(&row, &f) == -1,
          "class AND event_id together are still UNDECIDABLE");
}

/* ── §3 bypass suite: make the guard unreachable, or satisfy it blind ──── */

/* (a) The thing being checked is ABSENT rather than wrong: a row whose
 * matches_event_filter is a real 0. Both predicates drop it, so this
 * fixture cannot see the bug at all. Asserted, not assumed — a reviewer
 * must be able to tell this fixture from coverage. */
static void bypass_real_zero_is_blind(void)
{
    struct pgwt_execution row = raw_row(0, 0, 0);
    struct pgwt_filter f = filter_class("Lock");
    CHECK(old_execution_matches_request(&row, &f) == 0 &&
          pgwt_execution_filter_verdict(&row, &f) == 0,
          "a genuinely non-matching row drops on BOTH predicates: this "
          "fixture is blind to the defect");
}

/* (b) No event dimension in the filter: nothing to be undecidable about.
 * Both predicates keep the row, poison and all — also blind. This is the
 * case that must NOT refuse, or the index could never answer the default
 * unfiltered request. */
static void bypass_no_event_dimension_is_blind(void)
{
    struct pgwt_execution row = index_row();
    struct pgwt_filter f = filter_none();
    CHECK(old_execution_matches_request(&row, &f) == 1 &&
          pgwt_execution_filter_verdict(&row, &f) == 1,
          "an unfiltered request keeps the row on BOTH predicates: blind "
          "fixture, and the case that must not refuse");
}

/* (c) A decidable DROP must stay decidable even with poisoned counts:
 * pid and query_id are boundary fields. If this refused, a pid-filtered
 * Waterfall (the drill-down path) could never be served from the index —
 * a guard that refuses everything is indistinguishable from a broken
 * feature. */
static void bypass_boundary_dimensions_stay_decidable(void)
{
    struct pgwt_execution row = index_row();
    struct pgwt_filter f = filter_none();
    f.pid = 9999;                       /* row.pid is 4242 */
    CHECK(pgwt_execution_filter_verdict(&row, &f) == 0,
          "pid mismatch is a decidable DROP on a poisoned row");

    struct pgwt_filter g = filter_none();
    g.pid = 4242;
    CHECK(pgwt_execution_filter_verdict(&row, &g) == 1,
          "pid match with no event dimension is a decidable KEEP");

    struct pgwt_filter h = filter_none();
    h.query_id = 0x1111111111111111ULL; /* row.query_id differs */
    CHECK(pgwt_execution_filter_verdict(&row, &h) == 0,
          "query_id mismatch is a decidable DROP on a poisoned row");

    /* And a pid mismatch outranks the undecidable event dimension: the row
     * is excluded whatever the event verdict would have been. */
    struct pgwt_filter k = filter_class("Lock");
    k.pid = 9999;
    CHECK(pgwt_execution_filter_verdict(&row, &k) == 0,
          "a decidable drop is reported even when the event dimension is "
          "undecidable");
}

/* (d) A DIFFERENT poison sentinel. The guard must test "is this a verdict",
 * not "is this exactly -1" — otherwise a future source poisoning with -2
 * walks straight through it. */
static void bypass_other_negative_sentinels(void)
{
    struct pgwt_execution row = index_row();
    row.matches_event_filter = -7;
    struct pgwt_filter f = filter_class("Lock");
    CHECK(pgwt_execution_filter_verdict(&row, &f) == -1,
          "any negative matches_event_filter is UNDECIDABLE, not just -1");

    row.matches_event_filter = INT_MIN;
    CHECK(pgwt_execution_filter_verdict(&row, &f) == -1,
          "INT_MIN is UNDECIDABLE");
}

/* (e) A real positive verdict must still be served, or the guard has
 * disabled the raw path too. */
static void bypass_real_match_still_served(void)
{
    struct pgwt_execution row = raw_row(3, 1, 1);
    struct pgwt_filter f = filter_class("Lock");
    CHECK(pgwt_execution_filter_verdict(&row, &f) == 1,
          "a computed matching row is still KEPT");
    /* matches_event_filter is only ever set to 1 by compute.c, but a
     * hypothetical >1 must not read as undecidable. */
    row.matches_event_filter = 5;
    CHECK(pgwt_execution_filter_verdict(&row, &f) == 1,
          "any positive verdict is a KEEP");
}

/* (f) NULL inputs. A missing row must never read as "keep"; a missing
 * filter has no dimension to refuse on. */
static void bypass_null_inputs(void)
{
    struct pgwt_execution row = index_row();
    CHECK(pgwt_execution_filter_verdict(NULL, &(struct pgwt_filter){0}) == 0,
          "a NULL row is never a KEEP");
    CHECK(pgwt_execution_filter_verdict(&row, NULL) == 1,
          "a NULL filter filters nothing");
    CHECK(pgwt_execution_counts_known(NULL) == 0,
          "a NULL row has no known counts");
}

/* ── §4 pgwt_execution_counts_known: the serialisation guard ───────────── */

static void test_counts_known(void)
{
    CHECK(pgwt_execution_counts_known(&(struct pgwt_execution){
              .n_events = 0, .n_workers = 0}) == 1,
          "0/0 is a real pair of counts (an execution with no wait events)");
    CHECK(pgwt_execution_counts_known(&(struct pgwt_execution){
              .n_events = 7, .n_workers = 2}) == 1,
          "7/2 is known");

    struct pgwt_execution r = index_row();
    CHECK(pgwt_execution_counts_known(&r) == 0,
          "a fully poisoned index row is not serialisable");

    /* Each field alone is enough to refuse: a guard that required BOTH to be
     * poison would pass a half-poisoned row, which is exactly the
     * "satisfied without checking anything" shape. */
    r = raw_row(5, PGWT_EXEC_INDEX_NOT_INDEXED, 1);
    CHECK(pgwt_execution_counts_known(&r) == 0,
          "poisoned n_workers alone refuses (n_events was real)");
    r = raw_row(PGWT_EXEC_INDEX_NOT_INDEXED, 5, 1);
    CHECK(pgwt_execution_counts_known(&r) == 0,
          "poisoned n_events alone refuses (n_workers was real)");

    /* A DIFFERENT negative sentinel. Found by G08 of
     * tests/exec_index_guard_mutations.py: `!= -1` and `>= 0` agree on
     * every fixture above, so the harness turned `>= 0` into `!= -1` and
     * this test stayed GREEN -- a blind spot in the test, not a surviving
     * mutant. A count is a cardinality; anything negative is not one. */
    r = raw_row(-7, 2, 1);
    CHECK(pgwt_execution_counts_known(&r) == 0,
          "n_events = -7 is not a count either (not just the -1 sentinel)");
    r = raw_row(2, INT_MIN, 1);
    CHECK(pgwt_execution_counts_known(&r) == 0,
          "n_workers = INT_MIN is not a count");

    /* And the counts guard is INDEPENDENT of the filter verdict: a row may
     * have a real matches_event_filter and still carry no counts. */
    r = raw_row(PGWT_EXEC_INDEX_NOT_INDEXED, PGWT_EXEC_INDEX_NOT_INDEXED, 1);
    CHECK(pgwt_execution_filter_verdict(&r, &(struct pgwt_filter){0}) == 1 &&
          pgwt_execution_counts_known(&r) == 0,
          "filter-decidable but not serialisable: the two guards are "
          "independent and both must be consulted");
}

int main(void)
{
    printf("=== test_exec_index_guards ===\n");
    printf("--- 1. the two poison sentinels agree ---\n");
    test_sentinels_agree();
    printf("--- 2. differential vs the predicate that shipped ---\n");
    test_class_filter_on_a_poisoned_row();
    test_event_id_filter_on_a_poisoned_row();
    test_both_dimensions_set();
    printf("--- 3. bypass suite: the guard made blind or unreachable ---\n");
    bypass_real_zero_is_blind();
    bypass_no_event_dimension_is_blind();
    bypass_boundary_dimensions_stay_decidable();
    bypass_other_negative_sentinels();
    bypass_real_match_still_served();
    bypass_null_inputs();
    printf("--- 4. the serialisation guard ---\n");
    test_counts_known();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
