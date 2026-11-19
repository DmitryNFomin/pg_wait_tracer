#!/usr/bin/env python3
"""Host regression for the coverage drain's event-loop bound (Refs #947).

Compile the unchanged production drain bodies and real coverage tracker.
Only ring I/O, publication and clocks are fakes: each callback advances the
controlled wall clock by 20 ms, as in the live capture-smoke slow hook. This
makes the wall-time assertion deterministic even on a busy host; the live
capture-smoke phase remains the proof of display-timer progress on Linux.
"""
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
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

@DEFINES@
static struct pgwt_daemon d;
static struct pgwt_event_writer writer;
static struct pgwt_counter_vector counters;
static uint64_t wall = 1000000000ULL, written_w;
static unsigned remaining, callbacks, publishes, watermarks;
static int failures;
#define CHECK(ok, message) do { if (!(ok)) { \
    fprintf(stderr, "FAIL: %s\n", message); failures++; } } while (0)

uint64_t pgwt_debug_monotonic_ns(void) { return wall; }
static uint64_t now_ns(void) { return wall; }
int pgwt_consume_event_ring(struct pgwt_daemon *daemon, bool bounded)
{
    assert(daemon == &d && bounded);
    while (remaining) {
        remaining--;
        callbacks++;
        wall += 20000000ULL;
        d.event_drain_callbacks_current++;
        if (d.event_drain_callback_limit &&
            d.event_drain_callbacks_current >= d.event_drain_callback_limit)
            return -EAGAIN;
    }
    return 0;
}
int pgwt_writer_publish(struct pgwt_event_writer *w)
{ assert(w == &writer); publishes++; return 0; }
int pgwt_writer_coverage_watermark(struct pgwt_event_writer *w,
    uint64_t obs, uint64_t mark, uint64_t fd, uint64_t fi)
{
    assert(w == &writer && obs == wall && fi <= fd && fd <= mark);
    written_w = mark;
    watermarks++;
    return 0;
}
/* Not reached: known, unchanged counters, no seal and no floor population. */
int pgwt_writer_coverage_gap(struct pgwt_event_writer *w, uint64_t obs,
    bool open, uint64_t id, uint64_t at, uint8_t reason)
{ (void)w; (void)obs; (void)open; (void)id; (void)at; (void)reason; abort(); }
int pgwt_writer_coverage_seal(struct pgwt_event_writer *w, uint64_t obs,
    uint64_t mark)
{ (void)w; (void)obs; (void)mark; abort(); }
int pgwt_is_idle_event(uint32_t event) { (void)event; abort(); }

@FUNCTIONS@

static struct pgwt_counter_vector read_counters(void *ctx)
{ assert(ctx == &d); return counters; }
static uint64_t tick_clock(void *ctx) { assert(ctx == &d); return now_ns(); }
static bool tick(void)
{
    struct pgwt_cov_tick_in in = {0};
    struct pgwt_counter_vector cv;
    const struct pgwt_cov_tick_ops ops = {
        read_counters, tick_clock, cov_drain_publish,
    };
    pgwt_cov_collect_tick(&in, &cv, &d, &ops);
    assert(in.publish_ok);
    assert(pgwt_cov_tracker_tick(&d.coverage, &writer, &in) == 0);
    assert(d.event_drain_callback_limit == 0);
    return in.drain_complete;
}
int main(void)
{
    assert(unsetenv("PGWT_TEST_COVERAGE_YIELD") == 0);
    counters.valid = true;
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; i++)
        counters.components[i].valid = true;
    d.event_rb = (struct ring_buffer *)&d;
    d.event_writer = &writer;
    pgwt_cov_tracker_init(&d.coverage, wall, true, &counters);
    /* Establish a written W AND a later pending triple: advancing W on a
     * yield would now be observable, rather than hidden by boot_start. */
    wall += 1000000000ULL; assert(tick());
    wall += 1000000000ULL; assert(tick());
    uint64_t held_w = written_w;
    assert(held_w > d.coverage.boot_start_ns && d.coverage.has_pending);

    remaining = 1024; callbacks = 0;
    uint64_t start = wall;
    assert(consume_event_ring(&d, "host loop drain", true));
    unsigned loop_callbacks = callbacks;
    uint64_t loop_wall = wall - start;
    assert(loop_callbacks == 64 && loop_wall == 1280000000ULL);
    for (int i = 0; i < 2; i++) {
        remaining = 1024; callbacks = 0;
        start = wall;
        unsigned before_publish = publishes, before_wm = watermarks;
        bool complete = tick();
        printf("coverage slow drain: callbacks=%u wall_ms=%llu "
               "loop_bound_ms=%llu complete=%d W=%llu previous_W=%llu\n",
               callbacks, (unsigned long long)((wall - start) / 1000000),
               (unsigned long long)(loop_wall / 1000000), complete,
               (unsigned long long)written_w, (unsigned long long)held_w);
        CHECK(callbacks <= loop_callbacks && wall - start <= loop_wall,
              "coverage drain exceeded the event-loop wall bound");
        CHECK(!complete, "backlogged coverage drain must yield");
        CHECK(written_w == held_w, "yield must repeat W instead of advancing");
        CHECK(publishes == before_publish + 1 && watermarks == before_wm + 1,
              "yield must still publish and write one heartbeat");
    }
    remaining = 0;
    assert(tick() && written_w > held_w);
    CHECK(d.coverage.heartbeats == 3, "initial heartbeat plus both yields");
    if (failures) return 1;
    puts("test_coverage_drain: PASS (loop wall bound, yield heartbeats, recovery)");
    return 0;
}
'''


def main():
    daemon = (ROOT / "src/daemon.c").read_text()
    snapshot = (ROOT / "src/open_snapshot.c").read_text()
    # Read either location so this same regression also builds on 5d156f7b,
    # before the event-loop callback budget moved into the shared header.
    definitions = "\n".join(re.findall(
        r"^#define PGWT_(?:EVENT_DRAIN_CALLBACK_BUDGET|DEBUG_BLOCK_NS|OPEN_SNAPSHOT_DRAIN_BUDGET) .*$",
        daemon + "\n" + snapshot, re.M))
    bodies = [function(daemon, "consume_event_ring")]
    bodies += [function(snapshot, name) for name in (
        "cov_thread_cpu_ns", "cov_test_force_yield", "cov_drain_publish")]
    harness = HARNESS.replace("@DEFINES@", definitions).replace(
        "@FUNCTIONS@", "\n\n".join(bodies))
    with tempfile.TemporaryDirectory(prefix="pgwt-coverage-drain-") as tmp:
        source, binary = Path(tmp) / "drain.c", Path(tmp) / "drain"
        source.write_text(harness)
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
                        "-Wall", "-Wextra", "-Werror", "-DPGWT_SERVER",
                        *shlex.split(os.environ.get("CFLAGS", "")),
                        "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"),
                        str(source), str(ROOT / "src/open_snapshot.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
