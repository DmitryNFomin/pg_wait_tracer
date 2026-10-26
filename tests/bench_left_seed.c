/* bench_left_seed.c — #588: measure #586's needy-pid pass.
 *
 * #586 added left_seed_find_needy(): one O(total) pass with one
 * pgwt_pid_index hash probe per record, run on EVERY bounded raw load
 * (server_load_events_fi_mode, ls.enabled = from_wall_ns != 0 &&
 * !markers_only), whether or not the window straddles anything. Its own
 * commit message flagged the cost as unmeasured. This is the measurement,
 * not a gate: a wall-clock assertion on a shared runner is noise dressed up
 * as a check (same call as bench_block_agg above), so this prints numbers
 * and is deliberately NOT in tests/unit_tests.list.
 *
 * Two measurements, both warm (repeated calls in ONE process so the first,
 * cold, page-fault-heavy call is discarded and the reported numbers are the
 * steady-state cost a live server actually pays):
 *
 *   A. left_seed_find_needy() ISOLATED, over an in-memory array -- the exact
 *      added pass, nothing else. Run once with every pid resolved
 *      immediately (closed on its first record: the fast `if (e->closed)
 *      continue` path every record after) to model the NON-STRADDLING
 *      common case, and once with one pid that never resolves (the needy
 *      path) to model the STRADDLING case. The function cannot tell which
 *      case it is in until the pass completes, so the two should cost the
 *      SAME per record -- straddling's extra cost is downstream, in
 *      left_seed_resolve, not here.
 *
 *   B. The full bounded raw load (server_load_events_fi), end to end,
 *      against a REAL on-disk 900 s trace -- once where the window has no
 *      needy pid (left_seed_resolve never runs) and once where it does (one
 *      straddling command, mirroring test_left_edge_attrib's FX_MAIN
 *      fixture). This is "added wall time per bounded raw load" as the
 *      issue asks it, not an isolated function's cost.
 *
 * Usage: ./bench_left_seed [n1,n2,...]  (record counts; default a spread
 * from 10k to 1M, chosen to bracket what a 900 s window holds across light
 * to heavy workloads -- the 2026-10-25 demo capture measured ~2300
 * events/sec, i.e. ~2.1M over 900 s at the high end).
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL
#define WINDOW_SECS 900

#define EV_LOCK WEI(PG_WAIT_LOCK, 0)
#define EV_IO   WEI(PG_WAIT_IO, 21)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ── A: left_seed_find_needy in isolation, in-memory, no disk at all ──── */

/* Builds `n` events across `pids` distinct pids. If `straddle` is 0, every
 * pid's FIRST record in the array reports a nonzero query_id (closes
 * immediately; every later record for that pid takes the `if (e->closed)
 * continue` fast path). If `straddle` is 1, pid 0's records all carry
 * query_id 0 and are ordinary foreground waits (never a marker, never an
 * idle sample) -- genuinely needy, same as a left-straddling command whose
 * EXEC_START fell outside the window. */
static struct pgwt_trace_event *build_array(int n, int pids, int straddle)
{
    struct pgwt_trace_event *ev = calloc((size_t)n, sizeof(*ev));
    if (!ev) { fprintf(stderr, "calloc failed\n"); exit(1); }
    for (int i = 0; i < n; i++) {
        uint32_t pid = (uint32_t)(1000 + (i % pids));
        ev[i].timestamp_ns = (uint64_t)i * (WINDOW_SECS * ONE_SEC / (uint64_t)n);
        ev[i].pid = pid;
        ev[i].old_event = EV_LOCK;
        ev[i].new_event = EV_IO;
        ev[i].duration_ns = 1 * MS;
        ev[i].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        int first_for_pid = (i < pids);
        if (straddle && pid == 1000) {
            ev[i].query_id = 0;      /* pid 1000 never reports an id: needy */
        } else if (first_for_pid) {
            ev[i].query_id = 0xABCD0000u + pid;  /* resolves this pid at once */
        } else {
            ev[i].query_id = 0xABCD0000u + pid;  /* already closed; irrelevant */
        }
    }
    return ev;
}

static double bench_find_needy_once(struct pgwt_trace_event *ev, int n)
{
    struct left_needy_tab t;
    memset(&t, 0, sizeof(t));
    pgwt_pid_index_init(&t.ix);
    int out_needy = 0;
    double t0 = now_ms();
    int rc = left_seed_find_needy(ev, n, &t, &out_needy);
    double t1 = now_ms();
    if (rc != 0) { fprintf(stderr, "left_seed_find_needy failed\n"); exit(1); }
    left_needy_tab_free(&t);
    return t1 - t0;
}

static void bench_isolated(int n, int pids)
{
    struct pgwt_trace_event *ev_closed = build_array(n, pids, 0);
    struct pgwt_trace_event *ev_needy  = build_array(n, pids, 1);

    const int warmup = 1, iters = 10;
    double closed_ms[iters], needy_ms[iters];

    for (int i = 0; i < warmup; i++) bench_find_needy_once(ev_closed, n);
    for (int i = 0; i < iters; i++) closed_ms[i] = bench_find_needy_once(ev_closed, n);

    for (int i = 0; i < warmup; i++) bench_find_needy_once(ev_needy, n);
    for (int i = 0; i < iters; i++) needy_ms[i] = bench_find_needy_once(ev_needy, n);

    double closed_sum = 0, needy_sum = 0;
    for (int i = 0; i < iters; i++) { closed_sum += closed_ms[i]; needy_sum += needy_ms[i]; }
    double closed_avg = closed_sum / iters, needy_avg = needy_sum / iters;

    printf("  A  n=%-9d non-straddle(all-closed)=%8.3f ms (%.1f ns/rec)   "
           "straddle(1-needy)=%8.3f ms (%.1f ns/rec)   n=%d iters\n",
           n, closed_avg, closed_avg * 1e6 / n, needy_avg, needy_avg * 1e6 / n, iters);

    free(ev_closed);
    free(ev_needy);
}

/* ── B: end-to-end server_load_events_fi against a real on-disk trace ──── */

static void push_q(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
                   uint32_t old_ev, uint32_t new_ev, uint64_t dur, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = old_ev;
    e.new_event = new_ev;
    e.duration_ns = dur;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    if (pgwt_writer_push_event(w, &e) != 0) {
        fprintf(stderr, "writer refused a record\n");
        exit(1);
    }
}

static void mark(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
                 uint32_t marker, uint64_t qid)
{
    push_q(w, ts, pid, marker, marker, 0, qid);
}

/* `n` filler records spread over WINDOW_SECS across `pids` backends, each
 * resolved inside the window (marker pair then one wait) -- the realistic
 * non-straddling bulk. If `straddle` is set, ONE extra pid (STRADDLE_PID)
 * gets test_left_edge_attrib's FX_MAIN shape: EXEC_START 5.9 s before
 * `from`, Lock closing 2.5 s inside -- a genuine needy pid that forces
 * left_seed_resolve to run its bounded marker lookback. */
#define STRADDLE_PID 999001u

/* `pgwt_trace_event.timestamp_ns` is a MONOTONIC kernel timestamp (ktime_get_ns);
 * server_load_events_fi takes WALL nanoseconds. server_init derives the
 * mono->wall offset from the trace itself (srv->earliest_wall_ns corresponds
 * to the earliest mono timestamp actually written). build_trace writes its
 * earliest event at mono `base`, so a caller must ask srv for the wall
 * equivalent rather than assuming `base` IS wall time. */
static uint64_t build_trace(const char *dir, uint64_t base, int n, int pids,
                           int straddle)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        fprintf(stderr, "writer init failed (%s)\n", dir);
        exit(1);
    }
    /* Pre-window coverage so `from` has a real left edge: every filler pid
     * opens its command 1 s before the window and reports its id there,
     * i.e. CLOSED for rule 2 without any lookback -- the non-straddling
     * bulk the issue calls "the common case". */
    uint64_t from = base + 1 * ONE_SEC;
    uint64_t span_ns = (uint64_t)WINDOW_SECS * ONE_SEC;
    for (int i = 0; i < n; i++) {
        uint32_t pid = (uint32_t)(2000 + (i % pids));
        uint64_t ts = from + (uint64_t)i * (span_ns / (uint64_t)n);
        if (i < pids) {
            mark(&writer, base, pid, PGWT_MARKER_CMD_START, 0);
            mark(&writer, base + 100 * MS, pid,
                 PGWT_MARKER_EXEC_START, 0x9000000000000000ULL + pid);
        }
        push_q(&writer, ts, pid, EV_LOCK, EV_IO, 1 * MS, 0);
    }

    if (straddle) {
        /* FX_MAIN shape, scaled to this window: EXEC_START well before
         * `from`, Lock straddling the left edge. */
        mark(&writer, base, STRADDLE_PID, PGWT_MARKER_CMD_START, 0);
        mark(&writer, base + 100 * MS, STRADDLE_PID,
             PGWT_MARKER_EXEC_START, 0x9000000000000001ULL);
        uint64_t close_ts = from + 2 * ONE_SEC; /* closes 2s inside */
        push_q(&writer, close_ts, STRADDLE_PID, EV_LOCK, EV_IO,
               3 * ONE_SEC, 0);
        mark(&writer, close_ts + 100 * MS, STRADDLE_PID,
             PGWT_MARKER_EXEC_END, 0);
        mark(&writer, close_ts + 200 * MS, STRADDLE_PID,
             PGWT_MARKER_CMD_END, 0);
    }

    pgwt_writer_close(&writer);
    return from;
}

static void bench_end_to_end(int n, int pids)
{
    char dir_plain[64], dir_straddle[64];
    snprintf(dir_plain, sizeof(dir_plain), "/tmp/pgwt_bench588_plain_%d", n);
    snprintf(dir_straddle, sizeof(dir_straddle), "/tmp/pgwt_bench588_strad_%d", n);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s && mkdir -p %s %s",
             dir_plain, dir_straddle, dir_plain, dir_straddle);
    if (system(cmd) != 0) { fprintf(stderr, "setup failed\n"); exit(1); }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t base = (((uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec)
                      / ONE_SEC) * ONE_SEC;

    build_trace(dir_plain, base, n, pids, 0);
    build_trace(dir_straddle, base, n, pids, 1);

    struct pgwt_server srv_plain, srv_straddle;
    memset(&srv_plain, 0, sizeof(srv_plain));
    memset(&srv_straddle, 0, sizeof(srv_straddle));
    if (server_init(&srv_plain, dir_plain) != 0 ||
        server_init(&srv_straddle, dir_straddle) != 0) {
        fprintf(stderr, "server_init failed\n");
        exit(1);
    }

    /* Window opens 1s after the earliest (pre-window marker) event, same
     * offset build_trace used when writing the mono timestamps. */
    uint64_t from_plain    = srv_plain.earliest_wall_ns    + 1 * ONE_SEC;
    uint64_t from_straddle = srv_straddle.earliest_wall_ns + 1 * ONE_SEC;
    uint64_t to_plain    = from_plain    + (uint64_t)WINDOW_SECS * ONE_SEC;
    uint64_t to_straddle = from_straddle + (uint64_t)WINDOW_SECS * ONE_SEC;

    const int warmup = 1, iters = 8;
    double plain_ms[iters], straddle_ms[iters];
    struct pgwt_load_info info;

    /* A load that silently admitted fewer records than written (a bad window,
     * an overloaded/failed load) would report a bogus, too-fast number
     * instead of refusing -- exactly the false-negative shape this measurement
     * exists to avoid. Check the FIRST call of each pair out loud rather than
     * trusting a near-zero timing that actually means "measured nothing". */
    for (int i = 0; i < warmup; i++) {
        int cnt;
        struct pgwt_trace_event *e = server_load_events_fi(
            &srv_plain, from_plain, to_plain, 0, &cnt, &info);
        if (cnt != n || info.overloaded || info.allocation_failed) {
            fprintf(stderr, "bench_left_seed: plain load admitted %d of %d "
                    "records (overloaded=%d alloc_failed=%d) -- refusing to "
                    "report a number that measured the wrong thing\n",
                    cnt, n, info.overloaded, info.allocation_failed);
            exit(1);
        }
        free(e);
    }
    for (int i = 0; i < iters; i++) {
        double t0 = now_ms();
        int cnt;
        struct pgwt_trace_event *e = server_load_events_fi(
            &srv_plain, from_plain, to_plain, 0, &cnt, &info);
        plain_ms[i] = now_ms() - t0;
        free(e);
    }

    for (int i = 0; i < warmup; i++) {
        int cnt;
        struct pgwt_trace_event *e = server_load_events_fi(
            &srv_straddle, from_straddle, to_straddle, 0, &cnt, &info);
        /* n filler waits + the straddler's in-window Lock wait, EXEC_END and
         * CMD_END (its EXEC_START/CMD_START sit before `from` by design --
         * #586's whole point is that those are NOT in this array). */
        if (cnt != n + 3 || info.overloaded || info.allocation_failed) {
            fprintf(stderr, "bench_left_seed: straddle load admitted %d of "
                    "%d+3 records (overloaded=%d alloc_failed=%d) -- "
                    "refusing to report a number that measured the wrong "
                    "thing\n", cnt, n, info.overloaded,
                    info.allocation_failed);
            exit(1);
        }
        free(e);
    }
    for (int i = 0; i < iters; i++) {
        double t0 = now_ms();
        int cnt;
        struct pgwt_trace_event *e = server_load_events_fi(
            &srv_straddle, from_straddle, to_straddle, 0, &cnt, &info);
        straddle_ms[i] = now_ms() - t0;
        free(e);
    }

    double plain_sum = 0, straddle_sum = 0;
    for (int i = 0; i < iters; i++) { plain_sum += plain_ms[i]; straddle_sum += straddle_ms[i]; }
    double plain_avg = plain_sum / iters, straddle_avg = straddle_sum / iters;

    printf("  B  n=%-9d full-load non-straddle=%8.3f ms   "
           "full-load straddle=%8.3f ms   delta=%7.3f ms   n=%d iters\n",
           n, plain_avg, straddle_avg, straddle_avg - plain_avg, iters);

    server_destroy(&srv_plain);
    server_destroy(&srv_straddle);
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", dir_plain, dir_straddle);
    if (system(cmd) != 0) { /* best effort cleanup */ }
}

int main(int argc, char **argv)
{
    int sizes[] = { 10000, 100000, 500000, 1000000 };
    int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    (void)argc; (void)argv;

    printf("#588: left_seed_find_needy isolated (A) and full bounded raw "
           "load (B), 900s window, warm/in-process (1 warmup + N timed "
           "iters discarding the cold call)\n\n");

    printf("-- A: left_seed_find_needy() alone, in-memory, 64 distinct pids --\n");
    for (int i = 0; i < nsizes; i++)
        bench_isolated(sizes[i], 64);

    printf("\n-- B: server_load_events_fi() end-to-end, real on-disk trace, 64 pids --\n");
    int b_sizes[] = { 10000, 100000, 500000, 1000000 };
    for (int i = 0; i < (int)(sizeof(b_sizes)/sizeof(b_sizes[0])); i++)
        bench_end_to_end(b_sizes[i], 64);

    return 0;
}
