/* test_watchpoint_throttle.c — #390: a hardware watchpoint's own perf_event
 * has no ring buffer (SET_BPF) and nothing ever observes
 * PERF_RECORD_THROTTLE/UNTHROTTLE (src/perf_event.c). The first draft of
 * this issue's fix proposed PERF_FORMAT_TOTAL_TIME_ENABLED|_RUNNING as a
 * ring-buffer-free substitute signal: a cumulative time_running shortfall
 * against time_enabled, the theory went, IS the throttle, quantified.
 *
 * THAT THEORY IS WRONG, and this test is the proof, not the thing it was
 * originally written to prove.
 *
 * TOTAL_TIME_ENABLED/_RUNNING track PMU MULTIPLEXING (whether the kernel
 * had to time-share a scarce hardware counter across competing events) —
 * a real but DIFFERENT mechanism from the sample-rate throttle this issue
 * is about. Measured directly on a gate box (gate-1, kernel 6.8.0-139):
 * with /proc/sys/kernel/perf_event_max_sample_rate lowered to 1 and up to
 * 2,000,000 forced hits on a self-watched hardware breakpoint, this test's
 * own time_enabled/time_running read back BIT-IDENTICAL every single time
 * (gap always exactly 0) — while, independently and under the IDENTICAL
 * sysctl, `perf record -e cycles -c 1 -- <same workload>` reported 513
 * THROTTLE and 512 UNTHROTTLE events via `perf report --stats`: the kernel
 * genuinely throttled, repeatedly, and TOTAL_TIME_ENABLED/_RUNNING on a
 * directly-read fd never moved by a single nanosecond. perf's THROTTLE
 * records reached userspace only through its OWN ring buffer — the exact
 * thing SET_BPF (src/perf_event.c) does not have.
 *
 * So the finding this test pins is the NEGATIVE one: no ring buffer, and
 * no substitute field either. A counter built on this read-format would
 * read "0 throttled" while throttling was actively happening — a false
 * healthy signal, worse than no counter — which is why #390's fix does NOT
 * use this field in the shipped daemon. This test exists so nobody
 * re-discovers that the hard way by re-plumbing it.
 *
 * Needs CAP_SYS_ADMIN (hardware breakpoints) and write access to the
 * sysctl. SKIPs (prints SKIP, exits 0, no failure counted) when either is
 * unavailable — an environment gap, not a defect in the code under test.
 *
 * The sysctl is GLOBAL and machine-wide. Every path that successfully
 * lowers it restores it before returning, including every failure path
 * after that point, and the restore's own success is itself asserted
 * (CHECK), never swallowed — a test that left a box's perf sysctl lowered
 * would silently degrade every later capture on that machine. */

#define _GNU_SOURCE
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
    attr.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED |
                          PERF_FORMAT_TOTAL_TIME_RUNNING;

    perf_fd = sys_perf_event_open(&attr, 0, -1, -1, 0);
    if (perf_fd < 0) {
        printf("SKIP: perf_event_open(PERF_TYPE_BREAKPOINT) failed (%s) — "
               "needs CAP_SYS_ADMIN or a permissive perf_event_paranoid\n",
               strerror(errno));
        goto restore;
    }

    /* Fire it hard: sample_period=1 means every write to `watched` is a
     * kernel overflow interrupt, and at a 1/s system-wide cap this is
     * exactly the condition that made `perf record` throttle for real in
     * the development measurement cited above. 2,000,000 iterations is the
     * larger of the two counts that measurement used, kept here so this
     * test alone reproduces the no-gap result without relying on the
     * `perf record` cross-check being rerun. */
    for (int i = 0; i < 2000000; i++) {
        watched = i;
        __asm__ volatile("" ::: "memory"); /* the store must not be elided */
    }

    {
        uint64_t buf[3] = {0, 0, 0}; /* value, time_enabled, time_running */
        ssize_t n = read(perf_fd, buf, sizeof(buf));
        CHECK(n == (ssize_t)sizeof(buf), "read(2) on the perf_event fd returns the full triple");
        uint64_t te = buf[1], tr = buf[2];
        CHECK(te > 0, "time_enabled advanced (the event really was enabled)");
        CHECK(buf[0] >= 1000000,
              "the breakpoint actually fired at least 1,000,000 times "
              "(this is not a no-op loop the compiler elided)");
        /* THE FINDING: not a gap, EQUALITY — under conditions independently
         * confirmed to throttle a perf event for real (see header comment),
         * this field shows none of it. A production counter built on this
         * field would read "0 throttled" right now, while — by the
         * perf-record cross-check — the kernel may well be throttling. */
        CHECK(tr == te,
              "TOTAL_TIME_ENABLED/_RUNNING show NO gap under a forced "
              "sample-rate throttle (the negative finding this test "
              "exists to pin — see the file header)");
        fprintf(stderr,
                "INFO: value=%llu time_enabled=%llu time_running=%llu gap=%lld "
                "(expected gap: 0 — this field does not see rate-limit throttle)\n",
                (unsigned long long)buf[0], (unsigned long long)te,
                (unsigned long long)tr, (long long)(te - tr));
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
