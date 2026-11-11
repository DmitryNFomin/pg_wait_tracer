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
 *                       [--raw-ns] [--show-idle] [--abc]
 *                       [--nominal-period-ns NS]
 *
 * --abc appends the offline A/B/C decomposition and join/conservation checks.
 * --nominal-period-ns supplies the configured sampler period (default 100 ms,
 * the shipped 10 Hz configuration). The trace stores measured weights only;
 * the configured nominal period cannot be recovered from those weights.
 * A failed join check exits 5 and suppresses the ABC share rows.
 *
 * --raw-ns adds a RAW block after the share table: the nanosecond totals
 * behind every share, the per-side AAS they imply, the CPU/non-CPU split of
 * each side, and the exact we==0 time this comparator's own command gate
 * REJECTED. Shares alone cannot tell "the sampled side lost CPU" from "the
 * exact side gained it": both move every other share by the same factor.
 * Off by default: no existing output line changes.
 *
 * --show-idle adds the IDLE section: the events this comparator DROPS from
 * both sides because pgwt_is_idle_event() says they are not the database
 * doing work (Client:ClientRead, Activity, the Timeout pacing set). Without
 * it those nanoseconds are invisible, so "CPU left the sampled side" can only
 * be INFERRED to have become ClientRead/idle rather than SHOWN. The section is
 * labelled non-DB-Time and is accumulated in its own table: it is not part of
 * any share, of total_exact_ns/total_sampled_ns, of max_delta, or of RESULT.
 *
 * --show-idle REFUSES rather than printing zeros when the Timeout pacing mask
 * is empty (no sidecar and no verified PG major in the header -- see
 * src/idle_rule.h PGWT_TIMEOUT_TABLE_VERIFIED). With an empty mask no Timeout
 * pacing sleep can be classified as idle, so the idle totals would be an
 * UNDERCOUNT of unknown size while looking exactly like a measured result.
 * It prints IDLE-UNAVAILABLE and exits 4. Exit 4 is reachable only when
 * --show-idle was asked for; the share table's 0/1 PASS/FAIL contract and
 * every byte of its output are unchanged.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define MAX_EVENTS 65536
#define MAX_DISTINCT 1024

struct evt_acc {
    uint32_t event_id;
    double   sampled_ns;   /* ASH estimate */
    double   exact_ns;     /* sum of durations */
    uint64_t a_joined_ns, c_measured_ns, c_nominal_ns; /* --abc only */
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

/* --abc retains only window-clipped intervals and window samples. Classification
 * is supplied by the EXISTING command sweep, including its majority rule; CPU
 * means we==0 wall time, not cpu_ns. Sort by (pid,start) once, then binary-search
 * each instant: O(I log I + S log I), independent of trace block interleaving.
 * Intervals are half-open [start,end), including at a transition boundary. */
struct abc_interval {
    uint64_t start, end;
    uint32_t pid, event;
    int excluded, cpu_known;
};
struct abc_sample {
    uint64_t ts, weight;
    uint32_t pid, event;
};
struct abc_state {
    struct abc_interval *intervals;
    struct abc_sample *samples;
    size_t ni, ci, ns, cs;
};

static void *abc_grow(void *p, size_t *capacity, size_t size)
{
    size_t next = *capacity ? *capacity * 2 : 1024;
    void *q = realloc(p, next * size);
    if (!q) { perror("ABC realloc"); exit(1); }
    *capacity = next;
    return q;
}

static int abc_interval_cmp(const void *va, const void *vb)
{
    const struct abc_interval *a = va, *b = vb;
    if (a->pid != b->pid) return a->pid < b->pid ? -1 : 1;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return a->end < b->end ? -1 : a->end > b->end;
}

struct abc_shares {
    double a, aj, b, cm, cn;
    double coverage, reading, selection, weighting;
};

static struct abc_shares abc_shares(const struct evt_acc *e,
                                    const struct evt_acc *total)
{
    struct abc_shares v = {
        .a = total->sampled_ns ? 100 * e->sampled_ns / total->sampled_ns : 0,
        .aj = total->a_joined_ns ? 100.0 * e->a_joined_ns / total->a_joined_ns : 0,
        .b = total->exact_ns ? 100 * e->exact_ns / total->exact_ns : 0,
        .cm = total->c_measured_ns ? 100.0 * e->c_measured_ns / total->c_measured_ns : 0,
        .cn = total->c_nominal_ns ? 100.0 * e->c_nominal_ns / total->c_nominal_ns : 0,
    };
    v.coverage = v.a - v.aj;
    v.reading = v.aj - v.cm;
    v.selection = v.cm - v.b;
    v.weighting = v.cm - v.cn;
    return v;
}

static void abc_row(const char *name, const struct evt_acc *e,
                    const struct evt_acc *total)
{
    struct abc_shares v = abc_shares(e, total);
    printf("ABC_ROW %-28s %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
           "%.0f %llu %.0f %llu %llu\n", name,
           v.a, v.aj, v.b, v.cm, v.cn,
           v.coverage, v.reading, v.selection, v.weighting,
           e->sampled_ns, (unsigned long long)e->a_joined_ns, e->exact_ns,
           (unsigned long long)e->c_measured_ns,
           (unsigned long long)e->c_nominal_ns);
}

static int abc_report(struct abc_state *s, struct evt_acc *table,
                      double at, double a_idle, double bt, double min_share,
                      uint64_t nominal)
{
    uint64_t unjoined_ns = 0, excluded = 0, excluded_nominal = 0;
    uint64_t aj_total = 0, aj_excluded = 0;
    uint64_t cm_total = 0, cn_total = 0, interval_ns = 0;
    size_t unjoined = 0, known = 0, unknown = 0, ik = 0, iu = 0;
    qsort(s->intervals, s->ni, sizeof(*s->intervals), abc_interval_cmp);
    for (size_t i = 0; i < s->ni; i++) {
        interval_ns += s->intervals[i].end - s->intervals[i].start;
        if (s->intervals[i].cpu_known) ik++; else iu++;
        /* An ambiguous timeline cannot support a point join. */
        if (i && s->intervals[i-1].pid == s->intervals[i].pid &&
            s->intervals[i-1].end > s->intervals[i].start) {
            printf("\n=== ABC ===\nABC_UNAVAILABLE overlapping exact intervals\n");
            return 5;
        }
    }
    for (size_t i = 0; i < s->ns; i++) {
        struct abc_sample *sample = &s->samples[i];
        size_t lo = 0, hi = s->ni;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            struct abc_interval *v = &s->intervals[mid];
            if (v->pid < sample->pid ||
                (v->pid == sample->pid && v->start <= sample->ts))
                lo = mid + 1;
            else hi = mid;
        }
        struct abc_interval *v = lo ? &s->intervals[lo - 1] : NULL;
        if (!v || v->pid != sample->pid || sample->ts >= v->end) {
            unjoined++;
            unjoined_ns += sample->weight;
            continue;
        }
        if (v->cpu_known) known++; else unknown++;
        /* Keep A's recorded classification and measured weight; only the
         * population changes. C then reclassifies this SAME joined set. */
        if (pgwt_is_idle_event(sample->event))
            aj_excluded += sample->weight;
        else
            acc_find(table, sample->event)->a_joined_ns += sample->weight;
        if (v->excluded) {
            excluded += sample->weight;
            excluded_nominal += nominal;
        } else {
            struct evt_acc *c = acc_find(table, v->event);
            c->c_measured_ns += sample->weight;
            c->c_nominal_ns += nominal;
        }
    }
    /* Sum independently from the event buckets, not a counter incremented
     * beside the input sum: losing a deposit must make this check go red. */
    for (int i = 0; i < MAX_DISTINCT; i++) {
        aj_total += table[i].a_joined_ns;
        cm_total += table[i].c_measured_ns;
        cn_total += table[i].c_nominal_ns;
    }
    /* Use A's actual accumulators, not an input-weight counter rebuilt by
     * this join. Idle must reconcile separately because A and C can disagree
     * about whether an instant is DB Time (the A_joined-C question). */
    long double residual = (long double)cm_total + excluded -
                           ((long double)at + a_idle - unjoined_ns);
    long double aj_residual = (long double)aj_total + aj_excluded -
                              ((long double)at + a_idle - unjoined_ns);
    int join_closes = residual == 0 && aj_residual == 0;
    printf("\n=== ABC ===\n");
    printf("ABC_NOMINAL_PERIOD_NS %llu (input; default 100000000; "
           "not retained in trace)\n",
           (unsigned long long)nominal);
    printf("ABC_POPULATION all window samples; shares exclude idle/command-gated "
           "time independently on each estimator\n");
    printf("ABC_SAMPLES %zu\n", s->ns);
    printf("ABC_SAMPLED_MEASURED_NS %.0f\n", at + a_idle);
    printf("ABC_SAMPLES_NOMINAL_NS %llu\n", (unsigned long long)(s->ns * nominal));
    printf("ABC_EXACT_WINDOW_NS %.0f\n", bt);
    printf("ABC_EXACT_INTERVAL_NS %llu (includes idle/command-gated time)\n",
           (unsigned long long)interval_ns);
    printf("ABC_MEASURED_NOMINAL_RATIO %.9f\n",
           s->ns ? (at + a_idle) / ((double)s->ns * nominal) : 0);
    printf("UNJOINED %zu %llu\n", unjoined, (unsigned long long)unjoined_ns);
    printf("ABC_JOINED_CPU_NS_PRESENT %zu %zu\n", known, unknown);
    printf("CPU_NS_PRESENT %zu %zu\n", ik, iu);
    printf("ABC_A_EXCLUDED_NS %.0f\n", a_idle);
    printf("ABC_A_JOINED_EXCLUDED_NS %llu\n", (unsigned long long)aj_excluded);
    printf("ABC_C_EXCLUDED_NS %llu %llu\n", (unsigned long long)excluded,
           (unsigned long long)excluded_nominal);
    printf("ABC_JOIN_IDENTITY C_measured_DB + C_excluded = "
           "A_DB + A_excluded - UNJOINED_ns\n");
    printf("ABC_A_JOINED_IDENTITY A_joined_DB + A_joined_excluded = "
           "A_DB + A_excluded - UNJOINED_ns\n");
    printf("JOIN_CLOSES %s %.0Lf\n", join_closes ? "yes" : "no",
           residual != 0 ? residual : aj_residual);
    if (!join_closes) {
        printf("JOIN_RESIDUAL_A_JOINED %.0Lf\n", aj_residual);
        printf("JOIN_RESIDUAL_UNEXPLAINED %.0Lf\n",
               residual != 0 ? residual : aj_residual);
        return 5;
    }
    if (!cm_total || !cn_total) {
        printf("ABC_UNAVAILABLE no joined DB Time samples\n");
        return 5;
    }
    struct evt_acc total = {
        .sampled_ns = at, .a_joined_ns = aj_total, .exact_ns = bt,
        .c_measured_ns = cm_total, .c_nominal_ns = cn_total,
    };
    /* Check every event, including those hidden by --min-share, and TOTAL.
     * Allow only floating-point roundoff, far below printed pp precision. */
    for (int i = 0; i <= MAX_DISTINCT; i++) {
        struct evt_acc *e = i == MAX_DISTINCT ? &total : &table[i];
        if (i != MAX_DISTINCT && !e->used) continue;
        struct abc_shares v = abc_shares(e, &total);
        double error = (v.a - v.b) - (v.coverage + v.reading + v.selection);
        if (!(error >= -1e-9 && error <= 1e-9)) {
            printf("ABC_DECOMPOSITION_CLOSES no %s %.12g\n",
                   i == MAX_DISTINCT ? "TOTAL" : pgwt_event_name(e->event_id), error);
            return 5;
        }
    }
    printf("ABC_DECOMPOSITION_IDENTITY A-B = "
           "(A-A_joined)+(A_joined-C_measured)+(C_measured-B)\n");
    printf("ABC_DECOMPOSITION_CLOSES yes\n");
    printf("ABC_COLUMNS event A A_joined B C_measured C_nominal "
           "A-A_joined A_joined-C_measured C_measured-B C_measured-C_nominal "
           "A_ns A_joined_ns B_ns C_measured_ns C_nominal_ns (shares/deltas in pp)\n");
    printf("ABC_DELTAS A-A_joined: coverage gap (exact-tier); "
           "A_joined-C_measured: reading and classification; "
           "C_measured-B: selection/weighting; C_measured-C_nominal: weighting\n");
    /* Same eligibility as max_delta: exact share >= --min-share. Totals
     * include ALL events, including those below that display threshold. */
    for (int i = 0; i < MAX_DISTINCT; i++) {
        struct evt_acc *e = &table[i];
        if (e->used && 100 * e->exact_ns / bt >= min_share)
            abc_row(pgwt_event_name(e->event_id), e, &total);
    }
    abc_row("TOTAL", &total, &total);
    printf("=== END ABC ===\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace_dir> [--tolerance PCT] "
                "[--min-share PCT] [--raw-ns] [--show-idle]\n", argv[0]);
        return 2;
    }
    const char *trace_dir = argv[1];
    double tolerance = 10.0;   /* percentage points */
    double min_share = 2.0;    /* ignore events below this exact share */
    int raw_ns = 0;
    int show_idle = 0;
    int abc = 0;
    uint64_t nominal_period_ns = 100000000ULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--tolerance") && i + 1 < argc)
            tolerance = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-share") && i + 1 < argc)
            min_share = atof(argv[++i]);
        else if (!strcmp(argv[i], "--raw-ns"))
            raw_ns = 1;
        else if (!strcmp(argv[i], "--show-idle"))
            show_idle = 1;
        else if (!strcmp(argv[i], "--abc"))
            abc = 1;
        else if (!strcmp(argv[i], "--nominal-period-ns")) {
            if (i + 1 == argc) {
                fprintf(stderr, "ERROR: --nominal-period-ns requires positive ns\n");
                return 2;
            }
            char *end;
            const char *value = argv[++i];
            errno = 0;
            nominal_period_ns = strtoull(value, &end, 10);
            if (errno || *end || *value < '0' || *value > '9' || !nominal_period_ns) {
                fprintf(stderr, "ERROR: --nominal-period-ns requires positive ns\n");
                return 2;
            }
        }
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
     * No sidecar: fall back exactly the way pgwt-server does, via the shared
     * pgwt_init_event_names_for_trace() -- the trace HEADER's major, then the
     * unknown-major state (PG18 tables for display, EMPTY pacing mask). This
     * tool used to call pgwt_init_event_names(18) here and warn, which made it
     * the last place still assuming PG18: a PG13 or PG16 trace with no sidecar
     * was classified with PG18's Timeout ids by the very tool whose +/-10pp
     * verdict justifies the shipped sample rate, while pgwt-server on the same
     * directory would refuse to classify it at all. Two tools disagreeing about
     * what is idle is exactly the failure the shared helper exists to prevent,
     * so it must be used here too, not reimplemented. */
    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(trace_dir, files, 256);
    if (nfiles <= 0) {
        fprintf(stderr, "ERROR: no trace files in %s\n", trace_dir);
        return 1;
    }

    int hdr_major = 0;
    for (int i = 0; i < nfiles && hdr_major == 0; i++) {
        struct pgwt_event_reader hr;
        if (pgwt_reader_open(&hr, files[i].path) != 0)
            continue;
        if (hr.header.pg_version > 0)
            hdr_major = (int)hr.header.pg_version;
        pgwt_reader_close(&hr);
    }

    /* Hoisted out of the block below: --show-idle must report the PROVENANCE
     * of the idle classification (sidecar? which major settled?), because an
     * unresolved provenance is exactly when its totals are an undercount. */
    int sidecar_ok = 0, settled = 0;
    {
        /* Same shape as src/server.c's server_init(): probe the sidecar to
         * decide whether to warn, then let the shared helper settle the
         * tables. Loading twice is idempotent. */
        sidecar_ok = (pgwt_load_names_json(trace_dir) == 0);
        settled = pgwt_init_event_names_for_trace(trace_dir, hdr_major);
        if (!sidecar_ok) {
            if (settled > 0)
                fprintf(stderr, "WARN: %s has no wait_event_names.json "
                        "sidecar -- using the PG%d tables recorded in the "
                        "trace header. Names are this build's static tables, "
                        "not the ones the trace was written with.\n",
                        trace_dir, settled);
            else
                fprintf(stderr, "WARN: %s has neither a "
                        "wait_event_names.json sidecar nor a PG major in any "
                        "trace header -- NO event is classified as idle "
                        "(empty pacing mask), so pacing sleeps are counted "
                        "as DB Time in the shares below\n", trace_dir);
        }
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
    /* --raw-ns diagnostics. gate_rejected_cpu_ns is the exact we==0 time this
     * comparator threw away because its own CMD_START/CMD_END sweep called the
     * interval out-of-command: the quantity that decides whether a CPU share
     * gap lives on the sampled side or on this gate. Clipped to the window, so
     * it is directly comparable with total_exact_ns. */
    double gate_rejected_cpu_ns = 0;
    long   gate_rejected_intervals = 0;
    /* --show-idle: the NON-DB-TIME events both sides drop. Its own table, its
     * own totals -- nothing here feeds total_exact_ns, total_sampled_ns, any
     * share, max_delta or RESULT. */
    struct evt_acc *idle_tab = calloc(MAX_DISTINCT, sizeof(*idle_tab));
    double idle_exact_ns = 0, idle_sampled_ns = 0;
    long   idle_samples = 0;
    if (!idle_tab) { perror("calloc idle_tab"); return 1; }
    long   n_samples_cpu = 0;        /* we==0 samples counted in the window */
    long   n_samples_noncpu = 0;
    uint64_t sample_period_seen = 0; /* v4 nominal cadence / legacy block mean */
    struct abc_state abc_data = {0};

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
                    /* The product decoder supplies each v4 tick's exact
                     * weight in duration_ns; v2/v3 retain the stored mean. */
                    uint32_t ev = e->new_event;
                    /* Learn the period BEFORE the idle filter: a window whose
                     * only in-range samples are idle still has a period, and
                     * the idle section converts ns back to samples with it. */
                    sample_period_seen = r.header.version >= 4
                                       ? info.nominal_sample_period_ns
                                       : info.sample_period_ns;
                    if (abc) {
                        if (abc_data.ns == abc_data.cs)
                            abc_data.samples = abc_grow(abc_data.samples,
                                &abc_data.cs, sizeof(*abc_data.samples));
                        abc_data.samples[abc_data.ns++] = (struct abc_sample){
                            w, e->duration_ns, e->pid, ev};
                    }
                    /* T2: we==0 samples are first-class CPU (the capture
                     * side already applied the command gate). */
                    if (ev != 0 && pgwt_is_idle_event(ev)) {
                        /* NOT DB Time -- kept out of every share, recorded in
                         * the idle table so --show-idle can SHOW where the
                         * time went instead of leaving it to be inferred. */
                        double ic = (double)e->duration_ns;
                        acc_find(idle_tab, ev)->sampled_ns += ic;
                        idle_sampled_ns += ic;
                        idle_samples++;
                        continue;
                    }
                    double contrib = (double)e->duration_ns;
                    acc_find(table, ev)->sampled_ns += contrib;
                    total_sampled_ns += contrib;
                    n_samples++;
                    if (ev == 0) n_samples_cpu++; else n_samples_noncpu++;
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
                    if (abc) {
                        uint64_t end = pgwt_reader_mono_to_wall(&r, e->timestamp_ns);
                        uint64_t start = end > e->duration_ns ? end - e->duration_ns : 0;
                        uint64_t cs = start > win_from ? start : win_from;
                        uint64_t ce = end < win_to ? end : win_to;
                        if (ce > cs) {
                            if (abc_data.ni == abc_data.ci)
                                abc_data.intervals = abc_grow(abc_data.intervals,
                                    &abc_data.ci, sizeof(*abc_data.intervals));
                            abc_data.intervals[abc_data.ni++] = (struct abc_interval){
                                cs, ce, e->pid, ev,
                                pgwt_is_idle_event(ev) || (ev == 0 && !in_cmd),
                                e->cpu_ns != PGWT_CPU_NS_UNKNOWN};
                        }
                    }
                    if (pgwt_is_idle_event(ev)) {
                        /* NOT DB Time. Clipped to the SAME overlap window the
                         * accepted path clips to below, so the idle totals are
                         * directly comparable with total_exact_ns rather than
                         * being whole-capture figures. */
                        uint64_t iend = pgwt_reader_mono_to_wall(
                            &r, e->timestamp_ns);
                        uint64_t idur = e->duration_ns;
                        uint64_t istart = iend > idur ? iend - idur : 0;
                        if (iend > win_from && istart < win_to) {
                            uint64_t cs = istart > win_from ? istart : win_from;
                            uint64_t ce = iend < win_to ? iend : win_to;
                            if (ce > cs) {
                                acc_find(idle_tab, ev)->exact_ns +=
                                    (double)(ce - cs);
                                idle_exact_ns += (double)(ce - cs);
                            }
                        }
                        continue;
                    }
                    /* T2: exact we==0 intervals count as CPU only when
                     * majority in-command — the same definition the
                     * sampled side captured with. */
                    if (ev == 0 && !in_cmd) {
                        /* Same window clip the accepted path applies below, so
                         * the rejected total is comparable with total_exact_ns
                         * rather than being a whole-capture figure. */
                        uint64_t rend = pgwt_reader_mono_to_wall(
                            &r, e->timestamp_ns);
                        uint64_t rdur = e->duration_ns;
                        uint64_t rstart = rend > rdur ? rend - rdur : 0;
                        if (rend > win_from && rstart < win_to) {
                            uint64_t cs = rstart > win_from ? rstart : win_from;
                            uint64_t ce = rend < win_to ? rend : win_to;
                            if (ce > cs) {
                                gate_rejected_cpu_ns += (double)(ce - cs);
                                gate_rejected_intervals++;
                            }
                        }
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

    /* --show-idle: where the nanoseconds that are NOT DB Time went.
     *
     * Both sides drop pgwt_is_idle_event() time from every share, so a CPU
     * total that shrinks between tiers has no visible destination: it is
     * inferred to have become ClientRead/idle. This section prints that
     * destination. It is NOT part of the shares above -- the denominators here
     * are each side's DB-Time total, so these percentages deliberately do not
     * sum into the 100%% of the table above.
     *
     * The refusal: Client:ClientRead and Activity are classified structurally
     * (by class, version-independently), but the Timeout PACING subset comes
     * from an id mask derived from the resolved PG major (src/idle_rule.c).
     * With an empty mask, CheckpointWriteDelay et al. stay in DB Time, so
     * these totals would be an undercount of unknown size that reads exactly
     * like a measurement. A diagnostic that cannot see refuses. */
    int idle_unavailable = 0;
    if (show_idle) {
        uint32_t pacing_mask = pgwt_idle_rule_timeout_mask();
        printf("\n=== IDLE: NOT DB Time (excluded from every share above) ==="
               "\n");
        printf("IDLE_PACING_MASK 0x%X  settled_pg_major=%d  sidecar=%s\n",
               pacing_mask, settled, sidecar_ok ? "yes" : "no");
        if (pacing_mask == 0) {
            printf("IDLE-UNAVAILABLE: the Timeout pacing mask is EMPTY, so no "
                   "Timeout pacing sleep can be classified as idle and these "
                   "totals would UNDERCOUNT by an unknown amount. Refusing to "
                   "print numbers that would read as measured zeros. Supply a "
                   "wait_event_names.json sidecar, or a trace header with a "
                   "verified PG major (13/17/18).\n");
            idle_unavailable = 1;
        } else {
            struct evt_acc il[MAX_DISTINCT];
            int nil = 0;
            for (int i = 0; i < MAX_DISTINCT; i++)
                if (idle_tab[i].used &&
                    (idle_tab[i].exact_ns > 0 || idle_tab[i].sampled_ns > 0))
                    il[nil++] = idle_tab[i];
            for (int i = 1; i < nil; i++) {
                struct evt_acc tmp = il[i];
                int j = i - 1;
                while (j >= 0 && il[j].exact_ns < tmp.exact_ns) {
                    il[j + 1] = il[j]; j--;
                }
                il[j + 1] = tmp;
            }
            if (nil == 0) {
                /* Absence stated, not implied by an empty table: "this trace
                 * recorded no idle event" and "nothing was classified" are
                 * different answers and must not look the same. */
                printf("IDLE_EVENTS none -- mask resolved, and this trace "
                       "carries no idle event on either tier\n");
            } else {
                printf("\n%-28s %14s %14s %9s %9s\n", "idle_event",
                       "exact_ns", "sampled_ns", "ex%DBT", "sm%DBT");
                printf("------------------------------------------------------"
                       "--------------------------\n");
                for (int i = 0; i < nil; i++)
                    printf("%-28s %14.0f %14.0f %8.1f%% %8.1f%%\n",
                           pgwt_event_name(il[i].event_id),
                           il[i].exact_ns, il[i].sampled_ns,
                           100.0 * il[i].exact_ns / total_exact_ns,
                           100.0 * il[i].sampled_ns / total_sampled_ns);
            }
            printf("IDLE_EVENT_ROWS %d\n", nil);
            printf("IDLE_TOTAL_EXACT_NS %.0f\n", idle_exact_ns);
            printf("IDLE_TOTAL_SAMPLED_NS %.0f\n", idle_sampled_ns);
            printf("IDLE_SAMPLES %ld\n", idle_samples);
            printf("IDLE_EXACT_PCT_OF_DBTIME %.2f\n",
                   100.0 * idle_exact_ns / total_exact_ns);
            printf("IDLE_SAMPLED_PCT_OF_DBTIME %.2f\n",
                   100.0 * idle_sampled_ns / total_sampled_ns);
        }
    }

    /* --raw-ns: the nanoseconds behind the shares. A share table cannot
     * localise a disagreement — if one side's CPU total is wrong, EVERY other
     * share on that side moves by the same factor, which looks identical to
     * "every event is inflated". These raw totals, plus the AAS each side
     * implies over the same window, separate a missing numerator (sampled CPU
     * dropped) from an inflated one (exact CPU over-counted). */
    if (raw_ns) {
        struct evt_acc *cpu = acc_find(table, 0);
        double cpu_ex = cpu->exact_ns, cpu_sm = cpu->sampled_ns;
        double non_ex = total_exact_ns - cpu_ex;
        double non_sm = total_sampled_ns - cpu_sm;
        printf("\nRAW_WINDOW_S %.6f\n", win_s);
        printf("RAW_SAMPLE_PERIOD_NS %llu\n",
               (unsigned long long)sample_period_seen);
        printf("RAW_TOTAL_EXACT_NS %.0f\n", total_exact_ns);
        printf("RAW_TOTAL_SAMPLED_NS %.0f\n", total_sampled_ns);
        printf("RAW_AAS_EXACT %.6f\n", total_exact_ns / 1e9 / win_s);
        printf("RAW_AAS_SAMPLED %.6f\n", total_sampled_ns / 1e9 / win_s);
        printf("RAW_CPU_EXACT_NS %.0f\n", cpu_ex);
        printf("RAW_CPU_SAMPLED_NS %.0f\n", cpu_sm);
        printf("RAW_NONCPU_EXACT_NS %.0f\n", non_ex);
        printf("RAW_NONCPU_SAMPLED_NS %.0f\n", non_sm);
        printf("RAW_CPU_SAMPLES %ld\n", n_samples_cpu);
        printf("RAW_NONCPU_SAMPLES %ld\n", n_samples_noncpu);
        printf("RAW_GATE_REJECTED_EXACT_CPU_NS %.0f\n", gate_rejected_cpu_ns);
        printf("RAW_GATE_REJECTED_INTERVALS %ld\n", gate_rejected_intervals);
        /* How many extra we==0 samples the sampled side would have had to
         * record for its CPU share to equal the exact side's. Positive = the
         * sampled side is SHORT of CPU. Denominator is the sampled total with
         * those samples added, which is why this is not a plain subtraction. */
        double ex_share = total_exact_ns > 0 ? cpu_ex / total_exact_ns : 0;
        double missing_ns = ex_share < 1.0
            ? (ex_share * non_sm / (1.0 - ex_share)) - cpu_sm : 0;
        printf("RAW_SAMPLED_CPU_NS_NEEDED_FOR_PARITY %.0f\n", missing_ns);
        if (sample_period_seen > 0)
            printf("RAW_SAMPLED_CPU_SAMPLES_NEEDED_FOR_PARITY %.0f\n",
                   missing_ns / (double)sample_period_seen);
    }

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

    int abc_status = abc ? abc_report(&abc_data, table, total_sampled_ns,
                                    idle_sampled_ns, total_exact_ns, min_share,
                                    nominal_period_ns) : 0;
    free(abc_data.intervals);
    free(abc_data.samples);

    free(table);
    free(evs);
    free(cmd_ht);
    free(idle_tab);
    /* Exit 4 = --show-idle was asked for and could not be answered. Only
     * reachable when --show-idle is passed, so the share table's 0/1
     * PASS/FAIL contract is untouched for every existing caller. */
    if (abc_status)
        return abc_status;
    if (idle_unavailable)
        return 4;
    return ok ? 0 : 1;
}
