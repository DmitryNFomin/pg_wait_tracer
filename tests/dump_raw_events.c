/* dump_raw_events.c — INSTRUMENT ONLY (issues #261 / #306), not a product tool.
 *
 * Dumps every record of a tiered trace directory as TSV so the sampled and
 * exact tiers can be re-compared offline under definitions cross_validate
 * does not offer: in particular, evaluating the EXACT state of each backend
 * at the SAMPLER'S OWN tick timestamps (identical backend population,
 * identical instants), which is what #261 asks for and what shares cannot
 * distinguish from a sampler classification error.
 *
 * Columns (tab separated, one header line starting with '#'):
 *   kind       S (sample) | T (transition)
 *   pid
 *   mono_ns    record timestamp (ktime)
 *   wall_ns    same instant mapped to the file's wall clock
 *   old_event  transitions: the event the interval was spent in
 *   new_event  samples: the sampled event (0 = on CPU)
 *   duration_ns
 *   cpu_ns     measured on-CPU ns in the interval, or -1 for UNKNOWN
 *   flags      in-memory annotation (CMD_OPEN etc.)
 *   period_ns  SAMPLES block sample period (0 for transitions)
 *
 * Usage: dump_raw_events <trace_dir>
 */
#include "event_reader.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"
#include "idle_rule.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MAX_EVENTS 65536

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <trace_dir>\n", argv[0]); return 2; }

    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(argv[1], files, 256);
    if (nfiles <= 0) { fprintf(stderr, "ERROR: no trace files in %s\n", argv[1]); return 1; }

    /* Same name/idle-rule resolution cross_validate does, so the `idle`
     * column here IS the comparator's idle rule and not a second guess at
     * it: sidecar first, then the trace header's major. */
    int hdr_major = 0;
    for (int i = 0; i < nfiles && hdr_major == 0; i++) {
        struct pgwt_event_reader hr;
        if (pgwt_reader_open(&hr, files[i].path) != 0) continue;
        if (hr.header.pg_version > 0) hdr_major = (int)hr.header.pg_version;
        pgwt_reader_close(&hr);
    }
    int sidecar_ok = (pgwt_load_names_json(argv[1]) == 0);
    int settled = pgwt_init_event_names_for_trace(argv[1], hdr_major);
    fprintf(stderr, "dump_raw_events: sidecar=%d settled_major=%d\n",
            sidecar_ok, settled);
    if (!sidecar_ok && settled <= 0) {
        /* An empty pacing mask would silently mark every pacing sleep
         * non-idle, i.e. the `idle` column would be a measured-looking
         * undercount. Refuse rather than emit it. */
        fprintf(stderr, "ERROR: no sidecar and no PG major -- the idle rule "
                "cannot be resolved, so the idle column would be wrong. "
                "Refusing.\n");
        return 3;
    }

    struct pgwt_trace_event *evs = malloc(MAX_EVENTS * sizeof(*evs));
    if (!evs) { perror("malloc"); return 1; }

    printf("#kind\tpid\tmono_ns\twall_ns\told_event\tnew_event\tduration_ns\tcpu_ns\tflags\tperiod_ns\tidle\tmarker\n");
    long emitted = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0) continue;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info info;
            int n = pgwt_reader_decode_block_info(&r, b, evs, MAX_EVENTS, &info);
            if (n <= 0) continue;
            char kind = (info.block_type == PGWT_BLOCK_SAMPLES) ? 'S' : 'T';
            for (int i = 0; i < n; i++) {
                struct pgwt_trace_event *e = &evs[i];
                long long cpu = (e->cpu_ns == PGWT_CPU_NS_UNKNOWN)
                                    ? -1 : (long long)e->cpu_ns;
                uint32_t cls = (kind == 'S') ? e->new_event : e->old_event;
                int marker = PGWT_IS_MARKER(cls) ? 1 : 0;
                int idle = (!marker && cls != 0 && pgwt_is_idle_event(cls)) ? 1 : 0;
                printf("%c\t%u\t%llu\t%llu\t%u\t%u\t%llu\t%lld\t%u\t%llu\t%d\t%d\n",
                       kind, e->pid,
                       (unsigned long long)e->timestamp_ns,
                       (unsigned long long)pgwt_reader_mono_to_wall(&r, e->timestamp_ns),
                       e->old_event, e->new_event,
                       (unsigned long long)e->duration_ns,
                       cpu, e->flags,
                       (unsigned long long)(kind == 'S' ? info.sample_period_ns : 0),
                       idle, marker);
                emitted++;
            }
        }
        pgwt_reader_close(&r);
    }
    free(evs);
    fprintf(stderr, "dump_raw_events: %ld records from %d file(s)\n", emitted, nfiles);
    return emitted > 0 ? 0 : 1;
}
