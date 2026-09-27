/* wait_cpu_canary.c — offline per-class wait-CPU canary (issue #202).
 *
 * WHAT IT ASKS. Every trace record carries the exact on-CPU nanoseconds the
 * backend burned inside the interval it closes (`pgwt_trace_event.cpu_ns`,
 * the se.sum_exec_runtime delta). For a record whose old_event is a WAIT,
 * that number is a self-check the product never spends: a sleeping task
 * accrues no CPU.
 *
 *   - `Timeout:PgSleep` and the `Lock` class are PURE SLEEPS. A correct
 *     implementation records essentially no on-CPU time inside them —
 *     bounded by the odd deadlock_timeout wakeup, which is tens of
 *     microseconds of deadlock checking per SECOND of waiting. Millisecond-
 *     scale cpu_ns on one of those events is an over-attribution signature:
 *     a stale wait label (the interval was not really spent waiting), or an
 *     `on_cpu_ts` that was opened and never closed.
 *   - The `IO` class is the OPPOSITE and legitimately carries CPU: BPF
 *     measures on-CPU between the wait-start and wait-end writes, which
 *     spans the syscall's own on-CPU work, so a `pwrite` into page cache is
 *     nearly all on-CPU under an IO label. A non-zero IO figure is NOT a
 *     defect and this tool never flags it.
 *   - `cpu_ns > duration_ns` on ANY event, any class, is a defect outright:
 *     a task cannot be on a CPU for longer than the interval it was in.
 *     src/compute.c's wait branch (the `wait_gap_cpu_ns` accumulation) does
 *     NOT clamp this — only the CPU-gap branch does — so `cpu_clamped_ms`
 *     vouches for on-CPU gaps alone and says nothing about wait events.
 *     Nothing else in the tree catches it; this does.
 *
 * THRESHOLDS, derived rather than tuned. PostgreSQL wakes a lock waiter at
 * most once per `deadlock_timeout` (1 s by default) and the deadlock check
 * is tens of microseconds; pg_sleep wakes once, at the end. So a pure-sleep
 * class should sit around 0.01% cpu/dur even pathologically. The class
 * ratio limit is 0.1% — a 10x margin over that — and the per-event limit is
 * 1 ms of cpu_ns, the scale at which a single event stops being explicable
 * as wakeup bookkeeping.
 *
 * A CANARY THAT CANNOT SEE MUST REFUSE. No trace files, no events, or no
 * event carrying a measured cpu_ns at all (every record
 * PGWT_CPU_NS_UNKNOWN: a v2 file, the sampled tier, a box with no BTF) is
 * reported as "cannot check" and exits 2. It is never reported as clean.
 *
 * Built with -DPGWT_SERVER (no BPF), like cross_validate / dump_markers.
 *
 * Usage: wait_cpu_canary <trace_dir> [<trace_dir> ...]
 * Exit:  0 clean, 1 defect signature found, 2 cannot check / usage.
 */
#include "event_reader.h"
#include "wait_event.h"
#include "compute.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WCC_RATIO_LIMIT_PCT   0.1     /* pure-sleep class Σcpu/Σdur */
#define WCC_EVENT_LIMIT_NS    1000000ULL  /* pure-sleep single event: 1 ms */
#define WCC_MAX_EVENTS        65536
#define WCC_MAX_SLEEP_EVENTS  256

enum {
    WCC_CLEAN      = 0,
    WCC_DEFECT     = 1,
    WCC_CANNOT_SEE = 2,
};

struct wcc_stats {
    uint64_t n_events;       /* records seen with this old_event/class */
    uint64_t n_measured;     /* … of which carried a real cpu_ns */
    uint64_t sum_dur_ns;
    uint64_t sum_cpu_ns;     /* measured records only */
    uint64_t max_cpu_ns;
    uint64_t n_cpu_gt_dur;   /* the impossible ones */
    uint64_t max_excess_ns;  /* max(cpu_ns - duration_ns) over those */
};

/* Classes that are pure sleeps: a backend in one of these is blocked, not
 * running. IO is deliberately absent (it legitimately carries CPU, see the
 * header comment); so are CPU/Activity/Unknown, which are not waits.
 * LWLock, IPC and BufferPin can legitimately spin before blocking, so they
 * are reported but not flagged. */
int wcc_is_pure_sleep_class(int cls)
{
    return cls == PGWT_CLASS_LOCK || cls == PGWT_CLASS_TIMEOUT;
}

void wcc_add(struct wcc_stats *s, uint64_t dur_ns, uint64_t cpu_ns)
{
    s->n_events++;
    s->sum_dur_ns += dur_ns;
    if (cpu_ns == PGWT_CPU_NS_UNKNOWN)
        return;                       /* not measured: never counted as 0 */
    s->n_measured++;
    s->sum_cpu_ns += cpu_ns;
    if (cpu_ns > s->max_cpu_ns)
        s->max_cpu_ns = cpu_ns;
    if (cpu_ns > dur_ns) {
        s->n_cpu_gt_dur++;
        uint64_t excess = cpu_ns - dur_ns;
        if (excess > s->max_excess_ns)
            s->max_excess_ns = excess;
    }
}

double wcc_ratio_pct(const struct wcc_stats *s)
{
    return s->sum_dur_ns ? 100.0 * (double)s->sum_cpu_ns / (double)s->sum_dur_ns
                         : 0.0;
}

/* Why this row is (or is not) a defect. Returns 0 when clean. Bit 1: the
 * class ratio is over the pure-sleep limit. Bit 2: a single pure-sleep
 * event carried >= 1 ms. Bit 4: cpu_ns > duration_ns (any class). */
#define WCC_FLAG_RATIO    1
#define WCC_FLAG_EVENT    2
#define WCC_FLAG_OVER_DUR 4

int wcc_flags(const struct wcc_stats *s, int cls)
{
    int f = 0;
    if (s->n_cpu_gt_dur > 0)
        f |= WCC_FLAG_OVER_DUR;
    if (!wcc_is_pure_sleep_class(cls) || s->n_measured == 0)
        return f;
    if (wcc_ratio_pct(s) >= WCC_RATIO_LIMIT_PCT)
        f |= WCC_FLAG_RATIO;
    if (s->max_cpu_ns >= WCC_EVENT_LIMIT_NS)
        f |= WCC_FLAG_EVENT;
    return f;
}

/* The run's verdict. `by_class` is indexed by PGWT_CLASS_*; `total_events`
 * and `total_measured` are over every non-marker record, including classes
 * this tool does not flag — because "nothing was measured" must refuse even
 * when the pure-sleep classes happen to be empty. */
int wcc_verdict(const struct wcc_stats *by_class, uint64_t total_events,
                uint64_t total_measured)
{
    if (total_events == 0 || total_measured == 0)
        return WCC_CANNOT_SEE;
    for (int c = 0; c < PGWT_NUM_CLASSES; c++)
        if (wcc_flags(&by_class[c], c) != 0)
            return WCC_DEFECT;
    return WCC_CLEAN;
}

#ifndef WCC_NO_MAIN

struct wcc_event_row {
    uint32_t we;
    struct wcc_stats s;
};

static struct wcc_event_row *find_event_row(struct wcc_event_row *rows,
                                            int *n, uint32_t we)
{
    for (int i = 0; i < *n; i++)
        if (rows[i].we == we)
            return &rows[i];
    if (*n >= WCC_MAX_SLEEP_EVENTS)
        return NULL;
    struct wcc_event_row *r = &rows[(*n)++];
    memset(r, 0, sizeof(*r));
    r->we = we;
    return r;
}

static const char *flag_text(int f, char *buf, size_t bufsz)
{
    if (f == 0) { snprintf(buf, bufsz, "-"); return buf; }
    snprintf(buf, bufsz, "%s%s%s",
             (f & WCC_FLAG_RATIO)    ? "RATIO " : "",
             (f & WCC_FLAG_EVENT)    ? "EVENT " : "",
             (f & WCC_FLAG_OVER_DUR) ? "CPU>DUR" : "");
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace_dir> [<trace_dir> ...]\n", argv[0]);
        return WCC_CANNOT_SEE;
    }

    struct wcc_stats by_class[PGWT_NUM_CLASSES];
    memset(by_class, 0, sizeof(by_class));
    struct wcc_event_row sleep_rows[WCC_MAX_SLEEP_EVENTS];
    int n_sleep_rows = 0;
    uint64_t total_events = 0, total_measured = 0, total_files = 0;
    uint64_t total_records = 0, skipped_markers = 0, skipped_samples = 0;

    struct pgwt_trace_event *evs = malloc(WCC_MAX_EVENTS * sizeof(*evs));
    if (!evs) { perror("malloc"); return WCC_CANNOT_SEE; }

    for (int a = 1; a < argc; a++) {
        struct pgwt_trace_file_entry files[256];
        int nfiles = pgwt_scan_trace_files(argv[a], files, 256);
        if (nfiles <= 0) {
            fprintf(stderr, "wait_cpu_canary: no trace files in %s\n", argv[a]);
            continue;
        }
        for (int fi = 0; fi < nfiles; fi++) {
            struct pgwt_event_reader r;
            if (pgwt_reader_open(&r, files[fi].path) != 0) {
                fprintf(stderr, "wait_cpu_canary: cannot open %s\n",
                        files[fi].path);
                continue;
            }
            total_files++;
            printf("# trace: %s (%d block(s))\n", files[fi].path, r.num_blocks);
            for (int b = 0; b < r.num_blocks; b++) {
                int n = pgwt_reader_decode_block(&r, b, evs, WCC_MAX_EVENTS);
                if (n <= 0)
                    continue;
                for (int i = 0; i < n; i++) {
                    struct pgwt_trace_event *e = &evs[i];
                    total_records++;
                    /* Markers carry no interval; SAMPLES records are point
                     * observations with duration 0 and no measured cpu. */
                    if (PGWT_IS_MARKER(e->old_event) ||
                        PGWT_IS_MARKER(e->new_event)) {
                        skipped_markers++;
                        continue;
                    }
                    if (e->flags & PGWT_EVENT_FLAG_SAMPLE) {
                        skipped_samples++;
                        continue;
                    }
                    int cls = pgwt_wait_class_index(e->old_event);
                    if (cls < 0 || cls >= PGWT_NUM_CLASSES)
                        cls = PGWT_CLASS_UNKNOWN;
                    wcc_add(&by_class[cls], e->duration_ns, e->cpu_ns);
                    total_events++;
                    if (e->cpu_ns != PGWT_CPU_NS_UNKNOWN)
                        total_measured++;
                    if (wcc_is_pure_sleep_class(cls)) {
                        struct wcc_event_row *row =
                            find_event_row(sleep_rows, &n_sleep_rows,
                                           e->old_event);
                        if (row)
                            wcc_add(&row->s, e->duration_ns, e->cpu_ns);
                    }
                }
            }
            pgwt_reader_close(&r);
        }
    }
    free(evs);

    printf("\n# records=%llu  intervals=%llu  measured=%llu  "
           "markers=%llu  samples=%llu  files=%llu\n",
           (unsigned long long)total_records,
           (unsigned long long)total_events,
           (unsigned long long)total_measured,
           (unsigned long long)skipped_markers,
           (unsigned long long)skipped_samples,
           (unsigned long long)total_files);

    printf("\n%-10s %9s %9s %14s %14s %8s %14s %10s  %s\n",
           "class", "events", "measured", "Sum dur (ms)", "Sum cpu (ms)",
           "cpu/dur%", "max cpu (us)", "cpu>dur", "flag");
    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        struct wcc_stats *s = &by_class[c];
        if (s->n_events == 0)
            continue;
        char fb[32];
        printf("%-10s %9llu %9llu %14.1f %14.3f %8.4f %14.1f %10llu  %s%s\n",
               pgwt_class_display[c],
               (unsigned long long)s->n_events,
               (unsigned long long)s->n_measured,
               s->sum_dur_ns / 1e6, s->sum_cpu_ns / 1e6,
               wcc_ratio_pct(s), s->max_cpu_ns / 1e3,
               (unsigned long long)s->n_cpu_gt_dur,
               flag_text(wcc_flags(s, c), fb, sizeof(fb)),
               wcc_is_pure_sleep_class(c) ? "  [pure sleep]" : "");
    }

    if (n_sleep_rows > 0) {
        printf("\n# pure-sleep classes, per event\n");
        printf("%-34s %9s %9s %14s %14s %8s %14s  %s\n",
               "event", "events", "measured", "Sum dur (ms)", "Sum cpu (ms)",
               "cpu/dur%", "max cpu (us)", "flag");
        for (int i = 0; i < n_sleep_rows; i++) {
            char name[64], fb[32];
            pgwt_event_full_name(sleep_rows[i].we, name, sizeof(name));
            struct wcc_stats *s = &sleep_rows[i].s;
            int cls = pgwt_wait_class_index(sleep_rows[i].we);
            printf("%-34s %9llu %9llu %14.1f %14.3f %8.4f %14.1f  %s\n",
                   name,
                   (unsigned long long)s->n_events,
                   (unsigned long long)s->n_measured,
                   s->sum_dur_ns / 1e6, s->sum_cpu_ns / 1e6,
                   wcc_ratio_pct(s), s->max_cpu_ns / 1e3,
                   flag_text(wcc_flags(s, cls), fb, sizeof(fb)));
        }
    }

    int v = wcc_verdict(by_class, total_events, total_measured);
    printf("\nVERDICT: %s\n",
           v == WCC_CLEAN ? "CLEAN — no wait-CPU over-attribution signature"
         : v == WCC_DEFECT ? "DEFECT — see the flagged rows above"
         : "CANNOT CHECK — no measured cpu_ns in this trace (refusing to "
           "report clean)");
    return v;
}

#endif /* WCC_NO_MAIN */
