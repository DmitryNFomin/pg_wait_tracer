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
