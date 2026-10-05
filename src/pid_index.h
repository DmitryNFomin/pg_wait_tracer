/* pid_index.h — open-addressing uint32 PID → int slot map (#275).
 *
 * Two execution-lifecycle scans used to keep per-PID state in a FIXED
 * 512-entry array with a linear probe, and dropped every PID past the
 * 512th SILENTLY (`if (pi < 0) continue;` in handle_top_queries,
 * `if (num_pids >= MAX_PIDS) continue;` in pgwt_compute_variants). Under
 * connection churn — no pooler, serverless, PHP-FPM, health checks, a
 * failover storm — the number of DISTINCT pids in a 15-minute window is
 * driven by turnover, not by concurrency, so 512 is reached routinely and
 * the reported exec counts and percentiles are quietly short.
 *
 * This map has NO bound: it doubles on load and only ever fails on a real
 * allocation failure, which the caller must propagate rather than skip.
 * There is deliberately no "drop it" branch to get wrong.
 *
 * Key 0 is a legal pid here (an `used` byte decides occupancy, not the key),
 * because the input is a trace file and not a guarantee.
 *
 * Grow-only: no deletion, hence no tombstones and no probe-chain repair to
 * get wrong either. Callers that want to bound memory decide WHICH pids get
 * a slot (e.g. only pids that actually open an execution); they never bound
 * HOW MANY.
 */
#ifndef PGWT_PID_INDEX_H
#define PGWT_PID_INDEX_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Allocation-failure injection, same shape (and same reasoning) as
 * src/server.c's test_load_alloc_failure: PGWT_TEST_ALLOC_FAIL=<point>
 * makes the named allocation fail once per call site. Production never
 * sets it. It exists because the refusal paths below are otherwise
 * unreachable, and an unreachable refusal is indistinguishable from one
 * that approves — which is the whole shape of #275. */
static inline int pgwt_test_alloc_fail(const char *point)
{
    const char *v = getenv("PGWT_TEST_ALLOC_FAIL");
    return v != NULL && strcmp(v, point) == 0;
}

struct pgwt_pid_index {
    uint32_t *keys;
    int      *vals;
    uint8_t  *used;
    int       cap;      /* power of two, or 0 when empty */
    int       n;
};

static inline void pgwt_pid_index_init(struct pgwt_pid_index *ix)
{
    memset(ix, 0, sizeof(*ix));
}

static inline void pgwt_pid_index_free(struct pgwt_pid_index *ix)
{
    free(ix->keys);
    free(ix->vals);
    free(ix->used);
    memset(ix, 0, sizeof(*ix));
}

static inline uint32_t pgwt_pid_index_hash(uint32_t pid)
{
    /* Fibonacci scramble: consecutive backend pids must not form one run. */
    return (uint32_t)((uint64_t)pid * 2654435761ULL);
}

/* Slot index of `pid`, or -1 if absent. */
static inline int pgwt_pid_index_find(const struct pgwt_pid_index *ix,
                                      uint32_t pid)
{
    if (ix->cap == 0)
        return -1;
    uint32_t mask = (uint32_t)ix->cap - 1;
    uint32_t h = pgwt_pid_index_hash(pid) & mask;
    while (ix->used[h]) {
        if (ix->keys[h] == pid)
            return ix->vals[h];
        h = (h + 1) & mask;
    }
    return -1;
}

/* Insert without growing. Caller guarantees spare capacity. */
static inline void pgwt_pid_index_put_nogrow(struct pgwt_pid_index *ix,
                                             uint32_t pid, int val)
{
    uint32_t mask = (uint32_t)ix->cap - 1;
    uint32_t h = pgwt_pid_index_hash(pid) & mask;
    while (ix->used[h]) {
        if (ix->keys[h] == pid) { ix->vals[h] = val; return; }
        h = (h + 1) & mask;
    }
    ix->used[h] = 1;
    ix->keys[h] = pid;
    ix->vals[h] = val;
    ix->n++;
}

/* Map `pid` → `val`, growing as needed. 0 on success, -1 on allocation
 * failure (the map is left untouched and usable). */
static inline int pgwt_pid_index_put(struct pgwt_pid_index *ix,
                                     uint32_t pid, int val)
{
    /* Grow at 70% load, and on the very first insert. */
    if (ix->cap == 0 || (ix->n + 1) * 10 >= ix->cap * 7) {
        if (pgwt_test_alloc_fail("pid_index_grow"))
            return -1;
        int newcap = ix->cap ? ix->cap * 2 : 128;
        uint32_t *nk = (uint32_t *)calloc((size_t)newcap, sizeof(*nk));
        int      *nv = (int *)calloc((size_t)newcap, sizeof(*nv));
        uint8_t  *nu = (uint8_t *)calloc((size_t)newcap, sizeof(*nu));
        if (!nk || !nv || !nu) {
            free(nk); free(nv); free(nu);
            return -1;
        }
        struct pgwt_pid_index old = *ix;
        ix->keys = nk; ix->vals = nv; ix->used = nu;
        ix->cap = newcap; ix->n = 0;
        for (int i = 0; i < old.cap; i++)
            if (old.used[i])
                pgwt_pid_index_put_nogrow(ix, old.keys[i], old.vals[i]);
        free(old.keys); free(old.vals); free(old.used);
    }
    pgwt_pid_index_put_nogrow(ix, pid, val);
    return 0;
}

#endif /* PGWT_PID_INDEX_H */
