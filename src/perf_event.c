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

/* No-throttle invariant (issue #390): measured on a Linux 6.8.0 gate box,
 * HZ=1000, with kernel.perf_event_max_sample_rate lowered to 250: the per-tick
 * allowance was 1 sample. 200000 writes to a watched 4-byte word produced
 * exactly 200000 BPF-handler invocations and 200000 kernel-counted events,
 * with zero PERF_RECORD_THROTTLE records in a ring mmap-ed on the fd.
 * Positive control: the same box and kernel DO throttle a cpu-clock event;
 * perf record -F 100000 over the same interval logged 5123 THROTTLE and
 * 5124 UNTHROTTLE records. Throttling is live; the zero is a real absence,
 * not a blind measurement.
 * perf_swevent_overflow() calls perf_swevent_set_period() and iterates with
 * throttle=0 on the FIRST overflow, setting it to 1 only for later ones.
 * __perf_event_account_interrupt() can hit MAX_INTERRUPTS only with throttle=1.
 * sample_period=1 and freq=0 give exactly one overflow per breakpoint write,
 * making per-tick throttling unreachable. PERF_TYPE_BREAKPOINT REJECTS
 * attr.freq with EINVAL (also measured), so sample_period is the only lever:
 * raising it above 1 would make per-tick throttling reachable.
 * test_wp_attr pins the ATTRIBUTE, not the loss: it is a structural pin on
 * the one input the measurement showed to be load-bearing, and is not itself
 * a measurement that no fire was lost.
 */
void pgwt_fill_watchpoint_attr(struct perf_event_attr *attr, uint64_t addr,
                               int start_enabled)
{
    memset(attr, 0, sizeof(*attr));

    attr->type           = PERF_TYPE_BREAKPOINT;
    attr->size           = sizeof(*attr);
    attr->bp_type        = HW_BREAKPOINT_W;
    attr->bp_addr        = addr;
    attr->bp_len         = HW_BREAKPOINT_LEN_4;
    attr->sample_period  = 1;
    attr->sample_type    = 0;             /* no sample data needed (BPF reads directly) */
    attr->wakeup_events  = 0;            /* BPF handles events, no perf buffer wakeup */
    attr->exclude_kernel = 1;            /* PGPROC is userspace-only */
    attr->exclude_hv     = 1;            /* no hypervisor writes */
    /* CAP-8: callers that preseed the BPF state_map open the watchpoint
     * disabled, seed, then enable — so the first fire always sees a seeded
     * state and the opening interval is timed correctly. */
    attr->disabled       = start_enabled ? 0 : 1;
}

static int open_watchpoint_common(pid_t pid, uint64_t addr, int bpf_prog_fd,
                                  int start_enabled)
{
    struct perf_event_attr attr;
    pgwt_fill_watchpoint_attr(&attr, addr, start_enabled);

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
