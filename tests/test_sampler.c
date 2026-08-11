/* test_sampler.c — Unit tests for the sampled provider's core (A2, T4)
 *
 * Exercises the BPF-free sampler core (pgwt_sampler_read_targets and
 * pgwt_sampler_build_batch) against a controlled target: this process's own
 * memory (read via process_vm_readv on getpid()) and child processes with
 * known 4-byte values at known addresses. Built with -DPGWT_SERVER against
 * the server objects so it needs no BPF skeleton (buildable without bpftool,
 * and in CI), matching test_trace_v2.
 *
 * T4 additions:
 *   - SMP-2: a process-LOCAL address (same VA in a forked child, private
 *     pages) must be read per-pid, never batched through another pid — the
 *     batched read SUCCEEDS with the wrong (reader's) value.
 *   - CAP-2/5 backstop: garbage class-byte readings are dropped + counted.
 *   - SMP-1: the read-health state machine (loud on first + persistent
 *     failure, recovery).
 *   - SMP-3: effective sample period compensates missed/late ticks.
 *   - SMP-4: the pid->query_id join index.
 */
#define _GNU_SOURCE
#include "sampler.h"
#include "anomaly.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* ── Test 1: read this process's own memory via process_vm_readv ──────── */

static void test_self_read(void)
{
    printf("--- self-read via process_vm_readv ---\n");

    /* Three known values; one is 0 (on-CPU) to confirm reads, not just
     * encoding, handle it. Marked is_shared so the batch path is used
     * (reading one's own memory through one's own pid is trivially sound). */
    volatile uint32_t v0 = WEI(PG_WAIT_IO, 0x12);     /* IO:something */
    volatile uint32_t v1 = 0;                          /* on CPU */
    volatile uint32_t v2 = WEI(PG_WAIT_LOCK, 0x03);   /* Lock:something */

    struct pgwt_sample_target targets[3] = {
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&v0, .query_id = 111, .is_shared = 1 },
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&v1, .query_id = 222, .is_shared = 1 },
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&v2, .query_id = 333, .is_shared = 1 },
    };

    uint32_t vals[3] = { 0xdead, 0xdead, 0xdead };
    uint8_t valid[3] = { 0, 0, 0 };
    uint64_t faults = 0;
    int got = pgwt_sampler_read_targets(targets, 3, vals, valid, &faults);

    CHECK(got == 3, "expected 3 reads, got %d", got);
    CHECK(vals[0] == v0, "vals[0]=0x%x expected 0x%x", vals[0], v0);
    CHECK(vals[1] == 0,  "vals[1]=0x%x expected 0", vals[1]);
    CHECK(vals[2] == v2, "vals[2]=0x%x expected 0x%x", vals[2], v2);
    CHECK(valid[0] && valid[1] && valid[2],
          "all three successful reads are marked valid");
    CHECK(faults == 0, "expected no fallback faults, got %llu",
          (unsigned long long)faults);
}

/* ── Test 2: build_batch encodes fields; gated on-CPU skip is counted ──── */

static void test_build_batch(void)
{
    printf("--- build_batch encoding + gated on-CPU skip ---\n");

    struct pgwt_sample_target targets[3] = {
        { .pid = 1001, .wait_event_addr = 0x1000, .query_id = 111,
          .backend_type = PGWT_BT_CLIENT },
        /* client, command NOT open: its we==0 reading is between-command
         * churn — not recorded, but counted (T2 decision). */
        { .pid = 1002, .wait_event_addr = 0x2000, .query_id = 222,
          .backend_type = PGWT_BT_CLIENT, .cmd_open = 0 },
        { .pid = 1003, .wait_event_addr = 0x3000, .query_id = 333,
          .backend_type = PGWT_BT_CLIENT },
    };
    uint32_t vals[3] = {
        WEI(PG_WAIT_IO, 0x01),  /* recorded */
        0,                       /* on CPU, no command — skipped + counted */
        WEI(PG_WAIT_LWLOCK, 0x07),
    };
    uint8_t valid[3] = { 1, 1, 1 };

    struct pgwt_trace_event out[3];
    memset(out, 0xAA, sizeof(out));
    uint64_t ts = 0x123456789ABCULL;
    uint64_t noncmd = 0;
    int n = pgwt_sampler_build_batch(targets, vals, valid, 3, ts, out,
                                     NULL, &noncmd);

    CHECK(n == 2, "expected 2 records (1 non-command on-CPU skipped), got %d", n);
    CHECK(noncmd == 1, "expected 1 non-command CPU skip counted, got %llu",
          (unsigned long long)noncmd);

    /* Record 0 = target 0 */
    CHECK(out[0].timestamp_ns == ts, "ts mismatch");
    CHECK(out[0].pid == 1001, "pid=%u expected 1001", out[0].pid);
    CHECK(out[0].new_event == vals[0], "new_event=0x%x expected 0x%x",
          out[0].new_event, vals[0]);
    CHECK(out[0].old_event == 0, "old_event must be 0 for samples");
    CHECK(out[0].duration_ns == 0, "duration must be 0 for samples");
    CHECK(out[0].query_id == 111, "query_id=%llu expected 111",
          (unsigned long long)out[0].query_id);
    CHECK(out[0].flags == 0, "client sample carries no category flag");

    /* Record 1 = target 2 (target 1 was gated on-CPU) */
    CHECK(out[1].pid == 1003, "pid=%u expected 1003", out[1].pid);
    CHECK(out[1].new_event == vals[2], "new_event=0x%x expected 0x%x",
          out[1].new_event, vals[2]);
    CHECK(out[1].query_id == 333, "query_id=%llu expected 333",
          (unsigned long long)out[1].query_id);
}

/* ── Test 2c: the T2 on-CPU policy (docs/AAS_SEMANTICS_DECISION.md) ────── */

static void test_build_batch_cpu_policy(void)
{
    printf("--- build_batch on-CPU policy (T2 decomposed AAS) ---\n");

    struct pgwt_sample_target targets[6] = {
        /* client INSIDE a command: we==0 is a first-class CPU sample */
        { .pid = 1, .query_id = 42, .backend_type = PGWT_BT_CLIENT,
          .cmd_open = 1 },
        /* client outside a command: skipped (counted) */
        { .pid = 2, .backend_type = PGWT_BT_CLIENT, .cmd_open = 0 },
        /* background types: we==0 always records (their idle states are
         * instrumented Activity waits), each with its category flag */
        { .pid = 3, .backend_type = PGWT_BT_CHECKPOINTER },
        { .pid = 4, .backend_type = PGWT_BT_AUTOVAC_WORKER },
        { .pid = 5, .backend_type = PGWT_BT_IO_WORKER },
        /* parallel workers exist only inside a query: always CPU-recordable,
         * foreground (no flag) */
        { .pid = 6, .backend_type = PGWT_BT_PARALLEL_WORKER },
    };
    uint32_t vals[6] = { 0, 0, 0, 0, 0, 0 };
    uint8_t valid[6] = { 1, 1, 1, 1, 1, 1 };

    struct pgwt_trace_event out[6];
    uint64_t noncmd = 0;
    int n = pgwt_sampler_build_batch(targets, vals, valid, 6, 7, out,
                                     NULL, &noncmd);

    CHECK(n == 5, "expected 5 CPU records (1 gated out), got %d", n);
    CHECK(noncmd == 1, "expected 1 gated skip, got %llu",
          (unsigned long long)noncmd);

    CHECK(out[0].pid == 1 && out[0].new_event == 0,
          "in-command client CPU sample recorded as event 0");
    CHECK(out[0].flags == 0, "client CPU sample is foreground (no flag)");
    CHECK(out[0].query_id == 42, "CPU sample keeps its query_id");

    CHECK(out[1].pid == 3 && out[1].flags == PGWT_EVENT_FLAG_BACKGROUND,
          "checkpointer CPU sample flagged BACKGROUND");
    CHECK(out[2].pid == 4 && out[2].flags == PGWT_EVENT_FLAG_MAINT,
          "autovacuum worker CPU sample flagged MAINT");
    CHECK(out[3].pid == 5 && out[3].flags == PGWT_EVENT_FLAG_IO_WORKER,
          "io_worker CPU sample flagged IO_WORKER");
    CHECK(out[4].pid == 6 && out[4].flags == 0,
          "parallel worker CPU sample is foreground (no flag)");

    /* Waits carry the category flag too (an io_worker's DataFileRead must
     * be excludable from AAS by every consumer). */
    uint32_t wvals[6] = { WEI(PG_WAIT_IO, 1), WEI(PG_WAIT_IO, 1),
                          WEI(PG_WAIT_IO, 1), WEI(PG_WAIT_IO, 1),
                          WEI(PG_WAIT_IO, 1), WEI(PG_WAIT_IO, 1) };
    n = pgwt_sampler_build_batch(targets, wvals, valid, 6, 8, out,
                                 NULL, NULL);
    CHECK(n == 6, "all wait readings recorded, got %d", n);
    CHECK(out[4].flags == PGWT_EVENT_FLAG_IO_WORKER,
          "io_worker WAIT sample flagged IO_WORKER");
    CHECK(out[1].flags == 0,
          "client WAIT sample recorded even with command closed");

    /* UNKNOWN type is conservative: gated like a client. */
    struct pgwt_sample_target unk = { .pid = 9,
                                      .backend_type = PGWT_BT_UNKNOWN };
    uint32_t zero = 0;
    uint8_t valid_one = 1;
    n = pgwt_sampler_build_batch(&unk, &zero, &valid_one, 1, 9, out,
                                 NULL, NULL);
    CHECK(n == 0, "UNKNOWN type we==0 is gated (command closed)");
    unk.cmd_open = 1;
    n = pgwt_sampler_build_batch(&unk, &zero, &valid_one, 1, 9, out,
                                 NULL, NULL);
    CHECK(n == 1, "UNKNOWN type we==0 records when command open");
}

/* ── Test 2b: build_batch drops + counts garbage readings (CAP-2/5) ───── */

static void test_build_batch_garbage(void)
{
    printf("--- build_batch garbage class-byte filter (CAP-2/5) ---\n");

    struct pgwt_sample_target targets[3] = {
        { .pid = 2001, .wait_event_addr = 0x1000, .query_id = 1 },
        { .pid = 2002, .wait_event_addr = 0x2000, .query_id = 2 },
        { .pid = 2003, .wait_event_addr = 0x3000, .query_id = 3 },
    };
    uint32_t vals[3] = {
        WEI(PG_WAIT_LOCK, 0x00),  /* valid — recorded */
        0xDEADBEEFu,              /* garbage class 0xDE — dropped + counted */
        0x7F000001u,              /* garbage class 0x7F — dropped + counted */
    };
    uint8_t valid[3] = { 1, 1, 1 };

    struct pgwt_trace_event out[3];
    uint64_t invalid = 0;
    int n = pgwt_sampler_build_batch(targets, vals, valid, 3, 1, out,
                                     &invalid, NULL);

    CHECK(n == 1, "expected 1 record (2 garbage dropped), got %d", n);
    CHECK(out[0].pid == 2001, "the valid record survived");
    CHECK(invalid == 2, "expected 2 invalid reads counted, got %llu",
          (unsigned long long)invalid);

    /* NULL counter must not crash and must still drop garbage. */
    n = pgwt_sampler_build_batch(targets, vals, valid, 3, 1, out, NULL, NULL);
    CHECK(n == 1, "NULL invalid counter: still 1 record, got %d", n);
}

/* ── Test 3: per-pid pread fallback on a bad leading entry ────────────── */

static void test_fallback(void)
{
    printf("--- per-pid pread fallback ---\n");

    /* A valid value we want recovered. */
    volatile uint32_t good = WEI(PG_WAIT_CLIENT, 0x00);

    /* Target 0 points at an unmapped address: process_vm_readv faults at the
     * first iovec, returning a partial (0 bytes) result, so the whole batch
     * falls to the per-pid pread path. Target 1 is a valid self address and
     * must still be recovered by pread(/proc/self/mem). */
    void *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(bad != MAP_FAILED, "mmap PROT_NONE failed");
    munmap(bad, 4096);   /* now definitely unmapped */

    struct pgwt_sample_target targets[2] = {
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)bad, .query_id = 1, .is_shared = 1 },
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&good, .query_id = 2, .is_shared = 1 },
    };

    uint32_t vals[2] = { 0xdead, 0xdead };
    uint8_t valid[2] = { 1, 1 };
    uint64_t faults = 0;
    int got = pgwt_sampler_read_targets(targets, 2, vals, valid, &faults);

    /* The good entry must be recovered via pread even though entry 0 faulted. */
    CHECK(vals[1] == good, "fallback vals[1]=0x%x expected 0x%x", vals[1], good);
    CHECK(valid[0] == 0, "faulting entry is marked invalid");
    CHECK(valid[1] == 1, "recovered entry is marked valid");
    CHECK(got >= 1, "expected at least the good entry recovered, got %d", got);
    CHECK(faults >= 1, "expected >=1 fallback fault recorded, got %llu",
          (unsigned long long)faults);
}

/* ── AAS-1 Stage 1: read validity, coverage, and classification ───────── */

static void test_read_validity_excludes_failures(void)
{
    printf("--- AAS-1: failed reads cannot fabricate CPU samples ---\n");

    volatile uint32_t cpu = 0;
    volatile uint32_t wait = WEI(PG_WAIT_IO, 0x33);
    void *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    CHECK(bad != MAP_FAILED, "mmap PROT_NONE failed");
    if (bad == MAP_FAILED)
        return;
    munmap(bad, 4096);

    struct pgwt_sample_target targets[4] = {
        /* Failed shared-batch read. If its invalid zero reaches build_batch,
         * this command-open client is incorrectly fabricated as CPU. */
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)bad,
          .query_id = 10, .is_shared = 1, .backend_type = PGWT_BT_CLIENT,
          .cmd_open = 1 },
        /* A real successful zero must remain a CPU sample. */
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&cpu,
          .query_id = 20, .is_shared = 1, .backend_type = PGWT_BT_CLIENT,
          .cmd_open = 1 },
        /* A successful wait must retain its normal classification. */
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&wait,
          .query_id = 30, .is_shared = 1, .backend_type = PGWT_BT_CLIENT },
        /* No address on the per-pid path is independently invalid. */
        { .pid = getpid(), .wait_event_addr = 0, .query_id = 40,
          .is_shared = 0, .backend_type = PGWT_BT_CLIENT, .cmd_open = 1 },
    };
    uint32_t vals[4] = { 0xdead, 0xdead, 0xdead, 0xdead };
    uint8_t valid[4] = { 1, 1, 1, 1 };
    uint64_t faults = 0;

    int got = pgwt_sampler_read_targets(targets, 4, vals, valid, &faults);
    CHECK(got == 2, "mixed read got %d valid targets, expected 2", got);
    CHECK(valid[0] == 0 && valid[1] == 1 && valid[2] == 1
              && valid[3] == 0,
          "validity bitmap distinguishes failures from successful zero/wait");
    CHECK(vals[0] == 0 && vals[1] == 0 && vals[2] == wait && vals[3] == 0,
          "failed reads stay zero but are distinguishable by validity");

    struct pgwt_sampler sampler;
    memset(&sampler, 0, sizeof(sampler));
    pgwt_sampler_note_coverage(&sampler, 4, got);
    CHECK(sampler.read_targets_last == 4, "coverage targets=4");
    CHECK(sampler.read_valid_last == 2, "coverage valid=2");
    CHECK(sampler.read_invalid_last == 2, "coverage invalid=2");
    CHECK(sampler.read_failures_total == 2,
          "cumulative read failures=2 after one mixed tick");

    struct pgwt_trace_event out[4];
    memset(out, 0xAA, sizeof(out));
    int n = pgwt_sampler_build_batch(targets, vals, valid, 4, 99, out,
                                     NULL, NULL);
    CHECK(n == 2, "only 2 valid targets classified, got %d", n);
    CHECK(out[0].pid == (uint32_t)getpid() && out[0].new_event == 0
              && out[0].query_id == 20,
          "successful zero remains a command-open CPU sample");
    CHECK(out[1].pid == (uint32_t)getpid() && out[1].new_event == wait
              && out[1].query_id == 30,
          "successful wait remains classified and attributed");

    int cpu_samples = 0;
    for (int i = 0; i < n; i++)
        if (out[i].new_event == 0)
            cpu_samples++;
    CHECK(cpu_samples == 1,
          "exactly one CPU sample: failed reads add no CPU-class demand");

    double aas = -1.0, lock_fraction = -1.0;
    pgwt_anomaly_metrics_from_batch(out, n, &aas, &lock_fraction);
    CHECK(aas == 2.0,
          "AAS includes only valid CPU+wait targets (%.1f expected 2.0)", aas);
}

/* ── Test 4: child process read (cross-pid, shared mapping) ───────────── */

static void test_child_read(void)
{
    printf("--- read child process value (cross-pid, MAP_SHARED) ---\n");

    /* Shared anonymous mmap: parent writes a known value, child sees it at
     * its own (possibly different) virtual address, reports the address back
     * via pipe, then sleeps so the parent can sample it. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED, "mmap shared failed");
    if (page == MAP_FAILED)
        return;

    volatile uint32_t *slot = page;
    *slot = WEI(PG_WAIT_IPC, 0x42);

    int pfd[2];
    if (pipe(pfd) != 0) { CHECK(0, "pipe failed"); return; }

    pid_t child = fork();
    if (child == 0) {
        /* Child: report the slot's address (same VA — MAP_SHARED inherited),
         * then idle until killed. */
        uint64_t addr = (uint64_t)(uintptr_t)slot;
        ssize_t w = write(pfd[1], &addr, sizeof(addr));
        (void)w;
        for (;;) pause();
        _exit(0);
    }
    CHECK(child > 0, "fork failed");

    uint64_t child_addr = 0;
    ssize_t r = read(pfd[0], &child_addr, sizeof(child_addr));
    CHECK(r == (ssize_t)sizeof(child_addr), "did not get child address");

    struct pgwt_sample_target targets[1] = {
        { .pid = child, .wait_event_addr = child_addr, .query_id = 7, .is_shared = 1 },
    };
    uint32_t vals[1] = { 0xdead };
    uint8_t valid[1] = { 0 };
    uint64_t faults = 0;
    int got = pgwt_sampler_read_targets(targets, 1, vals, valid, &faults);

    CHECK(got == 1, "expected to read child value, got %d", got);
    CHECK(valid[0] == 1, "successful child read marked valid");
    CHECK(vals[0] == WEI(PG_WAIT_IPC, 0x42),
          "child vals[0]=0x%x expected 0x%x", vals[0], WEI(PG_WAIT_IPC, 0x42));

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(pfd[0]);
    close(pfd[1]);
    munmap(page, 4096);
}

/* ── Test 5 (SMP-2): process-LOCAL addresses must be read per-pid ─────── */

/* A .data global: after fork it exists at the SAME virtual address in the
 * child, but the pages are PRIVATE (COW) — the child's value diverges from
 * the parent's. The old batched sweep read the child's target through the
 * PARENT pid (the batch's reader): the read SUCCEEDED and returned the
 * PARENT's value, silently misattributed to the child. With is_shared unset
 * the target must be read via /proc/<child>/mem and return the CHILD's
 * value. */
static volatile uint32_t smp2_local_slot = WEI(PG_WAIT_LOCK, 0x01);

static void test_local_addr_not_batched(void)
{
    printf("--- SMP-2: process-local address read per-pid, not batched ---\n");

    const uint32_t parent_val = WEI(PG_WAIT_LOCK, 0x01);
    const uint32_t child_val  = WEI(PG_WAIT_IO, 0x05);

    int pfd[2];
    if (pipe(pfd) != 0) { CHECK(0, "pipe failed"); return; }

    pid_t child = fork();
    if (child == 0) {
        /* Child: overwrite ITS OWN copy of the global (COW page), signal
         * readiness, idle until killed. */
        smp2_local_slot = child_val;
        char ready = 1;
        ssize_t w = write(pfd[1], &ready, 1);
        (void)w;
        for (;;) pause();
        _exit(0);
    }
    CHECK(child > 0, "fork failed");

    char ready = 0;
    ssize_t r = read(pfd[0], &ready, 1);
    CHECK(r == 1 && ready == 1, "child did not signal readiness");

    /* Target 0: the parent's own slot (batched — its own pid is the batch
     * reader, sound). Target 1: the CHILD's slot at the same VA, correctly
     * marked NOT shared. Before the SMP-2 fix, target 1 was read through
     * target 0's pid (the parent) and returned parent_val — this asserts
     * the child's value comes back. */
    struct pgwt_sample_target targets[2] = {
        { .pid = getpid(), .query_id = 1, .is_shared = 1,
          .wait_event_addr = (uint64_t)(uintptr_t)&smp2_local_slot },
        { .pid = child, .query_id = 2, .is_shared = 0,
          .wait_event_addr = (uint64_t)(uintptr_t)&smp2_local_slot },
    };
    uint32_t vals[2] = { 0xdead, 0xdead };
    uint8_t valid[2] = { 0, 0 };
    int got = pgwt_sampler_read_targets(targets, 2, vals, valid, NULL);

    CHECK(got == 2, "expected both targets read, got %d", got);
    CHECK(valid[0] == 1 && valid[1] == 1,
          "both shared and per-pid reads marked valid");
    CHECK(vals[0] == parent_val, "parent slot = 0x%x expected 0x%x",
          vals[0], parent_val);
    CHECK(vals[1] == child_val,
          "SMP-2: child's local slot = 0x%x expected the CHILD's value 0x%x "
          "(reader-pid value = misattribution)", vals[1], child_val);

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(pfd[0]);
    close(pfd[1]);
}

/* ── Test 6 (SMP-1): read-health state machine ────────────────────────── */

static void test_health(void)
{
    printf("--- SMP-1: sampler read-health state machine ---\n");

    const uint64_t SEC = 1000000000ULL;
    struct pgwt_sampler_health h;
    memset(&h, 0, sizeof(h));
    h.healthy = 1;

    /* Healthy ticks: no action, stays healthy. */
    CHECK(pgwt_sampler_health_note(&h, 10, 10, 0, 1 * SEC)
              == PGWT_SAMPLER_LOG_NONE, "healthy tick -> no log");
    CHECK(h.healthy == 1, "still healthy");

    /* Zero-target ticks are neutral (nothing to read != failure). */
    CHECK(pgwt_sampler_health_note(&h, 0, 0, 0, 2 * SEC)
              == PGWT_SAMPLER_LOG_NONE, "no-target tick is neutral");
    CHECK(h.healthy == 1, "no-target tick must not flag unhealthy");

    /* FIRST total failure: loud immediately + unhealthy. */
    CHECK(pgwt_sampler_health_note(&h, 10, 0, 1 /*EPERM*/, 3 * SEC)
              == PGWT_SAMPLER_LOG_DEGRADED, "first total failure logs");
    CHECK(h.healthy == 0, "unhealthy after first total failure");
    CHECK(h.last_errno == 1, "errno recorded");

    /* Following failures within the re-log window: silent but counted. */
    CHECK(pgwt_sampler_health_note(&h, 10, 0, 1, 4 * SEC)
              == PGWT_SAMPLER_LOG_NONE, "repeat failure within window silent");
    CHECK(h.consec_failed_ticks == 2, "consecutive failures counted");

    /* Persistent failure: re-logs after the period (60s). */
    CHECK(pgwt_sampler_health_note(&h, 10, 0, 1, 64 * SEC)
              == PGWT_SAMPLER_LOG_DEGRADED, "persistent failure re-logs");

    /* Recovery: logs once, healthy again, consec reset. */
    CHECK(pgwt_sampler_health_note(&h, 10, 5, 0, 65 * SEC)
              == PGWT_SAMPLER_LOG_RECOVERED, "recovery logs");
    CHECK(h.healthy == 1, "healthy after recovery");
    CHECK(h.consec_failed_ticks == 0, "consecutive counter reset");
    CHECK(h.failed_ticks_total == 3, "total failed ticks preserved (got %llu)",
          (unsigned long long)h.failed_ticks_total);

    /* A partial read (some targets fail) is NOT a health failure — only a
     * TOTAL failure is indistinguishable from idle. */
    CHECK(pgwt_sampler_health_note(&h, 10, 1, 0, 66 * SEC)
              == PGWT_SAMPLER_LOG_NONE, "partial read stays healthy");
    CHECK(h.healthy == 1, "partial read keeps healthy");
}

/* ── Test 7 (SMP-3): effective sample period ──────────────────────────── */

static void test_effective_period(void)
{
    printf("--- SMP-3: effective sample period (missed-tick weight) ---\n");

    const uint64_t NOM = 100000000ULL;   /* 100ms nominal (10 Hz) */

    /* First tick: nominal. */
    CHECK(pgwt_sampler_effective_period(NOM, 0, 5000) == NOM,
          "first tick uses nominal");

    /* On-time tick: measured == nominal. */
    CHECK(pgwt_sampler_effective_period(NOM, 1000, 1000 + NOM) == NOM,
          "on-time tick = nominal");

    /* Stalled daemon: 3 ticks coalesced -> weight = the real elapsed time
     * (this is the SMP-3 fix: nominal weight would deflate AAS under load). */
    CHECK(pgwt_sampler_effective_period(NOM, 1000, 1000 + 3 * NOM) == 3 * NOM,
          "coalesced ticks weighted by measured elapsed");

    /* Early/backwards clock never shrinks below nominal. */
    CHECK(pgwt_sampler_effective_period(NOM, 1000, 1000 + NOM / 2) == NOM,
          "early tick clamps to nominal");
    CHECK(pgwt_sampler_effective_period(NOM, 5000, 1000) == NOM,
          "non-monotonic input clamps to nominal");

    /* Absurd stall clamps at 60s. */
    CHECK(pgwt_sampler_effective_period(NOM, 0x1000, 0x1000 + 3600ULL * 1000000000ULL)
              == 60ULL * 1000000000ULL,
          "stall clamped to 60s");
}

/* ── Test 8 (SMP-4): pid -> query_id join index ───────────────────────── */

static void test_qid_index(void)
{
    printf("--- SMP-4: qid join index sort + lookup ---\n");

    struct pgwt_qid_entry e[5] = {
        { .pid = 500, .query_id = 55 },
        { .pid = 100, .query_id = 11 },
        { .pid = 900, .query_id = 99 },
        { .pid = 300, .query_id = 33 },
        { .pid = 700, .query_id = 77 },
    };
    pgwt_qid_index_sort(e, 5);
    for (int i = 1; i < 5; i++)
        CHECK(e[i - 1].pid < e[i].pid, "sorted order at %d", i);

    CHECK(pgwt_qid_index_lookup(e, 5, 100) == 11, "lookup first");
    CHECK(pgwt_qid_index_lookup(e, 5, 900) == 99, "lookup last");
    CHECK(pgwt_qid_index_lookup(e, 5, 300) == 33, "lookup middle");
    CHECK(pgwt_qid_index_lookup(e, 5, 301) == 0, "missing pid -> 0");
    CHECK(pgwt_qid_index_lookup(e, 0, 100) == 0, "empty index -> 0");
}

int main(void)
{
    test_self_read();
    test_build_batch();
    test_build_batch_garbage();
    test_build_batch_cpu_policy();
    test_fallback();
    test_read_validity_excludes_failures();
    test_child_read();
    test_local_addr_not_batched();
    test_health();
    test_effective_period();
    test_qid_index();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
