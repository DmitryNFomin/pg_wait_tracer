/* asan_probe.c — ASan/LSan over the #327 growth path only (scratch, not
 * committed). Builds a 4097-distinct-pattern index, queries it, frees
 * everything, and also exercises the growth-allocation refusal path. */
#include "variant_index.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pgwt_visit_summaries(const char *d, uint64_t a, uint64_t b, void *v, void *c);
int pgwt_visit_summaries(const char *d, uint64_t a, uint64_t b, void *v, void *c)
{ (void)d; (void)a; (void)b; (void)v; (void)c; return -1; }

#define MS   1000000ULL
#define BASE 1000000000000ULL
#define ES PGWT_MARKER_EXEC_START
#define EE PGWT_MARKER_EXEC_END

static struct pgwt_trace_event mk(uint64_t ts, uint32_t pid, uint32_t kind)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts; e.pid = pid; e.old_event = kind; e.new_event = kind;
    e.query_id = 7; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}
static struct pgwt_trace_event wt(uint64_t ts, uint32_t pid, uint32_t wei,
                                  uint64_t dur)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts; e.pid = pid; e.old_event = wei; e.new_event = 0;
    e.duration_ns = dur; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

int main(void)
{
    pgwt_init_event_names(18);
    const int np = 4097;
    struct pgwt_trace_event *ev = calloc((size_t)np * 4, sizeof(*ev));
    uint64_t t = BASE;
    int k = 0, made = 0;
    for (int a = 0; a < 65 && made < np; a++)
        for (int b = 0; b < 65 && made < np; b++) {
            if (a == b) continue;
            uint32_t ea = (PG_WAIT_IO << 24) | (uint32_t)(a + 1);
            uint32_t eb = (PG_WAIT_IO << 24) | (uint32_t)(b + 1);
            uint32_t pid = 3000 + (uint32_t)(made % 7);
            ev[k++] = mk(t, pid, ES); t += MS;
            ev[k++] = wt(t, pid, ea, 1000 + (uint64_t)made); t += MS;
            ev[k++] = wt(t, pid, eb, 2000 + (uint64_t)made); t += MS;
            ev[k++] = mk(t, pid, EE); t += MS;
            made++;
        }
    struct pgwt_variant_index idx;
    pgwt_variant_index_init(&idx);
    for (int bl = 0; bl < 8; bl++) {
        int s = (int)((long)k * bl / 8), e = (int)((long)k * (bl + 1) / 8);
        if (pgwt_variant_index_add_block(&idx, bl, ev[s].timestamp_ns,
                                         ev[e - 1].timestamp_ns, &ev[s],
                                         e - s) != 0) {
            printf("add_block failed\n"); return 1;
        }
    }
    if (pgwt_variant_index_seal(&idx) != 0) { printf("seal failed\n"); return 1; }

    struct pgwt_filter zero;
    memset(&zero, 0, sizeof(zero));
    struct pgwt_variant_index_query_result q;
    int rc = pgwt_variant_index_query(&idx, ev[0].timestamp_ns,
                                      ev[k - 1].timestamp_ns, &zero, 8192,
                                      PGWT_PHASE_EXEC, &q);
    printf("grow:   rc=%d nv=%d te=%d failed=%d si=%d refused=%s\n", rc,
           q.res.num_variants, q.res.total_executions, q.res.failed,
           q.sequences_indexed, pgwt_variant_index_refusal_str(q.refused));
    pgwt_variant_index_query_free(&q);

    setenv("PGWT_TEST_ALLOC_FAIL", "variant_index_ht_grow", 1);
    rc = pgwt_variant_index_query(&idx, ev[0].timestamp_ns,
                                  ev[k - 1].timestamp_ns, &zero, 8192,
                                  PGWT_PHASE_EXEC, &q);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    printf("refuse: rc=%d nv=%d te=%d failed=%d si=%d refused=%s\n", rc,
           q.res.num_variants, q.res.total_executions, q.res.failed,
           q.sequences_indexed, pgwt_variant_index_refusal_str(q.refused));
    pgwt_variant_index_query_free(&q);

    pgwt_variant_index_free(&idx);
    free(ev);
    return 0;
}
