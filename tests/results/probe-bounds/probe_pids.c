/* probe_pids.c — how close does a REAL trace get to the 1024-slot cap at
 * compute.c:1580 / :2517? Counts distinct pids (= num_rows of
 * pgwt_compute_top_sessions) over the whole trace and over every 900 s
 * window at 300 s steps, which is the UI's boot default width.
 *
 * Build: see probe327.c's header; same objects, plus this file.
 */
#include "compute.h"
#include "event_reader.h"
#include "spawn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pgwt_proc_open(struct pgwt_proc *p, char *const a[]) { (void)p; (void)a; return -1; }
int pgwt_proc_open_rw(struct pgwt_proc *p, char *const a[]) { (void)p; (void)a; return -1; }
int pgwt_proc_close(struct pgwt_proc *p) { (void)p; return -1; }
int pgwt_proc_close_bounded(struct pgwt_proc *p, int t) { (void)p; (void)t; return -1; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: probe_pids <trace>\n"); return 2; }
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, argv[1]) != 0) { fprintf(stderr, "open failed\n"); return 2; }

    int cap = 1 << 20, count = 0;
    struct pgwt_trace_event *evs = malloc((size_t)cap * sizeof(*evs));
    struct pgwt_trace_event *blk = malloc(1000000 * sizeof(*blk));
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, blk, 1000000, &bi);
        if (n < 0 || bi.block_type == PGWT_BLOCK_SAMPLES) continue;
        while (count + n > cap) { cap *= 2; evs = realloc(evs, (size_t)cap * sizeof(*evs)); }
        memcpy(evs + count, blk, (size_t)n * sizeof(*blk));
        count += n;
    }
    if (count == 0) { fprintf(stderr, "no transition events\n"); return 2; }
    uint64_t t0 = evs[0].timestamp_ns, t1 = evs[count - 1].timestamp_ns;
    printf("events=%d window=%.1f s\n", count, (double)(t1 - t0) / 1e9);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_sessions_result all;
    pgwt_compute_top_sessions(evs, count, &f, 0, 0, 0, &all);
    printf("WHOLE TRACE: distinct pids = %d  (cap at :1580/:2517 was 1024, "
           "%.1f%%)\n", all.num_rows, 100.0 * all.num_rows / 1024.0);
    free(all.rows);

    /* Every 900 s window at 300 s steps — the UI boot default width. */
    const uint64_t W = 900ULL * 1000000000ULL, STEP = 300ULL * 1000000000ULL;
    int worst = 0;
    double worst_at = 0;
    for (uint64_t s = t0; s + W <= t1 + STEP; s += STEP) {
        uint64_t e = s + W;
        /* Same selection the loader uses: events whose END is in [s, e). */
        int lo = 0, hi = 0;
        while (lo < count && evs[lo].timestamp_ns < s) lo++;
        hi = lo;
        while (hi < count && evs[hi].timestamp_ns < e) hi++;
        if (hi <= lo) continue;
        struct pgwt_sessions_result w;
        pgwt_compute_top_sessions(evs + lo, hi - lo, &f, s, e, 0, &w);
        double at = (double)(s - t0) / 1e9;
        printf("  900 s window @ +%6.0f s: distinct pids = %4d  (%.1f%% of "
               "1024)\n", at, w.num_rows, 100.0 * w.num_rows / 1024.0);
        if (w.num_rows > worst) { worst = w.num_rows; worst_at = at; }
        free(w.rows);
    }
    printf("WORST 900 s WINDOW: %d distinct pids @ +%.0f s (%.1f%% of the "
           "1024 cap)\n", worst, worst_at, 100.0 * worst / 1024.0);

    pgwt_reader_close(&r);
    return 0;
}
