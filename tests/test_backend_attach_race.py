#!/usr/bin/env python3
"""Production attach/fork/liveness bodies with injected I/O, on Mac or Linux.

Uses the portable backend harness's unchanged-function extraction. No kernel
watchpoints are opened. --mutants requires a build and the intended assertion.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_backend_init_order import ROOT, function

HARNESS = r'''
#include "daemon.h"
#include "backend_init.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct pg_wait_tracer_bpf { struct { void *on_watchpoint, *on_bootstrap; } progs; };
static struct pgwt_daemon d;
static int attempts, enables, closes, stat_opens, stat_closes, stat_reads;
static int target_state, operation_failed, enable_failure, open_error, anchor_error;
static int resolve_ready, direct_inits, replacement;
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
{ (void)op; (void)pid; (void)ns; }
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
{ CHECK(fd == 99, "close failed watchpoint"); ++closes; }
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
{ (void)ctx; (void)pid; return resolve_ready ? 1234 : 0; }
int pgwt_handle_init(struct pgwt_daemon *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{ (void)ctx; (void)pid; (void)addr; (void)ts; ++direct_inits; return 0; }
static int pgwt_addr_is_shared(pid_t pid, uint64_t ptr)
{ (void)pid; (void)ptr; return 1; }
void pgwt_pending_init_add(struct pgwt_backend_table *bt, struct pgwt_backend *be)
{ (void)bt; (void)be; }

@FUNCTIONS@

static void reset(int state)
{
    static struct pg_wait_tracer_bpf skel;
    memset(&d, 0, sizeof(d));
    d.skel = &skel; d.postmaster_pid = 7; d.my_wait_ptr_addr = 1234;
    attempts = enables = closes = stat_opens = stat_closes = stat_reads = 0;
    operation_failed = enable_failure = anchor_error = resolve_ready = direct_inits = replacement = 0;
    open_error = ESRCH; target_state = state;
}
static void check_counts(int state)
{
    int gone = state == GONE || state == ZOMBIE || state == FOREIGN || state == REUSED;
    CHECK(d.counters.wp_attach_failures_total == (uint64_t)!gone,
          "live or unknown failure stays a real failure");
    CHECK(d.counters.wp_attach_exit_race_total == (uint64_t)gone,
          "confirmed exit counted separately");
    CHECK(attempts == 1, "exactly one hardware attempt");
    CHECK(stat_opens == stat_closes, "retained handles closed");
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
            CHECK(stat_reads == 1, "post-failure liveness read");

            reset(state); open_error = errors[i];
            CHECK(pgwt_handle_fork(&d, 42) == -1, "bootstrap failure returned");
            check_counts(state);
            CHECK(stat_reads == 2, "eligibility and post-failure reads");
        }
        reset(state); enable_failure = 1;
        struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
        CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "enable failure returned");
        check_counts(state);
        CHECK(enables == 1 && closes == 1 && be.wp_fd == -1, "enable failure cleans up");
    }
    puts("real open, bootstrap open, enable: gone +1 race/+0 failures; live/unknown +1 failure: PASS");
    puts("same-parent PID reuse retains original handle; errno independent classification: PASS");

    int errors_before[] = {ENOENT, ESRCH, EACCES, EMFILE};
    for (size_t i = 0; i < sizeof(errors_before)/sizeof(errors_before[0]); ++i) {
        reset(LIVE); anchor_error = errors_before[i];
        struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
        CHECK(pgwt_attach_backend_watchpoint(&d, &be) == -1, "failed identity open handled");
        int gone = anchor_error == ENOENT || anchor_error == ESRCH;
        CHECK(d.counters.wp_attach_exit_race_total == (uint64_t)gone &&
              d.counters.wp_attach_failures_total == (uint64_t)!gone,
              "only missing proc identity proves exit");
    }
    reset(LIVE); anchor_error = EACCES;
    CHECK(pgwt_handle_fork(&d, 42) == -1, "unknown lifecycle eligibility refused");
    CHECK(attempts == 0 && d.counters.wp_attach_failures_total == 1 &&
          d.counters.wp_attach_exit_race_total == 0, "validation refusal remains a failure");
    reset(LIVE); resolve_ready = 1;
    CHECK(pgwt_handle_fork(&d, 42) == 0 && direct_inits == 1, "bootstrap direct-init fallback");
    CHECK(d.counters.wp_attach_failures_total == 0 && d.counters.wp_attach_exit_race_total == 0,
          "successful fallback not counted as failure");
    CHECK(stat_opens == stat_closes, "fallback closes retained identity");
    reset(LIVE); enable_failure = 2;
    struct pgwt_backend be = {.pid=42, .wp_addr=1234, .wp_fd=-1};
    CHECK(pgwt_attach_backend_watchpoint(&d, &be) == 0, "successful attach");
    CHECK(d.counters.wp_attach_failures_total == 0 && d.counters.wp_attach_exit_race_total == 0 &&
          stat_opens == stat_closes, "success closes identity without failure counts");
    puts("missing/unknown identity, validation refusal, direct fallback and success: PASS");
    return 0;
}
'''


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
    for name in ("wp_attach_failures_total", "wp_attach_exit_race_total"):
        assert f'cjson_add_uint64(root, "{name}",' in control
    variants = [("baseline", functions, None)]
    if args.mutants:
        needle = "if (pgwt_backend_target_live_fd(target_fd, d->postmaster_pid) == 0)"
        assert functions.count(needle) == 1
        variants.append(("every_failure_is_exit_race", functions.replace(needle, "if (1)"),
                         "FAIL live or unknown failure stays a real failure"))
    with tempfile.TemporaryDirectory(prefix="pgwt-attach-race-") as tmp:
        for name, source, intended in variants:
            cfile, binary = Path(tmp) / f"{name}.c", Path(tmp) / name
            cfile.write_text(HARNESS.replace("@FUNCTIONS@", source))
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
    print("test_backend_attach_race: PASS")


if __name__ == "__main__":
    main()
