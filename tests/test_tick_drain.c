/* test_tick_drain.c — #416: a display tick must contain every record the
 * producer has already emitted.
 *
 * THE DEFECT. handle_timer built the display as a copy of the cumulative
 * event accumulator PLUS a LIVE pgwt_read_state_map() read for still-open
 * stretches, and did NOT drain the event ringbuf. The single-threaded epoll
 * loop drained afterwards, 64 callbacks per iteration, against a measured
 * producer rate of ~37,700 records/s. Any record still queued when the timer
 * fired was absent from that tick while state_map, read live, already showed
 * the interval it closed as re-opened. Most classes lose only the last few ms
 * of the stream, but a LONG wait's whole contribution rides on ONE record:
 * Timeout:PgSleep lost 100% of an 8.6 s sleep and whole-capture DB Time came
 * out 50.1% short. The trace on disk was correct the whole time.
 *
 * WHY THIS IS A UNIT TEST AND NOT ONLY A LIVE ONE. The live symptom
 * reproduced 1-2 times in 10 on gate-2, so a live gate alone is a coin flip
 * per run. The property itself is deterministic and has nothing to do with
 * PostgreSQL: "every record the ring held at tick entry is in the tick".
 * This drives the REAL pgwt_handle_timer — the defect was the ORDER of the
 * steps inside it, so a test calling a narrower seam could not have seen it.
 * Only ring_buffer__consume is replaced (-Wl,--wrap), by a fake ring that
 * feeds synthetic records through the production callback.
 *
 * ADVERSARIAL QUESTIONS, answered by the BYPASS section at the bottom:
 *   - What input makes this pass while the product is broken? An EMPTY ring:
 *     "display == emitted" is 0 == 0 either way. Pinned by case 1, which
 *     asserts its own emitted total is > 0 and that the accumulator did not
 *     already hold the record; by case 2, which queues more than one budget
 *     pass; and by B1 and B3, which show what an unobserved tick looks like.
 *   - What single component failing makes this skip rather than fail? A
 *     pgwt_handle_timer that returns early — the display is then untouched,
 *     and untouched reads the same as "correctly empty". Pinned by B2: the
 *     gate asserts the handler ran to completion (tick advanced) and that
 *     the drain was reached at all (consume_calls), and B2 shows those
 *     refuse rather than approve when the handler bails out. The other
 *     stall shape, a drain that never returns, is B5: its "endless"
 *     producer is finite on purpose so an unbounded drain FAILS the test
 *     rather than hanging it.
 *   - What depends on timing or ordering? The ordering the fix is about,
 *     pinned structurally: the wrapped consume is the ONLY writer of these
 *     records into event_accum, so if pgwt_accum_copy_used ran first the
 *     display CANNOT contain them. No wall clock, no sleeps, no PostgreSQL
 *     anywhere except B7, which measures the drain's own wall-clock budget
 *     and is deliberately ONE-SIDED (an upper bound on work done), so a
 *     slow runner makes it more true rather than flaky.
 */
#define _GNU_SOURCE
#include "daemon.h"
#include "map_reader.h"
#include "event_stream.h"
#include "pg_wait_tracer.h"
#include "pg_wait_tracer.skel.h"
#include "backend.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(cond, fmt, ...) do {                                  \
    checks++;                                                       \
    if (!(cond)) {                                                  \
        failures++;                                                 \
        printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__);   \
    }                                                               \
} while (0)

#define MS(x)  ((uint64_t)(x) * 1000000ULL)
#define PGSLEEP WEI(PG_WAIT_TIMEOUT, 2)     /* PG18 Timeout:PgSleep */

/* ------------------------------------------------------------------ *
 * The fake ring.                                                      *
 * ------------------------------------------------------------------ */
#define QUEUE_CAP 512
static struct pgwt_daemon *g_d;
static struct pgwt_trace_event queue[QUEUE_CAP];
static int queue_n;          /* records written into the queue */
static int queue_head;       /* next record to deliver */
static int consume_calls;    /* how often the daemon asked us to drain */
static int records_delivered;
static bool consume_delivers = true;  /* B1 turns this off */
static bool queue_endless;            /* B5: a producer that never stops */
static long endless_delivered;        /* records refilled in THIS tick */

/* B5's "endless" producer is deliberately NOT infinite. A product drain with
 * its cap REMOVED would otherwise never return, and the suite would HANG —
 * the stall-instead-of-fail shape this file exists to rule out. So the fake
 * ring stops refilling after a cap of its own, chosen well above the shipped
 * drain's 256 x 64 = 16,384 records: a correctly bounded drain gives up long
 * before reaching it (and reports the truncation), while an unbounded one
 * runs past it, finds the ring empty, reports NO truncation, and fails B5
 * with an explanation instead of hanging. */
#define ENDLESS_FAKE_CAP  (64L * 5000L)   /* 320,000 records */

/* libbpf contract this mirrors: ring_buffer__consume() invokes the callback
 * per record and returns the number consumed, OR the callback's own negative
 * return once it refuses — which is how the 64-callback budget yields
 * (src/event_stream.c returns -EAGAIN at the limit, src/daemon.c's
 * consume_event_ring reads rc < 0 as "records remain"). Records not yet
 * delivered stay queued for the next pass, exactly like the real ring. */
int __wrap_ring_buffer__consume(struct ring_buffer *rb);
int __wrap_ring_buffer__consume(struct ring_buffer *rb)
{
    (void)rb;
    consume_calls++;
    if (!consume_delivers)
        return 0;
    int n = 0;
    for (;;) {
        if (queue_head >= queue_n) {
            if (!queue_endless || queue_n == 0
                || endless_delivered >= ENDLESS_FAKE_CAP)
                break;
            queue_head = 0;     /* the producer refilled the ring */
        }
        if (queue_endless)
            endless_delivered++;
        struct pgwt_trace_event evt = queue[queue_head++];
        records_delivered++;
        n++;
        int rc = pgwt_handle_trace_event(g_d, &evt, sizeof(evt));
        if (rc < 0)
            return rc;
    }
    return n;
}

static void queue_reset(void)
{
    queue_n = queue_head = 0;
    consume_calls = records_delivered = 0;
    consume_delivers = true;
    queue_endless = false;
}

static uint64_t queue_push(uint32_t pid, uint32_t we, uint64_t dur_ns)
{
    if (queue_n >= QUEUE_CAP) {
        fprintf(stderr, "test bug: queue overflow\n");
        exit(2);
    }
    queue[queue_n++] = (struct pgwt_trace_event){
        .timestamp_ns = MS(1000) + (uint64_t)queue_n * 1000ULL,
        .pid = pid,
        .old_event = we,
        .new_event = 0,
        .flags = 0,
        .duration_ns = dur_ns,
        .query_id = 0,
        .cpu_ns = 0,
    };
    return dur_ns;
}

/* ------------------------------------------------------------------ *
 * Daemon fixture.                                                     *
 *                                                                     *
 * The skeleton is calloc'd, so every maps.* pointer is NULL and       *
 * bpf_map__fd(NULL) returns -EINVAL: pgwt_read_state_map iterates an  *
 * empty map and pgwt_read_bpf_fail_counter returns 0, both without    *
 * touching the kernel. No bpf_* wraps are needed, and the production  *
 * handler still executes every one of its stages.                     *
 * ------------------------------------------------------------------ */
struct fixture {
    struct pgwt_daemon *d;
    int pipe_r, pipe_w;
};

static struct fixture fixture_new(void)
{
    struct fixture f;
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); exit(2); }
    f.pipe_r = fds[0];
    f.pipe_w = fds[1];

    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d) exit(2);
    d->skel = calloc(1, sizeof(*d->skel));
    if (!d->skel) exit(2);
    d->event_accum = calloc(1, sizeof(*d->event_accum));
    if (!d->event_accum) exit(2);
    pgwt_accum_init(d->event_accum);
    pgwt_accum_init(&d->accum);
    pgwt_backend_init(&d->backends);

    d->mode = PGWT_MODE_FULL;
    d->timer_fd = f.pipe_r;
    d->quiet = true;            /* no view output in a unit test */
    d->daemon_mode = false;     /* no postmaster health check */
    d->interval = 1;
    d->running = true;
    d->postmaster_pid = getpid();
    /* Opaque, never dereferenced: __wrap_ring_buffer__consume ignores it.
     * Non-NULL is what makes drain_event_ring_for_tick run at all. */
    d->event_rb = (struct ring_buffer *)(void *)d;

    f.d = d;
    g_d = d;
    return f;
}

static void fixture_free(struct fixture *f)
{
    close(f->pipe_r);
    close(f->pipe_w);
    free(f->d->event_accum);
    free(f->d->skel);
    free(f->d);
    g_d = NULL;
}

/* Make the timerfd readable the way the kernel would, then run one tick. */
static void run_tick(struct fixture *f)
{
    endless_delivered = 0;      /* B5's fake producer cap is per-tick */
    uint64_t expirations = 1;
    if (write(f->pipe_w, &expirations, sizeof(expirations))
            != (ssize_t)sizeof(expirations)) {
        perror("write timerfd stand-in");
        exit(2);
    }
    pgwt_handle_timer(f->d);
}

static uint64_t sys_row(const struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == we)
            return acc->system_events[i].total_ns;
    return 0;
}

static uint64_t sys_count(const struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == we)
            return acc->system_events[i].count;
    return 0;
}

int main(void)
{
    /* The handler's two optional repair passes read live BPF state we do not
     * have; both have a production env guard, which is what they are for. */
    setenv("PGWT_TEST_NO_RECOVERY", "1", 1);
    setenv("PGWT_TEST_NO_STALE_SWEEP", "1", 1);

    printf("=== #416: the tick contains every emitted record ===\n");

    /* ---------------------------------------------------------------- *
     * 1. The reproduction, as measured: ONE long closing record queued  *
     *    when the timer fires. This is the whole defect — before the    *
     *    fix the display showed 0 ms of an 8605.7 ms sleep.             *
     * ---------------------------------------------------------------- */
    {
        struct fixture f = fixture_new();
        queue_reset();
        const uint64_t sleep_ns = 8605746700ULL;   /* attempt01's real value */
        uint64_t emitted = queue_push(4242, PGSLEEP, sleep_ns);

        /* Non-vacuity: the record must not already be in the accumulator,
         * or the post-tick equality proves nothing. */
        CHECK(emitted > 0, "the positive case queues a nonzero duration");
        CHECK(sys_row(f.d->event_accum, PGSLEEP) == 0,
              "pre-tick: the record is NOT already accumulated");
        CHECK(sys_row(&f.d->accum, PGSLEEP) == 0,
              "pre-tick: the display is NOT already showing it");

        run_tick(&f);

        /* The gate can see: the handler ran to the end, and the drain was
         * actually reached. Either being false makes the value assertions
         * below meaningless, so they are checked first. */
        CHECK(f.d->tick == 1,
              "the handler completed the tick (tick=%d, expected 1)",
              f.d->tick);
        CHECK(consume_calls >= 1,
              "the tick drained the ring (consume_calls=%d)", consume_calls);
        CHECK(records_delivered == 1,
              "the queued record was delivered (delivered=%d)",
              records_delivered);

        /* CONSERVATION, not a floor: the display total equals the emitted
         * total exactly. A `> 2000 ms` floor passes on a 3 s truncation of
         * this same 8.6 s record, which is why #416 survived for months. */
        CHECK(sys_row(&f.d->accum, PGSLEEP) == emitted,
              "display Timeout:PgSleep = %llu ns, emitted %llu ns",
              (unsigned long long)sys_row(&f.d->accum, PGSLEEP),
              (unsigned long long)emitted);
        CHECK(sys_count(&f.d->accum, PGSLEEP) == 1,
              "display Timeout:PgSleep count = %llu, expected 1",
              (unsigned long long)sys_count(&f.d->accum, PGSLEEP));
        CHECK(f.d->accum.tm.timeout_time_ns == emitted,
              "time-model Timeout = %llu ns, emitted %llu ns",
              (unsigned long long)f.d->accum.tm.timeout_time_ns,
              (unsigned long long)emitted);
        CHECK(f.d->accum.tm.db_time_ns >= emitted,
              "DB Time (%llu ns) is not short of the one wait it contains",
              (unsigned long long)f.d->accum.tm.db_time_ns);
        CHECK(f.d->counters.tick_drain_records_total == 1,
              "tick_drain_records_total = %llu, expected 1",
              (unsigned long long)f.d->counters.tick_drain_records_total);
        CHECK(f.d->counters.tick_drain_truncated_total == 0,
              "one record does not truncate the drain");

        fixture_free(&f);
    }

    /* ---------------------------------------------------------------- *
     * 2. A backlog DEEPER THAN ONE 64-CALLBACK BUDGET PASS.             *
     *    At the ~37,700 records/s measured in #416, one 10 ms main-loop *
     *    quantum is ~377 records, so a drain of a single budgeted pass  *
     *    would still leave ~5/6 of the steady-state backlog out of the  *
     *    tick. This fails on a one-pass fix as well as on no fix.       *
     * ---------------------------------------------------------------- */
    {
        struct fixture f = fixture_new();
        queue_reset();
        const int N = 400;              /* > 377, and > 6 budget passes */
        const uint64_t each = MS(1);
        uint64_t emitted = 0;
        for (int i = 0; i < N; i++)
            emitted += queue_push(5000 + (uint32_t)(i % 7), PGSLEEP, each);

        CHECK(N > 64,
              "the backlog case exceeds one 64-callback budget pass (N=%d)",
              N);
        run_tick(&f);

        CHECK(f.d->tick == 1, "backlog tick completed");
        CHECK(records_delivered == N,
              "every queued record was drained (%d of %d)",
              records_delivered, N);
        CHECK(consume_calls > 1,
              "a backlog needed more than one budgeted pass (calls=%d)",
              consume_calls);
        CHECK(sys_row(&f.d->accum, PGSLEEP) == emitted,
              "display = %llu ns, emitted %llu ns",
              (unsigned long long)sys_row(&f.d->accum, PGSLEEP),
              (unsigned long long)emitted);
        CHECK(f.d->counters.tick_drain_max_records == (uint64_t)N,
              "tick_drain_max_records = %llu, expected %d",
              (unsigned long long)f.d->counters.tick_drain_max_records, N);
        CHECK(f.d->counters.tick_drain_truncated_total == 0,
              "%d records is far under the pass cap", N);

        /* No double counting from the new second drain site: a following
         * tick with an empty ring must report the SAME total, not twice it.
         * (pgwt_accum_copy_used overwrites; this pins that it stays so.) */
        int delivered_after_first = records_delivered;
        run_tick(&f);
        CHECK(f.d->tick == 2, "second tick completed");
        CHECK(records_delivered == delivered_after_first,
              "an empty ring delivers nothing on the next tick");
        CHECK(sys_row(&f.d->accum, PGSLEEP) == emitted,
              "the total is unchanged by a second tick (%llu vs %llu)",
              (unsigned long long)sys_row(&f.d->accum, PGSLEEP),
              (unsigned long long)emitted);

        fixture_free(&f);
    }

    /* ================================================================ *
     * BYPASS SUITE — ways this gate's DETECTION could be made           *
     * unreachable. A gate that cannot see must refuse, never approve.   *
     * ================================================================ */
    printf("=== bypass suite: the gate must refuse when it cannot see ===\n");

    /* B1. The drain is present but consumes NOTHING (the dependency is a
     *     no-op: a consume that returns 0 without delivering). If the
     *     positive assertions above could be satisfied by anything other
     *     than the drain, this case would stay green. It must not. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        uint64_t emitted = queue_push(4242, PGSLEEP, MS(8605));
        consume_delivers = false;

        run_tick(&f);

        CHECK(f.d->tick == 1, "B1: the handler still completed");
        CHECK(consume_calls >= 1, "B1: the drain was still called");
        CHECK(records_delivered == 0, "B1: nothing was delivered, by design");
        /* The detection: with no delivery the display MUST be short. If this
         * ever reads `== emitted`, the positive case is not measuring the
         * drain and the whole test is worthless. */
        CHECK(sys_row(&f.d->accum, PGSLEEP) != emitted,
              "B1: a non-delivering drain leaves the display SHORT "
              "(got %llu, emitted %llu) — the positive case is "
              "drain-sensitive",
              (unsigned long long)sys_row(&f.d->accum, PGSLEEP),
              (unsigned long long)emitted);
        CHECK(f.d->counters.tick_drain_records_total == 0,
              "B1: the counter reports zero recovered, not a guess");
        fixture_free(&f);
    }

    /* B2. The handler RETURNS EARLY (PostgreSQL gone) — the "skip, don't
     *     fail" shape. The display is untouched, which reads identically to
     *     "correctly empty" if you only look at values. The gate's liveness
     *     assertions must notice: tick must NOT have advanced, and the drain
     *     must NOT have been reached. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        queue_push(4242, PGSLEEP, MS(8605));
        f.d->daemon_mode = true;
        /* Above every attainable pid_max (4194304 on 64-bit Linux), so
         * kill() is ESRCH deterministically — no fork, no pid reuse, no
         * flake. NOT -1 or 0: both are broadcast forms that SUCCEED. */
        f.d->postmaster_pid = 2147483647;
        CHECK(kill(f.d->postmaster_pid, 0) != 0,
              "B2 precondition: the stand-in postmaster pid is really dead");

        run_tick(&f);

        CHECK(f.d->tick == 0,
              "B2: an early return does NOT advance tick (tick=%d)",
              f.d->tick);
        CHECK(consume_calls == 0,
              "B2: an early return never reaches the drain (calls=%d)",
              consume_calls);
        CHECK(f.d->running == false, "B2: the early return stopped the loop");
        /* So the liveness assertions used in cases 1 and 2 (tick == 1 and
         * consume_calls >= 1) are the thing that stops a skipped handler
         * being read as a pass. Demonstrated, not asserted in prose. */
        fixture_free(&f);
    }

    /* B3. EMPTY INPUT — the thing being checked is ABSENT rather than
     *     wrong. "display == emitted" is 0 == 0 and tells you nothing. The
     *     gate must be able to tell this apart from a real observation, so
     *     the positive cases assert a nonzero emitted total (checked at the
     *     top of case 1) and this case pins what an empty tick looks like. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        uint64_t emitted = 0;           /* nothing queued, deliberately */

        run_tick(&f);

        CHECK(f.d->tick == 1, "B3: the handler completed on an empty ring");
        CHECK(sys_row(&f.d->accum, PGSLEEP) == emitted,
              "B3: an empty ring yields an empty display");
        CHECK(records_delivered == 0, "B3: nothing to deliver");
        CHECK(f.d->counters.tick_drain_records_total == 0,
              "B3: the drain reports zero, and zero is not evidence");
        /* The guard that makes case 1 non-vacuous, restated here as an
         * executable statement rather than a comment: the equality used
         * there is only meaningful because its right-hand side is > 0. */
        CHECK(emitted == 0,
              "B3: this case's emitted total is zero, so its equality is "
              "deliberately uninformative — case 1 asserts emitted > 0");
        fixture_free(&f);
    }

    /* B4. NO RING AT ALL (the lightweight / no-event_rb build): the drain
     *     must be a clean no-op, not a crash and not a fabricated count. A
     *     gate that only ever runs with a ring present would never find out. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        queue_push(4242, PGSLEEP, MS(8605));
        f.d->event_rb = NULL;

        run_tick(&f);

        CHECK(f.d->tick == 1, "B4: a daemon with no event ring still ticks");
        CHECK(consume_calls == 0, "B4: no ring means no consume attempt");
        CHECK(f.d->counters.tick_drain_records_total == 0,
              "B4: no ring recovers nothing, and says so");
        fixture_free(&f);
    }

    /* B5. A PARTIAL drain: the producer outruns the cap. Modelled by
     *     refusing to deliver past a point — the ring still reports a
     *     backlog after every pass. The drain must give up BOUNDED (so the
     *     tick is not hostage to the producer), count the truncation, and
     *     say so loudly. Silently returning a short tick is the #416 shape;
     *     the whole point is that it must never be silent again. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        for (int i = 0; i < 64; i++)
            queue_push(6000, PGSLEEP, MS(1));
        queue_endless = true;   /* the ring refills as fast as we drain it */

        run_tick(&f);

        CHECK(f.d->tick == 1,
              "B5: the tick COMPLETED against an endless producer — the "
              "drain is bounded, so the next tick is not hostage to the "
              "workload (tick=%d)", f.d->tick);
        CHECK(f.d->counters.tick_drain_truncated_total == 1,
              "B5: the truncation is COUNTED, not absorbed (total=%llu)",
              (unsigned long long)f.d->counters.tick_drain_truncated_total);
        CHECK(f.d->tick_drain_truncated_logged,
              "B5: the truncation was logged loudly, once");
        /* Which bound fired is NOT pinned here on purpose: 16,384 records at
         * ~5 us each is ~87 ms against a 500 ms budget, so the record cap
         * normally wins, but a loaded runner could cross the budget first.
         * Pinning it would be hardening a timing race into an assertion.
         * What must hold either way is that a bound was recorded at all --
         * a truncation that cannot say WHY is a WARN that cannot be
         * trusted. B7 pins the specific value, where it is structural. */
        CHECK(f.d->counters.tick_drain_last_bound
                  != PGWT_TICK_DRAIN_BOUND_NONE,
              "B5: the truncation recorded WHICH bound it hit (got %d)",
              (int)f.d->counters.tick_drain_last_bound);
        /* The cap is read off the shipped constant via its observable
         * effect: the drain stopped after a bounded number of records, and
         * that number is what the WARN reports. A smaller/larger cap simply
         * changes this number; a REMOVED cap makes this case hang, which is
         * the failure mode the bound exists to prevent. */
        CHECK(records_delivered > 64
                  && (long)records_delivered < ENDLESS_FAKE_CAP,
              "B5: the drain LOOPED past one budget pass and then stopped "
              "on its OWN cap, after %d records — reaching the fake "
              "producer's %ld-record cap instead would mean the product "
              "drain is unbounded",
              records_delivered, (long)ENDLESS_FAKE_CAP);
        CHECK(f.d->counters.tick_drain_max_records
                  == (uint64_t)records_delivered,
              "B5: the counter reports what was really drained (%llu vs %d)",
              (unsigned long long)f.d->counters.tick_drain_max_records,
              records_delivered);
        queue_endless = false;
        fixture_free(&f);
    }

    /* B6. A SECOND truncation must not log twice but MUST keep counting —
     *     a one-shot latch that also silences the counter would turn a
     *     persistent degradation into a single historical blip. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        for (int i = 0; i < 64; i++)
            queue_push(6000, PGSLEEP, MS(1));
        queue_endless = true;

        run_tick(&f);
        run_tick(&f);

        CHECK(f.d->counters.tick_drain_truncated_total == 2,
              "B6: both truncated ticks are counted (total=%llu)",
              (unsigned long long)f.d->counters.tick_drain_truncated_total);
        queue_endless = false;
        fixture_free(&f);
    }

    /* B7. A SLOW consumer, not a fast producer: the record cap bounds how
     *     many records the drain takes, never how long each one takes.
     *     tests/test_capture_smoke.py's event-ring phase deliberately makes
     *     one trace callback 20 ms (10,000x production) and asserts the
     *     DISPLAY TIMER KEEPS RUNNING, so a record-only bound would let one
     *     tick spend 256 x 64 x 20 ms inside this drain. The wall-clock
     *     budget must stop it far earlier, and still count the truncation.
     *     The assertion is one-sided (an upper bound on work done), so a
     *     slow runner makes it MORE true, not flaky. */
    {
        struct fixture f = fixture_new();
        queue_reset();
        for (int i = 0; i < 64; i++)
            queue_push(7000, PGSLEEP, MS(1));
        queue_endless = true;
        f.d->test_event_callback_delay_us = 20000;   /* 20 ms per record */
        /* Bounded like B5's producer, and for the same reason: with BOTH
         * product bounds removed this hook would otherwise delay 320,000
         * records by 20 ms each — 1.8 hours of "still running" instead of a
         * failure. 200 delays is 4 s whatever the implementation does, and
         * a correct drain uses only the first pass's 64. */
        f.d->test_event_callback_delays_left = 200;

        run_tick(&f);

        CHECK(f.d->tick == 1,
              "B7: the tick completed despite 20 ms callbacks (tick=%d)",
              f.d->tick);
        CHECK(f.d->counters.tick_drain_truncated_total == 1,
              "B7: the time-bounded give-up is counted (total=%llu)",
              (unsigned long long)f.d->counters.tick_drain_truncated_total);
        /* The specific bound IS pinned here, and it is structural rather
         * than timing-dependent: one 64-record pass of 20 ms nanosleeps is
         * >= 1.28 s against a 500 ms budget on any machine, and nanosleep
         * cannot run short, so the deadline is always already past at the
         * first check and the record cap (16,384) is nowhere near. This is
         * what makes the WARN's "hit its time budget (500 ms)" text
         * assertable instead of merely plausible -- message and field come
         * from the same branch. */
        CHECK(f.d->counters.tick_drain_last_bound
                  == PGWT_TICK_DRAIN_BOUND_TIME,
              "B7: the bound reported is the TIME budget, not the record "
              "cap (got %d)", (int)f.d->counters.tick_drain_last_bound);
        /* 500 ms / 20 ms = 25 records' worth of budget; the drain checks the
         * deadline once per 64-record pass, so one pass (64) is the
         * granularity. Two passes would already be ~2.5 s. */
        CHECK(records_delivered <= 128,
              "B7: the WALL-CLOCK budget stopped the drain after %d records, "
              "not the %u-record cap",
              records_delivered,
              256U * 64U);
        queue_endless = false;
        f.d->test_event_callback_delay_us = 0;
        fixture_free(&f);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0)
        printf("PASS: tick drain conservation\n");
    return failures ? 1 : 0;
}
