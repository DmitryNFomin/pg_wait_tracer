#!/usr/bin/env python3
"""Linux live red demonstrations for #871 capture failure accounting.

Each rodata injection claims one eligible operation per load. A clean pgbench
arm runs first and reports any natural failures at their observed values. PG13
exercises its query-text route; PG17/18 exercise pgstat_report_query_id instead.
Both routes and the remaining silent paths also have host C cases/mutants.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from demo_rehearsal_lib import daemon_integrity_ok
from test_init_hint_recovery import ctl, stop, wait_for
from testutil import find_postmaster, phase_contract_apply

TRACER = Path(__file__).resolve().parents[1] / "pg_wait_tracer"
NEW_COUNTERS = (
    "wait_read_fail_total", "exact_query_update_fail_total", "exact_cmd_update_fail_total",
    "exact_phase_update_fail_total", "accum_update_fail_total", "fork_state_update_fail_total",
    "writer_push_fail_total", "init_state_read_fail_total",
)
FAULTS = (
    ("wait_read", "wait_read_fail_total"),
    ("exact_query", "exact_query_update_fail_total"),
    ("exact_cmd", "exact_cmd_update_fail_total"),
    ("accum", "accum_update_fail_total"),
    ("fork_state", "fork_state_update_fail_total"),
)


def run_arm(pm_pid, fault, target=None, *, expected_phase=None, mode="full"):
    label = (fault or "clean") + (f" ({mode})" if mode != "full" else "")
    with tempfile.TemporaryDirectory(prefix="pgwt-counter-live-") as tmp:
        directory = Path(tmp)
        env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
        if fault == "writer":
            env["PGWT_TEST_WRITER_FAIL"] = "1"
        elif fault:
            env["PGWT_TEST_COUNTER_FAILURE"] = fault
        workload = directory / "workload.sql"
        workload.write_text("SELECT pg_sleep(0.01);\nSELECT 1;\n")
        log = directory / "daemon.stderr"
        with log.open("w") as err:
            proc = subprocess.Popen(
                [str(TRACER), "--mode", mode, "--pid", str(pm_pid), "-T", str(directory),
                 "-i", "1", "--duration", "60", "-q"],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            try:
                wait_for(proc, lambda: (directory / "pgwt.sock").exists(), 45,
                         "counter test control socket not ready")
                load = subprocess.run(
                    ["pgbench", "-n", "-U", "postgres", "-d", "postgres", "-c", "1", "-j", "1",
                     "-T", "3", "-f", str(workload)],
                    capture_output=True, text=True, timeout=30, env=env)
                assert load.returncode == 0, (load.stdout, load.stderr)
                assert re.search(r"number of transactions actually processed:\s*[1-9]", load.stdout), load.stdout
                metrics = ctl(directory)
                for key in NEW_COUNTERS:
                    assert type(metrics.get(key)) is int and metrics[key] >= 0, (key, metrics)
                print(f"counter-vector live {label}: {json.dumps(metrics, sort_keys=True)}", flush=True)
                assert metrics.get("counter_vector_valid") is True, metrics
                if expected_phase is not None:
                    phase_contract_apply(
                        expected_phase, [metrics.get("exact_probe_attached_mask")],
                        {target: metrics.get(target)}, "counter-vector exact_phase")
                if fault:
                    # A missing stimulus is a failure; do not repeat the arm.
                    assert metrics[target] > 0, (fault, target, metrics)
                    ok, detail = daemon_integrity_ok(metrics)
                    assert not ok and target in detail, detail
                    knob = "PGWT_TEST_WRITER_FAIL" if fault == "writer" else "PGWT_TEST_COUNTER_FAILURE=" + fault
                    assert knob in log.read_text(), log.read_text()
                else:
                    for key in NEW_COUNTERS:
                        assert metrics[key] == 0, f"natural failure: {key}={metrics[key]} (wait read rate belongs to Refs #909)"
            finally:
                stop(proc)
        print(f"counter-vector live {label}: PASS", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "no postmaster for live counter test"
    version = subprocess.check_output(
        ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt", "-v", "ON_ERROR_STOP=1",
         "-c", "SHOW server_version_num"], text=True).strip()
    major = int(version) // 10000
    assert major in (13, 17, 18), version
    run_arm(pm_pid, None)
    for fault, target in FAULTS:
        run_arm(pm_pid, fault, target)
    run_arm(pm_pid, "pg13_query_text" if major == 13 else "query_text",
            "ringbuf_pg13_query_text_drops_total" if major == 13 else "ringbuf_query_text_drops_total")
    run_arm(pm_pid, "writer", "writer_push_fail_total")
    run_arm(pm_pid, "writer", "writer_push_fail_total", mode="sampled")


if __name__ == "__main__":
    main()
