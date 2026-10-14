/* cpudecomp.c — DECOMPOSE the #408 summary-vs-raw CPU divergence on a real
 * capture. Measurement only: it changes nothing in src/ and asserts nothing.
 *
 * It regenerates summaries from the capture's own raw trace through the REAL
 * writer (the on-disk summaries are whatever version the capture's daemon
 * wrote), tags a second copy of the same events with the REAL pgwt_tag_events
 * against the capture's own backends.jsonl, and prints, per window:
 *   raw   CPU (running) / CPU (waiting for a core) / Idle / DB Time
 *   summ  CPU* / Idle / DB Time
 * plus the two candidate mechanisms, measured off the raw array directly:
 *   A. measured-CPU split    Sigma cpu_ns over CPU-class records
 *   B. non-command relabel   Sigma duration of records pgwt_tag_events moved
 *                            from old_event 0 to PGWT_WEI_NONCMD_CPU (raw
 *                            charges those to Idle; the writer never sees the
 *                            relabel, so they stay in its CPU class)
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "event_reader.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "cmdline.h"
#include "sampler.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define ONE_SEC 1000000000ULL

struct registry { struct pgwt_pid_cat *cats; int n; int n_io; };

static enum pgwt_backend_type bt_from_type_string(const char *type)
{
    for (int bt = 0; bt <= PGWT_BT_UNKNOWN; bt++)
        if (strcmp(type, pgwt_backend_type_name((enum pgwt_backend_type)bt)) == 0)
            return (enum pgwt_backend_type)bt;
    return PGWT_BT_UNKNOWN;
}
static int pid_cat_cmp(const void *a, const void *b)
{
    uint32_t pa = ((const struct pgwt_pid_cat *)a)->pid;
    uint32_t pb = ((const struct pgwt_pid_cat *)b)->pid;
    return (pa > pb) - (pa < pb);
}
static int registry_load_file(struct registry *reg, const char *dir)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int parsed = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = strstr(line, "\"pid\":");
        const char *t = strstr(line, "\"type\":\"");
        if (!p || !t) continue;
        long pid = strtol(p + 6, NULL, 10);
        char type[64]; t += 8; size_t i = 0;
        while (t[i] && t[i] != '"' && i < sizeof(type) - 1) { type[i] = t[i]; i++; }
        type[i] = '\0';
        if (pid <= 0 || i == 0) continue;
        struct pgwt_pid_cat *g = realloc(reg->cats, (size_t)(reg->n + 1) * sizeof(*g));
        if (!g) break;
        reg->cats = g;
        uint32_t flag = pgwt_backend_type_flag(bt_from_type_string(type));
        reg->cats[reg->n].pid = (uint32_t)pid;
        reg->cats[reg->n].flag = flag;
        reg->n++;
        if (flag & PGWT_EVENT_FLAG_IO_WORKER) reg->n_io++;
        parsed++;
    }
    fclose(f);
    if (reg->n > 1) qsort(reg->cats, (size_t)reg->n, sizeof(*reg->cats), pid_cat_cmp);
    return parsed > 0 ? parsed : -1;
}

static int ts_cmp(const void *a, const void *b)
{
    uint64_t ta = ((const struct pgwt_trace_event *)a)->timestamp_ns;
    uint64_t tb = ((const struct pgwt_trace_event *)b)->timestamp_ns;
    return (ta > tb) - (ta < tb);
}

static uint32_t cat_of(const struct registry *reg, uint32_t pid)
{
    for (int i = 0; i < reg->n; i++) if (reg->cats[i].pid == pid) return reg->cats[i].flag;
    return 0;
}

struct rec_ctx { int records; uint64_t min_sec, max_sec; };
static int rec_visitor(const struct pgwt_summary_accum *rec, void *v)
{
    struct rec_ctx *c = v;
    c->records++;
    if (c->min_sec == 0 || rec->second_wall_ns < c->min_sec) c->min_sec = rec->second_wall_ns;
    if (rec->second_wall_ns > c->max_sec) c->max_sec = rec->second_wall_ns;
    return 0;
}

/* Load every *.trace.lz4 plus current.trace in dir. */
static int load_events(const char *dir, struct pgwt_trace_event **out, int *samples_blocks)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    char files[64][800];
    int nf = 0;
    struct dirent *de;
    while ((de = readdir(d)) && nf < 64) {
        size_t l = strlen(de->d_name);
        int hour = (l > 10 && strcmp(de->d_name + l - 10, ".trace.lz4") == 0);
        int cur  = (strcmp(de->d_name, "current.trace") == 0);
        if (hour || cur) snprintf(files[nf++], sizeof(files[0]), "%s/%s", dir, de->d_name);
    }
    closedir(d);
    if (nf == 0) return -1;
    for (int i = 0; i < nf; i++)
        for (int j = i + 1; j < nf; j++)
            if (strcmp(files[j], files[i]) < 0) {
                char t[800]; snprintf(t, sizeof(t), "%s", files[i]);
                snprintf(files[i], sizeof(files[i]), "%s", files[j]);
                snprintf(files[j], sizeof(files[j]), "%s", t);
            }
    int cap = 0, n = 0;
    *samples_blocks = 0;
    for (int fi = 0; fi < nf; fi++) {
        struct pgwt_event_reader r; memset(&r, 0, sizeof(r));
        if (pgwt_reader_open(&r, files[fi]) != 0) continue;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info bi;
            if (pgwt_reader_block_info(&r, b, &bi) == 0) cap += (int)bi.num_events;
        }
        pgwt_reader_close(&r);
    }
    struct pgwt_trace_event *ev = calloc((size_t)cap + 1, sizeof(*ev));
    if (!ev) return -1;
    for (int fi = 0; fi < nf; fi++) {
        struct pgwt_event_reader r; memset(&r, 0, sizeof(r));
        if (pgwt_reader_open(&r, files[fi]) != 0) continue;
        fprintf(stderr, "    %s: %d blocks\n", files[fi], r.num_blocks);
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info bi;
            int got = pgwt_reader_decode_block_info(&r, b, ev + n, cap - n, &bi);
            if (got < 0) continue;
            if (bi.block_type == PGWT_BLOCK_SAMPLES) (*samples_blocks)++;
            n += got;
        }
        pgwt_reader_close(&r);
    }
    qsort(ev, (size_t)n, sizeof(*ev), ts_cmp);
    *out = ev;
    return n;
}

static uint64_t overlap_ns(const struct pgwt_trace_event *ev, uint64_t from, uint64_t to)
{
    uint64_t end = ev->timestamp_ns;
    uint64_t start = end >= ev->duration_ns ? end - ev->duration_ns : 0;
    if (start < from) start = from;
    if (end > to) end = to;
    return end > start ? end - start : 0;
}

static double row_ms(const struct pgwt_tm_result *r, const char *name)
{
    for (int i = 0; i < r->num_rows; i++)
        if (strcmp(r->rows[i].name, name) == 0) return r->rows[i].time_ms;
    return -1.0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <trace dir>\n", argv[0]); return 2; }
    const char *dir = argv[1];
    struct registry reg; memset(&reg, 0, sizeof(reg));
    int entries = registry_load_file(&reg, dir);
    printf("registry: %d entries, %d io_workers\n", entries, reg.n_io);
    if (entries <= 0) return 1;

    struct pgwt_trace_event *ev = NULL;
    int samples_blocks = 0;
    int n = load_events(dir, &ev, &samples_blocks);
    printf("raw: %d records, %d SAMPLES blocks\n", n, samples_blocks);
    if (n <= 0) return 1;

    struct timespec tsp; clock_gettime(CLOCK_MONOTONIC, &tsp);
    uint64_t rebase = (((uint64_t)tsp.tv_sec * ONE_SEC + (uint64_t)tsp.tv_nsec) / ONE_SEC) * ONE_SEC;
    uint64_t first_sec_mono = (ev[0].timestamp_ns / ONE_SEC) * ONE_SEC;
    for (int i = 0; i < n; i++)
        ev[i].timestamp_ns = ev[i].timestamp_ns - first_sec_mono + rebase;

    /* The OFFLINE producer's verdict on the non-command relabel (summary v7,
     * #408): the writer is TOLD the effective id, so this harness must
     * resolve one, and the production resolver available offline is
     * pgwt_tag_events -- the same pass src/server.c runs over every raw
     * window. Run it on a THROWAWAY copy here, before the push, so the array
     * that goes into the writer still carries the daemon's raw ids. */
    uint32_t *eff = calloc((size_t)n, sizeof(*eff));
    {
        struct pgwt_trace_event *probe = calloc((size_t)n + 1, sizeof(*probe));
        memcpy(probe, ev, (size_t)n * sizeof(*probe));
        if (!pgwt_tag_events(probe, n, reg.cats, reg.n)) {
            printf("pgwt_tag_events REFUSED on the producer probe\n");
            return 1;
        }
        int relabelled = 0;
        for (int i = 0; i < n; i++) {
            eff[i] = probe[i].old_event;
            if (eff[i] != ev[i].old_event) relabelled++;
        }
        printf("producer: pgwt_tag_events relabelled %d of %d records\n",
               relabelled, n);
        free(probe);
    }

    const char *outbase = getenv("CPUDECOMP_OUT");
    char sdir[800];
    snprintf(sdir, sizeof(sdir), "%s/summaries", outbase ? outbase : "/tmp");
    char cmd[900]; snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", sdir, sdir);
    if (system(cmd) != 0) return 1;

    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (pgwt_summary_writer_init(w, sdir, 24, NULL) != 0) return 1;
    long pushed = 0;
    for (int i = 0; i < n; i++) {
        if (pgwt_summary_push_event(w, &ev[i], cat_of(&reg, ev[i].pid),
                                    eff[i]) != 0) break;
        pushed++;
    }
    pgwt_summary_flush(w);
    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
    printf("summary: pushed %ld of %d into %s (format v%d)\n", pushed, n, sdir,
           PGWT_SUMMARY_VERSION);

    struct rec_ctx rc; memset(&rc, 0, sizeof(rc));
    int visited = pgwt_visit_summaries(sdir, 0, 0, rec_visitor, &rc);
    printf("summary: visit=%d records=%d span=%.0f s\n", visited, rc.records,
           (double)(rc.max_sec + ONE_SEC - rc.min_sec) / 1e9);
    if (rc.records == 0) return 1;

    struct pgwt_trace_event *evw = calloc((size_t)n + 1, sizeof(*evw));
    memcpy(evw, ev, (size_t)n * sizeof(*evw));
    uint64_t shift = rc.min_sec - (ev[0].timestamp_ns / ONE_SEC) * ONE_SEC;
    for (int i = 0; i < n; i++) evw[i].timestamp_ns += shift;
    /* mechanism B: how much time pgwt_tag_events moves out of the CPU class */
    uint32_t *pre = calloc((size_t)n, sizeof(*pre));
    for (int i = 0; i < n; i++) pre[i] = evw[i].old_event;
    if (!pgwt_tag_events(evw, n, reg.cats, reg.n)) {
        printf("pgwt_tag_events REFUSED\n"); return 1;
    }

    uint64_t end = rc.max_sec + ONE_SEC;
    struct { const char *label; uint64_t from, to; } wins[] = {
        { "900 s ending at capture end", end > 900 * ONE_SEC ? end - 900 * ONE_SEC : rc.min_sec, end },
        { "900 s ending 1 s short",      end > 901 * ONE_SEC ? end - 901 * ONE_SEC : rc.min_sec, end - ONE_SEC },
        { "full span",                   rc.min_sec, end },
    };

    for (size_t k = 0; k < sizeof(wins)/sizeof(wins[0]); k++) {
        uint64_t from = wins[k].from, to = wins[k].to;
        struct pgwt_filter f; memset(&f, 0, sizeof(f));
        double wall_ms = (double)(to - from) / 1e6;
        struct pgwt_tm_result raw, sum;
        memset(&raw, 0, sizeof(raw)); memset(&sum, 0, sizeof(sum));
        pgwt_compute_time_model(evw, n, &f, from, to, wall_ms, &raw);
        pgwt_compute_time_model_from_summaries(sdir, from, to, &f, wall_ms, &sum);

        /* mechanisms, measured off the raw array */
        double noncmd_ns = 0, cpu_meas_ns = 0, cpu_gap_ns = 0, unknown_gap_ns = 0;
        int meas_recs = 0;
        for (int i = 0; i < n; i++) {
            uint64_t ov = overlap_ns(&evw[i], from, to);
            if (!ov) continue;
            double frac = evw[i].duration_ns ? (double)ov / (double)evw[i].duration_ns : 0;
            if (evw[i].flags & PGWT_EVENT_FLAG_IO_WORKER) continue;
            if (pre[i] == 0 && evw[i].old_event == PGWT_WEI_NONCMD_CPU)
                noncmd_ns += (double)ov;
            if (evw[i].old_event == 0) {
                cpu_gap_ns += (double)ov;
                if (evw[i].cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                    double m = (double)evw[i].cpu_ns * frac;
                    if (m > (double)ov) m = (double)ov;
                    cpu_meas_ns += m; meas_recs++;
                } else unknown_gap_ns += (double)ov;
            }
        }
        printf("\n=== %s (%.0f s) ===\n", wins[k].label, wall_ms / 1000.0);
        printf("  raw   DB %15.3f  Idle %15.3f  CPU(run) %13.3f  CPU(wait) %13.3f  CPU* %13.3f\n",
               raw.db_time_ms, raw.idle_time_ms, row_ms(&raw, "CPU (running)"),
               row_ms(&raw, "CPU (waiting for a core)"), row_ms(&raw, "CPU*"));
        printf("  summ  DB %15.3f  Idle %15.3f  CPU(run) %13.3f  CPU(wait) %13.3f  CPU* %13.3f\n",
               sum.db_time_ms, sum.idle_time_ms, row_ms(&sum, "CPU (running)"),
               row_ms(&sum, "CPU (waiting for a core)"), row_ms(&sum, "CPU*"));
        printf("  delta DB %+15.3f (%+.3f%%)  Idle %+15.3f (%+.3f%%)\n",
               sum.db_time_ms - raw.db_time_ms,
               raw.db_time_ms ? (sum.db_time_ms - raw.db_time_ms)/raw.db_time_ms*100 : 0,
               sum.idle_time_ms - raw.idle_time_ms,
               raw.idle_time_ms ? (sum.idle_time_ms - raw.idle_time_ms)/raw.idle_time_ms*100 : 0);
        printf("  raw has_measured_cpu=%d offcpu_ms=%.3f cpu_ms=%.3f clamped=%.3f waitgap=%.3f\n",
               raw.has_measured_cpu, raw.offcpu_ms, raw.cpu_ms, raw.cpu_clamped_ms, raw.wait_gap_cpu_ms);
        printf("  summ has_measured_cpu=%d offcpu_ms=%.3f cpu_ms=%.3f\n",
               sum.has_measured_cpu, sum.offcpu_ms, sum.cpu_ms);
        printf("  MECHANISM A (measured split): gap %.3f ms = measured %.3f + unknown %.3f, %d measured records\n",
               cpu_gap_ns/1e6, cpu_meas_ns/1e6, unknown_gap_ns/1e6, meas_recs);
        printf("  MECHANISM B (non-command relabel, raw moves to Idle): %.3f ms\n", noncmd_ns/1e6);
        free(raw.rows); free(sum.rows);
    }
    free(pre); free(ev); free(evw);
    return 0;
}
