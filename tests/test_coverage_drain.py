#!/usr/bin/env python3
"""Mac coverage-fence regressions using the production tracker and tick ops.

Only positions, ring I/O, writer calls and clocks are fakes. The daemon's
position/publication bodies are extracted unchanged; open_snapshot.c's pure
tracker is compiled whole. --mutants requires compilation and the intended
assertion, not a build failure. --source-root and --case demonstrate regressions on a base.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_backend_init_order import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "daemon.h"
#include "capture_counters.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

@DEFINES@
#define SEC 1000000000ULL
struct ring { unsigned long producer, consumer; bool busy; };
static struct ring positions;
static struct pgwt_daemon d;
static struct pgwt_event_writer writer;
static struct pgwt_counter_vector counters;
static uint64_t wall, callback_cost, written_w, gap_start, gap_end, sealed_w;
static unsigned consumes, callbacks, publishes, watermarks, gaps;
static bool publish_ok, watermark_ok, reject_callback, fail_push, ring_missing;
static int failures;
#define CHECK(ok, message) do { if (!(ok)) { \
    fprintf(stderr, "FAIL %s: %s\n", __func__, message); failures++; } } while (0)

uint64_t pgwt_debug_monotonic_ns(void) { return wall; }
static uint64_t now_ns(void) { return wall; }
struct ring *ring_buffer__ring(struct ring_buffer *rb, unsigned int idx)
{ assert(rb == d.event_rb && idx == 0); return ring_missing ? NULL : &positions; }
unsigned long ring__producer_pos(const struct ring *r) { return r->producer; }
unsigned long ring__consumer_pos(const struct ring *r) { return r->consumer; }
int ring_buffer__consume(struct ring_buffer *rb)
{
    assert(rb == d.event_rb);
    consumes++;
    /* A busy reservation blocks the consumer, even when the later prefix
     * is committed. libbpf returns zero, which is NOT a completion proof. */
    if (positions.busy) return 0;
    while (positions.consumer < positions.producer) {
        positions.consumer += 8;
        callbacks++;
        wall += callback_cost;
        d.event_drain_callbacks_current++;
        if (reject_callback) { reject_callback = false; return -EIO; }
        if (fail_push) {
            counters.components[PGWT_C_WRITER_PUSH_FAIL].value++;
            fail_push = false;
        }
        if (d.event_drain_callback_limit &&
            d.event_drain_callbacks_current >= d.event_drain_callback_limit)
            return -EAGAIN;
    }
    return 0;
}
int pgwt_writer_publish(struct pgwt_event_writer *w)
{ assert(w == &writer); publishes++; return publish_ok ? 0 : -1; }
int pgwt_writer_coverage_watermark(struct pgwt_event_writer *w,
    uint64_t obs, uint64_t mark, uint64_t fd, uint64_t fi)
{
    assert(w == &writer && obs == wall && fi <= fd && fd <= mark);
    if (!watermark_ok) return -1;
    written_w = mark;
    watermarks++;
    return 0;
}
int pgwt_writer_coverage_gap(struct pgwt_event_writer *w, uint64_t obs,
    bool open, uint64_t id, uint64_t at, uint8_t reason)
{
    assert(w == &writer && obs == wall && id > 0);
    if (open) { gap_start = at; gaps++; assert(reason != 0); }
    else gap_end = at;
    return 0;
}
int pgwt_writer_coverage_seal(struct pgwt_event_writer *w, uint64_t obs,
    uint64_t mark)
{ assert(w == &writer && obs == wall); sealed_w = mark; return 0; }
int pgwt_is_idle_event(uint32_t event) { (void)event; abort(); }

/* This suite exercises the shared-ring branch of shutdown. exact_perf's
 * gather/producer binding is exercised by test_exact_perf_coverage.py. */
static void cov_gather(struct pgwt_daemon *daemon, struct pgwt_cov_tick_in *in,
                        struct pgwt_counter_vector *cv)
{ (void)daemon; (void)in; (void)cv; abort(); }

@FUNCTIONS@

struct pgwt_counter_vector pgwt_daemon_counter_vector(struct pgwt_daemon *daemon)
{ assert(daemon == &d); return counters; }
static uint64_t tick_clock(void *ctx) { assert(ctx == &d); return now_ns(); }
static struct pgwt_cov_tick_in collect(void)
{
    struct pgwt_cov_tick_in in = {0};
    static struct pgwt_counter_vector cv;
    const struct pgwt_cov_tick_ops ops = { @OPS@ };
    in.floors_known = true;
    in.floor_db_ns = wall - 10;
    in.floor_idle_ns = wall - 20;
    pgwt_cov_collect_tick(&in, &cv, &d, &ops);
    return in;
}
static int tick(unsigned second)
{
    wall = (second + 1) * SEC;
    struct pgwt_cov_tick_in in = collect();
    return pgwt_cov_tracker_tick(&d.coverage, &writer, &in);
}
static void reset(void)
{
    memset(&d, 0, sizeof(d)); memset(&writer, 0, sizeof(writer));
    memset(&counters, 0, sizeof(counters)); memset(&positions, 0, sizeof(positions));
    wall = SEC; callback_cost = 1; publish_ok = watermark_ok = true;
    consumes = callbacks = publishes = watermarks = gaps = 0; reject_callback = fail_push = false;
    gap_start = gap_end = sealed_w = 0; written_w = SEC; ring_missing = false;
    counters.valid = true;
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; i++)
        counters.components[i].valid = true;
    d.event_rb = (struct ring_buffer *)&positions;
    d.event_writer = &writer; writer.cov_enabled = true;
    pgwt_cov_tracker_init(&d.coverage, wall, true, &counters);
}
static void T1_sustained_backlog(void)
{
    reset(); positions.producer = 800;
    CHECK(tick(1) == 0, "first tick");
    for (unsigned k = 2; k <= 10; k++) {
        unsigned long old_fence = positions.producer;
        positions.producer += 1600;
        /* Loop work reaches the previous fence, but always yields on newer
         * traffic. The ring is NEVER empty, including during every tick. */
        positions.consumer = old_fence - 8;
        CHECK(consume_event_ring(&d, "host loop", true), "bounded loop yielded");
        CHECK(positions.consumer >= old_fence && positions.consumer < positions.producer,
              "loop crossed the old fence with sustained backlog");
        CHECK(tick(k) == 0, "tick");
        CHECK(written_w == k * SEC, "T1 W advances every tick despite backlog");
        CHECK(wall - written_w <= 2 * SEC, "T1 lag at most two ticks");
    }
}
static void T2_oldest_pending(void)
{
    reset(); positions.producer = 800;
    CHECK(tick(1) == 0, "first tick");
    for (unsigned k = 2; k <= 5; k++) {
        positions.producer += 800; positions.consumer = 792;
        CHECK(tick(k) == 0, "heartbeat tick");
        CHECK(written_w == SEC, "T2 W frozen below fence");
        CHECK(d.coverage.p_ns == 2 * SEC && d.coverage.pfd_ns == 2 * SEC - 10,
              "T2 oldest pending P and floors retained");
        CHECK(d.coverage.p_fence == 800, "T2 oldest fence retained");
    }
    CHECK(d.coverage.heartbeats == 5, "T2 initial plus N=4 heartbeats");
    positions.consumer = 800;
    CHECK(tick(6) == 0, "crossing tick");
    CHECK(written_w == 2 * SEC, "T2 crossing promotes OLDEST pending P");
    CHECK(d.coverage.fd_ns == 2 * SEC - 10, "T2 promoted floors belong to oldest P");
    CHECK(d.coverage.p_ns == 7 * SEC && d.coverage.p_fence == 4000,
          "T2 replacement only after promotion");
}
static void T3_busy_head(void)
{
    reset(); positions.producer = 800; positions.consumer = 792;
    CHECK(tick(1) == 0, "first tick");
    positions.producer = 1600; positions.busy = true;
    CHECK(!consume_event_ring(&d, "busy head", true), "busy consume returns nonnegative");
    CHECK(tick(2) == 0, "busy tick");
    CHECK(written_w == SEC, "T3 busy head prevents promotion despite committed tail");
    CHECK(d.coverage.p_fence == 800, "T3 pending fence unchanged");
    positions.busy = false;
    CHECK(consume_event_ring(&d, "released head", true), "bounded drain after release");
    CHECK(tick(3) == 0 && written_w == 2 * SEC, "T3 promotion after busy prefix settles");
    /* The final drain is subject to the identical busy-head proof. */
    positions.busy = true;
    wall = 5 * SEC;
    struct pgwt_cov_tick_in in = collect();
    CHECK(pgwt_cov_tracker_seal(&d.coverage, &writer, &in) == 0,
          "busy shutdown seal");
    CHECK(sealed_w == 2 * SEC, "T3 SEAL repeats last promoted W below pending fence");
}
static void T4_loss_and_failure(void)
{
    for (int fault = 0; fault < 6; fault++) {
        reset(); positions.producer = 800;
        CHECK(tick(1) == 0, "first tick");
        positions.consumer = 800;
        if (fault == 0) counters.components[PGWT_C_EVENT_RING_DROP].value++;
        if (fault == 1) counters.components[PGWT_C_WAIT_READ_FAIL].valid = false;
        if (fault == 2) counters.components[PGWT_C_WRITER_PUSH_FAIL].value++;
        if (fault == 3) publish_ok = false;
        if (fault == 4) watermark_ok = false;
        if (fault == 5) {
            positions.consumer = 792; reject_callback = true;
            CHECK(!consume_event_ring(&d, "rejected callback", true), "error is not a budget yield");
            CHECK(positions.consumer == 800 && d.coverage.consume_failed,
                  "T4 consumer may pass a rejected callback; error remains sticky");
        }
        int rc = tick(2);
        CHECK((rc == 0) == (fault != 4), "T4 watermark failure returned");
        CHECK(d.coverage.w_ns == SEC && written_w == SEC,
              "T4 drop/unknown/rejected callback/publish/write failure blocks promotion");
        if (fault <= 2 || fault == 5) {
            CHECK(gaps == 1 && gap_start == SEC, "T4 gap opens at last written W");
            if (fault != 1 && fault != 5) CHECK(gap_end == 3 * SEC, "T4 loss gap is [W,P]");
        }
        CHECK(d.coverage.has_pending && d.coverage.p_ns == 2 * SEC,
              "T4 failure preserves oldest pending");
        /* Shutdown must not promote through the same failure. */
        wall = 4 * SEC;
        struct pgwt_cov_tick_in in = collect();
        if (fault == 0 || fault == 2)
            counters.components[PGWT_C_WRITER_PUSH_FAIL].value++;
        in.cv = &counters;
        rc = pgwt_cov_tracker_seal(&d.coverage, &writer, &in);
        CHECK(d.coverage.w_ns == SEC && (fault == 4 || sealed_w == SEC),
              "T4 shutdown failure cannot advance W_final");
        CHECK((rc == 0) == (fault != 4), "T4 shutdown write result");
    }
    /* Run the REAL daemon shutdown binding: a callback rejects an old
     * record after the final-drain counter snapshot but before SEAL. */
    reset(); positions.producer = 800; d.coverage_started = true;
    CHECK(tick(1) == 0, "first tick");
    d.final_drain_counters = counters; d.final_drain_from_ns = 3 * SEC;
    positions.consumer = 792; fail_push = true;
    CHECK(!consume_event_ring(&d, "final rejected push", true), "final drain settled");
    wall = 4 * SEC;
    pgwt_coverage_shutdown(&d);
    CHECK(d.coverage.w_ns == SEC && sealed_w == SEC,
          "T4 shutdown rereads final callback failures before promoting");
    CHECK(gaps == 1 && gap_start == SEC && gap_end == 4 * SEC,
          "T4 final callback gap uses counters before the shutdown P");
    /* A ring expected by the mode must remain fail closed. */
    reset(); ring_missing = true;
    CHECK(tick(1) == 0 && !d.coverage.has_pending && written_w == SEC,
          "T4 failed ring lookup means no completion certificate");
    reset(); d.event_rb = NULL;
    CHECK(tick(1) == 0 && !d.coverage.has_pending && written_w == SEC,
          "T4 no ring means no completion certificate");
}
static void T7_sampled_progress(void)
{
    reset(); d.mode = PGWT_MODE_SAMPLED; d.event_rb = NULL;
    struct pgwt_cov_tick_in in = collect();
    CHECK(in.positions_known && in.producer_pos == 0 && in.consumer_pos == 0,
          "T7 sampled empty fence is known and crossed");
    CHECK(tick(1) == 0 && d.coverage.has_pending, "T7 sampled takes pending P");
    CHECK(tick(2) == 0 && written_w == 2 * SEC,
          "T7 sampled W advances beyond boot_start");
    CHECK(tick(3) == 0 && written_w == 3 * SEC, "T7 sampled keeps advancing");
    wall = 5 * SEC; in = collect();
    CHECK(pgwt_cov_tracker_seal(&d.coverage, &writer, &in) == 0 && sealed_w == 4 * SEC,
          "T7 sampled shutdown promotes pending P");
    CHECK(consumes == 0, "T7 sampled has no event ring to consume");
    reset(); d.mode = PGWT_MODE_SAMPLED; d.event_rb = NULL; d.exact_perf = true;
    CHECK(tick(1) == 0 && !d.coverage.has_pending && written_w == SEC,
          "T7 exact-perf does not use sampled empty-ring certificate");
}
static void T8_first_watermark_failure(void)
{
    reset(); watermark_ok = false;
    CHECK(tick(1) == -1 && written_w == SEC, "T8 first watermark write fails");
    CHECK(!d.coverage.has_pending && d.coverage.p_ns == 0,
          "T8 first failed watermark rolls back has_pending");
    watermark_ok = true;
    CHECK(tick(2) == 0 && written_w == SEC && d.coverage.p_ns == 3 * SEC,
          "T8 retry takes its own pending point");
    CHECK(tick(3) == 0 && written_w == 3 * SEC && d.coverage.fd_ns == 3 * SEC - 10,
          "T8 retry promotes its own point and floors");
}
static void T5_no_consume(void)
{
    reset(); positions.producer = 800;
    for (unsigned k = 1; k <= 4; k++) CHECK(tick(k) == 0, "tick");
    CHECK(consumes == 0 && callbacks == 0, "T5 coverage tick makes ZERO consume calls");
    CHECK(publishes == 4 && watermarks == 4, "T5 heartbeat still publishes every tick");
}
static void T6_slow_callbacks(void)
{
    const uint64_t costs[] = {1, 20000000};
    for (unsigned i = 0; i < sizeof(costs) / sizeof(costs[0]); i++) {
        reset(); callback_cost = costs[i]; positions.producer = 1600;
        CHECK(tick(1) == 0, "first tick");
        uint64_t before = wall;
        CHECK(consume_event_ring(&d, "host slow loop", true), "loop yields");
        uint64_t loop_cost = wall - before;
        CHECK(loop_cost == 64 * callback_cost, "controlled callback cost in loop");
        before = wall; unsigned before_callbacks = callbacks, before_consumes = consumes;
        struct pgwt_cov_tick_in in = collect();
        CHECK(pgwt_cov_tracker_tick(&d.coverage, &writer, &in) == 0, "tick");
        printf("T6 callback_ns=%llu loop_ns=%llu tick_ns=%llu callbacks_in_tick=%u\n",
               (unsigned long long)callback_cost, (unsigned long long)loop_cost,
               (unsigned long long)(wall - before), callbacks - before_callbacks);
        CHECK(wall == before && callbacks == before_callbacks && consumes == before_consumes,
              "T6 tick cost independent of slow callback cost");
        CHECK(written_w == SEC, "T6 incomplete fence still heartbeats");
    }
}
int main(int argc, char **argv)
{
    assert(unsetenv("PGWT_TEST_COVERAGE_YIELD") == 0);
    const char *which = argc > 1 ? argv[1] : "all";
#define RUN(n, fn) if (!strcmp(which, "all") || !strcmp(which, n)) { \
    int before = failures; fn(); printf("%s: %s\n", n, before == failures ? "PASS" : "FAIL"); }
    RUN("T1", T1_sustained_backlog);
#ifndef BASELINE
    RUN("T2", T2_oldest_pending);
    RUN("T3", T3_busy_head);
    RUN("T4", T4_loss_and_failure);
    RUN("T5", T5_no_consume);
    RUN("T6", T6_slow_callbacks);
    RUN("T7", T7_sampled_progress);
    RUN("T8", T8_first_watermark_failure);
#endif
    return failures ? 1 : 0;
}
'''


def build_run(root, tmp, name, snapshot, daemon, case="all", baseline=False):
    definitions = "\n".join(re.findall(
        r"^#define PGWT_(?:EVENT_DRAIN_CALLBACK_BUDGET|DEBUG_BLOCK_NS|OPEN_SNAPSHOT_DRAIN_BUDGET) .*$",
        daemon + "\n" + snapshot, re.M))
    names = ("cov_thread_cpu_ns", "cov_test_force_yield", "cov_drain_publish") if baseline else (
        "cov_test_force_yield", "cov_positions", "cov_publish", "cov_clock", "pgwt_coverage_shutdown")
    bodies = [function(daemon, "pgwt_consume_event_ring"), function(daemon, "consume_event_ring")]
    read_counters = re.search(r"^static struct pgwt_counter_vector cov_read_counters\([^;]*?\)\n\{.*?^\}",
                              snapshot, re.M | re.S)
    assert read_counters
    bodies.append(read_counters[0])
    bodies += [function(snapshot, fn) for fn in names]
    ops = "cov_read_counters, tick_clock, cov_drain_publish" if baseline else (
        "cov_read_counters, tick_clock, cov_positions, cov_publish")
    harness = HARNESS.replace("@DEFINES@", definitions).replace(
        "@FUNCTIONS@", "\n\n".join(bodies)).replace("@OPS@", ops)
    if baseline:
        # T1 has no knowledge of the new tracker representation: run exactly
        # that test against the original tracker and daemon collection body.
        a, b = harness.index("static void T2_oldest_pending"), harness.index("int main(")
        harness = harness[:a] + harness[b:]
    source, binary = tmp / (name + ".c"), tmp / name
    source.write_text(harness)
    implementation = tmp / (name + "-snapshot.c")
    implementation.write_text(snapshot)
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
                    "-Wall", "-Wextra", "-Werror", "-DPGWT_SERVER",
                    *(["-DBASELINE"] if baseline else []),
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    "-I" + str(root / "src"), "-I" + str(root / "include"),
                    str(source), str(implementation), "-o", str(binary)], check=True)
    return subprocess.run([str(binary), case], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutants", action="store_true")
    ap.add_argument("--case", default="all", choices=["all", *[f"T{i}" for i in range(1, 9)]])
    ap.add_argument("--source-root", type=Path, default=ROOT)
    args = ap.parse_args()
    root = args.source_root.resolve()
    daemon = (root / "src/daemon.c").read_text()
    snapshot = (root / "src/open_snapshot.c").read_text()
    baseline = "static void cov_positions(" not in snapshot
    with tempfile.TemporaryDirectory(prefix="pgwt-coverage-fence-") as tmp:
        tmp = Path(tmp)
        result = build_run(root, tmp, "base", snapshot, daemon,
                           "T1" if baseline else args.case, baseline)
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
        if args.mutants:
            mutants = [
                ("sampled-unknown-positions", "T7", "T7 sampled W advances beyond boot_start",
                 "        in->positions_known = true;", "        in->positions_known = false;"),
                ("drop-has-pending-rollback", "T8", "T8 first failed watermark rolls back has_pending",
                 "        t->has_pending = before.has_pending;", "        /* rollback removed */"),
                ("drop-fence", "T2", "T2 W frozen below fence",
                 "in->consumer_pos >= t->p_fence", "true"),
                ("newer-fence", "T1", "T1 W advances every tick despite backlog",
                 "in->consumer_pos >= t->p_fence", "in->consumer_pos >= in->producer_pos"),
                ("consume-nonnegative", "T3", "T3 busy head prevents promotion",
                 "        in->consumer_pos = ring__consumer_pos(ring);",
                 "        in->consumer_pos = ring__consumer_pos(ring);\n"
                 "        if (pgwt_consume_event_ring(d, true) >= 0)\n"
                 "            in->consumer_pos = in->producer_pos;"),
                ("tick-consumes", "T5", "T5 coverage tick makes ZERO consume calls",
                 "    in->positions_known = ring != NULL;",
                 "    (void)pgwt_consume_event_ring(d, true);\n    in->positions_known = ring != NULL;"),
                ("newest-pending", "T2", "T2 crossing promotes OLDEST pending P",
                 "            cov_take_pending(t);",
                 "            { cov_set_pending(t, in); cov_take_pending(t); }"),
                ("frozen-shutdown-counters", "T4", "T4 shutdown rereads final callback failures",
                 "    pgwt_cov_collect_tick(&in, &cv, d, &ops);\nseal:\n    if (pgwt_cov_tracker_seal",
                 "    pgwt_cov_collect_tick(&in, &cv, d, &ops);\n"
                 "seal:\n    cv = d->final_drain_counters;\n    if (pgwt_cov_tracker_seal"),
            ]
            for name, case, intended, old, new in mutants:
                assert snapshot.count(old) == 1, (name, snapshot.count(old))
                result = build_run(root, tmp, name, snapshot.replace(old, new), daemon, case)
                assert result.returncode != 0 and intended in result.stderr, (
                    name, result.returncode, result.stdout, result.stderr)
                print(f"MUTANT {name}: RED on {case}: {intended}", flush=True)
    print("test_coverage_drain: PASS (production fence tracker, T1-T8)")


if __name__ == "__main__":
    main()
