/* iwcapture.c — ACCEPTANCE measurement for #315/#376 on a REAL PG18 capture.
 *
 * Not a unit test: it needs a real trace directory (26 MB, untracked), so it
 * lives here with its evidence rather than in tests/unit_tests.list.
 *
 * WHAT IT MEASURES, and why this and not tests/test_agg_raw_crosscheck:
 * that gate's fixture is deliberately io_worker-FREE (hole N7), so it cannot
 * see this bug at all. This drives the REAL writer and the REAL readers over
 * every event of tests/results/demo_rehearsal/trace (PG18, io_method=worker)
 * and prints, for the same whole-capture window:
 *
 *   RAW      pgwt_compute_time_model / pgwt_compute_heatmap over the tagged
 *            events, exactly as pgwt-server's raw branch sees them
 *            (pgwt_tag_events + backends.jsonl, the server's own pipeline).
 *   SUMMARY  pgwt_compute_*_from_summaries over records this program wrote
 *            with the real pgwt_summary_writer.
 *
 * and does the summary side TWICE:
 *   cat=resolved   every push carries the pid's category (what the daemon
 *                  now passes: event_stream.c hoists pgwt_live_pid_cat_flag
 *                  above the push, escalation.c resolves in the capture-end
 *                  flush) -> the AFTER number;
 *   cat=0          every push carries 0, which is byte-for-byte what the
 *                  pre-fix writer did: it had no category input at all
 *                  -> the BEFORE number.
 *
 * THE INPUT IS NEVER PRE-FLAGGED. The events pushed into the writer are the
 * UNTAGGED copy (flags as the daemon emitted them: no category bits). The
 * tagged copy exists only for the raw path, which is where production tags.
 * That is the trap this measurement exists to avoid: a fixture that pre-sets
 * PGWT_EVENT_FLAG_IO_WORKER greens a writer check that is dead in the daemon.
 *
 * TIMING/ORDERING, pinned: events are sorted by timestamp before being
 * pushed (the writer refuses to reopen a flushed second, #277), the mono
 * origin is shifted to the current monotonic second so the records land
 * inside the file's own wall hour exactly as in production, and the compared
 * window is derived from the records that were actually written -- never from
 * a clock. Nothing here sleeps.
 *
 * REFUSES rather than printing a comfortable 0 == 0: no trace dir, no
 * events, no io_worker records in the input, no summary records written, or
 * a summary window the reader refuses, all exit non-zero.
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "event_reader.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define ONE_SEC 1000000000ULL
#define MAX_FILES 64

/* spawn.c's pgwt_proc_open/close pull in pipe2(), which macOS lacks; nothing
 * on this path calls them. */
int  pgwt_proc_open(const char *const argv[], FILE **out);
int  pgwt_proc_close(FILE *fp);
int  pgwt_proc_open(const char *const argv[], FILE **out) { (void)argv; (void)out; return -1; }
int  pgwt_proc_close(FILE *fp) { (void)fp; return -1; }

static int fail;
#define NEED(cond, fmt, ...) do { if (!(cond)) { \
    printf("REFUSE: " fmt "\n", ##__VA_ARGS__); fail = 1; } } while (0)

/* ── pid → category, the way src/server.c does it ─────────────────────── */

struct pgwt_pid_cat cats[1024];
static int n_cats;

static int load_cats(const char *dir)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp) && n_cats < 1024) {
        const char *p = strstr(line, "\"pid\":");
        const char *t = strstr(line, "\"type\":\"");
        if (!p || !t) continue;
        uint32_t pid = (uint32_t)strtoul(p + 6, NULL, 10);
        t += 8;
        uint32_t flag;
        if (strncmp(t, "io_worker", 9) == 0)          flag = PGWT_EVENT_FLAG_IO_WORKER;
        else if (strncmp(t, "autovac_worker", 14) == 0) flag = PGWT_EVENT_FLAG_MAINT;
        else if (strncmp(t, "client", 6) == 0
              || strncmp(t, "parallel_worker", 15) == 0
              || strncmp(t, "unknown", 7) == 0)        flag = 0;
        else                                           flag = PGWT_EVENT_FLAG_BACKGROUND;
        cats[n_cats].pid = pid;
        cats[n_cats].flag = flag;
        n_cats++;
    }
    fclose(fp);
    return n_cats;
}

static uint32_t cat_of(uint32_t pid)
{
    for (int i = 0; i < n_cats; i++)
        if (cats[i].pid == pid) return cats[i].flag;
    return 0;
}

static int cmp_cat(const void *a, const void *b)
{
    uint32_t x = ((const struct pgwt_pid_cat *)a)->pid;
    uint32_t y = ((const struct pgwt_pid_cat *)b)->pid;
    return (x > y) - (x < y);
}

/* ── load every event of the capture, timestamps in MONO ──────────────── */

static struct pgwt_trace_event *all;
static int n_all;
static int64_t mono_to_wall;

static int cmp_ts(const void *a, const void *b)
{
    uint64_t x = ((const struct pgwt_trace_event *)a)->timestamp_ns;
    uint64_t y = ((const struct pgwt_trace_event *)b)->timestamp_ns;
    return (x > y) - (x < y);
}

static int load_events(const char *dir)
{
    struct pgwt_trace_file_entry files[MAX_FILES];
    int nf = pgwt_scan_trace_files(dir, files, MAX_FILES);
    int cap = 1 << 20;
    all = malloc((size_t)cap * sizeof(*all));
    int samples = 0, transitions = 0;
    for (int i = 0; i < nf; i++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[i].path) != 0) continue;
        mono_to_wall = r.mono_to_wall;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info bi;
            if (n_all + 100000 > cap) {
                cap *= 2;
                all = realloc(all, (size_t)cap * sizeof(*all));
            }
            int got = pgwt_reader_decode_block_info(&r, b, all + n_all,
                                                    cap - n_all, &bi);
            if (got <= 0) continue;
            if (bi.block_type == PGWT_BLOCK_SAMPLES) samples += got;
            else transitions += got;
            n_all += got;
        }
        pgwt_reader_close(&r);
    }
    printf("trace files=%d events=%d (transitions=%d samples=%d)\n",
           nf, n_all, transitions, samples);
    qsort(all, (size_t)n_all, sizeof(*all), cmp_ts);
    return n_all;
}

/* ── summary side ─────────────────────────────────────────────────────── */

struct rec_ctx { int records; uint64_t min_sec, max_sec; };

static int rec_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct rec_ctx *c = vctx;
    uint64_t wall_ns = rec->second_wall_ns;
    if (c->records == 0 || wall_ns < c->min_sec) c->min_sec = wall_ns;
    if (c->records == 0 || wall_ns > c->max_sec) c->max_sec = wall_ns;
    c->records++;
    return 0;
}

/* Push every event through the REAL writer. `resolve` selects the AFTER
 * (category resolved per pid, as both daemon producers now do) or the BEFORE
 * (cat_flag 0 everywhere, which is exactly the pre-v6 writer: it had no
 * category input) run. Input events are the UNTAGGED copy. */
static int write_summaries(const char *dir, const struct pgwt_trace_event *ev,
                           int n, int resolve, uint64_t *excl_ns,
                           uint64_t *excl_events)
{
    mkdir(dir, 0755);
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) { free(w); return -1; }
    for (int i = 0; i < n; i++) {
        uint32_t cf = resolve ? cat_of(ev[i].pid) : 0;
        pgwt_summary_push_event(w, &ev[i], cf, ev[i].old_event);
    }
    pgwt_summary_flush(w);
    pgwt_summary_close(w);
    *excl_ns = w->io_worker_ns_excluded_total;
    *excl_events = w->io_worker_events_excluded_total;
    int recs = (int)w->total_records_written;
    pgwt_summary_destroy(w);
    free(w);
    return recs;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1]
        : "/Users/dmitryfomin/work/git/pg_wait_tracer/tests/results/demo_rehearsal/trace";
    const char *scratch = argc > 2 ? argv[2] : "/tmp/iwcapture";

    NEED(load_cats(dir) > 0, "no backends.jsonl entries in %s -- without pid "
         "categories the raw path has nothing to exclude and both sides would "
         "agree for the wrong reason", dir);
    qsort(cats, (size_t)n_cats, sizeof(cats[0]), cmp_cat);
    int n_iw_pids = 0;
    for (int i = 0; i < n_cats; i++)
        if (cats[i].flag == PGWT_EVENT_FLAG_IO_WORKER) n_iw_pids++;
    NEED(n_iw_pids > 0, "backends.jsonl names no io_worker pid");
    printf("pid categories=%d io_worker pids=%d\n", n_cats, n_iw_pids);

    NEED(load_events(dir) > 0, "no events decoded from %s", dir);
    if (fail) return 2;

    /* The io_worker population of the INPUT, so a 0-vs-0 agreement cannot
     * pass for a fix. Also the capture-end straddler count: the record that
     * carries the bulk of the divergence is the one flush_open_intervals
     * synthesizes at capture end. */
    uint64_t iw_ns = 0, iw_idle_ns = 0;
    int iw_recs = 0, iw_flagged_in_input = 0;
    uint64_t last_ts = all[n_all - 1].timestamp_ns;
    uint64_t iw_last_sec_ns = 0;
    for (int i = 0; i < n_all; i++) {
        if (all[i].flags & PGWT_EVENT_FLAG_IO_WORKER) iw_flagged_in_input++;
        if (cat_of(all[i].pid) != PGWT_EVENT_FLAG_IO_WORKER) continue;
        if (PGWT_IS_MARKER(all[i].old_event)) continue;
        iw_recs++;
        iw_ns += all[i].duration_ns;
        if (pgwt_is_idle_event(all[i].old_event)) iw_idle_ns += all[i].duration_ns;
        if (all[i].timestamp_ns + ONE_SEC > last_ts)
            iw_last_sec_ns += all[i].duration_ns;
    }
    printf("io_worker records in input=%d  ns=%llu (idle %llu)  "
           "in final second=%llu ms\n", iw_recs,
           (unsigned long long)iw_ns, (unsigned long long)iw_idle_ns,
           (unsigned long long)(iw_last_sec_ns / 1000000ULL));
    NEED(iw_recs > 0, "the capture carries NO io_worker records -- this "
         "measurement would be vacuous");
    NEED(iw_flagged_in_input == 0,
         "%d input events already carry PGWT_EVENT_FLAG_IO_WORKER: the writer "
         "input must be UNFLAGGED or the daemon-side resolve is not what is "
         "being tested", iw_flagged_in_input);
    if (fail) return 2;

    /* THE RESIDUAL. pgwt_live_pid_cat_flag answers 0 (foreground) until a
     * pid's metadata is parsed, and rescans at most once per display tick.
     * So an io_worker's earliest records are written as foreground even
     * after this fix, while the raw path tags them post hoc from
     * backends.jsonl. Upper bound, measured rather than assumed: the
     * io_worker time in records that close within the first second of the
     * capture (discovery parses metadata at startup, before attach, so one
     * second is generous) and in each pid's FIRST record. */
    uint64_t first_sec_ns = 0, first_rec_ns = 0;
    int first_sec_recs = 0;
    uint64_t t0 = all[0].timestamp_ns;
    for (int p = 0; p < n_cats; p++) {
        if (cats[p].flag != PGWT_EVENT_FLAG_IO_WORKER) continue;
        int seen = 0;
        for (int i = 0; i < n_all; i++) {
            if (all[i].pid != cats[p].pid) continue;
            if (PGWT_IS_MARKER(all[i].old_event)) continue;
            if (!seen) { first_rec_ns += all[i].duration_ns; seen = 1; }
            if (all[i].timestamp_ns < t0 + ONE_SEC) {
                first_sec_ns += all[i].duration_ns;
                first_sec_recs++;
            }
        }
    }
    printf("RESIDUAL upper bounds: first-record ns=%llu (%.6f%% of io_worker "
           "ns)  first-capture-second records=%d ns=%llu (%.6f%%)\n",
           (unsigned long long)first_rec_ns,
           iw_ns ? 100.0 * (double)first_rec_ns / (double)iw_ns : 0.0,
           first_sec_recs, (unsigned long long)first_sec_ns,
           iw_ns ? 100.0 * (double)first_sec_ns / (double)iw_ns : 0.0);

    /* Shift mono to the current monotonic second: the writer maps mono->wall
     * with the offset it captures at open, and the reader's file-level filter
     * covers [file start, +1h]. A 2026-10-02 capture replayed through a
     * writer opened today would convert to a wall time outside its own file's
     * hour, every window query would miss it, and the summary side would
     * answer 0.000 ms -- an agreement test's favourite false pass. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_sec = (((uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec)
                        / ONE_SEC) * ONE_SEC;
    uint64_t base = (all[0].timestamp_ns / ONE_SEC) * ONE_SEC;
    struct pgwt_trace_event *untagged = malloc((size_t)n_all * sizeof(*untagged));
    for (int i = 0; i < n_all; i++) {
        untagged[i] = all[i];
        untagged[i].timestamp_ns = all[i].timestamp_ns - base + now_sec;
    }
    printf("capture span = %.3f s\n",
           (double)(last_ts - all[0].timestamp_ns) / 1e9);

    for (int run = 1; run >= 0; run--) {
        char sdir[700];
        snprintf(sdir, sizeof(sdir), "%s-%s", scratch, run ? "fixed" : "cat0");
        char rm[800];
        snprintf(rm, sizeof(rm), "rm -rf '%s'", sdir);
        if (system(rm) != 0) { /* ignore */ }
        uint64_t excl_ns = 0, excl_ev = 0;
        int recs = write_summaries(sdir, untagged, n_all, run, &excl_ns, &excl_ev);
        NEED(recs > 0, "%s: the writer committed no records", sdir);

        struct rec_ctx c;
        memset(&c, 0, sizeof(c));
        pgwt_visit_summaries(sdir, 0, 0, rec_visitor, &c);
        NEED(c.records > 0, "%s: the reader found no records (a refused "
             "version or a wall window outside the file would read as 0 "
             "and make every comparison below pass on nothing)", sdir);
        if (fail) return 2;
        int considered = -1, unusable = -1;
        uint64_t from = c.min_sec, to = c.max_sec + ONE_SEC;
        NEED(pgwt_summaries_window_current(sdir, from, to, &considered,
                                           &unusable) == 1,
             "%s: the version preflight refuses the window it just wrote",
             sdir);
        double wall_ms = (double)(to - from) / 1e6;

        /* The raw side sees the SAME wall window: shift the (tagged) events
         * by the writer's own mono->wall offset for this run, recovered from
         * the records rather than from a clock. */
        uint64_t wall_shift = from - now_sec;   /* writer's mono->wall offset */
        struct pgwt_trace_event *tagged =
            malloc((size_t)n_all * sizeof(*tagged));
        for (int i = 0; i < n_all; i++) {
            tagged[i] = untagged[i];
            tagged[i].timestamp_ns = untagged[i].timestamp_ns + wall_shift;
        }
        /* #363: a refused pass leaves the oracle partly rewritten; every
         * number below would then be computed from a half-built raw side. */
        NEED(pgwt_tag_events(tagged, n_all, cats, n_cats) == 1,
             "pgwt_tag_events REFUSED the capture's %d records -- the raw "
             "oracle must be discarded, not measured", n_all);
        if (fail) return 2;
        int tagged_iw = 0;
        for (int i = 0; i < n_all; i++)
            if (tagged[i].flags & PGWT_EVENT_FLAG_IO_WORKER) tagged_iw++;
        NEED(tagged_iw > 0, "pgwt_tag_events flagged no io_worker record -- "
             "the raw path would have nothing to exclude");

        printf("\n=== run: cat_flag %s (%d records, %.1f s span) ===\n",
               run ? "RESOLVED per pid  [AFTER]" : "always 0  [BEFORE = pre-v6 writer]",
               c.records, wall_ms / 1000.0);
        printf("  writer excluded io_worker: %llu records, %llu ns\n",
               (unsigned long long)excl_ev, (unsigned long long)excl_ns);

        /* FOUR WINDOWS, because the divergence is not uniform in time: an
         * io_worker's single Activity:IoWorkerMain interval closes at capture
         * end, so the whole of it is bucketed into the LAST second. #376
         * measured +49.97% on a window including that second against +0.09%
         * one second earlier -- so a one-window measurement could report
         * either "huge" or "nothing" and be equally true. */
        struct { const char *name; uint64_t a, b; } win[4] = {
            { "whole capture      ", from, to },
            { "last 150 s (end)   ", to - 150 * ONE_SEC, to },
            { "last 150 s minus 1s", to - 151 * ONE_SEC, to - ONE_SEC },
            { "middle 150 s       ", from + 600 * ONE_SEC, from + 750 * ONE_SEC },
        };
        for (int k = 0; k < 4; k++) {
            uint64_t a = win[k].a, b = win[k].b;
            double wms = (double)(b - a) / 1e6;
            struct pgwt_filter f;
            memset(&f, 0, sizeof(f));
            struct pgwt_tm_result raw_tm, sum_tm;
            pgwt_compute_time_model(tagged, n_all, &f, a, b, wms, &raw_tm);
            pgwt_compute_time_model_from_summaries(sdir, a, b, &f, wms, &sum_tm);
            struct pgwt_heatmap_result raw_hm, sum_hm;
            pgwt_compute_heatmap(tagged, n_all, &f, a, b, 200, &raw_hm);
            pgwt_compute_heatmap_from_summaries(sdir, a, b, &f, 200, &sum_hm);
            NEED(raw_tm.idle_time_ms > 0 && sum_tm.db_time_ms > 0,
                 "window %s: one side is empty (raw idle %.3f, summary db "
                 "%.3f) -- an empty side makes every delta below meaningless",
                 win[k].name, raw_tm.idle_time_ms, sum_tm.db_time_ms);
            printf("  [%s] DB Time raw %14.2f  sum %14.2f  %+12.2f ms (%+8.4f%%)\n",
                   win[k].name, raw_tm.db_time_ms, sum_tm.db_time_ms,
                   sum_tm.db_time_ms - raw_tm.db_time_ms,
                   raw_tm.db_time_ms ? 100.0 * (sum_tm.db_time_ms - raw_tm.db_time_ms)
                                       / raw_tm.db_time_ms : 0.0);
            printf("  [%s] Idle    raw %14.2f  sum %14.2f  %+12.2f ms (%+8.4f%%)\n",
                   win[k].name, raw_tm.idle_time_ms, sum_tm.idle_time_ms,
                   sum_tm.idle_time_ms - raw_tm.idle_time_ms,
                   raw_tm.idle_time_ms ? 100.0 * (sum_tm.idle_time_ms - raw_tm.idle_time_ms)
                                    / raw_tm.idle_time_ms : 0.0);
            printf("  [%s] heatmap raw %14llu  sum %14llu  %+12lld    (%+8.4f%%)\n",
                   win[k].name,
                   (unsigned long long)raw_hm.total_events,
                   (unsigned long long)sum_hm.total_events,
                   (long long)sum_hm.total_events - (long long)raw_hm.total_events,
                   raw_hm.total_events
                     ? 100.0 * ((double)sum_hm.total_events - (double)raw_hm.total_events)
                       / (double)raw_hm.total_events : 0.0);
            free(raw_tm.rows); free(sum_tm.rows);
            free(raw_hm.grid); free(raw_hm.times);
            free(sum_hm.grid); free(sum_hm.times);
        }
        free(tagged);
    }
    free(untagged);
    free(all);
    printf("\n%s\n", fail ? "REFUSED (see above)" : "measurement complete");
    return fail ? 2 : 0;
}
