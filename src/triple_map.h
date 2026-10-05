/* triple_map.h — open-addressing (uint32,uint32,uint32) → int map (#276).
 *
 * WHY. `pgwt_compute_concurrency` counted DISTINCT pids per (bucket, wait
 * event) in fixed arrays — `ev_pids[64]` events per bucket, `pids[128]` pids
 * per event per bucket — and materialised candidate intervals into a fixed
 * 100,000-entry array filled in ARRIVAL order, so at the UI's default 900 s
 * window the panel examined the first ~60 s and reported the remaining 825 s
 * as `max: 0`, indistinguishable from an idle database (#276).
 *
 * Distinct counting cannot be done without remembering what has been seen,
 * so the fix needs a set, not a bigger array. This map has NO bound: it
 * doubles on load and fails only on a real allocation failure, which the
 * caller must propagate (`pgwt_concurrency_result.failed`) rather than
 * skip. There is deliberately no "drop it" branch to get wrong — the same
 * reasoning as src/pid_index.h (#275), one key wider.
 *
 * The memory this bounds by is the number of DISTINCT keys, which for
 * concurrency is (buckets × distinct events × distinct pids) — a function of
 * the chart's resolution and the workload's shape, never of the event count.
 * A 10-million-event window with 20 backends and 39 wait events needs ~47k
 * slots at 60 buckets whether it holds ten minutes or ten hours.
 *
 * Key (0,0,0) is legal (an `used` byte decides occupancy, not the key),
 * because bucket 0 / pid 0 are both real inputs and the trace file is not a
 * guarantee. Grow-only: no deletion, hence no tombstones and no probe-chain
 * repair to get wrong. `clear()` keeps the allocation and forgets the
 * contents, for callers that reuse one map across many small groups.
 */
#ifndef PGWT_TRIPLE_MAP_H
#define PGWT_TRIPLE_MAP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_alloc_fail.h"

struct pgwt_triple_map {
    uint32_t *k0;
    uint32_t *k1;
    uint32_t *k2;
    int      *vals;
    uint8_t  *used;
    int       cap;      /* power of two, or 0 when empty */
    int       n;
};

static inline void pgwt_triple_map_init(struct pgwt_triple_map *m)
{
    memset(m, 0, sizeof(*m));
}

static inline void pgwt_triple_map_free(struct pgwt_triple_map *m)
{
    free(m->k0); free(m->k1); free(m->k2); free(m->vals); free(m->used);
    memset(m, 0, sizeof(*m));
}

/* Forget every key, keep the allocation. */
static inline void pgwt_triple_map_clear(struct pgwt_triple_map *m)
{
    if (m->cap > 0)
        memset(m->used, 0, (size_t)m->cap);
    m->n = 0;
}

static inline uint32_t pgwt_triple_map_hash(uint32_t a, uint32_t b, uint32_t c)
{
    /* Mix all three words: consecutive backend pids, consecutive bucket
     * indices and dense wait-event ids must not form one probe run. */
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    h = (h ^ a) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ b) * 0x94D049BB133111EBULL;
    h = (h ^ c) * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 31;   /* the mask takes LOW bits, so fold the high ones down */
    return (uint32_t)h;
}

static inline void pgwt_triple_map_put_nogrow(struct pgwt_triple_map *m,
                                              uint32_t a, uint32_t b, uint32_t c,
                                              int val)
{
    uint32_t mask = (uint32_t)m->cap - 1;
    uint32_t h = pgwt_triple_map_hash(a, b, c) & mask;
    while (m->used[h]) {
        if (m->k0[h] == a && m->k1[h] == b && m->k2[h] == c) {
            m->vals[h] = val;
            return;
        }
        h = (h + 1) & mask;
    }
    m->used[h] = 1;
    m->k0[h] = a; m->k1[h] = b; m->k2[h] = c;
    m->vals[h] = val;
    m->n++;
}

/* Grow to `newcap` slots, rehashing. 0 on success, -1 on allocation failure
 * (the map is left untouched and usable). */
static inline int pgwt_triple_map_grow(struct pgwt_triple_map *m, int newcap)
{
    if (pgwt_test_alloc_fail("triple_map_grow"))
        return -1;
    uint32_t *n0 = (uint32_t *)calloc((size_t)newcap, sizeof(*n0));
    uint32_t *n1 = (uint32_t *)calloc((size_t)newcap, sizeof(*n1));
    uint32_t *n2 = (uint32_t *)calloc((size_t)newcap, sizeof(*n2));
    int      *nv = (int *)calloc((size_t)newcap, sizeof(*nv));
    uint8_t  *nu = (uint8_t *)calloc((size_t)newcap, sizeof(*nu));
    if (!n0 || !n1 || !n2 || !nv || !nu) {
        free(n0); free(n1); free(n2); free(nv); free(nu);
        return -1;
    }
    struct pgwt_triple_map old = *m;
    m->k0 = n0; m->k1 = n1; m->k2 = n2; m->vals = nv; m->used = nu;
    m->cap = newcap; m->n = 0;
    for (int i = 0; i < old.cap; i++)
        if (old.used[i])
            pgwt_triple_map_put_nogrow(m, old.k0[i], old.k1[i], old.k2[i],
                                       old.vals[i]);
    free(old.k0); free(old.k1); free(old.k2); free(old.vals); free(old.used);
    return 0;
}

/* Pointer to the value for (a,b,c), inserting it with value 0 if absent.
 * *created is set to 1 when the key was inserted by this call, 0 when it was
 * already present — that flag is how callers count DISTINCT keys without a
 * second lookup. Returns NULL on allocation failure, and ONLY then; a caller
 * must propagate that as an absent answer, never as a short one.
 *
 * The returned pointer is invalidated by the next call that grows the map. */
static inline int *pgwt_triple_map_slot(struct pgwt_triple_map *m,
                                        uint32_t a, uint32_t b, uint32_t c,
                                        int *created)
{
    *created = 0;
    /* Grow at 70% load, and on the very first insert. */
    if (m->cap == 0 || (m->n + 1) * 10 >= m->cap * 7) {
        /* Only a MISS needs capacity; a hit on a full-ish map must not fail. */
        if (m->cap > 0) {
            uint32_t mask = (uint32_t)m->cap - 1;
            uint32_t h = pgwt_triple_map_hash(a, b, c) & mask;
            while (m->used[h]) {
                if (m->k0[h] == a && m->k1[h] == b && m->k2[h] == c)
                    return &m->vals[h];
                h = (h + 1) & mask;
            }
        }
        if (pgwt_triple_map_grow(m, m->cap ? m->cap * 2 : 256) != 0)
            return NULL;
    }
    uint32_t mask = (uint32_t)m->cap - 1;
    uint32_t h = pgwt_triple_map_hash(a, b, c) & mask;
    while (m->used[h]) {
        if (m->k0[h] == a && m->k1[h] == b && m->k2[h] == c)
            return &m->vals[h];
        h = (h + 1) & mask;
    }
    m->used[h] = 1;
    m->k0[h] = a; m->k1[h] = b; m->k2[h] = c;
    m->vals[h] = 0;
    m->n++;
    *created = 1;
    return &m->vals[h];
}

#endif /* PGWT_TRIPLE_MAP_H */
