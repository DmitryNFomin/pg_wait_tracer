/* replay.c — Offline replay: scan trace files, decode blocks, display */
#include "replay.h"
#include "daemon.h"
#include "event_reader.h"
#include "output.h"
#include "wait_event.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int pgwt_run_replay(struct pgwt_daemon *d, const char *from_str,
                    const char *to_str)
{
    d->replay_mode = true;

    if (!d->trace_dir) {
        fprintf(stderr, "FATAL: --replay requires --trace-dir\n");
        return 1;
    }

    if (d->view == PGWT_VIEW_ACTIVE) {
        fprintf(stderr, "FATAL: active view requires live BPF — not available in replay mode\n");
        return 1;
    }

    /* Parse time bounds (wall-clock nanoseconds) */
    uint64_t from_wall = 0, to_wall = 0;

    if (from_str) {
        if (pgwt_parse_time(from_str, &from_wall) != 0) {
            fprintf(stderr, "FATAL: cannot parse --from '%s'\n", from_str);
            return 1;
        }
    }
    if (to_str) {
        if (pgwt_parse_time(to_str, &to_wall) != 0) {
            fprintf(stderr, "FATAL: cannot parse --to '%s'\n", to_str);
            return 1;
        }
    }
    if (from_wall > 0 && to_wall > 0 && from_wall >= to_wall) {
        fprintf(stderr, "FATAL: --from must be before --to\n");
        return 1;
    }

    /* Scan trace files */
    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(d->trace_dir, files, 256);
    if (nfiles < 0) {
        fprintf(stderr, "FATAL: cannot scan trace directory '%s'\n", d->trace_dir);
        return 1;
    }
    if (nfiles == 0) {
        fprintf(stderr, "FATAL: no trace files found in '%s'\n", d->trace_dir);
        return 1;
    }

    if (d->verbose)
        fprintf(stderr, "INFO: found %d trace file(s) in %s\n", nfiles, d->trace_dir);

    /* Initialize accumulator */
    pgwt_accum_init(&d->accum);

    /* Event decode buffer */
    struct pgwt_trace_event *events = malloc(PGWT_BLOCK_EVENTS * sizeof(*events));
    if (!events) {
        fprintf(stderr, "FATAL: cannot allocate event buffer\n");
        return 1;
    }

    uint64_t total_events = 0;
    int files_read = 0;
    bool pg_version_set = false;
    struct pgwt_replay_stats rstats = {0};

    for (int fi = 0; fi < nfiles; fi++) {
        /* Quick filter: skip files that end before our --from time */
        if (from_wall > 0 && fi + 1 < nfiles &&
            files[fi + 1].start_wall_ns > 0 &&
            files[fi + 1].start_wall_ns < from_wall)
            continue;

        struct pgwt_event_reader reader;
        if (pgwt_reader_open(&reader, files[fi].path) != 0)
            continue;

        /* Get PG version from first successfully opened file */
        if (!pg_version_set && reader.header.pg_version > 0) {
            d->pg_major_version = (int)reader.header.pg_version;
            pgwt_init_event_names(d->pg_major_version);
            pg_version_set = true;
            if (d->verbose)
                fprintf(stderr, "INFO: trace file PG version: %d\n",
                        d->pg_major_version);
        }

        /* Convert wall-clock bounds to this file's monotonic clock */
        uint64_t from_mono = 0, to_mono = 0;
        if (from_wall > 0)
            from_mono = pgwt_reader_wall_to_mono(&reader, from_wall);
        if (to_wall > 0)
            to_mono = pgwt_reader_wall_to_mono(&reader, to_wall);

        /* Find starting block */
        int start_block = 0;
        if (from_mono > 0)
            start_block = pgwt_reader_find_block(&reader, from_mono);

        if (d->verbose)
            fprintf(stderr, "INFO: %s: %d blocks, starting at block %d\n",
                    files[fi].path, reader.num_blocks, start_block);

        for (int bi = start_block; bi < reader.num_blocks; bi++) {
            /* Quick check: if block starts after --to, we're done with this file */
            if (to_mono > 0 &&
                reader.block_index[bi].timestamp_ns > to_mono)
                break;

            struct pgwt_block_info binfo;
            int count = pgwt_reader_decode_block_info(&reader, bi, events,
                                                      PGWT_BLOCK_EVENTS,
                                                      &binfo);
            if (count <= 0)
                continue;

            pgwt_replay_events(&d->accum, events, count, from_mono, to_mono,
                               &binfo, &rstats);
            total_events += count;
        }

        pgwt_reader_close(&reader);
        files_read++;
    }

    free(events);

    if (files_read == 0) {
        fprintf(stderr, "FATAL: could not read any trace files\n");
        return 1;
    }

    /* REFUSE a trace that never declared its PostgreSQL major.
     *
     * This used to install PG18 here -- AFTER every event had already been
     * accounted -- which was incoherent in both directions: the accounting
     * ran under whatever mask happened to be current, and the PG18 mask it
     * then installed could not retroactively change it. Since 2026-10-07 the
     * uninitialised mask is empty, so such a trace silently counted pacing
     * waits as DB Time while pgwt-server on the same directory answered
     * differently.
     *
     * Rather than pick an assumption, refuse: a replay is pure accounting, and
     * numbers produced under an undefined classification are the failure this
     * branch exists to remove. Every trace this build writes records its
     * major (pgwt_writer_init takes it), so this only rejects a file that
     * genuinely does not say. */
    if (!pg_version_set) {
        fprintf(stderr, "FATAL: no trace file in this directory records a "
                "PostgreSQL major version, so wait events cannot be named or "
                "classified (Timeout pacing waits would be counted as DB Time "
                "without any way to tell). Refusing to replay rather than "
                "report numbers under an assumed version.\n");
        return 1;
    }

    if (d->verbose)
        fprintf(stderr, "INFO: replayed %lu events from %d file(s)\n",
                (unsigned long)total_events, files_read);

    /* Fidelity notice (FID-5): a tiered trace is mostly SAMPLES records.
     * Their time contributions are ASH estimates (count × sample period),
     * and they carry no real latencies — say so loudly instead of quietly
     * rendering near-zero activity or fabricated histograms. */
    if (rstats.samples > 0) {
        printf("NOTE: this trace contains sampled data "
               "(%llu samples at %.1f ms period, %llu exact transitions).\n"
               "      Sampled wait durations are ASH estimates "
               "(sample count x period);\n"
               "      histograms/min/max reflect exact data only. "
               "For merged analysis use pgwt-server (--view).\n\n",
               (unsigned long long)rstats.samples,
               rstats.sample_period_ns / 1e6,
               (unsigned long long)rstats.transitions);
    }

    /* Display results using existing output functions */
    d->tick = 1;  /* pretend we have one interval for display */
    d->interval = 0;  /* suppress "interval" display */

    pgwt_print_header(d);
    if (d->view == PGWT_VIEW_HISTOGRAM &&
        rstats.samples > 0 && rstats.transitions == 0) {
        printf("Latency histogram unavailable: this window contains only "
               "sampled data,\nwhich carries no real durations. "
               "Escalate to full fidelity to capture latencies.\n");
        return 0;
    }
    switch (d->view) {
    case PGWT_VIEW_TIME_MODEL:   pgwt_print_time_model(d);   break;
    case PGWT_VIEW_SYSTEM_EVENT: pgwt_print_system_event(d);  break;
    case PGWT_VIEW_SESSION_EVENT: pgwt_print_session_event(d); break;
    case PGWT_VIEW_HISTOGRAM:    pgwt_print_histogram(d);     break;
    case PGWT_VIEW_QUERY_EVENT:  pgwt_print_query_event(d);   break;
    default: break;
    }

    return 0;
}
