#!/usr/bin/env python3
"""T-cov box gates: exact_perf SIGKILL publication, capacity VOID and startup gap.

Linux root only; never run eBPF on macOS. The attachment and (off EL8) kernel
premises are the existing explicit transport-test premises, not qualification.
An external capacity-stimulus binary uses 64 transport slots to force saturation;
no producer, coverage rule, expected answer or production REARM gate is changed.
The pre-existing-client arm stays incomplete with capture_loss before and after
the idle client exits: startup auxiliaries keep the boot-start PAUSE gap open
while the postmaster runs.
All scratch, rebuilt objects and traces are outside the repository.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time

import test_coverage_live as coverage_live
from test_coverage_live import boot_files, find_pgbench, idle_psql, sql
from test_init_hint_recovery import wait_for
from test_transport_daemon_live import build_mutant
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / "pg_wait_tracer"
SEC = 1_000_000_000


def qb_prefix(path):
    """Count actual TRANS payload identities only through a flushed FRONTIER.

    A SIGKILL can tear the final write. Ignore that physical suffix, which
    cannot carry a complete frontier. Never infer publication from END/stats.
    """
    data = path.read_bytes()
    assert data[:8] == b"PGWTQB02" and len(data) >= 24, "QB02 header"
    version, cpus = struct.unpack_from("<II", data, 8)
    assert version == 2 and cpus > 0, "QB02 version/CPUs"
    offset = 24 + cpus * 4
    seen, certified, frontier = Counter(), Counter(), 0
    voids = []
    while offset + 8 <= len(data):
        size, kind, flags, source = struct.unpack_from("<HBBI", data, offset)
        assert size >= 8 and kind <= 4, "QB02 record prefix"
        if offset + size > len(data):
            break
        if kind == 1 and ((flags >> 1) & 3) == 2:
            assert size == 88, "TRACE payload size"
            t, pid = struct.unpack_from("<QI", data, offset + 40)
            seen[t, pid] += 1
        elif kind == 2:
            assert size == 152, "FRONTIER size"
            frontier = struct.unpack_from("<Q", data, offset + 16)[0]
            certified = seen.copy()
        elif kind == 3:
            voids.append(source)
        offset += size
    return certified, frontier, voids


def read_trace(directory):
    return boot_files(directory)


def w_now(directory):
    if not (directory / "current.trace").exists() or (directory / "current.trace").stat().st_size < 164:
        return 0
    records, _ = read_trace(directory)
    return max((r[3] for r in records if r[1] == "WATERMARK"), default=0)


def build_reader(directory):
    binary = directory / "coverage-verdict"
    source = (ROOT / "src/event_writer.c").read_text()
    match = re.search(r"^int pgwt_decode_varint\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
    assert match, "production varint decoder"
    varint = directory / "varint.c"
    varint.write_text('#include "event_writer.h"\n' + match[0] + "\n")
    command = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
               "-DPGWT_SERVER", "-Isrc", "tests/coverage_trace_verdict.c", "src/event_reader.c",
               "src/coverage.c", str(varint), "-llz4", "-lz", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    return binary


def verdict(reader, directory, start, end):
    _, files = read_trace(directory)
    output = subprocess.check_output([str(reader), str(start), str(end), str(time.monotonic_ns()),
                                      *[str(f[1]) for f in files]], text=True)
    return json.loads(output)


def incomplete_loss(result):
    assert result["verdict"] == "incomplete" and "capture_loss" in result["reasons"], result


def start_daemon(binary, pm, trace, *, idle_round=True, debug=False):
    trace.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
    env["PGWT_TEST_EXACT_ATTACH_PREMISE"] = "1"
    env.pop("PGWT_DEBUG_DUMP_STATE", None)
    if debug:
        env["PGWT_DEBUG_DUMP_STATE"] = "1"
    release = os.uname().release
    if not (release.startswith("4.18.") and ".el8" in release):
        env["PGWT_TEST_EXACT_UNCERTIFIED_KERNEL"] = "1"
    log = trace / "daemon.stderr"
    error = log.open("w")
    command = [str(binary), "--mode", "full", "--exact-perf", "--pid", str(pm),
               "-T", str(trace), "-i", "5", "-q"]
    if not idle_round:
        command += ["--no-exact-perf-idle-round"]
    print("RUN " + shlex.join(command), flush=True)
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=error, env=env)
    try:
        wait_for(process, lambda: (trace / "pgwt.sock").exists(), 45, "exact_perf control socket")
        wait_for(process, lambda: w_now(trace) > 0, 10, "one-second autonomous coverage watermark")
    except BaseException:
        stop(process)
        error.close()
        raise
    return process, error, log


def stop(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        raise AssertionError("daemon exceeded SIGTERM deadline")


def load(pgb, db, seconds, rate=0):
    command = [pgb, "-n", "-S", "-U", "postgres", "-c", "16", "-j", "4", "-T", str(seconds)]
    if rate:
        command += ["-R", str(rate)]
    command += [db]
    print("RUN " + shlex.join(command), flush=True)
    return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)


def finish_load(worker):
    _, error = worker.communicate(timeout=30)
    assert worker.returncode == 0, error


def kill_gate(pm, db, pgb, root, reader):
    trace = root / "sigkill"
    daemon, error, log = start_daemon(TRACER, pm, trace)
    worker = None
    try:
        initial = w_now(trace)
        worker = load(pgb, db, 10, rate=1000)
        wait_for(daemon, lambda: w_now(trace) > initial + 3 * SEC, 12, "W advances under pgbench")
        assert worker.poll() is None, "SIGKILL stimulus must be under load"
        daemon.kill()
        daemon.wait(timeout=5)
        error.close()
        finish_load(worker)
        records, files = read_trace(trace)
        assert not any(r[1] == "SEAL" for r in records), "SIGKILL never seals"
        w = max(r[3] for r in records if r[1] == "WATERMARK")
        expected, frontier, voids = qb_prefix(trace / "merge-qualification.bin")
        assert not voids, ("clean kill arm VOID", voids, log.read_text())
        assert frontier + 1 >= w, "W cannot outrun flushed QB02 FRONTIER"
        expected = Counter({key: n for key, n in expected.items() if key[0] < w})
        actual = Counter()
        proc = subprocess.Popen([str(reader), "--events-before", str(w), *[str(f[1]) for f in files]],
                                stdout=subprocess.PIPE, text=True)
        for line in proc.stdout:
            timestamp, pid = map(int, line.split())
            actual[timestamp, pid] += 1
        assert proc.wait(timeout=15) == 0, "trace DATA reader"
        assert expected and actual == expected, ("exact per-record count below W", sum(expected.values()),
                                                 sum(actual.values()), len(expected - actual), len(actual - expected))
        print(f"SIGKILL: PASS W={w} exact_trace_records={sum(actual.values())}", flush=True)
    finally:
        stop(daemon)
        error.close()
        if worker and worker.poll() is None:
            worker.kill()
            worker.wait(timeout=5)


def saturation_gate(pm, db, pgb, root, reader):
    binary = build_mutant(root, "capacity-stimulus", [
        ("pgwt_transport_create(ids, l->ncpu, 262144, &io)",
         "pgwt_transport_create(ids, l->ncpu, 64, &io)")])
    trace = root / "saturation"
    daemon, error, log = start_daemon(binary, pm, trace)
    worker = None
    try:
        worker = load(pgb, db, 8)
        wait_for(daemon, lambda: "COVERAGE-GAP:" in log.read_text(), 12, "capacity GAP fsync diagnostic")
        finish_load(worker)
        stop(daemon)
        error.close()
        lines = re.findall(r"COVERAGE-GAP: reason=(\w+) void_reason=(\d+) start_ns=(\d+) "
                           r"detected_ns=(\d+) fsync_return_ns=(\d+) duration_ns=(\d+) durable=(\d+)", log.read_text())
        assert lines, log.read_text()
        pause = [r for r in lines if r[0] == "PAUSE" and int(r[1]) == 10]
        assert pause, ("capacity VOID must be measured", lines)
        reason, _, start, detected, returned, duration, durable = pause[0]
        start, detected, returned, duration = map(int, (start, detected, returned, duration))
        assert durable == "1" and returned >= detected and duration == returned - detected, pause[0]
        records, _ = read_trace(trace)
        assert any(r[1] == "GAP_OPEN" and r[2] == 1 and r[3] == start for r in records), \
            "durable PAUSE before any later watermark"
        incomplete_loss(verdict(reader, trace, start, start + 1))
        print(f"SATURATION: PASS incomplete=capture_loss reason={reason} fsync_delay_ns={duration}", flush=True)
    finally:
        stop(daemon)
        error.close()
        if worker and worker.poll() is None:
            worker.kill()
            worker.wait(timeout=5)


def preexisting_gate(pm, root, reader):
    # psql keeps stdin open after the initial command, leaving ClientRead.
    idle = idle_psql()
    daemon = None
    try:
        idle.stdin.write("SELECT 1;\n")
        idle.stdin.flush()
        time.sleep(1)
        assert idle.poll() is None, "pre-existing idle connection is alive"
        trace = root / "preexisting"
        daemon, error, log = start_daemon(TRACER, pm, trace, debug=True)
        try:
            # Measure a sustained quiet interval before making any assertion
            # about W. Reading the first heartbeat says nothing about progress.
            progress = [w_now(trace)]
            for _ in range(10):
                time.sleep(1)
                assert daemon.poll() is None, ("idle daemon exited", log.read_text())
                progress.append(w_now(trace))
            records, _ = read_trace(trace)
            boot = records[0][3]
            w = max(r[3] for r in records if r[1] == "WATERMARK")
            ticks = [dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", line))
                     for line in log.read_text().splitlines() if line.startswith("COVERAGE-TICK:")]
            assert ticks, "idle coverage tick measurement"
            latest = ticks[-1]
            print(f"IDLE-W: w_minus_boot_ns={w - boot} rounds_requested={latest['rounds_requested']} "
                  f"frontiers={latest['frontiers']} data_records={latest['data_records']}", flush=True)
            assert all(a <= b for a, b in zip(progress, progress[1:])), "idle W never regresses"
            assert all(progress[i + 3] > progress[i] for i in range(len(progress) - 3)), (
                "default idle W advances at least every 3 s", progress, log.read_text())
            startup_gaps = [r for r in records if r[1] == "GAP_OPEN" and r[2] == 1 and r[3] == boot]
            assert startup_gaps, "startup PAUSE gap begins at boot_start"
            assert not any(r[1] == "GAP_CLOSE" for r in records), "startup auxiliaries keep PAUSE open"
            incomplete_loss(verdict(reader, trace, boot + 1, boot + 2))
            idle.stdin.close()
            idle.wait(timeout=5)
            exited = time.monotonic_ns()
            # Default idle certification also advances W after the client exits.
            time.sleep(3)
            assert daemon.poll() is None, ("idle daemon exited", log.read_text())
            after_w = w_now(trace)
            print(f"IDLE-W-AFTER-EXIT: before_ns={w} after_ns={after_w} "
                  f"advance_ns={after_w - w}", flush=True)
            assert after_w > w, ("default idle W advances after exit", w, after_w, log.read_text())
            after = verdict(reader, trace, exited + SEC // 4, exited + SEC // 2)
            incomplete_loss(after)
            records, _ = read_trace(trace)
            assert all(r in records for r in startup_gaps), "boot-start PAUSE remains in the trace"
            assert not any(r[1] == "GAP_CLOSE" for r in records), (
                "startup auxiliaries keep PAUSE open after idle client exit", log.read_text())
            print("PREEXISTING: PASS capture_loss before and after exit; boot-start PAUSE stays open", flush=True)
        finally:
            stop(daemon)
            error.close()
    finally:
        if idle.poll() is None:
            idle.kill()
            idle.wait(timeout=5)


def task_cpu_ticks(pid):
    """utime+stime (fields 14/15), keyed by TID, from the live proc task set.

    Sample every thread; after shutdown the production helper diagnostics
    identify which TIDs are helpers, excluding resolver/reader threads.
    """
    result = {}
    for path in Path(f"/proc/{pid}/task").glob("*/stat"):
        fields = path.read_text().rsplit(")", 1)[1].split()
        result[int(path.parent.name)] = int(fields[11]) + int(fields[12])
    assert result, "live task CPU samples"
    return result


def idle_cost_gate(pm, root):
    def measure(enabled):
        option = "on" if enabled else "off"
        trace = root / f"idle-cost-{option}"
        daemon, error, log = start_daemon(TRACER, pm, trace, idle_round=enabled)
        try:
            start_w = w_now(trace)
            start_cpu = task_cpu_ticks(daemon.pid)
            started = time.monotonic()
            progress = [start_w]
            # Exactly one 60 s quiet interval per option; no cost threshold.
            for _ in range(60):
                time.sleep(1)
                assert daemon.poll() is None, ("idle cost daemon exited", log.read_text())
                progress.append(w_now(trace))
            elapsed = time.monotonic() - started
            end_cpu = task_cpu_ticks(daemon.pid)
            stop(daemon)
            error.close()
            helpers = [int(tid) for tid in re.findall(r"exact-perf: schedstat role=helper tid=(\d+)", log.read_text())]
            assert helpers and len(helpers) == len(set(helpers)), "helper TID diagnostics"
            assert all(tid in start_cpu and tid in end_cpu for tid in helpers), "both CPU samples for every helper"
            ticks = sum(end_cpu[tid] - start_cpu[tid] for tid in helpers)
            cpu_pct = 100 * ticks / os.sysconf("SC_CLK_TCK") / elapsed
            advance = (progress[-1] - start_w) / SEC
            print(f"IDLE-PROGRESS: option={option} helper_count={len(helpers)} "
                  f"w_minus_start_s={[round((w - start_w) / SEC, 3) for w in progress]}", flush=True)
            print(f"IDLE-COST: option={option} helper_cpu_pct_of_one_core={cpu_pct:.6f} "
                  f"w_advance_s={advance:.6f}", flush=True)
            # Shutdown asks for a final round; progress/cost above exclude it.
            _, _, voids = qb_prefix(trace / "merge-qualification.bin")
            assert not voids, ("idle cost capture VOID", voids, log.read_text())
            assert all(a <= b for a, b in zip(progress, progress[1:])), "idle W never regresses"
            if enabled:
                assert progress[-1] > start_w, "default idle rounds advance W"
        finally:
            stop(daemon)
            error.close()

    failures = []
    for enabled in (False, True):
        option = "on" if enabled else "off"
        try:
            measure(enabled)
        except Exception as exc:
            failures.append(option)
            print(f"IDLE-COST option={option}: FAIL {type(exc).__name__}: {exc}", flush=True)
        else:
            print(f"IDLE-COST option={option}: PASS", flush=True)
    assert not failures, f"idle cost arms failed: {', '.join(failures)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--pg-version", type=int)
    args = ap.parse_args()
    if sys.platform != "linux" or os.geteuid() != 0:
        print("LIVE-REFUSED: Linux root required; no eBPF on macOS", file=sys.stderr)
        return 78
    if args.pg_version is not None and args.pg_version not in (13, 17, 18):
        print("LIVE-REFUSED: PostgreSQL version has no exact_perf layout", file=sys.stderr)
        return 78
    pm = args.pid or (find_postmaster(pg_version=args.pg_version) if args.pg_version else find_postmaster())
    assert pm, "running postmaster required"
    pgb = find_pgbench()
    with tempfile.TemporaryDirectory(prefix="pgwt-tcov-live-") as tmp:
        root = Path(tmp)
        reader = build_reader(root)
        db = f"pgwt_tcov_{os.getpid()}"
        coverage_live.DB = db
        sql(f"CREATE DATABASE {db}")
        try:
            subprocess.run([pgb, "-i", "-s", "1", "-U", "postgres", db], check=True)
            print("TEST PREMISE: attachment proofs assumed; off EL8 kernel premise assumed; no qualification claim", flush=True)
            failures = []
            arms = [
                ("SIGKILL", lambda: kill_gate(pm, db, pgb, root, reader)),
                ("SATURATION", lambda: saturation_gate(pm, db, pgb, root, reader)),
                ("PREEXISTING", lambda: preexisting_gate(pm, root, reader)),
                ("IDLE-COST", lambda: idle_cost_gate(pm, root)),
            ]
            for name, arm in arms:
                try:
                    arm()
                except Exception as exc:
                    failures.append(name)
                    print(f"{name}: FAIL {type(exc).__name__}: {exc}", flush=True)
        finally:
            sql(f"DROP DATABASE {db}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
