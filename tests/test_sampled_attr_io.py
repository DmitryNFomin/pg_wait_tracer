#!/usr/bin/env python3
"""Portable attribution syscall budget and mutations; all outputs stay in /tmp."""
import os
from pathlib import Path
import re
import shlex
import subprocess
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
                assert "\nmax_connections=540\n" in config
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
             mock.patch.object(live, "psql", side_effect=lambda sql: "540" if "max_connections" in sql else str(major * 10000)):
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
    print("private cluster: PG13/17/18 default-100 runner, own 540 config, cleanup/environment restore PASS")


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
    policy = "\n".join(function(sampler, name) for name in
                       ("resolve_sampled_attr", "finish_sampled_attr", "note_idle_attr_sample"))
    # Compile the real registry paths and the actual target-building loop.
    # Hardware, directory enumeration, maps and stat are injected by the C harness.
    for name in ("pgwt_scan_existing_backends", "pgwt_handle_init", "pgwt_handle_fork"):
        assert "pgwt_sampler_enroll_backend(be, now_ns());" in function(backend, name), name
    poll = function(sampler, "pgwt_sampler_poll")
    assert "resolve_sampled_attr(d, be, tick_ts, &maps_cache);" in poll
    assert "finish_sampled_attr(be, attr_rows[n].rc, attr_rows[n].error, tick_ts);" in poll
    assert "note_idle_attr_sample(d, &targets[i], tick_enabled[i] && attr_valid[i]," in poll
    assert "getenv" not in function(sampler, "note_idle_attr_sample")
    assert sampler.count('getenv("PGWT_DEBUG_SAMPLED_ATTR_SHADOW")') == 1
    target_loop = poll[poll.index("    int n = 0;"):poll.index("    int target_count = n;")]
    registry = """
static int sample_registry(struct pgwt_daemon *d, uint64_t tick_ts)
{
    const bool tick_source_enabled = pgwt_pgbs_sampled_attr_enabled(&d->backend_status_layout);
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
                  registry_source=registry, recording_source=recording):
            implementation = tmp / f"{label}.c"
            implementation.write_text(text)
            injected = tmp / f"{label}-lifecycle.h"
            injected.write_text(enrollment + "\n" + resolution + registry_source + recording_source)
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



if __name__ == "__main__":
    main()
