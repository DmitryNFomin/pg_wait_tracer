/* qid_index.h — open-addressing uint64 key → int slot map (#327).
 *
 * The uint64-keyed twin of pid_index.h, with the same contract and for the
 * same reason. Five tables in src/compute.c keyed a FIXED power-of-two array
 * on a 64-bit id (query_id, or a flow-pattern hash) and probed it with
 *
 *     while (ht[h].<accumulator> > 0 && ht[h].key != k) h = (h + 1) & MASK;
 *
 * which has no fullness test: once the table holds <size> distinct keys that
 * loop never terminates. pgwt-server is single-threaded, so the request never
 * answers, the connection never closes, and every tab stops at once — not a
 * wrong number, a hang.
 *
 * Two properties of this map are load-bearing, both learned from #275/#276:
 *
 *  - NO BOUND. It doubles on load and only ever fails on a real allocation
 *    failure, which the caller must propagate (`failed`) rather than skip.
 *    There is deliberately no "drop it" branch to get wrong: a silently
 *    short total is worse than a refusal, because nobody notices a number
 *    that is quietly short.
 *  - OCCUPANCY IS A `used` BYTE, never "an accumulator is non-zero". That
 *    second form is what #347 is: a slot claimed by a zero-duration record
 *    reads as free, so the next key that probes to it RE-KEYS the slot and
 *    inherits whatever the first key had already accumulated into the
 *    non-zero-tested fields. Measured on the parent tree: fingerprints
 *    reported one query id with 3 transitions where two ids had 1 and 2.
 *    Key 0 is therefore a legal key here — the input is a trace file, not a
 *    guarantee.
 *
 * Grow-only: no deletion, hence no tombstones and no probe-chain repair to
 * get wrong either.
 */
#ifndef PGWT_QID_INDEX_H
#define PGWT_QID_INDEX_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Allocation-failure injection: PGWT_TEST_ALLOC_FAIL=qid_index_grow makes
 * the growth below fail once per call site. Production never sets it. It
 * exists because the refusal path is otherwise unreachable, and an
 * unreachable refusal is indistinguishable from one that approves. */
#include "test_alloc_fail.h"

struct pgwt_qid_index {
    uint64_t *keys;
    int      *vals;
    uint8_t  *used;
    int       cap;      /* power of two, or 0 when empty */
    int       n;
};

static inline void pgwt_qid_index_init(struct pgwt_qid_index *ix)
{
    memset(ix, 0, sizeof(*ix));
}

static inline void pgwt_qid_index_free(struct pgwt_qid_index *ix)
{
    free(ix->keys);
    free(ix->vals);
    free(ix->used);
    memset(ix, 0, sizeof(*ix));
}

/* splitmix64 finalizer — the same mixer compute.c's hash64() uses, so a run
 * of consecutive query ids does not form one probe chain. */
static inline uint64_t pgwt_qid_index_hash(uint64_t key)
{
    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    return key ^ (key >> 31);
}

/* Slot value for `key`, or -1 if absent. */
static inline int pgwt_qid_index_find(const struct pgwt_qid_index *ix,
                                      uint64_t key)
{
    if (ix->cap == 0)
        return -1;
    uint32_t mask = (uint32_t)ix->cap - 1;
    uint32_t h = (uint32_t)(pgwt_qid_index_hash(key) & mask);
    while (ix->used[h]) {
        if (ix->keys[h] == key)
            return ix->vals[h];
        h = (h + 1) & mask;
    }
    return -1;
}

/* Insert without growing. Caller guarantees spare capacity. */
static inline void pgwt_qid_index_put_nogrow(struct pgwt_qid_index *ix,
                                             uint64_t key, int val)
{
    uint32_t mask = (uint32_t)ix->cap - 1;
    uint32_t h = (uint32_t)(pgwt_qid_index_hash(key) & mask);
    while (ix->used[h]) {
        if (ix->keys[h] == key) { ix->vals[h] = val; return; }
        h = (h + 1) & mask;
    }
    ix->used[h] = 1;
    ix->keys[h] = key;
    ix->vals[h] = val;
    ix->n++;
}

/* Map `key` → `val`, growing as needed. 0 on success, -1 on allocation
 * failure (the map is left untouched and usable). */
static inline int pgwt_qid_index_put(struct pgwt_qid_index *ix,
                                     uint64_t key, int val)
{
    /* Grow at 70% load, and on the very first insert. */
    if (ix->cap == 0 || (ix->n + 1) * 10 >= ix->cap * 7) {
        if (pgwt_test_alloc_fail("qid_index_grow"))
            return -1;
        if (ix->cap > (1 << 29))
            return -1;                 /* refuse rather than overflow cap */
        int newcap = ix->cap ? ix->cap * 2 : 128;
        uint64_t *nk = (uint64_t *)calloc((size_t)newcap, sizeof(*nk));
        int      *nv = (int *)calloc((size_t)newcap, sizeof(*nv));
        uint8_t  *nu = (uint8_t *)calloc((size_t)newcap, sizeof(*nu));
        if (!nk || !nv || !nu) {
            free(nk); free(nv); free(nu);
            return -1;
        }
        struct pgwt_qid_index old = *ix;
        ix->keys = nk; ix->vals = nv; ix->used = nu;
        ix->cap = newcap; ix->n = 0;
        for (int i = 0; i < old.cap; i++)
            if (old.used[i])
                pgwt_qid_index_put_nogrow(ix, old.keys[i], old.vals[i]);
        free(old.keys); free(old.vals); free(old.used);
    }
    pgwt_qid_index_put_nogrow(ix, key, val);
    return 0;
}

#endif /* PGWT_QID_INDEX_H */
