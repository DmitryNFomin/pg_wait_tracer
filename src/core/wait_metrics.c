#include "pgwt_core.h"

#include <string.h>

static uint64_t min_time(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t max_time(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* contract 4: half-open intersection, subtract only after ordering. */
static uint64_t clipped_ns(uint64_t start, uint64_t end,
                           uint64_t from, uint64_t to)
{
    uint64_t a = max_time(start, from), b = min_time(end, to);
    return a < b ? b - a : 0;
}

/* contract 1: nonempty events, compared exactly without a registry. */
static bool valid_event(pgwt_core_event event)
{
    return event.data != NULL && event.length != 0;
}

static bool same_event(pgwt_core_event a, pgwt_core_event b)
{
    return a.length == b.length && memcmp(a.data, b.data, a.length) == 0;
}

/* contract 2.1-2 and 7.2: lifetime validity is independent of waits. */
static bool valid_lifetime(const pgwt_core_evidence *e)
{
    return !(e->invalid_fields & PGWT_CORE_LIFE_INVALID) &&
           (!e->has_life_end || e->life_start <= e->life_end);
}

/* contract 2.7-8, 7.1-2: empty spans never overlap positive spans, but
 * still participate in start ordering and can certify the exit. */
static bool valid_coverage(const pgwt_core_evidence *e)
{
    uint64_t positive_end = 0;
    if (!valid_lifetime(e) ||
        (e->invalid_fields & PGWT_CORE_COVERAGE_INVALID) ||
        (e->coverage_count && !e->coverage))
        return false;
    if (e->invalid_fields & PGWT_CORE_OPEN_AS_OF_INVALID)
        return false;
    for (size_t i = 0; i < e->coverage_count; ++i) {
        pgwt_core_span s = e->coverage[i];
        if (s.start > s.end || s.start < e->life_start ||
            (e->has_life_end && s.end > e->life_end) ||
            (i && s.start < e->coverage[i - 1].start))
            return false;
        /* contract 2.8 */
        if (e->has_open && s.end > e->open.as_of)
            return false;
        if (s.start < s.end) {
            if (s.start < positive_end)
                return false;
            positive_end = s.end;
        }
    }
    return true;
}

/* Sort by start then end, with zero-length waits before positive waits at
 * the same start. Heapsort has bounded O(n log n) work and allocates nothing. */
static bool later_wait(pgwt_core_wait a, pgwt_core_wait b)
{
    return a.start > b.start || (a.start == b.start && a.end > b.end);
}

static void sift_down(pgwt_core_wait *waits, size_t root, size_t count)
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && later_wait(waits[child + 1], waits[child]))
            ++child;
        if (!later_wait(waits[child], waits[root])) break;
        pgwt_core_wait tmp = waits[root];
        waits[root] = waits[child];
        waits[child] = tmp;
        root = child;
    }
}

static void sort_waits(pgwt_core_wait *waits, size_t count)
{
    for (size_t i = count / 2; i > 0; --i) sift_down(waits, i - 1, count);
    for (size_t i = count; i > 1; --i) {
        pgwt_core_wait tmp = waits[0];
        waits[0] = waits[i - 1];
        waits[i - 1] = tmp;
        sift_down(waits, 0, i - 1);
    }
}

static bool valid_waits(const pgwt_core_evidence *e, pgwt_core_wait *scratch)
{
    if (e->invalid_fields || (e->wait_count && !e->waits))
        return false;
    for (size_t i = 0; i < e->wait_count; ++i) scratch[i] = e->waits[i];
    sort_waits(scratch, e->wait_count);
    uint64_t positive_end = 0;
    for (size_t i = 0; i < e->wait_count; ++i) {
        pgwt_core_wait w = scratch[i];
        /* contract 2.1, 2.3 */
        if (!valid_event(w.event) || w.start > w.end ||
            w.start < e->life_start ||
            (e->has_life_end && w.end > e->life_end))
            return false;
        /* contract 2.4-5: the previous positive interval's end suffices
         * after sorting. Zeros at its start sort before it; at its end they
         * pass. Duplicate zeros remain distinct; positive duplicates fail. */
        if (w.start < positive_end) return false;
        if (w.start < w.end) positive_end = w.end;
        /* contract 2.6: also excludes zero-length closes after open.start. */
        if (e->has_open && w.end > e->open.start)
            return false;
    }
    /* contract 2.6 */
    if (e->has_open &&
        (!valid_event(e->open.event) || e->open.start > e->open.as_of ||
         e->open.start < e->life_start ||
         (e->has_life_end && e->open.as_of >= e->life_end)))
        return false;
    return true;
}

/* contract 4: sum only valid disjoint spans, so the sum fits uint64_t. */
static uint64_t covered_ns(const pgwt_core_evidence *e, uint64_t a, uint64_t b)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < e->coverage_count; ++i)
        sum += clipped_ns(e->coverage[i].start, e->coverage[i].end, a, b);
    return sum;
}

/* contract 4 and 7.1: an empty span at the exit is sufficient. */
static bool exit_certified(const pgwt_core_evidence *e, const pgwt_core_query *q)
{
    if (!e->has_life_end || e->life_end < q->from || e->life_end >= q->to)
        return true;
    for (size_t i = 0; i < e->coverage_count; ++i)
        if (e->coverage[i].end == e->life_end)
            return true;
    return false;
}

static void init_result(pgwt_core_result *r, const char *metric, const char *unit)
{
    memset(r, 0, sizeof(*r));
    r->metric = metric;
    r->version = 1;
    r->unit = unit;
    r->basis = "observed";
    r->completeness = "unavailable";
}

/* contract 5: all applicable reasons in alphabetical order. */
static void reasons(pgwt_core_result *r, bool gap, bool evidence_invalid,
                    bool query_invalid, bool window_invalid)
{
    if (gap) r->reasons[r->reason_count++] = "coverage_incomplete";
    if (evidence_invalid) r->reasons[r->reason_count++] = "evidence_invalid";
    if (query_invalid) r->reasons[r->reason_count++] = "invalid_query";
    if (window_invalid) r->reasons[r->reason_count++] = "invalid_window";
}

int pgwt_core_prepare(const pgwt_core_evidence *e, pgwt_core_wait *scratch,
                      size_t scratch_len, pgwt_core_prepared *prepared)
{
    *prepared = (pgwt_core_prepared){0};
    if (!e) return 0;
    if (e->wait_count > scratch_len || (e->wait_count && !scratch)) return -1;
    prepared->evidence = e;
    prepared->coverage_knowable = valid_coverage(e);
    prepared->evidence_valid = prepared->coverage_knowable && valid_waits(e, scratch);
    return 0;
}

void pgwt_core_wait_metrics(const pgwt_core_prepared *prepared,
                            const pgwt_core_query *q,
                            pgwt_core_result *time,
                            pgwt_core_result *completed)
{
    /* Null input objects are malformed; outputs must be distinct and non-null. */
    const pgwt_core_evidence missing_e = {0};
    const pgwt_core_evidence *e = prepared && prepared->evidence ?
                                  prepared->evidence : &missing_e;
    const pgwt_core_query missing_q = {
        .invalid_fields = PGWT_CORE_QUERY_OBJECT_INVALID |
                          PGWT_CORE_QUERY_FROM_INVALID | PGWT_CORE_QUERY_TO_INVALID
    };
    if (!q) q = &missing_q;
    init_result(time, "wait_time", "ns");
    init_result(completed, "completed_waits", "count");

    /* contract 3, 7.4: invalid_window depends on the two endpoint tokens
     * independently of event validity and other query shape failures. */
    bool endpoints_valid = !(q->invalid_fields &
        (PGWT_CORE_QUERY_FROM_INVALID | PGWT_CORE_QUERY_TO_INVALID));
    bool invalid_window = endpoints_valid && q->from >= q->to;
    bool invalid_query = q->invalid_fields || !valid_event(q->event);
    bool coverage_knowable = prepared && prepared->coverage_knowable;
    bool evidence_invalid = !prepared || !prepared->evidence_valid;
    bool coverage_gap = false;
    bool completion_gap = false;
    uint64_t required = 0, covered = 0;

    /* contract 4, 5, 7.2: invalid waits cannot hide a knowable coverage gap;
     * invalid query/window or invalid lifetime/coverage cannot establish one. */
    if (coverage_knowable && !invalid_query && !invalid_window) {
        uint64_t a = max_time(q->from, e->life_start);
        uint64_t b = e->has_life_end ? min_time(q->to, e->life_end) : q->to;
        required = a < b ? b - a : 0;
        covered = covered_ns(e, a, b);
        coverage_gap = covered != required;
        completion_gap = coverage_gap || !exit_certified(e, q);
    }
    reasons(time, coverage_gap, evidence_invalid, invalid_query, invalid_window);
    reasons(completed, completion_gap, evidence_invalid, invalid_query, invalid_window);

    /* contract 7.3: ALL diagnostics stay null on any of these refusals. */
    if (evidence_invalid || invalid_query || invalid_window)
        return;

    /* contract 4, 5: use ALL recorded waits, including those outside coverage.
     * Validate first: disjoint occupied times bound the duration sum by to-from
     * even at UINT64_MAX. Never sum invalid overlapping full-range records. */
    uint64_t closed = 0, open = 0, count = 0;
    for (size_t i = 0; i < e->wait_count; ++i) {
        pgwt_core_wait w = e->waits[i];
        if (!same_event(w.event, q->event))
            continue;
        closed += clipped_ns(w.start, w.end, q->from, q->to);
        /* contract 4: zero-length waits count; endpoints are half-open. */
        if (q->from <= w.end && w.end < q->to)
            ++count;
    }
    if (e->has_open && same_event(e->open.event, q->event))
        open = clipped_ns(e->open.start, e->open.as_of, q->from, q->to);
    uint64_t recorded = closed + open;
    time->diagnostic.recorded = (pgwt_core_optional_u64){true, recorded};
    completed->diagnostic.recorded = (pgwt_core_optional_u64){true, count};
    time->diagnostic.covered_ns = completed->diagnostic.covered_ns =
        (pgwt_core_optional_u64){true, covered};
    time->diagnostic.required_ns = completed->diagnostic.required_ns =
        (pgwt_core_optional_u64){true, required};

    /* contract 5: a diagnostic never becomes a value on incomplete coverage. */
    time->value.present = !coverage_gap;
    time->value.value = recorded;
    completed->value.present = !completion_gap;
    completed->value.value = count;
    if (time->value.present) time->completeness = "complete";
    if (completed->value.present) completed->completeness = "complete";
    time->detail_present = time->value.present;
    if (time->detail_present) {
        time->detail.closed_ns = closed;
        time->detail.open_ns = open;
    }
}
