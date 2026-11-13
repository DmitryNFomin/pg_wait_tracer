/* Measurement-only DATA residence under merge-v1 section 3. Refs #871. */
#ifndef NMI_TRANSPORT_STAGING_H
#define NMI_TRANSPORT_STAGING_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Exact nearest-rank percentiles. Samples are retained until reporting, after
 * the timed phase; allocation failure fails the run rather than dropping ages. */
struct transport_samples { uint64_t *values; size_t count, capacity; };
static inline bool transport_sample(struct transport_samples *s, uint64_t value)
{
    if (s->count == s->capacity) {
        size_t cap = s->capacity ? s->capacity * 2 : 256;
        if (cap < s->capacity || cap > SIZE_MAX / sizeof(*s->values)) return false;
        uint64_t *p = realloc(s->values, cap * sizeof(*p));
        if (!p) return false;
        s->values = p; s->capacity = cap;
    }
    s->values[s->count++] = value;
    return true;
}
static inline int transport_sample_compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static inline uint64_t transport_p99(struct transport_samples *s)
{
    if (!s->count) return 0;
    qsort(s->values, s->count, sizeof(*s->values), transport_sample_compare);
    /* ceil(99*n/100)-1 == n-floor(n/100)-1, without multiplication overflow. */
    return s->values[s->count - s->count / 100 - 1];
}
struct transport_held { uint64_t stamp, arrival; };
struct transport_queue {
    struct transport_held *records;
    size_t head, count, capacity;
};
struct transport_staging {
    struct transport_queue *queues;
    size_t sources;
    uint64_t arrived, released, depth, max_depth, max_held_age_ns;
    struct transport_samples held_ages;
};
static inline bool transport_stage(struct transport_staging *s, size_t source,
                                    uint64_t stamp, uint64_t arrival)
{
    struct transport_queue *q = &s->queues[source];
    if (q->count == q->capacity) {
        size_t cap = q->capacity ? q->capacity * 2 : 256;
        if (cap < q->capacity || cap > SIZE_MAX / sizeof(*q->records)) return false;
        struct transport_held *p = malloc(cap * sizeof(*p));
        if (!p) return false;
        for (size_t i = 0; i < q->count; ++i) p[i] = q->records[(q->head + i) % q->capacity];
        free(q->records); q->records = p; q->head = 0; q->capacity = cap;
    }
    q->records[(q->head + q->count++) % q->capacity] = (struct transport_held){stamp, arrival};
    ++s->arrived; ++s->depth;
    if (s->depth > s->max_depth) s->max_depth = s->depth;
    return true;
}
/* Evaluate after each input. Strict t<W: equality remains held. */
static inline bool transport_release(struct transport_staging *s, uint64_t w, uint64_t now)
{
    for (size_t i = 0; i < s->sources; ++i) {
        struct transport_queue *q = &s->queues[i];
        while (q->count && q->records[q->head].stamp < w) {
            uint64_t age = now - q->records[q->head].arrival;
            if (!transport_sample(&s->held_ages, age)) return false;
            if (age > s->max_held_age_ns) s->max_held_age_ns = age;
            q->head = (q->head + 1) % q->capacity; --q->count;
            --s->depth; ++s->released;
        }
    }
    return true;
}
static inline uint64_t transport_oldest_age(const struct transport_staging *s, uint64_t now)
{
    uint64_t age = 0;
    for (size_t i = 0; i < s->sources; ++i) {
        const struct transport_queue *q = &s->queues[i];
        if (q->count && now - q->records[q->head].arrival > age)
            age = now - q->records[q->head].arrival;
    }
    return age;
}
static inline bool transport_request_due(bool outstanding, uint64_t frontier,
                                          uint64_t minimum, uint64_t depth,
                                          uint64_t age, uint64_t after,
                                          uint64_t now, uint64_t coverage)
{
    if (outstanding) return false;
    if (depth) return frontier == minimum && age >= after;
    return coverage && (now >= frontier && now - frontier >= coverage);
}
static inline void transport_staging_free(struct transport_staging *s)
{
    if (s->queues) for (size_t i = 0; i < s->sources; ++i) free(s->queues[i].records);
    free(s->queues);
    free(s->held_ages.values);
}
#endif
