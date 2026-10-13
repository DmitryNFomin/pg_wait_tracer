/* offcpu_decompose.c — what is actually INSIDE the Off-CPU* residual.
 *
 * WHY. On the raw/exact path Off-CPU* is not measured, it is the residual
 * `DB Time - CPU* - sum(waits)` (src/compute.c, the `resid` block). Because
 * DB Time and every wait total are timestamp-exact, that residual reduces
 * algebraically to ONE thing:
 *
 *     Off-CPU* == sum over CPU-class (old_event == 0) records inside DB Time
 *                 of ( clipped_duration - min(cpu_ns * window_frac, dur) )
 *
 * i.e. the part of each on-CPU GAP that the measured se.sum_exec_runtime
 * delta did not account for. A `time_model_conserves` / `offcpu_cap`
 * failure therefore says nothing about waits: it says the gaps got wider
 * than their measured CPU. This tool prints that sum the long way, split by
 * the things that distinguish the candidate causes:
 *
 *   - cpu_ns UNKNOWN      -> contributes 0 (whole gap booked as CPU*)
 *   - cpu_ns == 0         -> contributes the WHOLE gap. This is the
 *                            documented tick-quantisation leak: sum_exec_runtime
 *                            is only current at a 1 ms tick or a context
 *                            switch, so a sub-ms gap can read a delta of 0
 *                            while it really did run. Dominated by SHORT gaps
 *                            and scales with the NUMBER of gaps, not with
 *                            real runqueue pressure.
 *   - 0 < cpu_ns < dur    -> contributes the deficit. Real runqueue delay
 *                            looks like this, and so does a long gap whose
 *                            first sub-ms slice leaked.
 *   - cpu_ns >= dur       -> contributes 0 and is clamped (cpu_clamped_ms).
 *
 * and additionally by gap-duration bucket, by pid, and by the wait event the
 * gap transitions INTO (`new_event`) — the "which wait classes border the
 * gap" question — plus a per-sub-window series so growth with capture offset
 * is visible rather than inferred.
 *
 * A TOOL THAT CANNOT SEE MUST REFUSE. No trace, no events in the window, or
 * no record carrying a measured cpu_ns at all (every record UNKNOWN: a v2
 * file, the sampled tier, a box with no BTF) exits 2 and says so — it is
 * never reported as "residual 0%". A window whose official has_measured_cpu
 * is false likewise refuses: compute.c reports offcpu_ms = 0 there by
 * definition, and printing a 0% decomposition of it would read as a clean
 * result. The decomposition is also RECONCILED against
 * pgwt_compute_time_model's own offcpu_ms on every window, and a mismatch
 * beyond 1e-6 relative is a hard error (exit 3): the whole point is to
 * explain the product's number, not a number of this tool's own.
 *
 * Built with -DPGWT_SERVER (no BPF), like cross_validate / wait_cpu_canary.
 *
 * Usage: offcpu_decompose <trace_dir> [--window-s N] [--step-s N]
 *                         [--from-offset S] [--to-offset S] [--top N]
 */
#include "event_reader.h"
#include "event_writer.h"
#include "event_sort.h"
#include "compute.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define MAX_TOP 4096

struct top_acc {
    uint64_t key;
    double   resid_ns;
    double   dur_ns;
    uint64_t n;
};

static int top_find(struct top_acc *t, int *n, uint64_t key)
{
    for (int i = 0; i < *n; i++)
        if (t[i].key == key)
            return i;
    if (*n >= MAX_TOP)
        return -1;
    t[*n].key = key;
    return (*n)++;
}

static int top_cmp(const void *a, const void *b)
{
    double x = ((const struct top_acc *)a)->resid_ns;
    double y = ((const struct top_acc *)b)->resid_ns;
    return (x < y) - (x > y);
}

/* The duration buckets, in ns. Chosen around the 1 ms scheduler tick: the
 * tick-quantisation story lives strictly below it, real runqueue delay does
 * not care where the boundary is. */
static const uint64_t bucket_hi[] = {
    10000ULL, 100000ULL, 1000000ULL, 10000000ULL, 100000000ULL, UINT64_MAX
};
static const char *bucket_name[] = {
    "<10us", "10-100us", "100us-1ms", "1-10ms", "10-100ms", ">100ms"
};
#define NBUCKET (int)(sizeof(bucket_hi) / sizeof(bucket_hi[0]))

/* Mirrors src/compute.c event_window_ns()/event_window_frac(): a straddler
 * contributes only its overlap. Kept as a local copy because those are
 * static in compute.c; tests/test_offcpu_decompose.c pins the two against
 * each other on a synthetic straddler. */
uint64_t ocd_window_ns(const struct pgwt_trace_event *ev,
                       uint64_t from_ns, uint64_t to_ns)
{
    uint64_t end = ev->timestamp_ns;
    uint64_t start = end > ev->duration_ns ? end - ev->duration_ns : 0;
    if (from_ns && start < from_ns) start = from_ns;
    if (to_ns && end > to_ns)       end = to_ns;
    return end > start ? end - start : 0;
}

/* The residual contribution of ONE record, and its reason code. Pure, so
 * tests/test_offcpu_decompose.c can pin every branch without a trace. */
enum ocd_reason {
    OCD_NOT_GAP = 0,     /* not a CPU-class in-DB-Time record */
    OCD_UNKNOWN,         /* cpu_ns == PGWT_CPU_NS_UNKNOWN: whole gap is CPU* */
    OCD_ZERO_CPU,        /* measured 0: whole gap lands in the residual */
    OCD_PARTIAL,         /* 0 < cpu < dur */
    OCD_CLAMPED,         /* cpu >= dur */
};

enum ocd_reason ocd_classify(const struct pgwt_trace_event *ev,
                             uint64_t from_ns, uint64_t to_ns,
                             double *resid_ns, double *dur_out,
                             double *clamp_ns)
{
    *resid_ns = 0.0; *dur_out = 0.0; *clamp_ns = 0.0;
    if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
        return OCD_NOT_GAP;
    if (pgwt_is_idle_event(ev->old_event))
        return OCD_NOT_GAP;
    if (pgwt_wait_class_index(ev->old_event) != PGWT_CLASS_CPU)
        return OCD_NOT_GAP;

    uint64_t clip = ocd_window_ns(ev, from_ns, to_ns);
    /* No overlap with the window at all: this record is not part of THIS
     * window's residual and must not inflate its gap counts either. (It
     * contributes 0 ns either way, so the residual totals are unaffected --
     * the counts are what this guard protects, and without it every window
     * reported the whole capture's gap count.) */
    if (clip == 0)
        return OCD_NOT_GAP;
    double dur = (double)clip;
    *dur_out = dur;
    if (ev->cpu_ns == PGWT_CPU_NS_UNKNOWN)
        return OCD_UNKNOWN;

    double frac = ev->duration_ns ? (double)clip / (double)ev->duration_ns : 1.0;
    double m = (double)ev->cpu_ns * frac;
    if (m > dur) {
        *clamp_ns = m - dur;
        return OCD_CLAMPED;
    }
    *resid_ns = dur - m;
    if (ev->cpu_ns == 0)
        return OCD_ZERO_CPU;
    return OCD_PARTIAL;
}

#ifndef OCD_NO_MAIN

struct ocd_window_stats {
    double resid_by_reason[5];
    uint64_t n_by_reason[5];
    double resid_by_bucket[NBUCKET];
    uint64_t n_by_bucket[NBUCKET];
    double gap_dur_ns;
    uint64_t n_gaps;
    double clamp_ns;
    int any_measured;
};

static void analyse(const struct pgwt_trace_event *ev, int count,
                    uint64_t from_ns, uint64_t to_ns,
                    struct ocd_window_stats *st,
                    struct top_acc *by_pid, int *n_pid,
                    struct top_acc *by_next, int *n_next)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < count; i++) {
        double resid = 0, dur = 0, clamp = 0;
        enum ocd_reason r = ocd_classify(&ev[i], from_ns, to_ns,
                                         &resid, &dur, &clamp);
        if (r == OCD_NOT_GAP)
            continue;
        if (r != OCD_UNKNOWN)
            st->any_measured = 1;
        st->resid_by_reason[r] += resid;
        st->n_by_reason[r]++;
        st->gap_dur_ns += dur;
        st->n_gaps++;
        st->clamp_ns += clamp;
        int b = 0;
        while (b < NBUCKET - 1 && (uint64_t)dur >= bucket_hi[b]) b++;
        st->resid_by_bucket[b] += resid;
        st->n_by_bucket[b]++;
        if (by_pid) {
            int k = top_find(by_pid, n_pid, ev[i].pid);
            if (k >= 0) { by_pid[k].resid_ns += resid; by_pid[k].dur_ns += dur;
                          by_pid[k].n++; }
        }
        if (by_next) {
            int k = top_find(by_next, n_next, ev[i].new_event);
            if (k >= 0) { by_next[k].resid_ns += resid; by_next[k].dur_ns += dur;
                          by_next[k].n++; }
        }
    }
}

static double sum5(const double *v) { return v[0]+v[1]+v[2]+v[3]+v[4]; }

int main(int argc, char **argv)
{
    const char *dir = NULL;
    double window_s = 60.0, step_s = 60.0, from_off = -1, to_off = -1;
    int topn = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--window-s") && i + 1 < argc) window_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--step-s") && i + 1 < argc) step_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--from-offset") && i + 1 < argc) from_off = atof(argv[++i]);
        else if (!strcmp(argv[i], "--to-offset") && i + 1 < argc) to_off = atof(argv[++i]);
        else if (!strcmp(argv[i], "--top") && i + 1 < argc) topn = atoi(argv[++i]);
        else if (argv[i][0] != '-') dir = argv[i];
    }
    if (!dir) { fprintf(stderr, "usage: offcpu_decompose <trace_dir> [...]\n"); return 1; }

    char path[1024];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0) {
        fprintf(stderr, "REFUSE: cannot open %s\n", path);
        return 2;
    }
    int major = (int)(r.header.pg_version / 10000);
    if (major == 0) major = (int)r.header.pg_version;
    pgwt_init_event_names(major);
    (void)pgwt_load_names_json(dir);
    printf("trace=%s pg_major=%d blocks=%d idle_timeout_mask=0x%x\n",
           path, major, r.num_blocks, pgwt_idle_rule_timeout_mask());

    /* Load every block. */
    int cap = 1 << 20, count = 0;
    struct pgwt_trace_event *ev = malloc((size_t)cap * sizeof(*ev));
    struct pgwt_trace_event *blk = malloc(200000 * sizeof(*blk));
    if (!ev || !blk) { fprintf(stderr, "REFUSE: oom\n"); return 2; }
    int n_samples_blocks = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info info;
        int n = pgwt_reader_decode_block_info(&r, b, blk, 200000, &info);
        if (n <= 0) continue;
        if (info.block_type != PGWT_BLOCK_TRANSITIONS) n_samples_blocks++;
        while (count + n > cap) {
            cap *= 2;
            struct pgwt_trace_event *g = realloc(ev, (size_t)cap * sizeof(*ev));
            if (!g) { fprintf(stderr, "REFUSE: oom at %d\n", cap); return 2; }
            ev = g;
        }
        memcpy(ev + count, blk, (size_t)n * sizeof(*ev));
        count += n;
    }
    free(blk);
    if (count <= 0) { fprintf(stderr, "REFUSE: no events in %s\n", path); return 2; }

    /* Same order and same tagging the server applies before any raw
     * estimator runs (server_load_events_fi_mode). */
    if (pgwt_stable_sort_events(ev, count, NULL) != 0) {
        fprintf(stderr, "REFUSE: sort failed\n"); return 2;
    }
    /* pid categories from backends.jsonl, mapped exactly as
     * server.c bm_type_to_cat_flag does. Without this io_worker records
     * would be counted inside DB Time. */
    struct pgwt_pid_cat *cats = NULL; int n_cats = 0, cap_cats = 0;
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *bf = fopen(path, "r");
    if (bf) {
        char line[1024];
        while (fgets(line, sizeof(line), bf)) {
            unsigned pid = 0; char type[64] = "";
            const char *p = strstr(line, "\"pid\":");
            const char *t = strstr(line, "\"type\":\"");
            if (!p || !t) continue;
            pid = (unsigned)strtoul(p + 6, NULL, 10);
            sscanf(t + 8, "%63[^\"]", type);
            uint32_t flag = 0;
            if (!strcmp(type, "io_worker")) flag = PGWT_EVENT_FLAG_IO_WORKER;
            else if (!strcmp(type, "autovac_worker")) flag = PGWT_EVENT_FLAG_MAINT;
            else if (!strcmp(type, "client") || !strcmp(type, "parallel_worker")
                     || !strcmp(type, "unknown")) flag = 0;
            else flag = PGWT_EVENT_FLAG_BACKGROUND;
            int dup = 0;
            for (int i = 0; i < n_cats; i++) if (cats[i].pid == pid) { dup = 1; break; }
            if (dup) continue;
            if (n_cats == cap_cats) {
                cap_cats = cap_cats ? cap_cats * 2 : 256;
                cats = realloc(cats, (size_t)cap_cats * sizeof(*cats));
            }
            cats[n_cats].pid = pid; cats[n_cats].flag = flag; n_cats++;
        }
        fclose(bf);
    }
    if (!pgwt_tag_events(ev, count, cats, n_cats)) {
        fprintf(stderr, "REFUSE: pgwt_tag_events refused -- array must be discarded\n");
        return 2;
    }

    uint64_t t_first = ev[0].timestamp_ns, t_last = ev[count - 1].timestamp_ns;
    printf("events=%d samples_blocks=%d span=%.1fs pid_cats=%d\n",
           count, n_samples_blocks, (double)(t_last - t_first) / 1e9, n_cats);

    double cap_s = (double)(t_last - t_first) / 1e9;
    double lo = from_off >= 0 ? from_off : 0.0;
    double hi = to_off >= 0 ? to_off : cap_s;

    printf("\n# series: trailing %.0fs windows, step %.0fs. "
           "offcpu_pct is the gate's own quantity.\n", window_s, step_s);
    printf("%8s %12s %10s %9s %9s %9s %9s %9s %8s\n",
           "end_off", "db_time_ms", "offcpu_ms", "offcpu%", "zero_cpu%",
           "partial%", "n_gaps", "gap_ms", "clamp_ms");

    int refused_all_unknown = 1, n_win = 0, worst_i = -1, n_win_with_data = 0;
    double worst_pct = -1.0, worst_end = 0;
    for (double end = lo + window_s; end <= hi + 1e-9; end += step_s) {
        uint64_t to_ns = t_first + (uint64_t)(end * 1e9);
        uint64_t from_ns = t_first + (uint64_t)((end - window_s) * 1e9);
        struct pgwt_filter f; memset(&f, 0, sizeof(f));
        struct pgwt_tm_result tm; memset(&tm, 0, sizeof(tm));
        pgwt_compute_time_model(ev, count, &f, from_ns, to_ns,
                                window_s * 1000.0, &tm);
        struct ocd_window_stats st;
        analyse(ev, count, from_ns, to_ns, &st, NULL, NULL, NULL, NULL);
        double mine = sum5(st.resid_by_reason);
        double off_ms = tm.offcpu_ms;
        if (tm.has_measured_cpu) {
            refused_all_unknown = 0;
            double denom = off_ms > 1e-9 ? off_ms : 1.0;
            if (fabs(mine / 1e6 - off_ms) / denom > 1e-6) {
                fprintf(stderr,
                        "MISMATCH at end_off=%.1f: decomposition %.6f ms vs "
                        "compute.c offcpu_ms %.6f ms -- the decomposition does "
                        "not explain the product's number; refusing.\n",
                        end, mine / 1e6, off_ms);
                return 3;
            }
        }
        double pct = tm.db_time_ms > 0 ? off_ms / tm.db_time_ms * 100.0 : 0.0;
        printf("%8.1f %12.1f %10.2f %9.3f %9.3f %9.3f %9llu %9.1f %8.3f\n",
               end, tm.db_time_ms, off_ms, pct,
               off_ms > 0 ? st.resid_by_reason[OCD_ZERO_CPU] / 1e6 / off_ms * 100 : 0,
               off_ms > 0 ? st.resid_by_reason[OCD_PARTIAL] / 1e6 / off_ms * 100 : 0,
               (unsigned long long)st.n_gaps, st.gap_dur_ns / 1e6,
               st.clamp_ns / 1e6);
        /* A window with no DB Time is NOT a 0% window: compute.c reports
         * offcpu_ms = 0 there, and it even reports has_measured_cpu = TRUE
         * (its has_measured_cpu is set from any record carrying a cpu_ns,
         * including records whose in-window fraction is zero), so "no data"
         * is indistinguishable from "clean" on the response alone. Counted
         * separately and refused below if EVERY window looks like this --
         * the first version of this tool printed `offcpu 0.000%` and exited
         * 0 for a window past the end of the capture. */
        if (tm.db_time_ms > 0 && st.n_gaps > 0)
            n_win_with_data++;
        if (tm.has_measured_cpu && tm.db_time_ms > 0 && pct > worst_pct) {
            worst_pct = pct; worst_end = end; worst_i = n_win;
        }
        n_win++;
        free(tm.rows);
    }
    if (refused_all_unknown) {
        fprintf(stderr, "REFUSE: no window carried a measured cpu_ns "
                        "(has_measured_cpu false everywhere) -- the residual "
                        "is UNAVAILABLE here, not zero.\n");
        return 2;
    }
    if (n_win_with_data == 0 || worst_i < 0) {
        fprintf(stderr, "REFUSE: no window in [%.1f, %.1f] contained any "
                        "DB Time or any on-CPU gap (%d windows examined) -- "
                        "the residual is UNMEASURED here, not 0%%.\n",
                lo, hi, n_win);
        return 2;
    }

    /* Full detail for the worst window. */
    uint64_t to_ns = t_first + (uint64_t)(worst_end * 1e9);
    uint64_t from_ns = t_first + (uint64_t)((worst_end - window_s) * 1e9);
    struct ocd_window_stats st;
    struct top_acc *by_pid = calloc(MAX_TOP, sizeof(*by_pid));
    struct top_acc *by_next = calloc(MAX_TOP, sizeof(*by_next));
    int n_pid = 0, n_next = 0;
    analyse(ev, count, from_ns, to_ns, &st, by_pid, &n_pid, by_next, &n_next);
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm; memset(&tm, 0, sizeof(tm));
    pgwt_compute_time_model(ev, count, &f, from_ns, to_ns, window_s * 1000.0, &tm);

    printf("\n# worst window: end_off=%.1fs offcpu=%.3f%% of db_time "
           "(db_time=%.1f ms, offcpu=%.2f ms, wait_gap_cpu=%.2f ms)\n",
           worst_end, worst_pct, tm.db_time_ms, tm.offcpu_ms, tm.wait_gap_cpu_ms);
    static const char *rname[5] = { "not_gap", "cpu_ns UNKNOWN",
                                    "cpu_ns == 0", "0<cpu<dur", "cpu>=dur(clamped)" };
    printf("  by reason:\n");
    for (int i = 1; i < 5; i++)
        printf("    %-20s resid=%10.2f ms (%6.2f%% of resid)  n=%llu\n",
               rname[i], st.resid_by_reason[i] / 1e6,
               tm.offcpu_ms > 0 ? st.resid_by_reason[i] / 1e6 / tm.offcpu_ms * 100 : 0,
               (unsigned long long)st.n_by_reason[i]);
    printf("  by gap duration:\n");
    for (int b = 0; b < NBUCKET; b++)
        printf("    %-12s resid=%10.2f ms (%6.2f%%)  n=%llu\n",
               bucket_name[b], st.resid_by_bucket[b] / 1e6,
               tm.offcpu_ms > 0 ? st.resid_by_bucket[b] / 1e6 / tm.offcpu_ms * 100 : 0,
               (unsigned long long)st.n_by_bucket[b]);
    qsort(by_pid, (size_t)n_pid, sizeof(*by_pid), top_cmp);
    printf("  top pids by residual:\n");
    for (int i = 0; i < n_pid && i < topn; i++)
        printf("    pid %-8llu resid=%10.2f ms gap=%10.2f ms n=%llu\n",
               (unsigned long long)by_pid[i].key, by_pid[i].resid_ns / 1e6,
               by_pid[i].dur_ns / 1e6, (unsigned long long)by_pid[i].n);
    qsort(by_next, (size_t)n_next, sizeof(*by_next), top_cmp);
    printf("  top bordering wait (the event the gap ENTERS):\n");
    for (int i = 0; i < n_next && i < topn; i++)
        printf("    %-28s resid=%10.2f ms gap=%10.2f ms n=%llu\n",
               pgwt_event_name((uint32_t)by_next[i].key),
               by_next[i].resid_ns / 1e6, by_next[i].dur_ns / 1e6,
               (unsigned long long)by_next[i].n);
    free(tm.rows);
    free(by_pid); free(by_next); free(cats); free(ev);
    pgwt_reader_close(&r);
    return 0;
}
#endif /* OCD_NO_MAIN */
