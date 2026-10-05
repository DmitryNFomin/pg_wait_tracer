/* test_concurrency_bounds.c — #276: the five silent bounds in
 * pgwt_compute_concurrency, and the one that made a 15-minute window render
 * 8% of its data.
 *
 * THE DEFECT. Candidate intervals were materialised into a fixed
 * 100,000-entry array filled in ARRIVAL order ("limit for memory"), so the
 * intervals kept were the OLDEST in the window and every bucket after them
 * came back `max: 0` — indistinguishable from an idle database, with
 * "fidelity": "exact" beside it. Measured on the demo workload at the UI's
 * default 900 s window: the last non-zero bucket was 4 of 59 (8%). Four more
 * bounds in the same function were equally silent: ev_pids[64] distinct
 * events per bucket, pids[128] distinct pids per event per bucket,
 * burst_cap=256 bursts, and pids[64] per burst — the last of which also
 * capped the burst's REPORTED session count at 64.
 *
 * WHY THE FIXTURE LOOKS LIKE THIS. Section 1's window holds ~153,000
 * qualifying intervals spread EVENLY over 60 buckets, each interval short
 * enough to live inside exactly one bucket, and every bucket's expected peak
 * is a closed form of the generator's parameters (never read back from the
 * implementation). Three properties are load-bearing and section 2 proves
 * each one by running a VERBATIM COPY of the deleted code against variants
 * that lack it:
 *   - more than 100,000 qualifying intervals (a smaller fixture is GREEN on
 *     the broken code: the cap never fires);
 *   - intervals confined to one bucket (intervals spanning the window are
 *     GREEN on the broken code: a prefix of long intervals touches every
 *     bucket);
 *   - exact peak VALUES and the peak EVENT, not just "non-zero" (counting
 *     intervals instead of distinct pids, or dropping the bucket from the
 *     key, both survive a non-zero check).
 * Pids are deliberately REUSED across buckets, so a distinct-pid table keyed
 * without the bucket counts zero new pids after the first bucket and the
 * assertions go red.
 *
 * SECTION 4 IS ABOUT FALSE NEGATIVES — every way this check could pass
 * without checking anything, or be unable to see: no events at all, a
 * degenerate window, a window where nothing QUALIFIES (absent rather than
 * wrong — and the section-1 assertion must be able to tell those apart),
 * allocation failures in both growing structures (which must come back
 * ABSENT, failed=1, never short), and an UNKNOWN injection point that must
 * leave the answer complete — so an injection hook that silently did nothing
 * cannot be what made the failure cases pass.
 *
 * Runs anywhere: synthetic timestamps, no clock, no I/O, no threads, no
 * subprocess. qsort is not stable, so every comparator the result depends on
 * is a total order and section 5 pins that by recomputing and comparing.
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                        \
    checks++;                                                        \
    if (!(cond)) { failures++;                                       \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); }   \
    else printf("  ok: %s\n", msg);                                  \
} while (0)

#define CHECK_INT(got, want, msg) do {                               \
    checks++;                                                        \
    long _g = (long)(got), _w = (long)(want);                        \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %ld, want %ld (%s:%d)\n", msg,      \
               _g, _w, __FILE__, __LINE__); }                        \
    else printf("  ok: %s (%ld)\n", msg, _g);                        \
} while (0)

/* compute.c's only foreign symbol (summary streaming) — unused here. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

/* ── A verbatim copy of the deleted phase 1 ───────────────────────────────
 * src/compute.c at 1a728be, lines 2772-2843: the 100,000-interval cap,
 * ev_pids[64] and pids[128]. Kept so this file can show the defect going RED
 * on the same fixtures that prove the fix, forever, without a second
 * checkout. Only the output plumbing differs (peaks into caller arrays).
 */
struct legacy_entry {
    uint32_t pid;
    uint32_t event_id;
    uint64_t start_ns;
    uint64_t end_ns;
};

static int legacy_cmp_by_start(const void *a, const void *b)
{
    uint64_t sa = ((const struct legacy_entry *)a)->start_ns;
    uint64_t sb = ((const struct legacy_entry *)b)->start_ns;
    return (sa > sb) - (sa < sb);
}

static void legacy_peaks(const struct pgwt_trace_event *events, int count,
                         const struct pgwt_filter *f,
                         uint64_t from_ns, uint64_t to_ns, int num_buckets,
                         int *peak_sessions, uint32_t *peak_event)
{
    uint64_t bucket_ns = (to_ns - from_ns) / num_buckets;
    if (bucket_ns == 0) bucket_ns = 1;
    memset(peak_sessions, 0, (size_t)num_buckets * sizeof(*peak_sessions));
    memset(peak_event, 0, (size_t)num_buckets * sizeof(*peak_event));

    int cap = count < 100000 ? count : 100000;  /* limit for memory */
    struct legacy_entry *active = malloc((size_t)cap * sizeof(*active));
    int nactive = 0;

    for (int i = 0; i < count && nactive < cap; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->old_event == 0) continue;
        if (ev->timestamp_ns < from_ns || ev->timestamp_ns > to_ns)
            continue;
        active[nactive].pid = ev->pid;
        active[nactive].event_id = ev->old_event;
        active[nactive].start_ns = ev->timestamp_ns - ev->duration_ns;
        active[nactive].end_ns = ev->timestamp_ns;
        nactive++;
    }
    qsort(active, nactive, sizeof(active[0]), legacy_cmp_by_start);

    for (int b = 0; b < num_buckets; b++) {
        uint64_t bstart = from_ns + (uint64_t)b * bucket_ns;
        uint64_t bend = bstart + bucket_ns;
        struct {
            uint32_t eid;
            uint32_t pids[128];
            int npids;
        } ev_pids[64];
        int nev = 0;

        for (int i = 0; i < nactive; i++) {
            if (active[i].start_ns >= bend) break;
            if (active[i].end_ns <= bstart) continue;
            int found = -1;
            for (int j = 0; j < nev; j++)
                if (ev_pids[j].eid == active[i].event_id) { found = j; break; }
            if (found < 0 && nev < 64) {
                found = nev;
                ev_pids[nev].eid = active[i].event_id;
                ev_pids[nev].npids = 0;
                nev++;
            }
            if (found < 0) continue;
            int dup = 0;
            for (int k = 0; k < ev_pids[found].npids; k++)
                if (ev_pids[found].pids[k] == active[i].pid) { dup = 1; break; }
            if (!dup && ev_pids[found].npids < 128)
                ev_pids[found].pids[ev_pids[found].npids++] = active[i].pid;
        }
        for (int j = 0; j < nev; j++)
            if (ev_pids[j].npids > peak_sessions[b]) {
                peak_sessions[b] = ev_pids[j].npids;
                peak_event[b] = ev_pids[j].eid;
            }
    }
    free(active);
}

/* ── Fixtures ─────────────────────────────────────────────────────────── */

#define BASE_NS     1700000000000000000ULL
#define WINDOW_NS   (900ULL * 1000000000ULL)   /* the UI's default window */
#define U_BUCKETS   60
#define U_REPS      300            /* ~153,000 intervals: over the 100k cap */
#define DUR_NS      1000000ULL     /* 1 ms — each interval fits one bucket */
#define EV1         0x01000001u    /* LWLock class: not idle, not a marker */
#define EV2         0x01000002u
#define BURST_W_NS  10000000ULL    /* 10 ms, as src/server.c passes */
#define BURST_MIN   4              /* 4+ sessions, as src/server.c passes */

struct evbuf {
    struct pgwt_trace_event *ev;
    int n, cap;
};

static void push(struct evbuf *b, uint32_t pid, uint32_t eid,
                 uint64_t start_ns, uint64_t dur_ns)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->ev = realloc(b->ev, (size_t)b->cap * sizeof(*b->ev));
        if (!b->ev) { printf("  FATAL: fixture OOM\n"); exit(2); }
    }
    struct pgwt_trace_event *e = &b->ev[b->n++];
    memset(e, 0, sizeof(*e));
    e->pid = pid;
    e->old_event = eid;
    e->duration_ns = dur_ns;
    e->timestamp_ns = start_ns + dur_ns;
    e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
}

/* How many distinct pids wait on EV1 / EV2 inside bucket b. Even buckets put
 * the crowd on EV1, odd buckets on EV2, so peak_event is a real assertion and
 * not a constant; the quiet event always has 2 pids, below BURST_MIN. */
static int kpids(int b, int which)
{
    if (b % 2 == 0) return which == 1 ? 4 + (b % 6) : 2;
    return which == 1 ? 2 : 4 + (b % 5);
}
static int expect_peak(int b)
{
    int a = kpids(b, 1), c = kpids(b, 2);
    return a > c ? a : c;
}
static uint32_t expect_event(int b) { return b % 2 == 0 ? EV1 : EV2; }

/* The uniform window: `reps` onsets in every one of `nbuckets` buckets,
 * generated oldest-first (the order a trace file is read in), each interval
 * 1 ms long and wholly inside its bucket. `span_window`, when set, stretches
 * every interval to cover the WHOLE window instead — the shape that makes the
 * broken code look correct (section 2.3). */
static void build_uniform(struct evbuf *b, int nbuckets, int reps,
                          int span_window)
{
    uint64_t bucket_ns = WINDOW_NS / (uint64_t)nbuckets;
    uint64_t step = bucket_ns / (uint64_t)(reps + 2);
    for (int bk = 0; bk < nbuckets; bk++) {
        uint64_t bstart = BASE_NS + (uint64_t)bk * bucket_ns;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = bstart + (uint64_t)(r + 1) * step;
            uint64_t off = 0;
            for (int which = 1; which <= 2; which++) {
                uint32_t pid_base = which == 1 ? 1000 : 2000;
                uint32_t eid = which == 1 ? EV1 : EV2;
                int k = kpids(bk, which);
                for (int p = 0; p < k; p++) {
                    uint64_t start = t0 + off;
                    off += 1000;   /* 1 us apart: one onset per repeat */
                    if (span_window) {
                        /* [window start, window end]: overlaps EVERY bucket,
                         * so even a 100,000-interval prefix fills them all. */
                        (void)start;
                        push(b, pid_base + (uint32_t)p, eid, BASE_NS, WINDOW_NS);
                    } else {
                        push(b, pid_base + (uint32_t)p, eid, start, DUR_NS);
                    }
                }
            }
        }
    }
}

/* The anchor of bucket bk's burst: the first entry of its first repeat on the
 * crowded event. Even buckets crowd EV1 (offset 0); odd buckets crowd EV2,
 * which is emitted after EV1's two quiet entries (offset 2 us). */
static uint64_t expect_burst_ts(int bk, int nbuckets, int reps)
{
    uint64_t bucket_ns = WINDOW_NS / (uint64_t)nbuckets;
    uint64_t step = bucket_ns / (uint64_t)(reps + 2);
    uint64_t t0 = BASE_NS + (uint64_t)bk * bucket_ns + step;
    return bk % 2 == 0 ? t0 : t0 + 2 * 1000;
}

static void free_result(struct pgwt_concurrency_result *r)
{
    free(r->peak_sessions);
    free(r->peak_event);
    free(r->bursts);
    memset(r, 0, sizeof(*r));
}

int main(void)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    const uint64_t to_ns = BASE_NS + WINDOW_NS;

    printf("=== #276: concurrency bounds ===\n\n");

    /* ── 1. Acceptance criterion: every bucket of a uniform window ──── */
    printf("1. A uniform 900 s window: every bucket, exact peaks\n");
    struct evbuf big = {0};
    build_uniform(&big, U_BUCKETS, U_REPS, 0);
    printf("  fixture: %d events over %d buckets (cap was 100000)\n",
           big.n, U_BUCKETS);
    CHECK(big.n > 100000, "fixture exceeds the old 100,000-interval cap");

    struct pgwt_concurrency_result res;
    pgwt_compute_concurrency(big.ev, big.n, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &res);
    CHECK_INT(res.failed, 0, "complete answer, not a refusal");
    CHECK_INT(res.num_buckets, U_BUCKETS, "bucket count");
    CHECK_INT((long)res.bucket_ns, (long)(WINDOW_NS / U_BUCKETS), "bucket_ns");

    int empty = 0, wrong = 0, wrong_ev = 0, last_nonzero = -1;
    for (int b = 0; b < U_BUCKETS; b++) {
        if (res.peak_sessions[b] == 0) empty++;
        else last_nonzero = b;
        if (res.peak_sessions[b] != expect_peak(b)) wrong++;
        if (res.peak_event[b] != expect_event(b)) wrong_ev++;
    }
    printf("  last non-zero bucket: %d of %d\n", last_nonzero, U_BUCKETS - 1);
    CHECK_INT(empty, 0, "NO bucket is empty (the criterion)");
    CHECK_INT(wrong, 0, "every bucket's peak equals the expected distinct pids");
    CHECK_INT(wrong_ev, 0, "every bucket's peak_event is the crowded event");

    /* Bursts must cover the window too: one per bucket, the largest in it. */
    CHECK_INT(res.num_bursts, U_BUCKETS, "a burst reported for every bucket");
    CHECK_INT(res.bursts_total, U_BUCKETS * U_REPS,
              "bursts_total counts every onset detected");
    int seen_bucket[U_BUCKETS];
    memset(seen_bucket, 0, sizeof(seen_bucket));
    int burst_bad = 0, burst_ts_bad = 0, burst_pid_bad = 0;
    for (int i = 0; i < res.num_bursts; i++) {
        int bk = (int)((res.bursts[i].timestamp_ns - BASE_NS) / res.bucket_ns);
        if (bk < 0 || bk >= U_BUCKETS) { burst_bad++; continue; }
        seen_bucket[bk]++;
        if (res.bursts[i].num_sessions != expect_peak(bk)) burst_bad++;
        if (res.bursts[i].event_id != expect_event(bk)) burst_bad++;
        if (res.bursts[i].timestamp_ns != expect_burst_ts(bk, U_BUCKETS, U_REPS))
            burst_ts_bad++;
        /* Under PGWT_BURST_PID_SAMPLE the list is the whole burst. */
        if (res.bursts[i].num_pids != res.bursts[i].num_sessions) burst_pid_bad++;
    }
    int dup_bucket = 0;
    for (int b = 0; b < U_BUCKETS; b++) if (seen_bucket[b] != 1) dup_bucket++;
    CHECK_INT(burst_bad, 0, "each burst's sessions and event match its bucket");
    CHECK_INT(burst_ts_bad, 0, "each burst is anchored at its first onset");
    CHECK_INT(burst_pid_bad, 0, "pid list is complete below the sample bound");
    CHECK_INT(dup_bucket, 0, "exactly one burst per bucket, none repeated");
    int desc_bad = 0;
    for (int i = 1; i < res.num_bursts; i++)
        if (res.bursts[i - 1].num_sessions < res.bursts[i].num_sessions) desc_bad++;
    CHECK_INT(desc_bad, 0, "bursts are returned largest first");

    /* ── 2. The same fixtures against the deleted code ──────────────── */
    printf("\n2. The deleted code on the same inputs (the RED side)\n");
    int *lp = calloc(U_BUCKETS, sizeof(int));
    uint32_t *le = calloc(U_BUCKETS, sizeof(uint32_t));
    legacy_peaks(big.ev, big.n, &f, BASE_NS, to_ns, U_BUCKETS, lp, le);
    int lempty = 0, llast = -1;
    for (int b = 0; b < U_BUCKETS; b++) {
        if (lp[b] == 0) lempty++; else llast = b;
    }
    printf("  deleted code: %d empty buckets, last non-zero %d of %d\n",
           lempty, llast, U_BUCKETS - 1);
    CHECK(lempty > 0, "2.1 the 100,000 cap DOES blank buckets on this fixture");
    CHECK(llast < U_BUCKETS - 1, "2.1 and the blanked buckets are the LATEST");

    /* 2.2 A fixture below the cap is BLIND: the broken code passes it. */
    struct evbuf small = {0};
    build_uniform(&small, U_BUCKETS, 2, 0);
    printf("  blind fixture: %d events (under the cap)\n", small.n);
    legacy_peaks(small.ev, small.n, &f, BASE_NS, to_ns, U_BUCKETS, lp, le);
    int sempty = 0;
    for (int b = 0; b < U_BUCKETS; b++) if (lp[b] == 0) sempty++;
    CHECK_INT(sempty, 0,
              "2.2 a sub-cap fixture is GREEN on the broken code (never shrink it)");

    /* 2.3 Window-spanning intervals are BLIND: a prefix touches every bucket. */
    struct evbuf spanning = {0};
    build_uniform(&spanning, U_BUCKETS, U_REPS, 1);
    legacy_peaks(spanning.ev, spanning.n, &f, BASE_NS, to_ns, U_BUCKETS, lp, le);
    int spempty = 0;
    for (int b = 0; b < U_BUCKETS; b++) if (lp[b] == 0) spempty++;
    CHECK_INT(spempty, 0,
              "2.3 window-spanning intervals are GREEN on the broken code too");
    /* ...and the fix must agree with itself on that shape: every bucket sees
     * every pid, because every interval overlaps every bucket. */
    struct pgwt_concurrency_result sres;
    pgwt_compute_concurrency(spanning.ev, spanning.n, &f, BASE_NS, to_ns,
                             U_BUCKETS, BURST_W_NS, BURST_MIN, &sres);
    /* Every interval covers every bucket, so each bucket's peak is the widest
     * pid set either event ever uses anywhere in the window. */
    int want_span = 0;
    for (int src = 0; src < U_BUCKETS; src++) {
        int a = kpids(src, 1), c = kpids(src, 2);
        if (a > want_span) want_span = a;
        if (c > want_span) want_span = c;
    }
    int span_wrong = 0;
    for (int b = 0; b < U_BUCKETS; b++)
        if (sres.peak_sessions[b] != want_span) span_wrong++;
    CHECK_INT(span_wrong, 0, "2.3 spanning intervals counted in every bucket");
    free_result(&sres);
    free(spanning.ev);
    free(small.ev);

    /* ── 3. The other four bounds ───────────────────────────────────── */
    printf("\n3. The other four bounds in the same function\n");

    /* 3.1 ev_pids[64]: 70 distinct events in one bucket, the crowd on the
     * 65th+ — exactly the ones the fixed table dropped. */
    struct evbuf many_ev = {0};
    uint64_t one_bucket_to = BASE_NS + 1000000000ULL;   /* 1 s, 1 bucket */
    for (int n = 0; n < 70; n++) {
        int k = n >= 64 ? 5 : 2;
        for (int p = 0; p < k; p++)
            push(&many_ev, 3000 + (uint32_t)p, 0x01000000u + (uint32_t)n,
                 BASE_NS + 1000000ULL + (uint64_t)(n * 70 + p) * 1000ULL, DUR_NS);
    }
    struct pgwt_concurrency_result mres;
    pgwt_compute_concurrency(many_ev.ev, many_ev.n, &f, BASE_NS, one_bucket_to,
                             1, BURST_W_NS, BURST_MIN, &mres);
    CHECK_INT(mres.peak_sessions[0], 5, "3.1 the 65th+ distinct event is counted");
    CHECK_INT((long)mres.peak_event[0], (long)(0x01000000u + 64),
              "3.1 and it owns the bucket's peak_event");
    legacy_peaks(many_ev.ev, many_ev.n, &f, BASE_NS, one_bucket_to, 1, lp, le);
    CHECK_INT(lp[0], 2, "3.1 ev_pids[64] dropped it: deleted code reports 2");
    free_result(&mres);
    free(many_ev.ev);

    /* 3.2 pids[128]: 200 distinct pids on one event in one bucket. */
    struct evbuf many_pid = {0};
    for (int p = 0; p < 200; p++)
        push(&many_pid, 4000 + (uint32_t)p, EV1,
             BASE_NS + 1000000ULL + (uint64_t)p * 1000ULL, DUR_NS);
    struct pgwt_concurrency_result pres;
    pgwt_compute_concurrency(many_pid.ev, many_pid.n, &f, BASE_NS,
                             one_bucket_to, 1, BURST_W_NS, BURST_MIN, &pres);
    CHECK_INT(pres.peak_sessions[0], 200, "3.2 all 200 distinct pids counted");
    legacy_peaks(many_pid.ev, many_pid.n, &f, BASE_NS, one_bucket_to, 1, lp, le);
    CHECK_INT(lp[0], 128, "3.2 pids[128] dropped 72: deleted code reports 128");

    /* 3.3 + 3.4 pids[64] inside one burst: 200 pids entering within 10 ms.
     * num_sessions is now EXACT; the pid list is a declared sample, so
     * num_pids < num_sessions is the bound being visible rather than silent
     * (the deleted code reported num_sessions = num_pids = 64). */
    CHECK_INT(pres.num_bursts, 1, "3.3 the burst is detected");
    CHECK_INT(pres.bursts[0].num_sessions, 200, "3.3 exact session count, not 64");
    CHECK_INT(pres.bursts[0].num_pids, PGWT_BURST_PID_SAMPLE,
              "3.4 pid list is the declared 64-pid sample");
    CHECK(pres.bursts[0].num_pids < pres.bursts[0].num_sessions,
          "3.4 and the sample is visibly shorter than the count");
    free_result(&pres);
    free(many_pid.ev);

    /* 3.5 burst_cap=256: 300 buckets, a burst in each, all 300 reported and
     * the LAST bucket's burst present (the deleted code stopped at 256
     * onsets, all from the oldest intervals it had kept). */
    struct evbuf wide = {0};
    build_uniform(&wide, 300, 4, 0);
    struct pgwt_concurrency_result wres;
    pgwt_compute_concurrency(wide.ev, wide.n, &f, BASE_NS, to_ns, 300,
                             BURST_W_NS, BURST_MIN, &wres);
    CHECK_INT(wres.num_bursts, 300, "3.5 more than 256 bursts are reported");
    CHECK_INT(wres.bursts_total, 300 * 4, "3.5 onsets counted across the window");
    int have_last = 0;
    for (int i = 0; i < wres.num_bursts; i++)
        if ((int)((wres.bursts[i].timestamp_ns - BASE_NS) / wres.bucket_ns) == 299)
            have_last = 1;
    CHECK(have_last, "3.5 including the burst in the FINAL bucket");
    free_result(&wres);
    free(wide.ev);

    /* ── 4. False negatives: every way this could pass blind ───────── */
    printf("\n4. Refusals and blind spots\n");

    struct pgwt_concurrency_result nres;
    pgwt_compute_concurrency(big.ev, 0, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &nres);
    CHECK_INT(nres.num_buckets, 0, "4.1 no events: no buckets claimed");
    CHECK_INT(nres.num_bursts, 0, "4.1 no events: no bursts claimed");
    CHECK_INT(nres.failed, 0, "4.1 no events is not a failure");
    free_result(&nres);

    pgwt_compute_concurrency(big.ev, big.n, &f, to_ns, BASE_NS, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &nres);
    CHECK_INT(nres.num_buckets, 0, "4.2 inverted window: nothing claimed");
    free_result(&nres);
    pgwt_compute_concurrency(big.ev, big.n, &f, BASE_NS, to_ns, 0,
                             BURST_W_NS, BURST_MIN, &nres);
    CHECK_INT(nres.num_buckets, 0, "4.3 zero buckets: nothing claimed");
    free_result(&nres);

    /* 4.4 The thing being checked is ABSENT, not wrong: a window whose events
     * are all idle / on-CPU qualifies nothing, so every bucket really is 0 —
     * and section 1's "no bucket is empty" assertion must be able to SEE that,
     * or it was never testing anything. */
    struct evbuf idle = {0};
    for (int bk = 0; bk < U_BUCKETS; bk++)
        for (int p = 0; p < 6; p++) {
            uint64_t t = BASE_NS + (uint64_t)bk * (WINDOW_NS / U_BUCKETS)
                       + 1000000ULL + (uint64_t)p * 1000ULL;
            push(&idle, 5000 + (uint32_t)p, 0x05000001u, t, DUR_NS); /* Activity */
            push(&idle, 5000 + (uint32_t)p, 0u, t + 2000, DUR_NS);   /* on CPU */
        }
    pgwt_compute_concurrency(idle.ev, idle.n, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &nres);
    int idle_empty = 0;
    for (int b = 0; b < U_BUCKETS; b++) if (nres.peak_sessions[b] == 0) idle_empty++;
    CHECK_INT(idle_empty, U_BUCKETS,
              "4.4 nothing qualifying -> every bucket 0 (the criterion can see it)");
    CHECK_INT(nres.num_bursts, 0, "4.4 and no bursts are invented");
    CHECK_INT(nres.failed, 0, "4.4 absent data is not a compute failure");
    free_result(&nres);
    free(idle.ev);

    /* 4.5 / 4.6 Allocation failure in each growing structure must come back
     * ABSENT (failed=1), never short. These paths are unreachable otherwise,
     * and an unreachable refusal is indistinguishable from one that approves. */
    setenv("PGWT_TEST_ALLOC_FAIL", "triple_map_grow", 1);
    pgwt_compute_concurrency(big.ev, big.n, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &nres);
    CHECK_INT(nres.failed, 1, "4.5 distinct-pid table OOM -> failed, not short");
    free_result(&nres);

    setenv("PGWT_TEST_ALLOC_FAIL", "concurrency_entries", 1);
    pgwt_compute_concurrency(big.ev, big.n, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &nres);
    CHECK_INT(nres.failed, 1, "4.6 burst-entry array OOM -> failed, not short");
    free_result(&nres);

    /* 4.7 An UNKNOWN injection point must change nothing — otherwise 4.5/4.6
     * could be passing because the hook fails everything, or nothing. */
    setenv("PGWT_TEST_ALLOC_FAIL", "no_such_allocation_point", 1);
    pgwt_compute_concurrency(big.ev, big.n, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &nres);
    CHECK_INT(nres.failed, 0, "4.7 unknown injection point: answer is complete");
    int inj_wrong = 0;
    for (int b = 0; b < U_BUCKETS; b++)
        if (nres.peak_sessions[b] != expect_peak(b)) inj_wrong++;
    CHECK_INT(inj_wrong, 0, "4.7 and identical to the uninjected run");
    free_result(&nres);
    unsetenv("PGWT_TEST_ALLOC_FAIL");

    /* ── 5. Determinism (qsort is not stable) ───────────────────────── */
    printf("\n5. Determinism\n");
    struct pgwt_concurrency_result again;
    pgwt_compute_concurrency(big.ev, big.n, &f, BASE_NS, to_ns, U_BUCKETS,
                             BURST_W_NS, BURST_MIN, &again);
    CHECK(memcmp(again.peak_sessions, res.peak_sessions,
                 U_BUCKETS * sizeof(int)) == 0, "5.1 peaks reproduce exactly");
    CHECK(memcmp(again.peak_event, res.peak_event,
                 U_BUCKETS * sizeof(uint32_t)) == 0, "5.1 peak events reproduce");
    CHECK_INT(again.num_bursts, res.num_bursts, "5.2 same burst count");
    CHECK(again.num_bursts == res.num_bursts &&
          memcmp(again.bursts, res.bursts,
                 (size_t)res.num_bursts * sizeof(*res.bursts)) == 0,
          "5.2 bursts reproduce byte-for-byte");
    free_result(&again);

    free_result(&res);
    free(big.ev);
    free(lp);
    free(le);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
