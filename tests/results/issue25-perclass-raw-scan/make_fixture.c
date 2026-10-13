/* make_fixture.c — write a tiny synthetic trace so the scanner's refusal and
 * detection paths can be driven on demand (the bypass suite in
 * test_wait_cpu_scan.py). Not a product tool; fixtures only.
 *
 * Usage: make_fixture DIR SPEC[,SPEC...]
 *   SPEC = event_id:count:duration_ns:cpu_ns
 *   cpu_ns = "unknown" writes the PGWT_CPU_NS_UNKNOWN sentinel on every
 *   record of that spec (what a legacy / v2 capture looks like).
 */
#include "event_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s DIR SPEC[,SPEC...]\n", argv[0]);
        return 2;
    }
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, argv[1], 18, 0, NULL) != 0) {
        fprintf(stderr, "writer_init failed\n");
        return 2;
    }
    w.cpu_measured = true;   /* v3 cpu_ns column present */

    uint64_t ts = 1000000000ULL;
    char *spec = strdup(argv[2]);
    uint64_t written = 0;
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long id = 0, count = 0;
        unsigned long long dur = 0;
        char cpu_text[64] = {0};
        if (sscanf(tok, "%lu:%lu:%llu:%63s", &id, &count, &dur, cpu_text) != 4) {
            fprintf(stderr, "bad spec: %s\n", tok);
            return 2;
        }
        int unknown = !strcmp(cpu_text, "unknown");
        uint64_t cpu = unknown ? PGWT_CPU_NS_UNKNOWN
                               : strtoull(cpu_text, NULL, 10);
        for (unsigned long i = 0; i < count; i++) {
            struct pgwt_trace_event e = {
                .timestamp_ns = ts,
                .pid = 4242 + (uint32_t)(i % 3),
                .old_event = (uint32_t)id,
                .new_event = 0,
                .flags = 0,
                .duration_ns = dur,
                .query_id = 777,
                .cpu_ns = cpu,
            };
            /* cpu_measured stays true: event_writer.c preserves a
             * per-record UNKNOWN sentinel set by the producer. */
            if (pgwt_writer_push_event(&w, &e) != 0) {
                fprintf(stderr, "push failed\n");
                return 2;
            }
            ts += dur ? dur : 1000;
            written++;
        }
    }
    free(spec);
    if (pgwt_writer_close(&w) != 0) {
        fprintf(stderr, "writer_close failed\n");
        return 2;
    }
    pgwt_writer_destroy(&w);
    printf("%llu\n", (unsigned long long)written);
    return 0;
}
