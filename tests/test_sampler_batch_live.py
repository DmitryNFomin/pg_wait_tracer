#!/usr/bin/env python3
"""Status-row syscall gate: 500 idle + 4 pgbench sessions, >=200 traced ticks.

Registered in run_all.sh's live tier. Requires Linux/root, perf and libpq.
Starts a private cluster (540 connections) using the selected runner's PG
binary, leaving its provisioned cluster untouched. N=500 idle + 4 active
proves two status batch calls on all supported versions.
The syscall evidence is the per-syscall summary of `perf trace -S` over the
daemon (counts by name per sampler tick, tests/sampler_batch_perf.py); it
needs no path names, which neither gate image's perf can resolve.
Master negative control: pass --master-tracer (or set PGWT_866_MASTER_TRACER)
to a tracer built from master on the SAME VM; the SAME assertions must then go
RED at the per-backend file I/O counts, and the test prints which ones.
--require-negative-control enforces that it was supplied.
Use --require-huge-pages for the additional huge_pages=on qualification.
All captures/evidence go to a temporary directory outside the repository
(or --evidence-dir). Example:
  sudo python3 tests/test_sampler_batch_live.py --pid PID \
    --master-tracer /tmp/master/pg_wait_tracer --require-negative-control
"""
import argparse
import ctypes
import contextlib
import ctypes.util
import hashlib
import json
import os
import pwd
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

from sampler_batch_perf import CALLS, SUPPORTED_MAJORS, assess_counts, file_io_reds, parse_summary
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
SHADOW = ("sampled_attr_shadow_total", "sampled_attr_shadow_mismatch_total",
          "sampled_attr_shadow_cmd_open_mismatch_total",
          "sampled_attr_shadow_query_id_mismatch_total")


def control(directory, request, timeout=5):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(directory / "pgwt.sock"))
        client.sendall(json.dumps(request).encode() + b"\n")
        data = b""
        while b"\n" not in data:
            part = client.recv(65536)
            assert part, "empty control reply"
            data += part
    result = json.loads(data.splitlines()[0])
    assert result.get("ok") is True, result
    return result


def metrics(directory, cmd="metrics"):
    """A control reply. Counters are in `metrics`; layout fields such as
    pgbackend_layout_validation are only in the `status` reply."""
    return control(directory, {"cmd": cmd})


def spawn(command, **kwargs):
    """Popen with SIGINT back at its default action.

    A test launched from a background job (nohup, `&`, a CI step) inherits
    SIGINT as ignored, and an ignored signal survives exec: pgbench then
    never stopped on the SIGINT sent to end it ("child failed to stop").
    """
    return subprocess.Popen(command, preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL),
                            **kwargs)


@contextlib.contextmanager
def private_cluster(source_pid, require_huge_pages):
    """Use that runner's exact PG binary, a private socket and owned data dir."""
    bindir = Path(os.readlink(f"/proc/{source_pid}/exe")).parent
    postgres = pwd.getpwnam("postgres")
    saved = {key: os.environ.get(key) for key in ("PGHOST", "PGPORT", "PATH")}
    # Short Unix socket path even when --evidence-dir is a long pathname.
    with tempfile.TemporaryDirectory(prefix="pgwt866-pg-", dir="/tmp") as tmp:
        directory = Path(tmp)
        os.chown(directory, postgres.pw_uid, postgres.pw_gid)
        data = directory / "data"
        log = directory / "postgres.log"
        as_postgres = ["runuser", "-u", "postgres", "--"]
        subprocess.run(as_postgres + [str(bindir / "initdb"), "-D", str(data),
                       "-U", "postgres", "-A", "trust", "--no-locale"],
                       check=True, stdout=subprocess.DEVNULL, timeout=60)
        version = subprocess.check_output([str(bindir / "postgres"), "--version"], text=True)
        major = int(re.search(r"PostgreSQL\)\s+(\d+)", version)[1])
        settings = (f"\nmax_connections=540\nshared_buffers='32MB'\n"
                    f"unix_socket_directories='{directory}'\nlisten_addresses=''\nport=55486\n"
                    f"autovacuum=off\nhuge_pages={'on' if require_huge_pages else 'try'}\n")
        if major >= 14:
            settings += "compute_query_id=on\n"
        else:
            # PG13 has no core query id: the exact tier needs pg_stat_statements
            # loaded (as the provisioned runner clusters do), or an escalation
            # is refused ("exact probe bundle attach failed").
            settings += "shared_preload_libraries='pg_stat_statements'\n"
        with (data / "postgresql.conf").open("a") as config:
            config.write(settings)
        os.environ.update(PGHOST=str(directory), PGPORT="55486",
                          PATH=str(bindir) + os.pathsep + os.environ["PATH"])
        try:
            subprocess.run(as_postgres + [str(bindir / "pg_ctl"), "-D", str(data),
                           "-l", str(log), "-w", "-t", "60", "start"],
                           check=True, stdout=subprocess.DEVNULL, timeout=70)
            pid = int((data / "postmaster.pid").read_text().splitlines()[0])
            assert int(psql("SHOW max_connections")) == 540
            assert int(psql("SHOW server_version_num")) // 10000 == major
            yield pid, major
        finally:
            try:
                if (data / "postmaster.pid").exists():
                    subprocess.run(as_postgres + [str(bindir / "pg_ctl"), "-D", str(data),
                                   "-m", "immediate", "-w", "-t", "30", "stop"],
                                   check=True, stdout=subprocess.DEVNULL, timeout=40)
            finally:
                for key, value in saved.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value


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
        self.lib.PQbackendPID.argtypes = [ctypes.c_void_p]
        self.lib.PQbackendPID.restype = ctypes.c_int

    def open(self, count=500, application="pgwt866_idle"):
        for _ in range(count):
            handle = self.lib.PQconnectdb(
                f"user=postgres dbname=postgres application_name={application} connect_timeout=10".encode())
            assert handle, "PQconnectdb allocation failed"
            self.handles.append(handle)
            assert self.lib.PQstatus(handle) == 0, self.lib.PQerrorMessage(handle)

    def pids(self):
        return {self.lib.PQbackendPID(handle) for handle in self.handles}

    def close(self):
        for handle in self.handles:
            self.lib.PQfinish(handle)
        self.handles.clear()


def run_escalation_arm(binary, pm_pid, directory):
    """Fork a named cohort while exact admission is open; prove each resumes."""
    trace = directory / "escalation-born"
    trace.mkdir()
    daemon = None
    cohort = IdleConnections()
    with (trace / "daemon.stderr").open("w") as err:
        try:
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            env["PGWT_DEBUG_SAMPLED_ATTR_SHADOW"] = "1"
            daemon = spawn(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", "10",
                 "--escalation-budget", "120", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90, "escalation socket not ready")
            wait_until(daemon, lambda: metrics(trace).get("sampler_ticks_total", 0) > 0,
                       30, "escalation sampler not ready")
            control(trace, {"cmd": "escalate", "duration_s": 90, "reason": "manual"}, timeout=30)
            before = wait_until(daemon, lambda: (m if (m := metrics(trace)).get("tier") == "escalated"
                                  and m.get("exact_probe_attached_mask", 0) & 3 == 3 else None),
                                30, "exact window did not open")
            cohort.open(4, "pgwt866_escalation_born")
            pids = cohort.pids()
            assert len(pids) == 4 and all(pid > 0 for pid in pids), pids
            wait_until(daemon, lambda: metrics(trace)["backends_tracked"] >= before["backends_tracked"] + 4,
                       30, "escalation-born cohort never entered registry")
            assert metrics(trace)["tier"] == "escalated", "cohort was not forked inside exact window"
            control(trace, {"cmd": "deescalate"}, timeout=30)
            sampled = wait_until(daemon, lambda: (m if (m := metrics(trace)).get("tier") == "sampled"
                                   and m.get("exact_probe_generation") == 0 else None),
                                 30, "de-escalation did not finish")
            offset = (trace / "daemon.stderr").stat().st_size
            def observed():
                text = (trace / "daemon.stderr").read_text()[offset:]
                return pids <= {int(pid) for pid in re.findall(
                    r"SAMPLED-ATTR-IDLE: pid=(\d+) databaseid=[1-9]\d* userid=[1-9]\d*", text)}
            wait_until(daemon, observed, 15, "escalation-born cohort lacks attributed samples after de-escalation")
            assert metrics(trace)["sampled_attr_tick_idle_samples_total"] > sampled["sampled_attr_tick_idle_samples_total"]
            print(f"escalation-born cohort: PASS pids={sorted(pids)} attributed after de-escalation", flush=True)
        finally:
            try:
                stop(daemon)
            finally:
                cohort.close()


def run_late_promotion_arm(binary, pm_pid, directory):
    """Existing sessions are scanned while the layout is degraded. The existing
    validation-failure hook forces the auth-free warmup to be the promoter.
    Both varying query ids and active/idle states are present before startup.
    """
    trace = directory / "late-promotion"
    trace.mkdir()
    script = trace / "vary.sql"
    script.write_text("\\set shape random(0, 1)\n\\if :shape\n"
                      "SELECT pg_sleep(0.02);\n\\else\n"
                      "SELECT pg_sleep(0.02), 42;\n\\endif\n\\sleep 20 ms\n")
    daemon = load = None
    cohort = IdleConnections()
    with (trace / "daemon.stderr").open("w") as err, \
         (trace / "pgbench.stdout").open("w") as out, \
         (trace / "pgbench.stderr").open("w") as load_err:
        try:
            cohort.open(4, "pgwt866_before_promotion")
            pids = cohort.pids()
            assert len(pids) == 4 and all(pid > 0 for pid in pids), pids
            load = spawn(
                ["pgbench", "-n", "-U", "postgres", "-c", "4", "-j", "4",
                 "-T", "120", "-f", str(script), "postgres"], stdout=out, stderr=load_err,
                env={**os.environ, "PGAPPNAME": "pgwt866_promotion_load"})
            wait_until(load, lambda: int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                            "application_name='pgwt866_promotion_load'")) == 4,
                       30, "promotion workload must precede daemon startup")
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            env.update(PGWT_TEST_PGBS_VALIDATION_FAIL="1", PGWT_DEBUG_SAMPLED_ATTR_SHADOW="1")
            daemon = spawn(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", "10",
                 "--escalation-budget", "0", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90,
                       "late-promotion control socket not ready")
            def observed():
                text = (trace / "daemon.stderr").read_text()
                return pids <= {int(pid) for pid in re.findall(
                    r"SAMPLED-ATTR-IDLE: pid=(\d+) databaseid=[1-9]\d* userid=[1-9]\d*", text)}
            wait_until(daemon, observed, 15, "pre-promotion cohort lacks attributed samples")
            layout = metrics(trace, "status")
            text = (trace / "daemon.stderr").read_text()
            assert "forced validation failure (test hook)" in text, text
            assert "bounded uprobe shadow validation completed" in text, text
            assert layout["pgbackend_layout_validation"] == "validated", layout
            assert load.poll() is None, "promotion workload stopped before verification"
            print(f"late layout promotion: PASS pre-existing pids={sorted(pids)} attributed", flush=True)
        finally:
            try:
                stop(daemon)
            finally:
                try:
                    stop(load, signal.SIGINT)
                finally:
                    cohort.close()


def run_arm(binary, pm_pid, major, directory, label, disable_status_batch=False):
    trace = directory / label
    trace.mkdir()
    perf = daemon = None
    with (trace / "daemon.stderr").open("w") as err, (trace / "perf.stderr").open("w") as perr:
        try:
            # Do not let another test's injection alter this production arm.
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            if disable_status_batch:
                env["PGWT_TEST_DISABLE_STATUS_BATCH"] = "1"
            daemon = spawn(
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
            # Sampler ticks bracket perf from both sides. `spanning` starts before
            # perf is launched and ends after it exited, so it contains every tick
            # perf could have seen; `inside` runs between two reads that both fall
            # inside perf's window. Calls are divided by the count that cannot
            # redden a bound by window misalignment (see assess_counts).
            spanning_start = metrics(trace)["sampler_ticks_total"]
            perf_path = trace / "perf.trace"
            # -S: the events as they happen (their arrival proves perf is attached)
            # and, at the end, the per-syscall summary that is the evidence. The
            # summary needs no path names, which neither gate image can resolve.
            perf = spawn(
                ["perf", "trace", "-S", "--no-comm", "-m", "1024", "-p", str(daemon.pid),
                 "-e", CALLS, "-o", str(perf_path)],
                stdout=subprocess.DEVNULL, stderr=perr)
            # Require actual trace data before opening the measured window.
            wait_until(perf, lambda: perf_path.exists() and perf_path.stat().st_size > 0,
                       15, "perf produced no trace")
            before = metrics(trace)
            target_ticks = before["sampler_ticks_total"] + 210
            after = wait_until(daemon, lambda: (m if (m := metrics(trace))["sampler_ticks_total"]
                                               >= target_ticks else None),
                               90, "fewer than 210 sampler ticks")
            assert perf.poll() is None, "perf exited during measurement"
            time.sleep(1)  # drain exits for syscalls entered inside the window
            stop(perf, signal.SIGINT)
            assert perf.returncode in (0, -signal.SIGINT), (trace / "perf.stderr").read_text()
            spanning_end = metrics(trace)["sampler_ticks_total"]
            ticks = after["sampler_ticks_total"] - before["sampler_ticks_total"]
            spanning = spanning_end - spanning_start
            population = min(before["sample_read_targets"], after["sample_read_targets"])
            assert population >= 504, "500 idle + 4 active targets must remain enrolled"
            failures = (after["sampled_attr_tick_read_failures_total"] -
                        before["sampled_attr_tick_read_failures_total"])
            idle_samples = (after.get("sampled_attr_tick_idle_samples_total", 0) -
                            before.get("sampled_attr_tick_idle_samples_total", 0))
            assert before["sample_read_targets"] == after["sample_read_targets"], "population changed"
            assert idle_samples >= 0, "idle success counter moved backwards"
            # Even if EVERY other target were an idle client on EVERY tick,
            # successes above that ceiling must come from our 500 idle pids.
            # This proves cohort success without per-pid logging under perf.
            cohort_idle_samples = max(0, idle_samples - (population - 500) * ticks)
            layout = metrics(trace, "status")
            assert layout["pgbackend_layout_validation"] == "validated", layout
            counts = parse_summary(perf_path.read_text(), (str((trace / "perf.stderr").read_text()),))
            # Use the guaranteed cohort N, excluding version-dependent aux
            # populations with no attribution row (notably PG13).
            result = assess_counts(counts, major, 504, ticks, spanning, failures, cohort_idle_samples)
            result.update(label=label, major=major, read_failures=failures, binary=str(binary),
                          binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                          population=population, ticks=ticks, ticks_spanning=spanning,
                          calls=counts, idle_samples_total=idle_samples,
                          idle_cohort_samples_lower_bound=cohort_idle_samples,
                          shadow_delta={key: after[key] - before[key] for key in SHADOW})
            (trace / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, sort_keys=True), flush=True)
            return result
        finally:
            try:
                stop(perf, signal.SIGINT)
            finally:
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
    source_pid = args.pid or find_postmaster()
    assert source_pid, "no runner postmaster from which to select the PG binary"
    version = subprocess.check_output([os.readlink(f"/proc/{source_pid}/exe"), "--version"], text=True)
    major = int(re.search(r"PostgreSQL\)\s+(\d+)", version)[1])
    if major not in SUPPORTED_MAJORS:
        print(f"EXCLUDED: PG{major}; status batch needs PostgreSQL 13, 17 or 18")
        return
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
    with evidence_context as tmp, private_cluster(source_pid, args.require_huge_pages) as (pid, major):
        huge_pages = psql("SHOW huge_pages")
        print(f"private PG{major} pid={pid} kernel={os.uname().release} "
              f"max_connections=540 huge_pages={huge_pages} N=504 (500 idle + 4 active)", flush=True)
        directory = Path(tmp)
        script = directory / "workload.sql"
        script.write_text("SELECT sum(i) FROM generate_series(1, 10000) AS i;\n")
        try:
            run_escalation_arm(ROOT / "pg_wait_tracer", pid, directory)
            if major >= 17:
                run_late_promotion_arm(ROOT / "pg_wait_tracer", pid, directory)
            else:
                # The existing hook returns before PG13's activity-buffer
                # size is discovered. Warmup cannot enable its tick source
                # with that size unset; the injected registry test covers
                # late promotion for PG13 without changing validation rules.
                print("PG13 late promotion: unit-covered; forced validation failure "
                      "precedes activity-buffer size discovery", flush=True)
            idle.open()
            assert int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                            "application_name='pgwt866_idle' AND state='idle'")) == 500
            with (directory / "pgbench.stdout").open("w") as out, (directory / "pgbench.stderr").open("w") as err:
                load = spawn(
                    ["pgbench", "-n", "-U", "postgres", "-c", "4", "-j", "4",
                     "-R", "100", "-T", "600", "-f", str(script), "postgres"], stdout=out, stderr=err,
                    env={**os.environ, "PGAPPNAME": "pgwt866_active"})
                wait_until(load, lambda: int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                               "application_name='pgwt866_active'")) == 4,
                           30, "four pgbench sessions not ready")
                candidate = run_arm(ROOT / "pg_wait_tracer", pid, major, directory, "candidate")
                assert not candidate["errors"], candidate["errors"]
                disabled = run_arm(ROOT / "pg_wait_tracer", pid, major, directory,
                                   "status-disabled-negative", disable_status_batch=True)
                # With the batch disabled no row is read, so every unread backend keeps
                # retrying its address resolution: measured 16.7 process_vm_readv per
                # tick (EL8, PG13), above the upper bound rather than below the floor.
                # Either direction is a RED at the process_vm_readv assertion; the
                # floor itself is proven live by the master arm (1 per tick).
                assert any(e.startswith("vm/tick=") for e in disabled["errors"]), disabled
                assert any("idle attributed samples=0" in e for e in disabled["errors"]), disabled
                print("status batch disabled: RED at the syscall assertion and the attribution floor: "
                      + "; ".join(disabled["errors"]), flush=True)
                if args.master_tracer:
                    master = run_arm(args.master_tracer.resolve(), pid, major, directory, "master-negative")
                    # The per-backend file I/O assertion itself must fire, not just
                    # some other bound.
                    file_io = file_io_reds(master["errors"])
                    assert file_io, ("master negative control did not redden the per-backend "
                                     f"file I/O assertion: {master['errors']} per_tick={master['per_tick']}")
                    for key in SHADOW[1:]:
                        assert candidate["shadow_delta"][key] <= master["shadow_delta"][key], \
                            (key, candidate["shadow_delta"], master["shadow_delta"])
                    print("master negative control: RED at " + "; ".join(master["errors"]) +
                          "; shadow deltas no worse")
                    if not master["shadow_delta"][SHADOW[0]]:
                        print("shadow comparison has zero observations (no attribute uprobes in baseline)")
                else:
                    print("PENDING: lead-run master negative control; pass --master-tracer")
                assert load.poll() is None, "pgbench stopped before measurement completed"
                # SIGINT kills pgbench without its report; SIGALRM is the signal -T
                # itself uses, so it ends the run normally and prints the summary.
                stop(load, signal.SIGALRM)
                workload = (directory / "pgbench.stdout").read_text()
                assert re.search(r"number of transactions actually processed:\s*[1-9]", workload), workload
                print("status-row syscall candidate gate: PASS (>=200 ticks, 500 idle + 4 pgbench)")
        finally:
            try:
                stop(load, signal.SIGINT)
            finally:
                idle.close()


if __name__ == "__main__":
    main()
