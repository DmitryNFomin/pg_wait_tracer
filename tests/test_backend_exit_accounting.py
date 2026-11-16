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
from demo_rehearsal_lib import DAEMON_INTEGRITY_COUNTERS, daemon_integrity_ok


HARNESS = r'''
#include "daemon.h"
#include "backend_init.h"
#include "capture_counters.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct pg_wait_tracer_bpf {
    struct { void *state_map, *exact_attr_map, *exact_seed_map; } maps;
};
static struct pgwt_daemon d;
static struct pgwt_pid_state state;
static int have_state, read_error, reads, deletes, closes, forgets, qexits, target_live;
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
struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *ctx, uint32_t slot)
{ (void)ctx; return (struct pgwt_counter_value){slot == PGWT_BPF_FAIL_INIT_NO_STATE ? 7 : 11, true}; }
int pgwt_backend_target_live(pid_t pid, pid_t parent)
{ (void)pid; (void)parent; return target_live; }
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
        .pid=42, .is_alive=true, .wp_fd=-1, .bootstrap_fd=88,
        .bootstrap_armed=true, .post_arm_checked=true, .post_arm_ptr=0x1234,
        .post_arm_addr_shared=-1, .post_arm_resolve_errno=EACCES,
        .post_arm_shared_errno=EIO, .enroll_counters_valid=true,
        .enroll_init_no_state=2, .enroll_wait_read_fail=3};
    if (registered) pgwt_pending_init_add(&d.backends, &d.backends.entries[0]);
    state = (struct pgwt_pid_state){0};
    have_state = 1;
    read_error = reads = deletes = closes = forgets = qexits = 0;
    target_live = 0;
}
static void retire(void)
{ CHECK(pgwt_handle_exit_at(&d, 42, 300) == 0, "EXIT handled"); }
int main(void)
{
    /* The measured second case: a state entry, bootstrap attached, no INIT. */
    reset(1); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 1, "retirement counted once");
    CHECK(d.counters.lifecycle_exit_before_attach_with_opportunity_total == 1 &&
          d.counters.lifecycle_exit_before_attach_unknown_total == 0 &&
          d.counters.lifecycle_exit_before_attach_no_opportunity_total == 0,
          "entry retirement has observed opportunity");
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
    CHECK(d.counters.lifecycle_exit_before_attach_unknown_total == 1,
          "absent entry without first-handling proof is unknown");
    reset(0); CHECK(lifecycle_target(&d, 42) == 0, "first handling finds gone"); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_no_opportunity_total == 1 &&
          d.counters.lifecycle_exit_before_attach_total == 1,
          "positive first-handling gone proof reports no opportunity once");

    reset(0); remember_enrollment_refusal(&d, 42);
    CHECK(lifecycle_target(&d, 42) == 0, "allocation refusal then gone");
    CHECK(d.counters.lifecycle_exit_before_attach_unknown_total == 1 &&
          d.counters.lifecycle_exit_before_attach_no_opportunity_total == 0 &&
          d.lifecycle_refused_count == 0,
          "prior allocation refusal cannot become first-handling no opportunity");
    reset(0);
    remember_enrollment_refusal(&d, 42); remember_enrollment_refusal(&d, 42);
    CHECK(d.lifecycle_refused_count == 1, "refusal evidence is deduplicated");
    reset(0); d.lifecycle_refused_count = MAX_BACKENDS;
    remember_enrollment_refusal(&d, 42);
    CHECK(d.lifecycle_refused_overflow && d.lifecycle_refused_count == MAX_BACKENDS,
          "refusal evidence is bounded and overflow is sticky");
    CHECK(lifecycle_target(&d, 42) == 0 &&
          d.counters.lifecycle_exit_before_attach_unknown_total == 1,
          "refusal evidence overflow stays conservatively unknown");
    reset(1); d.backends.entries[0].wp_attach_exit_race = true;
    d.counters.wp_attach_exit_race_total = 1;
    retire(); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_with_opportunity_total == 1 &&
          d.counters.lifecycle_exit_before_attach_total == 1 &&
          d.counters.lifecycle_init_lost_total == 0 &&
          d.counters.wp_attach_exit_race_total == 1, "exit race reason does not erase opportunity");
    reset(1); d.backends.entries[0].wp_attach_failed = true;
    retire();
    CHECK(d.counters.lifecycle_exit_before_attach_total == 0,
          "real counted failure excludes no-INIT retirement class");

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
    reset(1); have_state = 0; d.backends.entries[0].bootstrap_fd = -1;
    d.backends.entries[0].wp_attach_exit_race = true;
    retire(); retire();
    CHECK(d.counters.lifecycle_exit_before_attach_with_opportunity_total == 1,
          "startup exit race without state still classifies once");
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


POINTER_HARNESS = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
struct pgwt_daemon { int use_myproc; uint64_t my_wait_ptr_addr; int pgproc_wait_offset; };
static int open_error, read_error, read_size, closes;
static uint64_t pointer_value;
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", msg); exit(1); } } while (0)
static int fake_open(const char *path, int flags)
{
    CHECK(strcmp(path, "/proc/42/mem") == 0 && flags == O_RDONLY, "pointer read path");
    if (open_error) { errno = open_error; return -1; }
    return 99;
}
static ssize_t fake_pread(int fd, void *out, size_t size, off_t offset)
{
    CHECK(fd == 99 && size == 8 && offset == 4096, "pointer read identity");
    if (read_error) { errno = read_error; return -1; }
    memcpy(out, &pointer_value, read_size);
    return read_size;
}
static int fake_close(int fd)
{ CHECK(fd == 99, "pointer close identity"); ++closes; errno = EBUSY; return 0; }
static uint64_t pgwt_debug_block_begin(struct pgwt_daemon *d) { (void)d; return 0; }
static void pgwt_debug_block_end(struct pgwt_daemon *d, const char *op, pid_t pid, uint64_t start)
{ (void)d; (void)op; (void)pid; (void)start; errno = ENOSYS; }
#define open fake_open
#define pread fake_pread
#define close fake_close
@FUNCTIONS@
int main(void)
{
    struct pgwt_daemon d = {.my_wait_ptr_addr=4096, .pgproc_wait_offset=684};
    for (d.use_myproc = 0; d.use_myproc <= 1; ++d.use_myproc) {
        for (int which = 0; which < 6; ++which) {
            open_error = which == 0 ? EACCES : 0;
            read_error = which == 1 ? ESRCH : 0;
            read_size = which == 2 ? 0 : which == 3 ? 4 : 8;
            pointer_value = which == 5 ? 8192 : 0;
            closes = 0; errno = EINVAL;
            uint64_t ptr = pgwt_resolve_backend_wait_addr(&d, 42);
            int error = errno;
            CHECK(ptr == (which == 5 ? 8192 + (d.use_myproc ? 684 : 0) : 0),
                  "pointer result unchanged");
            CHECK(error == (which == 0 ? EACCES : which == 1 ? ESRCH : which < 4 ? EIO : 0),
                  "pointer error survives close and debug; short reads are EIO");
            CHECK(closes == (which != 0), "pointer descriptor closed");
        }
    }
    puts("pointer open/read/short-read errors and unset pointer remain visible after debug: PASS");
    return 0;
}
'''


def check_pointer_diagnostics(common, tmp):
    discovery = (ROOT / "src/discovery.c").read_text()
    backend = (ROOT / "src/backend.c").read_text()
    control = (ROOT / "src/control.c").read_text()
    for counter in (recovery.EXIT_BEFORE_ATTACH, *recovery.EXIT_CLASSES.values()):
        assert f'cjson_add_uint64(root, "{counter}",' in control, counter
    functions = "\n\n".join((function(discovery, "pgwt_read_pointer"),
                               function(discovery, "pgwt_resolve_wait_addr_via_myproc"),
                               function(backend, "pgwt_resolve_backend_wait_addr")))
    variants = [("pointer_diagnostics", functions, None)]
    for name, old, new in (
        ("pointer_close_clobbers_errno", "errno = error;", "errno = error * 0;"),
        ("pointer_short_read_silent", "got != sizeof(val) ? EIO : 0", "got != sizeof(val) ? 0 : 0"),
    ):
        assert old in functions
        variants.append((name, functions.replace(old, new),
                         "FAIL pointer error survives close and debug; short reads are EIO"))
    for name, source, intended in variants:
        cfile, binary = Path(tmp) / f"{name}.c", Path(tmp) / name
        cfile.write_text(POINTER_HARNESS.replace("@FUNCTIONS@", source))
        subprocess.run([*common, str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if intended:
            assert result.returncode == 1 and intended in result.stderr, result
            print(f"{name}: KILLED (built, reached intended assertion: {intended})")
        else:
            assert result.returncode == 0, result
            print(result.stdout, end="")


def check_cohort_controls():
    before = dict.fromkeys(("lifecycle_init_lost_no_opportunity_total",
                           "lifecycle_init_lost_with_opportunity_total",
                           "wp_attach_exit_race_total", recovery.EXIT_BEFORE_ATTACH,
                           *recovery.EXIT_CLASSES.values()), 0)
    after = {**before, "lifecycle_init_lost_no_opportunity_total": 1,
             "wp_attach_exit_race_total": 1, recovery.EXIT_BEFORE_ATTACH: 1,
             recovery.EXIT_CLASSES["no_opportunity"]: 1}
    text = "\n".join((
        "INFO: PID 41 initialized (client), real watchpoint at 0x1234",
        "INIT-LOST: clock=CLOCK_MONOTONIC pid=42 init_ts=201 no_opportunity=1 exit_ts_source=record",
        "WP-ATTACH-FAIL: pid=42 site=bootstrap_watchpoint_open errno=3 resolve_errno=3 "
        "liveness=exiting liveness_result=1 liveness_errno=0 exit_proof=2",
        "EXIT-BEFORE-ATTACH: pid=44 opportunity=no entry_present=0"))
    pids = {41, 42, 44}
    assert recovery.attach_failure_pids(text) == set()
    assert recovery.attach_failure_pids(text + "\n" + "\n".join((
        "WP-ATTACH-FAIL: pid=42 site=watchpoint_open errno=5 liveness=gone exit_proof=0",
        "ERROR: cannot validate lifecycle target PID 51: denied",
        "WARN: max backends (1024) reached, cannot track PID 52",
        "ERROR: live FORK PID 53 conflicts with retained attach exit race; enrollment refused and counted",
    ))) == {42, 51, 52, 53}
    # Retained INIT and exit race on the same PID are compatible. No lifetime exemption.
    recovery.check_cohort(text, pids, set(), before, after)

    def reject(name, changed_text=text, changed_after=after, expected=""):
        try:
            recovery.check_cohort(changed_text, pids, set(pids), before, changed_after)
        except recovery.CohortFailure as error:
            assert expected in str(error), (name, error)
        else:
            raise AssertionError(f"churn negative control survived: {name}")
        print(f"churn control {name}: rejected by {expected}")

    for name in ("lifecycle_init_lost_no_opportunity_total", "wp_attach_exit_race_total",
                 recovery.EXIT_BEFORE_ATTACH, recovery.EXIT_CLASSES["no_opportunity"]):
        reject("missing " + name, changed_after={**after, name: 0},
               expected="delta does not match PID diagnostics")
    reject("exit proof with gated INIT", text.replace("no_opportunity=1", "no_opportunity=0"),
           expected="with-opportunity INIT loss")
    for label, key in (("with", "with_opportunity"), ("unknown", "unknown")):
        reject(label, text.replace("opportunity=no", "opportunity=" + label),
               expected=f"{key} retirement is gated")
        reject(label + " noncohort delta", changed_after={**after, recovery.EXIT_CLASSES[key]: 1},
               expected=f"{key} retirement delta must be zero")
    reject("race alone", text.replace("INIT-LOST:", "unrecognized:"),
           expected="backend has no counted outcome")
    reject("missing retirement line", text.replace("EXIT-BEFORE-ATTACH:", "unrecognized:"),
           expected="backend has no counted outcome")
    reject("overlap", text + "\nINFO: PID 44 initialized (client)",
           expected="overlapping retirement classes")
    reject("duplicate attachment", text + "\nINFO: PID 41 initialized (client)",
           expected="duplicate outcome diagnostic")
    reject("duplicate retirement", text + "\nEXIT-BEFORE-ATTACH: pid=44 opportunity=no",
           expected="duplicate outcome diagnostic")
    reject("uncounted extra PID", text + "\nEXIT-BEFORE-ATTACH: pid=45 opportunity=no",
           expected="delta does not match PID diagnostics")
    metrics = dict.fromkeys(DAEMON_INTEGRITY_COUNTERS, 0)
    metrics["counter_vector_valid"] = True
    assert daemon_integrity_ok(metrics)[0]
    for key in ("wp_attach_failures_total", recovery.EXIT_CLASSES["with_opportunity"],
                recovery.EXIT_CLASSES["unknown"]):
        for value in (1, None):
            ok, detail = daemon_integrity_ok({**metrics, key: value})
            assert not ok and key + "=" in detail, (key, value, detail)
    assert daemon_integrity_ok({**metrics, recovery.EXIT_CLASSES["no_opportunity"]: 9})[0]
    output = io.StringIO()
    extra = text + "\nINFO: fork detected PID 44, bootstrap watchpoint attached\nINFO: PID 42 exited (client)"
    with redirect_stdout(output):
        recovery.print_failure_diagnostics(extra, None, {42, 44})
    for line in extra.splitlines():
        if "pid=42" in line or "pid=44" in line or "PID 42" in line or "PID 44" in line:
            assert line in output.getvalue(), line
    print("churn PID reconciliation, retained INIT/race, opportunity gates and diagnostics: PASS")

def main():
    backend = (ROOT / "src/backend.c").read_text()
    control = (ROOT / "src/control.c").read_text()
    for counter in (recovery.EXIT_BEFORE_ATTACH, *recovery.EXIT_CLASSES.values()):
        assert f'cjson_add_uint64(root, "{counter}",' in control, counter
    functions = "\n\n".join(function(backend, name) for name in (
        "enrollment_was_refused", "remember_enrollment_refusal", "forget_enrollment_refusal",
        "lifecycle_target", "read_init_state", "record_init_loss", "pgwt_handle_exit", "pgwt_handle_exit_at"))
    needle = "d->counters.lifecycle_exit_before_attach_total++;"
    assert functions.count(needle) == 1
    builds = [("retirement", functions, None),
              ("missing_retirement_count", functions.replace(needle, ";"),
               "FAIL retirement counted once")]
    for key, intended in (("with_opportunity", "entry retirement has observed opportunity"),
                          ("no_opportunity", "positive first-handling gone proof reports no opportunity once"),
                          ("unknown", "absent entry without first-handling proof is unknown")):
        needle = f"d->counters.lifecycle_exit_before_attach_{key}_total++;"
        assert functions.count(needle) == 1
        builds.append(("missing_" + key, functions.replace(needle, ";"), "FAIL " + intended))
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
                assert "entry_present=1 bootstrap_armed=1 post_arm_checked=1 post_arm_ptr=0x1234 addr_shared=-1 resolve_errno=13 shared_errno=5 counter_deltas_valid=1 lifecycle_init_no_state_delta=5 wait_read_fail_delta=8" in result.stderr, result
                print(result.stdout, end="")
        check_pointer_diagnostics(common, tmp)
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
