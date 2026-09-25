/* query_attr.h — deferred per-query attribution of a pid's closed intervals
 * (issue #128). Header-only, pure: no daemon, no BPF, no allocation.
 *
 * THE DEFECT. Every trace record carries the query_id BPF resolved at the
 * instant the interval CLOSED. PostgreSQL reports a statement's id
 * (pgstat_report_query_id, or standard_ExecutorStart on PG13) only after
 * parse analysis — and parse analysis is where relation locks are taken
 * (parserOpenTable → LockRelationOid). A backend that blocks on
 * Lock:relation during parse therefore closes that wait with query_id 0:
 * the wait reached the system/pid rows, but every per-query consumer
 * (live query_event, server top_queries / the query drill, the summary
 * writer) dropped it silently. Exactly the wait a user drills into a
 * query to see was the one it never showed.
 *
 * THE RULE (one state machine, shared by every consumer and mirrored in
 * compute.c pgwt_tag_events for the server's raw path):
 *
 *   A foreground, non-idle interval that closed with query_id 0 belongs to
 *     1. the first query id REPORTED LATER in the same command (a later
 *        record or marker of the pid carrying a nonzero id, before any
 *        command boundary) — the parse-phase case above;
 *     2. else the last id reported EARLIER in the same command (cmd_qid) —
 *        trailing records of a command whose id edge PG13 resets before
 *        the run closes;
 *     3. else the explicit UNATTRIBUTED bucket (query_id 0 row) — visible,
 *        counted, never dropped.
 *
 *   Boundaries: CMD_START (new command: flush pending by rule 2/3 and
 *   forget cmd_qid), CMD_END and an idle record (flush by rule 2/3, keep
 *   cmd_qid — the run that straddles CMD_END still closes after it). The
 *   sampled tier has no markers: a sample taken outside a command
 *   (cmd_open == 0) is its CMD_START-equivalent.
 *
 * Live consumers defer (pgwt_qattr_defer) and resolve on the pid's next
 * observe/boundary; the server sees the whole array and back-fills in one
 * backward pass (compute.c). Both produce the same attribution for the
 * same stream — tests/test_live_accum.c diffs them. Pending state is
 * bounded (PGWT_QATTR_PENDING_MAX distinct wait events per pid); an
 * overflow spills to the unattributed bucket and is counted, never lost. */
#ifndef PGWT_QUERY_ATTR_H
#define PGWT_QUERY_ATTR_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PGWT_QATTR_PENDING_MAX 16

struct pgwt_qattr_pending {
    uint32_t we;
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};

/* Per-pid resolver state. Zero-initialised = fresh. */
struct pgwt_qattr_pid {
    uint64_t cmd_qid;     /* last nonzero id observed since the command opened */
    int      npending;
    struct pgwt_qattr_pending pending[PGWT_QATTR_PENDING_MAX];
};

/* Consumer callback: fold one resolved (query_id, wait_event) aggregate.
 * query_id == 0 is the unattributed bucket. `backfilled` is true when the
 * aggregate was deferred and is now attributed to a nonzero id (rule 1/2)
 * — consumers count it so a scrape tells how much attribution was late. */
typedef void (*pgwt_qattr_emit_fn)(void *ctx, uint64_t query_id, uint32_t we,
                                   const struct pgwt_qattr_pending *p,
                                   bool backfilled);

static inline void pgwt_qattr_flush(struct pgwt_qattr_pid *q, uint64_t qid,
                                    pgwt_qattr_emit_fn emit, void *ctx)
{
    for (int i = 0; i < q->npending; i++)
        emit(ctx, qid, q->pending[i].we, &q->pending[i], qid != 0);
    q->npending = 0;
}

/* Defer one closed foreground non-idle interval that carries no id.
 * Returns 1 if the pid's pending list was full and spilled to the
 * unattributed bucket first (the caller counts it). */
static inline int pgwt_qattr_defer(struct pgwt_qattr_pid *q, uint32_t we,
                                   uint64_t ns, pgwt_qattr_emit_fn emit,
                                   void *ctx)
{
    int spilled = 0;
    struct pgwt_qattr_pending *slot = NULL;
    for (int i = 0; i < q->npending; i++) {
        if (q->pending[i].we == we) {
            slot = &q->pending[i];
            break;
        }
    }
    if (!slot) {
        if (q->npending >= PGWT_QATTR_PENDING_MAX) {
            /* Never silent, never lost: EVERYTHING pending (all
             * PGWT_QATTR_PENDING_MAX aggregates) spills to the unattributed
             * bucket and this interval starts a fresh list; the caller
             * bumps its overflow metric. */
            pgwt_qattr_flush(q, 0, emit, ctx);
            spilled = 1;
        }
        slot = &q->pending[q->npending++];
        slot->we = we;
        slot->count = 0;
        slot->total_ns = 0;
        slot->min_ns = UINT64_MAX;
        slot->max_ns = 0;
    }
    slot->count++;
    slot->total_ns += ns;
    if (ns < slot->min_ns) slot->min_ns = ns;
    if (ns > slot->max_ns) slot->max_ns = ns;
    return spilled;
}

/* A nonzero query id was observed for the pid inside the current command
 * (a record or a non-CMD_START marker carrying it): rule 1 for everything
 * pending, and it becomes cmd_qid for rule 2. */
static inline void pgwt_qattr_observe(struct pgwt_qattr_pid *q, uint64_t qid,
                                      pgwt_qattr_emit_fn emit, void *ctx)
{
    if (qid == 0)
        return;
    pgwt_qattr_flush(q, qid, emit, ctx);
    q->cmd_qid = qid;
}

/* A command boundary: pending resolves by rule 2 (cmd_qid) or 3
 * (unattributed). `new_command` (CMD_START, or a sample outside any
 * command) also forgets cmd_qid so the next command never inherits it. */
static inline void pgwt_qattr_boundary(struct pgwt_qattr_pid *q,
                                       bool new_command,
                                       pgwt_qattr_emit_fn emit, void *ctx)
{
    pgwt_qattr_flush(q, q->cmd_qid, emit, ctx);
    if (new_command)
        q->cmd_qid = 0;
}

/* Immediate resolution for an interval that cannot wait (the live open
 * stretch shown at tick time, or a sample taken outside a command): rule 2
 * or 3 now. */
static inline uint64_t pgwt_qattr_resolve_now(const struct pgwt_qattr_pid *q)
{
    return q ? q->cmd_qid : 0;
}

#endif /* PGWT_QUERY_ATTR_H */
