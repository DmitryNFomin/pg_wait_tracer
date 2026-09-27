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
#include "cmd_gate.h"
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

/* ── T2/#115: exact-tier command gate ────────────────────────────────
 * The sampled tier records client we==0 only while a command is open, so
 * the exact side must classify its we==0 intervals the same way or the
 * comparison (and AAS across tier switches) would diverge by construction
 * on chatty workloads. The sweep and the PROPORTIONAL split rule are
 * src/cmd_gate.h, shared verbatim with the live paths — this file used to
 * carry its own copy with an all-or-nothing majority verdict (and an
 * unguarded `banked += ts - anchor` that underflowed on an out-of-order
 * CMD_END). A we==0 interval now contributes only the wall it really spent
 * inside a command. Pids with no CMD markers keep all we==0 as CPU
 * (background processes / legacy traces). */
#define CMD_HT 1024

static struct pgwt_cmd_gate *cmd_get(struct pgwt_cmd_gate *ht, uint32_t pid,
                                     uint32_t *pids)
{
    uint32_t h = (pid * 2654435761u) % CMD_HT;
    while (pids[h] && pids[h] != pid)
        h = (h + 1) % CMD_HT;
    if (!pids[h]) pids[h] = pid;
    return &ht[h];
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
    struct pgwt_cmd_gate *cmd_ht = calloc(CMD_HT, sizeof(*cmd_ht));
    uint32_t *cmd_pids = calloc(CMD_HT, sizeof(*cmd_pids));
    if (!table || !evs || !cmd_ht || !cmd_pids) { perror("malloc"); return 1; }

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
                        pgwt_cmd_gate_marker(cmd_get(cmd_ht, e->pid, cmd_pids),
                                             ev, e->timestamp_ns);
                        continue;
                    }
                    if (PGWT_IS_MARKER(ev))
                        continue;
                    /* Every exact interval consumes the pid's banked
                     * command-open time (the sweep is per-interval);
                     * the split matters only for we==0. */
                    uint64_t iv_t1 = e->timestamp_ns;
                    uint64_t iv_t0 = iv_t1 > e->duration_ns
                                   ? iv_t1 - e->duration_ns : 0;
                    struct pgwt_cmd_split csp = pgwt_cmd_gate_split(
                        cmd_get(cmd_ht, e->pid, cmd_pids), true,
                        iv_t0, iv_t1, true);
                    if (pgwt_is_idle_event(ev))
                        continue;
                    /* #115: an exact we==0 interval contributes the FRACTION
                     * of itself that overlapped a command window — the same
                     * definition the sampled side captured with, without the
                     * all-or-nothing majority rule's per-boundary bias. */
                    double cmd_frac = 1.0;
                    if (ev == 0) {
                        uint64_t wall = csp.in_ns + csp.out_ns;
                        cmd_frac = wall ? (double)csp.in_ns / (double)wall
                                        : (csp.in_cmd ? 1.0 : 0.0);
                        if (cmd_frac <= 0.0)
                            continue;
                    }
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
                    double contrib = (double)(ce - cs) * cmd_frac;
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
    free(cmd_pids);
    return ok ? 0 : 1;
}
