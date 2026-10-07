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

    /* #128: an IDLE sample carries the finished statement's raw st_query_id
     * (last_query_id); a non-idle sample without an effective id does not
     * (0 during parse analysis, resolved later by query_attr.h). */
    struct pgwt_sample_target idle_targets[3] = {
        { .pid = 2001, .wait_event_addr = 0x1000, .query_id = 0,
          .last_query_id = 555, .backend_type = PGWT_BT_CLIENT, .cmd_open = 0 },
        { .pid = 2002, .wait_event_addr = 0x2000, .query_id = 0,
          .last_query_id = 666, .backend_type = PGWT_BT_CLIENT, .cmd_open = 1 },
        { .pid = 2003, .wait_event_addr = 0x3000, .query_id = 777,
          .last_query_id = 888, .backend_type = PGWT_BT_CLIENT, .cmd_open = 1 },
    };
    uint32_t idle_vals[3] = {
        PG_WAIT_CLIENT_READ,      /* idle: takes last_query_id */
        WEI(PG_WAIT_LOCK, 0x00),  /* parse-phase lock wait: stays 0 */
        PG_WAIT_CLIENT_READ,      /* effective id present: kept */
    };
    n = pgwt_sampler_build_batch(idle_targets, idle_vals, valid, 3, ts, out,
                                 NULL, &noncmd);
    CHECK(n == 3, "3 idle-attribution records, got %d", n);
    CHECK(out[0].query_id == 555, "idle sample carries last_query_id (got %llu)",
          (unsigned long long)out[0].query_id);
    CHECK(out[1].query_id == 0, "in-command wait without an id stays 0 (got %llu)",
          (unsigned long long)out[1].query_id);
    CHECK(out[2].query_id == 777, "an effective id is never overridden (got %llu)",
          (unsigned long long)out[2].query_id);
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
    /* Foreground (no category flag) + the at-tick cmd_open reading, which
     * the live query attribution needs to tell a coherent idle sample from
     * one whose wait event contradicts the status read (#128 follow-up,
     * map_reader.h pgwt_live_qattr_sample). In-memory only: the SAMPLES
     * block carries no flags column. */
    CHECK(out[0].flags == PGWT_EVENT_FLAG_CMD_OPEN,
          "client CPU sample is foreground and carries cmd_open (got 0x%x)",
          out[0].flags);
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
    CHECK(out[0].flags == PGWT_EVENT_FLAG_CMD_OPEN,
          "client WAIT sample inside a command carries cmd_open (got 0x%x)",
          out[0].flags);

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

/* ── #294: the read-ORDER race, and every way the recheck could be blind ── */

#define RC_N 7
struct fake_gate {
    int calls[RC_N];     /* per-index consultations: proves WHO was consulted */
    int total_calls;
    int succeed;         /* 0 = the fresh read fails (writes nothing) */
    int open_now;        /* what a successful fresh read reports */
    uint64_t qid;
    int qid_valid;
};

static int fake_gate_read(void *ctx, int idx,
                          const struct pgwt_sample_target *t,
                          int *cmd_open, uint64_t *query_id,
                          int *query_id_valid)
{
    struct fake_gate *f = ctx;
    (void)t;
    if (idx >= 0 && idx < RC_N)
        f->calls[idx]++;
    f->total_calls++;
    if (!f->succeed)
        return 0;        /* a failed read must write NOTHING */
    *cmd_open = f->open_now;
    if (f->qid_valid) {
        *query_id = f->qid;
        *query_id_valid = 1;
    }
    return 1;
}

/* The tick's two reads, as the live path takes them:
 *   cmd_open  <- target loop, time T1
 *   we        <- batched process_vm_readv, time T2 > T1
 * A command that OPENS in [T1,T2] is the race: we==0 (fresh) with cmd_open==0
 * (stale). Targets 0 and 6 are in that state; nothing else is. */
static void rc_reset(struct pgwt_sample_target *t, uint32_t *vals,
                     uint8_t *valid)
{
    memset(t, 0, sizeof(*t) * RC_N);
    /* 0: CLIENT, on CPU, gate stale-closed            -> THE RACE */
    t[0].pid = 101; t[0].backend_type = PGWT_BT_CLIENT; t[0].query_id = 7;
    /* 1: CLIENT, on CPU, gate already open            -> nothing at risk */
    t[1].pid = 102; t[1].backend_type = PGWT_BT_CLIENT; t[1].cmd_open = 1;
    /* 2: CLIENT, WAITING                              -> gate does not apply */
    t[2].pid = 103; t[2].backend_type = PGWT_BT_CLIENT;
    /* 3: CLIENT, on CPU, but the read FAILED          -> not an observation */
    t[3].pid = 104; t[3].backend_type = PGWT_BT_CLIENT;
    /* 4: CHECKPOINTER, on CPU                         -> recordable anyway */
    t[4].pid = 105; t[4].backend_type = PGWT_BT_CHECKPOINTER;
    /* 5: LOGGER, on CPU                               -> never recordable */
    t[5].pid = 106; t[5].backend_type = PGWT_BT_LOGGER;
    /* 6: UNKNOWN, on CPU, gate stale-closed           -> THE RACE */
    t[6].pid = 107; t[6].backend_type = PGWT_BT_UNKNOWN;

    for (int i = 0; i < RC_N; i++) { vals[i] = 0; valid[i] = 1; }
    vals[2] = WEI(PG_WAIT_LOCK, 0x01);   /* waiting, not on CPU */
    valid[3] = 0;                        /* unread */
}

static void test_recheck_cmd_gate_order_race(void)
{
    printf("--- #294 read-order recheck: the race, and its blind spots ---\n");

    struct pgwt_sample_target t[RC_N];
    uint32_t vals[RC_N];
    uint8_t valid[RC_N];
    struct pgwt_trace_event out[RC_N];
    struct pgwt_sampler_recheck_stats st;
    uint64_t noncmd;

    /* RED without the fix: build the batch straight from the tick's reads and
     * both raced samples are lost — exactly the shipped behaviour #294
     * measured as 10.7-11.7 pp of missing CPU share. */
    rc_reset(t, vals, valid);
    noncmd = 0;
    int n = pgwt_sampler_build_batch(t, vals, valid, RC_N, 11, out, NULL,
                                     &noncmd);
    CHECK(n == 3, "unfixed order: only 3 of 6 readable samples survive (got %d)",
          n);
    CHECK(noncmd == 3,
          "unfixed order: 2 raced client/unknown CPU samples + the logger are "
          "dropped (got %llu)", (unsigned long long)noncmd);

    /* GREEN with the fix: a fresh gate read, taken AFTER the wait_event read,
     * finds both raced backends inside a command. */
    rc_reset(t, vals, valid);
    struct fake_gate f = { .succeed = 1, .open_now = 1,
                           .qid = 0xabcdef, .qid_valid = 1 };
    int rec = pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N,
                                            fake_gate_read, &f, &st);
    CHECK(rec == 2 && st.recovered == 2 && st.at_risk == 2,
          "both raced samples recovered (rec=%d recovered=%llu at_risk=%llu)",
          rec, (unsigned long long)st.recovered,
          (unsigned long long)st.at_risk);
    /* The at-risk set is exactly the raced pair: anything wider would recover
     * samples that were never dropped, which is how this check could pass
     * while measuring nothing. */
    CHECK(f.total_calls == 2 && f.calls[0] == 1 && f.calls[6] == 1 &&
          f.calls[1] == 0 && f.calls[2] == 0 && f.calls[3] == 0 &&
          f.calls[4] == 0 && f.calls[5] == 0,
          "only the raced targets are consulted (%d calls: "
          "%d %d %d %d %d %d %d)", f.total_calls, f.calls[0], f.calls[1],
          f.calls[2], f.calls[3], f.calls[4], f.calls[5], f.calls[6]);
    CHECK(t[0].query_id == 0xabcdef &&
          t[0].query_quality == PGWT_QUERY_QUALITY_REAL,
          "a recovered sample carries the id from the SAME coherent snapshot");
    noncmd = 0;
    n = pgwt_sampler_build_batch(t, vals, valid, RC_N, 12, out, NULL, &noncmd);
    CHECK(n == 5, "fixed order: 5 samples recorded (got %d)", n);
    CHECK(noncmd == 1,
          "the remainder is the logger alone — the counter must NOT reach 0, "
          "genuinely-non-command CPU still belongs outside AAS (got %llu)",
          (unsigned long long)noncmd);
    CHECK(out[0].pid == 101 && out[0].new_event == 0 &&
          (out[0].flags & PGWT_EVENT_FLAG_CMD_OPEN),
          "the recovered sample is an on-CPU record stamped command-open");

    /* Blind spot 1 — the fresh read FAILS (backend exited, EPERM, torn row).
     * It must leave the gate closed and say so, never fabricate a command. */
    rc_reset(t, vals, valid);
    f = (struct fake_gate){ .succeed = 0 };
    rec = pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read,
                                        &f, &st);
    CHECK(rec == 0 && st.read_failed == 2 && st.at_risk == 2 &&
          !t[0].cmd_open && !t[6].cmd_open,
          "a failed fresh read never opens the gate (read_failed=%llu)",
          (unsigned long long)st.read_failed);
    noncmd = 0;
    n = pgwt_sampler_build_batch(t, vals, valid, RC_N, 13, out, NULL, &noncmd);
    CHECK(n == 3 && noncmd == 3,
          "unreadable gates stay dropped and stay counted (n=%d noncmd=%llu)",
          n, (unsigned long long)noncmd);

    /* Blind spot 2 — the thing being checked is ABSENT, not wrong: the
     * backend really is between commands. The recheck must confirm the drop,
     * not launder it into a CPU sample. */
    rc_reset(t, vals, valid);
    f = (struct fake_gate){ .succeed = 1, .open_now = 0 };
    rec = pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read,
                                        &f, &st);
    CHECK(rec == 0 && st.confirmed_closed == 2 && !t[0].cmd_open,
          "a genuinely idle backend stays dropped (confirmed=%llu)",
          (unsigned long long)st.confirmed_closed);

    /* Blind spot 3 — WRONG PLACEMENT. Run before the wait_event batch read
     * and every valid[] is still 0; the recheck must then see nothing at all.
     * A nonzero at_risk here would mean it is deciding on unread values. */
    rc_reset(t, vals, valid);
    memset(valid, 0, sizeof(valid));
    f = (struct fake_gate){ .succeed = 1, .open_now = 1 };
    rec = pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read,
                                        &f, &st);
    CHECK(rec == 0 && st.at_risk == 0 && f.total_calls == 0,
          "placed before the wait_event read it recovers nothing (at_risk=%llu)",
          (unsigned long long)st.at_risk);

    /* Blind spot 4 — missing dependency / empty input: refuse, never approve.
     * stats must come back zeroed rather than stale. */
    rc_reset(t, vals, valid);
    st.at_risk = st.recovered = 99;
    CHECK(pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, NULL, NULL,
                                        &st) == 0 &&
          st.at_risk == 0 && st.recovered == 0 && !t[0].cmd_open,
          "no fresh-read function: refuses with zeroed stats");
    st.at_risk = st.recovered = 99;
    f = (struct fake_gate){ .succeed = 1, .open_now = 1 };
    CHECK(pgwt_sampler_recheck_cmd_gate(t, vals, valid, 0, fake_gate_read,
                                        &f, &st) == 0 &&
          st.at_risk == 0 && f.total_calls == 0,
          "no targets: refuses");
    CHECK(pgwt_sampler_recheck_cmd_gate(t, NULL, valid, RC_N, fake_gate_read,
                                        &f, &st) == 0 &&
          st.at_risk == 0 && f.total_calls == 0,
          "no readings array: refuses");
    CHECK(pgwt_sampler_recheck_cmd_gate(t, vals, NULL, RC_N, fake_gate_read,
                                        &f, &st) == 0 &&
          st.at_risk == 0 && f.total_calls == 0,
          "no validity array: refuses");
    CHECK(pgwt_sampler_recheck_cmd_gate(NULL, vals, valid, RC_N,
                                        fake_gate_read, &f, &st) == 0 &&
          st.at_risk == 0, "no targets array: refuses");
    CHECK(pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read,
                                        &f, NULL) == 2,
          "a NULL stats pointer is tolerated, the recovery still happens");

    /* Conservation: every at-risk target lands in exactly one outcome, so no
     * outcome can hide inside another (the two-sides-from-one-sum trap). */
    rc_reset(t, vals, valid);
    t[6].backend_type = PGWT_BT_CLIENT;
    f = (struct fake_gate){ .succeed = 1, .open_now = 1 };
    pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read, &f,
                                  &st);
    CHECK(st.at_risk == st.recovered + st.confirmed_closed + st.read_failed &&
          st.at_risk == 2,
          "at_risk == recovered + confirmed + failed (%llu vs %llu+%llu+%llu)",
          (unsigned long long)st.at_risk, (unsigned long long)st.recovered,
          (unsigned long long)st.confirmed_closed,
          (unsigned long long)st.read_failed);

    /* One-directional by design: an already-open gate is never re-read, so the
     * recheck cannot CLOSE one. That is the residual over-count side, and it
     * is deliberate — documented in sampler.h. */
    rc_reset(t, vals, valid);
    f = (struct fake_gate){ .succeed = 1, .open_now = 0 };
    pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read, &f,
                                  &st);
    CHECK(t[1].cmd_open == 1 && f.calls[1] == 0,
          "an already-open gate is left alone (one-directional)");

    /* PG13 has no st_query_id: the gate opens, the id is left as it was,
     * never overwritten with a zero that would look like a real reading. */
    rc_reset(t, vals, valid);
    t[0].query_id = 777;
    f = (struct fake_gate){ .succeed = 1, .open_now = 1, .qid_valid = 0 };
    pgwt_sampler_recheck_cmd_gate(t, vals, valid, RC_N, fake_gate_read, &f,
                                  &st);
    CHECK(st.recovered == 2 && t[0].cmd_open == 1 && t[0].query_id == 777,
          "no query id available: gate opens, prior id preserved (qid=%llu)",
          (unsigned long long)t[0].query_id);
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

    double aas = -1.0, lock_fraction = -1.0, cpu_aas = -1.0;
    pgwt_anomaly_metrics_from_batch(out, n, &aas, &lock_fraction, &cpu_aas);
    CHECK(aas == 2.0,
          "AAS includes only valid CPU+wait targets (%.1f expected 2.0)", aas);
    CHECK(cpu_aas == 1.0,
          "CPU AAS includes only the valid CPU target (%.1f expected 1.0)",
          cpu_aas);
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

    struct pgwt_exact_attr edge = {
        .query_generation = 8,
        .cmd_generation = 8,
        .query_id = 123,
        .cmd_open = 1,
    };
    CHECK(pgwt_exact_attr_shadow_comparable(&edge, 8),
          "same-generation query/activity tuple is shadow-comparable");
    edge.query_generation = 7;
    CHECK(!pgwt_exact_attr_shadow_comparable(&edge, 8),
          "stale query edge is excluded from shadow comparison");
    edge.query_generation = 8;
    edge.cmd_generation = 7;
    CHECK(!pgwt_exact_attr_shadow_comparable(&edge, 8),
          "stale activity edge is excluded from shadow comparison");
}

static void test_sampled_attr_source_gate(void)
{
    printf("--- Stage 2 sampled-attribution source gate + shadow compare ---\n");
    struct pgwt_sampled_attr_value uprobe = {
        .query_id = 111, .cmd_open = 1,
    };
    struct pgwt_sampled_attr_value tick = {
        .query_id = 222, .cmd_open = 1,
    };
    struct pgwt_sample_target target = {
        .query_id = UINT64_MAX, .cmd_open = 1,
    };

    CHECK(pgwt_sampler_select_attr(0, 1, &tick, &uprobe, &target) ==
              PGWT_SAMPLED_ATTR_UPROBE &&
          target.query_id == 111 && target.cmd_open == 1,
          "degraded/PG13 gate preserves the existing uprobe source");
    CHECK(pgwt_sampler_select_attr(1, 1, &tick, &uprobe, &target) ==
              PGWT_SAMPLED_ATTR_TICK &&
          target.query_id == 222 && target.cmd_open == 1,
          "validated coherent read selects the at-tick source");

    /* Seed stale values before a failed validated-layout read.  A CLIENT must
     * fail closed instead of falling back to the shadow map. */
    target.backend_type = PGWT_BT_CLIENT;
    target.query_id = UINT64_MAX;
    target.cmd_open = 1;
    CHECK(pgwt_sampler_select_attr(1, 0, &tick, &uprobe, &target) ==
              PGWT_SAMPLED_ATTR_DROP &&
          target.query_id == 0 && target.cmd_open == 0,
          "incoherent CLIENT tick drops and zeroes attribution");

    target.backend_type = PGWT_BT_UNKNOWN;
    target.query_id = UINT64_MAX;
    target.cmd_open = 1;
    CHECK(pgwt_sampler_select_attr(1, 0, &tick, &uprobe, &target) ==
              PGWT_SAMPLED_ATTR_DROP &&
          target.query_id == 0 && target.cmd_open == 0,
          "incoherent UNKNOWN tick remains command-gated and drops");

    /* An io_worker has no meaningful query attribution and its CPU admission
     * is cmd_open-independent.  Preserve the observation without consulting
     * the stale uprobe shadow; the poll loop therefore keeps it covered. */
    target.backend_type = PGWT_BT_IO_WORKER;
    target.query_id = UINT64_MAX;
    target.cmd_open = 1;
    enum pgwt_sampled_attr_source source = pgwt_sampler_select_attr(
        1, 0, &tick, &uprobe, &target);
    CHECK(source == PGWT_SAMPLED_ATTR_UNATTRIBUTED &&
          target.query_id == 0 && target.cmd_open == 0,
          "incoherent io_worker tick records without attribution");

    uint32_t we = 0;
    uint8_t valid = source != PGWT_SAMPLED_ATTR_DROP;
    struct pgwt_trace_event sample = {0};
    CHECK(pgwt_sampler_build_batch(&target, &we, &valid, 1, 123,
                                   &sample, NULL, NULL) == 1 &&
          sample.query_id == 0 &&
          sample.flags == PGWT_EVENT_FLAG_IO_WORKER &&
          sample.new_event == 0,
          "failed-attribution io_worker CPU observation is admitted");

    /* #128 stale-slot path: targets[] is a static array reused every tick.
     * A slot whose previous occupant was a client in statement 999 is now
     * a PARALLEL_WORKER whose PgBackendStatus read failed: it stays
     * recordable (UNATTRIBUTED), and its IDLE sample must NOT carry 999 —
     * the live resolver would take that as an id report and back-fill the
     * worker's pending waits to a foreign query. */
    tick.last_query_id = 777;
    target.backend_type = PGWT_BT_PARALLEL_WORKER;
    target.query_id = UINT64_MAX;
    target.cmd_open = 1;
    target.last_query_id = 999;                 /* previous occupant's */
    source = pgwt_sampler_select_attr(1, 0, &tick, &uprobe, &target);
    CHECK(source == PGWT_SAMPLED_ATTR_UNATTRIBUTED && target.last_query_id == 0,
          "failed tick read zeroes last_query_id (stale slot, got %llu)",
          (unsigned long long)target.last_query_id);
    we = PG_WAIT_CLIENT_READ;
    valid = 1;
    CHECK(pgwt_sampler_build_batch(&target, &we, &valid, 1, 124,
                                   &sample, NULL, NULL) == 1 &&
          sample.query_id == 0,
          "…so its idle sample carries no foreign id (got %llu)",
          (unsigned long long)sample.query_id);
    /* A coherent tick read is the only source of last_query_id. */
    target.last_query_id = 999;
    CHECK(pgwt_sampler_select_attr(1, 1, &tick, &uprobe, &target) ==
              PGWT_SAMPLED_ATTR_TICK && target.last_query_id == 777,
          "coherent tick read sets last_query_id from the tick (got %llu)",
          (unsigned long long)target.last_query_id);
    target.last_query_id = 999;
    CHECK(pgwt_sampler_select_attr(0, 1, &tick, &uprobe, &target) ==
              PGWT_SAMPLED_ATTR_UPROBE && target.last_query_id == 0,
          "uprobe-shadow source never supplies last_query_id (got %llu)",
          (unsigned long long)target.last_query_id);

    struct pgwt_sampler coverage = {0};
    pgwt_sampler_note_coverage(&coverage, 1, valid);
    CHECK(coverage.read_valid_last == 1 && coverage.read_invalid_last == 0,
          "recorded io_worker remains covered after an incoherent tick read");

    pgwt_sampler_note_coverage(&coverage, 2, 0);
    CHECK(coverage.read_valid_last == 0 && coverage.read_invalid_last == 2,
          "dropped CLIENT/UNKNOWN targets leave coverage incomplete");

    CHECK(pgwt_sampled_attr_compare(&uprobe, &uprobe) == 0,
          "identical sources agree");
    unsigned mismatch = pgwt_sampled_attr_compare(&tick, &uprobe);
    CHECK(mismatch == PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID,
          "query-id-only mismatch is split by field");
    tick.cmd_open = 0;
    mismatch = pgwt_sampled_attr_compare(&tick, &uprobe);
    CHECK((mismatch & PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN) &&
          (mismatch & PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID),
          "tuple mismatch reports both fields");

    tick.query_id = 0xaaaa;
    tick.cmd_open = 0;
    uprobe.query_id = 0xbbbb; /* retained last query while idle */
    uprobe.cmd_open = 0;
    CHECK(pgwt_sampled_attr_compare(&tick, &uprobe) == 0,
          "idle sources compare effective query-id zero, not retained ids");
    CHECK(!pgwt_sampled_attr_active_query_mismatch(&tick, &uprobe),
          "idle backend is excluded from the active query-id proof");

    tick.query_id = uprobe.query_id;
    tick.cmd_open = 1;
    uprobe.cmd_open = 0;
    CHECK(!pgwt_sampled_attr_active_query_mismatch(&tick, &uprobe),
          "equal raw active query ids pass despite a gate-boundary straddle");
    uprobe.query_id++;
    CHECK(pgwt_sampled_attr_active_query_mismatch(&tick, &uprobe),
          "active st_query_id disagreement fails the load-bearing proof");
}

int main(void)
{
    test_self_read();
    test_build_batch();
    test_build_batch_garbage();
    test_build_batch_cpu_policy();
    test_recheck_cmd_gate_order_race();
    test_fallback();
    test_read_validity_excludes_failures();
    test_child_read();
    test_local_addr_not_batched();
    test_health();
    test_effective_period();
    test_qid_index();
    test_sampled_attr_source_gate();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
