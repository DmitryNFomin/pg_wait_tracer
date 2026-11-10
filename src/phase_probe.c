/* Read the kernel's published uprobe PMU ABI, independently of perf_event_open
 * and bpf_program__attach_usdt. Linux kernel/events/core.c publishes
 * PMU_FORMAT_ATTR(ref_ctr_offset, "config:32-63") in uprobe_format_group.
 * https://github.com/torvalds/linux/blob/v6.8/kernel/events/core.c
 * A hidden/unmounted sysfs is an unknown measurement, never no support. */
#include "phase_capability.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_attr(int dirfd, const char *name, char *buf, size_t cap)
{
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    int saved = errno;
    if (n >= 0) {
        char extra;
        ssize_t tail = read(fd, &extra, 1);
        if (tail != 0) { n = -1; saved = EINVAL; }
    }
    if (close(fd) != 0) { n = -1; saved = errno; }
    if (n <= 0) { errno = n == 0 ? EINVAL : saved; return -1; }
    buf[n] = 0;
    return 0;
}

enum pgwt_phase_fact pgwt_phase_kernel_fact(const char *pmu_dir)
{
    enum pgwt_phase_fact fact = PGWT_PHASE_ERROR;
    char buf[128], *end;
    int pmu = open(pmu_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pmu < 0) return fact;
    if (read_attr(pmu, "type", buf, sizeof(buf)) != 0) goto done;
    /* Verify that sysfs actually exposed a PMU identity before interpreting
     * ENOENT for its optional field. Do not infer anything from uname. */
    if (buf[0] < '0' || buf[0] > '9') goto done;
    errno = 0;
    unsigned long type = strtoul(buf, &end, 10);
    if (errno || type > 0x7fffffffUL || (*end && strcmp(end, "\n"))) goto done;
    int fmt = openat(pmu, "format", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fmt < 0) goto done;
    if (read_attr(fmt, "ref_ctr_offset", buf, sizeof(buf)) == 0) {
        if (!strcmp(buf, "config:32-63\n") || !strcmp(buf, "config:32-63"))
            fact = PGWT_PHASE_PRESENT;
    } else if (errno == ENOENT) {
        fact = PGWT_PHASE_ABSENT;
    }
    if (close(fmt) != 0) fact = PGWT_PHASE_ERROR;
 done:
    if (close(pmu) != 0) fact = PGWT_PHASE_ERROR;
    return fact;
}

void pgwt_phase_measure(const char *pmu_dir, pgwt_phase_note_fn enumerate,
                         void *ctx, struct pgwt_phase_value phase[PGWT_CAP_PHASE_COUNT])
{
    static const char *notes[PGWT_CAP_PHASE_COUNT][2] = {
        {"query__execute__start", "query__execute__done"},
        {"query__plan__start", "query__plan__done"}
    };
    enum pgwt_phase_fact kernel = pgwt_phase_kernel_fact(pmu_dir);
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) {
        int start = enumerate ? enumerate(ctx, notes[p][0]) : -1;
        int done = enumerate ? enumerate(ctx, notes[p][1]) : -1;
        enum pgwt_phase_fact binary = start < 0 || done < 0 ? PGWT_PHASE_ERROR :
            start == 0 || done == 0 ? PGWT_PHASE_ABSENT : PGWT_PHASE_PRESENT;
        phase[p] = pgwt_phase_classify(kernel, binary);
    }
}
