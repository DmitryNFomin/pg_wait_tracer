/* test_replay_window.c — #424: `--replay --from/--to` selects by OVERLAP and
 * contributes CLIPPED time, with per-occurrence statistics still selected by
 * the record's own close timestamp.
 *
 * Replay was the last reader still carrying the pre-event_window_ns rule, and
 * it was wrong at BOTH edges of a window:
 *
 *   1. No clipping. pgwt_replay_events added evt->duration_ns WHOLE, so a
 *      wait already running when the window opened was reported in full --
 *      a 10 s Lock closing 2 ms into a window added 10 s, not 2 ms.
 *   2. Right-edge omission. The record loop `break`ed on
 *      `timestamp_ns > to_mono_ns`, so a wait still running at --to
 *      contributed 0 -- and src/replay.c's block loop ALSO stopped at the
 *      last block before --to, which is the half people miss: the closing
 *      record of a right straddler lives in a block that starts AFTER --to
 *      (possibly in the next hourly file), so clipping alone would have left
 *      it unreachable. Both halves are needed; this test pins both.
 *
 * The rule, identical to the server's raw path:
 *   - TIME (event totals, time model, per-query totals, per-pid db/wait/cpu)
 *     is OVERLAP-selected and CLIPPED to the window;
 *   - per-OCCURRENCE statistics (count, histogram, min/max) are selected by
 *     the record's own close timestamp and use the WHOLE duration, so
 *     widening admission cannot double-count one close in two windows.
 *
 * Which assertions DISCRIMINATE the old behaviour (they go red on it):
 *   - the explicit per-event expectations for the left straddler, the right
 *     straddler and the window spanner (CASE 1-3);
 *   - the Lipschitz check (CASE 7): growing --to by 1 ms may add at most 1 ms
 *     of time to an event; the old rule added a whole 1500 ms duration;
 *   - the no-double-count check (CASE 6): one close is counted in exactly
 *     one of two adjacent windows while its TIME is split across both. This
 *     is what an admission-only fix fails.
 * The tiling conservation check (CASE 8) does NOT discriminate the old rule
 * (whole-duration-on-close conserves too); it guards THIS implementation
 * against losing or double-counting time, and is labelled as such.
 *
 * Plus a bypass suite (CASE B1-B8) for the new right-edge lookahead, because
 * a scan that cannot see must refuse, never approve: budget exhausted, an
 * unreadable block header, a SAMPLES tail that must not spend budget, no
 * straddler present at all, bad arguments, empty input, and the impossible
 * record that must not wrap a uint64.
 *
 * Links the daemon-flavour event_reader.c (which holds pgwt_replay_events and
 * pgwt_replay_file) against the BPF-free accumulator core -- no bpftool, runs
 * anywhere.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "map_reader.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (cond) { printf("  PASS: "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define CHECK_EQ(got, want, ...) do { \
    uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want); \
    checks++; \
    if (g_ == w_) { printf("  PASS: "); printf(__VA_ARGS__); \
                    printf(" = %" PRIu64 "\n", g_); } \
    else { printf("  FAIL: "); printf(__VA_ARGS__); \
           printf(" = %" PRIu64 ", want %" PRIu64 "\n", g_, w_); \
           failures++; } \
} while (0)

/* ── The trace ────────────────────────────────────────────────────────────
 *
 * Monotonic ns, BASE = 10000 s. Window under test: [FROM, TO] =
 * BASE + [1000 ms, 2000 ms].
 *
 * block 0 (TRANSITIONS, first ts BASE+500 ms)
 *   EV_BEFORE   close  500 ms, dur  100 ms  -> [ 400,  500] wholly left   = 0
 *   EV_ATFROM   close 1000 ms, dur   20 ms  -> [ 980, 1000] touches FROM  = 0
 *   EV_LEFT     close 1002 ms, dur   10 ms  -> [ 992, 1002] left straddle = 2 ms
 *   EV_IN       close 1500 ms, dur  100 ms  -> [1400, 1500] inside        = 100 ms
 *   EV_ATTO     close 2000 ms, dur   50 ms  -> [1950, 2000] closes at TO  = 50 ms
 * block 1 (SAMPLES, first ts BASE+2500 ms) 12 samples @ 100 ms, all past TO
 * block 2 (TRANSITIONS, first ts BASE+3000 ms)
 *   EV_RIGHT    close 3000 ms, dur 1500 ms  -> [1500, 3000] right straddle= 500 ms
 *   EV_SPAN     close 3200 ms, dur 2500 ms  -> [ 700, 3200] spans window  = 1000 ms
 *   EV_AFTER    close 3400 ms, dur  100 ms  -> [3300, 3400] wholly right  = 0
 *
 * The SAMPLES block sits between the two TRANSITIONS blocks on purpose: it
 * forces the writer to cut block 0 (so the straddlers really do land in a
 * block that STARTS after TO), and it is the input for the "a sampled tail
 * must not spend lookahead budget" bypass case.
 */
#define MS        1000000ULL
#define BASE      10000000000000ULL
#define W_FROM    (BASE + 1000 * MS)
#define W_TO      (BASE + 2000 * MS)
#define PERIOD    (100 * MS)
#define NSAMPLES  12

#define EV_BEFORE 0x0A000001u
#define EV_ATFROM 0x0A000002u
#define EV_LEFT   0x0A000003u
#define EV_IN     0x0A000004u
#define EV_ATTO   0x0A000005u
#define EV_RIGHT  0x0A000006u
#define EV_SPAN   0x0A000007u
#define EV_AFTER  0x0A000008u
#define EV_SAMPLE 0x03000000u   /* Lock:relation — sampled, never exact */

#define QID       7777ULL
#define TPID      4242u

static struct pgwt_trace_event ev(uint64_t ts_ms, uint64_t dur_ms, uint32_t we)
{
    struct pgwt_trace_event e = {0};
    e.pid = TPID;
    e.timestamp_ns = BASE + ts_ms * MS;
    e.duration_ns  = dur_ms * MS;
    e.old_event    = we;
    e.new_event    = 0;
    e.query_id     = QID;
    return e;
}

/* Write the trace described above into `dir`. Returns 0 / -1. */
static int write_trace(const char *dir)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0)
        return -1;

    struct pgwt_trace_event blk0[] = {
        ev(500,  100, EV_BEFORE),
        ev(1000,  20, EV_ATFROM),
        ev(1002,  10, EV_LEFT),
        ev(1500, 100, EV_IN),
        ev(2000,  50, EV_ATTO),
    };
    for (size_t i = 0; i < sizeof(blk0) / sizeof(blk0[0]); i++)
        if (pgwt_writer_push_event(&w, &blk0[i]) != 0) return -1;

    /* 12 samples spanning 1.1 s past TO: the batch's own time-based cut
     * flushes it, and flush ordering writes the pending TRANSITIONS block
     * (older first timestamp) before it. */
    struct pgwt_trace_event smp[NSAMPLES];
    for (int i = 0; i < NSAMPLES; i++) {
        smp[i] = ev(2500 + (uint64_t)i * 100, 0, 0);
        smp[i].old_event = 0;
        smp[i].new_event = EV_SAMPLE;
    }
    if (pgwt_writer_push_samples(&w, smp, NSAMPLES, PERIOD) != 0) return -1;

    struct pgwt_trace_event blk2[] = {
        ev(3000, 1500, EV_RIGHT),
        ev(3200, 2500, EV_SPAN),
        ev(3400,  100, EV_AFTER),
    };
    for (size_t i = 0; i < sizeof(blk2) / sizeof(blk2[0]); i++)
        if (pgwt_writer_push_event(&w, &blk2[i]) != 0) return -1;

    if (pgwt_writer_close(&w) != 0) return -1;
    pgwt_writer_destroy(&w);
    return 0;
}

/* A trace with NO straddler at all — the "absent, not wrong" input. */
static int write_clean_trace(const char *dir)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0)
        return -1;
    struct pgwt_trace_event e0 = ev(1200, 100, EV_IN);
    struct pgwt_trace_event e1 = ev(1800, 200, EV_ATTO);
    if (pgwt_writer_push_event(&w, &e0) != 0) return -1;
    if (pgwt_writer_push_event(&w, &e1) != 0) return -1;
    if (pgwt_writer_close(&w) != 0) return -1;
    pgwt_writer_destroy(&w);
    return 0;
}

/* Replay `path` over [from, to] into a fresh accumulator.
 * budget: NULL = unlimited; otherwise in/out, the caller sees what is left.
 * corrupt_block: >= 0 points that block's index entry at a file offset past
 *                EOF, so its header cannot be read (the "cannot see" case).
 * Returns NULL only if the trace could not be opened at all — which fails
 * the test rather than skipping it (see the `opened` counter in main). */
static struct pgwt_accumulator *replay(const char *path,
                                       uint64_t from, uint64_t to,
                                       int *budget,
                                       struct pgwt_replay_stats *st,
                                       int corrupt_block,
                                       uint64_t *out_decoded,
                                       int *out_blocks)
{
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0)
        return NULL;
    if (out_blocks) *out_blocks = r.num_blocks;
    if (corrupt_block >= 0 && corrupt_block < r.num_blocks)
        r.block_index[corrupt_block].file_offset = (uint64_t)1 << 40;

    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    uint64_t decoded = 0;

    pgwt_replay_file(&r, acc, from, to, buf, PGWT_BLOCK_EVENTS,
                     budget, st, &decoded);

    if (out_decoded) *out_decoded = decoded;
    free(buf);
    pgwt_reader_close(&r);
    return acc;
}

static uint64_t tot(struct pgwt_accumulator *acc, uint32_t we)
{
    struct pgwt_event_stats *s = pgwt_find_system_event(acc, we);
    return s ? s->total_ns : 0;
}
static uint64_t cnt(struct pgwt_accumulator *acc, uint32_t we)
{
    struct pgwt_event_stats *s = pgwt_find_system_event(acc, we);
    return s ? s->count : 0;
}
static uint64_t hist_total(struct pgwt_accumulator *acc, uint32_t we)
{
    struct pgwt_event_stats *s = pgwt_find_system_event(acc, we);
    uint64_t n = 0;
    if (s) for (int i = 0; i < HISTOGRAM_BUCKETS; i++) n += s->histogram[i];
    return n;
}
static uint64_t qtot(struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_query_events; i++)
        if (acc->query_events[i].query_id == QID &&
            acc->query_events[i].wait_event == we)
            return acc->query_events[i].total_ns;
    return 0;
}

int main(void)
{
    char dir[]  = "/tmp/pgwt_replay_win_XXXXXX";
    char dir2[] = "/tmp/pgwt_replay_cln_XXXXXX";
    if (!mkdtemp(dir) || !mkdtemp(dir2)) { perror("mkdtemp"); return 1; }
    pgwt_init_event_names(18);

    if (write_trace(dir) != 0) {
        fprintf(stderr, "FATAL: could not write the test trace\n");
        return 1;
    }
    if (write_clean_trace(dir2) != 0) {
        fprintf(stderr, "FATAL: could not write the straddler-free trace\n");
        return 1;
    }
    char path[600], path2[600];
    snprintf(path,  sizeof(path),  "%s/current.trace", dir);
    snprintf(path2, sizeof(path2), "%s/current.trace", dir2);

    /* ── Preconditions. A writer/reader failure must FAIL this test, never
     * let it pass vacuously on an accumulator nobody filled. ───────────── */
    int blocks = 0;
    uint64_t decoded = 0;
    struct pgwt_replay_stats st_all = {0};
    struct pgwt_accumulator *all =
        replay(path, 0, 0, NULL, &st_all, -1, &decoded, &blocks);
    if (!all) { fprintf(stderr, "FATAL: cannot open %s\n", path); return 1; }
    printf("PRECONDITIONS\n");
    CHECK_EQ(blocks, 3, "trace cut into 3 blocks (transitions/samples/transitions)");
    CHECK_EQ(decoded, 8 + NSAMPLES, "records decoded by the unbounded replay");
    CHECK_EQ(st_all.transitions, 8, "8 exact transitions seen");
    CHECK_EQ(st_all.samples, NSAMPLES, "12 samples seen");

    /* The whole-capture path must be bit-identical to pre-#424: unbounded
     * means every record contributes its whole duration. */
    printf("CASE 0: unbounded replay unchanged (from=to=0)\n");
    CHECK_EQ(tot(all, EV_LEFT),  10 * MS,   "EV_LEFT whole duration");
    CHECK_EQ(tot(all, EV_RIGHT), 1500 * MS, "EV_RIGHT whole duration");
    CHECK_EQ(tot(all, EV_SPAN),  2500 * MS, "EV_SPAN whole duration");
    CHECK_EQ(cnt(all, EV_AFTER), 1,         "EV_AFTER counted");
    CHECK_EQ(tot(all, EV_SAMPLE), (uint64_t)NSAMPLES * PERIOD,
             "samples still worth count x period");
    CHECK_EQ(st_all.straddlers_admitted, 0,
             "no straddlers on an unbounded window (nothing to straddle)");
    CHECK_EQ(st_all.right_edge_truncated, 0, "unbounded replay never truncates");

    /* ── The window under test ───────────────────────────────────────── */
    int budget = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
    struct pgwt_replay_stats st = {0};
    struct pgwt_accumulator *w =
        replay(path, W_FROM, W_TO, &budget, &st, -1, NULL, NULL);
    if (!w) { fprintf(stderr, "FATAL: cannot open %s\n", path); return 1; }

    printf("CASE 1: left straddler — clipped time, WHOLE-duration occurrence\n");
    CHECK_EQ(tot(w, EV_LEFT), 2 * MS,
             "EV_LEFT time clipped to its in-window part (was 10 ms whole)");
    CHECK_EQ(cnt(w, EV_LEFT), 1, "EV_LEFT occurrence counted once");
    CHECK_EQ(hist_total(w, EV_LEFT), 1, "EV_LEFT in the histogram once");
    {
        struct pgwt_event_stats *s = pgwt_find_system_event(w, EV_LEFT);
        CHECK_EQ(s ? s->max_ns : 0, 10 * MS,
                 "EV_LEFT max is the WHOLE latency, not the clipped part");
    }
    CHECK_EQ(qtot(w, EV_LEFT), 2 * MS, "per-query EV_LEFT time clipped too");
    CHECK_EQ(tot(w, EV_ATFROM), 0,
             "EV_ATFROM closing exactly at --from contributes no time");
    CHECK_EQ(cnt(w, EV_ATFROM), 1, "EV_ATFROM still counted as an occurrence");

    printf("CASE 2: right straddler — time admitted, occurrence NOT counted\n");
    CHECK_EQ(tot(w, EV_RIGHT), 500 * MS,
             "EV_RIGHT in-window part (was 0: dropped by close-only admission)");
    CHECK_EQ(cnt(w, EV_RIGHT), 0,
             "EV_RIGHT occurrence belongs to the window it CLOSES in");
    CHECK_EQ(hist_total(w, EV_RIGHT), 0, "EV_RIGHT not in this histogram");
    {
        struct pgwt_event_stats *s = pgwt_find_system_event(w, EV_RIGHT);
        CHECK_EQ(s ? s->max_ns : 0, 0, "EV_RIGHT contributes no max latency");
    }
    CHECK_EQ(qtot(w, EV_RIGHT), 500 * MS, "per-query EV_RIGHT time admitted");

    printf("CASE 3: spanner — the whole window, counted nowhere here\n");
    CHECK_EQ(tot(w, EV_SPAN), 1000 * MS, "EV_SPAN covers the entire window");
    CHECK_EQ(cnt(w, EV_SPAN), 0, "EV_SPAN occurrence is not in this window");

    printf("CASE 4: inside / at --to / outside\n");
    CHECK_EQ(tot(w, EV_IN), 100 * MS, "EV_IN whole (no edge cut)");
    CHECK_EQ(cnt(w, EV_IN), 1, "EV_IN counted");
    CHECK_EQ(tot(w, EV_ATTO), 50 * MS,
             "EV_ATTO closing exactly at --to is IN (selection is inclusive)");
    CHECK_EQ(cnt(w, EV_ATTO), 1, "EV_ATTO counted");
    CHECK(pgwt_find_system_event(w, EV_BEFORE) == NULL,
          "EV_BEFORE (wholly before) creates NO entry — absence, not zero");
    CHECK(pgwt_find_system_event(w, EV_AFTER) == NULL,
          "EV_AFTER (wholly after) creates NO entry — absence, not zero");
    CHECK(pgwt_find_system_event(w, EV_SAMPLE) == NULL,
          "samples past --to contribute nothing");

    printf("CASE 5: totals and stats\n");
    CHECK_EQ(w->tm.io_time_ns, (2 + 100 + 50 + 500 + 1000) * MS,
             "time model IO = sum of the clipped parts");
    CHECK_EQ(w->num_pids > 0 ? w->pids[0].db_time_ns : 0,
             (2 + 100 + 50 + 500 + 1000) * MS,
             "per-pid DB time = the same clipped sum");
    CHECK_EQ(st.transitions, 4,
             "4 exact occurrences closed in the window (BEFORE/AFTER excluded)");
    CHECK_EQ(st.straddlers_admitted, 2,
             "2 right-open records admitted for time only");
    CHECK_EQ(st.right_edge_truncated, 0, "the right-edge scan completed");
    CHECK_EQ(budget, PGWT_REPLAY_LOOKAHEAD_BLOCKS - 1,
             "exactly ONE transitions block past --to was decoded");

    printf("CASE 6: adjacent windows — time splits, the close is counted ONCE\n");
    {
        /* [W_TO, BASE+4000ms] is the window EV_RIGHT/EV_SPAN close in. An
         * admission-only fix (no instant/interval split) counts them in BOTH
         * windows: that is the overcount the undercount must not become. */
        int b2 = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats st2 = {0};
        struct pgwt_accumulator *nx =
            replay(path, W_TO, BASE + 4000 * MS, &b2, &st2, -1, NULL, NULL);
        CHECK_EQ(cnt(w, EV_RIGHT) + cnt(nx, EV_RIGHT), 1,
                 "EV_RIGHT counted exactly once across the two windows");
        CHECK_EQ(cnt(w, EV_SPAN) + cnt(nx, EV_SPAN), 1,
                 "EV_SPAN counted exactly once across the two windows");
        CHECK_EQ(tot(w, EV_RIGHT) + tot(nx, EV_RIGHT), 1500 * MS,
                 "EV_RIGHT time split across the two windows, nothing lost");
        CHECK_EQ(hist_total(w, EV_RIGHT) + hist_total(nx, EV_RIGHT), 1,
                 "EV_RIGHT in exactly one histogram");
        free(nx);
    }

    printf("CASE 7: Lipschitz — growing --to by 1 ms adds at most 1 ms\n");
    {
        /* The sharpest statement of the old bug: with whole-duration
         * accounting, moving --to from just before a close to just after it
         * added the record's ENTIRE 1500 ms to a window that grew by 1 ms. */
        int ba = PGWT_REPLAY_LOOKAHEAD_BLOCKS, bb = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats sa = {0}, sb = {0};
        struct pgwt_accumulator *a =
            replay(path, W_FROM, BASE + 2999 * MS, &ba, &sa, -1, NULL, NULL);
        struct pgwt_accumulator *b =
            replay(path, W_FROM, BASE + 3000 * MS, &bb, &sb, -1, NULL, NULL);
        uint64_t ta = tot(a, EV_RIGHT), tb = tot(b, EV_RIGHT);
        CHECK(tb >= ta && tb - ta <= 1 * MS,
              "EV_RIGHT grew by %" PRIu64 " ns for a 1 ms wider window",
              tb - ta);
        CHECK_EQ(ta, 1499 * MS, "EV_RIGHT at to=2999 ms");
        CHECK_EQ(tb, 1500 * MS, "EV_RIGHT at to=3000 ms");
        free(a); free(b);
    }

    printf("CASE 8: tiling conservation (guards THIS implementation; the old "
           "rule also conserves)\n");
    {
        const uint64_t edges[] = { BASE, BASE + 750 * MS, BASE + 1250 * MS,
                                   BASE + 1750 * MS, BASE + 2250 * MS,
                                   BASE + 2750 * MS, BASE + 3250 * MS,
                                   BASE + 3750 * MS, BASE + 4500 * MS };
        const int nedge = (int)(sizeof(edges) / sizeof(edges[0]));
        const uint32_t ids[] = { EV_BEFORE, EV_ATFROM, EV_LEFT, EV_IN,
                                 EV_ATTO, EV_RIGHT, EV_SPAN, EV_AFTER };
        uint64_t sum_t[8] = {0}, sum_c[8] = {0}, sum_s = 0;
        for (int i = 0; i + 1 < nedge; i++) {
            int b = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
            struct pgwt_replay_stats ts = {0};
            struct pgwt_accumulator *t =
                replay(path, edges[i], edges[i + 1], &b, &ts, -1, NULL, NULL);
            for (int k = 0; k < 8; k++) {
                sum_t[k] += tot(t, ids[k]);
                sum_c[k] += cnt(t, ids[k]);
            }
            sum_s += tot(t, EV_SAMPLE);
            free(t);
        }
        const uint64_t want_t[] = { 100 * MS, 20 * MS, 10 * MS, 100 * MS,
                                    50 * MS, 1500 * MS, 2500 * MS, 100 * MS };
        int all_t = 1, all_c = 1;
        for (int k = 0; k < 8; k++) {
            if (sum_t[k] != want_t[k]) {
                all_t = 0;
                printf("    tile sum %08x = %" PRIu64 " ns, whole = %" PRIu64 "\n",
                       ids[k], sum_t[k], want_t[k]);
            }
            if (sum_c[k] != 1) {
                all_c = 0;
                printf("    tile count %08x = %" PRIu64 ", want 1\n",
                       ids[k], sum_c[k]);
            }
        }
        CHECK(all_t, "every record's time is conserved exactly across 8 tiles");
        CHECK(all_c, "every occurrence is counted exactly once across 8 tiles");
        CHECK_EQ(sum_s, (uint64_t)NSAMPLES * PERIOD,
                 "sampled time conserved across the tiles too");
    }

    printf("CASE 9: left edge — a block that STARTS earlier but runs into "
           "the window\n");
    {
        /* Found by CASE 8 before it was fixed: pgwt_reader_find_block returns
         * ONE start index, which is exact only while blocks do not overlap in
         * time. The SAMPLES block here spans 2500-3600 ms and the TRANSITIONS
         * block 3000-3400 ms, so a window opening at 3250 ms starts at the
         * transitions block and used to skip the samples block entirely —
         * 400 ms of sampled time (4 ticks at 3300/3400/3500/3600) silently
         * gone. */
        int b = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats s9 = {0};
        struct pgwt_accumulator *a =
            replay(path, BASE + 3250 * MS, BASE + 3750 * MS, &b, &s9, -1,
                   NULL, NULL);
        CHECK_EQ(tot(a, EV_SAMPLE), 4 * PERIOD,
                 "4 samples from the earlier-starting block are in the window");
        CHECK_EQ(s9.samples, 4, "…and are reported as samples");
        CHECK_EQ(tot(a, EV_AFTER), 100 * MS,
                 "the transitions block in the same range is unaffected");
        free(a);
    }

    /* ── Bypass suite: every way the right-edge scan could fail to SEE ──
     * A scan that cannot reach the straddlers must say so, not report a
     * short answer as if it were complete. */
    printf("CASE B1: lookahead budget exhausted -> refuses, does not approve\n");
    {
        int b0 = 0;
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path, W_FROM, W_TO, &b0, &sb, -1, NULL, NULL);
        CHECK_EQ(sb.right_edge_truncated, 1,
                 "right_edge_truncated set when the budget runs out");
        CHECK_EQ(tot(a, EV_RIGHT), 0,
                 "and the answer really is short (so the flag is the only "
                 "thing standing between the user and a silent undercount)");
        CHECK_EQ(tot(a, EV_IN), 100 * MS,
                 "records before --to are unaffected by the truncation");
        free(a);
    }

    printf("CASE B2: no budget supplied -> unlimited scan, complete answer\n");
    {
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path, W_FROM, W_TO, NULL, &sb, -1, NULL, NULL);
        CHECK_EQ(sb.right_edge_truncated, 0, "not truncated");
        CHECK_EQ(tot(a, EV_RIGHT), 500 * MS, "straddler found");
        free(a);
    }

    printf("CASE B3: unreadable block header -> refuses rather than skipping\n");
    {
        /* Block 2 is the straddlers' block; point it past EOF so its header
         * cannot be read. The scan must mark the answer, not continue as if
         * the block held nothing. */
        int b = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path, W_FROM, W_TO, &b, &sb, 2, NULL, NULL);
        CHECK_EQ(sb.right_edge_truncated, 1,
                 "right_edge_truncated set when a header cannot be read");
        CHECK_EQ(tot(a, EV_RIGHT), 0, "the straddler is genuinely missing");
        CHECK_EQ(b, PGWT_REPLAY_LOOKAHEAD_BLOCKS,
                 "an unreadable header spends no budget");
        free(a);
    }

    printf("CASE B4: a SAMPLES tail must not spend lookahead budget\n");
    {
        /* If it did, a sampled trace would exhaust the budget on blocks that
         * cannot contain a straddler and report a false truncation. Block 1
         * is 12 sample records past --to; only block 2 may cost budget. */
        int b = 1;
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path, W_FROM, W_TO, &b, &sb, -1, NULL, NULL);
        CHECK_EQ(b, 0, "budget 1 was enough: the samples block was free");
        CHECK_EQ(sb.right_edge_truncated, 0, "no false truncation");
        CHECK_EQ(tot(a, EV_RIGHT), 500 * MS, "and the straddler was found");
        free(a);
    }

    printf("CASE B5: absent, not wrong — a trace with no straddler\n");
    {
        int b = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path2, W_FROM, W_TO, &b, &sb, -1, NULL, NULL);
        CHECK(a != NULL, "straddler-free trace opened");
        CHECK_EQ(sb.straddlers_admitted, 0,
                 "straddlers_admitted stays 0 when there is nothing to admit");
        CHECK_EQ(sb.right_edge_truncated, 0, "nothing to truncate");
        CHECK_EQ(tot(a, EV_IN), 100 * MS, "the ordinary records are intact");
        CHECK_EQ(tot(a, EV_ATTO), 200 * MS, "…including the one ending at --to");
        free(a);
    }

    printf("CASE B6: bad arguments -> -1, nothing accumulated\n");
    {
        struct pgwt_event_reader r;
        CHECK(pgwt_reader_open(&r, path) == 0, "reader opened for the arg checks");
        struct pgwt_accumulator *a = calloc(1, sizeof(*a));
        pgwt_accum_init(a);
        struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
        uint64_t d = 123;
        CHECK(pgwt_replay_file(NULL, a, 0, 0, buf, PGWT_BLOCK_EVENTS,
                               NULL, NULL, &d) == -1, "NULL reader refused");
        CHECK_EQ(d, 0, "out_decoded cleared on refusal");
        CHECK(pgwt_replay_file(&r, NULL, 0, 0, buf, PGWT_BLOCK_EVENTS,
                               NULL, NULL, NULL) == -1, "NULL accumulator refused");
        CHECK(pgwt_replay_file(&r, a, 0, 0, NULL, PGWT_BLOCK_EVENTS,
                               NULL, NULL, NULL) == -1, "NULL buffer refused");
        CHECK(pgwt_replay_file(&r, a, 0, 0, buf, 0,
                               NULL, NULL, NULL) == -1, "zero capacity refused");
        CHECK_EQ(a->num_system_events, 0, "nothing accumulated by a refusal");
        free(buf); free(a);
        pgwt_reader_close(&r);
    }

    printf("CASE B7: empty input and the impossible record\n");
    {
        struct pgwt_accumulator *a = calloc(1, sizeof(*a));
        pgwt_accum_init(a);
        struct pgwt_replay_stats sb = {0};
        struct pgwt_block_info bi = { PGWT_BLOCK_TRANSITIONS, 0, 0, 0, 0 };
        struct pgwt_trace_event none[1] = {{0}};
        pgwt_replay_events(a, none, 0, W_FROM, W_TO, &bi, &sb);
        CHECK_EQ(a->num_system_events, 0, "zero records accumulate nothing");
        CHECK_EQ(sb.transitions + sb.samples + sb.straddlers_admitted, 0,
                 "and report nothing");

        /* duration longer than its own absolute timestamp: impossible, and
         * the naive subtraction would wrap uint64 into ~18 exaseconds. The
         * server's raw path (event_window_ns) refuses it the same way. */
        struct pgwt_trace_event bad = ev(1500, 0, EV_IN);
        bad.duration_ns = bad.timestamp_ns + 1;
        pgwt_replay_events(a, &bad, 1, W_FROM, W_TO, &bi, &sb);
        CHECK_EQ(tot(a, EV_IN), 0,
                 "an impossible record contributes 0, not a wrapped uint64");
        free(a);
    }

    printf("CASE B9: an undecodable block is REPORTED, not swallowed\n");
    {
        /* Block 0 holds every in-window transition. Point it past EOF: the
         * decode fails, and the window must come back saying a block is
         * missing rather than looking like a complete, quieter window. */
        int b = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path, W_FROM, W_TO, &b, &sb, 0, NULL, NULL);
        CHECK_EQ(sb.blocks_unreadable, 1, "the corrupt block is counted");
        CHECK_EQ(tot(a, EV_IN), 0, "and its records really are missing");
        CHECK_EQ(tot(a, EV_RIGHT), 500 * MS,
                 "the readable blocks still answer");
        free(a);
    }

    printf("CASE B8: a window past the end of the trace\n");
    {
        int b = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
        struct pgwt_replay_stats sb = {0};
        struct pgwt_accumulator *a =
            replay(path, BASE + 9000 * MS, BASE + 9500 * MS, &b, &sb, -1,
                   NULL, NULL);
        CHECK_EQ(a->num_system_events, 0, "no events in an empty window");
        CHECK_EQ(sb.right_edge_truncated, 0, "nothing to look ahead for");
        free(a);
    }

    free(all);
    free(w);

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", dir, dir2);
    if (system(cmd) != 0) { /* best effort */ }

    printf("\n%d checks, %d failures\n", checks, failures);
    printf("%s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
