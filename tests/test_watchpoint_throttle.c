/* test_watchpoint_throttle.c — #390: a hardware watchpoint's own perf_event
 * has no ring buffer (SET_BPF) and nothing ever observes
 * PERF_RECORD_THROTTLE/UNTHROTTLE (src/perf_event.c). The kernel's
 * rate-limit response — the event silently stops firing for part of a
 * window — is otherwise completely invisible to the daemon.
 *
 * This test induces a REAL kernel throttle deterministically: lower
 * /proc/sys/kernel/perf_event_max_sample_rate to a value this process's own
 * watchpoint fire rate will exceed almost immediately, then assert
 * pgwt_watchpoint_read_times() (src/perf_event.c) reports time_running <
 * time_enabled — the signal #390 asked for.
 *
 * It does NOT go through pgwt_open_watchpoint()/pgwt_open_watchpoint_disabled():
 * those require a loaded BPF program for PERF_EVENT_IOC_SET_BPF, and the
 * kernel's throttle check runs in the overflow path BEFORE the BPF handler
 * is invoked regardless of whether one is attached (that is the architectural
 * finding behind #390 — see src/perf_event.c's open_watchpoint_common
 * comment). So a plain PERF_TYPE_BREAKPOINT event on this process's own
 * watched variable, with the identical attr fields, exercises the same
 * throttle mechanism without needing a BPF skeleton loaded here.
 *
 * Needs CAP_SYS_ADMIN (hardware breakpoints) and write access to the sysctl.
 * SKIPs (prints SKIP, exits 0, no failure counted) when either is
 * unavailable — box-check's root gate box has both; a weaker/unprivileged
 * environment legitimately cannot run this, which is an environment gap,
 * not a defect in the code under test.
 *
 * The sysctl is GLOBAL and machine-wide. Every path that successfully
 * lowers it restores it before returning, including every failure path
 * after that point — a test that left a box's perf sysctl lowered would
 * silently degrade every later capture on that machine, which the task
 * this test was written for named explicitly as a worse defect than the
 * one being fixed. The restore's own success is itself asserted (CHECK),
 * never swallowed. */

#define _GNU_SOURCE
#include "perf_event.h"

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

static int checks, failures;
#define CHECK(x, msg) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

#define SYSCTL_PATH "/proc/sys/kernel/perf_event_max_sample_rate"

static long sys_perf_event_open(struct perf_event_attr *attr, pid_t pid,
                                int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* Fresh open/read/close and open/write/close each time — proc sysctl files
 * are single-value and do not need (or always tolerate) a held-open fd
 * across reads and writes. Returns 0 on success. */
static int read_sysctl(char *buf, size_t buf_sz)
{
    int fd = open(SYSCTL_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, buf_sz - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    for (char *p = buf; *p; p++)
        if (*p == '\n') { *p = 0; break; }
    return 0;
}

static int write_sysctl(const char *val)
{
    int fd = open(SYSCTL_PATH, O_WRONLY);
    if (fd < 0)
        return -1;
    size_t len = strlen(val);
    ssize_t n = write(fd, val, len);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return (n == (ssize_t)len) ? 0 : -1;
}

static volatile int watched = 0; /* the hardware write-watchpoint's target */

int main(void)
{
    char orig[64];
    if (read_sysctl(orig, sizeof(orig)) != 0) {
        printf("SKIP: cannot read %s (%s) — needs root\n",
               SYSCTL_PATH, strerror(errno));
        printf("%d checks, %d failures\n", checks, failures);
        return 0;
    }

    if (write_sysctl("1") != 0) {
        printf("SKIP: cannot write %s (%s) — needs root\n",
               SYSCTL_PATH, strerror(errno));
        printf("%d checks, %d failures\n", checks, failures);
        return 0;
    }
    fprintf(stderr, "INFO: lowered %s from '%s' to '1'\n", SYSCTL_PATH, orig);

    /* From here on, the sysctl IS mutated — every exit restores it. */
    int perf_fd = -1;

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type           = PERF_TYPE_BREAKPOINT;
    attr.size           = sizeof(attr);
    attr.bp_type        = HW_BREAKPOINT_W;
    attr.bp_addr        = (uint64_t)(uintptr_t)&watched;
    attr.bp_len         = HW_BREAKPOINT_LEN_4;
    attr.sample_period  = 1;
    attr.sample_type    = 0;
    attr.wakeup_events  = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv     = 1;
    attr.disabled       = 0;
    /* Same read_format open_watchpoint_common now requests unconditionally
     * (src/perf_event.c) — this test exercises that exact field, not a
     * reimplementation of it. */
    attr.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED |
                          PERF_FORMAT_TOTAL_TIME_RUNNING;

    perf_fd = sys_perf_event_open(&attr, 0, -1, -1, 0);
    if (perf_fd < 0) {
        printf("SKIP: perf_event_open(PERF_TYPE_BREAKPOINT) failed (%s) — "
               "needs CAP_SYS_ADMIN or a permissive perf_event_paranoid\n",
               strerror(errno));
        goto restore;
    }

    /* Fire it: sample_period=1 means every write to `watched` is a kernel
     * overflow interrupt. At a 1/s system-wide cap this throttles within
     * the first handful of the loop below — no timing/sleep dependency,
     * the loop itself is what exceeds the rate. */
    for (int i = 0; i < 200000; i++) {
        watched = i;
        __asm__ volatile("" ::: "memory"); /* the store must not be elided */
    }

    {
        uint64_t te = 0, tr = 0;
        int rc = pgwt_watchpoint_read_times(perf_fd, &te, &tr);
        CHECK(rc == 0, "pgwt_watchpoint_read_times succeeds on a real fd");
        CHECK(te > 0, "time_enabled advanced (the event really was enabled)");
        CHECK(tr < te, "REAL kernel throttle: time_running fell short of "
                        "time_enabled after lowering "
                        "perf_event_max_sample_rate to 1 — this is the #390 "
                        "signal, measured, not asserted");
        fprintf(stderr,
                "INFO: time_enabled=%llu time_running=%llu gap_ns=%llu\n",
                (unsigned long long)te, (unsigned long long)tr,
                (unsigned long long)(te > tr ? te - tr : 0));
    }

restore:
    if (perf_fd >= 0)
        close(perf_fd);

    /* Unconditional restore — reached from every path past the successful
     * write_sysctl("1") above, pass or fail. Its own outcome is asserted,
     * not swallowed: a restore that silently failed would leave the box
     * degraded with the log saying nothing about it. */
    int restore_rc = write_sysctl(orig);
    CHECK(restore_rc == 0, "sysctl restored to its original value");
    if (restore_rc == 0)
        fprintf(stderr, "INFO: restored %s to '%s'\n", SYSCTL_PATH, orig);
    else
        fprintf(stderr,
                "FAIL: could not restore %s — manual fix needed: "
                "echo %s > %s (error: %s)\n",
                SYSCTL_PATH, orig, SYSCTL_PATH, strerror(errno));

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
