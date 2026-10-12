/* test_alloc_tune.c — src/alloc_tune.h: the allocator property pgwt-server
 * depends on, and the ways this test could be blind to it.
 *
 * The product claim is NOT "mallopt() was called". It is: a second request
 * that allocates a ~274 MB event array reuses pages that are already faulted
 * in, so it pays neither kernel page-zeroing nor a first-touch fault per
 * 4 KB page. That is observable without a stopwatch:
 *
 *   - sbrk(0) moves by at least the allocation size  -> the block came from
 *     the brk heap, not from a fresh mmap;
 *   - sbrk(0) does not move back on free()           -> the heap is not
 *     returned to the OS between requests;
 *   - ru_minflt barely moves when the SECOND allocation is touched in full
 *     -> the pages were still resident. This is the win itself, counted.
 *
 * Every phase is paired with the same sequence run BEFORE the tuning is
 * applied, in the same process. That control is what makes the test able to
 * fail: if the environment already carries MALLOC_MMAP_MAX_ (or the test's
 * allocation is too small to be mmap'd in the first place), the control does
 * not show mmap behaviour, and this test REFUSES rather than reporting a pass
 * it cannot justify.
 *
 * Blindness cases, each reachable from the environment so they can be
 * demonstrated going red:
 *   PGWT_ALLOC_TUNE=0                     the fix is absent  -> FAIL
 *   MALLOC_MMAP_MAX_=0 (or TRIM/TOP_PAD)  pre-tuned env      -> REFUSE
 *   PGWT_TEST_ALLOC_TUNE_SIZE_MB=1        alloc below the mmap threshold,
 *                                         so both phases are heap-served
 *                                         and the check is vacuous -> REFUSE
 *   PGWT_TEST_ALLOC_TUNE_SBRK_FAIL=1      the observation primitive is
 *                                         unavailable          -> REFUSE
 * and an LD_PRELOAD stub whose mallopt() always fails must also come back
 * FAIL, not pass, because the bitmask is compared against the full set.
 */
#include "alloc_tune.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __GLIBC__
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* A gate that cannot see must refuse, never approve. */
#define REFUSE(fmt, ...) do { \
    printf("  REFUSE: " fmt "\n", ##__VA_ARGS__); \
    printf("test_alloc_tune: cannot observe its own subject -- refusing\n"); \
    exit(2); \
} while(0)

#ifdef __GLIBC__

/* Current program break, or NULL if it cannot be read. NULL is never treated
 * as "no growth": every caller refuses instead. */
static char *brk_now(void)
{
    if (getenv("PGWT_TEST_ALLOC_TUNE_SBRK_FAIL"))
        return NULL;            /* injected blindness */
    void *p = sbrk(0);
    if (p == (void *)-1)
        return NULL;
    return (char *)p;
}

static long minflt_now(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return -1;
    return (long)ru.ru_minflt;
}

/* Touch every page so the kernel must have backed all of them. */
static void touch_all(volatile char *p, size_t n)
{
    for (size_t off = 0; off < n; off += 4096)
        p[off] = (char)(off & 0x7f);
}

struct seq_result {
    ptrdiff_t brk_delta_first;   /* heap growth caused by allocation 1 */
    ptrdiff_t brk_delta_free;    /* heap movement caused by free()      */
    ptrdiff_t brk_delta_second;  /* heap growth caused by allocation 2  */
    long      minflt_first;      /* faults touching allocation 1        */
    long      minflt_second;     /* faults touching allocation 2        */
    int       alloc_failed;
};

/* malloc+touch+free+malloc+touch, instrumented. This is one request pair of
 * the server's shared path in miniature. */
static void run_seq(size_t sz, struct seq_result *r)
{
    memset(r, 0, sizeof(*r));

    char *b0 = brk_now();
    if (!b0) REFUSE("sbrk(0) unavailable: cannot tell a heap allocation from an mmap");

    char *p = malloc(sz);
    if (!p) { r->alloc_failed = 1; return; }
    char *b1 = brk_now();
    if (!b1) REFUSE("sbrk(0) unavailable after the first allocation");
    r->brk_delta_first = b1 - b0;

    long f0 = minflt_now();
    touch_all(p, sz);
    long f1 = minflt_now();
    if (f0 < 0 || f1 < 0) REFUSE("getrusage(RUSAGE_SELF) unavailable: cannot count page faults");
    r->minflt_first = f1 - f0;

    free(p);
    char *b2 = brk_now();
    if (!b2) REFUSE("sbrk(0) unavailable after free()");
    r->brk_delta_free = b2 - b1;

    char *q = malloc(sz);
    if (!q) { r->alloc_failed = 1; return; }
    char *b3 = brk_now();
    if (!b3) REFUSE("sbrk(0) unavailable after the second allocation");
    r->brk_delta_second = b3 - b2;

    long f2 = minflt_now();
    touch_all(q, sz);
    long f3 = minflt_now();
    if (f2 < 0 || f3 < 0) REFUSE("getrusage(RUSAGE_SELF) unavailable on the second allocation");
    r->minflt_second = f3 - f2;

    free(q);
}

static void print_seq(const char *tag, const struct seq_result *r, size_t sz)
{
    printf("  %-9s alloc1 brk+%.1f MB faults=%ld | free brk%+.1f MB | "
           "alloc2 brk+%.1f MB faults=%ld  (size %.0f MB)\n",
           tag, r->brk_delta_first / 1048576.0, r->minflt_first,
           r->brk_delta_free / 1048576.0,
           r->brk_delta_second / 1048576.0, r->minflt_second,
           sz / 1048576.0);
}

/* The off switch, checked in a child forked BEFORE the parent tunes anything:
 * mallopt() cannot be undone, so an in-process check after the fact would be
 * measuring the tuned allocator and would pass for the wrong reason. */
static int child_off_switch(size_t sz)
{
    setenv("PGWT_ALLOC_TUNE", "0", 1);
    if (pgwt_tune_allocator() != PGWT_ALLOC_TUNE_NONE) {
        printf("  FAIL: PGWT_ALLOC_TUNE=0 still applied tuning\n");
        return 1;
    }
    struct seq_result r;
    run_seq(sz, &r);
    if (r.alloc_failed) {
        printf("  FAIL: off-switch child could not allocate %.0f MB\n", sz / 1048576.0);
        return 1;
    }
    if (r.brk_delta_first >= (ptrdiff_t)sz) {
        printf("  FAIL: PGWT_ALLOC_TUNE=0 but the allocation still came from the heap "
               "(brk +%.1f MB)\n", r.brk_delta_first / 1048576.0);
        return 1;
    }
    return 0;
}

int main(void)
{
    size_t sz = 128u * 1024u * 1024u;
    const char *szenv = getenv("PGWT_TEST_ALLOC_TUNE_SIZE_MB");
    if (szenv) {
        long mb = strtol(szenv, NULL, 10);
        if (mb <= 0) REFUSE("PGWT_TEST_ALLOC_TUNE_SIZE_MB=%s is not a positive size", szenv);
        sz = (size_t)mb * 1024u * 1024u;
    }

    printf("=== test_alloc_tune (glibc) ===\n");

    /* Refusal 1: an inherited MALLOC_* tunable would make the tuned phase
     * pass with no code at all, because the allocator is already configured.
     * This is the false negative the whole test exists to avoid. */
    const char *pre[] = { "MALLOC_MMAP_MAX_", "MALLOC_TRIM_THRESHOLD_",
                          "MALLOC_TOP_PAD_", "GLIBC_TUNABLES" };
    for (size_t i = 0; i < sizeof(pre) / sizeof(pre[0]); i++)
        if (getenv(pre[i]))
            REFUSE("%s=%s is set in the environment: the allocator is already "
                   "tuned, so this test cannot attribute anything to the code "
                   "under test", pre[i], getenv(pre[i]));

    /* -- Phase A: control, untuned. ----------------------------------- */
    printf("--- untuned control ---\n");
    struct seq_result a;
    run_seq(sz, &a);
    if (a.alloc_failed)
        REFUSE("could not allocate %.0f MB twice: no control to compare against",
               sz / 1048576.0);
    print_seq("untuned", &a, sz);

    /* Refusal 2: if the untuned allocation did NOT come from mmap, either the
     * size is below the mmap threshold or something else already changed the
     * allocator. Either way the tuned phase proves nothing. */
    if (a.brk_delta_first >= (ptrdiff_t)sz)
        REFUSE("an untuned %.0f MB allocation already grew the heap by %.1f MB; "
               "nothing distinguishes the tuned phase from this one "
               "(size below the mmap threshold, or a pre-tuned allocator)",
               sz / 1048576.0, a.brk_delta_first / 1048576.0);
    if (a.minflt_second <= 0)
        REFUSE("the untuned second allocation faulted %ld pages: with no faults "
               "to remove, a fault-count comparison cannot go red",
               a.minflt_second);

    /* The control's own shape: fresh mmap every time means the second touch
     * faults just as much as the first. */
    CHECK(a.minflt_second > a.minflt_first / 2,
          "untuned: second touch faulted %ld pages vs %ld for the first -- "
          "the control is not showing fresh pages per allocation",
          a.minflt_second, a.minflt_first);

    /* -- Phase C: the off switch, in a child that has not been tuned. -- */
    printf("--- off switch (PGWT_ALLOC_TUNE=0, forked child) ---\n");
    pid_t pid = fork();
    if (pid < 0) REFUSE("fork() failed: cannot check the off switch untainted");
    if (pid == 0)
        _exit(child_off_switch(sz));
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        REFUSE("waitpid() failed: the off-switch result is unknown");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "off-switch child exited %d (signalled=%d): PGWT_ALLOC_TUNE=0 did not "
          "leave the allocator alone",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1, !WIFEXITED(status));

    /* -- Phase B: tuned. ---------------------------------------------- */
    printf("--- tuned ---\n");
    int applied = pgwt_tune_allocator();
    CHECK(applied == PGWT_ALLOC_TUNE_ALL,
          "pgwt_tune_allocator() applied 0x%x, expected 0x%x (a partial result "
          "means one mallopt() was rejected)",
          applied, PGWT_ALLOC_TUNE_ALL);

    struct seq_result b;
    run_seq(sz, &b);
    if (b.alloc_failed)
        REFUSE("tuned: could not allocate %.0f MB from the heap", sz / 1048576.0);
    print_seq("tuned", &b, sz);

    /* 1. The block is on the brk heap now, not mmap'd. */
    CHECK(b.brk_delta_first >= (ptrdiff_t)sz,
          "tuned: a %.0f MB allocation grew the heap by only %.1f MB -- still mmap'd",
          sz / 1048576.0, b.brk_delta_first / 1048576.0);

    /* 2. free() does not hand it back, so the pages stay faulted. */
    CHECK(b.brk_delta_free == 0,
          "tuned: free() moved the program break by %.1f MB -- the heap is being "
          "trimmed between requests, which is exactly what M_TRIM_THRESHOLD "
          "is supposed to prevent",
          b.brk_delta_free / 1048576.0);

    /* 3. The second allocation needs no new address space. */
    CHECK(b.brk_delta_second == 0,
          "tuned: the second allocation grew the heap by a further %.1f MB -- "
          "the first one's pages were not reused",
          b.brk_delta_second / 1048576.0);

    /* 4. The win, counted: touching the second allocation in full costs
     *    almost no minor faults, where untuned it costs one per page. */
    CHECK(b.minflt_second * 4 < a.minflt_second,
          "tuned: the second touch still faulted %ld pages (untuned: %ld) -- "
          "no page reuse, so none of the measured time saving can come from here",
          b.minflt_second, a.minflt_second);

    printf("  faults avoided on the second allocation: %ld -> %ld (%.1fx)\n",
           a.minflt_second, b.minflt_second,
           b.minflt_second > 0 ? (double)a.minflt_second / b.minflt_second
                               : (double)a.minflt_second);

    printf("\n=== %d/%d checks passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

#else  /* !__GLIBC__ */

int main(void)
{
    printf("=== test_alloc_tune (not glibc) ===\n");
    /* Not a skip: on a non-glibc libc the product change must be an explicit,
     * reported no-op rather than a silent one, and that IS checkable here. */
    int applied = pgwt_tune_allocator();
    CHECK(applied == PGWT_ALLOC_TUNE_NONE,
          "pgwt_tune_allocator() returned 0x%x on a non-glibc build; "
          "mallopt()/M_MMAP_MAX do not exist here", applied);
    printf("  non-glibc: tuning is a declared no-op (the allocator property "
           "itself is only observable on glibc)\n");
    printf("\n=== %d/%d checks passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

#endif
