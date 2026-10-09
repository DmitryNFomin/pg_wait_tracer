/* variant_pattern.h — the flow-pattern primitives `variants` is built on,
 * in ONE copy.
 *
 * WHY THIS FILE EXISTS. src/compute.c (pgwt_compute_variants, the oracle)
 * and src/variant_index.c (the step-sequence index that answers the same
 * question from committed blocks) must produce BIT-IDENTICAL patterns,
 * hashes, loop counts and per-step averages. They did it by holding two
 * verbatim copies of these five primitives, because compute.c was held by
 * other agents when the index was written. The copies were pinned against
 * each other by a 42,266-case differential; this is the fold that makes
 * them one copy, so the question of drift stops existing.
 *
 * WHAT THE FOLD COSTS, STATED PLAINLY. While there were two copies, a
 * differential between them caught a change in either. With one copy a
 * change moves BOTH sides at once and the differential is blind to it by
 * construction. What catches it instead is the ORACLE-INDEPENDENT literal
 * expectations in tests/test_variant_index.c — [A,B] and [B,A] are two
 * variants, [A] and [A,A] are two variants, a CPU-only execution is one
 * step with event id 0, ABAB compresses to two steps, the per-step averages
 * reproduce the oracle's si-clamping quirk (#343). That those still go RED
 * when a primitive here is mutated on both sides at once is not assumed: it
 * is demonstrated by tests/variant_pattern_shared_mutations.py, which
 * mutates this header (so compute.c and variant_index.c BOTH see the
 * mutation) and requires the test to fail each time.
 *
 * NOTHING HERE MAY BE "IMPROVED". Every function reproduces what
 * pgwt_compute_variants shipped, quirks included:
 *   - detect_loop accepts a loop at TWO repetitions, and scans body lengths
 *     ascending, so the shortest repeating body wins.
 *   - the per-step walk CLAMPS si to the last step (#343 makes loop-pattern
 *     step averages wrong because of it; the index reproduces it on
 *     purpose, and the fix lands in one commit on both sides).
 *   - loop_n is 0 for a single-record sequence and a DOUBLE division
 *     otherwise, because the caller sums it as a double and IEEE
 *     non-associativity makes the last bits order-dependent.
 *   - the CPU-only fill-in mutates len BEFORE loop_n is computed from it.
 * A change to any of it is a change to the answer `variants` gives, so it
 * bumps PGWT_VARIANT_INDEX_VERSION in the same commit.
 */
#ifndef PGWT_VARIANT_PATTERN_H
#define PGWT_VARIANT_PATTERN_H

#include "compute.h"            /* PGWT_MAX_VARIANT_STEPS, struct pgwt_variant */

#include <stdint.h>
#include <string.h>

/* raw_exec.events[] capacity: the number of wait records one execution can
 * contribute before the sequence is capped. */
#define PGWT_VARIANT_RAW_MAX 128

/* One raw execution: the events between an opening and closing marker. */
struct pgwt_variant_raw_exec {
    uint32_t events[PGWT_VARIANT_RAW_MAX];      /* event_ids in order */
    uint64_t durations[PGWT_VARIANT_RAW_MAX];   /* duration per event */
    int      len;
    uint64_t total_ns;                          /* sum of durations */
    uint64_t query_id;
};

/* The compressed pattern that gets hashed: adjacent repetitions collapsed
 * into a loop step. */
struct pgwt_variant_pattern {
    uint32_t steps[PGWT_MAX_VARIANT_STEPS];
    int      is_loop[PGWT_MAX_VARIANT_STEPS];
    int      loop_len[PGWT_MAX_VARIANT_STEPS];
    int      num_steps;
};

/* Longest repeating subsequence starting at pos; loop body length, 0 if
 * none. Body lengths ascend, so the SHORTEST repeating body wins, and two
 * repetitions are enough. */
static inline int pgwt_variant_detect_loop(const uint32_t *events, int len,
                                           int pos)
{
    for (int body = 1; body <= (len - pos) / 2; body++) {
        int reps = 1;
        int j = pos + body;
        while (j + body <= len) {
            int match = 1;
            for (int k = 0; k < body; k++) {
                if (events[pos + k] != events[j + k]) { match = 0; break; }
            }
            if (!match) break;
            reps++;
            j += body;
        }
        if (reps >= 2) return body;  /* found a loop with >=2 repetitions */
    }
    return 0;
}

/* Compress a raw event sequence: collapse loops. */
static inline void pgwt_variant_compress(const struct pgwt_variant_raw_exec *raw,
                                         struct pgwt_variant_pattern *out)
{
    out->num_steps = 0;
    int i = 0;
    while (i < raw->len && out->num_steps < PGWT_MAX_VARIANT_STEPS) {
        int body = pgwt_variant_detect_loop(raw->events, raw->len, i);
        if (body > 0 && out->num_steps + body < PGWT_MAX_VARIANT_STEPS) {
            /* Mark first step of loop */
            out->steps[out->num_steps] = raw->events[i];
            out->is_loop[out->num_steps] = 1;
            out->loop_len[out->num_steps] = body;
            out->num_steps++;
            /* Add remaining loop body steps */
            for (int k = 1; k < body && out->num_steps < PGWT_MAX_VARIANT_STEPS; k++) {
                out->steps[out->num_steps] = raw->events[i + k];
                out->is_loop[out->num_steps] = 0;
                out->loop_len[out->num_steps] = 0;
                out->num_steps++;
            }
            /* Skip past all repetitions */
            int reps = 0;
            int j = i;
            while (j + body <= raw->len) {
                int match = 1;
                for (int k = 0; k < body; k++) {
                    if (raw->events[i + k] != raw->events[j + k]) { match = 0; break; }
                }
                if (!match) break;
                reps++;
                j += body;
            }
            i = j;
        } else {
            out->steps[out->num_steps] = raw->events[i];
            out->is_loop[out->num_steps] = 0;
            out->loop_len[out->num_steps] = 0;
            out->num_steps++;
            i++;
        }
    }
}

/* Hash a compressed pattern. Step ORDER is folded in, which is why [A, B]
 * and [B, A] are two variants, and so is the loop flag, which is why [A]
 * and [A, A] are two variants despite an identical step list. */
static inline uint64_t
pgwt_variant_hash_pattern(const struct pgwt_variant_pattern *p)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < p->num_steps; i++) {
        h ^= p->steps[i];
        h *= 0x100000001b3ULL;
        if (p->is_loop[i]) {
            h ^= 0xDEADBEEF;
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

/* A CPU-only execution (no wait records) becomes ONE step with event id 0
 * carrying the execution's own total. Called BEFORE loop_n, which is
 * computed from len, so a pure-CPU execution has raw_len 1 and loop_n 0. */
static inline void
pgwt_variant_cpu_only_fill(struct pgwt_variant_raw_exec *re)
{
    if (re->len == 0) {
        re->events[0] = 0;  /* CPU */
        re->durations[0] = re->total_ns;
        re->len = 1;
    }
}

/* The average repetition count attributed to one execution: 0 for a
 * single-record sequence, raw_len / num_steps as a DOUBLE otherwise. The
 * caller sums these as doubles, so the type and the order both matter. */
static inline double pgwt_variant_loop_n(int raw_len, int num_steps)
{
    double loop_n = 0;
    if (raw_len > 1)
        loop_n = num_steps > 0 ? (double)raw_len / num_steps : 1;
    return loop_n;
}

/* Walk one execution's raw records against its compressed pattern and
 * report what each pattern step received: `total_ns_out[s]` and
 * `count_out[s]` for s < pattern->num_steps (both arrays must hold
 * PGWT_MAX_VARIANT_STEPS entries and are fully zeroed here).
 *
 * The si CLAMP is deliberate and wrong-on-purpose (#343): once the walk
 * reaches the last pattern step it stays there, so a loop pattern's trailing
 * records all land on one step. pgwt_compute_variants has always done this
 * and the index reproduces it; the fix changes both sides and
 * tests/test_variant_index.c in one commit. */
static inline void
pgwt_variant_step_walk(const struct pgwt_variant_raw_exec *re,
                       const struct pgwt_variant_pattern *cp,
                       uint64_t *total_ns_out, int *count_out)
{
    memset(total_ns_out, 0, sizeof(*total_ns_out) * PGWT_MAX_VARIANT_STEPS);
    memset(count_out, 0, sizeof(*count_out) * PGWT_MAX_VARIANT_STEPS);
    int si = 0;
    for (int j = 0; j < re->len && si < cp->num_steps; j++) {
        if (re->events[j] == cp->steps[si]) {
            total_ns_out[si] += re->durations[j];
            count_out[si]++;
            /* Advance through pattern steps (handle loops) */
            if (!cp->is_loop[si] || j + 1 >= re->len ||
                re->events[j + 1] != cp->steps[si])
                si++;
            if (si >= cp->num_steps) si = cp->num_steps - 1;
        }
    }
}

/* Variants are reported by descending total time. Returning 0 for equal
 * total_ns means qsort's tie order is unspecified by the standard — which
 * is why both sides share this comparator AND accumulate in the same close
 * order: one pre-sort permutation through one comparator is one result. */
static inline int pgwt_variant_cmp_time_desc(const void *a, const void *b)
{
    const struct pgwt_variant *va = a, *vb = b;
    if (vb->total_ns > va->total_ns) return 1;
    if (vb->total_ns < va->total_ns) return -1;
    return 0;
}

#endif /* PGWT_VARIANT_PATTERN_H */
