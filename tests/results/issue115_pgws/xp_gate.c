/* xp_gate.c — THROWAWAY experiment tool for issue #115.
 *
 * Variants of the exact-tier command gate over one trace dir:
 *   S  stream order (what the product does today) + all-or-nothing majority
 *   A  per-pid timestamp(end)-sorted single-pass sweep + majority
 *   B  per-pid timestamp-sorted single-pass sweep + proportional credit
 *   C  two-pass: per-pid sorted command-interval list, proportional overlap
 *
 * Usage: xp_gate <trace_dir> [--exclude-pid N]... [--client-only]
 */
#include "event_reader.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_EVENTS 65536

struct rec {
    uint64_t s, e;
    uint64_t cpu;
    uint32_t pid;
    uint32_t we;
    int      marker;    /* 0 none, 1 CMD_START, 2 CMD_END */
};

static struct rec *recs;
static size_t nrecs, caprecs;

static void push(struct rec r)
{
    if (nrecs == caprecs) {
        caprecs = caprecs ? caprecs * 2 : 1 << 16;
        recs = realloc(recs, caprecs * sizeof(*recs));
        if (!recs) { perror("realloc"); exit(1); }
    }
    recs[nrecs++] = r;
}

struct smp { uint32_t pid, we; uint64_t period; };
static struct smp *smps; static size_t nsmp, capsmp;
static void pushs(struct smp s)
{
    if (nsmp == capsmp) {
        capsmp = capsmp ? capsmp * 2 : 4096;
        smps = realloc(smps, capsmp * sizeof(*smps));
        if (!smps) { perror("realloc"); exit(1); }
    }
    smps[nsmp++] = s;
}

static int cmp_rec(const void *a, const void *b)
{
    const struct rec *x = a, *y = b;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    if (x->e != y->e) return x->e < y->e ? -1 : 1;
    return (y->marker != 0) - (x->marker != 0);
}

static uint64_t win_from, win_to;

static uint64_t clipd(uint64_t s, uint64_t e)
{
    if (e <= win_from || s >= win_to) return 0;
    uint64_t cs = s > win_from ? s : win_from;
    uint64_t ce = e < win_to   ? e : win_to;
    return ce > cs ? ce - cs : 0;
}

#define HT 4096
struct st { uint32_t pid; int seen, open; uint64_t anchor, banked; };
static struct st ht[HT];
static struct st *stget(uint32_t pid)
{
    uint32_t h = (pid * 2654435761u) % HT;
    while (ht[h].pid && ht[h].pid != pid) h = (h + 1) % HT;
    if (!ht[h].pid) ht[h].pid = pid;
    return &ht[h];
}

static uint32_t excl[512]; static int nexcl;
static int excluded(uint32_t pid)
{
    for (int k = 0; k < nexcl; k++) if (pid == excl[k]) return 1;
    return 0;
}

#define PHT 8192
static uint32_t cmdpid[PHT];
static int is_cmdpid(uint32_t p)
{
    uint32_t h = (p * 2654435761u) % PHT;
    while (cmdpid[h] && cmdpid[h] != p) h = (h + 1) % PHT;
    return cmdpid[h] == p;
}

/* per-event table */
#define ET 1024
struct ev { uint32_t id; int used; double s_ns, c_ns, sm_ns; };
static struct ev evt[ET];
static struct ev *evget(uint32_t id)
{
    uint32_t h = (id * 2654435761u) % ET;
    while (evt[h].used && evt[h].id != id) h = (h + 1) % ET;
    if (!evt[h].used) { evt[h].used = 1; evt[h].id = id; }
    return &evt[h];
}
static int cmp_ev(const void *a, const void *b)
{
    const struct ev *x = a, *y = b;
    return x->c_ns < y->c_ns ? 1 : x->c_ns > y->c_ns ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <trace_dir> [--exclude-pid N] [--client-only]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    int client_only = 0, per_pid = 0;
    uint64_t ovr_from = 0, ovr_to = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--exclude-pid") && i + 1 < argc && nexcl < 512)
            excl[nexcl++] = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--client-only"))
            client_only = 1;
        else if (!strcmp(argv[i], "--per-pid"))
            per_pid = 1;
        else if (!strcmp(argv[i], "--win-from") && i + 1 < argc)
            ovr_from = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--win-to") && i + 1 < argc)
            ovr_to = strtoull(argv[++i], NULL, 10);
    }

    struct pgwt_trace_file_entry files[256];
    int nfiles = pgwt_scan_trace_files(dir, files, 256);
    if (nfiles <= 0) { fprintf(stderr, "no trace files\n"); return 1; }

    uint64_t tr_f = UINT64_MAX, tr_l = 0, sm_f = UINT64_MAX, sm_l = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0) continue;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info info;
            if (pgwt_reader_block_info(&r, b, &info) != 0) continue;
            uint64_t fw = pgwt_reader_mono_to_wall(&r, info.first_timestamp_ns);
            uint64_t lw = pgwt_reader_mono_to_wall(&r, info.last_timestamp_ns);
            if (info.block_type == PGWT_BLOCK_TRANSITIONS) {
                if (fw < tr_f) tr_f = fw;
                if (lw > tr_l) tr_l = lw;
            } else if (info.block_type == PGWT_BLOCK_SAMPLES) {
                if (fw < sm_f) sm_f = fw;
                if (lw > sm_l) sm_l = lw;
            }
        }
        pgwt_reader_close(&r);
    }
    if (!tr_l || !sm_l) { fprintf(stderr, "missing tier\n"); return 1; }
    win_from = tr_f > sm_f ? tr_f : sm_f;
    win_to   = tr_l < sm_l ? tr_l : sm_l;
    if (ovr_from && ovr_from > win_from) win_from = ovr_from;
    if (ovr_to && ovr_to < win_to) win_to = ovr_to;
    printf("WINDOW wall_from=%llu wall_to=%llu secs=%.3f\n",
           (unsigned long long)win_from, (unsigned long long)win_to,
           (double)(win_to - win_from) / 1e9);

    struct pgwt_trace_event *evs = malloc(MAX_EVENTS * sizeof(*evs));
    if (!evs) { perror("malloc"); return 1; }

    for (int fi = 0; fi < nfiles; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0) continue;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_block_info info;
            int n = pgwt_reader_decode_block_info(&r, b, evs, MAX_EVENTS, &info);
            if (n <= 0) continue;
            for (int i = 0; i < n; i++) {
                struct pgwt_trace_event *e = &evs[i];
                if (excluded(e->pid)) continue;
                if (info.block_type == PGWT_BLOCK_SAMPLES) {
                    uint64_t w = pgwt_reader_mono_to_wall(&r, e->timestamp_ns);
                    if (w < win_from || w >= win_to) continue;
                    struct smp s = { e->pid, e->new_event, info.sample_period_ns };
                    pushs(s);
                    continue;
                }
                uint32_t ev = e->old_event;
                uint64_t end = pgwt_reader_mono_to_wall(&r, e->timestamp_ns);
                struct rec rr; memset(&rr, 0, sizeof rr);
                rr.pid = e->pid;
                if (ev == PGWT_MARKER_CMD_START || ev == PGWT_MARKER_CMD_END) {
                    rr.marker = ev == PGWT_MARKER_CMD_START ? 1 : 2;
                    rr.s = rr.e = end;
                    push(rr);
                    continue;
                }
                if (PGWT_IS_MARKER(ev)) continue;
                rr.we = ev; rr.e = end;
                rr.s = end > e->duration_ns ? end - e->duration_ns : 0;
                rr.cpu = e->cpu_ns;
                push(rr);
            }
        }
        pgwt_reader_close(&r);
    }
    free(evs);

    for (size_t k = 0; k < nrecs; k++) {
        if (!recs[k].marker) continue;
        uint32_t h = (recs[k].pid * 2654435761u) % PHT;
        while (cmdpid[h] && cmdpid[h] != recs[k].pid) h = (h + 1) % PHT;
        cmdpid[h] = recs[k].pid;
    }

    double sm_cpu = 0, sm_nonidle = 0, sm_total = 0, sm_noncmd = 0;
    for (size_t k = 0; k < nsmp; k++) {
        if (client_only && !is_cmdpid(smps[k].pid)) continue;
        uint32_t ev = smps[k].we;
        sm_total += (double)smps[k].period;
        if (ev == PGWT_WEI_NONCMD_CPU) sm_noncmd += (double)smps[k].period;
        if (ev != 0 && pgwt_is_idle_event(ev)) continue;
        sm_nonidle += (double)smps[k].period;
        if (ev == 0) sm_cpu += (double)smps[k].period;
        evget(ev)->sm_ns += (double)smps[k].period;
    }

    double S_cpu = 0, S_wait = 0;
    for (size_t k = 0; k < nrecs; k++) {
        struct rec *r = &recs[k];
        if (client_only && !is_cmdpid(r->pid)) continue;
        struct st *s = stget(r->pid);
        if (r->marker) {
            s->seen = 1;
            if (r->marker == 1) { if (!s->open) { s->open = 1; s->anchor = r->s; } }
            else { if (s->open) { if (r->s > s->anchor) s->banked += r->s - s->anchor; s->open = 0; } }
            continue;
        }
        uint64_t open_ns = s->banked;
        if (s->open) {
            uint64_t a = s->anchor > r->s ? s->anchor : r->s;
            if (r->e > a) open_ns += r->e - a;
        }
        s->banked = 0;
        if (s->open) s->anchor = r->e;
        uint64_t dur = r->e > r->s ? r->e - r->s : 0;
        int in_cmd = !s->seen ? 1 : (dur == 0 ? s->open : (open_ns * 2 >= dur));
        uint64_t cd = clipd(r->s, r->e);
        if (!cd || pgwt_is_idle_event(r->we)) continue;
        if (r->we == 0) { if (in_cmd) { S_cpu += (double)cd; evget(0)->s_ns += (double)cd; } }
        else { S_wait += (double)cd; evget(r->we)->s_ns += (double)cd; }
    }

    qsort(recs, nrecs, sizeof(*recs), cmp_rec);

    double A_cpu = 0, A_wait = 0, B_cpu = 0, C_cpu = 0, C_wait = 0;
    double cmdopen_total = 0, wait_in_cmd = 0, idle_in_cmd = 0, cpu_out_cmd = 0;
    long ncmd = 0, n_cpu_iv = 0, n_wait_iv = 0, n_straddle = 0;
    double straddle_ns = 0;
    double idle_wall = 0, cpu_meas = 0, cpu_wall_all = 0;
    long overlaps = 0; int npids = 0, npids_cmd = 0;
    long n_start = 0, n_end = 0, n_dup_start = 0, n_orphan_end = 0, n_unclosed = 0;
    long wh[24] = {0}, ch[24] = {0};      /* log2 us histograms: wait, cmd-open */
    double wait_lt1us = 0, wait_lt10us = 0;
    long n_wait_lt1us = 0, n_wait_lt10us = 0;
    double wait_clip_lo = 0, wait_clip_hi = 0; long n_wait_clip = 0;

    size_t i = 0;
    while (i < nrecs) {
        size_t j = i; uint32_t pid = recs[i].pid;
        while (j < nrecs && recs[j].pid == pid) j++;
        int seen_cmd = is_cmdpid(pid);
        npids++; if (seen_cmd) npids_cmd++;
        if (client_only && !seen_cmd) { i = j; continue; }

        size_t cmax = j - i + 2, cn = 0;
        uint64_t (*ci)[2] = malloc(cmax * sizeof(*ci));
        int open = 0; uint64_t anchor = 0;
        for (size_t k = i; k < j; k++) {
            if (recs[k].marker == 1) { n_start++; if (!open) { open = 1; anchor = recs[k].s; } else n_dup_start++; }
            else if (recs[k].marker == 2) {
                n_end++;
                if (open) { if (recs[k].s > anchor) { ci[cn][0] = anchor; ci[cn][1] = recs[k].s; cn++; } open = 0; }
                else n_orphan_end++;
            }
        }
        if (open) { n_unclosed++; }
        if (open) {
            uint64_t last = recs[j-1].e > anchor ? recs[j-1].e : anchor;
            if (last > anchor) { ci[cn][0] = anchor; ci[cn][1] = last; cn++; }
        }

        {
            int sopen = 0, sseen = 0; uint64_t sanchor = 0, banked = 0, prev_end = 0;
            int have_prev = 0;
            for (size_t k = i; k < j; k++) {
                struct rec *r = &recs[k];
                if (r->marker) {
                    sseen = 1;
                    if (r->marker == 1) { if (!sopen) { sopen = 1; sanchor = r->s; } }
                    else { if (sopen) { if (r->s > sanchor) banked += r->s - sanchor; sopen = 0; } }
                    continue;
                }
                uint64_t open_ns = banked;
                if (sopen) {
                    uint64_t a = sanchor > r->s ? sanchor : r->s;
                    if (r->e > a) open_ns += r->e - a;
                }
                banked = 0;
                if (sopen) sanchor = r->e;
                uint64_t dur = r->e > r->s ? r->e - r->s : 0;
                int in_cmd = !sseen ? 1 : (dur == 0 ? sopen : (open_ns * 2 >= dur));
                double frac = !sseen ? 1.0 : (dur == 0 ? (sopen ? 1.0 : 0.0)
                             : (double)(open_ns > dur ? dur : open_ns) / (double)dur);
                if (have_prev && r->s < prev_end) overlaps++;
                if (r->e > prev_end) prev_end = r->e;
                have_prev = 1;
                uint64_t cd = clipd(r->s, r->e);
                if (!cd) continue;
                if (pgwt_is_idle_event(r->we)) { idle_wall += (double)cd; continue; }
                if (r->we == 0) {
                    cpu_wall_all += (double)cd;
                    if (r->cpu != PGWT_CPU_NS_UNKNOWN) cpu_meas += (double)r->cpu;
                    if (in_cmd) A_cpu += (double)cd;
                    B_cpu += frac * (double)cd;
                } else A_wait += (double)cd;
            }
        }

        double p_cmdopen = 0, p_cpu = 0, p_wait = 0, p_idle = 0, p_first = 0, p_last = 0;
        for (size_t m = 0; m < cn; m++) {
            { uint64_t d = ci[m][1] - ci[m][0]; int b = 0; uint64_t v = d / 1000; while (v && b < 23) { v >>= 1; b++; } ch[b]++; }
            double cd = (double)clipd(ci[m][0], ci[m][1]);
            cmdopen_total += cd;
            p_cmdopen += cd;
            if (cd > 0) ncmd++;
        }
        for (size_t k = i; k < j; k++) {
            if (recs[k].marker) continue;
            double cd = (double)clipd(recs[k].s, recs[k].e);
            if (!cd) continue;
            if (p_first == 0) p_first = (double)(recs[k].s > win_from ? recs[k].s : win_from);
            p_last = (double)(recs[k].e < win_to ? recs[k].e : win_to);
            if (pgwt_is_idle_event(recs[k].we)) p_idle += cd;
            else if (recs[k].we == 0) p_cpu += cd;
            else p_wait += cd;
        }
        {
            size_t cur = 0;
            for (size_t k = i; k < j; k++) {
                struct rec *r = &recs[k];
                if (r->marker) continue;
                uint64_t cd = clipd(r->s, r->e);
                if (!cd) continue;
                uint64_t cs = r->s > win_from ? r->s : win_from;
                uint64_t ce = r->e < win_to ? r->e : win_to;
                uint64_t ov = 0;
                if (cn) {
                    while (cur > 0 && ci[cur-1][1] > cs) cur--;
                    while (cur < cn && ci[cur][1] <= cs) cur++;
                    for (size_t m = cur; m < cn && ci[m][0] < ce; m++) {
                        uint64_t a = ci[m][0] > cs ? ci[m][0] : cs;
                        uint64_t b = ci[m][1] < ce ? ci[m][1] : ce;
                        if (b > a) ov += b - a;
                    }
                }
                if (pgwt_is_idle_event(r->we)) { idle_in_cmd += (double)ov; continue; }
                if (r->we != 0) {
                    { uint64_t d = r->e - r->s; int b = 0; uint64_t v = d / 1000; while (v && b < 23) { v >>= 1; b++; } wh[b]++;
                      if (d < 1000) { n_wait_lt1us++; wait_lt1us += (double)d; }
                      if (d < 10000) { n_wait_lt10us++; wait_lt10us += (double)d; }
                      if (r->s < win_from) { wait_clip_lo += (double)(win_from - r->s); n_wait_clip++; }
                      if (r->e > win_to)   { wait_clip_hi += (double)(r->e - win_to); n_wait_clip++; } }
                    C_wait += (double)cd; evget(r->we)->c_ns += (double)cd;
                    wait_in_cmd += (double)ov;
                    n_wait_iv++;
                    continue;
                }
                n_cpu_iv++;
                if (cn == 0) { C_cpu += (double)cd; evget(0)->c_ns += (double)cd; continue; }
                if (ov > 0 && ov < cd) { n_straddle++; straddle_ns += (double)(cd - ov); }
                C_cpu += (double)ov;
                cpu_out_cmd += (double)(cd - ov);
                evget(0)->c_ns += (double)ov;
            }
        }
        if (per_pid)
            printf("PID %u covered=%.2fs span=%.2fs cmdopen=%.2fs (%.1f%%) cpu_gap=%.2fs wait=%.2fs idle=%.2fs\n",
                   pid, (p_cpu + p_wait + p_idle) / 1e9, (p_last - p_first) / 1e9,
                   p_cmdopen / 1e9,
                   (p_cpu + p_wait + p_idle) > 0 ? 100.0 * p_cmdopen / (p_cpu + p_wait + p_idle) : 0,
                   p_cpu / 1e9, p_wait / 1e9, p_idle / 1e9);
        free(ci);
        i = j;
    }

    printf("PIDS total=%d with_cmd_markers=%d overlapping_intervals=%ld client_only=%d\n",
           npids, npids_cmd, overlaps, client_only);
    printf("IDLE raw wall = %.3f s (of which inside a command: %.3f s)\n",
           idle_wall / 1e9, idle_in_cmd / 1e9);
    printf("CMD-OPEN total = %.3f s over %ld commands (mean %.1f us)   wait inside cmd = %.3f s   cpu outside cmd = %.3f s\n",
           cmdopen_total / 1e9, ncmd, ncmd ? cmdopen_total / ncmd / 1e3 : 0,
           wait_in_cmd / 1e9, cpu_out_cmd / 1e9);
    printf("INTERVALS cpu=%ld (mean %.1f us) wait=%ld (mean %.1f us) straddling=%ld (%.3f s outside)\n",
           n_cpu_iv, n_cpu_iv ? cpu_wall_all / n_cpu_iv / 1e3 : 0,
           n_wait_iv, n_wait_iv ? A_wait / n_wait_iv / 1e3 : 0,
           n_straddle, straddle_ns / 1e9);
    printf("CPU(we==0) wall = %.3f s   measured cpu_ns = %.3f s\n",
           cpu_wall_all / 1e9, cpu_meas / 1e9);
    printf("MARKERS start=%ld end=%ld dup_start(missing END)=%ld orphan_end(missing START)=%ld pids_unclosed_at_end=%ld\n",
           n_start, n_end, n_dup_start, n_orphan_end, n_unclosed);
    printf("WAIT-SHORT n<1us=%ld (%.3f s)  n<10us=%ld (%.3f s)  clipped_at_edges=%ld (%.3f s lo + %.3f s hi)\n",
           n_wait_lt1us, wait_lt1us / 1e9, n_wait_lt10us, wait_lt10us / 1e9,
           n_wait_clip, wait_clip_lo / 1e9, wait_clip_hi / 1e9);
    printf("HIST-WAIT(us log2)");
    for (int b = 0; b < 24; b++) if (wh[b]) printf(" %d:%ld", b, wh[b]);
    printf("\nHIST-CMDOPEN(us log2)");
    for (int b = 0; b < 24; b++) if (ch[b]) printf(" %d:%ld", b, ch[b]);
    printf("\n");
    printf("SAMPLED                        cpu=%.3fs nonidle=%.3fs  share=%.1f%%  total=%.3fs active_frac=%.1f%% noncmd_cpu=%.3fs\n",
           sm_cpu / 1e9, sm_nonidle / 1e9, sm_nonidle > 0 ? 100.0 * sm_cpu / sm_nonidle : 0,
           sm_total / 1e9, sm_total > 0 ? 100.0 * sm_nonidle / sm_total : 0, sm_noncmd / 1e9);
    printf("EXACT S (stream, majority)     cpu=%.3fs wait=%.3fs  share=%.1f%%\n",
           S_cpu / 1e9, S_wait / 1e9, 100.0 * S_cpu / (S_cpu + S_wait));
    printf("EXACT A (sorted, majority)     cpu=%.3fs wait=%.3fs  share=%.1f%%\n",
           A_cpu / 1e9, A_wait / 1e9, 100.0 * A_cpu / (A_cpu + A_wait));
    printf("EXACT B (sorted, proportional) cpu=%.3fs wait=%.3fs  share=%.1f%%\n",
           B_cpu / 1e9, A_wait / 1e9, 100.0 * B_cpu / (B_cpu + A_wait));
    printf("EXACT C (2-pass, proportional) cpu=%.3fs wait=%.3fs  share=%.1f%%\n",
           C_cpu / 1e9, C_wait / 1e9, 100.0 * C_cpu / (C_cpu + C_wait));

    struct ev list[ET]; int nl = 0;
    for (int k = 0; k < ET; k++)
        if (evt[k].used && (evt[k].c_ns > 0 || evt[k].sm_ns > 0)) list[nl++] = evt[k];
    qsort(list, nl, sizeof(list[0]), cmp_ev);
    double tC = C_cpu + C_wait, tS = sm_cpu + sm_nonidle - sm_cpu;
    (void)tS;
    printf("\n%-24s %10s %10s %10s %10s\n", "event", "S_sec", "C_sec", "C_share", "smp_share");
    for (int k = 0; k < nl && k < 12; k++)
        printf("%-24s %10.2f %10.2f %9.1f%% %9.1f%%\n",
               pgwt_event_name(list[k].id), list[k].s_ns / 1e9, list[k].c_ns / 1e9,
               100.0 * list[k].c_ns / tC,
               sm_nonidle > 0 ? 100.0 * list[k].sm_ns / sm_nonidle : 0);
    return 0;
}
