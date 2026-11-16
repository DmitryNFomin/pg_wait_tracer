#!/usr/bin/env python3
"""Host controls for unchanged production EXIT bodies and churn assertions.

No kernel watchpoints: map I/O and retirement side effects are injected.
Mutants must build, reach the missing count and fail its intended assertion.
"""
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_backend_init_order import ROOT, function
import test_init_hint_recovery as recovery


HARNESS = r'''
#include "daemon.h"
#include "backend_init.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct pg_wait_tracer_bpf {
    struct { void *state_map, *exact_attr_map, *exact_seed_map; } maps;
};
static struct pgwt_daemon d;
static struct pgwt_pid_state state;
static int have_state, read_error, reads, deletes, closes, forgets, qexits;
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", msg); exit(1); } } while (0)
uint64_t pgwt_debug_monotonic_ns(void) { return 300; }
void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t ns)
{ (void)op; (void)pid; (void)ns; }
static int bpf_map__fd(void *map)
{ return map == d.skel->maps.state_map ? 1 : map == d.skel->maps.exact_attr_map ? 2 : 3; }
static int bpf_map_lookup_elem(int fd, const void *key, void *out)
{
    CHECK(fd == 1 && *(const uint32_t *)key == 42, "read state identity");
    ++reads;
    if (read_error || !have_state) { errno = read_error ? read_error : ENOENT; return -1; }
    *(struct pgwt_pid_state *)out = state;
    return 0;
}
static int bpf_map_delete_elem(int fd, const void *key)
{
    CHECK(*(const uint32_t *)key == 42, "delete state identity");
    if (fd == 1) { have_state = 0; ++deletes; }
    return 0;
}
static void pgwt_close_watchpoint(int fd)
{ if (fd >= 0) ++closes; }
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    return bt->count && bt->entries[0].pid == pid ? &bt->entries[0] : NULL;
}
void pgwt_summary_forget_open(struct pgwt_summary_writer *w, uint32_t pid)
{ (void)w; CHECK(pid == 42, "summary identity"); ++forgets; }
void pgwt_live_qattr_pid_exit(struct pgwt_accumulator *acc, uint32_t pid)
{ (void)acc; CHECK(pid == 42, "query identity"); ++qexits; }
const char *pgwt_backend_type_name(enum pgwt_backend_type bt)
{ (void)bt; return "client"; }
@FUNCTIONS@
static void reset(int registered)
{
    static struct pg_wait_tracer_bpf skel;
    skel.maps.state_map = (void *)1;
    skel.maps.exact_attr_map = (void *)2;
    skel.maps.exact_seed_map = (void *)3;
    memset(&d, 0, sizeof(d));
    d.skel = &skel; d.mode = PGWT_MODE_FULL; d.verbose = true;
    d.summary_writer = (void *)1;
    d.init_last_sweep_start_ns = 200;
    d.backends.count = registered;
    d.backends.entries[0] = (struct pgwt_backend){
        .pid=42, .is_alive=true, .wp_fd=-1, .bootstrap_fd=88};
    if (registered) pgwt_pending_init_add(&d.backends, &d.backends.entries[0]);
    state = (struct pgwt_pid_state){0};
    have_state = 1;
    read_error = reads = deletes = closes = forgets = qexits = 0;
}
static void retire(void)
{ CHECK(pgwt_handle_exit_at(&d, 42, 300) == 0, "EXIT handled"); }
int main(void)
{
    /* The measured second case: a state entry, bootstrap attached, no INIT. */
    reset(1); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 1, "retirement counted once");
    CHECK(d.counters.lifecycle_init_lost_total == 0 &&
          d.counters.lifecycle_init_lost_untraced_ns_total == 0 &&
          d.counters.lifecycle_init_lost_no_opportunity_total == 0,
          "unknown INIT invents no timestamp or opportunity");
    CHECK(!have_state && d.backends.entries[0].pid == 0 &&
          d.backends.pending_init_count == 0 && closes == 1 &&
          forgets == 1 && qexits == 1, "retirement cleans all state");
    retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 1 &&
          d.counters.lifecycle_init_lost_total == 0, "duplicate retirement counts once");

    /* BPF may enroll before the userspace registry; deletion still deduplicates. */
    reset(0); retire(); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 1,
          "state-only retirement counted once");

    reset(1); d.backends.entries[0].wp_attach_exit_race = true;
    d.counters.wp_attach_exit_race_total = 1;
    retire(); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 0 &&
          d.counters.lifecycle_init_lost_total == 0 &&
          d.counters.wp_attach_exit_race_total == 1, "exit race has one outcome");

    /* Real INIT evidence retains its existing loss accounting. */
    for (int no_opportunity = 0; no_opportunity <= 1; ++no_opportunity) {
        reset(1); state.init_stored = 1; state.init_ts = 200 + no_opportunity;
        retire(); retire();
        CHECK(d.counters.lifecycle_exit_before_attach_total == 0 &&
              d.counters.lifecycle_init_lost_total == 1 &&
              d.counters.lifecycle_init_lost_no_opportunity_total == (uint64_t)no_opportunity &&
              d.counters.lifecycle_init_lost_with_opportunity_total == (uint64_t)!no_opportunity,
              "INIT evidence preserves loss partition");
    }
    reset(1); d.backends.entries[0].wp_attach_exit_race = true;
    state.init_stored = 1; state.init_ts = 199;
    retire();
    CHECK(d.counters.lifecycle_init_lost_with_opportunity_total == 1 &&
          d.counters.lifecycle_exit_before_attach_total == 0,
          "exit proof cannot erase retained INIT evidence");
    reset(1); d.backends.entries[0].init_ts = 199;
    retire();
    CHECK(d.counters.lifecycle_init_lost_total == 1 &&
          d.counters.lifecycle_exit_before_attach_total == 0, "delivered INIT evidence preserved");
    reset(1); d.backends.entries[0].init_attached = true;
    d.backends.entries[0].wp_fd = 99;
    retire();
    CHECK(d.counters.lifecycle_init_lost_total == 0 &&
          d.counters.lifecycle_exit_before_attach_total == 0, "attached backend is not lost");
    reset(1); read_error = EIO;
    retire();
    CHECK(d.counters.init_state_read_fail_total == 1 &&
          d.counters.lifecycle_exit_before_attach_total == 0, "read error cannot prove missing INIT");
    reset(0); have_state = 0;
    retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 0, "missing state is not a new retirement");
    reset(1); have_state = 0;
    retire(); retire();
    CHECK(d.counters.lifecycle_init_lost_with_opportunity_total == 1 &&
          d.counters.lifecycle_exit_before_attach_total == 0, "missing state preserves old loss");
    reset(1); d.mode = PGWT_MODE_SAMPLED;
    retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 0, "sampled capture needs no watchpoint");
    puts("retirement counts, duplicate cleanup, evidence, read errors and sampled controls: PASS");
    return 0;
}
'''


def check_cohort_controls():
    before = dict.fromkeys(("lifecycle_init_lost_no_opportunity_total",
                           "wp_attach_exit_race_total", recovery.EXIT_BEFORE_ATTACH), 0)
    after = dict.fromkeys(before, 1)
    text = "\n".join((
        "INFO: PID 41 initialized (client), real watchpoint at 0x1234",
        "INIT-LOST: clock=CLOCK_MONOTONIC pid=42 init_ts=201 no_opportunity=1 exit_ts_source=record",
        "WP-ATTACH-FAIL: pid=43 site=bootstrap_watchpoint_open errno=3 resolve_errno=3 "
        "liveness=exiting liveness_result=1 liveness_errno=0 exit_proof=2",
        "EXIT-BEFORE-ATTACH: pid=44 exited before real attach; INIT/opportunity unknown"))
    pids = {41, 42, 43, 44}
    recovery.check_cohort(text, pids, {43, 44}, before, after)

    def reject(name, changed_text=text, changed_after=after, short={43, 44}, expected=""):
        try:
            recovery.check_cohort(changed_text, pids, short, before, changed_after)
        except recovery.CohortFailure as error:
            assert expected in str(error), (name, error)
            assert error.pids, (name, error)
        else:
            raise AssertionError(f"churn negative control survived: {name}")
        print(f"churn control {name}: rejected by {expected}")

    for name in before:
        reject("missing " + name, changed_after={**after, name: 0},
               expected="delta does not match PID diagnostics")
    reject("ESRCH alone", text.replace("liveness=exiting", "liveness=live"),
           expected="backend has no counted outcome")
    reject("unknown exit proof", text.replace("exit_proof=2", "exit_proof=-1"),
           expected="backend has no counted outcome")
    reject("other errno", text.replace("errno=3 resolve_errno=3", "errno=5 resolve_errno=3"),
           expected="backend has no counted outcome")
    reject("missing retirement line", text.replace("EXIT-BEFORE-ATTACH:", "unrecognized:"),
           expected="backend has no counted outcome")
    reject("long exit race", short={44}, expected="outlived a tick")
    reject("long retirement", short={43}, expected="outlived a tick")
    reject("overlap", text + "\nINFO: PID 44 initialized (client)",
           expected="overlapping retirement classes")
    reject("duplicate attachment", text + "\nINFO: PID 41 initialized (client)",
           expected="duplicate outcome diagnostic")
    reject("duplicate retirement", text + "\nEXIT-BEFORE-ATTACH: pid=44 duplicate",
           expected="duplicate outcome diagnostic")
    reject("uncounted extra PID", text + "\nEXIT-BEFORE-ATTACH: pid=45 extra",
           expected="delta does not match PID diagnostics")
    # Counter deltas also work with preexisting non-cohort diagnostic records.
    baseline_text = text.replace("PID 41", "PID 51").replace("pid=42", "pid=52").replace(
        "pid=43", "pid=53").replace("pid=44", "pid=54")
    recovery.check_cohort(baseline_text + "\n" + text, pids, {43, 44},
                          {name: 1 for name in before}, {name: 2 for name in before})
    output = io.StringIO()
    extra = text + "\nINFO: fork detected PID 44, bootstrap watchpoint attached\nINFO: PID 43 exited (client)"
    with redirect_stdout(output):
        recovery.print_failure_diagnostics(extra, None, {43, 44})
    for line in extra.splitlines():
        if "pid=43" in line or "pid=44" in line or "PID 43" in line or "PID 44" in line:
            assert line in output.getvalue(), line
    print("churn failing-PID diagnostics include every matching daemon line: PASS")


def main():
    backend = (ROOT / "src/backend.c").read_text()
    functions = "\n\n".join(function(backend, name) for name in (
        "read_init_state", "record_init_loss", "pgwt_handle_exit_at"))
    needle = "d->counters.lifecycle_exit_before_attach_total++;"
    assert functions.count(needle) == 1
    builds = [("retirement", functions, None),
              ("missing_retirement_count", functions.replace(needle, ";"),
               "FAIL retirement counted once")]
    common = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
              "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
              "-I" + str(ROOT / "include"), *shlex.split(os.environ.get("CFLAGS", ""))]
    with tempfile.TemporaryDirectory(prefix="pgwt-exit-accounting-") as tmp:
        for name, source, intended in builds:
            cfile, binary = Path(tmp) / f"{name}.c", Path(tmp) / name
            cfile.write_text(HARNESS.replace("@FUNCTIONS@", source))
            subprocess.run([*common, str(cfile), str(ROOT / "src/backend_init.c"),
                            "-o", str(binary)], check=True)
            print(f"{name}: portable C build PASS", flush=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if intended:
                assert result.returncode == 1 and intended in result.stderr, result
                print(f"{name}: KILLED (built, reached intended assertion: {intended})")
            else:
                assert result.returncode == 0, result
                assert result.stderr.count("EXIT-BEFORE-ATTACH: pid=42 ") == 2, result
                print(result.stdout, end="")
        # The changed registered C units run on the Mac through this host tier.
        for name, implementation in (("test_cmdline", "cmdline"),
                                     ("test_backend_init", "backend_init")):
            binary = Path(tmp) / name
            subprocess.run([*common, str(ROOT / f"tests/{name}.c"),
                            str(ROOT / f"src/{implementation}.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    check_cohort_controls()
    print("test_backend_exit_accounting: PASS")


if __name__ == "__main__":
    main()
