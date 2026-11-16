#!/usr/bin/env python3
"""Portable attribution syscall budget and mutations; all outputs stay in /tmp."""
import os
import contextlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import struct
import tempfile
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent


def run(args):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def perf_gate_cases():
    from sampler_batch_perf import assess, parse_trace
    clean = "1000.000 ( 0.001 ms): 77 process_vm_readv(pid: 42) = 24\n"
    counts = parse_trace(clean, 999, 1001)
    for major, population, minimum, bound in ((13, 512, 3, 5), (17, 512, 3, 5),
                                             (18, 512, 3, 5), (13, 1024, 5, 7),
                                             (17, 1024, 4, 6), (18, 1024, 4, 6)):
        counts["process_vm_readv"] = minimum * 200
        result = assess(counts, major, population, 200, 0, 500)
        assert result["vm_calls_per_tick_bound"] == bound and not result["errors"]
        assert result["vm_calls_per_tick_minimum"] == minimum
        counts["process_vm_readv"] -= 1
        assert any(" < " in e for e in assess(counts, major, population, 200, 0, 500)["errors"])
        counts["process_vm_readv"] = bound * 200
        assert not assess(counts, major, population, 200, 0, 500)["errors"]
        counts["process_vm_readv"] += 1
        assert any(" > " in e for e in assess(counts, major, population, 200, 0, 500)["errors"])
    legacy = ("1000.000 ( 0.001 ms): 77 openat(dfd: CWD, filename: /proc/123/mem) = 9\n"
              "1000.001 ( 0.001 ms): 77 pread64(fd: 9, buf: 0) = 4\n"
              "1000.002 ( 0.001 ms): 77 close(fd: 9) = 0\n") + clean
    counts = parse_trace(legacy, 999, 1001)
    counts["process_vm_readv"] = 600
    assert counts["mem_open"] == counts["mem_pread"] == counts["mem_close"] == 1
    assert len(assess(counts, 18, 512, 200, 0, 500)["errors"]) == 4
    interrupted = ("1000.000 (         ): 77 process_vm_readv(pid: 42) ...\n"
                   "1000.000 ( 0.001 ms): 77 ... [continued]: process_vm_readv()) = 24\n")
    assert parse_trace(interrupted, 999, 1001)["process_vm_readv"] == 1
    for invalid in ("", clean + "LOST 2 events!", "1000.000 ( 0.001 ms): 77 close(fd: 3) ..."):
        try:
            parse_trace(invalid, 999, 1001)
        except AssertionError:
            pass
        else:
            raise AssertionError("incomplete perf evidence accepted")
    counts = parse_trace(clean, 999, 1001)
    counts["process_vm_readv"] = 1201
    assert assess(counts, 18, 512, 200, 0, 500)["errors"]
    counts["process_vm_readv"] = 600
    assert assess(counts, 18, 512, 200, 103, 500)["errors"]
    assert assess(counts, 18, 512, 200, 0, 0)["errors"]
    counts["process_vm_readv"] = 200  # wait words only, status batch disabled
    disabled = assess(counts, 18, 512, 200, 0, 0)
    assert any(" < " in e for e in disabled["errors"])
    assert any("idle attributed samples=0" in e for e in disabled["errors"])
    print("perf gate: lower/upper syscall and positive-attribution bounds PASS; status batch disabled RED")


def function(source, name):
    """Compile the production lifecycle/policy functions unchanged, as in the INIT harness."""
    match = re.search(r"^(?:static )?(?:int|void|uint64_t|struct pgwt_backend \*)\s*" + name +
                      r"\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
    assert match, f"missing production function {name}"
    return match[0]


def live_setup_cases():
    """Drive the private-cluster launcher with process boundaries faked only."""
    import test_sampler_batch_live as live
    for major in (13, 17, 18):
        capacity = 1100 if major == 13 else 540
        commands = []
        def fake_run(command, **kwargs):
            commands.append(command)
            assert command[:4] == ["runuser", "-u", "postgres", "--"]
            data = Path(command[command.index("-D") + 1])
            if command[4].endswith("/initdb"):
                data.mkdir()
                (data / "postgresql.conf").write_text("# default max_connections=100\n")
            elif command[-1] == "start":
                config = (data / "postgresql.conf").read_text()
                assert f"\nmax_connections={capacity}\n" in config
                assert "listen_addresses=''" in config and "port=55486" in config
                assert ("compute_query_id=on" in config) == (major >= 14)
                assert os.environ["PGHOST"] == str(data.parent)
                assert os.environ["PGPORT"] == "55486"
                (data / "postmaster.pid").write_text("42\n")
            else:
                assert command[-1] == "stop" and "immediate" in command
                (data / "postmaster.pid").unlink()
            return subprocess.CompletedProcess(command, 0)

        original = {key: os.environ.get(key) for key in ("PGHOST", "PGPORT", "PATH")}
        with mock.patch.object(live.os, "readlink", return_value=f"/runner/pg{major}/bin/postgres"), \
             mock.patch.object(live.pwd, "getpwnam", return_value=SimpleNamespace(pw_uid=1, pw_gid=1)), \
             mock.patch.object(live.os, "chown"), \
             mock.patch.object(live.subprocess, "run", side_effect=fake_run), \
             mock.patch.object(live.subprocess, "check_output", return_value=f"postgres (PostgreSQL) {major}.6"), \
             mock.patch.object(live, "psql", side_effect=lambda sql: str(capacity) if "max_connections" in sql else str(major * 10000)):
            class TestFailure(Exception):
                pass
            try:
                with live.private_cluster(99, False) as (pid, selected):
                    assert pid == 42 and selected == major
                    raise TestFailure  # failures must stop the private postmaster
            except TestFailure:
                pass
        assert len(commands) == 3 and commands[-1][-1] == "stop"
        assert all(command[4].startswith(f"/runner/pg{major}/bin/") for command in commands)
        assert original == {key: os.environ.get(key) for key in original}
    print("private cluster: PG13 own 1100 / PG17-18 own 540 config, cleanup/environment restore PASS")


def pg13_live_pure_cases():
    """Timing truth is hand-built, never fitted to candidate output."""
    import test_sampler_batch_live as live
    from sampler_batch_perf import assess_pg13_timing, require_pg13_negative_control
    cohort = [{"pid": 1000 + i, "backend_start_ns": 10_000_000_000_000 + i * 100_000_000}
              for i in range(200)]
    samples = [{"pid": row["pid"], "timestamp_ns": row["backend_start_ns"] + (i + 1) * 100_000,
                "query_id": 42} for i, row in enumerate(cohort)]
    green = assess_pg13_timing(cohort, reversed(samples), 10_000_000)
    assert not green["timing_errors"] and green["attributed_sessions"] == 200
    assert green["max_first_attribution_ns"] == 20_000_000
    assert green["p99_first_attribution_ns"] == 19_800_000
    # A later duplicate cannot replace the first observation. An idle sample
    # and another pid cannot satisfy missing attribution.
    assert assess_pg13_timing(cohort, samples + [dict(samples[0], timestamp_ns=samples[0]["timestamp_ns"] + 99_000_000)],
                              10_000_000) == green
    late = [*samples[:-1], dict(samples[-1], timestamp_ns=cohort[-1]["backend_start_ns"] + 20_000_001)]
    red = assess_pg13_timing(cohort, late, 10_000_000)
    # The pre-fix tracer has validation status but no reason field. It must
    # pass the anchor prerequisite and reach the timing negative control.
    legacy_status = {"pgbackend_layout_validation": "validated"}
    live.require_pg13_anchor_status(legacy_status, negative=True)
    assert "first attribution > 2 tick periods" in require_pg13_negative_control(red)
    candidate_status = dict(legacy_status,
                            pgbackend_layout_reason="controlled backend retired before capture")
    live.require_pg13_anchor_status(candidate_status)
    for negative in (False, True):
        try:
            live.require_pg13_anchor_status(dict(candidate_status, pgbackend_layout_validation="degraded"),
                                            negative=negative)
        except AssertionError:
            pass
        else:
            raise AssertionError("unvalidated anchor accepted")
    for status in (legacy_status, dict(candidate_status, pgbackend_layout_reason="validation failed")):
        try:
            live.require_pg13_anchor_status(status)
        except (KeyError, AssertionError):
            pass
        else:
            raise AssertionError("candidate accepted without controlled-anchor reason")
    missing = assess_pg13_timing(cohort, samples[:-1] + [dict(samples[-1], query_id=0),
                                                      dict(samples[-1], pid=999)], 10_000_000)
    assert "first attribution missing: pids=[1199]" in require_pg13_negative_control(missing)
    try:
        require_pg13_negative_control(dict(green, other_errors=["route missing", "perf failed"]))
    except AssertionError as error:
        assert "did not redden first attribution" in str(error)
    else:
        raise AssertionError("unrelated failure qualified as negative control")
    try:
        assess_pg13_timing(cohort, [dict(samples[0], timestamp_ns=cohort[0]["backend_start_ns"] - 1)], 10_000_000)
    except AssertionError as error:
        assert "predates backend_start" in str(error)
    else:
        raise AssertionError("mixed clocks accepted")

    from test_sampler_batch_summary import vm_parser_cases
    vm_parser_cases()

    args = live.parse_args(["--pg13-far-rows", "--master-tracer", "/tmp/0b1d7dd6/pg_wait_tracer",
                            "--require-negative-control", "--dry-run", "--pg-version", "13"])
    assert args.pg13_far_rows and args.require_negative_control and args.master_tracer.name == "pg_wait_tracer"
    for argv in (["--require-negative-control"], ["--pg13-far-rows", "--dry-run", "--pg-version", "18"],
                 ["--pg-version", "13"]):
        with contextlib.redirect_stderr(io.StringIO()), mock.patch.dict(os.environ, {"PGWT_866_MASTER_TRACER": ""}):
            try:
                live.parse_args(argv)
            except SystemExit as error:
                assert error.code == 2
            else:
                raise AssertionError(f"invalid arguments accepted: {argv}")
    command = ["python3", "-B", "tests/test_sampler_batch_live.py", "--pg13-far-rows", "--dry-run",
               "--pg-version", "13", "--master-tracer", "/tmp/0b1d7dd6/pg_wait_tracer", "--require-negative-control"]
    result = run(command)
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan["arms"] == ["pg13-far-rows"] and plan["max_connections"] >= 1100
    assert plan["pg13"]["sample_rate"] == 100 and plan["pg13"]["short_sessions"] == 200
    assert plan["pg13"]["first_attribution_bound_ns"] == 20_000_000
    assert plan["pg13"]["initial_idle"] - plan["pg13"]["retire_near_anchor"] + plan["pg13"]["later_idle"] < 1000
    result = run(["python3", "-B", "tests/test_sampler_batch_live.py", "--dry-run", "--pg-version", "13"])
    assert result.returncode == 0 and "pg13-far-rows" in json.loads(result.stdout)["arms"]
    print("PG13 timing: every pid, max/p99, exact two-period edge, missing/late RED; syscall census and per-tick bound PASS")
    print("PG13 anchor status: pre-fix reason absent reaches timing RED; validation required for both, candidate reason required PASS")
    print("PG13 arguments/default selection/dry-run: PASS (no Linux, PostgreSQL or libpq loaded)")


def pg13_recorded_sample_case():
    """The persisted v3 recording has documented, fixed point timestamps.

    Shift ONLY a temporary copy's wall-clock anchor to exercise conversion;
    never regenerate or edit the frozen recording in the repository.
    """
    import test_sampler_batch_live as live
    with tempfile.TemporaryDirectory(prefix="pgwt866-recording-") as tmp:
        directory = Path(tmp)
        reader = live.build_sample_reader(directory)
        data = bytearray((ROOT / "tests/fixtures/sample_weights_v3/current.trace").read_bytes())
        struct.pack_into("<Q", data, 12, 20_000_000_000_000)
        (directory / "current.trace").write_bytes(data)
        samples = live.read_cohort_samples(reader, directory, [{"pid": 1000}])
        assert [row["timestamp_ns"] for row in samples] == [20_000_000_000_000,
                                                             20_000_090_000_000,
                                                             20_000_290_000_000]
        assert all(row["pid"] == 1000 and row["query_id"] == 100 for row in samples)
        assert live.read_cohort_samples(reader, directory, [{"pid": 999}]) == []
        (directory / "current.trace").write_bytes(b"truncated trace")
        try:
            live.read_cohort_samples(reader, directory, [{"pid": 1000}])
        except subprocess.CalledProcessError as error:
            assert error.returncode == 2
        else:
            raise AssertionError("undecodable recording accepted as missing attribution")
    print("recorded samples: persisted timestamps/identity, wall-clock conversion, corrupt-trace refusal PASS")


def writer_group_failure_case():
    """Fortify's unused-result contract and the real counted writer error path."""
    import sys
    with tempfile.TemporaryDirectory(prefix="pgwt866-fchown-") as tmp:
        directory = Path(tmp)
        header = directory / "fchown-result.h"
        # Darwin lacks glibc's fortify annotation; require it explicitly so
        # this Mac check detects the original ignored-result build failure.
        header.write_text("#include <unistd.h>\n"
                          "extern int fchown(int, uid_t, gid_t) __attribute__((warn_unused_result));\n")
        source = directory / "group-failure.c"
        source.write_text(r'''#include "event_writer.h"
#include <assert.h>
#include <errno.h>
#include <unistd.h>
int pgwt_test_fchown(int fd, uid_t owner, gid_t group) {
    (void)fd; (void)owner; (void)group; errno = EPERM; return -1;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    struct pgwt_event_writer w;
    assert(pgwt_writer_init(&w, argv[1], 13, 1, NULL) == 0);
    w.trace_gid = getgid();
    struct pgwt_trace_event event = {.timestamp_ns = 1000000, .pid = 1000};
    uint64_t failures = 0;
    assert(pgwt_writer_push_event_counted(&w, &event, &failures) == -1);
    assert(failures == 1 && w.failed && !w.enabled && w.fp == NULL);
    assert(pgwt_writer_push_event_counted(&w, &event, &failures) == -1);
    assert(failures == 2 && w.fp == NULL);
    pgwt_writer_destroy(&w);
    return 0;
}
''')
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-D_FORTIFY_SOURCE=2",
            "-Werror=unused-result", "-Dfchown=pgwt_test_fchown", "-include", str(header),
            "-I" + str(ROOT / "src"), str(source), str(ROOT / "src/event_writer.c"),
            "-llz4", "-o", str(directory / "group-failure")]
        if sys.platform == "darwin":
            prefix = subprocess.check_output(["brew", "--prefix", "lz4"], text=True).strip()
            command[1:1] = ["-I" + prefix + "/include", "-L" + prefix + "/lib",
                             "-include", str(ROOT / "demos/replay-compat.h")]
        result = run(command)
        assert result.returncode == 0, result.stdout + result.stderr
        result = run([str(directory / "group-failure"), str(directory / "trace")])
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stderr.count("WARN: cannot set trace group") == 1, result.stderr
    print("fchown: -D_FORTIFY_SOURCE=2 -Werror=unused-result build, counted sticky writer failure PASS")


def late_promotion_setup_case():
    """Run the real arm with only libpq/process/SQL boundaries replaced."""
    import test_sampler_batch_live as live
    events, children = [], []
    cohort = mock.Mock()
    cohort.open.side_effect = lambda *args: events.append("idle")
    cohort.pids.return_value = {2000, 2001, 2002, 2003}

    class Child:
        returncode = None
        def poll(self):
            return self.returncode
        def send_signal(self, sig):
            self.returncode = 0
        def wait(self, timeout):
            return self.returncode
        def kill(self):
            raise AssertionError("arm failed to stop child")

    def popen(command, **kwargs):
        child = Child()
        children.append(child)
        if command[0] == "pgbench":
            assert events == ["idle"]
            events.append("workload")
            script = Path(command[command.index("-f") + 1]).read_text()
            assert "\\if :shape" in script and "\\sleep 20 ms" in script
            assert "SELECT pg_sleep(0.02), 42" in script
        else:
            assert events == ["idle", "workload"]
            events.append("daemon")
            assert kwargs["env"]["PGWT_TEST_PGBS_VALIDATION_FAIL"] == "1"
            trace = Path(command[command.index("-T") + 1])
            (trace / "pgwt.sock").touch()
            kwargs["stderr"].write("forced validation failure (test hook)\n"
                                   "bounded uprobe shadow validation completed\n" +
                "".join(f"SAMPLED-ATTR-IDLE: pid={pid} databaseid=1 userid=1\n"
                        for pid in cohort.pids.return_value))
            kwargs["stderr"].flush()
        return child

    with tempfile.TemporaryDirectory(prefix="pgwt866-promotion-unit-") as tmp, \
         mock.patch.object(live, "IdleConnections", return_value=cohort), \
         mock.patch.object(live.subprocess, "Popen", side_effect=popen), \
         mock.patch.object(live, "psql", return_value="4"), \
         mock.patch.object(live, "metrics", return_value={"pgbackend_layout_validation": "validated"}):
        live.run_late_promotion_arm(Path("/candidate"), 42, Path(tmp))
    assert all(child.returncode == 0 for child in children) and len(children) == 2
    cohort.close.assert_called_once()
    print("late-promotion arm: sessions/workload precede daemon, validation hook, per-pid proof and cleanup PASS")


def unsupported_version_cases():
    """Exercise the runner's actual exclusion branch, including require-live.
    Direct invocation also exits before opening a cluster or needing libpq.
    """
    import test_sampler_batch_live as live
    from sampler_batch_perf import assess
    runner = (ROOT / "tests/run_all.sh").read_text()
    start = runner.index('        if [[ "$name" == "test_sampler_batch_live" && "$PG_VERSION" =~')
    end = runner.index('        fi', start) + len('        fi')
    guard = runner[start:end]
    for major in (13, 14, 15, 16, 17, 18):
        body = ('exclude_test() { echo "EXCLUDED: $1 $2"; }; REQUIRE_LIVE=1; '
                f'PG_VERSION={major}; for name in test_sampler_batch_live test_other; do\n' +
                guard + '\necho "RUN: $name"; done')
        result = run(["bash", "-c", body])
        assert result.returncode == 0, result.stderr
        assert ("EXCLUDED: test_sampler_batch_live" in result.stdout) == (14 <= major <= 16)
        assert "RUN: test_other" in result.stdout
        if major in (14, 15, 16):
            with mock.patch.object(live.sys, "argv", ["test_sampler_batch_live.py", "--pid", "99"]), \
                 mock.patch.object(live.sys, "platform", "linux"), \
                 mock.patch.object(live.os, "geteuid", return_value=0), \
                 mock.patch.object(live.os, "readlink", return_value="/runner/bin/postgres"), \
                 mock.patch.object(live.subprocess, "check_output", return_value=f"postgres (PostgreSQL) {major}.6"), \
                 mock.patch.object(live, "private_cluster", side_effect=AssertionError("unsupported cluster started")), \
                 mock.patch.object(live, "IdleConnections", side_effect=AssertionError("unsupported libpq loaded")):
                live.main()
            try:
                assess({}, major, 504, 200, 0, 1)
            except ValueError as error:
                assert "qualification matrix" in str(error)
            else:
                raise AssertionError("unsupported version was qualified")
    print("PG14-16: real runner branch EXCLUDED under --require-live; direct test exits before cluster/libpq")


def main():
    perf_gate_cases()
    pg13_live_pure_cases()
    pg13_recorded_sample_case()
    writer_group_failure_case()
    live_setup_cases()
    late_promotion_setup_case()
    unsupported_version_cases()
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT / 'src'}"] + shlex.split(os.environ.get("CFLAGS", ""))
    if os.uname().sysname == "Darwin":
        flags += ["-include", str(ROOT / "demos/replay-compat.h")]
        flags += ["-include", str(ROOT / "tests/sample_weight_compat.h")]
    source = (ROOT / "src/backend_status_layout.c").read_text()
    backend = (ROOT / "src/backend.c").read_text()
    sampler = (ROOT / "src/sampler.c").read_text()
    # Keep the merged attach failure classification in the compiled lifecycle,
    # including its real retained-fd liveness parser; only libc I/O is injected.
    cmdline = (ROOT / "src/cmdline.c").read_text()
    lifecycle = function(cmdline, "pgwt_backend_target_live_fd") + "\n" + "\n".join(
        function(backend, name) for name in (
            "open_watchpoint_target", "record_watchpoint_failure", "alloc_backend",
            "pgwt_sampler_enroll_backend", "pgwt_scan_existing_backends",
            "pgwt_handle_fork", "pgwt_handle_init"))
    discovery = (ROOT / "src/discovery.c").read_text()
    policy = function(discovery, "pgwt_pg13_status_offsets") + "\n" + "\n".join(function(sampler, name) for name in
                       ("resolve_sampled_attr", "resolve_pg13_sampled_attr", "pgwt_sampler_refresh_pg13_generation",
                        "finish_sampled_attr", "note_idle_attr_sample"))
    # Compile the real registry paths and the actual target-building loop.
    # Hardware, directory enumeration, maps and stat are injected by the C harness.
    for name in ("pgwt_scan_existing_backends", "pgwt_handle_init", "pgwt_handle_fork"):
        assert "pgwt_sampler_enroll_backend(be, now_ns());" in function(backend, name), name
    control = (ROOT / "src/control.c").read_text()
    assert 'd->backend_status_layout.pg13_computed ? "computed" : "scan"' in control
    poll = function(sampler, "pgwt_sampler_poll")
    assert "resolve_sampled_attr(d, be, tick_ts, &maps_cache);" in poll
    assert "if (target_tick_enabled && d->backend_status_layout.major != 13)" in poll
    assert "finish_sampled_attr(be, attr_rows[n].rc, attr_rows[n].error, tick_ts);" in poll
    assert "note_idle_attr_sample(d, &targets[i], tick_enabled[i] && attr_valid[i]," in poll
    assert "getenv" not in function(sampler, "note_idle_attr_sample")
    assert sampler.count('getenv("PGWT_DEBUG_SAMPLED_ATTR_SHADOW")') == 1
    target_loop = poll[poll.index("    int n = 0;"):poll.index("    int target_count = n;")]
    period_end = poll.index("\n", poll.index("uint64_t period_ns =")) + 1
    tick_prelude = poll[period_end:poll.index("    const bool tick_source_enabled")]
    daemon = (ROOT / "src/daemon.c").read_text()
    timer = function(daemon, "pgwt_handle_timer")
    timer_start = timer.index("    if (pgwt_sampler_refresh_pg13_generation(d)")
    timer_end = timer.index("    /* Stage 3 refreshes capacity", timer_start)
    interval = "static void interval_registry(struct pgwt_daemon *d)\n{\n    (void)d;\n" + timer[timer_start:timer_end] + "}\n"
    registry = """
static int sample_registry(struct pgwt_daemon *d, uint64_t tick_ts)
{
""" + tick_prelude + """    const bool tick_source_enabled = pgwt_pgbs_sampled_attr_enabled(&d->backend_status_layout);
    struct pgwt_sample_target sample_targets[MAX_BACKENDS];
    struct pgwt_pgbs_batch_target attr_rows[MAX_BACKENDS];
    uint8_t tick_enabled[MAX_BACKENDS];
    struct pgwt_backend *backend_rows[MAX_BACKENDS];
""" + target_loop.replace("targets[", "sample_targets[") + """
    (void)tick_enabled; (void)backend_rows; (void)attr_errno; (void)attr_no_context;
    memset(targets, 0, sizeof(targets));
    memcpy(targets, attr_rows, (size_t)n * sizeof(*targets));
    memcpy(recorded_samples, sample_targets, (size_t)n * sizeof(*sample_targets));
    return n;
}
"""
    # Exercise the poll's real refusal/selection and validity-mask statements,
    # followed by the production wait reader and batch builder. Only the
    # successful row's value conversion is supplied here (no PG13 text hash).
    finish = poll[poll.index("        if (target_tick_enabled)\n            finish_sampled_attr"):
                  poll.index("        struct pgwt_sampled_attr_value uprobe_attr")]
    selection = poll[poll.index("        enum pgwt_sampled_attr_source source ="):
                     poll.index("    if (n == 0) {")]
    selection = selection[:selection.rfind("    }")]
    validity = poll[poll.index("    for (int i = 0; i < n; i++) {\n        if (!attr_valid[i]"):
                    poll.index("    /* #294")]
    recording = """
static int record_registry_tick(struct pgwt_daemon *d, uint64_t tick_ts)
{
    struct pgwt_sampler *s = d->sampler;
    int target_count = sample_registry(d, tick_ts);
    uint8_t attr_valid[MAX_BACKENDS], tick_enabled[MAX_BACKENDS];
    pgwt_pgbs_read_batch(d->postmaster_pid, &d->backend_status_layout,
                        targets, (size_t)target_count, false, &tick_failures);
    int n;
    for (n = 0; n < target_count; n++) {
        struct pgwt_backend *be = pgwt_find_backend(&d->backends, targets[n].pid);
        const bool target_tick_enabled = targets[n].reader != NULL;
        tick_enabled[n] = target_tick_enabled;
""" + finish.replace("attr_rows[", "targets[") + """
        const int tick_ok = targets[n].rc == 0;
        struct pgwt_sampled_attr_value tick_attr = {
            .query_id = targets[n].attr.query_id,
            .last_query_id = targets[n].attr.last_query_id,
            .cmd_open = targets[n].attr.cmd_open,
        };
        struct pgwt_sampled_attr_value uprobe_attr = {.cmd_open = 1};
""" + selection.replace("targets[", "recorded_samples[").replace("attr_rows[", "targets[") + """
    }
    int got = pgwt_sampler_read_targets(recorded_samples, n, s->read_vals,
                                        s->read_valid, &s->read_faults_total);
""" + validity.replace("targets[", "recorded_samples[") + """
    (void)got;
    return pgwt_sampler_build_batch(recorded_samples, s->read_vals, s->read_valid,
        n, tick_ts, s->samples, &d->counters.invalid_wait_reads_total,
        &d->counters.noncmd_cpu_samples_total);
}
"""
    assert 'cjson_add_uint64(root, "sampled_attr_tick_idle_samples_total",' in (ROOT / "src/control.c").read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-sampled-io-") as directory:
        tmp = Path(directory)

        def build(label, text, enrollment=lifecycle, resolution=policy, sampler_source=sampler,
                  registry_source=registry, recording_source=recording, interval_source=interval):
            implementation = tmp / f"{label}.c"
            implementation.write_text(text)
            injected = tmp / f"{label}-lifecycle.h"
            injected.write_text(enrollment + "\n" + resolution + interval_source + registry_source + recording_source)
            portable_sampler = tmp / f"{label}-sampler.c"
            portable_sampler.write_text(sampler_source.replace(
                "#include <sys/uio.h>", "#include <sys/uio.h>\n"
                "extern ssize_t pgwt_test_wait_readv(pid_t, const struct iovec *, unsigned long, "
                "const struct iovec *, unsigned long, unsigned long);\n"
                "#define process_vm_readv pgwt_test_wait_readv"))
            binary = tmp / label
            result = run(cc + flags + [
                "-DPGWT_SERVER",
                f'-DPGWT_PGBS_SOURCE="{implementation}"',
                f'-DPGWT_LIFECYCLE_SOURCE="{injected}"',
                str(ROOT / "tests/test_sampled_attr_io.c"),
                str(ROOT / "src/spawn.c"), str(ROOT / "src/backend_init.c"),
                str(portable_sampler), str(ROOT / "src/anomaly.c"),
                str(ROOT / "src/idle_rule.c"), "-o", str(binary)])
            assert result.returncode == 0, result.stdout + result.stderr
            return binary

        result = run([str(build("baseline", source))])
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end="")
        pg13_mutations = [
            ("pg13_scan_with_known_base", "    if (layout->status_array_base) return;",
             "    /* scan even though the computed base is available */",
             "PG13 known base keeps backendId -1 pending without a scan fallback"),
            ("pg13_pgprocno_as_backend_id", "int32_t id = hints[j].backend_id;",
             "int32_t id = hints[j].pgprocno;",
             "PG13 all eligible rows are attributed on their first tick"),
            ("pg13_drop_anchor_offset", "uint64_t delta = id > 0 ? (uint64_t)(id - 1) * layout->struct_size : 0;",
             "uint64_t delta = id > 0 ? (uint64_t)id * layout->struct_size : 0;",
             "PG13 anchor ID establishes array base independent of anchor position"),
            ("pg13_drop_row_minus_one", "uint64_t delta = (uint64_t)(id - 1) * layout->struct_size;",
             "uint64_t delta = (uint64_t)id * layout->struct_size;",
             "PG13 all eligible rows are attributed on their first tick"),
            ("pg13_scan_as_main_route", "    if (pending) {\n        got = pending ?",
             "    if (false && pending) {\n        got = pending ?",
             "PG13 signed far distances resolve on the first eligible tick"),
            ("pg13_one_discovery_call_per_target",
             "ssize_t got = count ? batch_readv(postmaster_pid, local, remote, count) : 0;",
             "ssize_t got = 0;\n    for (size_t j = 0; j < count; j++) {\n"
             "        ssize_t part = batch_readv(postmaster_pid, local + j, remote + j, 1);\n"
             "        if (part < 0) break;\n        got += part;\n    }",
             "PG13 pending discovery uses at most two bounded postmaster calls"),
            ("pg13_skip_shared_range",
             "uint64_t base = layout->status_array_base + delta;\n"
             "        if (!reader_shared_range(r, base, layout->struct_size)) continue;",
             "uint64_t base = layout->status_array_base + delta;",
             "PG13 computed row requires both shared range proofs"),
            ("pg13_skip_pgproc_pid", "if (hints[j].pid != r->pid || id <= 0) continue;",
             "if (id <= 0) continue;",
             "PG13 PGPROC pid mismatch is never a computed hint"),
            ("pg13_skip_row_pid", "snapshot->procpid == (uint32_t)expected_pid &&",
             "snapshot->procpid != 0 &&",
             "PG13 computed hints still require the row PID"),
            ("pg13_skip_start_time", "if (!pgwt_pgbs_pid_is_original(r->pid, r->start_time)) {",
             "if (false && !pgwt_pgbs_pid_is_original(r->pid, r->start_time)) {",
             "PG13 computed acceptance checks the enrolled start time"),
            ("pg13_accept_odd_counters", "return before == after && (after & 1u) == 0;",
             "return before == after;", "PG13 odd counters are refused"),
            ("pg13_accept_unequal_counters", "return before == after && (after & 1u) == 0;",
             "(void)before; return (after & 1u) == 0;", "PG13 unequal counters are refused"),
            ("pg13_zero_anchor_on_generation",
             "layout->status_array_base = 0;\n    layout->pg13_computed = false;",
             "layout->status_array_base = layout->status_anchor = 0;\n    layout->pg13_computed = false;",
             "PG13 same-range rebuild retains anchor and layout but clears base"),
            ("pg13_keep_reader_across_generation",
             "if (r->shm_generation != layout->shm_generation ||\n"
             "            r->shared_reader_pid != postmaster_pid) {",
             "if (r->shared_reader_pid != postmaster_pid) {",
             "PG13 generation change discards accepted rows and mapping proofs"),
        ]
        for label, before, after, assertion in pg13_mutations:
            assert source.count(before) == 1, (label, "mutation anchor changed")
            result = run([str(build(label, source.replace(before, after))), "pg13"])
            assert result.returncode == 1 and f"FAIL: {assertion}" in result.stderr, (
                label, result.returncode, result.stdout, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")
        pg13_policy_mutations = [
            ("pg13_keep_sampler_caches", "pgwt_pgbs_reader_reset(&be->sampled_reader);",
             "/* keep the old sampler enrollment */",
             "PG13 sampler generation change retires row and wait-context caches"),
            ("pg13_keep_retry_deadline", "uint64_t wait = backends[i]->wp_addr;",
             "if (backends[i]->pgbs_resolve_retry_ns) { rows[i].reader = NULL; continue; }\n"
             "        uint64_t wait = backends[i]->wp_addr;",
             "PG13 sampler batches all pending discovery without a retry deadline"),
        ]
        for label, before, after, assertion in pg13_policy_mutations:
            assert policy.count(before) == 1, (label, "mutation anchor changed")
            result = run([str(build(label, source, resolution=policy.replace(before, after))),
                          "pg13-registry"])
            assert result.returncode == 1 and f"FAIL: {assertion}" in result.stderr, (
                label, result.returncode, result.stdout, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")
        recovery_mutations = [
            ("pg13_unbounded_missing_mapping", source,
             "if (now_ms - layout->pg13_recovery_pending_since_ms < 30000) return 0;",
             "if (now_ms - layout->pg13_recovery_pending_since_ms < UINT64_MAX) return 0;",
             "PG13 renamed relocation degrades at the 30-second deadline"),
            ("pg13_no_interval_retry", interval,
             "pgwt_sampler_refresh_pg13_generation(d)",
             "(d->backend_status_layout.pg13_recovery == PGWT_PGBS_PG13_PENDING ? 0 : "
             "pgwt_sampler_refresh_pg13_generation(d))",
             "PG13 absent mapping recovers on the next daemon interval"),
            ("pg13_accept_changed_range", source,
             "(map.identity && (map.lo != layout->shm_start || map.hi != layout->shm_end))",
             "(false && map.identity && (map.lo != layout->shm_start || map.hi != layout->shm_end))",
             "PG13 changed range latches degraded and counts it once"),
            ("pg13_maps_per_tick", registry,
             "    const bool tick_source_enabled =",
             "    (void)pgwt_sampler_refresh_pg13_generation(d);\n    const bool tick_source_enabled =",
             "PG13 steady-state sampling tick performs zero maps reads"),
        ]
        for label, original, before, after, assertion in recovery_mutations:
            assert original.count(before) == 1, (label, "mutation anchor changed")
            changed = original.replace(before, after)
            kwargs = ({"interval_source": changed} if original == interval else
                      {"registry_source": changed} if original == registry else {})
            result = run([str(build(label, changed if original == source else source, **kwargs)),
                          "pg13-registry"])
            assert result.returncode == 1 and f"FAIL: {assertion}" in result.stderr, (
                label, result.returncode, result.stdout, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")
        mutant_cases = {"no_iov_max_chunking": "budget", "drop_cached_address_return": "resolve",
                        "drop_pid_zero_guard": "identity", "drop_existing_entry_identity_check": "identity",
                        "restore_exit_during_exact_duplicate": "identity", "drop_lazy_enrollment": "registry",
                        "drop_retry_deadline": "pending", "backoff_during_startup": "retry",
                        "gate_lazy_on_layout": "registry", "drop_deferred_cache_reset": "identity",
                        "reset_before_exact_drain": "identity", "drop_meta_reset": "identity",
                        "drop_acceptance_identity": "reuse", "drop_init_identity": "init-reuse",
                        "restart_rejected_backoff": "rejected-backoff",
                        "drop_refusal_invalidation": "refusal",
                        "drop_tick_maps_cache": "first-tick"}
        mutations = [
            ("drop_pid_compare",
             "snapshot->procpid == (uint32_t)expected_pid &&",
             "snapshot->procpid != 0 &&",
             "cross-wired row buffers fail PID identity"),
            ("accept_torn",
             "return before == after && (after & 1u) == 0;",
             "(void)before; return (after & 1u) == 0;",
             "persistent tear fails once after two retry calls"),
            ("parse_untransferred",
             "if (!row->complete) continue;",
             "if (!row->complete) { targets[i].rc = 0; continue; }",
             "untransferred rows cannot be parsed as valid"),
            ("iovec_owner_off_by_one",
             "owner[count++] = i;",
             "owner[count++] = (i + 1) % n;",
             "short byte count rejects partial row and resumes later rows"),
            ("no_iov_max_chunking",
             "if (want > PGWT_PGBS_IOV_MAX) want = PGWT_PGBS_IOV_MAX;",
             "if (want > PGWT_PGBS_IOV_MAX) want = count - base;",
             "batched syscall budget over 500 ticks"),
            ("skip_previously_idle",
             "if (!gate_only) memset(&targets[i].attr, 0, sizeof(targets[i].attr));",
             "if (targets[i].rc == 0 && !targets[i].attr.cmd_open) targets[i].reader = NULL;\n"
             "        if (!gate_only) memset(&targets[i].attr, 0, sizeof(targets[i].attr));",
             "idle row changes on the next tick"),
            ("counter_after_before_span",
             "ADD_RANGE(row->span, r->status_addr + lo, hi - lo);\n"
             "            if (text_len) ADD_RANGE(row->activity, r->activity_addr, text_len);\n"
             "            ADD_RANGE(&row->after, r->status_addr + layout->st_changecount.offset, 4);",
             "ADD_RANGE(&row->after, r->status_addr + layout->st_changecount.offset, 4);\n"
             "            if (text_len) ADD_RANGE(row->activity, r->activity_addr, text_len);\n"
             "            ADD_RANGE(row->span, r->status_addr + lo, hi - lo);",
             "counter after payload detects concurrent writer"),
            ("batch_unproven_address",
             "if (!r || !r->shared || !r->status_addr ||",
             "if (!r || !r->status_addr ||",
             "unproven shared address is never batched"),
            ("drop_acceptance_identity",
             "if (!pgwt_pgbs_pid_is_original(r->pid, r->start_time)) {",
             "if (false && !pgwt_pgbs_pid_is_original(r->pid, r->start_time)) {",
             "unseen pid reuse is refused at first candidate acceptance"),
        ]
        for label, before, after, assertion in mutations:
            assert source.count(before) == 1, (label, "mutation anchor changed")
            binary = build(label, source.replace(before, after))
            result = run([str(binary), mutant_cases.get(label, "faults")])
            assert result.returncode == 1, (label, result.returncode, result.stdout, result.stderr)
            assert f"FAIL: {assertion}" in result.stderr, (label, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")
        other_mutations = [
            ("drop_refusal_invalidation", recording,
             "if (target_tick_enabled && targets[n].error == ESTALE) attr_valid[n] = 0;",
             "/* retain predecessor sample on identity refusal */",
             "stale identity refusal records zero samples for every type and tier"),
            ("drop_meta_reset", registry,
             "memset(&be->meta, 0, sizeof(be->meta));\n            be->meta_parsed = false;",
             "/* retain predecessor metadata */",
             "replacement reparses metadata instead of inheriting predecessor type"),
            ("drop_init_identity", lifecycle,
             "if (be && be->sampled_reader.pid &&",
             "if (false && be && be->sampled_reader.pid &&",
             "INIT checks existing identity before the already-attached return"),
            ("restart_rejected_backoff", policy,
             "if (!candidate) {", "if (candidate || be->pgbs_addr == 0) {",
             "rejected candidates preserve exponential attempt times"),
            ("drop_tick_maps_cache", registry,
             "resolve_sampled_attr(d, be, tick_ts, &maps_cache);",
             "resolve_sampled_attr(d, be, tick_ts, NULL);",
             "N=500 first tick includes enrollment, acceptance and both mapping proofs"),
            ("drop_pid_zero_guard", lifecycle,
             "if (be->sampled_reader.pid == 0) {", "if (true) {",
             "pid-zero guard preserves identity and cached row across sampled ticks"),
            ("drop_cached_address_return", source,
             "if (reader->status_addr) return 0;", "/* always resolve */",
             "cached-address return prevents re-resolution on every tick"),
            ("drop_existing_entry_identity_check", lifecycle,
             "if (existing->sampled_reader.pid != 0 &&",
             "if (false && existing->sampled_reader.pid != 0 &&",
             "existing-entry identity check replaces stale enrollment"),
            ("restore_exit_during_exact_duplicate", lifecycle,
             "if (d->mode == PGWT_MODE_FULL || d->escalation.active) {", "if (false) {",
             "exact duplicate fork preserves new child state-map seed"),
            ("drop_lazy_enrollment", registry,
             "if (pgwt_sampler_enroll_backend(be, tick_ts) != 0)\n            continue;",
             "/* no lazy enrollment */",
             "first sampled tick enrolls every registry path"),
            ("backoff_during_startup", policy,
             "bool early = be->pgbs_resolve_fast_until_ns &&",
             "bool early = false && be->pgbs_resolve_fast_until_ns &&",
             "startup attempts follow 100 ms cadence"),
            ("gate_lazy_on_layout", registry,
             "if (pgwt_sampler_enroll_backend(be, tick_ts) != 0)\n            continue;",
             "if (tick_source_enabled && pgwt_sampler_enroll_backend(be, tick_ts) != 0)\n            continue;",
             "enrollment precedes sampling even without a tick attribution source"),
            ("drop_deferred_cache_reset", registry,
             "if (be->sampled_cache_stale) {", "if (false && be->sampled_cache_stale) {",
             "replacement cannot sample predecessor wait slot during exact capture"),
            ("reset_before_exact_drain", registry,
             "if (d->mode == PGWT_MODE_FULL || d->escalation.active)",
             "if (pgwt_mode_uses_watchpoints(d))",
             "deferred sampled cache invalidation waits through exact drain"),
            ("drop_retry_deadline", policy,
             "tick_ts < be->pgbs_resolve_retry_ns)", "false)",
             "N=500 pending cost is one VM per due backend independent of tick rate"),
        ]
        for label, original, before, after, assertion in other_mutations:
            assert original.count(before) == 1, (label, "mutation anchor changed")
            changed = original.replace(before, after)
            kwargs = ({"enrollment": changed} if original == lifecycle else
                      {"registry_source": changed} if original == registry else
                      {"recording_source": changed} if original == recording else
                      {"resolution": changed} if original == policy else
                      {"sampler_source": changed} if original == sampler else {})
            binary = build(label, changed if original == source else source, **kwargs)
            result = run([str(binary), mutant_cases.get(label, "faults")])
            assert result.returncode == 1 and f"FAIL: {assertion}" in result.stderr, (label, result)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")

        total = len(recovery_mutations) + len(pg13_mutations) + len(pg13_policy_mutations) + len(mutations) + len(other_mutations)
        print(f"sampled_attr_io mutants: {total}/{total} built, reached and rejected by named assertions")


if __name__ == "__main__":
    main()
