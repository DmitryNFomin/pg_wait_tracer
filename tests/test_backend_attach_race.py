#!/usr/bin/env python3
"""Production attach/fork/liveness bodies with injected I/O, on Mac or Linux.

Uses the portable backend harness's unchanged-function extraction. No kernel
watchpoints are opened by the default mode. --mutants requires a build and the
intended assertion. --live-socket runs the five-condition, 400-escalation Linux
churn gate against a prepared tiered daemon (see live_churn).
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
#include <ctype.h>
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
static int resolve_ready, direct_inits, replacement, allocation_failure, shared_result, shared_error, seed_failure;
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
{ (void)ctx; (void)pid; (void)gen; (void)coherent; return seed_failure; }
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
/* Sampler identity is independent of the retained watchpoint-target handle.
 * Model successful enrollment without /proc I/O, so it cannot change the
 * injected hardware errno, liveness state, or attach/race counters. The
 * finder below always returns NULL: duplicate-fork identity/reset are not
 * reached by these attach scenarios. */
bool pgwt_pgbs_pid_is_original(pid_t pid, uint64_t start_time)
{ return pid == 42 && start_time == 1; }
void pgwt_pgbs_reader_reset(struct pgwt_pgbs_reader *reader)
{
    free(reader->target_maps);
    free(reader->reader_maps);
    memset(reader, 0, sizeof(*reader));
}
int pgwt_sampler_enroll_backend(struct pgwt_backend *be, uint64_t tick_ts)
{
    (void)tick_ts;
    if (be->pid <= 0) return -1;
    if (!be->sampled_reader.pid) {
        be->sampled_reader.pid = be->pid;
        be->sampled_reader.start_time = 1;
    }
    return be->sampled_reader.pid == be->pid &&
           be->sampled_reader.start_time != 0 ? 0 : -1;
}
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
    seed_failure = 0;
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
    reset(LIVE); enable_failure = 2; seed_failure = -1;
    d.exact_probes.core.generation = 9;
    be = (struct pgwt_backend){.pid=42, .wp_addr=1234, .wp_fd=-1};
    CHECK(pgwt_attach_backend_watchpoint(&d, &be) == 0 &&
          d.counters.exact_preseed_late_attach_failures_total == 1,
          "late attach caller checks and counts seed refusal");
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
    parser.add_argument("--live-socket", help="Linux-only: prepared daemon's pgwt.sock")
    parser.add_argument("--dsn", default="dbname=pgbench user=postgres")
    parser.add_argument("--rounds", type=int, default=400)
    parser.add_argument("--cpu-clients", type=int, default=8)
    args = parser.parse_args()
    if args.live_socket:
        live_churn(args)
        return
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
            ("ignore_vsize", "(flags & 0x4ULL) && vsize == 0", "(flags & 0x4ULL) && (vsize || 1)",
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
    preseed_cases(args.mutants)
    wait_seed_cases(args.mutants)
    from test_preseed_startup_live import host_checks
    host_checks()
    print("test_backend_attach_race: PASS")



PRESEED_HARNESS = r'''
#include "daemon.h"
#include "synthetic_query.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct pg_wait_tracer_bpf { struct { void *exact_seed_map, *exact_attr_map, *state_map; } maps; };
static struct pgwt_daemon d;
static struct pgwt_exact_seed stored;
static int have_seed, map_failure, stat_reads, stat_opens, mem_reads, first_read_fail;
static int scenario, stat_closed, mem_closed, edge_present, mem_opens, wait_shared;
static unsigned start_change_on_read;
static unsigned checks;
static uint64_t resolved_wait_addr;
static int resolve_calls, fresh_wait_shared, reuse_on_resolve;
static int state_updates, have_state, state_failure;
static struct pgwt_pid_state wait_state;
/* Torn-row re-read: row reads through memory open N are torn (odd counter). */
static int torn_opens, sleeps, resolve_errno, stat_eio_from;
static uint64_t mono_now;
enum { NORMAL, LIVE, GONE, ZOMBIE, DEAD, EXITING, EXITING_VSIZE, NULL_ENTRY,
       STARTUP, EIO_READ, ZERO_READ, TORN, WRONG_PID, ZERO_PID, DB_ONLY,
       USER_ONLY, STARTUP_OPEN, UNKNOWN, REUSED, BAD_STAT, MEM_ESRCH, MEM_EPERM,
       MEM_EFAULT, MISSING_FIELD, UNRESOLVED, BAD_STATE, FOREIGN, BAD_OPEN,
       ENTRY_EIO, ENTRY_ZERO, UNKNOWN_AFTER, ZERO_VSIZE, OPEN_GONE,
       UNPUBLISHED, UNPUBLISHED_TORN, OUTSIDE, OFF_STRIDE, RACE_OK, RACE_FAIL,
       RACE_OPEN, RACE_OPEN_FAIL };
/* While stderr is captured, a failure still reaches the real stderr. */
static int saved_stderr = -1;
#define CHECK(x, msg) do { ++checks; if (!(x)) { fflush(stderr); \
    dprintf(saved_stderr >= 0 ? saved_stderr : 2, "FAIL %s\n", msg); exit(1); } } while (0)
/* stderr capture for diagnostics; defined before the open/close/pread fakes. */
static char capture_path[64];
static int capture_fd = -1;
static void capture_begin(void)
{
    fflush(stderr);
    strcpy(capture_path, "/tmp/pgwt-preseed-diag-XXXXXX");
    capture_fd = mkstemp(capture_path);
    CHECK(capture_fd >= 0, "create diagnostic capture");
    saved_stderr = dup(2);
    CHECK(saved_stderr >= 0 && dup2(capture_fd, 2) == 2, "redirect diagnostic capture");
}
static void capture_end(char *out, size_t size)
{
    fflush(stderr);
    CHECK(dup2(saved_stderr, 2) == 2, "restore stderr");
    close(saved_stderr); saved_stderr = -1;
    ssize_t n = pread(capture_fd, out, size - 1, 0);
    out[n > 0 ? n : 0] = '\0';
    close(capture_fd); unlink(capture_path);
}
static int count_lines(const char *text, const char *needle)
{
    int n = 0;
    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) ++n;
    return n;
}
static int fake_open(const char *path, int flags)
{
    CHECK(flags == (O_RDONLY | O_CLOEXEC), "classification descriptor flags");
    if (!strcmp(path, "/proc/42/stat")) {
        ++stat_opens;
        if (scenario == BAD_OPEN || scenario == OPEN_GONE) {
            errno = scenario == OPEN_GONE ? ENOENT : EACCES; return -1;
        }
        return 101;
    }
    CHECK(!strcmp(path, "/proc/42/mem"), "only target memory opened");
    ++mem_opens;
    if (scenario == RACE_FAIL || scenario == RACE_OPEN_FAIL) {
        CHECK(mem_opens <= 3, "reread bounded to one attempt");
        if (mem_opens >= 3) { errno = EIO; return -1; }
    }
    if (scenario == MEM_ESRCH || scenario == MEM_EPERM) {
        errno = scenario == MEM_ESRCH ? ESRCH : EPERM; return -1;
    }
    return 100;
}
static int fake_close(int fd)
{
    CHECK(fd == 100 || fd == 101, "close pinned memory or stat");
    if (fd == 100) ++mem_closed; else ++stat_closed;
    return 0;
}
static ssize_t fake_pread(int fd, void *buf, size_t size, off_t offset)
{
    if (fd == 101) {
        CHECK(offset == 0 && stat_opens == 1, "classification retains one original stat handle");
        ++stat_reads;
        if (scenario == GONE || (scenario == REUSED && stat_reads > 1)) {
            errno = ESRCH; return -1;
        }
        if (scenario == UNKNOWN || (scenario == UNKNOWN_AFTER && stat_reads > 1) ||
            (stat_eio_from && stat_reads >= stat_eio_from)) {
            errno = EIO; return -1;
        }
        char line[512];
        unsigned flags = scenario == EXITING || scenario == EXITING_VSIZE ? 4 : 0;
        unsigned vsize = scenario == EXITING || scenario == ZERO_VSIZE ? 0 : 8192;
        char state = scenario == ZOMBIE ? 'Z' : scenario == DEAD ? 'X' : 'R';
        int parent = scenario == FOREIGN ? 8 : 7;
        snprintf(line, sizeof(line), "42 (backend (worker)) %c %d 0 0 0 0 %u 0 0 0 0 0 0 0 0 0 0 0 0 123 %u", state, parent, flags, vsize);
        if (start_change_on_read && (unsigned)stat_reads == start_change_on_read) {
            char *start = strstr(line, " 123 "); CHECK(start != NULL, "stat start field located");
            memcpy(start + 1, "124", 3);
        }
        if (scenario == BAD_STAT) strcpy(line, "42 (backend) R 7");
        size_t n = strlen(line); CHECK(size > n, "full stat proof fits");
        memcpy(buf, line, n); return (ssize_t)n;
    }
    CHECK(fd == 100, "memory handle identity");
    ++mem_reads;
    if (first_read_fail) { first_read_fail = 0; errno = EIO; return -1; }
    if (scenario == EIO_READ || scenario == MEM_EFAULT ||
        (scenario == ENTRY_EIO && offset == 0x1000)) {
        errno = scenario == MEM_EFAULT ? EFAULT : EIO; return -1;
    }
    if (scenario == ZERO_READ || (scenario == ENTRY_ZERO && offset == 0x1000)) return 0;
    int recorded_live = offset >= 0x3000 && offset < 0x3040;
    if (recorded_live) offset -= 0x1000;
    uint64_t value = 0;
    if (scenario == OUTSIDE && offset >= 0xa000) offset -= 0x8000;
    if (scenario == OFF_STRIDE && offset >= 0x2001) offset -= 1;
    if (offset == 0x1000) value = scenario == NULL_ENTRY || scenario == UNKNOWN_AFTER ? 0 :
        scenario == OUTSIDE ? 0xa000 : scenario == OFF_STRIDE ? 0x2001 : 0x2000;
    /* 0x1008 deliberately contains NULL: off-by-eight pointer mutant. */
    else if (offset == 0x1008) value = 0;
    else if (offset == 0x2000) value = scenario == TORN || scenario == UNPUBLISHED_TORN ||
        mem_opens <= torn_opens ? 3 : 2;
    else if (offset == 0x2004) value = recorded_live ? 42 : scenario == WRONG_PID ? 43 :
        scenario == ZERO_PID || scenario == UNPUBLISHED || scenario == UNPUBLISHED_TORN ||
        scenario == OUTSIDE || scenario == OFF_STRIDE ? 0 : 42;
    else if (offset == 0x2008) value = scenario == STARTUP || scenario == USER_ONLY || scenario == STARTUP_OPEN ||
        ((scenario == RACE_OK || scenario == RACE_FAIL || scenario == RACE_OPEN ||
          scenario == RACE_OPEN_FAIL) && mem_opens == 1) ? 0 : 1;
    else if (offset == 0x200c) value = scenario == STARTUP || scenario == DB_ONLY || scenario == STARTUP_OPEN ||
        ((scenario == RACE_OK || scenario == RACE_FAIL || scenario == RACE_OPEN ||
          scenario == RACE_OPEN_FAIL) && mem_opens == 1) ? 0 : 10;
    else if (offset == 0x2010) value = scenario == STARTUP || scenario == RACE_OK || scenario == RACE_FAIL ? 0 : scenario == BAD_STATE ? 99 : 3;
    else if (offset == 0x2018) value = 123;
    else if (offset == 0x2020) value = 0; /* PG13 no activity text needed */
    else { CHECK(0, "known memory address"); }
    CHECK(size == 4 || size == 8, "field width");
    if (scenario == MISSING_FIELD && offset == 0x2008) { errno = EIO; return -1; }
    memcpy(buf, &value, size); return (ssize_t)size;
}
#define open fake_open
#define close fake_close
#define pread fake_pread
static int bpf_map__fd(void *map) { return map == d.skel->maps.exact_seed_map ? 1 :
    map == d.skel->maps.state_map ? 3 : 2; }
static int bpf_map_lookup_elem(int fd, const void *key, void *out)
{
    CHECK(*(const uint32_t *)key == 42, "map pid key");
    if (fd == 1 && have_seed) { memcpy(out, &stored, sizeof(stored)); return 0; }
    if (fd == 2 && edge_present) {
        struct pgwt_exact_attr edge = {.query_generation=9, .query_edge_ts=100};
        memcpy(out, &edge, sizeof(edge)); return 0;
    }
    return -1;
}
#define BPF_ANY 0
#define BPF_NOEXIST 1
static int bpf_map_update_elem(int fd, const void *key, const void *value, int flags)
{
    if (fd == 3) {
        CHECK(*(const uint32_t *)key == 42 && flags == BPF_NOEXIST, "idempotent wait state seed");
        ++state_updates;
        if (have_state) { errno = EEXIST; return -1; }
        if (state_failure) { errno = ENOSPC; return -1; }
        memcpy(&wait_state, value, sizeof(wait_state)); have_state = 1; return 0;
    }
    CHECK(fd == 1 && *(const uint32_t *)key == 42 && flags == BPF_ANY, "seed map update");
    if (map_failure) return -1;
    memcpy(&stored, value, sizeof(stored)); have_seed = 1; return 0;
}
static int bpf_map_get_next_key(int fd, const void *key, void *next)
{
    CHECK(fd == 1, "clear seed map");
    if (key || !have_seed) return -1;
    *(uint32_t *)next = 42; return 0;
}
static int bpf_map_delete_elem(int fd, const void *key)
{ CHECK(fd == 1 && *(const uint32_t *)key == 42, "delete generation seed"); have_seed = 0; return 0; }
static uint64_t mono_ns(void) { return mono_now; }
static int fake_nanosleep(const struct timespec *pause, struct timespec *rest)
{
    CHECK(pause && !rest && pause->tv_sec == 0 && pause->tv_nsec == 500000,
          "torn reread pauses 0.5 ms");
    ++sleeps;
    CHECK(sleeps <= 8, "torn reread bounded");
    return 0;
}
#define nanosleep fake_nanosleep
int pgwt_addr_is_shared(pid_t pid, uint64_t addr)
{
    CHECK(pid == 42 && (addr == 0x5000 || addr == 0x6000 || addr == 0xa000), "wait address belongs to backend");
    return addr == 0x5000 ? wait_shared : fresh_wait_shared;
}
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *ctx, pid_t pid)
{
    CHECK(ctx == &d && pid == 42, "resolve original backend once");
    ++resolve_calls;
    if (reuse_on_resolve) start_change_on_read = (unsigned)stat_reads + 2;
    if (resolve_errno) errno = resolve_errno;
    return resolved_wait_addr;
}
static int get_config(struct pgwt_daemon *ctx, struct pgwt_exact_config *cfg)
{ (void)ctx; *cfg = (struct pgwt_exact_config){.generation=9, .attach_boundary_ns=90}; return 0; }
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *table, pid_t pid)
{ return table->count && pid == 42 ? &table->entries[0] : NULL; }
int pgwt_parse_cmdline(pid_t pid, struct pgwt_metadata *meta)
{ (void)pid; (void)meta; return -1; }
void pgwt_bm_observe_context(struct pgwt_backend_meta_writer *bm, struct pgwt_backend *be, uint32_t db, uint32_t user)
{ (void)bm; (void)be; (void)db; (void)user; }
void pgwt_read_cmd_gate(pid_t pid, uint64_t debug, uint64_t entry, int offset, int *gate, uint64_t *qid)
{ (void)pid; (void)debug; (void)entry; (void)offset; (void)gate; (void)qid; }
int pgwt_exact_resolve_attr(struct pgwt_daemon *ctx, pid_t pid, uint64_t *qid, uint16_t *cmd,
                          uint16_t *quality, uint16_t *phase, uint64_t *plan, uint64_t *exec)
{ (void)ctx; (void)pid; (void)quality; (void)phase; (void)plan; (void)exec; *qid=456; *cmd=1; return 0; }
int pgwt_pg13_synthetic_query(uint32_t db, uint32_t user, const char *text, char *norm, size_t size, uint64_t *qid)
{ (void)db; (void)user; (void)text; (void)norm; (void)size; *qid=555; return 0; }
void pgwt_qt_store_synthetic(struct pgwt_query_text_capture *capture, const struct pgwt_query_text_key *key, const char *text)
{ (void)capture; (void)key; (void)text; }
bool pgwt_pgbs_sampled_attr_enabled(const struct PgBackendStatusLayout *layout)
{ return layout->validation == PGWT_PGBS_VALIDATION_VALIDATED; }
bool pgwt_pgbs_sampled_activity_enabled(const struct PgBackendStatusLayout *layout)
{ return pgwt_pgbs_sampled_attr_enabled(layout) && layout->major == 13; }
bool pgwt_pgbs_sampled_query_id_enabled(const struct PgBackendStatusLayout *layout)
{ return pgwt_pgbs_sampled_attr_enabled(layout) && layout->major >= 14; }
static int resolve_entry_fd(pid_t pid, int fd, const struct PgBackendStatusLayout *layout, uint64_t *out)
{ (void)pid; (void)fd; (void)layout; *out = scenario == UNRESOLVED ? 0 : 0x2000;
  return *out ? PGWT_PGBS_PRESEED_ROW : PGWT_PGBS_PRESEED_FAILED; }
uint64_t pgwt_pgbs_resolve_entry(pid_t pid, const struct PgBackendStatusLayout *layout)
{ uint64_t base; resolve_entry_fd(pid, -1, layout, &base); return base; }
struct pgwt_pgbs_mapping { uint64_t lo, hi, identity, backing; };
static uint64_t fresh_generation;
static unsigned maps_reads;
static struct pgwt_pgbs_mapping pg13_main_mapping(pid_t pid, const struct PgBackendStatusLayout *layout)
{
    CHECK((pid == 7 || (pid == 42 && layout->major >= 14 && layout->major <= 17)) && layout == &d.backend_status_layout, "fresh mapping belongs to postmaster");
    ++maps_reads;
    return (struct pgwt_pgbs_mapping){.lo=0x2000,.hi=0x9000,.identity=fresh_generation};
}
@FUNCTIONS@
static void reset(int test, int major)
{
    static struct pg_wait_tracer_bpf skel = {.maps={.exact_seed_map=(void *)1, .exact_attr_map=(void *)2, .state_map=(void *)3}};
    memset(&d, 0, sizeof(d)); memset(&stored, 0, sizeof(stored));
    d.skel = &skel; d.postmaster_pid = 7; d.my_be_entry_addr = 0x1000;
    d.backends.count = 1;
    d.backends.entries[0] = (struct pgwt_backend){.pid=42, .is_alive=true,
        .meta_parsed=true, .meta={.backend_type=PGWT_BT_CLIENT},
        .wp_addr=0x5000, .wp_addr_shared=1,
        .pgbs_addr=major == 13 && test != UNRESOLVED ? 0x2000 : 0};
    struct PgBackendStatusLayout *l = &d.backend_status_layout;
    l->major = major; l->validation = PGWT_PGBS_VALIDATION_VALIDATED;
    l->pg13_recovery = PGWT_PGBS_PG13_READY;
    l->shm_generation = fresh_generation = 77; maps_reads = 0;
    l->shm_start=0x2000; l->shm_end=0x9000; l->generation_postmaster=7;
    l->status_anchor=0x2040; l->struct_size=64;
    l->state_running = 3; l->state_fastpath = 4; l->state_max = 6;
    l->st_changecount=(struct pgwt_pgbs_field){.offset=0,.width=4,.present=true};
    l->st_procpid=(struct pgwt_pgbs_field){.offset=4,.width=4,.present=true};
    l->st_databaseid=(struct pgwt_pgbs_field){.offset=8,.width=4,.present=true};
    l->st_userid=(struct pgwt_pgbs_field){.offset=12,.width=4,.present=true};
    l->st_state=(struct pgwt_pgbs_field){.offset=16,.width=4,.present=true};
    l->st_query_id=(struct pgwt_pgbs_field){.offset=24,.width=8,.present=true};
    l->st_activity_raw=(struct pgwt_pgbs_field){.offset=32,.width=8,.present=true};
    start_change_on_read = 0;
    mem_opens=0; wait_shared=1;
    resolved_wait_addr=0; resolve_calls=0; fresh_wait_shared=1; reuse_on_resolve=0;
    torn_opens=sleeps=resolve_errno=stat_eio_from=0; mono_now=100;
    state_updates=have_state=state_failure=0; memset(&wait_state, 0, sizeof(wait_state));
    scenario=test; have_seed=map_failure=stat_reads=stat_opens=mem_reads=stat_closed=mem_closed=edge_present=0;
    first_read_fail = test == LIVE || test == GONE || test == ZOMBIE || test == DEAD ||
        test == EXITING || test == EXITING_VSIZE || test == UNKNOWN || test == REUSED ||
        test == BAD_STAT || test == BAD_OPEN || test == FOREIGN ||
        test == ZERO_VSIZE || test == OPEN_GONE;
}
static void deny(int test, int major, const char *message)
{
    reset(test, major);
    CHECK(pgwt_exact_seed_all(&d, 9) != 0, message);
    CHECK(!have_seed && d.counters.exact_preseed_no_command_total == 0 &&
          d.exact_probes.preseed_no_command == 0, "rollback publishes no proof seed");
    CHECK(d.counters.exact_preseed_failures_total == 1, "rollback counted");
    CHECK(stat_closed == (scenario == BAD_OPEN ? 0 : stat_opens), "failure closes pinned identity");
}
/* A live backend whose coherent read failed once, but whose fallback row is
 * published with context and an open command: never a no-command proof. PG13
 * rolls back; PG14+ takes the one coherent reread, which seeds the command. */
static void live_command(int test, int major, const char *message)
{
    if (major == 13) { deny(test, major, message); return; }
    reset(test, major);
    CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed && stored.cmd_open &&
          stored.query_id == 123 && d.counters.exact_preseed_reread_total == 1 &&
          d.counters.exact_preseed_no_command_total == 0 &&
          d.exact_probes.preseed_no_command == 0 && mem_opens == 3,
          "live published backend seeds its open command through the coherent reread");
}
static void auxiliary(int test, int major, enum pgwt_backend_type type, const char *message)
{
    reset(test, major);
    d.backends.entries[0].meta.backend_type = type;
    CHECK(pgwt_exact_seed_all(&d, 9) == 0, message);
    CHECK(have_seed && stored.generation == 9 && stored.query_id == 0 &&
          stored.cmd_open == 0 && stored.query_quality == PGWT_QUERY_QUALITY_NONE,
          "incoherent auxiliary retains closed seed");
    CHECK(d.counters.exact_preseed_no_command_total == 0 &&
          d.counters.exact_preseed_gone_total == 0 &&
          d.counters.exact_preseed_exiting_total == 0 &&
          d.counters.exact_preseed_null_entry_total == 0 &&
          d.counters.exact_preseed_startup_total == 0 &&
          d.exact_probes.preseed_no_command == 0, message);
    CHECK(d.counters.exact_preseed_failures_total == 0 &&
          stat_opens == 0 && stat_reads == 0,
          "auxiliary has zero rollbacks and no classification");
}
static struct pgwt_pgbs_acceptance acceptance(void)
{
    return (struct pgwt_pgbs_acceptance){.address=0x2000,.pid=42,.start_time=123,.shm_generation=77};
}
static void retirement_table(void)
{
    enum { CANDIDATE, RECORD_PID, RECORD_START, RECORD_GENERATION_ZERO,
           RECORD_GENERATION, FRESH_GENERATION, PENDING, DEGRADED, START_BEFORE,
           START_AFTER, ODD, UNEQUAL, INCOMPLETE, SAME_PID, READ_FAILED,
           ACCEPT_ZERO, ACCEPT_OTHER, OBSERVED_ZERO, OBSERVED_OTHER };
    const char *names[] = {
        "retirement candidate has no acceptance", "retirement record belongs to pid",
        "retirement record needs start time", "retirement record needs generation",
        "retirement record generation matches layout", "retirement fresh generation matches record",
        "retirement recovery pending refused", "retirement recovery degraded refused",
        "retirement start time before matches", "retirement start time after matches",
        "retirement odd counter refused", "retirement unequal counter refused",
        "retirement incomplete fields refused", "retirement live pid refused",
        "retirement failed read refused", "retirement accepted row zero",
        "retirement accepted row successor", "retirement sampler observed zero",
        "retirement sampler observed successor"
    };
    for (int clause=CANDIDATE; clause<=OBSERVED_OTHER; ++clause) {
        reset(ZERO_PID, 13);
        struct pgwt_pgbs_acceptance record = acceptance();
        struct pgwt_pgbs_snapshot row = {.read_mask=PGWT_PGBS_READ_CHANGECOUNT |
            PGWT_PGBS_READ_PROCPID | PGWT_PGBS_READ_DATABASEID | PGWT_PGBS_READ_USERID |
            PGWT_PGBS_READ_STATE, .changecount_before=2,.changecount_after=2,
            .procpid=0,.databaseid=1,.userid=10,.state=3};
        uint64_t before=123, after=123, current=77;
        int result=PGWT_PGBS_PRESEED_RETIRED, expected=PGWT_PGBS_RETIRED_NONE;
        switch (clause) {
        case CANDIDATE: record.address=0; break;
        case RECORD_PID: record.pid=43; break;
        case RECORD_START: record.start_time=0; before=after=0; break;
        case RECORD_GENERATION_ZERO: record.shm_generation=0; current=0; d.backend_status_layout.shm_generation=0; break;
        case RECORD_GENERATION: d.backend_status_layout.shm_generation=78; break;
        case FRESH_GENERATION: current=78; break;
        case PENDING: d.backend_status_layout.pg13_recovery=PGWT_PGBS_PG13_PENDING; break;
        case DEGRADED: d.backend_status_layout.pg13_recovery=PGWT_PGBS_PG13_DEGRADED; break;
        case START_BEFORE: before=124; break;
        case START_AFTER: after=124; break;
        case ODD: row.changecount_before=row.changecount_after=3; break;
        case UNEQUAL: row.changecount_after=4; break;
        case INCOMPLETE: row.read_mask &= ~PGWT_PGBS_READ_DATABASEID; break;
        case SAME_PID: row.procpid=42; break;
        case READ_FAILED: result=PGWT_PGBS_PRESEED_FAILED; break;
        case ACCEPT_ZERO: expected=PGWT_PGBS_RETIRED_ZERO; break;
        case ACCEPT_OTHER: row.procpid=43; expected=PGWT_PGBS_RETIRED_OTHER_PID; break;
        case OBSERVED_ZERO: record.exit_observed=expected=PGWT_PGBS_RETIRED_ZERO; result=-1; break;
        case OBSERVED_OTHER: record.exit_observed=expected=PGWT_PGBS_RETIRED_OTHER_PID; result=-1; break;
        }
        CHECK(pgwt_pgbs_retirement_proof(&record, 42, &d.backend_status_layout,
              current, before, after, result, &row) == expected, names[clause]);
        /* Retained observations must satisfy the SAME identity/generation gate. */
        if (clause <= START_AFTER) {
            record.exit_observed=PGWT_PGBS_RETIRED_ZERO;
            CHECK(pgwt_pgbs_retirement_proof(&record, 42, &d.backend_status_layout,
                  current, before, after, -1, NULL) == 0, names[clause]);
        }
        if (clause == INCOMPLETE) {
            const unsigned fields[] = {PGWT_PGBS_READ_CHANGECOUNT, PGWT_PGBS_READ_PROCPID,
                PGWT_PGBS_READ_USERID, PGWT_PGBS_READ_STATE};
            for (unsigned i=0; i<sizeof(fields)/sizeof(fields[0]); ++i) {
                row.read_mask = (PGWT_PGBS_READ_CHANGECOUNT | PGWT_PGBS_READ_PROCPID |
                    PGWT_PGBS_READ_DATABASEID | PGWT_PGBS_READ_USERID | PGWT_PGBS_READ_STATE) & ~fields[i];
                CHECK(pgwt_pgbs_retirement_proof(&record, 42, &d.backend_status_layout,
                      current, before, after, result, &row) == 0, names[clause]);
            }
        }
        printf("%s: PASS\n", names[clause]);
    }
}
static void retirement_seeds(void)
{
    retirement_table();
    for (int other=0; other<2; ++other) {
        reset(other ? WRONG_PID : ZERO_PID, 13);
        d.backends.entries[0].sampled_reader.accepted=acceptance();
        CHECK(pgwt_exact_seed_all(&d, 9) == 0 && stored.query_id == 0 &&
              !stored.cmd_open && stored.query_quality == PGWT_QUERY_QUALITY_NONE,
              "PG13 accepted retirement seeds closed after FATAL mid-command without close edge");
        CHECK(d.counters.exact_preseed_retired_zero_total == (uint64_t)!other &&
              d.counters.exact_preseed_retired_other_pid_total == (uint64_t)other &&
              d.counters.exact_preseed_no_command_total == 1 &&
              d.exact_probes.preseed_no_command == 1, "PG13 retirement counters distinguish zero and successor");
        CHECK(stat_opens == 1 && stat_closed == 1 && maps_reads == 1,
              "PG13 retirement pins stat and checks mapping fresh at seed time");
    }
    reset(ZERO_PID, 13);
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "PG13 address only in backend cache rolls back");
    reset(WRONG_PID, 13);
    d.backends.entries[0].sampled_reader.accepted=acceptance();
    d.backends.entries[0].sampled_reader.accepted.address=0x3000;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "PG13 live mid-query injected wrong cached address rolls back");
    for (unsigned when=2; when<=3; ++when) {
        reset(ZERO_PID, 13); start_change_on_read=when;
        d.backends.entries[0].sampled_reader.accepted=acceptance();
        CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
              when == 2 ? "PG13 pinned start changed before row rolls back" :
                          "PG13 pinned start changed after row rolls back");
    }
    reset(ZERO_PID, 13); fresh_generation=78;
    d.backends.entries[0].sampled_reader.accepted=acceptance();
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed && maps_reads == 1,
          "PG13 fresh mapping rejects stale interval generation");
    reset(MEM_EPERM, 13);
    d.backends.entries[0].sampled_reader.accepted=acceptance();
    d.backends.entries[0].sampled_reader.accepted.exit_observed=PGWT_PGBS_RETIRED_ZERO;
    CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed && !stored.cmd_open,
          "PG13 retained sampler retirement survives row unreadable at preseed");
    reset(ZERO_PID, 13); map_failure=1;
    d.backends.entries[0].sampled_reader.accepted=acceptance();
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed &&
          d.counters.exact_preseed_retired_zero_total == 0,
          "PG13 failed retirement seed map write remains rollback");
    reset(ZERO_PID, 13);
    d.backends.entries[0].meta.backend_type=PGWT_BT_PARALLEL_WORKER;
    d.backends.entries[0].sampled_reader.accepted=acceptance();
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "PG13 retirement proof is client only");
    reset(ZERO_PID, 18);
    d.backends.entries[0].sampled_reader.accepted=acceptance();
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "PG14+ does not use PG13 retirement proof");
}
static uint64_t proof_counters(void)
{
    return d.counters.exact_preseed_gone_total + d.counters.exact_preseed_exiting_total +
        d.counters.exact_preseed_null_entry_total + d.counters.exact_preseed_startup_total +
        d.counters.exact_preseed_unpublished_total + d.counters.exact_preseed_retired_zero_total +
        d.counters.exact_preseed_retired_other_pid_total;
}
static void torn_reread_cases(void)
{
    /* Beyond the bound: still torn after 8 paced re-reads denies and counts. */
    int torn_majors[] = {13, 14, 17, 18};
    for (unsigned i=0; i<sizeof(torn_majors)/sizeof(torn_majors[0]); ++i) {
        int major = torn_majors[i];
        reset(major >= 14 && major <= 17 ? UNPUBLISHED : STARTUP, major);
        torn_opens = 1000;
        CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed &&
              d.counters.exact_preseed_torn_exhausted_total == 1 &&
              d.counters.exact_preseed_failures_total == 1 &&
              d.counters.exact_preseed_no_command_total == 0 && proof_counters() == 0,
              "torn beyond the bound denies and counts");
        CHECK(sleeps == 8 && d.exact_probes.preseed_torn_pauses == 8 && mem_opens == 1 + 1 + 8,
              "torn reread spends exactly the bounded pauses, one row read each");
        CHECK(stat_closed == stat_opens, "torn denial closes pinned identity");
        /* The budget belongs to one escalation attempt, not the daemon. */
        sleeps = stat_opens = stat_closed = stat_reads = mem_opens = 0; /* a fresh escalation attempt */
        CHECK(pgwt_exact_seed_all(&d, 9) != 0 && sleeps == 8 &&
              d.counters.exact_preseed_torn_exhausted_total == 2,
              "torn reread budget resets per escalation attempt");
    }
    /* Torn, then clean within the bound: the existing proof, unchanged. */
    for (int major=13; major<=18; ++major) {
        for (int torn=1; torn<=8; torn+=7) {
            int kind = major >= 14 && major <= 17 ? UNPUBLISHED : STARTUP;
            reset(kind, major);
            torn_opens = 1 + torn; /* the coherent read, then `torn` preseed reads */
            CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed && !stored.cmd_open &&
                  stored.query_id == 0 && stored.query_quality == PGWT_QUERY_QUALITY_NONE,
                  "torn then clean within the bound gives the existing proof");
            CHECK(sleeps == torn && d.counters.exact_preseed_torn_exhausted_total == 0 &&
                  d.counters.exact_preseed_failures_total == 0,
                  "torn then clean re-reads only until clean, uncounted");
            CHECK(proof_counters() == 1 && (kind == UNPUBLISHED ?
                  d.counters.exact_preseed_unpublished_total == 1 :
                  d.counters.exact_preseed_startup_total == 1),
                  "torn then clean counts the same proof class as an untorn row");
        }
    }
    /* Identity changes between pauses: the pinned re-read refuses. Stat
     * reads: 1 classifier, 2 re-read pin, 3 first paced pin. The row is
     * clean by then, so only the pin can refuse it. */
    for (int major=13; major<=18; major+=5) {
        reset(STARTUP, major); torn_opens = 2; start_change_on_read = 3;
        CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed && sleeps == 1 &&
              d.counters.exact_preseed_torn_exhausted_total == 0,
              "torn reread under a changed identity pin denies");
    }
    /* An identity that cannot be pinned is never re-read. */
    reset(STARTUP, 18); torn_opens = 1000; stat_eio_from = 2;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed && sleeps == 0 && stat_reads >= 2 &&
          d.counters.exact_preseed_torn_exhausted_total == 0,
          "torn reread requires a readable pin");
    /* Live red control D on the host: the hook holds the counter odd past the budget. */
    char hook_path[] = "/tmp/pgwt-preseed-torn-XXXXXX";
    int hook_fd = mkstemp(hook_path);
    CHECK(hook_fd >= 0, "create torn-hold selector");
    FILE *hook_file = fdopen(hook_fd, "w");
    CHECK(hook_file != NULL, "open torn-hold selector");
    fprintf(hook_file, "42\n"); fclose(hook_file);
    for (int major=14; major<=17; major+=3) {
        reset(UNPUBLISHED, major);
        setenv("PGWT_TEST_PRESEED_TORN_PID_FILE", hook_path, 1);
        char log[4096];
        capture_begin();
        int result = pgwt_exact_seed_all(&d, 9);
        capture_end(log, sizeof(log));
        unsetenv("PGWT_TEST_PRESEED_TORN_PID_FILE");
        CHECK(result != 0 && !have_seed && sleeps == 8 && mem_opens == 1 + 1 + 8 &&
              d.counters.exact_preseed_torn_exhausted_total == 1 &&
              d.counters.exact_preseed_failures_total == 1 &&
              d.counters.exact_preseed_unpublished_total == 0,
              "held odd change counter denies escalation red control D");
        CHECK(count_lines(log, "TEST ONLY held status change counter odd PID 42") == 1 &&
              count_lines(log, "denied: cannot coherently preseed PID 42") == 1,
              "red control D logs its hook and the denial");
    }
    unlink(hook_path);
    reset(UNPUBLISHED, 17);
    CHECK(pgwt_exact_seed_all(&d, 9) == 0 && sleeps == 0,
          "torn-hold hook is default off");
    puts("torn reread: bound 8 x 0.5 ms, beyond-bound deny+count, clean-within-bound proofs, pin, red control D PASS");
}
static void command_open_route_cases(void)
{
    /* Published with the first command already open: the coherent reread
     * seeds the open command; it is never a no-command proof. */
    for (int major=14; major<=18; ++major) {
        reset(RACE_OPEN, major);
        CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed && stored.cmd_open &&
              stored.query_id == 123 && stored.query_quality == PGWT_QUERY_QUALITY_REAL,
              "published backend with an open command seeds the command");
        CHECK(d.counters.exact_preseed_reread_total == 1 && mem_opens == 3 &&
              d.counters.exact_preseed_no_command_total == 0 && proof_counters() == 0 &&
              d.counters.exact_preseed_failures_total == 0,
              "command-open route uses the one coherent reread, no proof");
        reset(RACE_OPEN_FAIL, major);
        CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed && mem_opens == 3 &&
              d.counters.exact_preseed_reread_total == 1 &&
              d.counters.exact_preseed_failures_total == 1,
              "command-open reread failure rolls back");
    }
    deny(RACE_OPEN, 13, "PG13 command-open route remains excluded");
    puts("command-open route: coherent reread seeds the open command, failure rolls back, PG13 excluded PASS");
}
static void refresh_diag_cases(void)
{
    struct { uint64_t addr; int shared, err; const char *line; } cases[] = {
        {0, 1, EIO, "pid=42 ptr=0x0 errno=5 is_shared=none in_shm=0 shm=[0x2000,0x9000) suppressed=0"},
        {0x6000, 0, 0, "pid=42 ptr=0x6000 errno=0 is_shared=0 in_shm=1 shm=[0x2000,0x9000) suppressed=0"},
        {0x6000, -1, 0, "pid=42 ptr=0x6000 errno=0 is_shared=-1 in_shm=1 shm=[0x2000,0x9000) suppressed=0"},
    };
    char log[8192];
    for (unsigned i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
        reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr = 0;
        resolved_wait_addr = cases[i].addr; fresh_wait_shared = cases[i].shared;
        resolve_errno = cases[i].err;
        capture_begin();
        int result = pgwt_exact_seed_all(&d, 9);
        capture_end(log, sizeof(log));
        CHECK(result != 0 && !have_seed && resolve_calls == 1 &&
              d.counters.exact_preseed_wait_addr_refreshed_total == 0,
              "non-shared refresh logs the diagnostic and denies");
        CHECK(count_lines(log, "WARN: preseed wait-address refresh not shared: ") == 1 &&
              strstr(log, cases[i].line) != NULL,
              "non-shared refresh diagnostic carries pointer errno shared and range");
    }
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr = 0;
    resolved_wait_addr = 0xa000; fresh_wait_shared = 0;
    capture_begin();
    int outside = pgwt_exact_seed_all(&d, 9);
    capture_end(log, sizeof(log));
    CHECK(outside != 0 && strstr(log, "ptr=0xa000 errno=0 is_shared=0 in_shm=0 shm=[0x2000,0x9000)") != NULL,
          "non-shared refresh diagnostic carries pointer errno shared and range");
    /* Rate limit: one line per second, with the suppressed count. */
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr = 0;
    resolved_wait_addr = 0x6000; fresh_wait_shared = 0;
    capture_begin();
    int denials = 0;
    for (int i=0; i<4; ++i) {
        if (i == 3) mono_now += 1000000000ULL;
        stat_opens = stat_closed = stat_reads = mem_opens = 0; /* a fresh escalation attempt */
        denials += pgwt_exact_seed_all(&d, 9) != 0;
    }
    capture_end(log, sizeof(log));
    CHECK(denials == 4 && count_lines(log, "refresh not shared") == 2 &&
          strstr(log, "suppressed=2") != NULL && d.exact_probes.refresh_diag_suppressed == 0,
          "refresh diagnostic is rate limited");
    /* Red control C performs no read, so it has nothing to report. */
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr = 0;
    setenv("PGWT_TEST_SKIP_PRESEED_WAIT_ADDR_REFRESH", "1", 1);
    capture_begin();
    int skipped = pgwt_exact_seed_all(&d, 9);
    capture_end(log, sizeof(log));
    unsetenv("PGWT_TEST_SKIP_PRESEED_WAIT_ADDR_REFRESH");
    CHECK(skipped != 0 && !strstr(log, "refresh not shared"), "disabled refresh has no pointer to report");
    /* A successful refresh never logs. */
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr = 0; resolved_wait_addr = 0x6000;
    capture_begin();
    int refreshed = pgwt_exact_seed_all(&d, 9);
    capture_end(log, sizeof(log));
    CHECK(refreshed == 0 && !strstr(log, "refresh not shared"), "shared refresh logs no diagnostic");
    puts("refresh diagnostic: pointer, errno, is_shared, mapping range, rate limit PASS");
}
int main(void)
{
    int auxiliary_majors[] = {18, 17, 13};
    for (unsigned i=0; i<sizeof(auxiliary_majors)/sizeof(auxiliary_majors[0]); i++) {
        int major = auxiliary_majors[i];
        auxiliary(NULL_ENTRY, major, PGWT_BT_LOGGER,
                  "logger NULL MyBEEntry alive is not proof-seeded");
        auxiliary(EIO_READ, major, PGWT_BT_CHECKPOINTER,
                  "checkpointer EIO does not roll back");
        auxiliary(TORN, major, PGWT_BT_AUTOVAC_WORKER,
                  "autovacuum worker torn read does not roll back");
    }
    puts("auxiliary controls: logger NULL MyBEEntry alive, checkpointer EIO, autovacuum worker torn read: PASS");
    for (int major=13; major<=18; major+=5) {
        reset(NORMAL, major);
        CHECK(pgwt_exact_seed_all(&d, 9) == 0 && stored.cmd_open && have_seed,
              "control coherent command preseed");
        CHECK(stat_opens == 0 && stat_reads == 0 &&
              d.counters.exact_preseed_no_command_total == 0 &&
              d.exact_probes.preseed_no_command == 0, "control zero proof seeds and classification cost");
        live_command(LIVE, major, "live initialised backend rolls back");
        if (major == 13) {
            reset(WRONG_PID, major);
            CHECK(seed_no_command(&d, &d.backends.entries[0], 9) != 0 && !have_seed,
                  "PG13 address only in backend cache rolls back");
        }
        int accepted[] = {GONE, ZOMBIE, DEAD, EXITING, STARTUP, REUSED, OPEN_GONE};
        for (unsigned i=0; i<sizeof(accepted)/sizeof(accepted[0]); i++) {
            reset(accepted[i], major);
            CHECK(pgwt_exact_seed_all(&d, 9) == 0, "positive proof allows escalation");
            CHECK(have_seed && stored.generation == 9 && stored.query_id == 0 &&
                  stored.cmd_open == 0 && stored.query_quality == PGWT_QUERY_QUALITY_NONE,
                  "proof seed has no command or query");
            CHECK(d.counters.exact_preseed_no_command_total == 1 &&
                  d.exact_probes.preseed_generation == 9 && d.exact_probes.preseed_no_command == 1,
                  "proof seed counted lifetime and generation");
            CHECK(d.counters.exact_preseed_gone_total + d.counters.exact_preseed_exiting_total +
                  d.counters.exact_preseed_null_entry_total + d.counters.exact_preseed_startup_total == 1,
                  "one proof class counted");
            if (accepted[i] == EXITING) CHECK(d.counters.exact_preseed_exiting_total == 1,
                                             "PF_EXITING with zero vsize proved");
            CHECK(stat_opens == 1 && stat_closed == (accepted[i] == OPEN_GONE ? 0 : 1),
                  "proof closes original identity");
            pgwt_exact_seed_clear(&d, 9);
            CHECK(!have_seed && d.exact_probes.preseed_no_command == 0 &&
                  d.counters.exact_preseed_no_command_total == 1, "clear generation preserves lifetime proof count");
        }
        live_command(EXITING_VSIZE, major, "PF_EXITING with nonzero vsize rolls back");
        live_command(ZERO_VSIZE, major, "zero vsize without PF_EXITING rolls back");
        deny(EIO_READ, major, "EIO rolls back");
        deny(ZERO_READ, major, "zero-byte memory read without exit proof rolls back");
        deny(TORN, major, "torn change counter rolls back");
        deny(WRONG_PID, major, "foreign row PID rolls back");
        deny(ZERO_PID, major, "zero row PID alone rolls back");
        deny(DB_ONLY, major, "database alone cannot prove startup");
        deny(USER_ONLY, major, "user alone cannot prove startup");
        deny(STARTUP_OPEN, major, "startup with open command rolls back");
        deny(UNKNOWN, major, "unknown stat liveness rolls back");
        deny(BAD_STAT, major, "incomplete stat rolls back");
        deny(MEM_ESRCH, major, "memory ESRCH alone rolls back");
        deny(MEM_EPERM, major, "memory EPERM rolls back");
        deny(MEM_EFAULT, major, "memory EFAULT rolls back");
        deny(MISSING_FIELD, major, "missing status field rolls back");
        deny(BAD_STATE, major, "invalid status state rolls back");
        deny(FOREIGN, major, "foreign parent is unknown for preseed");
        deny(BAD_OPEN, major, "failed identity pin rolls back");
    }
    for (int major=13; major<=18; major+=5) {
        reset(STARTUP, major);
        d.backends.entries[0].meta.backend_type=PGWT_BT_CHECKPOINTER;
        CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed && !stored.cmd_open &&
              stat_opens == 0 && d.counters.exact_preseed_no_command_total == 0,
              "control coherent auxiliary is not a proof-seeded startup client");
    }
    reset(NULL_ENTRY, 18);
    CHECK(pgwt_exact_seed_all(&d, 9) == 0 && d.counters.exact_preseed_null_entry_total == 1,
          "NULL MyBEEntry proves no command");
    reset(NULL_ENTRY, 13);
    struct pgwt_pgbs_snapshot row;
    CHECK(pgwt_pgbs_read_preseed(42, 0x1000, 0x2000, &d.backend_status_layout, &row) ==
          PGWT_PGBS_PRESEED_ROW, "PG13 never reads a MyBEEntry proof");
    deny(UNRESOLVED, 13, "PG13 unresolved startup stays counted rollback");
    deny(ENTRY_EIO, 18, "MyBEEntry EIO is not absence");
    deny(ENTRY_ZERO, 18, "MyBEEntry short read is not absence");
    deny(UNKNOWN_AFTER, 18, "unknown pinned identity after memory read refuses NULL proof");
    reset(NORMAL, 18); map_failure=1;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && stat_opens == 0 &&
          d.counters.exact_preseed_failures_total == 1, "seed-map failure cannot classify as exit");
    reset(NULL_ENTRY, 18); map_failure=1;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && d.counters.exact_preseed_no_command_total == 0,
          "failed proof map write is not counted as seeded");
    reset(NULL_ENTRY, 18); setenv("PGWT_TEST_EXACT_PRESEED_FAIL_ONCE", "1", 1);
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && mem_reads == 0 && stat_reads == 0 &&
          d.counters.exact_preseed_failures_total == 1, "forced hook still rolls back before classification");
    unsetenv("PGWT_TEST_EXACT_PRESEED_FAIL_ONCE");
    for (int major=13; major<=18; major+=5) {
        reset(EIO_READ, major); have_seed=1;
        stored=(struct pgwt_exact_seed){.generation=9,.query_id=777,.cmd_open=1,.query_quality=PGWT_QUERY_QUALITY_REAL};
        CHECK(pgwt_exact_seed_backend(&d, 42, 9, false) != 0 &&
              stored.query_id == 777 && stored.cmd_open == 1,
              "late attach preserves coherent same-generation seed");
        CHECK(d.counters.exact_preseed_overwrite_refusals_total == 1,
              "late attach overwrite refusal counted");
        have_seed=0;
        CHECK(pgwt_exact_seed_backend(&d, 42, 9, false) != 0 && !have_seed,
              "late attach cannot invent incoherent first seed");
        stored.generation=8; have_seed=1;
        CHECK(pgwt_exact_seed_backend(&d, 42, 9, false) != 0 && stored.generation == 8 &&
              d.counters.exact_preseed_overwrite_refusals_total == 1,
              "older generation is not a same-generation overwrite refusal");
        edge_present=1;
        CHECK(pgwt_exact_seed_backend(&d, 42, 9, true) == 0 && stored.query_id == 456,
              "post-boundary exact edge remains authoritative");
    }
    retirement_seeds();
    torn_reread_cases();
    command_open_route_cases();
    refresh_diag_cases();
    /* UNPUBLISHED only: zero and stale-private caches, no sampler tick. */
    for (int major=14; major<=17; ++major) {
        for (int stale=0; stale<=1; ++stale) {
            reset(UNPUBLISHED, major);
            struct pgwt_backend *be = &d.backends.entries[0];
            be->wp_addr = stale ? 0x5000 : 0;
            be->wp_addr_shared = stale ? 0 : -1; wait_shared=0;
            resolved_wait_addr=0x6000;
            CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed &&
                  stored.query_id == 0 && !stored.cmd_open &&
                  d.counters.exact_preseed_unpublished_total == 1,
                  "refresh preserves unpublished proof 7");
            CHECK(be->wp_addr == 0x6000 && be->wp_addr_shared == 1,
                  "refreshed shared wait address stored");
            CHECK(resolve_calls == 1 && state_updates == 1 && have_state &&
                  wait_state.wait_event_addr == 0x6000 && wait_state.last_ts == 100 &&
                  !wait_state.last_event && !wait_state.last_query_id &&
                  d.counters.exact_preseed_wait_addr_refreshed_total == 1,
                  "refresh seeds state and counts once");
        }
    }
    for (int stale=0; stale<=1; ++stale) {
        for (int failure=0; failure<5; ++failure) {
            reset(UNPUBLISHED, 17);
            struct pgwt_backend *be = &d.backends.entries[0];
            uint64_t original = stale ? 0x5000 : 0;
            int original_shared = stale ? 0 : -1;
            be->wp_addr=original; be->wp_addr_shared=original_shared; wait_shared=0;
            resolved_wait_addr = failure == 0 ? 0 : 0x6000;
            fresh_wait_shared = failure == 1 ? 0 : failure == 2 ? -1 : 1;
            reuse_on_resolve = failure == 3;
            map_failure = failure == 4;
            CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
                  failure == 1 ? "fresh private wait address denied" :
                  failure == 3 ? "pid reused during refresh denied" : "failed refresh denied");
            CHECK(be->wp_addr == original && be->wp_addr_shared == original_shared,
                  "failed refresh leaves cached address untouched");
            CHECK(resolve_calls == 1 && !state_updates && !have_state &&
                  d.counters.exact_preseed_wait_addr_refreshed_total == 0 &&
                  d.counters.exact_preseed_unpublished_total == 0 &&
                  be->exact_unpublished_generation == 0,
                  "failed refresh publishes no state or counter");
        }
    }
    /* Existing state must survive the idempotent shared seed helper. */
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr=0;
    resolved_wait_addr=0x6000; have_state=1;
    wait_state=(struct pgwt_pid_state){.wait_event_addr=0x5000,.last_event=7,.last_query_id=456};
    CHECK(pgwt_exact_seed_all(&d, 9) == 0 && state_updates == 1 &&
          wait_state.wait_event_addr == 0x5000 && wait_state.last_event == 7 &&
          wait_state.last_query_id == 456 && d.counters.state_map_full_total == 0,
          "refresh state seed preserves captured state");
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr=0;
    resolved_wait_addr=0x6000; state_failure=1;
    CHECK(pgwt_exact_seed_all(&d, 9) == 0 && d.counters.state_map_full_total == 1 &&
          d.state_map_full_logged && !have_state,
          "refresh preserves sampler seed capacity diagnostics");
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr=0;
    resolved_wait_addr=0x6000;
    setenv("PGWT_TEST_SKIP_PRESEED_WAIT_ADDR_REFRESH", "1", 1);
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed && !resolve_calls &&
          d.backends.entries[0].wp_addr == 0 && !state_updates,
          "disabled refresh rolls back cold startup red control C");
    unsetenv("PGWT_TEST_SKIP_PRESEED_WAIT_ADDR_REFRESH");
    for (int major=13; major<=18; major+=5) {
        int cases[] = {UNPUBLISHED, NORMAL, STARTUP, NULL_ENTRY, EIO_READ};
        for (unsigned i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
            reset(cases[i], major); d.backends.entries[0].wp_addr=0;
            resolved_wait_addr=0x6000;
            int result = pgwt_exact_seed_all(&d, 9);
            CHECK((result == 0) == (cases[i] != UNPUBLISHED && cases[i] != EIO_READ),
                  "PG13 PG18 preseed result unchanged with cold cache");
            CHECK(!resolve_calls && !state_updates && d.backends.entries[0].wp_addr == 0 &&
                  d.counters.exact_preseed_wait_addr_refreshed_total == 0,
                  "PG13 PG18 never refresh wait address");
        }
    }
    puts("wait address refresh: zero/stale cache, zero/private/unreadable, PID reuse, map failure, PG13/PG18 controls PASS");
    for (int major=14; major<=17; major++) {
        reset(UNPUBLISHED, major);
        CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed &&
              stored.query_id == 0 && !stored.cmd_open &&
              stored.query_quality == PGWT_QUERY_QUALITY_NONE,
              "PG14-17 unpublished own slot seeds no command");
        CHECK(d.counters.exact_preseed_unpublished_total == 1 &&
              d.counters.exact_preseed_no_command_total == 1 &&
              d.exact_probes.preseed_no_command == 1 &&
              d.backends.entries[0].exact_unpublished_generation == 9,
              "unpublished proof counted with generation marker");
        CHECK(!resolve_calls && !state_updates &&
              d.counters.exact_preseed_wait_addr_refreshed_total == 0,
              "shared cached wait address does not refresh");
    }
    deny(UNPUBLISHED, 13, "PG13 unpublished proof forbidden");
    deny(UNPUBLISHED, 18, "PG18 unpublished proof forbidden");
    reset(NORMAL, 17);
    char hook_path[] = "/tmp/pgwt-preseed-hook-XXXXXX";
    int hook_fd = mkstemp(hook_path);
    CHECK(hook_fd >= 0, "create temporary read-failure selector");
    FILE *hook_file = fdopen(hook_fd, "w");
    CHECK(hook_file != NULL, "open selector stream");
    fprintf(hook_file, "42\n"); fclose(hook_file);
    setenv("PGWT_TEST_PRESEED_READ_FAILURE_PID_FILE", hook_path, 1);
    int hook_result = pgwt_exact_seed_all(&d, 9);
    unsetenv("PGWT_TEST_PRESEED_READ_FAILURE_PID_FILE"); unlink(hook_path);
    CHECK(hook_result != 0 && !have_seed && d.counters.exact_preseed_failures_total == 1,
          "established pg_sleep read failure rolls back red control A");
    deny(EIO_READ, 17, "FAILED is not UNPUBLISHED red control A");
    deny(WRONG_PID, 17, "unpublished cannot name another pid");
    deny(UNPUBLISHED_TORN, 17, "unpublished torn row rolls back");
    deny(OUTSIDE, 17, "unpublished pointer outside mapping rolls back");
    deny(OFF_STRIDE, 17, "unpublished off-stride pointer rolls back");
    reset(UNPUBLISHED, 17); start_change_on_read=4;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "unpublished changed proc pin rolls back");
    reset(UNPUBLISHED, 17); d.backends.entries[0].wp_addr=0;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "unpublished unresolved wait address rolls back");
    reset(UNPUBLISHED, 17); wait_shared=0;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "unpublished local wait address rolls back");
    reset(UNPUBLISHED, 17); wait_shared=-1;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "unpublished unknown wait mapping rolls back");
    reset(UNPUBLISHED, 17); fresh_generation++;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed,
          "unpublished stale mapping generation rolls back");
    reset(UNPUBLISHED, 17); map_failure=1;
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed &&
          d.counters.exact_preseed_unpublished_total == 0 &&
          d.backends.entries[0].exact_unpublished_generation == 0,
          "failed unpublished seed write publishes no proof or marker");
    for (int major=17; major<=18; major++) {
        reset(RACE_OK, major);
        CHECK(pgwt_exact_seed_all(&d, 9) == 0 && have_seed &&
              d.counters.exact_preseed_reread_total == 1 && mem_opens == 3 &&
              !stored.cmd_open && !stored.query_id &&
              d.counters.exact_preseed_no_command_total == 0,
              "publication race reread succeeds once without proof seed");
    }
    reset(RACE_FAIL, 18);
    CHECK(pgwt_exact_seed_all(&d, 9) != 0 && !have_seed && mem_opens == 3 &&
          d.counters.exact_preseed_reread_total == 1 &&
          d.counters.exact_preseed_failures_total == 1,
          "failed reread rolls back after one attempt");
    deny(RACE_OK, 13, "PG13 publication race retry remains excluded");
    int no_retry[] = {EIO_READ, TORN, WRONG_PID, STARTUP_OPEN, DB_ONLY, USER_ONLY};
    for (unsigned i=0; i<sizeof(no_retry)/sizeof(no_retry[0]); i++) {
        reset(no_retry[i], 18);
        CHECK(pgwt_exact_seed_all(&d, 9) != 0 && d.counters.exact_preseed_reread_total == 0,
              "only own-pid context race shape retries");
    }
    printf("preseed proof/rollback/late-attach: %u assertions PASS\n", checks);
    return 0;
}
'''


def preseed_cases(mutants):
    import re
    def extract(source, name):
        match = re.search(r"^(?:static )?(?:int|void|bool|ssize_t) " + name +
                          r"\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
        assert match, name
        return match[0]
    pgbs = (ROOT / "src/backend_status_layout.c").read_text()
    exact = (ROOT / "src/exact_probe.c").read_text()
    cmdline = (ROOT / "src/cmdline.c").read_text()
    functions = "\n\n".join([
        extract(cmdline, "pgwt_backend_no_command_fd"),
        extract((ROOT / "src/sampler.c").read_text(), "pgwt_sampler_seed_state_entry"),
        *(extract(pgbs, n) for n in ("pread_exact", "read_entry_pointer_fd", "read_field_value", "read_snapshot_fd",
            "open_mem", "pgwt_pgbs_state_is_cmd_open", "snapshot_untorn",
            "snapshot_coherent_identity", "snapshot_usable", "pgwt_pgbs_derive_sampled_attr",
            "pgwt_pgbs_derive_sampled_activity", "read_sampled_attr_at_fd",
            "pgwt_pgbs_read_sampled_attr_at", "pgwt_pgbs_read_sampled_attr", "unpublished_slot", "pgwt_pgbs_read_preseed",
            "pinned_start_time", "pgwt_pgbs_preseed_pin", "pgwt_pgbs_retirement_proof", "pgwt_pgbs_read_retirement")),
        *(extract(exact, n) for n in ("test_preseed_read_failure", "pgwt_exact_seed_backend",
                                     "test_preseed_hold_torn", "preseed_row_read", "preseed_reread_torn",
                                     "log_refresh_not_shared", "seed_no_command",
                                     "pgwt_exact_seed_all", "pgwt_exact_seed_clear")),
    ])
    control = (ROOT / "src/control.c").read_text()
    for counter in ("exact_preseed_no_command_total", "exact_preseed_gone_total",
                    "exact_preseed_exiting_total", "exact_preseed_null_entry_total",
                    "exact_preseed_startup_total", "exact_preseed_retired_zero_total",
                    "exact_preseed_unpublished_total", "exact_preseed_reread_total",
                    "exact_preseed_wait_addr_refreshed_total", "exact_preseed_torn_exhausted_total",
                    "exact_preseed_retired_other_pid_total", "exact_preseed_failures_total",
                    "exact_preseed_overwrite_refusals_total", "exact_preseed_late_attach_failures_total"):
        assert f'cjson_add_uint64(root, "{counter}", ctr->{counter});' in control, counter
    assert 'cjson_add_uint64(root, "exact_preseed_wait_addr_refreshed_total", d->counters.exact_preseed_wait_addr_refreshed_total);' in control
    assert 'cjson_add_uint64(root, "exact_preseed_torn_exhausted_total", d->counters.exact_preseed_torn_exhausted_total);' in control
    assert '"exact_preseed_no_command_entries"' in control
    assert 'd->exact_probes.preseed_generation == d->escalation.generation' in control
    variants = [("preseed_baseline", functions, None)]
    if mutants:
        mutations = [
            ("m1_FAILED_as_UNPUBLISHED", "if (test_preseed_read_failure(be->pid))\n            rc = PGWT_PGBS_PRESEED_FAILED;",
             "if (test_preseed_read_failure(be->pid))\n            rc = PGWT_PGBS_PRESEED_UNPUBLISHED;",
             "FAIL established pg_sleep read failure rolls back red control A"),
            ("refresh_drop_shared_check", "if (shared == 1) {",
             "if (wait_addr) {", "FAIL non-shared refresh logs the diagnostic and denies"),
            ("refresh_prove_without_storing", "be->wp_addr = refreshed_wait_addr;",
             "(void)refreshed_wait_addr;", "FAIL refreshed shared wait address stored"),
            ("refresh_store_before_final_pin", "refreshed_wait_addr = wait_addr;",
             "refreshed_wait_addr = be->wp_addr = wait_addr;",
             "FAIL failed refresh leaves cached address untouched"),
            ("m2_drop_unpublished_pid_zero", "snapshot->procpid == 0 && unpublished_slot(pid, base, layout)",
             "unpublished_slot(pid, base, layout)", "FAIL unpublished cannot name another pid"),
            ("m3_drop_unpublished_mapping_stride", "unpublished_slot(pid, base, layout)",
             "(layout->major >= 14 && layout->major <= 17 && (unpublished_slot(pid, base, layout) || true))",
             "FAIL unpublished pointer outside mapping rolls back"),
            ("m4_unbounded_reread", "return pgwt_exact_seed_backend(d, be->pid, generation, true);",
             "while (pgwt_exact_seed_backend(d, be->pid, generation, true) != 0) {} return 0;",
             "FAIL reread bounded to one attempt"),
            ("m5_no_reread", "return pgwt_exact_seed_backend(d, be->pid, generation, true);",
             "return -1;", "FAIL live published backend seeds its open command through the coherent reread"),
            ("classify_auxiliaries", "bool needs_attr = !be->meta_parsed ||",
             "bool needs_attr = true ||",
             "FAIL logger NULL MyBEEntry alive is not proof-seeded"),
            ("every_failure_is_exit", "    if (proof <= 0) {\n", "    if (proof <= 0) proof = 1;\n    if (false) {\n",
             "FAIL live initialised backend rolls back"),
            ("proof_counter_dropped", "d->counters.exact_preseed_no_command_total++;", ";",
             "FAIL proof seed counted lifetime and generation"),
            ("PF_EXITING_mask_wrong", "flags & 0x4ULL", "flags & 0x8ULL",
             "FAIL positive proof allows escalation"),
            ("NULL_check_inverted", "return *base == 0 ?", "return *base != 0 ?",
             "FAIL control coherent command preseed"),
            ("MyBEEntry_off_by_8", "sizeof(*base), addr) < 0)",
             "sizeof(*base), addr + 8) < 0)", "FAIL control coherent command preseed"),
            ("accept_foreign_row_pid", "snapshot->procpid == (uint32_t)expected_pid &&",
             "snapshot->procpid > 0 &&", "FAIL foreign row PID rolls back"),
            ("retirement_candidate", "layout->major != 13 || !accepted || !accepted->address ||", "layout->major != 13 || !accepted ||",
             "FAIL retirement candidate has no acceptance"),
            ("retirement_record_pid", "accepted->pid != pid || !accepted->start_time ||",
             "!accepted->start_time ||", "FAIL retirement record belongs to pid"),
            ("retirement_record_start", "accepted->pid != pid || !accepted->start_time ||",
             "accepted->pid != pid ||", "FAIL retirement record needs start time"),
            ("retirement_generation_zero", "        !accepted->shm_generation ||\n", "",
             "FAIL retirement record needs generation"),
            ("retirement_generation", "        accepted->shm_generation != layout->shm_generation ||\n", "",
             "FAIL retirement record generation matches layout"),
            ("retirement_fresh_generation", "        accepted->shm_generation != current_generation ||\n",
             "        ((void)current_generation, false) ||\n",
             "FAIL retirement fresh generation matches record"),
            ("retirement_recovery", "        layout->pg13_recovery != PGWT_PGBS_PG13_READY ||\n        before",
             "        before", "FAIL retirement recovery pending refused"),
            ("retirement_start_before", "before != accepted->start_time || after != accepted->start_time",
             "((void)before, false) || after != accepted->start_time", "FAIL retirement start time before matches"),
            ("retirement_start_after", "before != accepted->start_time || after != accepted->start_time",
             "before != accepted->start_time || ((void)after, false)", "FAIL retirement start time after matches"),
            ("retirement_counter_odd", "!snapshot_untorn(row->changecount_before, row->changecount_after)",
             "row->changecount_before != row->changecount_after", "FAIL retirement odd counter refused"),
            ("retirement_counter_unequal", "!snapshot_untorn(row->changecount_before, row->changecount_after)",
             "(row->changecount_after & 1u)", "FAIL retirement unequal counter refused"),
            ("retirement_fields", "        (row->read_mask & required) != required ||\n",
             "        ((void)required, false) ||\n", "FAIL retirement incomplete fields refused"),
            ("retirement_pid_equal", "row->procpid == (uint32_t)pid)", "false)",
             "FAIL retirement live pid refused"),
            ("retirement_read_failed", "result != PGWT_PGBS_PRESEED_RETIRED || !row ||",
             "((void)result, false) || !row ||", "FAIL retirement failed read refused"),
            ("retirement_cache_only", "&be->sampled_reader.accepted, &d->backend_status_layout",
             "&(struct pgwt_pgbs_acceptance){.address=be->pgbs_addr,.pid=be->pid,.start_time=123,.shm_generation=d->backend_status_layout.shm_generation}, &d->backend_status_layout",
             "FAIL PG13 address only in backend cache rolls back"),
            ("retirement_zero_counter", "d->counters.exact_preseed_retired_zero_total++;", "(void)0;",
             "FAIL PG13 retirement counters distinguish zero and successor"),
            ("retirement_other_counter", "d->counters.exact_preseed_retired_other_pid_total++;", "(void)0;",
             "FAIL PG13 retirement counters distinguish zero and successor"),
            ("retirement_pinned_before_ignored",
             "if (pinned_start_time(stat_fd, &before) != 0) return PGWT_PGBS_RETIRED_NONE;",
             "if (pinned_start_time(stat_fd, &before) != 0) return PGWT_PGBS_RETIRED_NONE; before=accepted->start_time;",
             "FAIL PG13 pinned start changed before row rolls back"),
            ("retirement_pinned_after_ignored",
             "if (pinned_start_time(stat_fd, &after) != 0) return PGWT_PGBS_RETIRED_NONE;",
             "if (pinned_start_time(stat_fd, &after) != 0) return PGWT_PGBS_RETIRED_NONE; after=accepted->start_time;",
             "FAIL PG13 pinned start changed after row rolls back"),
            ("retirement_cached_mapping_generation",
             "struct pgwt_pgbs_mapping map = pg13_main_mapping(postmaster_pid, layout);",
             "struct pgwt_pgbs_mapping map = pg13_main_mapping(postmaster_pid, layout); map.identity=layout->shm_generation;",
             "FAIL PG13 fresh mapping rejects stale interval generation"),
            ("retirement_start_field_wrong", "if (field == 22) {", "if (field == 21) {",
             "FAIL PG13 accepted retirement seeds closed after FATAL mid-command without close edge"),
            ("retirement_wrong_recorded_address", "pgwt_pgbs_read_preseed(pid, 0, accepted->address, layout, &row)",
             "pgwt_pgbs_read_preseed(pid, 0, 0x2000, layout, &row)",
             "FAIL PG13 live mid-query injected wrong cached address rolls back"),
            # #616 r6: torn re-read, command-open route, refresh diagnostic.
            ("a1_reader_torn_as_unpublished", "rc = PGWT_PGBS_PRESEED_TORN; /* never a row, never UNPUBLISHED */",
             "rc = PGWT_PGBS_PRESEED_UNPUBLISHED;", "FAIL torn beyond the bound denies and counts"),
            ("a2_exhausted_torn_as_unpublished", "        return rc;\n    }\n    return PGWT_PGBS_PRESEED_TORN;",
             "        return rc;\n    }\n    return PGWT_PGBS_PRESEED_UNPUBLISHED;",
             "FAIL torn beyond the bound denies and counts"),
            ("b1_torn_reread_unbounded", "while (d->exact_probes.preseed_torn_pauses < PGWT_PRESEED_TORN_PAUSES)",
             "while (true)", "FAIL torn reread bounded"),
            ("b2_torn_reread_without_pin", "if (pgwt_pgbs_preseed_pin(pinned, &now) != 0 || now != pin)",
             "if (((void)now, (void)pin, false))", "FAIL torn reread under a changed identity pin denies"),
            ("b3_torn_budget_not_per_attempt", "    d->exact_probes.preseed_torn_pauses = 0;\n", "",
             "FAIL torn reread budget resets per escalation attempt"),
            ("torn_counter_dropped", "d->counters.exact_preseed_torn_exhausted_total++;", "(void)0;",
             "FAIL torn beyond the bound denies and counts"),
            ("torn_hold_hook_inert", "    if (hold_torn && rc != PGWT_PGBS_PRESEED_FAILED &&",
             "    if (false && hold_torn && rc != PGWT_PGBS_PRESEED_FAILED &&",
             "FAIL held odd change counter denies escalation red control D"),
            ("c1_command_open_skips_coherence", "return pgwt_exact_seed_backend(d, be->pid, generation, true);",
             "return pgwt_pgbs_state_is_cmd_open(&d->backend_status_layout, row.state) ? "
             "((void)pgwt_exact_seed_backend(d, be->pid, generation, true), 0) : "
             "pgwt_exact_seed_backend(d, be->pid, generation, true);",
             "FAIL command-open reread failure rolls back"),
            ("c2_command_open_not_routed", "rc == PGWT_PGBS_PRESEED_ROW && row.databaseid && row.userid) {",
             "rc == PGWT_PGBS_PRESEED_ROW && row.databaseid && row.userid &&\n"
             "                 !pgwt_pgbs_state_is_cmd_open(&d->backend_status_layout, row.state)) {",
             "FAIL live published backend seeds its open command through the coherent reread"),
            ("diag_silent", "                        log_refresh_not_shared(d, be->pid, wait_addr, read_errno, shared);",
             "                        (void)read_errno; (void)log_refresh_not_shared;",
             "FAIL non-shared refresh diagnostic carries pointer errno shared and range"),
            ("diag_lost_errno", "int read_errno = errno;", "int read_errno = 0;",
             "FAIL non-shared refresh diagnostic carries pointer errno shared and range"),
            ("diag_not_rate_limited", "        d->exact_probes.refresh_diag_suppressed++;\n        return;",
             "        d->exact_probes.refresh_diag_suppressed++;", "FAIL refresh diagnostic is rate limited"),
            ("late_attach_overwrite", "if (!coherent && !auxiliary) {\n        uint32_t key", "if (!coherent && !auxiliary && require_coherent) {\n        uint32_t key",
             "FAIL late attach preserves coherent same-generation seed"),
        ]
        for name, old, new, intended in mutations:
            assert functions.count(old) == 1, (name, functions.count(old))
            variants.append((name, functions.replace(old, new), intended))
    with tempfile.TemporaryDirectory(prefix="pgwt-preseed-") as tmp:
        for name, source, intended in variants:
            cfile, binary = Path(tmp) / f"{name}.c", Path(tmp) / name
            cfile.write_text(PRESEED_HARNESS.replace("@FUNCTIONS@", source))
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
                if result.returncode: print(result.stderr, end="")
                assert result.returncode == 0, result
    print(f"preseed: baseline PASS; {len(variants)-1} mutants killed")



WAIT_SEED_HARNESS = r'''
#include "daemon.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct pg_wait_tracer_bpf { struct { void *state_map; } maps; };
static struct pgwt_daemon d;
static struct pgwt_pid_state written;
static int updates, opens, reads, closes, checks;
#define CHECK(x, msg) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s\n", msg); exit(1); } } while (0)
#define PGWT_TEST_STALE_WEI (((uint32_t)PG_WAIT_TIMEOUT << 24) | 2)
static int fake_open(const char *path, int flags)
{ CHECK(!strcmp(path, "/proc/42/mem") && flags == O_RDONLY, "wait seed target memory"); ++opens; return 100; }
static ssize_t fake_pread(int fd, void *buf, size_t size, off_t offset)
{ CHECK(fd == 100 && size == 4 && offset == 0x5000, "read actual shared wait word");
  uint32_t lock = 0x03000001; memcpy(buf, &lock, size); ++reads; return (ssize_t)size; }
static int fake_close(int fd) { CHECK(fd == 100, "close wait memory"); ++closes; return 0; }
#define open fake_open
#define pread fake_pread
#define close fake_close
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *table, pid_t pid)
{ return table->count == 1 && pid == 42 ? &table->entries[0] : NULL; }
static int bpf_map__fd(void *map) { (void)map; return 1; }
static int bpf_map_lookup_elem(int fd, const void *key, void *value)
{ (void)fd; (void)key; (void)value; return -1; }
#define BPF_ANY 0
static int bpf_map_update_elem(int fd, const void *key, const void *value, int flags)
{ CHECK(fd == 1 && *(const uint32_t *)key == 42 && flags == BPF_ANY, "actual wait seed map");
  ++updates; memcpy(&written, value, sizeof(written)); return 0; }
static void report_state_map_full(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; (void)pid; CHECK(0, "wait seed map unexpectedly full"); }
void pgwt_event_full_name(uint32_t event, char *buf, size_t size)
{ (void)event; snprintf(buf, size, "test-event"); }
@FUNCTION@
static void seed(int expected, bool sweep, const char *message)
{
    opens=reads=closes=updates=0;
    memset(&written, 0, sizeof(written));
    CHECK(preseed_state_map(&d, 42, 0x5000, 100, sweep) == 0, message);
    CHECK(updates == expected && opens == expected && reads == expected && closes == expected, message);
    if (expected)
        CHECK(written.last_event == 0x03000001 && written.last_ts == 100 &&
              written.wait_event_addr == 0x5000 && written.wp_live &&
              !written.cmd_open && !written.last_query_id,
              "unpublished no-command proof preserves real in-flight Lock wait");
}
int main(void)
{
    static struct pg_wait_tracer_bpf skel;
    d.skel=&skel; d.backends.count=1; d.escalation.generation=9;
    d.backends.entries[0].pid=42;
    d.backends.entries[0].exact_unpublished_generation=9;
    seed(1, false, "unpublished real wait is seeded by default");
    setenv("PGWT_TEST_SKIP_UNPUBLISHED_WAIT_SEED", "1", 1);
    seed(0, false, "red control B skips actual wait seed");
    seed(1, true, "red control B retains sweep repair");
    d.backends.entries[0].exact_unpublished_generation=8;
    seed(1, false, "red control B requires same generation");
    d.backends.entries[0].exact_unpublished_generation=0;
    seed(1, false, "red control B requires unpublished proof");
    unsetenv("PGWT_TEST_SKIP_UNPUBLISHED_WAIT_SEED");
    seed(1, false, "established backend wait seeding preserved");
    printf("unpublished wait seed: 6 cases, %d assertions PASS; red control B hook fired on host\n", checks);
    return 0;
}
'''


def wait_seed_cases(mutants):
    source = function((ROOT / "src/backend.c").read_text(), "preseed_state_map")
    variants = [("wait_seed_baseline", source, None)]
    if mutants:
        old = 'if (!sweep_reseed && getenv("PGWT_TEST_SKIP_UNPUBLISHED_WAIT_SEED")) {'
        assert source.count(old) == 1
        variants.append(("wait_seed_control_B_disabled", source.replace(old, "if (false) {"),
                         "FAIL red control B skips actual wait seed"))
    with tempfile.TemporaryDirectory(prefix="pgwt-wait-seed-") as temporary:
        for label, body, intended in variants:
            cfile, binary = Path(temporary) / f"{label}.c", Path(temporary) / label
            cfile.write_text(WAIT_SEED_HARNESS.replace("@FUNCTION@", body))
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall",
                            "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                            *shlex.split(os.environ.get("CFLAGS", "")), str(cfile), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if intended:
                assert result.returncode == 1 and intended in result.stderr, result
                print(f"{label}: KILLED (built, reached intended assertion: {intended})")
            else:
                assert result.returncode == 0, result
                print(result.stdout, end="")


def live_churn(args):
    """Drive a prepared Linux daemon; no retries, sleeps or relaxed bounds.

    Start this branch's tracer with --mode tiered --escalation-budget unlimited,
    --anomaly-aas-factor 1000000 --anomaly-lock-fraction 1,
    --pid matching the DSN's postmaster, and a fresh -T directory. Initialise
    the DSN's pgbench database first. Keep its daemon log and this JSON output.
    Run once per OS (EL8/Ubuntu) and major (13/18). PG13 denials are reported
    and must equal its preseed-failure counter; PG14+ must have zero denials.
    """
    import contextlib
    import json
    import signal
    import socket
    import sys
    assert sys.platform == "linux", "live churn requires Linux"
    assert args.rounds > 0 and args.cpu_clients > 0

    def ctl(cmd, **fields):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(30)
            client.connect(args.live_socket)
            client.sendall(json.dumps({"cmd": cmd, **fields}).encode() + b"\n")
            data = b""
            while b"\n" not in data:
                chunk = client.recv(65536)
                assert chunk, "empty control reply"
                data += chunk
        result = json.loads(data.splitlines()[0])
        if cmd != "escalate":
            assert result.get("ok") is True, result
        return result

    def stop_group(proc):
        # A loop's current psql child belongs to this group too.
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)
            raise AssertionError("load process failed to stop")

    def psql(sql):
        import select
        probe.stdin.write(sql.rstrip(";") + ";\n")
        probe.stdin.flush()
        assert select.select([probe.stdout], [], [], 30)[0], "validation query timed out"
        row = probe.stdout.readline()
        assert row, "validation psql exited; see stderr"
        return row.strip()

    # Keep this validation client idle through the control arm: closing it
    # immediately before measurement would itself manufacture an exit race.
    with subprocess.Popen(["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", args.dsn],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, start_new_session=True) as probe:
        try:
            major = int(psql("SHOW server_version_num")) // 10000
            assert major == 13 or major >= 14, major
            status = ctl("status")
            assert status["mode"] == "tiered" and status["tier"] == "sampled", status
            assert status["escalation_budget_unlimited"] is True, status
            assert status["pgbackend_layout_validation"] == "validated", status
            # Bind the load's server to the daemon's postmaster, not just its version.
            assert int(psql("SELECT split_part(pg_read_file('postmaster.pid'), E'\\n', 1)")) == status["pg_pid"]
            psql("SELECT count(*) FROM pgbench_accounts")
            proof_counters = ("exact_preseed_gone_total", "exact_preseed_exiting_total",
                              "exact_preseed_null_entry_total", "exact_preseed_startup_total",
                              "exact_preseed_retired_zero_total", "exact_preseed_retired_other_pid_total",
                              "exact_preseed_unpublished_total")
            counters = ("exact_preseed_no_command_total", "exact_preseed_failures_total",
                        "exact_preseed_wait_addr_refreshed_total",
                        "escalation_windows_total", "escalation_denied_total", *proof_counters)
            summaries = []
            with tempfile.TemporaryDirectory(prefix="pgwt616-churn-") as tmp:
                for condition in ("idle", "cpu", "pgbench_connect", "psql_connect", "churn_cpu"):
                    with contextlib.ExitStack() as stack:
                        processes = []
                        def start(command):
                            log = stack.enter_context(open(Path(tmp) / f"{condition}-{len(processes)}.log", "w+"))
                            proc = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True)
                            stack.callback(stop_group, proc)
                            processes.append((proc, log))

                        if condition in ("cpu", "churn_cpu"):
                            start(["pgbench", "-n", "-c", str(args.cpu_clients), "-j", "4",
                                   "-T", "86400", args.dsn])
                        if condition in ("pgbench_connect", "churn_cpu"):
                            start(["pgbench", "-n", "-C", "-c", "16", "-j", "4", "-S", "-T", "86400", args.dsn])
                        if condition == "psql_connect":
                            start([sys.executable, "-c", "import subprocess,sys\nwhile True:\n subprocess.run(['psql','-XqAt','-v','ON_ERROR_STOP=1','-d',sys.argv[1],'-c','SELECT 1'],check=True)", args.dsn])
                        before = ctl("metrics")
                        ok = denied = generation_proofs = 0
                        for _ in range(args.rounds):
                            for proc, log in processes:
                                if proc.poll() is not None:
                                    log.seek(0)
                                    raise AssertionError(f"{condition} load exited {proc.returncode}: {log.read()}")
                            reply = ctl("escalate", duration_s=30)
                            if reply.get("ok") is True:
                                ok += 1
                                current = ctl("metrics")
                                assert current["exact_probe_generation"] > 0, current
                                generation_proofs += current["exact_preseed_no_command_entries"]
                                ctl("deescalate")
                            else:
                                assert reply.get("error") == "coherent exact preseed failed; escalation rolled back", reply
                                denied += 1
                            assert ctl("metrics")["exact_preseed_no_command_entries"] == 0
                        after = ctl("metrics")
                        delta = {key: after[key] - before[key] for key in counters}
                        assert delta["exact_preseed_failures_total"] == denied, delta
                        assert delta["escalation_denied_total"] == denied, delta
                        assert delta["escalation_windows_total"] == ok, delta
                        assert sum(delta[key] for key in proof_counters) == delta["exact_preseed_no_command_total"], delta
                        assert generation_proofs <= delta["exact_preseed_no_command_total"], delta
                        summary = dict(condition=condition, pg_major=major, rounds=args.rounds,
                                       ok=ok, denied=denied, generation_proofs=generation_proofs, counters=delta)
                        print(json.dumps(summary), flush=True)
                        summaries.append(summary)
            if major >= 14:
                assert all(row["denied"] == 0 for row in summaries), summaries
            assert sum(row["counters"]["exact_preseed_no_command_total"]
                       for row in summaries[2:]) > 0, "churn did not exercise any positive proof"
            # Stable control: auxiliary rows must not inflate the client proof count.
            assert summaries[0]["counters"]["exact_preseed_no_command_total"] == 0, summaries[0]
            print("live preseed churn: PASS", flush=True)
        finally:
            stop_group(probe)


if __name__ == "__main__":
    main()
