/* probe327.c — empirical saturation probe for issue #327.
 * Loads an ENTIRE trace file and runs variants + interference over it,
 * printing num_variants / num_rows. Not a unit test; a measurement.
 *
 * Build (macOS, from the worktree root):
 *   cc -g -O2 -Isrc -DPGWT_SERVER -o /tmp/probe327 \
 *      tests/results/probe-bounds/probe327.c src/compute.c src/event_reader.c \
 *      build/wait_event.o build/idle_rule.o build/cJSON.o -lm
 */
#include "compute.h"
#include "event_reader.h"
#include "spawn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* spawn.c does not build on macOS (pipe2); wait_event.c references these
 * but the probe never resolves a live PG, so stubs are enough. */
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[]) { (void)p; (void)argv; return -1; }
int pgwt_proc_open_rw(struct pgwt_proc *p, char *const argv[]) { (void)p; (void)argv; return -1; }
int pgwt_proc_close(struct pgwt_proc *p) { (void)p; return -1; }
int pgwt_proc_close_bounded(struct pgwt_proc *p, int t) { (void)p; (void)t; return -1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: probe327 <trace>\n"); return 2; }
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, argv[1]) != 0) {
        fprintf(stderr, "open failed\n"); return 2;
    }
    printf("blocks: %d\n", r.num_blocks);

    int cap = 1 << 20, count = 0;
    struct pgwt_trace_event *evs = malloc((size_t)cap * sizeof(*evs));
    struct pgwt_trace_event *blk = malloc(1000000 * sizeof(*blk));
    int nsamples = 0, ntrans = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, blk, 1000000, &bi);
        if (n < 0) continue;
        if (bi.block_type == PGWT_BLOCK_SAMPLES) { nsamples += n; continue; }
        ntrans += n;
        while (count + n > cap) {
            cap *= 2;
            evs = realloc(evs, (size_t)cap * sizeof(*evs));
        }
        memcpy(evs + count, blk, (size_t)n * sizeof(*blk));
        count += n;
    }
    printf("transition events: %d  (samples skipped: %d)\n", ntrans, nsamples);
    if (count > 0)
        printf("window: %llu .. %llu (%.1f s)\n",
               (unsigned long long)evs[0].timestamp_ns,
               (unsigned long long)evs[count - 1].timestamp_ns,
               (double)(evs[count - 1].timestamp_ns - evs[0].timestamp_ns) / 1e9);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    double t0 = now_s();
    struct pgwt_variants_result vr;
    memset(&vr, 0, sizeof(vr));
    pgwt_compute_variants(evs, count, &f, 20, PGWT_PHASE_EXEC, &vr);
    printf("VARIANTS exec: num_variants=%d total_executions=%d failed=%d  (%.2fs)\n",
           vr.num_variants, vr.total_executions, vr.failed, now_s() - t0);
    fflush(stdout);
    free(vr.variants);

    t0 = now_s();
    struct pgwt_variants_result pr;
    memset(&pr, 0, sizeof(pr));
    pgwt_compute_variants(evs, count, &f, 20, PGWT_PHASE_PLAN, &pr);
    printf("VARIANTS plan: num_variants=%d total_executions=%d failed=%d  (%.2fs)\n",
           pr.num_variants, pr.total_executions, pr.failed, now_s() - t0);
    fflush(stdout);
    free(pr.variants);

    t0 = now_s();
    struct pgwt_interference_result ir;
    memset(&ir, 0, sizeof(ir));
    pgwt_compute_interference(evs, count, &f, 30, &ir);
    printf("INTERFERENCE: num_rows=%d  (%.2fs)\n", ir.num_rows, now_s() - t0);
    fflush(stdout);
    free(ir.rows);

    pgwt_reader_close(&r);
    return 0;
}
