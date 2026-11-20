/* Compile the real daemon loop on the host, with injected I/O and scheduling. */
#define main policy_cases_main
#include "test_rt_priority.c"
#undef main
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

struct epoll_event { union { int fd; } data; };
struct pgwt_daemon {
    struct pgwt_rt_priority rt_priority;
    bool running, exact_perf, debug_dump_state, coverage_clean_shutdown;
    void *rb, *event_rb;
    int duration, epoll_fd, exit_reason, final_drain_counters;
    uint64_t debug_last_loop_ts_ns, last_epoll_return_ns, debug_max_loop_gap_ns;
    uint64_t debug_loop_iterations, final_drain_from_ns;
};
#define PGWT_EXIT_PG_DEAD 9
static struct pgwt_daemon *loop_daemon;
static int dispatches, final_drains;
static bool inject_eintr;
static int ring_buffer__epoll_fd(void *rb) { (void)rb; return 42; }
static int pgwt_percpu_transport_epoll_fd(struct pgwt_daemon *d) { (void)d; return 43; }
static uint64_t pgwt_debug_monotonic_ns(void) { return 100; }
static uint64_t pgwt_debug_block_begin(struct pgwt_daemon *d) { (void)d; return 0; }
static void pgwt_debug_block_end(struct pgwt_daemon *d, const char *op, int pid, uint64_t ns)
{ (void)d; (void)op; (void)pid; (void)ns; }
static int epoll_wait(int fd, struct epoll_event *events, int count, int timeout)
{
    (void)fd; (void)count; (void)timeout;
    if (inject_eintr) { inject_eintr = false; errno = EINTR; return -1; }
    events[0].data.fd = 1;
    return 1;
}
static int pgwt_percpu_transport_poll(struct pgwt_daemon *d) { (void)d; return 0; }
static int pgwt_merge_capture_poll(struct pgwt_daemon *d) { (void)d; return 0; }
static void pgwt_sweep_bootstrap_backends(struct pgwt_daemon *d) { (void)d; }
static void dispatch_ready_fd(struct pgwt_daemon *d, int fd, int rb, int event)
{ (void)fd; (void)rb; (void)event; ++dispatches; d->running = false; }
static bool consume_event_ring(struct pgwt_daemon *d, const char *op, bool bounded)
{ (void)d; (void)op; (void)bounded; return false; }
static int pgwt_daemon_counter_vector(struct pgwt_daemon *d) { (void)d; return 0; }
static int pgwt_consume_lifecycle_ring(struct pgwt_daemon *d) { (void)d; ++final_drains; return 0; }
static int pgwt_merge_capture_finish(struct pgwt_daemon *d) { (void)d; return 0; }

#include PGWT_RT_LOOP_SOURCE

static void injected_watchdog(int sig)
{
    (void)sig;
    signal_handler(SIGXCPU);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2, "loop arm supplied");
    const char *arm = argv[1];
    printf("REACHED LOOP: %s\n", arm); fflush(stdout);
    struct pgwt_daemon daemon = { .running = true };
    loop_daemon = &daemon;
    total = 2;
    pgwt_rt_init(&loop_daemon->rt_priority, &ops);
    if (!strcmp(arm, "inherited")) {
        policy[1] = policy[2] = PGWT_RT_FIFO;
        priority[1] = priority[2] = 7;
    }
    pgwt_rt_prepare(&loop_daemon->rt_priority);
    if (!strcmp(arm, "refused")) refused = 1;
    if (!strcmp(arm, "disabled")) loop_daemon->rt_priority.disabled = true;
    if (!strcmp(arm, "signal")) {
        CHECK(setenv("PGWT_TEST_RT_SPIN", "1", 1) == 0, "set forced spin");
        struct sigaction action = { .sa_handler = injected_watchdog };
        sigemptyset(&action.sa_mask);
        CHECK(sigaction(SIGALRM, &action, NULL) == 0, "install injected CPU timer");
        struct itimerval timer = { .it_value = { .tv_usec = 20000 } };
        CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0, "arm injected CPU watchdog");
        inject_eintr = true;
    }
    CHECK(pgwt_daemon_run(loop_daemon) == 0, "actual loop returns successfully");
    CHECK(dispatches == 1 && final_drains == 1 && daemon.coverage_clean_shutdown,
          "actual loop continues capture and clean final drain");
    if (!strcmp(arm, "signal")) {
        CHECK(daemon.rt_priority.state == PGWT_RT_DEMOTED &&
              daemon.rt_priority.demotions == 1 && warnings == 1,
              "actual spinning loop demotes and records once");
    } else if (!strcmp(arm, "refused")) {
        CHECK(daemon.rt_priority.state == PGWT_RT_REFUSED && warnings == 1,
              "actual loop captures after warned refusal");
    } else {
        CHECK(daemon.rt_priority.state == (!strcmp(arm, "disabled")
              ? PGWT_RT_DISABLED : PGWT_RT_ACTIVE), "actual loop applies CLI policy");
    }
    CHECK((policy[2] & ~PGWT_RT_RESET) == PGWT_RT_OTHER, "actual loop leaves worker normal");
    puts("LOOP CAPTURE CONTINUES: PASS");
    return 0;
}
