/* bench_block_agg.c — measures Phase 1's falsifiable prediction.
 *
 * THE PREDICTION (docs/PAINT_LATENCY_PLAN.md, Phase 1):
 *   per-request cost goes from O(events in window) to
 *   O(blocks x distinct pairs).
 *
 * HOW THAT IS MADE FALSIFIABLE HERE: hold the number of BLOCKS and the number
 * of DISTINCT PAIRS fixed and grow only the events per block. If the
 * prediction is right, the merge cost is flat while the raw cost grows
 * linearly with events. If merge time grows with events per block, the
 * prediction is WRONG and this prints it.
 *
 * This is NOT a unit test and is deliberately NOT in tests/unit_tests.list: a
 * wall-clock assertion on a shared runner is runner noise dressed up as a
 * gate. It prints numbers; a human or a PR body reads them.
 *
 *   make -C tests bench_block_agg && tests/bench_block_agg [reps]
 */
#include "block_agg.h"
#include "compute.h"
#include "summary_reader.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

#define N_DISTINCT_EVENTS 16    /* => at most 256 distinct pairs */

static const struct pgwt_trace_identity TRACE = {
    .trace_version = 3, .pg_version = 170004,
    .start_time_ns = 1700000000000000000ULL, .clock_offset_ns = 7,
};

/* Deterministic pseudo-random so two runs are comparable. */
static uint64_t rng_state = 88172645463325252ULL;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

int main(int argc, char **argv)
{
    int reps = argc > 1 ? atoi(argv[1]) : 5;
    if (reps < 1) reps = 1;

    printf("bench_block_agg — Phase 1 prediction: "
           "O(events in window) -> O(blocks x distinct pairs)\n");
    printf("%d repetition(s) per cell; %d distinct event ids "
           "(<= %d distinct pairs)\n\n",
           reps, N_DISTINCT_EVENTS, N_DISTINCT_EVENTS * N_DISTINCT_EVENTS);

    static const int block_counts[] = { 16, 64, 256 };
    static const int per_block[]    = { 256, 1024, 4096 };

    printf("%8s %10s %10s %14s %14s %10s\n",
           "blocks", "ev/block", "events", "raw ms", "merge ms", "speedup");

    for (size_t bi = 0; bi < sizeof(block_counts) / sizeof(block_counts[0]);
         bi++) {
        for (size_t ei = 0; ei < sizeof(per_block) / sizeof(per_block[0]);
             ei++) {
            int nb = block_counts[bi], ne = per_block[ei];
            long total_events = (long)nb * ne;

            struct pgwt_trace_event *ev =
                malloc(sizeof(*ev) * (size_t)total_events);
            struct pgwt_block_agg *aggs =
                calloc((size_t)nb, sizeof(*aggs));
            struct pgwt_block_identity *ids =
                calloc((size_t)nb, sizeof(*ids));
            if (!ev || !aggs || !ids) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }

            rng_state = 88172645463325252ULL;
            uint64_t ts = TRACE.start_time_ns;
            long k = 0;
            for (int b = 0; b < nb; b++) {
                uint64_t first = ts;
                for (int i = 0; i < ne; i++) {
                    uint32_t a = rng() % N_DISTINCT_EVENTS;
                    uint32_t c = rng() % N_DISTINCT_EVENTS;
                    memset(&ev[k], 0, sizeof(ev[k]));
                    ev[k].timestamp_ns = ts;
                    ev[k].pid = 100 + (rng() % 64);
                    ev[k].old_event = WEI(PG_WAIT_IO, a);
                    ev[k].new_event = WEI(PG_WAIT_LWLOCK, c);
                    ev[k].duration_ns = 1000 + (rng() % 100000);
                    ev[k].cpu_ns = PGWT_CPU_NS_UNKNOWN;
                    ts += 1000;
                    k++;
                }
                ids[b].trace = TRACE;
                ids[b].block_index = (uint32_t)b;
                ids[b].num_events = (uint32_t)ne;
                ids[b].file_offset = (uint64_t)b * 4096 + 4096;
                ids[b].first_timestamp_ns = first;
                ids[b].last_timestamp_ns = ts - 1000;
            }

            /* One-off, amortised across every later request on this block. */
            double t0 = now_ms();
            for (int b = 0; b < nb; b++) {
                int rc = pgwt_block_agg_build(&aggs[b], &ids[b],
                                              PGWT_BLOCK_TRANSITIONS, 1,
                                              &ev[(long)b * ne], ne);
                if (rc != PGWT_BAGG_OK) {
                    fprintf(stderr, "build refused rc=%d\n", rc);
                    return 1;
                }
            }
            double build_ms = now_ms() - t0;

            /* What a request costs today: walk every raw event. */
            struct pgwt_filter f;
            memset(&f, 0, sizeof(f));
            double raw_ms = 0;
            uint64_t raw_total = 0;
            int raw_rows = 0;
            for (int r = 0; r < reps; r++) {
                struct pgwt_transitions_result res;
                double s = now_ms();
                pgwt_compute_transitions(ev, (int)total_events, &f, 50, &res);
                raw_ms += now_ms() - s;
                raw_total = res.total_transitions;
                raw_rows = res.total_rows;
                free(res.rows);
            }
            raw_ms /= reps;

            /* What a request costs under Phase 1: merge the block aggregates. */
            double merge_ms = 0;
            uint64_t merged_total = 0;
            int merged_rows = 0;
            for (int r = 0; r < reps; r++) {
                struct pgwt_block_agg win;
                double s = now_ms();
                pgwt_block_agg_init_window(&win);
                for (int b = 0; b < nb; b++) {
                    int rc = pgwt_block_agg_merge(&win, &aggs[b]);
                    if (rc != PGWT_BAGG_OK) {
                        fprintf(stderr, "merge refused rc=%d\n", rc);
                        return 1;
                    }
                }
                merge_ms += now_ms() - s;
                merged_total = win.total_transitions;
                merged_rows = win.n_pairs;
                pgwt_block_agg_free(&win);
            }
            merge_ms /= reps;

            /* A benchmark that measured two different answers would be
             * measuring nothing. Checked, not assumed. */
            if (merged_total != raw_total || merged_rows != raw_rows) {
                fprintf(stderr, "ABORT: the two paths disagree "
                        "(merged %" PRIu64 "/%d vs raw %" PRIu64 "/%d) — "
                        "the timing below would be meaningless\n",
                        merged_total, merged_rows, raw_total, raw_rows);
                return 1;
            }

            printf("%8d %10d %10ld %14.3f %14.3f %9.1fx"
                   "   (build once: %.3f ms, %d pairs)\n",
                   nb, ne, total_events, raw_ms, merge_ms,
                   merge_ms > 0 ? raw_ms / merge_ms : 0.0,
                   build_ms, merged_rows);

            for (int b = 0; b < nb; b++)
                pgwt_block_agg_free(&aggs[b]);
            free(aggs);
            free(ids);
            free(ev);
        }
        printf("\n");
    }

    printf("Read it down each block-count group: events per block rises 16x "
           "while the block\ncount and the distinct-pair count stay fixed. "
           "The prediction holds only if the\nmerge column is FLAT down the "
           "group while the raw column grows with events.\n");
    return 0;
}
