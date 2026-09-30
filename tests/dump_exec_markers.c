/* dump_exec_markers.c — per-pid execution-lifecycle evidence from a trace dir.
 *
 * The live cancel test (tests/test_cancel_orphan.py) has to prove something
 * the server's own answer cannot prove on its own: that the cancelled
 * statement's row was closed at a DEDUCED boundary because no EXEC_END was
 * ever emitted for it — not that PostgreSQL quietly emitted one anyway, in
 * which case the defect would not exist and the test would be passing
 * vacuously. This tool reads the raw trace and prints, for one pid:
 *
 *   EXEC_START ts=<ns>
 *   EXEC_END   ts=<ns>
 *   IDLE       ts=<ns> we=<wait_event_info>   (transition INTO an idle wait)
 *   EXIT       ts=<ns>
 *
 * in timestamp order, one per line, so the test can assert on the sequence
 * itself. "IDLE" is the wait stream's own boundary — a record whose
 * new_event is Activity-class or Client:ClientRead (pgwt_is_idle_event);
 * that is exactly the boundary src/compute.c closes an orphan row at.
 *
 * Built with -DPGWT_SERVER (no BPF) so it runs anywhere the server builds.
 *
 * Usage: dump_exec_markers <trace_dir> [pid]
 *   With no pid, every pid that has at least one EXEC_START is printed.
 * Exit 0 if any line was printed, 1 if nothing matched (an empty or
 * marker-free trace must be a FAILURE for the caller, never a silent pass).
 */
#include "event_reader.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_EVENTS 65536

struct line {
    uint64_t ts;
    uint32_t pid;
    uint32_t we;
    const char *kind;
};

static int by_ts(const void *a, const void *b)
{
    const struct line *x = a, *y = b;
    if (x->ts < y->ts) return -1;
    if (x->ts > y->ts) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace_dir> [pid]\n", argv[0]);
        return 2;
    }
    const char *trace_dir = argv[1];
    uint32_t want_pid = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;

    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(trace_dir, files, 256);
    if (nfiles <= 0) {
        fprintf(stderr, "ERROR: no trace files in %s\n", trace_dir);
        return 1;
    }

    struct pgwt_trace_event *evs = malloc(MAX_EVENTS * sizeof(*evs));
    if (!evs) { perror("malloc"); return 1; }

    int cap = 4096, n_lines = 0;
    struct line *lines = malloc(cap * sizeof(*lines));
    if (!lines) { perror("malloc"); free(evs); return 1; }

    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0)
            continue;
        for (int b = 0; b < r.num_blocks; b++) {
            int n = pgwt_reader_decode_block(&r, b, evs, MAX_EVENTS);
            for (int i = 0; i < n; i++) {
                const struct pgwt_trace_event *e = &evs[i];
                if (want_pid && e->pid != want_pid)
                    continue;
                const char *kind = NULL;
                uint32_t we = 0;
                if (e->old_event == PGWT_MARKER_EXEC_START)
                    kind = "EXEC_START";
                else if (e->old_event == PGWT_MARKER_EXEC_END)
                    kind = "EXEC_END";
                else if (PGWT_IS_MARKER(e->old_event))
                    continue;
                else if (e->flags & PGWT_EVENT_FLAG_SAMPLE)
                    continue;   /* a sample is not a transition */
                else if (e->new_event == PGWT_EVENT_EXIT)
                    kind = "EXIT";
                else if (pgwt_is_idle_event(e->new_event)) {
                    kind = "IDLE";
                    we = e->new_event;
                } else
                    continue;

                if (n_lines >= cap) {
                    cap *= 2;
                    struct line *tmp = realloc(lines, cap * sizeof(*lines));
                    if (!tmp) { perror("realloc"); goto done; }
                    lines = tmp;
                }
                lines[n_lines].ts = e->timestamp_ns;
                lines[n_lines].pid = e->pid;
                lines[n_lines].we = we;
                lines[n_lines].kind = kind;
                n_lines++;
            }
        }
        pgwt_reader_close(&r);
    }

done:
    qsort(lines, n_lines, sizeof(*lines), by_ts);
    for (int i = 0; i < n_lines; i++)
        printf("%-10s pid=%u ts=%llu we=%u\n", lines[i].kind, lines[i].pid,
               (unsigned long long)lines[i].ts, lines[i].we);

    int found = n_lines;
    free(lines);
    free(evs);
    if (found == 0) {
        fprintf(stderr, "no execution-lifecycle records found\n");
        return 1;
    }
    return 0;
}
