#!/usr/bin/env python3
"""#622: forced USDT absence and its own FULL-mode positive control.

Run both legs against the same postmaster on a phase-capable Ubuntu tier.
The seam rehearses EL8's consequence, not the missing-ref_ctr_offset cause.
This test owns both declarations: refusal is a successful assertion, never 78.
"""
import argparse
import errno
import json
import math
import os
import socket
import subprocess
import sys
import tempfile
import time

from server_harness import SERVER_BIN, ServerHarness
from testutil import ATTR_MASK, PHASE_MASK, find_postmaster, phase_contract_verdict

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")
FIRE_FIELDS = ("exact_query_uprobe_fires_total", "exact_activity_uprobe_fires_total")
OUTCOME_FIELDS = ("exact_probe_exec_attach_outcome", "exact_probe_plan_attach_outcome")
ERRNO_FIELDS = ("exact_probe_exec_attach_errno", "exact_probe_plan_attach_errno")
tests_run = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_failed
    tests_run += 1
    if not cond:
        tests_failed += 1
    print(f"  {'PASS' if cond else 'FAIL'}: {msg}")


def ctl(trace_dir, request, timeout=5):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(os.path.join(trace_dir, "pgwt.sock"))
        client.sendall((json.dumps(request) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        return json.loads(data.splitlines()[0])


def numeric_field(status, field):
    # #622: missing/unreadable telemetry must redden the leg. In particular,
    # .get(field, 0) would turn a broken reader into evidence of absence.
    value = status.get(field)
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or value < 0 or value != int(value)):
        raise ValueError(f"status field {field} absent/non-numeric: {value!r}")
    return int(value)


def validate_status(status):
    if not isinstance(status, dict) or status.get("ok") is not True:
        raise ValueError(f"status did not succeed: {status!r}")
    numeric_field(status, "exact_probe_attached_mask")
    for field in FIRE_FIELDS + ERRNO_FIELDS:
        numeric_field(status, field)
    for field in OUTCOME_FIELDS:
        if not isinstance(status.get(field), str) or not status[field]:
            raise ValueError(f"status field {field} absent/non-string: {status.get(field)!r}")
    return status


def wait_ready(proc, trace_dir):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        if os.path.exists(os.path.join(trace_dir, "pgwt.sock")):
            try:
                status = ctl(trace_dir, {"cmd": "status"},
                             timeout=min(5, max(0.001, deadline - time.monotonic())))
            except (OSError, ValueError, IndexError):
                pass
            else:
                if isinstance(status, dict) and status.get("ok") is True:
                    # Validate outside the readiness exception handler: an
                    # answering daemon with absent fields fails immediately.
                    return validate_status(status)
        # Bounded socket-readiness polling, as in execution_span_live; no
        # settle/drain delay or re-capture can turn a failed verdict green.
        time.sleep(0.1)
    raise RuntimeError(f"daemon never became ready within 60s (rc={proc.poll()})")


def stop_daemon(proc):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
        raise RuntimeError("daemon did not stop/flush within 30s")


def workload():
    # Same short SQL batch on each call, independent of the measured mask.
    # The sleeps are workload waits that generate records, not readiness.
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-qAt",
         "-v", "ON_ERROR_STOP=1"],
        input="SELECT pg_backend_pid();\n" + "SELECT pg_sleep(0.02);\n" * 4,
        capture_output=True, text=True, timeout=30)
    lines = result.stdout.strip().splitlines()
    if result.returncode != 0 or not lines or not lines[0].isdigit():
        raise RuntimeError(f"workload failed: rc={result.returncode}, "
                           f"stdout={result.stdout!r}, stderr={result.stderr!r}")
    return int(lines[0])


def marker_artifacts(trace_dir, pids):
    with ServerHarness(trace_dir) as srv:
        reply = srv.query("executions", limit=200, timeout=15)
    if "error" not in reply and isinstance(reply.get("rows"), list):
        return sum(1 for row in reply["rows"] if row.get("pid") in pids
                   and row.get("end_ns") is not None
                   and row.get("in_progress") is False
                   and row.get("started_before_window") is False)
    if (reply.get("code") == "full_fidelity_required"
            and reply.get("fidelity") in ("sampled", "none")
            and "rows" not in reply):
        return None
    raise ValueError(f"executions lacks rows or explicit exact-data refusal: {reply!r}")


def capture(pm_pid, trace_dir, forced):
    env = os.environ.copy()
    # A/B isolation includes an inherited forcing instruction: CONTROL must
    # exercise real USDT attaches even when the enclosing suite forces absence.
    env.pop("PGWT_TEST_NO_USDT_SEMA_REFCNT", None)
    env.pop("PGWT_TEST_EXEC_SITE_ADMIT_ALL", None)
    if forced:
        env["PGWT_TEST_NO_USDT_SEMA_REFCNT"] = "1"
    with open(os.path.join(trace_dir, "daemon.err"), "w") as err:
        proc = subprocess.Popen(
            [TRACER, "--mode", "full", "--pid", str(pm_pid),
             "--trace-dir", trace_dir, "--duration", "120",
             "--interval", "1", "--quiet"],
            stdout=subprocess.DEVNULL, stderr=err, env=env)
        try:
            statuses = [wait_ready(proc, trace_dir)]
            pids = set()
            for _ in range(3):
                pids.add(workload())
                statuses.append(validate_status(ctl(trace_dir, {"cmd": "status"})))
            if proc.poll() is not None:
                raise RuntimeError(f"daemon exited during capture: rc={proc.returncode}")
        finally:
            stop_daemon(proc)
    masks = [numeric_field(status, "exact_probe_attached_mask") for status in statuses]
    return masks, statuses[-1], {"executions": marker_artifacts(trace_dir, pids)}


def assert_forced(masks, status, artifacts):
    union = 0
    for mask in masks:
        union |= mask
    check(union == 0x3, f"1 FORCED mask union exactly 0x3 (samples={masks})")
    check((union & ATTR_MASK) == ATTR_MASK,
          f"2 FORCED required attribution pair present (union=0x{union:x})")
    outcomes = {field: status[field] for field in OUTCOME_FIELDS}
    errors = {field: numeric_field(status, field) for field in ERRNO_FIELDS}
    check(all(value == "attach_failed" for value in outcomes.values())
          and all(value == errno.ENOTSUP for value in errors.values()),
          f"3 FORCED exec and plan attach_failed + ENOTSUP: {outcomes}, {errors}")
    fires = {field: numeric_field(status, field) for field in FIRE_FIELDS}
    check(all(value > 0 for value in fires.values()),
          f"4 FORCED query and activity plain uprobes still fire: {fires}")
    verdict, reason = phase_contract_verdict(0, masks, artifacts)
    check(verdict == "refuse", f"5 FORCED declared-0 verdict refuse: {verdict}: {reason}")


def assert_control(masks, artifacts):
    # Union inside the existing contract tolerates a legitimate 0x7 sample
    # while attached_mask is assembled; no individual sample must equal 0xF.
    verdict, reason = phase_contract_verdict(PHASE_MASK, masks, artifacts)
    check(verdict == "pass", f"6 CONTROL declared-present verdict pass: {verdict}: {reason}")
    count = artifacts["executions"]
    check(count is not None and count > 0,
          f"7 CONTROL produced completed marker-derived executions: {count}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    print("=== test_phase_absent ===")
    if sys.platform != "linux" or os.geteuid() != 0:
        check(False, "requires Linux and root for live eBPF capture")
        return 1
    pm_pid = args.pid or find_postmaster()
    if not pm_pid or not all(os.path.isfile(path) for path in (TRACER, SERVER_BIN)):
        check(False, "requires a running postmaster and built tracer/server")
        return 1
    for forced, label in ((True, "FORCED"), (False, "CONTROL")):
        trace_dir = tempfile.mkdtemp(prefix=f"pgwt_phase_{label.lower()}_")
        os.chmod(trace_dir, 0o755)
        print(f"  {label}: postmaster={pm_pid}, trace={trace_dir}")
        try:
            masks, status, artifacts = capture(pm_pid, trace_dir, forced)
            if forced:
                assert_forced(masks, status, artifacts)
            else:
                assert_control(masks, artifacts)
        except (OSError, ValueError, RuntimeError, IndexError,
                subprocess.SubprocessError) as exc:
            check(False, f"{label}: {exc}")
            err_path = os.path.join(trace_dir, "daemon.err")
            if os.path.exists(err_path):
                with open(err_path) as err:
                    print(f"  {label} daemon stderr:\n{err.read()}")
    print(f"\n{tests_run - tests_failed}/{tests_run} checks passed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
