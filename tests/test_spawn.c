/* test_spawn.c -- child signal state must not inherit daemon signalfd policy. */
#include "spawn.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int report_signal_state(void)
{
    sigset_t current;
    struct sigaction int_action, term_action;
    if (sigprocmask(SIG_SETMASK, NULL, &current) != 0 ||
        sigaction(SIGINT, NULL, &int_action) != 0 ||
        sigaction(SIGTERM, NULL, &term_action) != 0)
        return 2;
    printf("%d %d %d %d\n",
           sigismember(&current, SIGINT),
           sigismember(&current, SIGTERM),
           int_action.sa_handler == SIG_DFL,
           term_action.sa_handler == SIG_DFL);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--report-signal-state") == 0)
        return report_signal_state();
    if (argc == 2 && strcmp(argv[1], "--stall") == 0) {
        for (;;)
            pause();
    }

    sigset_t shutdown_signals, old_mask;
    struct sigaction ignored = { .sa_handler = SIG_IGN };
    struct sigaction old_int, old_term;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    sigemptyset(&ignored.sa_mask);

    if (sigprocmask(SIG_BLOCK, &shutdown_signals, &old_mask) != 0 ||
        sigaction(SIGINT, &ignored, &old_int) != 0 ||
        sigaction(SIGTERM, &ignored, &old_term) != 0) {
        fprintf(stderr, "FAIL: cannot arrange parent signal state\n");
        return 1;
    }

    struct pgwt_proc proc;
    char *child_argv[] = {
        "/proc/self/exe", "--report-signal-state", NULL
    };
    char output[64] = {0};
    int opened = pgwt_proc_open(&proc, child_argv) == 0;
    int read_ok = opened && fgets(output, sizeof(output), proc.out) != NULL;
    int status = opened ? pgwt_proc_close(&proc) : -1;

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    int int_blocked = -1, term_blocked = -1;
    int int_default = -1, term_default = -1;
    int parsed = sscanf(output, "%d %d %d %d", &int_blocked, &term_blocked,
                        &int_default, &term_default) == 4;
    int passed = opened && read_ok && status >= 0 && WIFEXITED(status) &&
                 WEXITSTATUS(status) == 0 && parsed &&
                 int_blocked == 0 && term_blocked == 0 &&
                 int_default == 1 && term_default == 1;

    struct pgwt_proc stalled;
    char *stall_argv[] = { "/proc/self/exe", "--stall", NULL };
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    int stall_opened = pgwt_proc_open(&stalled, stall_argv) == 0;
    int stall_status = stall_opened
        ? pgwt_proc_close_bounded(&stalled, 50) : 0;
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ms = (end.tv_sec - begin.tv_sec) * 1000L +
                      (end.tv_nsec - begin.tv_nsec) / 1000000L;
    int bounded = stall_opened && stall_status == -1 && elapsed_ms < 2000;
    printf("=== test_spawn ===\n");
    printf("  %s: exec child restores SIGINT/SIGTERM mask and disposition "
           "(got %s)", passed ? "PASS" : "FAIL",
           read_ok ? output : "no output\n");
    printf("  %s: bounded close kills/reaps a stalled helper (%ldms)\n",
           bounded ? "PASS" : "FAIL", elapsed_ms);
    return passed && bounded ? 0 : 1;
}
