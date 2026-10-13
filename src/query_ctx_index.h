/* query_ctx_index.h — one pass over the window's events, one slot per
 * query_id, carrying everything the Queries tab needs to know about a row's
 * CONTEXT (databaseid / userid / "ambiguous") and whether any of its events
 * was a PG13 synthetic-attribution seed.
 *
 * WHY THIS EXISTS. src/server.c's handle_top_queries used to resolve both
 * facts INSIDE the row loop: query_context_for_events() scanned all events
 * for every row, and a second inline loop scanned them again looking for
 * PGWT_EVENT_FLAG_QUERY_SYNTH. At the UI's default 900 s window that is
 * 26 rows x 2 full scans x 5.7M events (~274 MB per scan, ~14 GB of memory
 * traffic), measured at 1778 ms — about 40% of the whole top_queries
 * response. Both facts are per-query_id properties of the SAME array, so
 * they are computed once here and each row becomes a lookup.
 *
 * THE SAFETY ARGUMENT IS "BIT-IDENTICAL", and it rests on the resolution
 * rule being order-independent. query_context_for_events returned -1 at the
 * FIRST event whose backend metadata was missing/ambiguous/incomplete, and
 * otherwise kept the FIRST contributor's ids, refusing on any later
 * disagreement. So its result is a pure function of the contributing SET:
 *
 *   refused (-1)  iff  any contributing event's pid has no usable metadata,
 *                      OR two contributing events disagree;
 *   accepted (1)  with the common (databaseid, userid) — which, when
 *                      accepted, every contributor shares, so "the first"
 *                      and "any" are the same value;
 *   absent (0)    when no event carries that query_id (ids reported after
 *                      the interval closed, and the summary path's rows).
 *
 * That makes the accumulate-as-you-go form below identical to the per-row
 * scan, in any event order. tests/test_top_queries_context.c is the
 * differential that proves it against a verbatim copy of the deleted
 * function, including reversed and rotated orders.
 *
 * REFUSAL, NOT ABSENCE. Growth failure sets `failed` and the caller must
 * fail the whole response (handle_top_queries folds it into the same
 * `res.failed || lcres.failed` arm as #327). An empty table and a table that
 * could not be built are NOT the same thing: the first legitimately reports
 * every row as context-absent, the second would report plausible-looking
 * rows with no context and no complaint.
 *
 * The original's doc comment, kept because it is the rule being implemented:
 *   Resolve the context represented by a qid row. top_queries aggregates by
 *   qid for on-disk compatibility, so a row is context-addressable only when
 *   every contributing PID has metadata and agrees. Missing metadata is
 *   ambiguous: accepting the one known contributor could expose its text on
 *   a mixed row.
 */
#ifndef PGWT_QUERY_CTX_INDEX_H
#define PGWT_QUERY_CTX_INDEX_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pg_wait_tracer.h"
#include "qid_index.h"
#include "test_alloc_fail.h"

/* Per-query_id resolution. `state` uses query_context_for_events' return
 * values verbatim: 1 resolved, 0 nothing contributed, -1 ambiguous. */
struct pgwt_query_ctx {
    uint64_t query_id;
    uint32_t databaseid;
    uint32_t userid;
    int      state;        /* -1 ambiguous, 0 no contributor, 1 resolved */
    uint8_t  has_synth;    /* any contributing event had QUERY_SYNTH */
};

struct pgwt_query_ctx_index {
    struct pgwt_qid_index  by_qid;   /* query_id -> slot in `slots` */
    struct pgwt_query_ctx *slots;
    int  n;
    int  cap;
    int  failed;                     /* allocation failure: refuse the response */
};

/* Backend-metadata resolver. Returns 1 and fills the out-parameters when
 * `pid` has usable, unambiguous metadata; 0 otherwise (which is the
 * refusal trigger). src/server.c passes the bm_map lookup; the unit test
 * passes a stub. Must be a pure function of `pid` for the duration of one
 * build — nothing refreshes backend metadata inside handle_top_queries'
 * event pass, which is also what the per-row scans already relied on. */
typedef int (*pgwt_bm_resolver)(void *ctx, uint32_t pid,
                                uint32_t *databaseid, uint32_t *userid);

static inline void pgwt_query_ctx_index_init(struct pgwt_query_ctx_index *qc)
{
    memset(qc, 0, sizeof(*qc));
    pgwt_qid_index_init(&qc->by_qid);
}

static inline void pgwt_query_ctx_index_free(struct pgwt_query_ctx_index *qc)
{
    pgwt_qid_index_free(&qc->by_qid);
    free(qc->slots);
    qc->slots = NULL;
    qc->n = qc->cap = 0;
    qc->failed = 0;
}

/* Slot for `query_id`, or NULL when nothing in the window carried it.
 * NULL is the `state == 0` case: absent, not refused. */
static inline const struct pgwt_query_ctx *pgwt_query_ctx_lookup(
    const struct pgwt_query_ctx_index *qc, uint64_t query_id)
{
    int slot = pgwt_qid_index_find(&qc->by_qid, query_id);
    if (slot < 0 || slot >= qc->n)
        return NULL;
    return &qc->slots[slot];
}

/* Grow-or-find. Returns the slot index, or -1 on allocation failure. */
static inline int pgwt_query_ctx_slot(struct pgwt_query_ctx_index *qc,
                                      uint64_t query_id)
{
    int slot = pgwt_qid_index_find(&qc->by_qid, query_id);
    if (slot >= 0)
        return slot;
    if (qc->n == qc->cap) {
        if (pgwt_test_alloc_fail("query_ctx_slots_grow"))
            return -1;
        if (qc->cap > (1 << 29))
            return -1;              /* refuse rather than overflow */
        int newcap = qc->cap ? qc->cap * 2 : 64;
        struct pgwt_query_ctx *ns = (struct pgwt_query_ctx *)
            realloc(qc->slots, (size_t)newcap * sizeof(*ns));
        if (!ns)
            return -1;
        /* realloc does not zero (#271), and `n` only ever grows, so every
         * slot this function hands out lies in the tail being zeroed here.
         * This memset is the ONLY initialisation of a new slot — there is
         * deliberately no second, per-field one next to the `query_id`
         * assignment below: a redundant guard is indistinguishable from a
         * working one, and the duplicate made the real guard untestable
         * (its mutation stayed green). */
        memset(ns + qc->cap, 0,
               (size_t)(newcap - qc->cap) * sizeof(*ns));
        qc->slots = ns;
        qc->cap = newcap;
    }
    slot = qc->n;
    qc->slots[slot].query_id = query_id;
    if (pgwt_qid_index_put(&qc->by_qid, query_id, slot) != 0)
        return -1;
    qc->n = slot + 1;
    return slot;
}

/* One pass over [events, events+count). 0 on success; -1 on allocation
 * failure, with `failed` set — the caller must refuse the response rather
 * than serve a partially built table, whose missing rows are
 * indistinguishable from legitimately context-less ones.
 *
 * `resolve` must be non-NULL: a NULL resolver would make every pid look
 * unusable and silently turn every row ambiguous, so it is refused. */
static inline int pgwt_query_ctx_index_build(struct pgwt_query_ctx_index *qc,
                                             const struct pgwt_trace_event *events,
                                             int count,
                                             pgwt_bm_resolver resolve,
                                             void *rctx)
{
    pgwt_query_ctx_index_init(qc);
    if (!resolve) {
        qc->failed = 1;
        return -1;
    }
    if (!events || count <= 0)
        return 0;

    /* Memo for the previous event's pid. `resolve` is pure over one build,
     * so this changes nothing but the number of hash probes; wait events
     * from one backend arrive in runs, which is where it pays. */
    uint32_t memo_pid = 0;
    int      memo_ok = -1;           /* -1 = cold */
    uint32_t memo_db = 0, memo_uid = 0;
    /* Memo for the previous event's query_id -> slot. */
    uint64_t memo_qid = 0;
    int      memo_slot = -1;

    for (int i = 0; i < count; i++) {
        uint64_t qid = events[i].query_id;
        int slot;
        if (memo_slot >= 0 && memo_qid == qid) {
            slot = memo_slot;
        } else {
            slot = pgwt_query_ctx_slot(qc, qid);
            if (slot < 0) {
                qc->failed = 1;
                return -1;
            }
            memo_qid = qid;
            memo_slot = slot;
        }
        struct pgwt_query_ctx *e = &qc->slots[slot];

        if (events[i].flags & PGWT_EVENT_FLAG_QUERY_SYNTH)
            e->has_synth = 1;

        /* Once ambiguous, always ambiguous: the per-row scan returned on
         * the spot, so no later event could change the verdict. */
        if (e->state < 0)
            continue;

        uint32_t pid = events[i].pid;
        if (memo_ok < 0 || memo_pid != pid) {
            uint32_t db = 0, uid = 0;
            memo_ok = resolve(rctx, pid, &db, &uid) ? 1 : 0;
            memo_pid = pid;
            memo_db = memo_ok ? db : 0;
            memo_uid = memo_ok ? uid : 0;
        }
        if (!memo_ok) {
            e->state = -1;
            e->databaseid = 0;
            e->userid = 0;
        } else if (e->state == 0) {
            e->state = 1;
            e->databaseid = memo_db;
            e->userid = memo_uid;
        } else if (e->databaseid != memo_db || e->userid != memo_uid) {
            e->state = -1;
            e->databaseid = 0;
            e->userid = 0;
        }
    }
    return 0;
}

#endif /* PGWT_QUERY_CTX_INDEX_H */
