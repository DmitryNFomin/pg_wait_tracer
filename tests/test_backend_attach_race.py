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
#include "capture_counters.h"
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
static int resolve_ready, direct_inits, replacement, allocation_failure, shared_result, shared_error;
static int resolve_error, diagnostic_count, failure_reads, proof_state;
static char diagnostic[1024];
static int pinned_generation[32];
enum { LIVE, GONE, ZOMBIE, FOREIGN, UNKNOWN, MALFORMED, REUSED, EXITING,
       EXITING_VSIZE, ZERO_VSIZE, INCOMPLETE, EMPTY_STAT, DEAD };
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
    if (operation_failed && ++failure_reads == 2 && proof_state >= 0) state = proof_state;
    /* The pathname now names a live replacement with the SAME parent.
     * Only the retained handle still observes the original's exit. */
    if (state == REUSED)
        state = pinned_generation[fd] == replacement ? LIVE : GONE;
    if (state == GONE || state == UNKNOWN) {
        errno = state == GONE ? ESRCH : EACCES;
        return -1;
    }
    /* Ported proof fixtures from the escalation branch, limited to stat. */
    char stat_line[512];
    unsigned flags = state == EXITING || state == EXITING_VSIZE ? 4 : 0;
    unsigned vsize = state == EXITING || state == ZERO_VSIZE ? 0 : 8192;
    char status = state == ZOMBIE ? 'Z' : state == DEAD ? 'X' : 'R';
    int parent = state == FOREIGN ? 8 : 7;
    snprintf(stat_line, sizeof(stat_line), "42 (backend (worker)) %c %d 0 0 0 0 %u 0 0 0 0 0 0 0 0 0 0 0 0 123 %u", status, parent, flags, vsize);
    const char *line = state == MALFORMED ? "broken stat" :
                       state == INCOMPLETE ? "42 (backend) R 7" :
                       state == EMPTY_STAT ? "" : stat_line;
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
    if (allocation_failure) return NULL;
    struct pgwt_backend *be = &bt->entries[bt->count++];
    *be = (struct pgwt_backend){.pid=pid, .wp_fd=-1, .bootstrap_fd=-1, .is_alive=true};
    return be;
}
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    for (int i = 0; i < bt->count; ++i)
        if (bt->entries[i].pid == pid) return &bt->entries[i];
    return NULL;
}
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; (void)pid; errno = resolve_error; return resolve_ready ? 1234 : 0; }
int pgwt_handle_init(struct pgwt_daemon *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{ (void)ctx; (void)pid; (void)addr; (void)ts; ++direct_inits; return 0; }
static int pgwt_addr_is_shared(pid_t pid, uint64_t ptr)
{ (void)pid; (void)ptr; errno = shared_error; return shared_result; }
void pgwt_pending_init_add(struct pgwt_backend_table *bt, struct pgwt_backend *be)
{ (void)bt; (void)be; }

struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *ctx, uint32_t slot)
{ (void)ctx; (void)slot; return (struct pgwt_counter_value){0, true}; }

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
    operation_failed = enable_failure = anchor_error = resolve_ready = direct_inits = replacement = allocation_failure = 0;
    open_error = ESRCH; target_state = state; shared_result = 1; shared_error = 0;
    resolve_error = EFAULT; failure_reads = 0; proof_state = -1; diagnostic_count = 0; diagnostic[0] = '\0';
}
static void check_counts(int state, int error)
{
    int gone = error == ESRCH && (state == GONE || state == ZOMBIE ||
               state == DEAD || state == REUSED || state == EXITING);
    CHECK(d.counters.wp_attach_failures_total == (uint64_t)!gone,
          "live or unknown failure stays a real failure");
    CHECK(d.counters.wp_attach_exit_race_total == (uint64_t)gone,
          "confirmed exit counted separately");
    int live = state == LIVE || state == EXITING || state == EXITING_VSIZE ||
               state == ZERO_VSIZE || state == INCOMPLETE;
    CHECK(d.counters.wp_attach_failures_live_total == (uint64_t)(!gone && live),
          "live diagnostic count");
    int retired = state == GONE || state == ZOMBIE || state == DEAD || state == FOREIGN || state == REUSED;
    CHECK(d.counters.wp_attach_failures_gone_total == (uint64_t)(!gone && retired),
          "gone diagnostic count");
    CHECK(d.counters.wp_attach_failures_unknown_total ==
          (uint64_t)(!gone && !live && !retired), "unknown diagnostic count");
    CHECK(attempts == 1, "exactly one hardware attempt");
    CHECK(stat_opens == stat_closes, "retained handles closed");
}
static void check_record(int state, const char *site, int error, int resolution_error)
{
    int live = state == UNKNOWN || state == MALFORMED || state == EMPTY_STAT ? -1 :
               state == GONE || state == ZOMBIE || state == DEAD ||
               state == FOREIGN || state == REUSED ? 0 : 1;
    int read_error = state == UNKNOWN ? EACCES : state == MALFORMED ? EINVAL :
                     state == EMPTY_STAT ? EIO : 0;
    char expected[1024];
    snprintf(expected, sizeof(expected),
             "WP-ATTACH-FAIL: pid=42 site=%s errno=%d resolve_errno=%d "
             "liveness=%s liveness_result=%d liveness_errno=%d exit_proof=%d\n",
             site, error, resolution_error,
             error == ESRCH && state == EXITING ? "exiting" :
             live > 0 ? "live" : live == 0 ? "gone" : "unknown", live, read_error,
             error != ESRCH ? 0 : state == EXITING ? 2 :
             state == GONE || state == ZOMBIE || state == DEAD || state == REUSED ? 1 :
             state == UNKNOWN || state == MALFORMED || state == FOREIGN ||
             state == INCOMPLETE || state == EMPTY_STAT ? -1 : 0);
    CHECK(diagnostic_count == 1, "one diagnostic per recorded failure");
    CHECK(strcmp(diagnostic, expected) == 0, "exact failure evidence");
}
int main(void)
{
    int errors[] = {ESRCH, EACCES, EMFILE, ENOSPC, EIO};
    for (int state = LIVE; state <= DEAD; ++state) {
        for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
            reset(state); open_error = errors[i];
            struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
            CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "real open failure returned");
            CHECK(errno == open_error, "hardware errno preserved");
            check_counts(state, open_error);
            check_record(state, "watchpoint_open", open_error, 0);
            if (i == 0 && (state == LIVE || state == GONE || state == UNKNOWN))
                fputs(diagnostic, stdout);
            CHECK(stat_reads == 1 + (open_error == ESRCH), "post-failure liveness and proof reads");

            reset(state); open_error = errors[i];
            CHECK(pgwt_handle_fork(&d, 42) == -1, "bootstrap failure returned");
            CHECK(errno == open_error, "bootstrap hardware errno preserved");
            check_counts(state, open_error);
            check_record(state, "bootstrap_watchpoint_open/backend_wait_addr_resolve",
                         open_error, EFAULT);
            CHECK(stat_reads == 2 + (open_error == ESRCH), "eligibility and post-failure proof reads");
            if (open_error == ESRCH && state == EXITING) {
                CHECK(d.backends.entries[0].wp_attach_exit_race &&
                      d.backends.entries[0].pid == 42, "retain counted exit identity");
                CHECK(pgwt_attach_backend_watchpoint(&d, &d.backends.entries[0]) == -1 &&
                      attempts == 1, "proven exit is never retried");
            }
        }
        reset(state); enable_failure = 1;
        struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
        CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "enable failure returned");
        CHECK(errno == EIO, "enable errno preserved after cleanup");
        check_counts(state, EIO);
        check_record(state, "watchpoint_enable", EIO, 0);
        CHECK(enables == 1 && closes == 1 && be.wp_fd == -1, "enable failure cleans up");
    }
    /* The pinned task can change between the diagnostic read and proof.
     * Only the actual proof result decides the counter and PID class. */
    reset(GONE); proof_state = UNKNOWN;
    struct pgwt_backend changing = {.pid=42, .wp_addr=1234, .wp_fd=-1};
    CHECK(pgwt_attach_backend_watchpoint(&d, &changing) == -1, "changing proof failure returned");
    CHECK(d.counters.wp_attach_failures_total == 1 &&
          d.counters.wp_attach_exit_race_total == 0 && strstr(diagnostic, "exit_proof=-1"),
          "unknown second read refuses exit classification");
    reset(LIVE); proof_state = GONE;
    CHECK(pgwt_attach_backend_watchpoint(&d, &changing) == -1, "exit during proof returned");
    CHECK(d.counters.wp_attach_failures_total == 0 &&
          d.counters.wp_attach_exit_race_total == 1 && strstr(diagnostic, "liveness=gone") &&
          strstr(diagnostic, "exit_proof=1"), "positive second read records proven exit");
    reset(LIVE); resolve_error = 0;
    CHECK(pgwt_handle_fork(&d, 42) == -1, "unassigned bootstrap pointer handled");
    CHECK(errno == ESRCH, "unassigned pointer preserves bootstrap errno");
    check_record(LIVE, "bootstrap_watchpoint_open/backend_wait_addr_resolve", ESRCH, 0);
    puts("real open, bootstrap open, enable: ESRCH+gone/exiting +1 race; live/unknown/other errno +1 failure: PASS");
    puts("same-parent PID reuse retains original handle; ESRCH plus positive proof required: PASS");

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
        snprintf(expected, sizeof(expected), "liveness_errno=%d exit_proof=", gone ? 0 : anchor_error);
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
    CHECK(be.init_attached, "shared helper retains completed attachment");
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
    CHECK(d.backends.entries[0].bootstrap_armed && d.backends.entries[0].post_arm_checked &&
          d.backends.entries[0].post_arm_ptr == 0 &&
          d.backends.entries[0].post_arm_resolve_errno == EFAULT &&
          d.backends.entries[0].enroll_counters_valid,
          "silent post-arm pointer failure retained for EXIT diagnostic");
    reset(LIVE); enable_failure = 2; resolve_ready = 1; resolve_error = 0;
    shared_result = -1; shared_error = EACCES;
    CHECK(pgwt_handle_fork(&d, 42) == 0 && direct_inits == 0 &&
          d.backends.entries[0].post_arm_ptr == 1234 &&
          d.backends.entries[0].post_arm_addr_shared == -1 &&
          d.backends.entries[0].post_arm_resolve_errno == 0 &&
          d.backends.entries[0].post_arm_shared_errno == EACCES,
          "silent post-arm shared-address failure retained for EXIT diagnostic");
    reset(LIVE); allocation_failure = 1;
    CHECK(pgwt_handle_fork(&d, 42) == -1 && d.counters.wp_attach_failures_total == 1 &&
          attempts == 0 && enrollment_was_refused(&d, 42),
          "allocation refusal gates even without state or EXIT");
    reset(LIVE);
    struct pgwt_backend *old = alloc_backend(&d.backends, 42, false);
    old->wp_attach_exit_race = true; /* startup scan: no state, no EXIT before reuse */
    CHECK(pgwt_handle_fork(&d, 42) == -1 && d.counters.wp_attach_failures_total == 1 &&
          attempts == 0, "live reused PID conflicting with startup race is counted");
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
        "lifecycle_init_hint_drops_total", "init_state_read_fail_total",
        recovery.EXIT_BEFORE_ATTACH, *recovery.EXIT_CLASSES.values(), "ringbuf_lifecycle_fork_drops_total",
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
                         "".join(f"INFO: PID {pid} exited (client)\n" for pid in range(1, 7671) if pid != 42) +
                         diagnostic + "\n")
            stderr.flush()

        def poll(self):
            return 0

        def wait(self, timeout):
            return 0

    with patch.object(recovery.subprocess, "Popen", Daemon), \
            patch.object(recovery, "wait_for"), \
            patch.object(recovery, "ctl", side_effect=[metrics, after]), \
            patch.object(recovery, "short_cohort", return_value=(set(range(1, 7671)), set(range(1, 7671)))), \
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
    assert "Daemon lines for failing PID 42:" in text and "failing PID 41:" not in text, text
    assert "INFO: PID 41 exited" not in text and "INFO: PID 7670 exited" not in text, text
    assert "'wp_attach_failures_unknown_total': 1" in text, text
    assert len(text.splitlines()) <= 14, text  # three titled sections, one line, counters, error
    print("Forced churn failure output (7,670 cohort PIDs, 23,000 unrelated log lines excluded):")
    print(text, end="")
    # A retirement timeout identifies only the PID whose acknowledgement is
    # missing, even in a large cohort; generic failures identify no PID.
    def wait_once(proc, predicate, seconds, reason):
        if "EXIT" in reason and not predicate():
            raise AssertionError(reason)
    for wait in (wait_once, [None, RuntimeError("metrics unavailable")]):
        output = io.StringIO()
        with patch.object(recovery.subprocess, "Popen", Daemon), \
                patch.object(recovery, "wait_for", side_effect=wait), \
                patch.object(recovery, "ctl", side_effect=[metrics, after]), \
                patch.object(recovery, "short_cohort", return_value=(set(range(1, 7671)), set())), \
                redirect_stdout(output):
            try:
                recovery.run_arm(7, skip_store=False, churn=True)
            except (AssertionError, RuntimeError) as error:
                assert "EXIT" in str(error) or "metrics unavailable" in str(error), error
            else:
                raise AssertionError("forced timeout/generic failure survived")
        text = output.getvalue()
        assert "failing PID 41:" not in text and "INFO: PID 41 exited" not in text, text
        if callable(wait):
            assert "Daemon lines for failing PID 42:" in text, text
        else:
            assert "Daemon lines for failing PID" not in text, text
        assert len(text.splitlines()) <= 14, text
    print("large-cohort retirement timeout and generic failure PID selection: PASS")
    # Bound both selected streams, including pathological line width.
    output = io.StringIO()
    with redirect_stdout(output):
        print_failure_diagnostics("\n".join(
            prefix + str(i) + "x" * 1000
            for prefix in ("INIT-LOST:", "WP-ATTACH-FAIL:", "ERROR: cannot validate lifecycle target")
            for i in range(100)), after)
    lines = output.getvalue().splitlines()
    assert len(lines) == 126 and sum("last 40 of 100" in line for line in lines) == 3
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
        function(cmdline, "pgwt_backend_no_command_fd"),
        function(cmdline, "pgwt_backend_target_live"),
        *(function(backend, name) for name in (
            "open_watchpoint_target", "record_watchpoint_failure",
            "pgwt_attach_backend_watchpoint", "enrollment_failure_counted", "enrollment_was_refused",
            "remember_enrollment_refusal", "lifecycle_target", "pgwt_handle_fork")),
    ])
    control = (ROOT / "src/control.c").read_text()
    for name in ATTACH_COUNTERS:
        assert f'cjson_add_uint64(root, "{name}",' in control
    variants = [("baseline", functions, None)]
    if args.mutants:
        needle = "bool exit_race = error == ESRCH && proof > 0;"
        assert functions.count(needle) == 1
        variants.append(("every_failure_is_exit_race", functions.replace(
            needle, "bool exit_race = true;"),
                         "FAIL live or unknown failure stays a real failure"))
        for name, old, new, intended in (
            ("missing_exit_race_count", "wp_attach_exit_race_total++", "wp_attach_exit_race_total += 0",
             "confirmed exit counted separately"),
            ("ESRCH_alone_excused", "error == ESRCH && proof > 0", "error == ESRCH && proof >= -1",
             "live or unknown failure stays a real failure"),
            ("PF_EXITING_mask_wrong", "flags & 0x4ULL", "flags & 0x8ULL",
             "live or unknown failure stays a real failure"),
            ("ignore_vsize", "(flags & 0x4ULL) && vsize == 0", "(flags & 0x4ULL) && vsize >= 0",
             "live or unknown failure stays a real failure"),
            ("missing_live_count", "wp_attach_failures_live_total++", "wp_attach_failures_live_total += 0",
             "live diagnostic count"),
            ("missing_gone_count", "wp_attach_failures_gone_total++", "wp_attach_failures_gone_total += 0",
             "gone diagnostic count"),
            ("missing_pointer_errno", "be->post_arm_resolve_errno = errno;", "be->post_arm_resolve_errno = 0;",
             "silent post-arm pointer failure retained for EXIT diagnostic"),
            ("missing_shared_errno", "be->post_arm_shared_errno = errno;", "be->post_arm_shared_errno = 0;",
             "silent post-arm shared-address failure retained for EXIT diagnostic"),
            ("missing_allocation_failure_count", "d->counters.wp_attach_failures_total++;\n        remember_enrollment_refusal(d, child_pid);",
             "d->counters.wp_attach_failures_total += 0;\n        remember_enrollment_refusal(d, child_pid);",
             "allocation refusal gates even without state or EXIT"),
            ("missing_reused_pid_count", "d->counters.wp_attach_failures_total++;\n            existing->wp_attach_failed = true;",
             "d->counters.wp_attach_failures_total += 0;\n            existing->wp_attach_failed = true;",
             "live reused PID conflicting with startup race is counted"),
            ("missing_shared_attached_proof", "be->init_attached = true;", ";",
             "shared helper retains completed attachment"),
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
            ("unknown_logged_gone", ': "unknown",\n            live, live_error, proof', ': "gone",\n            live, live_error, proof',
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
