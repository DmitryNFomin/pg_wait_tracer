/* Repro harness: replay the LIVE producer's call sequence (daemon.c tick
 * order: drain ring -> push_event; read_state_map -> note_open_ex;
 * flush_completed) against the real writer, and print the v9 counters. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pg_wait_tracer.h"
#include "summary_writer.h"

#define SEC 1000000000ULL

int main(int argc, char **argv)
{
    char dir[] = "/tmp/pgwt_repro_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }

    struct pgwt_summary_writer w;
    if (pgwt_summary_writer_init(&w, dir, 24, NULL) != 0) {
        fprintf(stderr, "init failed\n"); return 2;
    }
    pgwt_summary_pin_clock(&w, 1000 * SEC, 1700000000ULL * SEC);

    /* t0 = 1000 s monotonic. A CPU backend (pid 1001) starts an on-CPU
     * interval at t0 and stays on CPU for 10 s. A second backend (pid 1002)
     * emits a closed wait record every tick, like real traffic. */
    uint64_t t0 = 1000 * SEC;
    uint64_t start = t0;               /* sval.last_ts of the on-CPU gap */
    int ticks = argc > 1 ? atoi(argv[1]) : 12;
    const char *mode = argc > 2 ? argv[2] : "stable";

    for (int i = 1; i <= ticks; i++) {
        uint64_t now = t0 + (uint64_t)i * SEC;

        /* 1. drain: a closed wait record from the OTHER pid */
        struct pgwt_trace_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.pid = 1002;
        ev.timestamp_ns = now;
        ev.old_event = 0x0B000004;     /* some wait */
        ev.new_event = 0;
        ev.duration_ns = SEC / 2;
        ev.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_summary_push_event(&w, &ev, 0, ev.old_event);

        /* 2. read_state_map: the open on-CPU note for pid 1001 */
        struct pgwt_summary_open_cpu ocpu = {
            .eff_we = 0,
            .cpu_prefix_ns = (uint64_t)i * SEC * 9 / 10,   /* measured */
        };
        uint64_t st = start;
        uint32_t cat = 0;
        {}
        if (strcmp(mode, "moving") == 0 || strcmp(mode, "chatty") == 0)
            st = now - SEC / 10000;        /* last_ts restamped every tick */
        if (strcmp(mode, "ioworker") == 0)
            cat = PGWT_EVENT_FLAG_IO_WORKER;
        pgwt_summary_note_open_ex(&w, 1001, 0, st, cat, now, &ocpu);

        /* 3. summary_writer maintenance */
        /* "chatty": the pid's own interval closes microseconds after the
         * note, as it does for a backend alternating CPU and IO waits
         * thousands of times a second. */
        if (strcmp(mode, "chatty") == 0) {
            struct pgwt_trace_event c;
            memset(&c, 0, sizeof(c));
            c.pid = 1001;
            c.timestamp_ns = st + SEC / 10000;   /* closes THIS interval */
            c.old_event = 0;                 /* the on-CPU gap closing */
            c.new_event = 0x0A000001;        /* into an IO wait */
            c.duration_ns = SEC / 10000;     /* 100 us of CPU */
            c.cpu_ns = SEC / 10000;
            pgwt_summary_push_event(&w, &c, 0, c.old_event);
        }
        pgwt_summary_flush_completed(&w, now);
        if (getenv("REPRO_TRACE"))
            printf("  tick %2d now=%.3f start=%.3f cpu_cred=%.3f wall_cred=%.3f\n",
                   i, (double)(now - t0) / 1e9, (double)(st - t0) / 1e9,
                   (double)w.open_cpu_credited_ns_total / 1e9,
                   (double)w.open_credited_ns_total / 1e9);
    }

    printf("open_cpu_notes_total        = %llu\n",
           (unsigned long long)w.open_cpu_notes_total);
    printf("open_cpu_credited_ns_total  = %llu\n",
           (unsigned long long)w.open_cpu_credited_ns_total);
    printf("open_credited_ns_total      = %llu\n",
           (unsigned long long)w.open_credited_ns_total);
    printf("open_cpu_measured_ns_total  = %llu\n",
           (unsigned long long)w.open_cpu_measured_ns_total);
    printf("open_cpu_io_worker_refused  = %llu\n",
           (unsigned long long)w.open_cpu_io_worker_refused_total);
    printf("open_cpu_unplaced_total     = %llu\n",
           (unsigned long long)w.open_cpu_unplaced_total);
    printf("open_cpu_refused_total      = %llu\n",
           (unsigned long long)w.open_cpu_refused_total);
    printf("open_io_worker_refused_total= %llu\n",
           (unsigned long long)w.open_io_worker_refused_total);
    printf("open_notes_expired_total    = %llu\n",
           (unsigned long long)w.open_notes_expired_total);
    printf("open_table_full_total       = %llu\n",
           (unsigned long long)w.open_table_full_total);
    printf("open_credit_late_ns_total   = %llu\n",
           (unsigned long long)w.open_credit_late_ns_total);
    printf("open_note_mismatch_total    = %llu\n",
           (unsigned long long)w.open_note_mismatch_total);
    pgwt_summary_close(&w);
    return 0;
}
