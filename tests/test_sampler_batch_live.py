#!/usr/bin/env python3
"""Status-row syscall gate: 500 idle + 4 pgbench sessions, >=200 traced ticks.

Registered in run_all.sh's live tier. Requires Linux/root, perf, libpq, trust
connections, and max_connections >= 520. Does not restart/reconfigure PG.
The lead supplies --master-tracer (master 8cafdcb2 at dispatch) to prove the
SAME assertion goes red on the SAME VM; --require-negative-control enforces it.
Use --require-huge-pages for the additional huge_pages=on qualification.
All captures/evidence go to a temporary directory outside the repository.
Example: sudo python3 tests/test_sampler_batch_live.py --pid PID \
  --master-tracer /tmp/master/pg_wait_tracer --require-negative-control
"""
import argparse
import ctypes
import contextlib
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

from sampler_batch_perf import CALLS, assess, parse_trace
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
SHADOW = ("sampled_attr_shadow_total", "sampled_attr_shadow_mismatch_total",
          "sampled_attr_shadow_cmd_open_mismatch_total",
          "sampled_attr_shadow_query_id_mismatch_total")


def metrics(directory):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(directory / "pgwt.sock"))
        client.sendall(b'{"cmd":"metrics"}\n')
        data = b""
        while b"\n" not in data:
            part = client.recv(65536)
            assert part, "empty control reply"
            data += part
    result = json.loads(data.splitlines()[0])
    assert result.get("ok") is True, result
    return result


def psql(sql):
    return subprocess.check_output(
        ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt",
         "-v", "ON_ERROR_STOP=1", "-c", sql], text=True, timeout=30).strip()


def wait_until(proc, predicate, seconds, reason):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert proc.poll() is None, f"{reason}: process exited {proc.returncode}"
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(reason)


def stop(proc, sig=signal.SIGTERM):
    if proc is None:
        return
    if proc.poll() is None:
        proc.send_signal(sig)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("child failed to stop")


class IdleConnections:
    """Use installed libpq, avoiding 500 extra psql processes/dependencies."""
    def __init__(self):
        self.handles = []
        name = ctypes.util.find_library("pq")
        assert name, "installed libpq is required"
        self.lib = ctypes.CDLL(name)
        self.lib.PQconnectdb.argtypes = [ctypes.c_char_p]
        self.lib.PQconnectdb.restype = ctypes.c_void_p
        self.lib.PQstatus.argtypes = [ctypes.c_void_p]
        self.lib.PQerrorMessage.argtypes = [ctypes.c_void_p]
        self.lib.PQerrorMessage.restype = ctypes.c_char_p
        self.lib.PQfinish.argtypes = [ctypes.c_void_p]

    def open(self):
        for _ in range(500):
            handle = self.lib.PQconnectdb(
                b"user=postgres dbname=postgres application_name=pgwt866_idle connect_timeout=10")
            assert handle, "PQconnectdb allocation failed"
            self.handles.append(handle)
            assert self.lib.PQstatus(handle) == 0, self.lib.PQerrorMessage(handle)

    def close(self):
        for handle in self.handles:
            self.lib.PQfinish(handle)
        self.handles.clear()


def run_arm(binary, pm_pid, major, directory, label):
    trace = directory / label
    trace.mkdir()
    perf = daemon = None
    with (trace / "daemon.stderr").open("w") as err, (trace / "perf.stderr").open("w") as perr:
        try:
            # Do not let another test's injection alter this production arm.
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            daemon = subprocess.Popen(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", "10",
                 "--escalation-budget", "0", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90, "control socket not ready")
            def enrolled():
                value = metrics(trace)
                return value if value.get("sample_read_targets", 0) >= 504 else None
            wait_until(daemon, enrolled, 30, "504 sessions not enrolled")
            # Warm address resolution, metadata and text resolver before perf.
            warm = metrics(trace)["sampler_ticks_total"]
            wait_until(daemon, lambda: metrics(trace)["sampler_ticks_total"] >= warm + 50,
                       30, "warmup did not advance")
            initial_fds = {}
            for path in Path(f"/proc/{daemon.pid}/fd").iterdir():
                try:
                    initial_fds[int(path.name)] = os.readlink(path)
                except FileNotFoundError:
                    pass
            perf_path = trace / "perf.trace"
            perf = subprocess.Popen(
                ["perf", "trace", "-T", "--no-comm", "-m", "1024", "-p", str(daemon.pid),
                 "-e", CALLS, "-o", str(perf_path)],
                stdout=subprocess.DEVNULL, stderr=perr)
            # Require actual trace data before opening the measured window.
            wait_until(perf, lambda: perf_path.exists() and perf_path.stat().st_size > 0,
                       15, "perf produced no trace")
            before = metrics(trace)
            start_ms = time.monotonic_ns() / 1e6
            target_ticks = before["sampler_ticks_total"] + 210
            after = wait_until(daemon, lambda: (m if (m := metrics(trace))["sampler_ticks_total"]
                                               >= target_ticks else None),
                               90, "fewer than 210 sampler ticks")
            end_ms = time.monotonic_ns() / 1e6
            assert perf.poll() is None, "perf exited during measurement"
            time.sleep(1)  # drain exits for syscalls entered inside the window
            stop(perf, signal.SIGINT)
            assert perf.returncode in (0, -signal.SIGINT), (trace / "perf.stderr").read_text()
            ticks = after["sampler_ticks_total"] - before["sampler_ticks_total"]
            population = min(before["sample_read_targets"], after["sample_read_targets"])
            failures = (after["sampled_attr_tick_read_failures_total"] -
                        before["sampled_attr_tick_read_failures_total"])
            assert before["sample_read_targets"] == after["sample_read_targets"], "population changed"
            assert after["pgbackend_layout_validation"] == "validated", after
            counts = parse_trace(perf_path.read_text(), start_ms, end_ms, initial_fds)
            result = assess(counts, major, population, ticks, failures)
            result.update(label=label, binary=str(binary),
                          binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                          population=population, ticks=ticks, calls=counts,
                          shadow_delta={key: after[key] - before[key] for key in SHADOW})
            (trace / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, sort_keys=True), flush=True)
            return result
        finally:
            stop(perf, signal.SIGINT)
            stop(daemon)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--master-tracer", type=Path, default=os.environ.get("PGWT_866_MASTER_TRACER"))
    parser.add_argument("--require-negative-control", action="store_true")
    parser.add_argument("--require-huge-pages", action="store_true")
    parser.add_argument("--evidence-dir", type=Path, help="new directory outside the repository")
    args = parser.parse_args()
    assert sys.platform == "linux" and os.geteuid() == 0, "Linux/root live test only"
    assert not args.require_negative_control or args.master_tracer, "supply --master-tracer"
    pid = args.pid or find_postmaster()
    assert pid, "no postmaster"
    major = int(psql("SHOW server_version_num")) // 10000
    assert major in (13, 17, 18), major
    pgdata = Path(psql("SHOW data_directory"))
    assert int((pgdata / "postmaster.pid").read_text().splitlines()[0]) == pid, "wrong PG port/PID"
    assert int(psql("SHOW max_connections")) >= 520, "provision max_connections >= 520 first"
    huge_pages = psql("SHOW huge_pages")
    assert not args.require_huge_pages or huge_pages == "on", huge_pages
    print(f"PG{major} kernel={os.uname().release} huge_pages={huge_pages}", flush=True)
    idle = IdleConnections()
    load = None
    # Deliberately not tests/results/: no product/repository artifacts.
    if args.evidence_dir:
        args.evidence_dir = args.evidence_dir.resolve()
        assert args.evidence_dir != ROOT and ROOT not in args.evidence_dir.parents, \
            "evidence must stay outside the repository"
        args.evidence_dir.mkdir(parents=True, exist_ok=False)
    evidence_context = (contextlib.nullcontext(str(args.evidence_dir)) if args.evidence_dir else
                        tempfile.TemporaryDirectory(prefix="pgwt866-live-"))
    with evidence_context as tmp:
        directory = Path(tmp)
        script = directory / "workload.sql"
        script.write_text("SELECT sum(i) FROM generate_series(1, 10000) AS i;\n")
        try:
            idle.open()
            assert int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                            "application_name='pgwt866_idle' AND state='idle'")) == 500
            with (directory / "pgbench.stdout").open("w") as out, (directory / "pgbench.stderr").open("w") as err:
                load = subprocess.Popen(
                    ["pgbench", "-n", "-U", "postgres", "-d", "postgres", "-c", "4", "-j", "4",
                     "-R", "100", "-T", "600", "-f", str(script)], stdout=out, stderr=err,
                    env={**os.environ, "PGAPPNAME": "pgwt866_active"})
                wait_until(load, lambda: int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                               "application_name='pgwt866_active'")) == 4,
                           30, "four pgbench sessions not ready")
                candidate = run_arm(ROOT / "pg_wait_tracer", pid, major, directory, "candidate")
                assert not candidate["errors"], candidate["errors"]
                if args.master_tracer:
                    master = run_arm(args.master_tracer.resolve(), pid, major, directory, "master-negative")
                    assert master["errors"] and (master["calls"]["mem_open"] > 0 or
                                                  master["calls"]["pread64"] > 0), \
                        "master negative control did not redden the memory-file assertion"
                    for key in SHADOW[1:]:
                        assert candidate["shadow_delta"][key] <= master["shadow_delta"][key], \
                            (key, candidate["shadow_delta"], master["shadow_delta"])
                    print("master negative control: RED at the same assertion; shadow deltas no worse")
                    if not master["shadow_delta"][SHADOW[0]]:
                        print("shadow comparison has zero observations (no attribute uprobes in baseline)")
                else:
                    print("PENDING: lead-run master negative control; pass --master-tracer")
                assert load.poll() is None, "pgbench stopped before measurement completed"
                stop(load, signal.SIGINT)
                workload = (directory / "pgbench.stdout").read_text()
                assert re.search(r"number of transactions actually processed:\s*[1-9]", workload), workload
                print("status-row syscall candidate gate: PASS (>=200 ticks, 500 idle + 4 pgbench)")
        finally:
            stop(load, signal.SIGINT)
            idle.close()


if __name__ == "__main__":
    main()
