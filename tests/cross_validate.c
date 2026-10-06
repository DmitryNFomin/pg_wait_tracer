/* cross_validate.c — A4 cross-validation: sampled estimators vs exact data.
 *
 * Reads a tiered-mode trace directory that contains BOTH sampled blocks
 * (always-on) and transition blocks (escalation window), and compares the two
 * fidelities over the time window where they OVERLAP (the escalation window).
 * The sampler runs through escalation, so within that window we have:
 *   - SAMPLES blocks  → ASH estimator: each sample of event E contributes
 *                       sample_period_ns to E's time.
 *   - TRANSITIONS     → exact: each interval contributes its duration_ns.
 *
 * Output: per-wait-event time share both ways, top-5 overlap, max share
 * disagreement, and per-class (time_model) agreement. Exits 0 if the chosen
 * tolerance is met. This both validates the sampled estimators and lets us
 * pick the default --sample-rate empirically.
 *
 * Built with -DPGWT_SERVER (no BPF) so it runs anywhere the server builds.
 *
 * Usage: cross_validate <trace_dir> [--tolerance PCT] [--min-share PCT]
 */
#include "event_reader.h"
#include "event_writer.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_EVENTS 65536
#define MAX_DISTINCT 1024

struct evt_acc {
    uint32_t event_id;
    double   sampled_ns;   /* ASH estimate */
    double   exact_ns;     /* sum of durations */
    int      used;
};

static struct evt_acc *acc_find(struct evt_acc *t, uint32_t id)
{
    uint32_t h = (id * 2654435761u) % MAX_DISTINCT;
    while (t[h].used && t[h].event_id != id)
        h = (h + 1) % MAX_DISTINCT;
    if (!t[h].used) { t[h].used = 1; t[h].event_id = id; }
    return &t[h];
}

/* ── T2: exact-tier command gate (mirror of pgwt_tag_events) ──────────
 * The sampled tier records client we==0 only while a command is open, so
 * the exact side must classify its we==0 intervals the same way or the
 * comparison (and AAS across tier switches) would diverge by construction
 * on chatty workloads. Per-pid sweep over the CMD_START/CMD_END markers;
 * majority rule per interval. Pids with no CMD markers keep all we==0 as
 * CPU (background processes / legacy traces). */
#define CMD_HT 1024
struct cmd_state {
    uint32_t pid;       /* 0 = empty */
    int      seen_cmd, cmd_open;
    uint64_t anchor, banked;
};

static struct cmd_state *cmd_get(struct cmd_state *ht, uint32_t pid)
{
    uint32_t h = (pid * 2654435761u) % CMD_HT;
    while (ht[h].pid && ht[h].pid != pid)
        h = (h + 1) % CMD_HT;
    if (!ht[h].pid) ht[h].pid = pid;
    return &ht[h];
}

/* Returns 1 if the we==0 interval ending at t1 (mono) is majority
 * in-command; also consumes the banked open time. */
static int cmd_interval_open(struct cmd_state *st, uint64_t t1, uint64_t dur)
{
    if (!st->seen_cmd)
        return 1;   /* no gate info: legacy behavior (CPU) */
    uint64_t t0 = t1 > dur ? t1 - dur : 0;
    uint64_t open_ns = st->banked;
    if (st->cmd_open) {
        uint64_t a = st->anchor > t0 ? st->anchor : t0;
        if (t1 > a)
            open_ns += t1 - a;
    }
    st->banked = 0;
    if (st->cmd_open)
        st->anchor = t1;
    if (dur == 0)
        return st->cmd_open;
    return open_ns * 2 >= dur;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace_dir> [--tolerance PCT] "
                "[--min-share PCT]\n", argv[0]);
        return 2;
    }
    const char *trace_dir = argv[1];
    double tolerance = 10.0;   /* percentage points */
    double min_share = 2.0;    /* ignore events below this exact share */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--tolerance") && i + 1 < argc)
            tolerance = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-share") && i + 1 < argc)
            min_share = atof(argv[++i]);
    }

    /* Decode this trace with the names it was WRITTEN with.
     *
     * This tool never initialised the event-name tables at all, so it ran on
     * wait_event.c's file-scope defaults (PG18). That was a cosmetic mislabel
     * while the tables were only used for display; since 2026-10-06 the
     * idle/pacing classification is DERIVED from the resolved names, so a
     * PG13 or PG16 trace was being classified with PG18 semantics -- and this
     * is the tool whose +/-10pp verdict is used to justify the shipped sample
     * rate. The sidecar carries the writing side'''s mapping and its pg_version,
     * and loading it re-selects the version tables and rebuilds the pacing
     * mask (src/wait_event.c pgwt_load_names_json).
     *
     * No sidecar: say so. The numbers are then produced under a PG18
     * assumption and a reader needs to know that rather than infer it. */
    if (pgwt_load_names_json(trace_dir) != 0) {
        pgwt_init_event_names(18);
        fprintf(stderr, "WARN: %s has no wait_event_names.json sidecar -- "
                "decoding and idle classification assume PG18; shares for a "
                "trace from another major may be mislabelled\n", trace_dir);
    }

    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(trace_dir, files, 256);
    if (nfiles <= 0) {
        fprintf(stderr, "ERROR: no trace files in %s\n", trace_dir);
        return 1;
    }

    /* First pass over all blocks: find the overlapping wall window where BOTH
     * sample blocks and transition blocks exist. */
    uint64_t tr_first = UINT64_MAX, tr_last = 0;
    uint64_t sm_first = UINT64_MAX, sm_last = 0;

    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0)
            continue;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info info;
            if (pgwt_reader_block_info(&r, b, &info) != 0)
                continue;
            uint64_t fw = pgwt_reader_mono_to_wall(&r, info.first_timestamp_ns);
            uint64_t lw = pgwt_reader_mono_to_wall(&r, info.last_timestamp_ns);
            if (info.block_type == PGWT_BLOCK_TRANSITIONS) {
                if (fw < tr_first) tr_first = fw;
                if (lw > tr_last)  tr_last = lw;
            } else if (info.block_type == PGWT_BLOCK_SAMPLES) {
                if (fw < sm_first) sm_first = fw;
                if (lw > sm_last)  sm_last = lw;
            }
        }
        pgwt_reader_close(&r);
    }

    if (tr_last == 0) {
        fprintf(stderr, "ERROR: no transition (exact) blocks — did the "
                "escalation window record any data?\n");
        return 1;
    }
    if (sm_last == 0) {
        fprintf(stderr, "ERROR: no sample blocks — sampler produced nothing\n");
        return 1;
    }

    uint64_t win_from = tr_first > sm_first ? tr_first : sm_first;
    uint64_t win_to   = tr_last  < sm_last  ? tr_last  : sm_last;
    if (win_to <= win_from) {
        fprintf(stderr, "ERROR: no wall-clock overlap between sampled and "
                "exact data\n");
        return 1;
    }
    double win_s = (double)(win_to - win_from) / 1e9;
    printf("Overlap window: %.1fs (sampled + exact both present)\n", win_s);

    /* Second pass: accumulate sampled and exact time per event WITHIN the
     * overlap window. A transition interval is clipped to the window; a
     * sample counts if its timestamp falls inside. */
    struct evt_acc *table = calloc(MAX_DISTINCT, sizeof(*table));
    struct pgwt_trace_event *evs = malloc(MAX_EVENTS * sizeof(*evs));
    struct cmd_state *cmd_ht = calloc(CMD_HT, sizeof(*cmd_ht));
    if (!table || !evs || !cmd_ht) { perror("malloc"); return 1; }

    double total_sampled_ns = 0, total_exact_ns = 0;
    int n_samples = 0;
    /* T8 §5.6 self-check: over the exact (v3) intervals, the CPU measured
     * during WAIT-labeled gaps must be ≈0 — a sleeping task burns no CPU, so
     * "traces prove their own CPU accounting." Accumulate measured cpu_ns vs
     * wait time for non-idle wait intervals; only assert when the trace carries
     * measured cpu_ns (v2/legacy traces stamp the UNKNOWN sentinel). */
    double wait_gap_cpu_ns = 0, wait_total_ns_sc = 0;
    int have_measured_cpu = 0;

    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0)
            continue;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info info;
            int n = pgwt_reader_decode_block_info(&r, b, evs, MAX_EVENTS, &info);
            if (n <= 0)
                continue;
            for (int i = 0; i < n; i++) {
                struct pgwt_trace_event *e = &evs[i];
                if (info.block_type == PGWT_BLOCK_SAMPLES) {
                    uint64_t w = pgwt_reader_mono_to_wall(&r, e->timestamp_ns);
                    if (w < win_from || w >= win_to)
                        continue;
                    uint32_t ev = e->new_event;
                    /* T2: we==0 samples are first-class CPU (the capture
                     * side already applied the command gate). */
                    if (ev != 0 && pgwt_is_idle_event(ev))
                        continue;
                    double contrib = (double)info.sample_period_ns;
                    acc_find(table, ev)->sampled_ns += contrib;
                    total_sampled_ns += contrib;
                    n_samples++;
                } else { /* TRANSITIONS */
                    uint32_t ev = e->old_event;
                    /* CMD markers drive the exact-tier command gate. */
                    if (ev == PGWT_MARKER_CMD_START || ev == PGWT_MARKER_CMD_END) {
                        struct cmd_state *st = cmd_get(cmd_ht, e->pid);
                        st->seen_cmd = 1;
                        if (ev == PGWT_MARKER_CMD_START && !st->cmd_open) {
                            st->cmd_open = 1;
                            st->anchor = e->timestamp_ns;
                        } else if (ev == PGWT_MARKER_CMD_END && st->cmd_open) {
                            st->banked += e->timestamp_ns - st->anchor;
                            st->cmd_open = 0;
                        }
                        continue;
                    }
                    if (PGWT_IS_MARKER(ev))
                        continue;
                    /* Every exact interval consumes the pid's banked
                     * command-open time (the sweep is per-interval);
                     * the verdict matters only for we==0. */
                    int in_cmd = cmd_interval_open(cmd_get(cmd_ht, e->pid),
                                                   e->timestamp_ns,
                                                   e->duration_ns);
                    if (pgwt_is_idle_event(ev))
                        continue;
                    /* T2: exact we==0 intervals count as CPU only when
                     * majority in-command — the same definition the
                     * sampled side captured with. */
                    if (ev == 0 && !in_cmd)
                        continue;
                    /* T8 self-check: fold measured cpu_ns of WAIT intervals
                     * (the ≈0 quantity). Uses full interval values — an edge
                     * interval partially outside the window barely perturbs a
                     * ratio that should be ~0 anyway. */
                    if (ev != 0 && e->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                        have_measured_cpu = 1;
                        wait_gap_cpu_ns += (double)e->cpu_ns;
                        wait_total_ns_sc += (double)e->duration_ns;
                    }
                    uint64_t end = pgwt_reader_mono_to_wall(&r, e->timestamp_ns);
                    uint64_t dur = e->duration_ns;
                    uint64_t start = end > dur ? end - dur : 0;
                    /* clip to window */
                    if (end <= win_from || start >= win_to)
                        continue;
                    uint64_t cs = start > win_from ? start : win_from;
                    uint64_t ce = end   < win_to   ? end   : win_to;
                    if (ce <= cs)
                        continue;
                    double contrib = (double)(ce - cs);
                    acc_find(table, ev)->exact_ns += contrib;
                    total_exact_ns += contrib;
                }
            }
        }
        pgwt_reader_close(&r);
    }

    if (total_exact_ns == 0 || total_sampled_ns == 0) {
        fprintf(stderr, "ERROR: empty estimate (exact=%.0f sampled=%.0f)\n",
                total_exact_ns, total_sampled_ns);
        return 1;
    }

    /* Collect into a flat array, sort by exact share descending. */
    struct evt_acc list[MAX_DISTINCT];
    int nl = 0;
    for (int i = 0; i < MAX_DISTINCT; i++)
        if (table[i].used &&
            (table[i].exact_ns > 0 || table[i].sampled_ns > 0))
            list[nl++] = table[i];
    /* simple insertion sort by exact_ns desc */
    for (int i = 1; i < nl; i++) {
        struct evt_acc tmp = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].exact_ns < tmp.exact_ns) {
            list[j + 1] = list[j]; j--;
        }
        list[j + 1] = tmp;
    }

    printf("\n%-28s %12s %12s %10s\n",
           "wait_event", "exact_share%", "sampled%", "delta_pp");
    printf("--------------------------------------------------------------"
           "----\n");

    double max_delta = 0;
    char max_delta_ev[64] = "";
    for (int i = 0; i < nl; i++) {
        double ex = 100.0 * list[i].exact_ns / total_exact_ns;
        double sm = 100.0 * list[i].sampled_ns / total_sampled_ns;
        double d = ex - sm; if (d < 0) d = -d;
        printf("%-28s %11.1f%% %11.1f%% %9.1f\n",
               pgwt_event_name(list[i].event_id), ex, sm, d);
        /* only hold significant events to the tolerance */
        if (ex >= min_share && d > max_delta) {
            max_delta = d;
            snprintf(max_delta_ev, sizeof(max_delta_ev), "%s",
                     pgwt_event_name(list[i].event_id));
        }
    }

    /* Top-N overlap: does the top-N by exact share match the top-N by sampled
     * estimate? Rank a copy by sampled_ns, then intersect the two top-N sets. */
    int top = nl < 5 ? nl : 5;
    int overlap = 0;
    {
        struct evt_acc s[MAX_DISTINCT];
        memcpy(s, list, (size_t)nl * sizeof(list[0]));
        for (int a = 1; a < nl; a++) {
            struct evt_acc tmp = s[a]; int j = a - 1;
            while (j >= 0 && s[j].sampled_ns < tmp.sampled_ns) {
                s[j+1] = s[j]; j--;
            }
            s[j+1] = tmp;
        }
        for (int i = 0; i < top; i++)
            for (int k = 0; k < top; k++)
                if (list[i].event_id == s[k].event_id) { overlap++; break; }
    }

    printf("\nSamples in window: %d\n", n_samples);
    printf("Top-%d event overlap: %d/%d\n", top, overlap, top);
    printf("Max share disagreement (events >= %.0f%% exact share): "
           "%.1f pp (%s)\n", min_share, max_delta,
           max_delta_ev[0] ? max_delta_ev : "n/a");
    printf("Tolerance: +/- %.1f pp\n", tolerance);

    int ok = (max_delta <= tolerance) && (overlap >= (top >= 5 ? 4 : top));

    /* T8 §5.6.2: the trace's own CPU-accounting proof. Measured CPU during
     * wait-labeled gaps should be a rounding error next to the wait time. */
    if (have_measured_cpu && wait_total_ns_sc > 0) {
        /* With S3 (exact sched_switch CPU), a nonzero cpu_ns during a wait is
         * REAL on-CPU spin (LWLock/latch/spin-delay), not accounting drift — a
         * backend genuinely runs on the CPU while spinning to acquire a lock.
         * So this is an OBSERVABILITY report, not a pass/fail gate. The real
         * accounting invariant (total measured on-CPU == /proc) is checked by
         * the conservation test on the live box, not here. */
        double wait_cpu_pct = 100.0 * wait_gap_cpu_ns / wait_total_ns_sc;
        printf("On-CPU-during-wait (spin): %.4f%% of wait time — exact on-CPU "
               "spin, attributed to its wait class (informational)\n",
               wait_cpu_pct);
    } else {
        printf("On-CPU-during-wait (spin): n/a (no measured cpu_ns — "
               "legacy/v2 trace)\n");
    }

    printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");

    free(table);
    free(evs);
    free(cmd_ht);
    return ok ? 0 : 1;
}
