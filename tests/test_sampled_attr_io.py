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


def main():
    perf_gate_cases()
    live_setup_cases()
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT / 'src'}"] + shlex.split(os.environ.get("CFLAGS", ""))
    if os.uname().sysname == "Darwin":
        flags += ["-include", str(ROOT / "demos/replay-compat.h")]
        flags += ["-include", str(ROOT / "tests/sample_weight_compat.h")]
    source = (ROOT / "src/backend_status_layout.c").read_text()
    backend = (ROOT / "src/backend.c").read_text()
    sampler = (ROOT / "src/sampler.c").read_text()
    lifecycle = "\n".join(function(backend, name) for name in
                          ("alloc_backend", "enroll_sampled_reader", "pgwt_handle_fork"))
    policy = "\n".join(function(sampler, name) for name in
                       ("resolve_sampled_attr", "note_idle_attr_sample"))
    # The scan, direct INIT, and fork paths must all call the tested enrollment
    # helper. Runtime coverage below drives the actual fork during escalation.
    for name in ("pgwt_scan_existing_backends", "pgwt_handle_init", "pgwt_handle_fork"):
        assert "enroll_sampled_reader(d, be);" in function(backend, name), name
    poll = function(sampler, "pgwt_sampler_poll")
    assert "resolve_sampled_attr(d, be, tick_ts);" in poll
    assert "note_idle_attr_sample(d, &targets[i], tick_enabled[i] && attr_valid[i]," in poll
    assert 'cjson_add_uint64(root, "sampled_attr_tick_idle_samples_total",' in (ROOT / "src/control.c").read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-sampled-io-") as directory:
        tmp = Path(directory)

        def build(label, text, enrollment=lifecycle, resolution=policy):
            implementation = tmp / f"{label}.c"
            implementation.write_text(text)
            injected = tmp / f"{label}-lifecycle.h"
            injected.write_text(enrollment + "\n" + resolution)
            binary = tmp / label
            result = run(cc + flags + [
                "-DPGWT_SERVER",
                f'-DPGWT_PGBS_SOURCE="{implementation}"',
                f'-DPGWT_LIFECYCLE_SOURCE="{injected}"',
                str(ROOT / "tests/test_sampled_attr_io.c"),
                str(ROOT / "src/spawn.c"), str(ROOT / "src/backend_init.c"),
                str(ROOT / "src/sampler.c"), str(ROOT / "src/anomaly.c"),
                str(ROOT / "src/idle_rule.c"), "-o", str(binary)])
            assert result.returncode == 0, result.stdout + result.stderr
            return binary

        result = run([str(build("baseline", source))])
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end="")
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
        ]
        for label, before, after, assertion in mutations:
            assert source.count(before) == 1, (label, "mutation anchor changed")
            binary = build(label, source.replace(before, after))
            result = run([str(binary)])
            assert result.returncode == 1, (label, result.returncode, result.stdout, result.stderr)
            assert f"FAIL: {assertion}" in result.stderr, (label, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")
        for label, original, before, after, assertion in [
            ("restore_mode_guard", lifecycle,
             "if (pgwt_pgbs_sampled_attr_enabled(&d->backend_status_layout) &&",
             "if (!pgwt_mode_uses_watchpoints(d) && pgwt_pgbs_sampled_attr_enabled(&d->backend_status_layout) &&",
             "escalation-born backend is attributed after de-escalation"),
            ("backoff_during_startup", policy,
             "bool early = be->pgbs_resolve_fast_until_ns &&",
             "bool early = false && be->pgbs_resolve_fast_until_ns &&",
             "late status row resolves on the next tick during first second"),
        ]:
            assert original.count(before) == 1, label
            changed = original.replace(before, after)
            binary = (build(label, source, enrollment=changed) if original == lifecycle else
                      build(label, source, resolution=changed))
            result = run([str(binary)])
            assert result.returncode == 1 and f"FAIL: {assertion}" in result.stderr, (label, result)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")


if __name__ == "__main__":
    main()
