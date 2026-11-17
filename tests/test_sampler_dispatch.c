/* Host regression: real dispatch, lifecycle consume, FORK enrollment and
 * sampler target construction; fake only kernel/process I/O. Reuse the
 * initialized shared-row fixture and production extraction of the I/O suite. */
#define main sampled_attr_suite_main
#include "test_sampled_attr_io.c"
#undef main
#include <assert.h>
#include "control.h"

enum { SAMPLE_FD = 20, LIFECYCLE_FD = 21, EVENT_FD = 22 };
static struct pgwt_daemon *dispatch_daemon;
static int queued_fork, ring_calls, provider_calls, rearm_calls, timer_reads;
static int poll_targets, emitted_samples, benchmark_clocks;
static uint64_t clock_calls;
#ifdef PGWT_SAMPLE_TICK_DELAY_TEST
static int sleep_calls;
static uint64_t slept_ns;
static int debug_sleep(const struct timespec *delay, struct timespec *remaining)
{
    assert(provider_calls == rearm_calls && provider_calls > 0 && !remaining);
    ++sleep_calls;
    slept_ns = (uint64_t)delay->tv_sec * UINT64_C(1000000000) + delay->tv_nsec;
    return 0;
}
#endif

static uint64_t host_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}
static uint64_t dispatch_clock(void)
{
    ++clock_calls;
    return benchmark_clocks ? host_ns() : clock_ns;
}
static int ring_buffer__consume(struct ring_buffer *rb)
{
    assert(rb == dispatch_daemon->rb);
    ++ring_calls;
    if (!queued_fork) return 0;
    queued_fork = 0;
    /* The callback's FORK handler retains its production identity enrollment.
     * The backend has already published its PGPROC and shared status row. */
    assert(pgwt_handle_fork(dispatch_daemon, 1000) == 0);
    return 1;
}
static int read_pending_init_state(void *ctx, pid_t pid, struct pgwt_pid_state *out)
{ (void)ctx; (void)pid; (void)out; abort(); }
static int attach_stored_init(void *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{ (void)ctx; (void)pid; (void)addr; (void)ts; abort(); }
static ssize_t sample_timer_read(int fd, void *out, size_t size)
{
    assert(fd == SAMPLE_FD && size == sizeof(uint64_t));
    *(uint64_t *)out = 1;
    ++timer_reads;
    return (ssize_t)size;
}
static void arm_sample_timer_jittered(struct pgwt_daemon *d)
{ assert(d == dispatch_daemon); ++rearm_calls; }
static void handle_signal(struct pgwt_daemon *d) { (void)d; abort(); }
void pgwt_handle_timer(struct pgwt_daemon *d) { (void)d; abort(); }
bool pgwt_escalation_is_timer_fd(const struct pgwt_daemon *d, int fd)
{ (void)d; (void)fd; return false; }
void pgwt_escalation_on_timer(struct pgwt_daemon *d) { (void)d; abort(); }
int pgwt_control_handle_fd(struct pgwt_control *control, int fd)
{ (void)control; (void)fd; abort(); }

#define read sample_timer_read
#define nanosleep debug_sleep
#define pgwt_debug_monotonic_ns dispatch_clock
#include PGWT_DISPATCH_SOURCE
#undef pgwt_debug_monotonic_ns
#undef read
#undef nanosleep

static int capture_poll(struct pgwt_daemon *d)
{
    ++provider_calls;
    assert(rearm_calls == provider_calls); /* keep re-arm at tick start */
    emitted_samples = record_registry_tick(d, clock_ns);
    poll_targets = sample_registry(d, clock_ns);
    return 0;
}
static const struct pgwt_capture_provider provider = {.poll = capture_poll};

static void dispatch_case(int major, int ring_first, int ring_returned)
{
    struct pgwt_daemon *d = new_daemon(major);
    dispatch_daemon = d;
    d->rb = (struct ring_buffer *)d;
    d->sample_timer_fd = SAMPLE_FD;
    d->timer_fd = 10; d->signal_fd = 11;
    d->provider = &provider;
    wait_addr = major == 13 ? test_wait(major, 0) : (uintptr_t)&wait_words[0];
    queued_fork = 1;
    ring_calls = provider_calls = rearm_calls = timer_reads = 0;
    poll_targets = emitted_samples = -1;
    struct { struct { int fd; } data; } events[2] = {
        {{ring_first ? LIFECYCLE_FD : SAMPLE_FD}},
        {{ring_first ? SAMPLE_FD : LIFECYCLE_FD}}
    };
    int n = ring_returned ? 2 : 1;
    for (int i = 0; i < n; ++i)
        dispatch_ready_fd(d, events[i].data.fd, LIFECYCLE_FD, EVENT_FD);
    if (poll_targets != 1 || recorded_samples[0].pid != 1000 || emitted_samples != 1) {
        fprintf(stderr, "FAIL: PG%d queued FORK pid=1000 must be a sampler target "
                "in the same poll (targets=%d samples=%d ring_first=%d ring_returned=%d)\n",
                major, poll_targets, emitted_samples, ring_first, ring_returned);
        exit(1);
    }
    assert(provider_calls == 1 && rearm_calls == 1 && timer_reads == 1);
    assert(ring_calls == (ring_returned ? 2 : 1) && !queued_fork);
    assert(d->backends.entries[0].sampled_reader.start_time == 1);
    /* Empty next poll neither duplicates enrollment nor adds another wakeup. */
    dispatch_ready_fd(d, SAMPLE_FD, LIFECYCLE_FD, EVENT_FD);
    assert(provider_calls == 2 && rearm_calls == 2 && d->backends.count == 1);
    assert(poll_targets == 1 && emitted_samples == 1);
    release_daemon(d);
}

static void benchmark(void)
{
    struct pgwt_daemon *d = new_daemon(18);
    dispatch_daemon = d;
    d->rb = (struct ring_buffer *)d;
    queued_fork = 0; benchmark_clocks = 1;
    const int ticks = 1000000;
    for (int population = 0; population <= 500; population += 500) {
        d->backends.count = population;
        for (int i = 0; i < population; ++i) {
            d->backends.entries[i].pid = 1000 + i;
            d->backends.entries[i].is_alive = true;
        }
        ring_calls = 0; clock_calls = 0;
        uint64_t start = host_ns();
        for (int i = 0; i < ticks; ++i)
            pgwt_consume_lifecycle_ring(d);
        uint64_t elapsed = host_ns() - start;
        assert(ring_calls == ticks && clock_calls == 2 * (uint64_t)ticks);
        assert(!d->backends.pending_init_count);
        printf("empty pre-tick consume: population=%d ticks=%d ring_calls/tick=1 "
               "clock_calls/tick=2 ns/tick=%.2f (host ring shim; production consume+sweep)\n",
               population, ticks, (double)elapsed / ticks);
    }
    benchmark_clocks = 0;
    release_daemon(d);
}

#ifdef PGWT_SAMPLE_TICK_DELAY_TEST
static void delay_cases(void)
{
    struct pgwt_daemon *d = new_daemon(18);
    dispatch_daemon = d;
    d->sample_timer_fd = SAMPLE_FD;
    d->provider = &provider;
    provider_calls = rearm_calls = sleep_calls = 0;
    assert(unsetenv("PGWT_DEBUG_SAMPLE_TICK_DELAY_US") == 0);
    d->debug_sample_tick_delay_us = 123; /* re-init must clear it */
    configure_sample_tick_delay(d);
    assert(!d->debug_sample_tick_delay_us);
    handle_sample_timer(d);
    assert(!sleep_calls);
    assert(setenv("PGWT_DEBUG_SAMPLE_TICK_DELAY_US", "20000", 1) == 0);
    handle_sample_timer(d); /* env is cached, not polled each tick */
    assert(!sleep_calls);
    configure_sample_tick_delay(d);
    handle_sample_timer(d);
    assert(sleep_calls == 1 && slept_ns == UINT64_C(20000000));
    const char *invalid[] = {"", "0", "-1", "+1", "x", "1x", "1000001",
                             "999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        assert(setenv("PGWT_DEBUG_SAMPLE_TICK_DELAY_US", invalid[i], 1) == 0);
        configure_sample_tick_delay(d);
        assert(!d->debug_sample_tick_delay_us);
        handle_sample_timer(d);
        assert(sleep_calls == 1);
    }
    assert(unsetenv("PGWT_DEBUG_SAMPLE_TICK_DELAY_US") == 0);
    release_daemon(d);
    puts("sample tick delay: PASS (unset, cached, opt-in after capture, invalid values)");
}
#endif

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) {
        benchmark();
        return 0;
    }
    for (int i = 0; i < 3; ++i) {
        const int major[] = {13, 17, 18};
        dispatch_case(major[i], 0, 1); /* [sample_timer, lifecycle_ring] */
        dispatch_case(major[i], 1, 1); /* [lifecycle_ring, sample_timer] */
        dispatch_case(major[i], 0, 0); /* FORK arrived after epoll_wait */
    }
    assert(!failures);
    puts("sampler dispatch: PASS (PG13/17/18, both fd orders, post-epoll FORK, next tick)");
#ifdef PGWT_SAMPLE_TICK_DELAY_TEST
    delay_cases();
#endif
    return 0;
}
