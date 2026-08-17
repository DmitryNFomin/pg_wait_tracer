/* spawn.c — fork/execvp child-process helper (CAP-7). See spawn.h. */
#define _GNU_SOURCE   /* pipe2 */
#include "spawn.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

static int proc_open(struct pgwt_proc *p, char *const argv[], bool writable)
{
    p->out = NULL;
    p->in = NULL;
    p->pid = -1;

    int out_fds[2];
    int in_fds[2] = {-1, -1};
    if (pipe2(out_fds, O_CLOEXEC) != 0)
        return -1;
    if (writable && pipe2(in_fds, O_CLOEXEC) != 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        if (writable) {
            close(in_fds[0]);
            close(in_fds[1]);
        }
        return -1;
    }

    if (pid == 0) {
        /* The daemon blocks shutdown signals before creating threads so its
         * signalfd owns graceful shutdown.  A fork/exec helper must not carry
         * that process-internal policy into runuser/psql/readelf children:
         * exec preserves the signal mask (and ignored dispositions). */
        sigset_t shutdown_signals;
        struct sigaction default_action = { .sa_handler = SIG_DFL };
        sigemptyset(&shutdown_signals);
        sigaddset(&shutdown_signals, SIGINT);
        sigaddset(&shutdown_signals, SIGTERM);
        sigemptyset(&default_action.sa_mask);
        if (sigaction(SIGINT, &default_action, NULL) != 0 ||
            sigaction(SIGTERM, &default_action, NULL) != 0 ||
            sigprocmask(SIG_UNBLOCK, &shutdown_signals, NULL) != 0)
            _exit(127);

        /* Child: stdout -> pipe and stderr -> /dev/null.  The read-only
         * variant also maps stdin to /dev/null; the bidirectional variant
         * maps it to the parent's input pipe.  dup2 clears O_CLOEXEC on the
         * duplicated fd, so the selected descriptors survive exec. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            if (!writable)
                dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        if (writable && dup2(in_fds[0], STDIN_FILENO) < 0)
            _exit(127);
        if (dup2(out_fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);   /* exec failed */
    }

    close(out_fds[1]);
    if (writable)
        close(in_fds[0]);
    FILE *out = fdopen(out_fds[0], "r");
    FILE *in = writable ? fdopen(in_fds[1], "w") : NULL;
    if (!out || (writable && !in)) {
        if (out)
            fclose(out);
        else
            close(out_fds[0]);
        if (writable) {
            if (in)
                fclose(in);
            else
                close(in_fds[1]);
        }
        /* Reap the child we can no longer talk to. */
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    p->out = out;
    p->in = in;
    p->pid = pid;
    return 0;
}

int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{
    return proc_open(p, argv, false);
}

int pgwt_proc_open_rw(struct pgwt_proc *p, char *const argv[])
{
    return proc_open(p, argv, true);
}

static int close_pipes(struct pgwt_proc *p)
{
    if (!p->out)
        return -1;
    if (p->in) {
        fclose(p->in);
        p->in = NULL;
    }
    fclose(p->out);
    p->out = NULL;
    return 0;
}

int pgwt_proc_close(struct pgwt_proc *p)
{
    if (close_pipes(p) != 0)
        return -1;

    int status = 0;
    pid_t r;
    do {
        r = waitpid(p->pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    p->pid = -1;
    return (r < 0) ? -1 : status;
}

int pgwt_proc_close_bounded(struct pgwt_proc *p, int timeout_ms)
{
    if (close_pipes(p) != 0)
        return -1;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t deadline_ms = (uint64_t)ts.tv_sec * 1000 +
                           (uint64_t)ts.tv_nsec / 1000000 +
                           (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == p->pid) {
            p->pid = -1;
            return status;
        }
        if (r < 0 && errno != EINTR) {
            p->pid = -1;
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 +
                          (uint64_t)ts.tv_nsec / 1000000;
        if (now_ms >= deadline_ms)
            break;
        usleep(10000);
    }

    /* This is the exact fork child, not a caller-supplied/system PID. Reap it
     * after SIGKILL so an overloaded helper cannot make daemon startup wait
     * forever or leave a zombie. */
    (void)kill(p->pid, SIGKILL);
    pid_t r;
    do {
        r = waitpid(p->pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    p->pid = -1;
    return -1;
}
