/* backend_span.c — the backend active span reducer (#455).
 *
 * Read src/backend_span.h first: it carries the contract, the two
 * independent axes and the ONE forbidden operation (no duration may ever be
 * derived from DB Time / Execs / Avg Exec — every number here comes from a
 * record timestamp).
 *
 * ONE PASS, NO RESCAN. `pgwt_span_reduce` walks the caller's already-loaded,
 * already-ordered event array exactly once. src/server.c's Queries handler
 * calls it on the `all_events` array it loads anyway and then answers each
 * row by an O(1) hash lookup — the per-query rescan that cost #265/#275 two
 * seconds of paint is not reintroduced here.
 *
 * REFUSAL, NEVER A SHORT ANSWER. Every table below grows; none silently
 * drops the record that would not fit. An allocation failure, an exhausted
 * budget or an input the pass cannot trust (records out of timestamp order,
 * so a span's boundaries would be invented rather than observed) sets
 * `out->failed` with a reason, and the caller must report unavailability.
 * This is the #363 lesson: leaving the surplus pids' records untracked and
 * returning normally is a WRONG answer, not a smaller one.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "backend_span.h"

#define SPAN_DEFAULT_SELECTED  50
#define SPAN_MAX_SELECTED      1000
/* Distinct query ids the per-row index will hold. Past this the pass
 * REFUSES: a row index that silently stopped indexing reports "no span" for
 * a query that has one, which is indistinguishable from the #455 defect. */
#define SPAN_MAX_QID_SLOTS     65536
/* Spans the pass will produce. Past this it refuses for the same reason:
 * the selection would be drawn from a prefix of the capture. */
#define SPAN_MAX_TOTAL         4000000

/* Test-only allocation injection — the same env var and the same shape as
 * src/server.c's test_load_alloc_failure and src/compute.c's
 * tag_alloc_fail, because this pass runs inside the same request. An
 * unknown point name must fail nothing. */
static int span_alloc_fail(const char *point)
{
    const char *fail = getenv("PGWT_TEST_LOAD_ALLOC_FAIL");
    return fail && strcmp(fail, point) == 0;
}

/* ── reasons (static strings; never freed, never formatted) ───────────── */

static const char *R_NO_CMD      =
    "no CMD_START/CMD_END markers cover this activity: not measured";
static const char *R_NO_START    =
    "start not captured: the command was already open when the capture or "
    "window began";
static const char *R_NO_END      =
    "end not observed: the backend was still active at the end of the "
    "processed stream";
static const char *R_MALFORMED   =
    "malformed marker pair: a CMD_START arrived while a command was still "
    "open";
static const char *R_GAP         =
    "a known capture gap falls inside this span: it is not bridged into a "
    "fully measured duration";
static const char *R_SAMPLED     =
    "sampled records fall inside this span: the exact tier was not active "
    "throughout";
static const char *R_ESCALATION  =
    "an escalation-window boundary falls inside this span: marker coverage "
    "changes inside it";

/* ── per-pid sweep state ─────────────────────────────────────────────── */

struct span_pid_state {
    uint32_t pid;              /* 0 = empty slot */
    uint8_t  used;
    uint8_t  seen_cmd;         /* a CMD marker has been seen for this pid */
    uint8_t  open;             /* a span is currently open */
    struct pgwt_backend_span cur;
};

struct span_ctx {
    struct span_pid_state *pid_ht;
    int pid_cap;               /* power of two */
    int pid_used;

    struct pgwt_span_qid_slot *qid_ht;
    int qid_cap;               /* power of two */
    int qid_used;

    struct pgwt_backend_span *sel;
    int sel_cap, sel_n;

    uint64_t *esc;             /* escalation marker timestamps, ascending */
    int esc_n, esc_cap;

    const struct pgwt_span_params *p;
    struct pgwt_span_result *out;
    int refused;
};

static void ctx_refuse(struct span_ctx *c, const char *why)
{
    if (!c->refused) {
        c->refused = 1;
        c->out->failed = 1;
        c->out->refusal_reason = why;
    }
}

static uint64_t span_mix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/* ── per-pid table (grows; never drops) ──────────────────────────────── */

static int pid_ht_grow(struct span_ctx *c);

static struct span_pid_state *pid_get(struct span_ctx *c, uint32_t pid)
{
    if (c->pid_used * 2 >= c->pid_cap && pid_ht_grow(c) != 0)
        return NULL;
    uint64_t h = span_mix64(pid);
    int mask = c->pid_cap - 1;
    int i = (int)(h & (uint64_t)mask);
    for (int probe = 0; probe < c->pid_cap; probe++) {
        struct span_pid_state *st = &c->pid_ht[i];
        if (st->used && st->pid == pid)
            return st;
        if (!st->used) {
            memset(st, 0, sizeof(*st));
            st->used = 1;
            st->pid = pid;
            c->pid_used++;
            return st;
        }
        i = (i + 1) & mask;
    }
    return NULL;   /* unreachable: load factor is held at 1/2 */
}

static int pid_ht_grow(struct span_ctx *c)
{
    int newcap = c->pid_cap ? c->pid_cap * 2 : 256;
    if (newcap <= c->pid_cap)
        return -1;
    struct span_pid_state *nt = span_alloc_fail("span_pid_ht")
                              ? NULL : calloc((size_t)newcap, sizeof(*nt));
    if (!nt)
        return -1;
    struct span_pid_state *old = c->pid_ht;
    int oldcap = c->pid_cap;
    c->pid_ht = nt;
    c->pid_cap = newcap;
    c->pid_used = 0;
    int mask = newcap - 1;
    for (int j = 0; j < oldcap; j++) {
        if (!old[j].used)
            continue;
        uint64_t h = span_mix64(old[j].pid);
        int i = (int)(h & (uint64_t)mask);
        while (c->pid_ht[i].used)
            i = (i + 1) & mask;
        c->pid_ht[i] = old[j];
        c->pid_used++;
    }
    free(old);
    return 0;
}

/* ── per-query-id row index (grows; refuses past the budget) ─────────── */

static int qid_ht_grow(struct span_ctx *c);

static struct pgwt_span_qid_slot *qid_get(struct span_ctx *c, uint64_t qid)
{
    if (c->qid_used * 2 >= c->qid_cap && qid_ht_grow(c) != 0)
        return NULL;
    uint64_t h = span_mix64(qid);
    int mask = c->qid_cap - 1;
    int i = (int)(h & (uint64_t)mask);
    for (int probe = 0; probe < c->qid_cap; probe++) {
        struct pgwt_span_qid_slot *s = &c->qid_ht[i];
        if (s->used && s->query_id == qid)
            return s;
        if (!s->used) {
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->query_id = qid;
            c->qid_used++;
            return s;
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

static int qid_ht_grow(struct span_ctx *c)
{
    int newcap = c->qid_cap ? c->qid_cap * 2 : 64;
    if (newcap > SPAN_MAX_QID_SLOTS) {
        ctx_refuse(c, "the per-query span index exceeded its slot budget");
        return -1;
    }
    struct pgwt_span_qid_slot *nt = span_alloc_fail("span_qid_ht")
                                  ? NULL : calloc((size_t)newcap, sizeof(*nt));
    if (!nt) {
        ctx_refuse(c, "the per-query span index could not grow");
        return -1;
    }
    struct pgwt_span_qid_slot *old = c->qid_ht;
    int oldcap = c->qid_cap;
    c->qid_ht = nt;
    c->qid_cap = newcap;
    int used = c->qid_used;
    c->qid_used = 0;
    int mask = newcap - 1;
    for (int j = 0; j < oldcap && used > 0; j++) {
        struct pgwt_span_qid_slot *s = &old[j];
        if (!s->used)
            continue;
        uint64_t h = span_mix64(s->query_id);
        int i = (int)(h & (uint64_t)mask);
        while (c->qid_ht[i].used)
            i = (i + 1) & mask;
        c->qid_ht[i] = *s;
        c->qid_used++;
        used--;
    }
    free(old);
    return 0;
}

/* ── public accessors ────────────────────────────────────────────────── */

int pgwt_span_observed_elapsed_ns(const struct pgwt_backend_span *s,
                                  uint64_t *out_ns)
{
    if (out_ns) *out_ns = 0;
    uint64_t lo, hi;
    if (s->start_observed)
        lo = s->start_ns;
    else if (s->has_evidence)
        lo = s->first_evidence_ns;
    else
        return 0;
    if (s->end_observed)
        hi = s->end_ns;
    else if (s->has_evidence)
        hi = s->last_evidence_ns;
    else
        return 0;
    if (hi <= lo)
        return 0;
    if (out_ns) *out_ns = hi - lo;
    return 1;
}

int pgwt_span_completed_duration_ns(const struct pgwt_backend_span *s,
                                    uint64_t *out_ns)
{
    if (out_ns) *out_ns = 0;
    /* BOTH boundaries, OBSERVED. The window end is not a boundary. */
    if (!s->start_observed || !s->end_observed)
        return 0;
    if (s->end_ns < s->start_ns)
        return 0;
    if (out_ns) *out_ns = s->end_ns - s->start_ns;
    return 1;
}

const char *pgwt_span_measurement_name(int m)
{
    switch (m) {
    case PGWT_SPAN_MEASURED:  return "measured";
    case PGWT_SPAN_PARTIAL:   return "partial";
    default:                  return "unmeasured";
    }
}

const char *pgwt_span_completion_name(int c)
{
    switch (c) {
    case PGWT_SPAN_CLOSED:    return "closed";
    case PGWT_SPAN_IN_FLIGHT: return "in_flight";
    default:                  return "unknown";
    }
}

const char *pgwt_span_kind_name(const struct pgwt_backend_span *s)
{
    if (s->measurement == PGWT_SPAN_UNMEASURED)
        return "Not measured";
    if (s->completion == PGWT_SPAN_IN_FLIGHT)
        return "In flight";
    if (s->completion == PGWT_SPAN_CLOSED)
        return "Closed";
    return "Not measured";
}

char *pgwt_span_identity(const struct pgwt_backend_span *s,
                         char *buf, size_t len)
{
    if (!buf || len == 0)
        return buf;
    if (s->start_observed)
        snprintf(buf, len, "%u:%llu", s->pid,
                 (unsigned long long)s->identity_ns);
    else if (s->has_evidence)
        snprintf(buf, len, "%u:~%llu", s->pid,
                 (unsigned long long)s->first_evidence_ns);
    else
        snprintf(buf, len, "%u:?", s->pid);
    return buf;
}

const struct pgwt_backend_span *
pgwt_span_row_pick(const struct pgwt_span_qid_slot *slot)
{
    if (!slot)
        return NULL;
    if (slot->has_inflight)
        return &slot->best_inflight;
    if (slot->has_closed)
        return &slot->best_closed;
    if (slot->has_other)
        return &slot->best_other;
    return NULL;
}

/* Binary search: `qid_slots` is compacted in ASCENDING query-id order by
 * pgwt_span_reduce, which is what makes the Queries row loop O(rows x log
 * slots) instead of O(rows x slots). The ordering is asserted by the
 * compaction itself (an insertion sort on the dense array), so this is not
 * an assumption about hash iteration order. */
const struct pgwt_span_qid_slot *
pgwt_span_qid_lookup(const struct pgwt_span_result *r, uint64_t query_id)
{
    if (!r || r->failed || !r->qid_slots)
        return NULL;
    int lo = 0, hi = r->num_qid_slots - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t k = r->qid_slots[mid].query_id;
        if (k == query_id)
            return &r->qid_slots[mid];
        if (k < query_id) lo = mid + 1;
        else              hi = mid - 1;
    }
    return NULL;
}

/* ── span ordering ───────────────────────────────────────────────────── */

/* The detail listing order: in-flight first (an operator looking at a
 * blocked cluster wants the thing that is still blocked), then observed
 * elapsed descending, tie-broken by pid then identity so the order is a
 * function of the data and not of hash iteration. */
static int span_better(const struct pgwt_backend_span *a,
                       const struct pgwt_backend_span *b)
{
    int ai = a->completion == PGWT_SPAN_IN_FLIGHT;
    int bi = b->completion == PGWT_SPAN_IN_FLIGHT;
    if (ai != bi) return ai > bi;
    uint64_t ae = 0, be = 0;
    pgwt_span_observed_elapsed_ns(a, &ae);
    pgwt_span_observed_elapsed_ns(b, &be);
    if (ae != be) return ae > be;
    if (a->pid != b->pid) return a->pid < b->pid;
    return a->identity_ns < b->identity_ns;
}

static void sel_insert(struct span_ctx *c, const struct pgwt_backend_span *s)
{
    int cap = c->sel_cap;
    if (c->sel_n == cap) {
        /* O(1) reject before any O(k) shuffle. */
        if (!span_better(s, &c->sel[cap - 1]))
            return;
        c->sel_n--;
    }
    int i = c->sel_n;
    while (i > 0 && span_better(s, &c->sel[i - 1])) {
        c->sel[i] = c->sel[i - 1];
        i--;
    }
    c->sel[i] = *s;
    c->sel_n++;
}

/* ── span finalisation ───────────────────────────────────────────────── */

static int esc_inside(const struct span_ctx *c, uint64_t lo, uint64_t hi)
{
    /* Escalation markers carry pid 0 and are therefore not per-pid state;
     * they are collected in stream order and probed here by binary search,
     * so a rare marker costs log n rather than a walk of every open span. */
    int a = 0, b = c->esc_n;
    while (a < b) {
        int mid = a + (b - a) / 2;
        if (c->esc[mid] < lo) a = mid + 1;
        else                  b = mid;
    }
    return a < c->esc_n && c->esc[a] <= hi;
}

static int gap_inside(const struct pgwt_span_params *p,
                      uint64_t lo, uint64_t hi, const char **why)
{
    for (int i = 0; i < p->n_gaps; i++) {
        const struct pgwt_span_gap *g = &p->gaps[i];
        if (g->from_ns <= hi && lo <= g->to_ns) {
            if (why) *why = g->reason ? g->reason : R_GAP;
            return 1;
        }
    }
    return 0;
}

/* The DETAIL-LISTING scope. The pid is NOT checked here: a pid scopes the
 * whole pass (see pgwt_span_params.pid), so every span that reaches this
 * point already belongs to it. */
static int span_matches_scope(const struct pgwt_span_params *p,
                              const struct pgwt_backend_span *s)
{
    if (p->unattributed_only && s->n_qids != 0)
        return 0;
    if (p->query_id != 0) {
        int hit = 0;
        for (uint32_t i = 0; i < s->n_qids; i++)
            if (s->qids[i].query_id == p->query_id) { hit = 1; break; }
        if (!hit)
            return 0;
    }
    return 1;
}

/* Fold one finished span into the query-id row index. A span with several
 * associations appears under EVERY one of them with the SAME identity — its
 * duration is NOT additive across those rows, which is why the identity is
 * carried verbatim rather than a per-row share being computed. */
static int qid_fold(struct span_ctx *c, const struct pgwt_backend_span *s)
{
    for (uint32_t k = 0; k < s->n_qids; k++) {
        struct pgwt_span_qid_slot *slot = qid_get(c, s->qids[k].query_id);
        if (!slot)
            return -1;
        if (s->is_worker) {
            uint64_t e = 0;
            pgwt_span_observed_elapsed_ns(s, &e);
            slot->n_worker_spans++;
            slot->has_worker = 1;
            /* NEVER added to the leader's wall time: a separate maximum. */
            if (e > slot->worker_observed_max_ns)
                slot->worker_observed_max_ns = e;
            continue;
        }
        slot->n_leader_spans++;
        if (s->completion == PGWT_SPAN_IN_FLIGHT) {
            uint64_t e = 0, be = 0;
            pgwt_span_observed_elapsed_ns(s, &e);
            if (slot->has_inflight)
                pgwt_span_observed_elapsed_ns(&slot->best_inflight, &be);
            if (!slot->has_inflight || e > be ||
                (e == be && span_better(s, &slot->best_inflight))) {
                slot->best_inflight = *s;
                slot->has_inflight = 1;
            }
        } else if (s->completion == PGWT_SPAN_CLOSED &&
                   s->measurement == PGWT_SPAN_MEASURED) {
            /* "the longest FULLY MEASURED closed span" — a partial close is
             * not eligible for this slot, it falls through to best_other. */
            uint64_t d = 0, bd = 0;
            pgwt_span_completed_duration_ns(s, &d);
            if (slot->has_closed)
                pgwt_span_completed_duration_ns(&slot->best_closed, &bd);
            if (!slot->has_closed || d > bd ||
                (d == bd && span_better(s, &slot->best_closed))) {
                slot->best_closed = *s;
                slot->has_closed = 1;
            }
        } else {
            uint64_t e = 0, be = 0;
            pgwt_span_observed_elapsed_ns(s, &e);
            if (slot->has_other)
                pgwt_span_observed_elapsed_ns(&slot->best_other, &be);
            if (!slot->has_other || e > be ||
                (e == be && span_better(s, &slot->best_other))) {
                slot->best_other = *s;
                slot->has_other = 1;
            }
        }
    }
    return 0;
}

static void span_classify(struct span_ctx *c, struct pgwt_backend_span *s,
                          int stream_ended)
{
    uint64_t lo = s->start_observed ? s->start_ns
                : s->has_evidence ? s->first_evidence_ns : 0;
    uint64_t hi = s->end_observed ? s->end_ns
                : s->has_evidence ? s->last_evidence_ns : lo;

    const char *gap_why = NULL;
    if (lo || hi) {
        if (gap_inside(c->p, lo, hi, &gap_why))
            s->gap_inside = 1;
        if (esc_inside(c, lo, hi))
            s->escalation_inside = 1;
    }

    /* completion */
    if (s->end_observed)
        s->completion = PGWT_SPAN_CLOSED;
    else if (s->start_observed && stream_ended)
        s->completion = PGWT_SPAN_IN_FLIGHT;
    else
        s->completion = PGWT_SPAN_UNKNOWN;

    /* measurement */
    if (!s->start_observed && !s->end_observed)
        s->measurement = PGWT_SPAN_UNMEASURED;
    else if (s->start_observed && s->end_observed && !s->gap_inside &&
             !s->sampled_inside && !s->escalation_inside && !s->malformed)
        s->measurement = PGWT_SPAN_MEASURED;
    else
        s->measurement = PGWT_SPAN_PARTIAL;

    /* reason: the FIRST thing that stops this being a fully measured,
     * cleanly closed span. Never NULL unless nothing does. */
    s->reason = NULL;
    if (s->measurement == PGWT_SPAN_UNMEASURED)
        s->reason = R_NO_CMD;
    else if (s->malformed)
        s->reason = R_MALFORMED;
    else if (s->gap_inside)
        s->reason = gap_why ? gap_why : R_GAP;
    else if (s->escalation_inside)
        s->reason = R_ESCALATION;
    else if (s->sampled_inside)
        s->reason = R_SAMPLED;
    else if (!s->start_observed)
        s->reason = R_NO_START;
    else if (!s->end_observed)
        s->reason = R_NO_END;

    s->multi_exec = s->n_exec_starts > 1;
    s->multi_qid  = s->n_qids > 1;
}

static void span_emit(struct span_ctx *c, struct pgwt_backend_span *s,
                      int stream_ended)
{
    if (c->refused)
        return;
    span_classify(c, s, stream_ended);

    c->out->total_spans++;
    if (c->out->total_spans > SPAN_MAX_TOTAL) {
        ctx_refuse(c, "the span pass exceeded its span budget");
        return;
    }
    if (s->is_worker)
        c->out->n_worker_spans++;
    switch (s->completion) {
    case PGWT_SPAN_CLOSED:    c->out->n_closed++; break;
    case PGWT_SPAN_IN_FLIGHT: c->out->n_inflight++; break;
    default: break;
    }
    if (s->measurement == PGWT_SPAN_UNMEASURED) c->out->n_unmeasured++;
    else if (s->measurement == PGWT_SPAN_PARTIAL) c->out->n_partial++;

    /* Every span the pass produced is folded into the row index. The pass
     * is already pid-scoped (records for other pids never reach it), and
     * the query/unattributed scopes below are DETAIL scopes: narrowing the
     * drill must not change the row summary of a different query. */
    if (qid_fold(c, s) != 0) {
        ctx_refuse(c, "the per-query span index could not grow");
        return;
    }
    if (span_matches_scope(c->p, s)) {
        c->out->scoped_spans++;
        sel_insert(c, s);
    }
}

/* ── association ─────────────────────────────────────────────────────── */

static void span_assoc(struct pgwt_backend_span *s, uint64_t qid,
                       int from_exec_start)
{
    if (qid == 0)
        return;
    for (uint32_t i = 0; i < s->n_qids; i++) {
        if (s->qids[i].query_id == qid) {
            if (from_exec_start) s->qids[i].n_exec_starts++;
            else                 s->qids[i].n_records++;
            return;
        }
    }
    if (s->n_qids >= PGWT_SPAN_MAX_QIDS) {
        s->n_qids_dropped++;
        return;
    }
    struct pgwt_span_qid *q = &s->qids[s->n_qids++];
    q->query_id = qid;
    q->n_records = from_exec_start ? 0 : 1;
    q->n_exec_starts = from_exec_start ? 1 : 0;
}

static void span_open(struct span_pid_state *st, uint32_t pid,
                      int start_observed, uint64_t ts, uint32_t leader_pid)
{
    memset(&st->cur, 0, sizeof(st->cur));
    st->cur.pid = pid;
    st->cur.leader_pid = leader_pid;
    st->cur.is_worker = leader_pid != 0;
    if (start_observed) {
        st->cur.start_observed = 1;
        st->cur.start_ns = ts;
        st->cur.identity_ns = ts;
    }
    st->open = 1;
}

static uint32_t leader_of(const struct pgwt_span_params *p, uint32_t pid)
{
    for (int i = 0; i < p->n_links; i++)
        if (p->links[i].pid == pid)
            return p->links[i].leader_pid == pid ? 0 : p->links[i].leader_pid;
    return 0;
}

/* ── the pass ────────────────────────────────────────────────────────── */

void pgwt_span_reduce(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_span_params *params,
                      struct pgwt_span_result *out)
{
    memset(out, 0, sizeof(*out));
    static const struct pgwt_span_params empty_params = {0};
    const struct pgwt_span_params *p = params ? params : &empty_params;

    int max_sel = p->max_selected > 0 ? p->max_selected : SPAN_DEFAULT_SELECTED;
    if (max_sel > SPAN_MAX_SELECTED) max_sel = SPAN_MAX_SELECTED;

    struct span_ctx c;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.out = out;

    /* An EMPTY input is not a failure: there is nothing to reduce, so the
     * result is complete and says so with zeroes in the COUNTERS and no
     * spans. It is the caller's job not to render "0 s" for it — every
     * per-span duration accessor returns 0-with-false in that case, and the
     * bypass suite asserts the empty result is distinguishable from a
     * measured one. */
    c.sel = span_alloc_fail("span_sel")
          ? NULL : calloc((size_t)max_sel, sizeof(*c.sel));
    if (!c.sel) {
        out->failed = 1;
        out->refusal_reason = "the span selection buffer could not be "
                              "allocated";
        return;
    }
    c.sel_cap = max_sel;

    if (count < 0)
        count = 0;

    /* Order check FIRST, over the whole array, before a single span
     * boundary is taken from it. The loader guarantees a timestamp-ordered
     * array (src/server.c stable_sort_events); if that guarantee is ever
     * broken, a span's "observed" start could be a later record's
     * timestamp, which is an invented boundary. Refuse instead. */
    for (int i = 1; i < count; i++) {
        if (events[i - 1].timestamp_ns > events[i].timestamp_ns) {
            free(c.sel);
            memset(out, 0, sizeof(*out));
            out->failed = 1;
            out->refusal_reason = "the event array is not ordered by "
                                  "timestamp: span boundaries would be "
                                  "invented rather than observed";
            return;
        }
    }

    /* Pass 0: escalation boundaries (pid 0, not per-pid state). */
    for (int i = 0; i < count && !c.refused; i++) {
        uint32_t m = events[i].old_event;
        if (m != PGWT_MARKER_ESCALATE_START && m != PGWT_MARKER_ESCALATE_END)
            continue;
        if (c.esc_n == c.esc_cap) {
            int nc = c.esc_cap ? c.esc_cap * 2 : 16;
            uint64_t *t = span_alloc_fail("span_esc")
                        ? NULL : realloc(c.esc, (size_t)nc * sizeof(*t));
            if (!t) {
                ctx_refuse(&c, "the escalation-boundary list could not grow");
                break;
            }
            c.esc = t;
            c.esc_cap = nc;
        }
        c.esc[c.esc_n++] = events[i].timestamp_ns;
    }

    for (int i = 0; i < count && !c.refused; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        uint32_t m = ev->old_event;

        if (m == PGWT_MARKER_ESCALATE_START || m == PGWT_MARKER_ESCALATE_END)
            continue;   /* pid 0: handled in pass 0 */
        if (p->pid != 0 && ev->pid != p->pid)
            continue;   /* a pid scopes the whole pass */

        struct span_pid_state *st = pid_get(&c, ev->pid);
        if (!st) {
            ctx_refuse(&c, "the per-pid span table could not grow");
            break;
        }

        if (PGWT_IS_MARKER(m)) {
            switch (m) {
            case PGWT_MARKER_CMD_START:
                if (st->open) {
                    /* Malformed pair. The open span is closed WITH ITS
                     * REASON rather than being silently extended across
                     * two commands. */
                    st->cur.malformed = 1;
                    span_emit(&c, &st->cur, 0);
                    st->open = 0;
                }
                st->seen_cmd = 1;
                span_open(st, ev->pid, 1, ev->timestamp_ns,
                          leader_of(p, ev->pid));
                /* DELIBERATELY NOT associating ev->query_id: the id BPF
                 * resolved at CMD_START is the PREVIOUS statement's
                 * (compute.c:492-504 ignores it for the same reason). */
                break;
            case PGWT_MARKER_CMD_END:
                if (!st->open) {
                    /* Left window edge: a command that was already open
                     * when the capture or window began. The span is real;
                     * its start was not observed. */
                    span_open(st, ev->pid, 0, 0, leader_of(p, ev->pid));
                }
                st->seen_cmd = 1;
                st->cur.end_observed = 1;
                st->cur.end_ns = ev->timestamp_ns;
                span_emit(&c, &st->cur, 0);
                st->open = 0;
                break;
            case PGWT_MARKER_EXEC_START:
                if (!st->open)
                    span_open(st, ev->pid, 0, 0, leader_of(p, ev->pid));
                st->cur.n_exec_starts++;
                span_assoc(&st->cur, ev->query_id, 1);
                break;
            case PGWT_MARKER_EXEC_END:
                if (st->open)
                    st->cur.n_exec_ends++;
                break;
            case PGWT_MARKER_PLAN_START:
                if (!st->open)
                    span_open(st, ev->pid, 0, 0, leader_of(p, ev->pid));
                st->cur.n_plan_starts++;
                span_assoc(&st->cur, ev->query_id, 1);
                break;
            default:
                break;
            }
            continue;
        }

        /* A non-marker record with nothing open. Before the pid's first CMD
         * marker this is the left-edge / marker-less case and opens an
         * IMPLICIT span (start not observed). AFTER a CMD_END it is time
         * between commands — counted, never turned into a span, so an idle
         * backend does not manufacture one span per client round trip. */
        if (!st->open) {
            if (st->seen_cmd) {
                out->n_outside_records++;
                continue;
            }
            span_open(st, ev->pid, 0, 0, leader_of(p, ev->pid));
        }

        uint64_t t1 = ev->timestamp_ns;
        uint64_t t0 = (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
                    ? t1 : t1 - ev->duration_ns;
        if (st->cur.start_observed && t0 < st->cur.start_ns)
            t0 = st->cur.start_ns;   /* never observe before the start */
        if (!st->cur.has_evidence) {
            st->cur.has_evidence = 1;
            st->cur.first_evidence_ns = t0;
            st->cur.last_evidence_ns = t1;
        } else {
            if (t0 < st->cur.first_evidence_ns)
                st->cur.first_evidence_ns = t0;
            if (t1 > st->cur.last_evidence_ns)
                st->cur.last_evidence_ns = t1;
        }
        st->cur.n_records++;
        if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
            st->cur.sampled_inside = 1;
        if (ev->old_event != 0 && WE_CLASS(ev->old_event) == PG_WAIT_CLIENT)
            st->cur.client_pause_ns += ev->duration_ns;
        /* Association from ATTRIBUTED WAIT RECORDS, not only from
         * lifecycle markers: #455's backend has no EXEC_START at all, and
         * requiring one is exactly mutation 2. */
        if (!(ev->flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB))
            span_assoc(&st->cur, ev->query_id, 0);
    }

    /* End of stream: every still-open span is emitted with
     * stream_ended = 1, which is the ONLY thing that makes a span
     * IN_FLIGHT. No window boundary is written into end_ns. */
    for (int i = 0; i < c.pid_cap && !c.refused; i++) {
        if (!c.pid_ht[i].used || !c.pid_ht[i].open)
            continue;
        span_emit(&c, &c.pid_ht[i].cur, 1);
        c.pid_ht[i].open = 0;
    }

    free(c.pid_ht);
    free(c.esc);

    if (c.refused) {
        free(c.sel);
        free(c.qid_ht);
        int failed = 1;
        const char *why = out->refusal_reason;
        memset(out, 0, sizeof(*out));
        out->failed = failed;
        out->refusal_reason = why;
        return;
    }

    /* Compact the qid hash into a dense array so callers never iterate
     * empty slots and the order is deterministic (by query id). */
    if (c.qid_used > 0) {
        struct pgwt_span_qid_slot *dense =
            span_alloc_fail("span_qid_dense")
            ? NULL : calloc((size_t)c.qid_used, sizeof(*dense));
        if (!dense) {
            free(c.sel);
            free(c.qid_ht);
            memset(out, 0, sizeof(*out));
            out->failed = 1;
            out->refusal_reason = "the per-query span index could not be "
                                  "compacted";
            return;
        }
        int n = 0;
        for (int i = 0; i < c.qid_cap && n < c.qid_used; i++) {
            struct pgwt_span_qid_slot *s = &c.qid_ht[i];
            if (!s->used)
                continue;
            dense[n++] = *s;
        }
        for (int a = 1; a < n; a++) {   /* insertion sort by query id */
            struct pgwt_span_qid_slot tmp = dense[a];
            int b = a - 1;
            while (b >= 0 && dense[b].query_id > tmp.query_id) {
                dense[b + 1] = dense[b];
                b--;
            }
            dense[b + 1] = tmp;
        }
        out->qid_slots = dense;
        out->num_qid_slots = n;
    }
    free(c.qid_ht);

    out->selected = c.sel;
    out->num_selected = c.sel_n;
    out->truncated = c.sel_n < out->scoped_spans;
}

void pgwt_span_free(struct pgwt_span_result *out)
{
    if (!out)
        return;
    free(out->selected);
    free(out->qid_slots);
    out->selected = NULL;
    out->qid_slots = NULL;
    out->num_selected = 0;
    out->num_qid_slots = 0;
}

/* The span CONTAINING a timestamp, for the execution-detail panel: the
 * leader's activity span around one execution. Chosen from the bounded
 * selection, narrowest observed extent first, so a nested or malformed pair
 * cannot hand back an outer span when an inner one also contains `ts`.
 * Returns NULL when no selected span's OBSERVED extent contains `ts` --
 * never the closest one, because "the span around this execution" and "a
 * span near it" are different answers. */
const struct pgwt_backend_span *
pgwt_span_containing(const struct pgwt_span_result *r, uint32_t pid,
                     uint64_t ts)
{
    if (!r || r->failed || !r->selected)
        return NULL;
    const struct pgwt_backend_span *best = NULL;
    uint64_t best_w = 0;
    for (int i = 0; i < r->num_selected; i++) {
        const struct pgwt_backend_span *s = &r->selected[i];
        if (pid != 0 && s->pid != pid)
            continue;
        if (!s->start_observed && !s->has_evidence)
            continue;          /* no observed extent: it contains nothing */
        uint64_t lo = s->start_observed ? s->start_ns : s->first_evidence_ns;
        uint64_t hi = s->end_observed ? s->end_ns
                    : s->has_evidence ? s->last_evidence_ns : lo;
        if (ts < lo || ts > hi)
            continue;
        uint64_t w = hi - lo;
        if (!best || w < best_w) {
            best = s;
            best_w = w;
        }
    }
    return best;
}
