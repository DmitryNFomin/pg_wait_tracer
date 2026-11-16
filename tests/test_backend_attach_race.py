#!/usr/bin/env python3
"""Production attach/fork/liveness bodies with injected I/O, on Mac or Linux.

Uses the portable backend harness's unchanged-function extraction. No kernel
watchpoints are opened. --mutants requires a build and the intended assertion.
"""
import argparse
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_backend_init_order import ROOT, function
from test_init_hint_recovery import print_failure_diagnostics, ATTACH_COUNTERS
import test_init_hint_recovery as recovery
from unittest.mock import patch

HARNESS = r'''
#include "daemon.h"
#include "backend_init.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct pg_wait_tracer_bpf { struct { void *on_watchpoint, *on_bootstrap; } progs; };
static struct pgwt_daemon d;
static int attempts, enables, closes, stat_opens, stat_closes, stat_reads;
static int target_state, operation_failed, enable_failure, open_error, anchor_error;
static int resolve_ready, direct_inits, replacement;
static int resolve_error, diagnostic_count;
static char diagnostic[1024];
static int pinned_generation[32];
enum { LIVE, GONE, ZOMBIE, FOREIGN, UNKNOWN, MALFORMED, REUSED };
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", msg); exit(1); } } while (0)

static int fake_open(const char *path, int flags)
{
    CHECK(strcmp(path, "/proc/42/stat") == 0, "stat path");
    CHECK(flags == (O_RDONLY | O_CLOEXEC), "stat open flags");
    if (anchor_error) { errno = anchor_error; return -1; }
    int fd = ++stat_opens;
    CHECK(fd < 32, "stat fd capacity");
    pinned_generation[fd] = replacement;
    return fd;
}
static int fake_close(int fd)
{
    CHECK(fd > 0 && fd <= stat_opens, "close only retained stat fd");
    ++stat_closes;
    errno = EBUSY;
    return 0;
}
static ssize_t fake_pread(int fd, void *buf, size_t size, off_t offset)
{
    CHECK(fd > 0 && fd <= stat_opens && offset == 0, "read retained stat fd");
    ++stat_reads;
    int state = operation_failed ? target_state : LIVE;
    /* The pathname now names a live replacement with the SAME parent.
     * Only the retained handle still observes the original's exit. */
    if (state == REUSED)
        state = pinned_generation[fd] == replacement ? LIVE : GONE;
    if (state == GONE || state == UNKNOWN) {
        errno = state == GONE ? ESRCH : EACCES;
        return -1;
    }
    const char *line = state == ZOMBIE ? "42 (backend (worker)) Z 7" :
                       state == FOREIGN ? "42 (backend (worker)) R 8" :
                       state == MALFORMED ? "broken stat" : "42 (backend (worker)) R 7";
    size_t len = strlen(line);
    CHECK(size > len, "stat buffer capacity");
    memcpy(buf, line, len);
    return (ssize_t)len;
}
#define open fake_open
#define close fake_close
#define pread fake_pread

uint64_t pgwt_debug_monotonic_ns(void) { return 100; }
void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t ns)
{ (void)op; (void)pid; (void)ns; errno = ENOSYS; }
static uint64_t now_ns(void) { return 100; }
static int bpf_program__fd(void *prog) { (void)prog; return 11; }
int pgwt_exact_seed_backend(struct pgwt_daemon *ctx, pid_t pid, uint64_t gen, bool coherent)
{ (void)ctx; (void)pid; (void)gen; (void)coherent; return 0; }
static int preseed_state_map(struct pgwt_daemon *ctx, pid_t pid, uint64_t addr, uint64_t ts, bool sweep)
{ (void)ctx; (void)pid; (void)addr; (void)ts; (void)sweep; return 0; }
static int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int prog)
{
    CHECK(pid == 42 && addr != 0 && prog == 11, "hardware open arguments");
    ++attempts;
    if (enable_failure) return 99;
    operation_failed = 1;
    if (target_state == REUSED) replacement = 1;
    errno = open_error;
    return -1;
}
static int pgwt_open_watchpoint_disabled(pid_t pid, uint64_t addr, int prog)
{ return pgwt_open_watchpoint(pid, addr, prog); }
static int pgwt_watchpoint_enable(int fd)
{
    CHECK(fd == 99, "enable opened watchpoint");
    ++enables;
    operation_failed = 1;
    if (target_state == REUSED) replacement = 1;
    errno = EIO;
    return enable_failure == 1 ? -1 : 0;
}
static void pgwt_close_watchpoint(int fd)
{ CHECK(fd == 99, "close failed watchpoint"); ++closes; errno = ENOTTY; }
bool pgwt_pgbs_exclusion_contains(const struct pgwt_pgbs_exclusion_set *set, pid_t pid)
{ (void)set; (void)pid; return false; }
int pgwt_handle_exit(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; (void)pid; return 0; }
static struct pgwt_backend *alloc_backend(struct pgwt_backend_table *bt, pid_t pid, bool pg13)
{
    (void)pg13;
    struct pgwt_backend *be = &bt->entries[bt->count++];
    *be = (struct pgwt_backend){.pid=pid, .wp_fd=-1, .bootstrap_fd=-1, .is_alive=true};
    return be;
}
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{ (void)bt; (void)pid; return NULL; }
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; (void)pid; errno = resolve_error; return resolve_ready ? 1234 : 0; }
int pgwt_handle_init(struct pgwt_daemon *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{ (void)ctx; (void)pid; (void)addr; (void)ts; ++direct_inits; return 0; }
static int pgwt_addr_is_shared(pid_t pid, uint64_t ptr)
{ (void)pid; (void)ptr; return 1; }
void pgwt_pending_init_add(struct pgwt_backend_table *bt, struct pgwt_backend *be)
{ (void)bt; (void)be; }

/* Capture the production record and deliberately clobber errno as a failed
 * stderr write could. Other production messages are irrelevant here. */
static int capture_fprintf(FILE *stream, const char *format, ...)
{
    (void)stream;
    if (strncmp(format, "WP-ATTACH-FAIL:", 15) == 0) {
        va_list ap;
        va_start(ap, format);
        vsnprintf(diagnostic, sizeof(diagnostic), format, ap);
        va_end(ap);
        ++diagnostic_count;
    }
    errno = EBADF;
    return 0;
}
#define fprintf capture_fprintf
@FUNCTIONS@
#undef fprintf

static void reset(int state)
{
    static struct pg_wait_tracer_bpf skel;
    memset(&d, 0, sizeof(d));
    d.skel = &skel; d.postmaster_pid = 7; d.my_wait_ptr_addr = 1234;
    d.debug_dump_state = true;
    attempts = enables = closes = stat_opens = stat_closes = stat_reads = 0;
    operation_failed = enable_failure = anchor_error = resolve_ready = direct_inits = replacement = 0;
    open_error = ESRCH; target_state = state;
    resolve_error = EFAULT; diagnostic_count = 0; diagnostic[0] = '\0';
}
static void check_counts(int state)
{
    int gone = state == GONE || state == ZOMBIE || state == FOREIGN || state == REUSED;
    CHECK(d.counters.wp_attach_failures_total == (uint64_t)!gone,
          "live or unknown failure stays a real failure");
    CHECK(d.counters.wp_attach_exit_race_total == (uint64_t)gone,
          "confirmed exit counted separately");
    CHECK(d.counters.wp_attach_failures_live_total == (uint64_t)(state == LIVE),
          "live diagnostic count");
    CHECK(d.counters.wp_attach_failures_unknown_total ==
          (uint64_t)(state == UNKNOWN || state == MALFORMED), "unknown diagnostic count");
    CHECK(attempts == 1, "exactly one hardware attempt");
    CHECK(stat_opens == stat_closes, "retained handles closed");
}
static void check_record(int state, const char *site, int error, int resolution_error)
{
    int live = state == LIVE ? 1 : state == UNKNOWN || state == MALFORMED ? -1 : 0;
    int read_error = state == UNKNOWN ? EACCES : state == MALFORMED ? EINVAL : 0;
    char expected[1024];
    snprintf(expected, sizeof(expected),
             "WP-ATTACH-FAIL: pid=42 site=%s errno=%d resolve_errno=%d "
             "liveness=%s liveness_result=%d liveness_errno=%d\n",
             site, error, resolution_error,
             live > 0 ? "live" : live == 0 ? "gone" : "unknown", live, read_error);
    CHECK(diagnostic_count == 1, "one diagnostic per recorded failure");
    CHECK(strcmp(diagnostic, expected) == 0, "exact failure evidence");
}
int main(void)
{
    int errors[] = {ESRCH, EACCES, EMFILE, ENOSPC, EIO};
    for (int state = LIVE; state <= REUSED; ++state) {
        for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
            reset(state); open_error = errors[i];
            struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
            CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "real open failure returned");
            CHECK(errno == open_error, "hardware errno preserved");
            check_counts(state);
            check_record(state, "watchpoint_open", open_error, 0);
            if (i == 0 && (state == LIVE || state == GONE || state == UNKNOWN))
                fputs(diagnostic, stdout);
            CHECK(stat_reads == 1, "post-failure liveness read");

            reset(state); open_error = errors[i];
            CHECK(pgwt_handle_fork(&d, 42) == -1, "bootstrap failure returned");
            CHECK(errno == open_error, "bootstrap hardware errno preserved");
            check_counts(state);
            check_record(state, "bootstrap_watchpoint_open/backend_wait_addr_resolve",
                         open_error, EFAULT);
            CHECK(stat_reads == 2, "eligibility and post-failure reads");
        }
        reset(state); enable_failure = 1;
        struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
        CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "enable failure returned");
        CHECK(errno == EIO, "enable errno preserved after cleanup");
        check_counts(state);
        check_record(state, "watchpoint_enable", EIO, 0);
        CHECK(enables == 1 && closes == 1 && be.wp_fd == -1, "enable failure cleans up");
    }
    reset(LIVE); resolve_error = 0;
    CHECK(pgwt_handle_fork(&d, 42) == -1, "unassigned bootstrap pointer handled");
    CHECK(errno == ESRCH, "unassigned pointer preserves bootstrap errno");
    check_record(LIVE, "bootstrap_watchpoint_open/backend_wait_addr_resolve", ESRCH, 0);
    puts("real open, bootstrap open, enable: gone +1 race/+0 failures; live/unknown +1 failure: PASS");
    puts("same-parent PID reuse retains original handle; errno independent classification: PASS");

    int errors_before[] = {ENOENT, ESRCH, EACCES, EMFILE};
    for (size_t i = 0; i < sizeof(errors_before)/sizeof(errors_before[0]); ++i) {
        reset(LIVE); anchor_error = errors_before[i];
        struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
        CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "failed identity open handled");
        CHECK(errno == ESRCH, "operation errno preserved after identity failure");
        int gone = anchor_error == ENOENT || anchor_error == ESRCH;
        CHECK(d.counters.wp_attach_exit_race_total == (uint64_t)gone &&
              d.counters.wp_attach_failures_total == (uint64_t)!gone,
              "only missing proc identity proves exit");
        CHECK(d.counters.wp_attach_failures_unknown_total == (uint64_t)!gone &&
              d.counters.wp_attach_failures_live_total == 0, "identity diagnostic counts");
        char expected[64];
        snprintf(expected, sizeof(expected), "liveness_errno=%d\n", gone ? 0 : anchor_error);
        CHECK(diagnostic_count == 1 && strstr(diagnostic, expected), "identity read errno recorded");
    }
    reset(LIVE); anchor_error = EACCES;
    CHECK(pgwt_handle_fork(&d, 42) == -1, "unknown lifecycle eligibility refused");
    CHECK(attempts == 0 && d.counters.wp_attach_failures_total == 1 &&
          d.counters.wp_attach_exit_race_total == 0, "validation refusal remains a failure");
    reset(LIVE); resolve_ready = 1;
    CHECK(pgwt_handle_fork(&d, 42) == 0 && direct_inits == 1, "bootstrap direct-init fallback");
    CHECK(d.counters.wp_attach_failures_total == 0 && d.counters.wp_attach_exit_race_total == 0,
          "successful fallback not counted as failure");
    CHECK(diagnostic_count == 0 && stat_reads == 1 &&
          d.counters.wp_attach_failures_live_total == 0 &&
          d.counters.wp_attach_failures_unknown_total == 0, "fallback has no failure work");
    CHECK(stat_opens == stat_closes, "fallback closes retained identity");
    reset(LIVE); enable_failure = 2;
    struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
    CHECK(pgwt_attach_backend_watchpoint(&d, &be) == 0, "successful attach");
    CHECK(d.counters.wp_attach_failures_total == 0 && d.counters.wp_attach_exit_race_total == 0 &&
          stat_opens == stat_closes, "success closes identity without failure counts");
    CHECK(diagnostic_count == 0 && stat_reads == 0 &&
          d.counters.wp_attach_failures_live_total == 0 &&
          d.counters.wp_attach_failures_unknown_total == 0, "success has no failure work");
    reset(LIVE); enable_failure = 2;
    CHECK(pgwt_handle_fork(&d, 42) == 0, "successful bootstrap attach");
    CHECK(attempts == 1 && stat_reads == 1 && stat_opens == stat_closes &&
          diagnostic_count == 0 && d.counters.wp_attach_failures_total == 0 &&
          d.counters.wp_attach_exit_race_total == 0 &&
          d.counters.wp_attach_failures_live_total == 0 &&
          d.counters.wp_attach_failures_unknown_total == 0, "bootstrap success has no failure work");
    puts("missing/unknown identity, validation refusal, direct fallback and success: PASS");
    return 0;
}
'''

PERF_HARNESS = r'''
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
struct perf_event_attr { int unused; };
#define PERF_FLAG_FD_CLOEXEC 1
#define PERF_EVENT_IOC_SET_BPF 2
#define PERF_EVENT_IOC_ENABLE 3
static int fail_site, operation_error, closes;
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", msg); exit(1); } } while (0)
static void pgwt_fill_watchpoint_attr(struct perf_event_attr *attr, uint64_t addr, int enabled)
{ (void)attr; (void)addr; (void)enabled; }
static int sys_perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group, unsigned long flags)
{
    (void)attr; (void)pid; (void)cpu; (void)group; (void)flags;
    if (fail_site == 1) { errno = operation_error; return -1; }
    return 99;
}
static int ioctl(int fd, int op, ...)
{
    CHECK(fd == 99, "perf ioctl fd");
    if ((fail_site == 2 && op == PERF_EVENT_IOC_SET_BPF) ||
        (fail_site == 3 && op == PERF_EVENT_IOC_ENABLE)) {
        errno = operation_error; return -1;
    }
    return 0;
}
static int close(int fd)
{ CHECK(fd == 99, "perf close fd"); ++closes; errno = EBUSY; return 0; }
static int clobber_fprintf(FILE *stream, const char *format, ...)
{ (void)stream; (void)format; errno = EBADF; return -1; }
#define fprintf clobber_fprintf
@FUNCTIONS@
#undef fprintf
int main(void)
{
    int errors[] = {ESRCH, EACCES, EMFILE, ENFILE, ENOSPC, EINVAL};
    for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        operation_error = errors[i];
        for (fail_site = 1; fail_site <= 3; ++fail_site) {
            closes = 0;
            CHECK(pgwt_open_watchpoint(42, 1234, 11) == -1, "perf failure returned");
            CHECK(errno == operation_error, "perf operation errno survives logging and close");
            CHECK(closes == (fail_site != 1), "perf failure cleanup");
        }
        fail_site = 3;
        CHECK(pgwt_watchpoint_enable(99) == -1, "standalone enable failure");
        CHECK(errno == operation_error, "standalone enable errno survives logging");
    }
    fail_site = 0;
    CHECK(pgwt_open_watchpoint(42, 1234, 11) == 99, "perf success unchanged");
    CHECK(pgwt_watchpoint_enable(99) == 0, "perf enable success unchanged");
    puts("perf open/SET_BPF/ENABLE preserve operation errno under failed logging and cleanup: PASS");
    return 0;
}
'''


def check_bounded_failure_output():
    metrics = dict.fromkeys((
        "lifecycle_init_lost_total", "lifecycle_init_lost_with_opportunity_total",
        "lifecycle_init_lost_no_opportunity_total", "lifecycle_init_lost_untraced_ns_total",
        "lifecycle_init_hint_drops_total", "ringbuf_lifecycle_fork_drops_total",
        "ringbuf_lifecycle_exit_drops_total", "state_map_full_total", *ATTACH_COUNTERS), 0)
    after = {**metrics, "lifecycle_init_hint_drops_total": 1,
             "wp_attach_failures_total": 1, "wp_attach_failures_unknown_total": 1}
    output = io.StringIO()
    # Exercise the real churn assertion and exception handler, replacing only
    # daemon/PG I/O. The same failure must escape with bounded evidence.
    diagnostic = ("WP-ATTACH-FAIL: pid=42 site=watchpoint_open errno=3 resolve_errno=0 "
                  "liveness=unknown liveness_result=-1 liveness_errno=13")

    class Daemon:
        def __init__(self, *args, stderr, **kwargs):
            stderr.write("PGWT_TEST_DROP_INIT_HINT\n" + "unrelated daemon noise\n" * 23000 +
                         diagnostic + "\n")
            stderr.flush()

        def poll(self):
            return 0

        def wait(self, timeout):
            return 0

    with patch.object(recovery.subprocess, "Popen", Daemon), \
            patch.object(recovery, "wait_for"), \
            patch.object(recovery, "ctl", side_effect=[metrics, after]), \
            patch.object(recovery, "short_cohort", return_value=({42}, {42})), \
            redirect_stdout(output):
        try:
            recovery.run_arm(7, skip_store=False, churn=True)
        except AssertionError as error:
            assert error.args == (("wp_attach_failures_total", 1),), error
            print(f"AssertionError: {error}")
        else:
            raise AssertionError("forced churn failure did not reach attach assertion")
    text = output.getvalue()
    assert "unrelated daemon noise" not in text and diagnostic in text, text
    assert "'wp_attach_failures_unknown_total': 1" in text, text
    assert len(text.splitlines()) <= 9, text  # three titled sections, one line, counters, error
    print("Forced churn failure output (23,000 unrelated log lines excluded):")
    print(text, end="")
    # Bound both selected streams, including pathological line width.
    output = io.StringIO()
    with redirect_stdout(output):
        print_failure_diagnostics("\n".join(
            prefix + str(i) + "x" * 1000
            for prefix in ("INIT-LOST:", "WP-ATTACH-FAIL:", "ERROR: cannot validate lifecycle target")
            for i in range(100)), after)
    lines = output.getvalue().splitlines()
    assert len(lines) == 124 and sum("last 40 of 100" in line for line in lines) == 3
    assert all(len(line) <= 512 for line in lines[:-1])
    print("bounded failure streams: last 40 each, 512 chars/line: PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutants", action="store_true")
    args = parser.parse_args()
    backend = (ROOT / "src/backend.c").read_text()
    cmdline = (ROOT / "src/cmdline.c").read_text()
    functions = "\n\n".join([
        function(cmdline, "pgwt_backend_target_live_fd"),
        function(cmdline, "pgwt_backend_target_live"),
        *(function(backend, name) for name in (
            "open_watchpoint_target", "record_watchpoint_failure",
            "pgwt_attach_backend_watchpoint", "lifecycle_target", "pgwt_handle_fork")),
    ])
    control = (ROOT / "src/control.c").read_text()
    for name in ATTACH_COUNTERS:
        assert f'cjson_add_uint64(root, "{name}",' in control
    variants = [("baseline", functions, None)]
    if args.mutants:
        needle = "if (live == 0)\n        d->counters.wp_attach_exit_race_total++;"
        assert functions.count(needle) == 1
        variants.append(("every_failure_is_exit_race", functions.replace(
            needle, "if (1)\n        d->counters.wp_attach_exit_race_total++;"),
                         "FAIL live or unknown failure stays a real failure"))
        for name, old, new, intended in (
            ("missing_live_count", "wp_attach_failures_live_total++", "wp_attach_failures_live_total += 0",
             "live diagnostic count"),
            ("missing_unknown_count", "wp_attach_failures_unknown_total++", "wp_attach_failures_unknown_total += 0",
             "unknown diagnostic count"),
            ("wrong_operation_errno", "pid, site, error, resolve_error,", "pid, site, 0, resolve_error,",
             "exact failure evidence"),
            ("wrong_liveness_errno", "int live_error = live < 0 ? errno : 0;", "int live_error = 0;",
             "exact failure evidence"),
            ("wrong_liveness_outcome", 'live > 0 ? "live"', 'live > 0 ? "gone"',
             "exact failure evidence"),
            ("gone_logged_live", 'live == 0 ? "gone"', 'live == 0 ? "live"',
             "exact failure evidence"),
            ("unknown_logged_gone", ': "unknown", live, live_error', ': "gone", live, live_error',
             "exact failure evidence"),
            ("lost_resolution_errno", "pid, site, error, resolve_error,", "pid, site, error, resolve_error * 0,",
             "exact failure evidence"),
            ("wrong_site", '"watchpoint_enable", error, 0', '"watchpoint_open", error, 0',
             "exact failure evidence"),
            ("lost_bootstrap_errno", "error, resolve_error);", "error + resolve_error, resolve_error);",
             "bootstrap hardware errno preserved"),
            ("lost_enable_errno", "be->wp_fd = -1;\n        errno = error;", "be->wp_fd = -1;",
             "enable errno preserved after cleanup"),
            ("lost_record_errno", "errno = error;\n}", "errno = 0;\n}", "hardware errno preserved"),
        ):
            assert functions.count(old) == 1, (name, old)
            variants.append((name, functions.replace(old, new), "FAIL " + intended))
    perf = (ROOT / "src/perf_event.c").read_text()
    perf_functions = "\n\n".join(function(perf, name) for name in (
        "open_watchpoint_common", "pgwt_open_watchpoint", "pgwt_watchpoint_enable"))
    builds = [(name, HARNESS.replace("@FUNCTIONS@", source), intended)
              for name, source, intended in variants]
    builds.append(("perf_errno", PERF_HARNESS.replace("@FUNCTIONS@", perf_functions), None))
    if args.mutants:
        for index in range(4):
            pieces = perf_functions.split("errno = error;")
            assert len(pieces) == 5
            mutated = "errno = error;".join(pieces[:index + 1]) + "errno = 0;" + \
                "errno = error;".join(pieces[index + 1:])
            intended = "standalone enable errno survives logging" if index == 3 else \
                "perf operation errno survives logging and close"
            builds.append((f"perf_lost_errno_{index}",
                           PERF_HARNESS.replace("@FUNCTIONS@", mutated), "FAIL " + intended))
    with tempfile.TemporaryDirectory(prefix="pgwt-attach-race-") as tmp:
        for name, source, intended in builds:
            cfile, binary = Path(tmp) / f"{name}.c", Path(tmp) / name
            cfile.write_text(source)
            command = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
                       "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                       *shlex.split(os.environ.get("CFLAGS", "")), str(cfile), "-o", str(binary)]
            subprocess.run(command, check=True)
            print(f"{name}: portable C build PASS", flush=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if intended:
                assert result.returncode == 1 and intended in result.stderr, result
                print(f"{name}: KILLED (built, reached intended assertion: {intended})")
            else:
                print(result.stdout, end="")
                print(result.stderr, end="")
                assert result.returncode == 0, result
    check_bounded_failure_output()
    print("test_backend_attach_race: PASS")


if __name__ == "__main__":
    main()
