#!/usr/bin/env python3
"""Compile the production INIT callback/sweep/accounting with fake I/O.

The extracted functions are compiled unchanged; only libbpf consumption,
record processing, map reads, attachment and the clock are fakes. This lets
Mac and Linux run the same per-record order case without BPF. --mutants
requires a build and the intended assertion failure, never a compiler error.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|void) " + name +
                      r"\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
    assert match, f"missing production function {name}"
    return match[0]


HARNESS = r'''
#include "daemon.h"
#include "backend_init.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pgwt_daemon d;
static struct pgwt_pid_state stored;
static uint64_t now = 100;
static int callbacks, reads, attaches, publish_in_query;
static int query_attached, fork_attached;
uint64_t pgwt_debug_monotonic_ns(void) { return now++; }
static int process_lifecycle_event(void *, void *, size_t);
static int ring_buffer__consume(struct ring_buffer *);
static int read_pending_init_state(void *, pid_t, struct pgwt_pid_state *);
static int attach_stored_init(void *, pid_t, uint64_t, uint64_t);

@FUNCTIONS@

static int read_pending_init_state(void *ctx, pid_t pid,
                                    struct pgwt_pid_state *out)
{
    assert(ctx == &d && pid == 42);
    assert(d.init_sweep_active);
    ++reads;
    *out = stored;
    return 0;
}
static int attach_stored_init(void *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{
    assert(ctx == &d && pid == 42 && addr == 1234 && ts == 90);
    ++attaches;
    /* Attachment must not recursively consume records or restart a sweep. */
    assert(pgwt_consume_lifecycle_ring(&d) == -EDEADLK);
    assert(pgwt_sweep_bootstrap_backends(&d) == 0);
    struct pgwt_backend *be = &d.backends.entries[0];
    be->init_attached = true;
    be->init_ts = ts;
    be->bootstrap_fd = -1;
    be->wp_fd = 99;
    pgwt_pending_init_remove(&d.backends, be);
    return 0;
}
static int process_lifecycle_event(void *ctx, void *data, size_t size)
{
    assert(ctx == &d && size == sizeof(struct pgwt_lifecycle_event));
    assert(d.lifecycle_batch_start_ns == 100);
    assert(d.lifecycle_batch_records == (uint64_t)++callbacks);
    const struct pgwt_lifecycle_event *ev = data;
    int type = ev->type;
    if (type == PGWT_LIFECYCLE_QUERY_TEXT) {
        if (publish_in_query)
            stored.init_stored = 1; /* becomes visible during QUERY_TEXT */
        query_attached = d.backends.entries[0].init_attached;
    } else if (type == PGWT_LIFECYCLE_FORK) {
        fork_attached = d.backends.entries[0].init_attached;
    } else {
        assert(type == PGWT_LIFECYCLE_EXIT);
        assert(d.lifecycle_exit_pid == 42 && d.lifecycle_exit_ts == 120);
        /* Intended mutant target: post-batch/tick recovery is too late. */
        if (!d.backends.entries[0].init_attached) {
            fputs("FAIL per-record attachment before EXIT\n", stderr);
            return -1;
        }
        d.backends.entries[0].is_alive = false;
    }
    return 0;
}
static int ring_buffer__consume(struct ring_buffer *rb)
{
    (void)rb;
    /* INIT is stored without a delivered hint, then QUERY_TEXT, FORK, EXIT
     * for this PID are all handled inside ONE consume call. */
    stored.init_stored = !publish_in_query;
    int batch[] = {PGWT_LIFECYCLE_QUERY_TEXT, PGWT_LIFECYCLE_FORK,
                   PGWT_LIFECYCLE_EXIT};
    for (int i = 0; i < 3; ++i) {
        struct pgwt_lifecycle_event ev = {.type = batch[i], .pid = 42,
                                          .timestamp = 120};
        if (handle_lifecycle_event(&d, &ev, sizeof(ev)) != 0)
            return -1;
        assert(d.lifecycle_exit_pid == 0 && d.lifecycle_exit_ts == 0);
    }
    return 3;
}
static void reset(void)
{
    memset(&d, 0, sizeof(d));
    stored = (struct pgwt_pid_state){.init_wait_addr = 1234, .init_ts = 90};
    callbacks = reads = attaches = query_attached = fork_attached = 0;
    now = 100;
    d.backends.count = 1;
    d.backends.entries[0] = (struct pgwt_backend){
        .pid = 42, .is_alive = true, .bootstrap_fd = 88, .wp_fd = -1};
    pgwt_pending_init_add(&d.backends, &d.backends.entries[0]);
}
static void check_loss(pid_t pid, uint64_t init_ts, uint64_t exit_ts,
                       int expected_with, int expected_without)
{
    uint64_t total = d.counters.lifecycle_init_lost_total;
    uint64_t with = d.counters.lifecycle_init_lost_with_opportunity_total;
    uint64_t without = d.counters.lifecycle_init_lost_no_opportunity_total;
    record_init_loss(&d, pid, init_ts, exit_ts);
    if (d.counters.lifecycle_init_lost_total != total + 1 ||
        d.counters.lifecycle_init_lost_with_opportunity_total != with + expected_with ||
        d.counters.lifecycle_init_lost_no_opportunity_total != without + expected_without ||
        d.counters.lifecycle_init_lost_total !=
            d.counters.lifecycle_init_lost_with_opportunity_total +
            d.counters.lifecycle_init_lost_no_opportunity_total) {
        fputs("FAIL loss partition\n", stderr);
        exit(1);
    }
}
int main(void)
{
    for (publish_in_query = 0; publish_in_query <= 1; ++publish_in_query) {
        reset();
        /* The callback is exercised without a preceding epoll/tick sweep. */
        int rc = pgwt_consume_lifecycle_ring(&d);
        if (rc != 3 || attaches != 1) {
            fputs("FAIL per-record attachment before EXIT\n", stderr);
            return 1;
        }
        assert(callbacks == 3 && d.backends.pending_init_count == 0);
        assert(query_attached == !publish_in_query && fork_attached);
        assert(d.lifecycle_batch_start_ns == 0 && d.lifecycle_batch_records == 0);
        assert(d.init_last_sweep_start_ns > 100 && !d.init_sweep_active);
        assert(reads == (publish_in_query ? 3 : 2));
    }
    puts("per-record batch order and non-recursion: PASS");

    reset();
    d.init_last_sweep_start_ns = 200;
    d.lifecycle_batch_start_ns = 250;
    d.lifecycle_batch_records = 4;
    d.last_epoll_return_ns = 240;
    now = 400;
    /* Equal and before are NOT no-opportunity. Only strictly after is. */
    check_loss(42, 200, 300, 1, 0);
    check_loss(43, 199, 300, 1, 0);
    check_loss(44, 201, 300, 0, 1);
    if (d.counters.lifecycle_init_lost_total != 3 ||
        d.counters.lifecycle_init_lost_with_opportunity_total != 2 ||
        d.counters.lifecycle_init_lost_no_opportunity_total != 1 ||
        d.counters.lifecycle_init_lost_untraced_ns_total != 99) {
        fputs("FAIL strict no-opportunity boundary and duration\n", stderr);
        return 1;
    }
    /* Sum across multiple losses; don't replace the previous duration. */
    check_loss(45, 220, 350, 0, 1);
    assert(d.counters.lifecycle_init_lost_total == 4);
    assert(d.counters.lifecycle_init_lost_with_opportunity_total == 2);
    assert(d.counters.lifecycle_init_lost_no_opportunity_total == 2);
    assert(d.counters.lifecycle_init_lost_untraced_ns_total == 229);
    puts("loss split equal/before/after and summed untraced ns: PASS");
    /* Missing INIT timestamp cannot claim the owner exclusion. */
    check_loss(46, 0, 350, 1, 0);
    assert(d.counters.lifecycle_init_lost_untraced_ns_total == 229);
    puts("every loss in exactly one opportunity class: PASS");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutants", action="store_true")
    args = parser.parse_args()
    daemon = (ROOT / "src/daemon.c").read_text()
    backend = (ROOT / "src/backend.c").read_text()
    # Verify the production epoll hook precedes ALL fd handling. The clock
    # read is unconditional (debug mode must not decide the loss split).
    loop = function(daemon, "pgwt_daemon_run")
    wake = loop.index("int n = epoll_wait(")
    fd_loop = loop.index("for (int i = 0; i < n; i++)", wake)
    assert wake < loop.index("d->last_epoll_return_ns = pgwt_debug_monotonic_ns();", wake)
    assert loop.index("pgwt_sweep_bootstrap_backends(d);", wake) < fd_loop
    assert "pgwt_handle_exit_at(d, ev->pid, ev->timestamp);" in function(
        daemon, "process_lifecycle_event")
    for name in ("lifecycle_init_lost_total",
                 "lifecycle_init_lost_with_opportunity_total",
                 "lifecycle_init_lost_no_opportunity_total",
                 "lifecycle_init_lost_untraced_ns_total"):
        assert f'cjson_add_uint64(root, "{name}",' in (ROOT / "src/control.c").read_text()
    callback = function(daemon, "handle_lifecycle_event")
    functions = "\n".join((function(backend, "pgwt_sweep_bootstrap_backends"),
                            function(daemon, "pgwt_consume_lifecycle_ring"),
                            callback, function(backend, "record_init_loss")))
    variants = [("baseline", functions, None)]
    if args.mutants:
        # Remove ONLY the per-record sweep; epoll, batch and tick remain.
        assert callback.count("    pgwt_sweep_bootstrap_backends(d);\n") == 1
        removed = callback.replace("    pgwt_sweep_bootstrap_backends(d);\n", "")
        variants.append(("remove_per_record_sweep", functions.replace(callback, removed),
                         "FAIL per-record attachment before EXIT"))
        assert functions.count("init_ts > d->init_last_sweep_start_ns") == 1
        variants.append(("boundary_includes_equal", functions.replace(
            "init_ts > d->init_last_sweep_start_ns",
            "init_ts >= d->init_last_sweep_start_ns"),
            "FAIL loss partition"))
        counted = "d->counters.lifecycle_init_lost_with_opportunity_total++;"
        assert functions.count(counted) == 1
        variants.append(("omit_with_opportunity_count", functions.replace(counted, ""),
                         "FAIL loss partition"))
    with tempfile.TemporaryDirectory(prefix="pgwt-init-order-") as tmp:
        for name, source, intended in variants:
            harness = HARNESS.replace("@FUNCTIONS@", source)
            cfile, binary = Path(tmp) / f"{name}.c", Path(tmp) / name
            cfile.write_text(harness)
            command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                       "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                       "-I" + str(ROOT / "include"), str(cfile),
                       str(ROOT / "src/backend_init.c"), "-o", str(binary)]
            subprocess.run(command, check=True)
            print(f"{name}: portable C build PASS", flush=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if intended:
                assert result.returncode == 1 and intended in result.stderr, result
                print(f"{name}: KILLED (built, reached intended assertion)")
            else:
                print(result.stdout, end="")
                print(result.stderr, end="")
                assert result.returncode == 0, result
                assert ("INIT-LOST: clock=CLOCK_MONOTONIC pid=44 init_ts=201 exit_ts=300 "
                        "last_sweep_start_ns=200 batch_start_ns=250 batch_records=4 "
                        "since_epoll_return_ns=162 no_opportunity=1") in result.stderr
    print("test_backend_init_order: PASS")


if __name__ == "__main__":
    main()
