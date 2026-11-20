#!/usr/bin/env python3
"""Measure CPU between genuine main-thread sleeps using independent perf events.

Linux/root only. No scheduler tunables are changed. Use --pid for a provisioned
PG13/17/18 postmaster; the workload uses a private cluster and 500 idle sessions.
Each mode gets one 30-second saturated, parallel-query connection-churn load.
Raw perf data, text, daemon logs, and JSON are saved outside the repository.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

from test_sampler_batch_live import private_cluster, IdleConnections, psql, stop, spawn
from test_rt_priority_live import ctl
from test_init_hint_recovery import wait_for

ROOT = Path(__file__).resolve().parents[1]
SWITCH = re.compile(r"\s(\d+)\.(\d+):\s+sched:sched_switch:\s+.*?:(\d+) "
                    r"\[(\d+)\]\s+(\S+)\s+==>\s+.*?:(\d+) \[(\d+)\]")


def analyze(text, tid):
    assert not re.search(r"LOST|lost", text), "lost scheduler events"
    on = None
    cpu = 0
    complete = False
    runs = []
    events = 0
    for line in text.splitlines():
        if "sched:sched_switch:" not in line:
            continue
        match = SWITCH.search(line)
        assert match, ("unknown perf switch format", line)
        sec, fraction, prev, prev_prio, state, nxt, next_prio = match.groups()
        stamp = int(sec) * 10**9 + int(fraction.ljust(9, "0"))
        prev, nxt = int(prev), int(nxt)
        if prev != tid and nxt != tid:
            continue
        events += 1
        if prev == tid:
            assert int(prev_prio) == 98, "main stopped being FIFO 1 during measurement"
            if on is not None:
                assert stamp >= on, "scheduler timestamps reversed"
                cpu += stamp - on
                on = None
            elif complete:
                raise AssertionError("missing switch-in event")
            if state not in ("R", "R+"):
                if complete:
                    runs.append(cpu)
                complete, cpu = True, 0
        if nxt == tid:
            assert int(next_prio) == 98, "main stopped being FIFO 1 during measurement"
            assert on is None, "missing switch-out event"
            on = stamp
    # The first partial run and final incomplete run cannot set the maximum.
    assert runs and events, "no complete main-thread runs"
    assert on is None and cpu == 0, "recording ended with an incomplete CPU run"
    return dict(longest_ns=max(runs), completed_runs=len(runs), switch_events=events,
                measured_cpu_ns=sum(runs), lost_events=0)


def measure(pm_pid, directory, mode, seconds):
    trace = directory / mode
    trace.mkdir()
    workload = trace / "parallel.sql"
    workload.write_text("SET max_parallel_workers_per_gather=2;\n"
                        "SET min_parallel_table_scan_size=0;\n"
                        "SET parallel_setup_cost=0;\nSET parallel_tuple_cost=0;\n"
                        "SELECT sum(sqrt(abalance + 100)) FROM pgbench_accounts;\n")
    log = trace / "daemon.log"
    with log.open("w") as output:
        daemon = spawn([str(ROOT / "pg_wait_tracer"), "--pid", str(pm_pid),
                        "--mode", mode, "--sample-rate", "100", "-q", "-T", str(trace),
                        "--rt-soft-us", "5000000", "--rt-hard-us", "10000000"],
                       stdout=subprocess.DEVNULL, stderr=output)
        perf = load = None
        try:
            wait_for(daemon, lambda: (trace / "pgwt.sock").exists(), 120, "measurement daemon not ready")
            assert ctl(trace)["rt_priority"] == "active", log.read_text()
            perf_data = trace / "perf.data"
            with (trace / "perf.log").open("w") as perf_log:
                perf = spawn(["perf", "record", "--clockid", "mono", "-a", "-m", "1024",
                              "-e", "sched:sched_switch", "--filter",
                              f"prev_pid == {daemon.pid} || next_pid == {daemon.pid}",
                              "-o", str(perf_data)], stdout=subprocess.DEVNULL, stderr=perf_log)
                # Readiness synchronization, never a repeated workload attempt.
                wait_for(perf, lambda: perf_data.exists() and perf_data.stat().st_size > 0,
                         10, "perf recording not ready")
                before = ctl(trace)
                with (trace / "pgbench.log").open("w") as load_log:
                    load = spawn(["pgbench", "-U", "postgres", "-d", "postgres", "-n", "-C",
                                  "-c", "16", "-j", "8", "-T", str(seconds), "-f", str(workload)],
                                 stdout=load_log, stderr=subprocess.STDOUT)
                    time.sleep(seconds / 2)
                    parallel = int(psql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='parallel worker'"))
                    assert parallel > 0, "planned parallel query did not launch workers under load"
                    assert load.wait(timeout=seconds + 60) == 0, (trace / "pgbench.log").read_text()
                # Collect idle sleeps after load so the final CPU run is complete.
                time.sleep(2)
                after = ctl(trace)
                assert after["rt_priority"] == "active" and after["rt_priority_demotions_total"] == 0, after
                # End at a known sleeping boundary rather than censoring the
                # last run. This affects only the observed daemon, after load.
                os.kill(daemon.pid, signal.SIGSTOP)
                wait_for(daemon, lambda: re.search(r"^State:\s+T", Path(
                    f"/proc/{daemon.pid}/status").read_text(), re.M),
                    10, "measurement daemon did not stop")
                stop(perf, signal.SIGINT)
                os.kill(daemon.pid, signal.SIGCONT)
                assert perf.returncode in (0, -signal.SIGINT), (trace / "perf.log").read_text()
            result = subprocess.run(["perf", "script", "--ns", "--show-lost-events", "-i", str(perf_data)],
                                    capture_output=True, text=True, check=True, timeout=60)
            (trace / "switches.txt").write_text(result.stdout)
            assert not re.search(r"LOST|lost", result.stderr + (trace / "perf.log").read_text()), result.stderr
            summary = analyze(result.stdout, daemon.pid)
            summary.update(mode=mode, seconds=seconds, idle_sessions=500, clients=16,
                           new_connections=True, workers_per_gather=2, observed_parallel_workers=parallel,
                           pgbench=(trace / "pgbench.log").read_text(),
                           events_delta=after["events_total"] - before["events_total"],
                           samples_delta=after["samples_total"] - before["samples_total"])
            assert summary["completed_runs"] >= 100, summary
            assert re.search(r"number of transactions actually processed:\s*[1-9]", summary["pgbench"]), summary
            assert summary["events_delta"] > 0 if mode == "full" else summary["samples_delta"] > 0, summary
            (trace / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            print(json.dumps(summary, sort_keys=True), flush=True)
            return summary
        finally:
            stop(load)
            if perf is not None and perf.poll() is None:
                stop(perf, signal.SIGINT)
            if daemon.poll() is None:
                os.kill(daemon.pid, signal.SIGCONT)
            stop(daemon)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    args.evidence_dir.mkdir(parents=True, exist_ok=False)
    with private_cluster(args.pid, False) as (pm_pid, major):
        subprocess.run(["pgbench", "-U", "postgres", "-i", "-s", "10", "postgres"],
                       check=True, stdout=subprocess.DEVNULL, timeout=120)
        plan = psql("SET max_parallel_workers_per_gather=2; SET min_parallel_table_scan_size=0; "
                    "SET parallel_setup_cost=0; SET parallel_tuple_cost=0; "
                    "EXPLAIN SELECT sum(sqrt(abalance + 100)) FROM pgbench_accounts")
        assert "Workers Planned: 2" in plan, plan
        (args.evidence_dir / "plan.txt").write_text(plan)
        idle = IdleConnections()
        try:
            idle.open(500, "pgwt989_idle")
            results = [measure(pm_pid, args.evidence_dir, mode, args.seconds)
                       for mode in ("full", "sampled")]
            summary = dict(kernel=os.uname().release, pg_major=major, arms=results,
                           longest_ns=max(result["longest_ns"] for result in results))
            (args.evidence_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            print("MEASURED: " + json.dumps(summary, sort_keys=True), flush=True)
        finally:
            idle.close()


if __name__ == "__main__":
    main()
