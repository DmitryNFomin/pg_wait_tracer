/* dump_markers.c — Decode escalation-window markers from a trace dir (A5).
 *
 * The escalation engine writes PGWT_MARKER_ESCALATE_START / _END records into
 * the trace, packing (window_seconds, reason) into query_id
 * (PGWT_ESC_PACK). No view surfaces these until B5, so this tiny tool reads a
 * trace directory and prints each escalation marker with its decoded reason,
 * giving the A5 live test concrete evidence that an ANOMALY-reason window was
 * recorded (distinct from a MANUAL one).
 *
 * Built with -DPGWT_SERVER (no BPF) so it runs anywhere the server builds.
 *
 * Usage: dump_markers <trace_dir>
 * Prints one line per marker:  <START|END> reason=<name> window_s=<n> ts=<ns>
 * Exit 0 if any escalation marker was found, 1 otherwise.
 */
#include "event_reader.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_EVENTS 65536

static const char *reason_name(unsigned r)
{
    switch (r) {
    case PGWT_ESC_REASON_MANUAL:   return "manual";
    case PGWT_ESC_REASON_ANOMALY:  return "anomaly";
    case PGWT_ESC_REASON_EXPIRED:  return "expired";
    case PGWT_ESC_REASON_REQUEST:  return "request";
    case PGWT_ESC_REASON_SHUTDOWN: return "shutdown";
    case PGWT_ESC_REASON_CPU_SATURATION: return "cpu_saturation";
    default:                       return "unknown";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace_dir>\n", argv[0]);
        return 2;
    }
    const char *trace_dir = argv[1];

    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(trace_dir, files, 256);
    if (nfiles <= 0) {
        fprintf(stderr, "ERROR: no trace files in %s\n", trace_dir);
        return 1;
    }

    struct pgwt_trace_event *evs = malloc(MAX_EVENTS * sizeof(*evs));
    if (!evs) { perror("malloc"); return 1; }

    int found = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0)
            continue;
        for (int b = 0; b < r.num_blocks; b++) {
            int n = pgwt_reader_decode_block(&r, b, evs, MAX_EVENTS);
            if (n <= 0)
                continue;
            for (int i = 0; i < n; i++) {
                uint32_t m = evs[i].new_event;
                if (m != PGWT_MARKER_ESCALATE_START &&
                    m != PGWT_MARKER_ESCALATE_END)
                    continue;
                unsigned reason = PGWT_ESC_UNPACK_REASON(evs[i].query_id);
                uint64_t secs = PGWT_ESC_UNPACK_SECS(evs[i].query_id);
                printf("%-5s reason=%-8s window_s=%llu ts=%llu\n",
                       m == PGWT_MARKER_ESCALATE_START ? "START" : "END",
                       reason_name(reason),
                       (unsigned long long)secs,
                       (unsigned long long)evs[i].timestamp_ns);
                found++;
            }
        }
        pgwt_reader_close(&r);
    }

    free(evs);
    if (found == 0) {
        fprintf(stderr, "no escalation markers found\n");
        return 1;
    }
    return 0;
}
