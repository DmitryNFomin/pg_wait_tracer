#include "phase_capability.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", m); } } while (0)
static void put(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f || fputs(s, f) < 0 || fclose(f)) exit(2);
}
static int note_counts[4], note_calls;
static int enumerate(void *ctx, const char *name)
{
    (void)ctx;
    const char *notes[] = {"query__execute__start", "query__execute__done",
                          "query__plan__start", "query__plan__done"};
    for (int i = 0; i < 4; i++) if (!strcmp(notes[i], name)) {
        note_calls++; return note_counts[i];
    }
    return -1;
}
int main(void)
{
    /* Exhaustive injected facts: hardwiring either arm fails on every host.
     * An attach errno, policy or test-forced attach absence is NOT an input. */
    for (int k = -1; k <= 1; k++) for (int b = -1; b <= 1; b++) {
        struct pgwt_phase_value v = pgwt_phase_classify(k, b);
        enum pgwt_phase_state want = k < 0 || b < 0 ? PGWT_PHASE_UNKNOWN :
            k == 0 || b == 0 ? PGWT_PHASE_UNAVAILABLE : PGWT_PHASE_AVAILABLE;
        CHECK(v.state == want, "classification matches injected kernel/binary facts");
        const char *reason = k < 0 ? "kernel_probe_failed" : b < 0 ? "binary_probe_failed" :
            k == 0 ? "kernel_no_ref_ctr" : b == 0 ? "binary_no_sdt" : "none";
        CHECK(!strcmp(v.reason, reason), "classification preserves reason, never turns probe error into absence");
    }
    struct pgwt_phase_value exec = pgwt_phase_classify(1, 1);
    struct pgwt_phase_value plan = pgwt_phase_classify(1, 0);
    CHECK(exec.state == PGWT_PHASE_AVAILABLE && plan.state == PGWT_PHASE_UNAVAILABLE,
          "execute and plan are independently classified");
    char dir[] = "/tmp/pgwt_phase_probe.XXXXXX", type[512], fmt[512], attr[512];
    if (!mkdtemp(dir)) return 2;
    snprintf(type, sizeof(type), "%s/type", dir);
    snprintf(fmt, sizeof(fmt), "%s/format", dir);
    snprintf(attr, sizeof(attr), "%s/format/ref_ctr_offset", dir);
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ERROR, "missing PMU is unknown");
    put(type, "7\n");
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ERROR, "missing format directory is unknown");
    if (mkdir(fmt, 0700)) return 2;
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ABSENT, "readable PMU without attribute proves absence");
    put(attr, "config:32-63\n");
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_PRESENT, "published ref counter ABI proves support");
    struct pgwt_phase_value phases[PGWT_CAP_PHASE_COUNT];
    note_counts[0] = 2; note_counts[1] = 2; note_counts[2] = 1; note_counts[3] = 0;
    pgwt_phase_measure(dir, enumerate, NULL, phases);
    CHECK(note_calls == 4 && phases[0].state == PGWT_PHASE_AVAILABLE &&
          phases[1].state == PGWT_PHASE_UNAVAILABLE && !strcmp(phases[1].reason, "binary_no_sdt"),
          "measurement requires both SDT notes per phase, permits multiple execute sites");
    note_counts[1] = -1; note_counts[3] = 1;
    pgwt_phase_measure(dir, enumerate, NULL, phases);
    CHECK(phases[0].state == PGWT_PHASE_UNKNOWN && phases[1].state == PGWT_PHASE_AVAILABLE,
          "binary enumeration failure stays unknown independently per phase");
    put(attr, "nonsense\n");
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ERROR, "malformed attribute is unknown");
    put(attr, "config:32-63\ntrailing junk");
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ERROR, "attribute must be consumed completely");
    put(attr, "config:32-63\n"); put(type, "no\n");
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ERROR, "unreadable PMU identity is unknown");
    put(type, "7\n"); unlink(attr);
    if (mkdir(attr, 0700)) return 2;
    CHECK(pgwt_phase_kernel_fact(dir) == PGWT_PHASE_ERROR, "attribute read failure is unknown even as root");
    rmdir(attr); unlink(type); rmdir(fmt); rmdir(dir);
    printf("phase_probe: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
