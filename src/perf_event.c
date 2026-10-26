/* perf_event.c — perf_event_open wrapper for hardware watchpoints */
#include "perf_event.h"

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static long sys_perf_event_open(struct perf_event_attr *attr, pid_t pid,
                                int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int open_watchpoint_common(pid_t pid, uint64_t addr, int bpf_prog_fd,
                                  int start_enabled)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.type           = PERF_TYPE_BREAKPOINT;
    attr.size           = sizeof(attr);
    attr.bp_type        = HW_BREAKPOINT_W;
    attr.bp_addr        = addr;
    attr.bp_len         = HW_BREAKPOINT_LEN_4;
    attr.sample_period  = 1;
    attr.sample_type    = 0;             /* no sample data needed (BPF reads directly) */
    attr.wakeup_events  = 0;            /* BPF handles events, no perf buffer wakeup */
    attr.exclude_kernel = 1;            /* PGPROC is userspace-only */
    attr.exclude_hv     = 1;            /* no hypervisor writes */
    /* #390: SET_BPF below means this event has no mmap'd ring buffer and
     * nothing ever reads PERF_RECORD_THROTTLE/UNTHROTTLE — the kernel's
     * rate-limit check (perf_event_max_sample_rate) runs in the overflow
     * path BEFORE the BPF handler either way, so a throttled watchpoint
     * just stops firing, silently, however this event is configured. A
     * plain read(2) of the fd works regardless of SET_BPF/no-ring-buffer,
     * so TOTAL_TIME_ENABLED/_RUNNING is the one signal available: a
     * cumulative time_running shortfall against time_enabled IS the
     * throttle, quantified in ns. Requested unconditionally — the read is
     * cheap and unused bytes cost nothing if a caller never reads them. */
    attr.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED |
                          PERF_FORMAT_TOTAL_TIME_RUNNING;
    /* CAP-8: callers that preseed the BPF state_map open the watchpoint
     * disabled, seed, then enable — so the first fire always sees a seeded
     * state and the opening interval is timed correctly. */
    attr.disabled       = start_enabled ? 0 : 1;

    int fd = sys_perf_event_open(&attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        /* ESRCH = process exited before we could attach — expected for
         * short-lived backends. Don't spam stderr with these.
         * EMFILE/ENFILE (CAP-12) is a very different failure — the daemon
         * ran out of file descriptors, and every further backend would
         * silently record nothing. Scream, with the fix. */
        if (errno == EMFILE || errno == ENFILE) {
            fprintf(stderr,
                    "ERROR: perf_event_open(pid=%d): %s — file descriptor "
                    "limit reached.\n"
                    "  Backends beyond this point CANNOT be traced. The "
                    "daemon raises RLIMIT_NOFILE at startup; if this "
                    "persists, raise the hard limit (ulimit -Hn / "
                    "LimitNOFILE= in the systemd unit).\n",
                    pid, strerror(errno));
        } else if (errno != ESRCH) {
            fprintf(stderr, "perf_event_open(pid=%d, addr=0x%lx): %s\n",
                    pid, (unsigned long)addr, strerror(errno));
        }
        return -1;
    }

    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, bpf_prog_fd) != 0) {
        fprintf(stderr, "ioctl SET_BPF(pid=%d): %s\n", pid, strerror(errno));
        close(fd);
        return -1;
    }

    if (start_enabled && ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        fprintf(stderr, "ioctl ENABLE(pid=%d): %s\n", pid, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int bpf_prog_fd)
{
    return open_watchpoint_common(pid, addr, bpf_prog_fd, 1);
}

int pgwt_open_watchpoint_disabled(pid_t pid, uint64_t addr, int bpf_prog_fd)
{
    return open_watchpoint_common(pid, addr, bpf_prog_fd, 0);
}

int pgwt_watchpoint_enable(int perf_fd)
{
    if (ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        fprintf(stderr, "ioctl ENABLE: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int pgwt_watchpoint_disable(int perf_fd)
{
    if (perf_fd < 0)
        return 0;
    if (ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0) != 0) {
        fprintf(stderr, "ioctl DISABLE: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

void pgwt_close_watchpoint(int perf_fd)
{
    if (perf_fd >= 0)
        close(perf_fd);
}

/* #390: the throttle-visibility read. Layout for read_format ==
 * PERF_FORMAT_TOTAL_TIME_ENABLED|_RUNNING with no GROUP/ID bit set (the
 * only format open_watchpoint_common ever requests) is fixed by the ABI:
 * value, then time_enabled, then time_running, 8 bytes each, in that order
 * — see the perf_event_open(2) "Reading results" section. Both are
 * CUMULATIVE since the event was created (or since the last
 * PERF_EVENT_IOC_RESET, which this codebase never calls), so a caller
 * wanting a per-tick delta must track the previous read itself; this
 * function only reports the raw cumulative pair. */
int pgwt_watchpoint_read_times(int perf_fd, uint64_t *time_enabled,
                               uint64_t *time_running)
{
    if (time_enabled)
        *time_enabled = 0;
    if (time_running)
        *time_running = 0;
    if (perf_fd < 0)
        return -1;

    uint64_t buf[3]; /* value, time_enabled, time_running */
    ssize_t n = read(perf_fd, buf, sizeof(buf));
    if (n != (ssize_t)sizeof(buf))
        return -1;

    if (time_enabled)
        *time_enabled = buf[1];
    if (time_running)
        *time_running = buf[2];
    return 0;
}
