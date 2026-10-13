/* Raw marker/transition evidence only; wait CPU statistics use the existing
 * tests/measure_wait_cpu.py. Times in the JSONL are wall-clock ns. */
#include "event_reader.h"
#include <inttypes.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    unsigned pid = (unsigned)strtoul(argv[2], NULL, 10);
    uint64_t from = strtoull(argv[3], NULL, 10);
    uint64_t to = strtoull(argv[4], NULL, 10);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, argv[1])) return 2;
    fprintf(stderr, "version=%u pg_version=%u blocks=%d mono_to_wall=%" PRId64 "\n",
            r.header.version, r.header.pg_version, r.num_blocks, r.mono_to_wall);
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info info;
        if (pgwt_reader_block_info(&r, b, &info)) return 2;
        struct pgwt_trace_event *events = calloc(info.num_events, sizeof(*events));
        if (!events) return 2;
        int n = pgwt_reader_decode_block(&r, b, events, (int)info.num_events);
        if (n < 0 || (unsigned)n != info.num_events) return 2;
        for (int i = 0; i < n; i++) {
            const struct pgwt_trace_event *e = &events[i];
            uint64_t end = pgwt_reader_mono_to_wall(&r, e->timestamp_ns);
            uint64_t start = end - e->duration_ns;
            /* pid=0 selects Lock and PgSleep records, without a lane cap. */
            if (pid ? e->pid != pid :
                ((e->old_event >> 24) != 3 && e->old_event != 150994946)) continue;
            if (end < from || start > to) continue;
            printf("{\"block\":%d,\"record\":%d,\"pid\":%u,"
                   "\"start_ns\":\"%" PRIu64 "\",\"end_ns\":\"%" PRIu64 "\","
                   "\"old_event\":%u,\"new_event\":%u,\"flags\":%u,"
                   "\"duration_ns\":\"%" PRIu64 "\",\"cpu_ns\":\"%" PRIu64 "\","
                   "\"query_id\":\"%" PRId64 "\"}\n",
                   b, i, e->pid, start, end, e->old_event, e->new_event,
                   e->flags, e->duration_ns, e->cpu_ns, (int64_t)e->query_id);
        }
        free(events);
    }
    pgwt_reader_close(&r);
    return 0;
}
