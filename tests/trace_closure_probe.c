/* trace_closure_probe.c — print how a trace file ended, and what checkpoint
 * (if any) sits beside it. Used by tests/crash_scenarios.sh so a shell script
 * never has to re-implement the flag mapping: the verdict comes from the
 * production pgwt_trace_file_closure / pgwt_open_ckpt_load, which is the whole
 * point — a second copy of the rule in a test harness is how a harness starts
 * approving things the product would refuse.
 *
 * Usage: trace_closure_probe <file.trace[.lz4]> ...
 * Prints one line per file:
 *   <path> closure=<unknown|in_progress|rotated|clean|unclean> \
 *          may_lose_open_intervals=<0|1> checkpoint=<none|refused:<reason>| \
 *          seq=<n>,intervals=<n>,open_ns=<sum>>
 * Exit status: 0 if every file was readable, 1 otherwise.
 */
#include "event_writer.h"
#include "open_ckpt.h"
#include "trace_closure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace file> ...\n", argv[0]);
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        enum pgwt_closure_status st;
        int ok = pgwt_trace_file_closure(path, &st) == 0;
        if (!ok)
            rc = 1;

        /* "live" only for the file a daemon is appending to right now. */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        bool live = strcmp(base, "current.trace") == 0;

        char ck[160];
        char side[1024];
        snprintf(side, sizeof(side), "%s%s", path, PGWT_OPEN_CKPT_SUFFIX);
        if (live)
            snprintf(side, sizeof(side), "%.*s/%s",
                     (int)(base - path > 0 ? base - path - 1 : 1), path,
                     PGWT_OPEN_CKPT_NAME);
        if (access(side, F_OK) != 0) {
            snprintf(ck, sizeof(ck), "none");
        } else {
            struct pgwt_open_ckpt_view v;
            void *buf = NULL;
            enum pgwt_open_ckpt_rc crc =
                pgwt_open_ckpt_load(side, NULL, &v, &buf);
            if (crc != PGWT_OPEN_CKPT_OK) {
                snprintf(ck, sizeof(ck), "refused:%s",
                         pgwt_open_ckpt_rc_name(crc));
            } else {
                unsigned long long sum = 0;
                for (int j = 0; j < v.count; j++)
                    sum += v.records[j].duration_ns;
                snprintf(ck, sizeof(ck),
                         "seq=%llu,intervals=%d,open_ns=%llu",
                         (unsigned long long)v.header.seq, v.count, sum);
            }
            free(buf);
        }

        printf("%s closure=%s may_lose_open_intervals=%d checkpoint=%s\n",
               path, ok ? pgwt_closure_name(st) : "unreadable",
               pgwt_closure_open_intervals_may_be_missing(
                   ok ? st : PGWT_CLOSURE_UNKNOWN, live) ? 1 : 0,
               ck);
    }
    return rc;
}
