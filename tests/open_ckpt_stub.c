/* open_ckpt_stub.c — write an open-interval checkpoint for a LIVE capture,
 * standing in for the daemon producer that is not wired yet (#424).
 *
 * Why this exists: pgwt_writer_checkpoint_open() is a library function with
 * no caller in src/daemon.c, so a real daemon never writes a checkpoint, and
 * a crash-scenario script against a real daemon could only ever assert
 * closure labels — which is exactly how two handover bugs survived a
 * "15/15 PASS" run. This stub lets tests/crash_scenarios.sh put a REAL,
 * validly-identified checkpoint beside a running daemon's current.trace and
 * then assert it survives a kill, so the end-to-end path is covered before
 * the producer lands.
 *
 * It writes through the PRODUCT's pgwt_open_ckpt_write(), never its own
 * serialiser: a test tool with a private copy of the format would validate
 * against itself and prove nothing.
 *
 * Usage: open_ckpt_stub <trace-dir> <n-records> [pid-base] [ns-per-record]
 * The identity is read from <trace-dir>/current.trace's own header, so a
 * checkpoint it writes is accepted for that file and refused for any other.
 * Prints "wrote <n> records open_ns=<sum>" so the caller can assert content.
 */
#include "event_writer.h"
#include "open_ckpt.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <trace-dir> <n-records> [pid-base] [ns-each]\n",
                argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    int n = atoi(argv[2]);
    uint32_t pid_base = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 90000;
    uint64_t ns_each = argc > 4 ? strtoull(argv[4], NULL, 10) : 1000000ULL;

    if (n < 0 || n > PGWT_OPEN_CKPT_MAX_RECORDS) {
        fprintf(stderr, "n out of range\n");
        return 2;
    }

    char trace[700];
    snprintf(trace, sizeof(trace), "%s/current.trace", dir);
    FILE *f = fopen(trace, "rb");
    if (!f) {
        fprintf(stderr, "no live capture at %s\n", trace);
        return 3;
    }
    struct pgwt_trace_file_header hdr;
    int got = fread(&hdr, sizeof(hdr), 1, f) == 1 &&
              hdr.magic == PGWT_TRACE_MAGIC;
    fclose(f);
    if (!got) {
        fprintf(stderr, "%s is not a trace file\n", trace);
        return 3;
    }

    /* CLOCK_MONOTONIC, the same clock the records' timestamps use. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    struct pgwt_open_ckpt_record *recs = NULL;
    uint64_t sum = 0;
    if (n > 0) {
        recs = calloc((size_t)n, sizeof(*recs));
        if (!recs) return 4;
        for (int i = 0; i < n; i++) {
            uint64_t dur = ns_each * (uint64_t)(i + 1);
            if (dur > now) dur = now;     /* never start before the clock did */
            recs[i] = (struct pgwt_open_ckpt_record){
                .timestamp_ns = now,
                .duration_ns  = dur,
                .query_id     = 4242,
                .cpu_ns       = PGWT_CPU_NS_UNKNOWN,
                .pid          = pid_base + (uint32_t)i,
                .event        = 0x01000004,
                .flags        = PGWT_EVENT_FLAG_CMD_OPEN,
                .reserved     = 0,
            };
            sum += dur;
        }
    }

    struct pgwt_open_ckpt_identity id = {
        .trace_start_wall_ns = hdr.start_time_ns,
        .trace_start_mono_ns = hdr.clock_offset_ns,
    };
    int rc = pgwt_open_ckpt_write(dir, &id, 1, now, 0, recs, n, (gid_t)-1);
    free(recs);
    if (rc != 0) {
        fprintf(stderr, "checkpoint write failed\n");
        return 5;
    }
    printf("wrote %d records open_ns=%llu\n", n, (unsigned long long)sum);
    return 0;
}
