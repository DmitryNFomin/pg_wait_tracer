
#include "compute.h"
#include <stdio.h>
#include <stdlib.h>
void __real_pgwt_compute_top_sessions_from_summaries(const char *, uint64_t,
    uint64_t, const struct pgwt_filter *, double, struct pgwt_sessions_result *);
void __real_pgwt_compute_top_sessions(const struct pgwt_trace_event *, int,
    const struct pgwt_filter *, uint64_t, uint64_t, double,
    struct pgwt_sessions_result *);
void __wrap_pgwt_compute_top_sessions_from_summaries(const char *dir,
    uint64_t from, uint64_t to, const struct pgwt_filter *f, double wall,
    struct pgwt_sessions_result *out) {
    fprintf(stderr, "CONTRACT_ROUTE summary\n");
    __real_pgwt_compute_top_sessions_from_summaries(dir, from, to, f, wall, out);
    fprintf(stderr, "CONTRACT_RESULT failed=%d num_rows=%d rows_null=%d\n",
        out->failed, out->num_rows, out->rows == NULL);
}
void __wrap_pgwt_compute_top_sessions(const struct pgwt_trace_event *events,
    int count, const struct pgwt_filter *f, uint64_t from, uint64_t to,
    double wall, struct pgwt_sessions_result *out) {
    const char *forced = getenv("PGWT_CONTRACT_FORCE_SUMMARY");
    fprintf(stderr, "CONTRACT_ROUTE %s\n", forced ? "forced-summary" : "raw");
    if (forced)
        __real_pgwt_compute_top_sessions_from_summaries(forced, from, to, f, wall, out);
    else
        __real_pgwt_compute_top_sessions(events, count, f, from, to, wall, out);
    fprintf(stderr, "CONTRACT_RESULT failed=%d num_rows=%d rows_null=%d\n",
        out->failed, out->num_rows, out->rows == NULL);
}
