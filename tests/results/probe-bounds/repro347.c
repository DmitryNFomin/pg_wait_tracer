/* repro347.c — does the #347 re-keying defect exist at compute.c:638 (AAS),
 * and does it exist at the OTHER tables that use the same
 * "occupancy == accumulator > 0" predicate?
 *
 * Run against the PARENT tree (no fix applied).
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pgwt_visit_summaries(const char *d, uint64_t a, uint64_t b,
                         pgwt_summary_visitor v, void *c)
{ (void)d; (void)a; (void)b; (void)v; (void)c; return -1; }

/* compute.c's private hash, copied so the fixture can pick colliding keys. */
static int hash64_copy(uint64_t key, int mask)
{
    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    key = key ^ (key >> 31);
    return (int)(key & mask);
}

#define FP_MASK 2047

static struct pgwt_trace_event *ev;
static int n;

static void push(uint32_t pid, uint32_t old_e, uint32_t new_e, uint64_t ts,
                 uint64_t dur, uint64_t qid)
{
    struct pgwt_trace_event *e = &ev[n++];
    memset(e, 0, sizeof(*e));
    e->pid = pid; e->old_event = old_e; e->new_event = new_e;
    e->timestamp_ns = ts; e->duration_ns = dur; e->query_id = qid;
    e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
}

#define WAIT_A 0x01000001u
#define WAIT_B 0x01000002u

int main(void)
{
    ev = calloc(64, sizeof(*ev));
    struct pgwt_filter nf;
    memset(&nf, 0, sizeof(nf));

    /* ── 1. AAS per-event table (compute.c:638), 512 slots, key = id & 511 */
    uint32_t idA = 0x01000001u;          /* & 511 == 1 */
    uint32_t idB = 0x01000201u;          /* & 511 == 1 as well */
    printf("AAS home slots: A=%u B=%u\n", idA & 511u, idB & 511u);

    n = 0;
    /* A: zero duration, claims the slot. B: real duration, same home slot. */
    push(100, idA, idB, 2000000000ULL, 0, 0);
    push(100, idB, idA, 3000000000ULL, 1000000ULL, 0);
    push(100, idA, idB, 4000000000ULL, 2000000ULL, 0);
    struct pgwt_aas_result aas;
    pgwt_compute_aas(ev, n, &nf, 1000000000ULL, 5000000000ULL, 4, 1, 10, &aas);
    printf("AAS series: %d\n", aas.num_event_series);
    for (int i = 0; i < aas.num_event_series; i++)
        printf("  id=0x%08x total_aas=%.9f\n",
               aas.event_series[i].event_id, aas.event_series[i].total_aas);
    free(aas.event_aas);

    /* ── 2. Fingerprints (compute.c:3006), 2048 slots, hash64(query_id) */
    uint64_t qA = 0, qB = 0;
    for (uint64_t k = 1; k < 2000000 && (!qA || !qB); k++) {
        if (!qA) { qA = k; continue; }
        if (hash64_copy(k, FP_MASK) == hash64_copy(qA, FP_MASK)) qB = k;
    }
    printf("\nFP colliding ids: qA=%llu qB=%llu slot=%d\n",
           (unsigned long long)qA, (unsigned long long)qB,
           hash64_copy(qA, FP_MASK));

    n = 0;
    /* qA: ONE zero-duration transition -> claims the slot, total_ns stays 0,
     * total_transitions becomes 1. */
    push(100, WAIT_A, WAIT_B, 2000000000ULL, 0, qA);
    /* qB: two real transitions. */
    push(101, WAIT_A, WAIT_B, 3000000000ULL, 1000000ULL, qB);
    push(101, WAIT_B, WAIT_A, 4000000000ULL, 1000000ULL, qB);
    struct pgwt_fingerprint_result fp;
    pgwt_compute_fingerprints(ev, n, &nf, &fp);
    printf("FP rows: %d\n", fp.num_rows);
    for (int i = 0; i < fp.num_rows; i++)
        printf("  query_id=%llu transitions=%llu\n",
               (unsigned long long)fp.rows[i].query_id,
               (unsigned long long)fp.rows[i].total_transitions);
    printf("EXPECTED: qB=%llu with transitions=2 (qA contributed 0 ns and "
           "must not donate its transition)\n", (unsigned long long)qB);
    free(fp.rows);

    /* ── 3. Top sessions (compute.c:1580), occupancy pid != 0 */
    n = 0;
    /* pid 0 accumulates 5 ms, then pid SESSION_HT_SIZE (home slot 0 too)
     * claims the same slot and inherits it. */
    push(0, WAIT_A, WAIT_B, 2000000000ULL, 5000000ULL, 0);
    push(1024, WAIT_A, WAIT_B, 3000000000ULL, 1000000ULL, 0);
    struct pgwt_sessions_result ss;
    pgwt_compute_top_sessions(ev, n, &nf, 1000000000ULL, 5000000000ULL, 4000,
                              &ss);
    printf("\nSessions rows: %d\n", ss.num_rows);
    for (int i = 0; i < ss.num_rows; i++)
        printf("  pid=%u db_time_ms=%.6f\n", ss.rows[i].pid,
               ss.rows[i].db_time_ms);
    printf("EXPECTED: pid 1024 with db_time_ms=1.0 (it must not inherit "
           "pid 0's 5.0 ms)\n");
    free(ss.rows);

    free(ev);
    return 0;
}
