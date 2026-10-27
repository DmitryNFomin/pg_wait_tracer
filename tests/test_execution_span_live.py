#!/usr/bin/env python3
"""test_execution_span_live.py — #479: containing spans on a REAL capture.

THE DEFECT. The Waterfall's containing-activity span (execution_detail's
backend_activity_span) was refused on EVERY execution of EVERY real capture,
and every gate in the tree was green while it was. The only coverage was
synthetic fixtures and tests/mock_server.py, which always emits a populated
span. Measured on the retained release-b7228ea rehearsal trace: 4 of 4
executions refused.

WHY REAL DATA MATTERS. An idle session holds a Client:ClientRead still open
at the clicked execution's right edge. #424's straddler lookahead admits
that record with its OWN closing timestamp, tens of seconds past the
window. The [pre-markers][window][post-markers] array the span pass reads
was therefore not ascending, and pgwt_span_reduce refused rather than
invent a boundary.

WHAT MAKES THIS A GATE. A span appearing is not enough: this capture must
demonstrably CONTAIN the hazard, and real marker-identified executions must
return real spans despite it. Session B is parked BEFORE capture and its
ClientRead closes AFTER session A's batch. Per-request stderr counters
prove right-edge straddlers were actually admitted. A run that cannot
establish the hazard FAILS and says so; "I could not look" is exactly how
#479 survived. No retries, tolerances or skips turn that into agreement.

Requires: Linux, root, running PostgreSQL, pg_wait_tracer and pgwt-server built.
Usage: sudo python3 tests/test_execution_span_live.py [--pid PM_PID]
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import SERVER_BIN, ServerHarness
from testutil import (find_postmaster, resolve_declared_phase_mask,
                      phase_contract_apply)

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")
N = 12
CLICK_N = 5
CAPTURE_DURATION_S = 120
CAPTURE_SETTLE_S = 2.0
CAPTURE_DRAIN_S = 3.0
HOLD_AFTER_A_S = 5.0
STRADDLE_REPORT = re.compile(
    r"^pgwt-straddle: admitted=(\d+) truncated=(\d+) "
    r"headers_unreadable=(\d+) lookahead_ns=\d+ max_blocks=\d+$", re.M)

tests_run = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_failed
    tests_run += 1
    if cond:
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def fatal(msg):
    """An unmet precondition is a failure, never a skip. run_all.sh already
    gates the live tier on root and running PostgreSQL."""
    global tests_failed
    tests_failed += 1
    print(f"  FAIL: {msg}")
    print(f"test_execution_span_live: FAILED ({tests_failed} failures)")
    sys.exit(1)


def psql(sql):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        fatal(f"psql failed (rc={result.returncode}): {result.stderr[-600:]!r}")
    return result.stdout.strip()


def wait_for_idle_in_tx(exclude_pids, timeout=30):
    """Poll for our newly parked ClientRead, excluding existing idle sessions.
    These are readiness polls, never retries of a failed capture or verdict."""
    deadline = time.monotonic() + timeout
    excl = ",".join(str(p) for p in exclude_pids) or "-1"
    sql = (
        "SELECT pid FROM pg_stat_activity "
        "WHERE backend_type = 'client backend' "
        "AND state = 'idle in transaction' "
        "AND wait_event_type = 'Client' AND wait_event = 'ClientRead' "
        f"AND pid NOT IN ({excl}) "
        "ORDER BY backend_start DESC LIMIT 1")
    while time.monotonic() < deadline:
        out = psql(sql)
        if out:
            return int(out.splitlines()[0])
        time.sleep(0.25)
    return None


def wait_for_end(pid, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        out = psql(f"SELECT state FROM pg_stat_activity WHERE pid = {pid}")
        if not out:
            fatal(f"session B (pid={pid}) disappeared before END was observed")
        if out != "idle in transaction":
            return
        time.sleep(0.25)
    fatal(f"session B (pid={pid}) stayed idle in transaction after END for 15s")


def psql_batch(statements, label):
    """One psql session; its OWN pg_backend_pid() identifies A's executions."""
    script = "SELECT pg_backend_pid();\n" + "\n".join(statements) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as fh:
        fh.write(script)
        path = fh.name
    try:
        res = subprocess.run(
            ["psql", "-U", "postgres", "-d", "postgres", "-q", "-t", "-A",
             "-v", "ON_ERROR_STOP=1", "-f", path],
            capture_output=True, text=True, timeout=180)
    finally:
        os.unlink(path)
    lines = (res.stdout or "").strip().splitlines()
    if res.returncode != 0 or not lines or not lines[0].strip().isdigit():
        fatal(f"{label}: could not run batch/read its backend pid "
              f"(rc={res.returncode}, stdout={res.stdout[:200]!r}, "
              f"stderr={res.stderr[-400:]!r})")
    return int(lines[0].strip()), res


def ctl(trace_dir, request, timeout=5):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    try:
        client.connect(os.path.join(trace_dir, "pgwt.sock"))
        client.sendall((json.dumps(request) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        return json.loads(data.splitlines()[0])
    finally:
        client.close()


def start_daemon(pm_pid, trace_dir):
    err_path = os.path.join(trace_dir, "daemon.err")
    err = open(err_path, "w")
    env = os.environ.copy()
    env.pop("PGWT_TEST_EXEC_SITE_ADMIT_ALL", None)
    # Full mode is essential: tiered records the long ClientRead as SAMPLED.
    # Samples cannot be straddlers or satisfy execution_detail's exact-fidelity
    # requirement, so a tiered capture would prove nothing about #479.
    # #622: --skip-usdt is the EL8 proxy for unavailable phase markers.
    phase_args = (["--skip-usdt"]
                  if env.get("PGWT_TEST_FORCE_NO_PHASE_PROBES") == "1" else [])
    try:
        proc = subprocess.Popen(
            [TRACER, "--mode", "full", "--pid", str(pm_pid),
             "--trace-dir", trace_dir, "--duration", str(CAPTURE_DURATION_S),
             "--interval", "1", "--quiet"] + phase_args,
            stdout=subprocess.DEVNULL, stderr=err, env=env)
    except OSError:
        err.close()
        raise
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        if os.path.exists(os.path.join(trace_dir, "pgwt.sock")):
            try:
                if ctl(trace_dir, {"cmd": "status"}).get("ok") is True:
                    masks = [ctl(trace_dir, {"cmd": "status"}).get(
                        "exact_probe_attached_mask")]
                    return proc, err, masks
            except (OSError, ValueError, IndexError):
                pass
        time.sleep(0.1)
    stop_daemon(proc, err)
    with open(err_path) as fh:
        log = fh.read()[-1500:]
    fatal(f"daemon never became ready; stderr tail:\n{log}")


def stop_daemon(proc, err):
    try:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    finally:
        err.close()


def stop_psql(proc, pid):
    try:
        proc.stdin.write(b"END;\n\\q\n")
        proc.stdin.close()
    except (BrokenPipeError, OSError):
        pass
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    # Only OUR backend, as in test_client_wait.py; other idle sessions belong
    # to other tests/agents and must never be terminated by this one.
    if pid is not None:
        psql(f"SELECT pg_terminate_backend({pid}) FROM pg_stat_activity "
             f"WHERE pid = {pid} AND state = 'idle in transaction'")


def assert_spans(trace_dir, pid_a, t_before, t_after, b_parked_ns, b_release_ns,
                 expected_phase, masks):
    stats_path = os.path.join(trace_dir, "server.err")
    with ServerHarness(trace_dir, env={"PGWT_STRADDLE_STATS": "1"},
                       stderr_path=stats_path) as srv:
        info = srv.query("info")
        execs = srv.query("executions", limit=200, filters={"pid": pid_a})
        if "error" in info or "error" in execs:
            fatal(f"info/executions must answer without error: {info!r}, {execs!r}")
        frm, to = int(info.get("from_ns") or 0), int(info.get("to_ns") or 0)
        # info.from_ns is the first record's timestamp. Requiring it to
        # precede our first statement demands an unrelated backend's event
        # during settle: background noise on the box, not capture readiness.
        # start_daemon() establishes readiness through the control socket
        # before the batch runs. The executions' own timestamps below prove
        # which statements were measured.
        if not (0 < frm and to >= t_after):
            fatal(f"capture [{frm}, {to}] must have a positive first record "
                  f"timestamp and extend past A's batch end {t_after}")
        print(f"  capture [{frm}, {to}] extends past A's batch "
              f"[{t_before}, {t_after}]")

        unavailable = (execs.get("code") == "full_fidelity_required"
                       and execs.get("fidelity") in ("sampled", "none")
                       and "rows" not in execs)
        if not isinstance(execs.get("rows"), list) and not unavailable:
            fatal("executions did not return rows or an explicit exact-data refusal")
        rows = [row for row in execs.get("rows", [])
                if row.get("pid") == pid_a and row.get("end_ns") is not None
                and row.get("in_progress") is False
                and row.get("started_before_window") is False]
        print(f"  found {len(rows)} complete marker-identified executions for A "
              f"(pid={pid_a}); need at least {CLICK_N}")
        count = None if unavailable else len(rows)
        phase_contract_apply(expected_phase, masks, {"complete_executions": count},
                             "execution spans", fail=fatal)
        if len(rows) < CLICK_N:
            fatal(f"only {len(rows)} complete marker-identified executions; "
                  "a missing/mis-attached capture is not agreement")
        clicked = rows[:CLICK_N]
        for i, row in enumerate(clicked, 1):
            start, end = int(row["start_ns"]), int(row["end_ns"])
            if not (t_before <= start <= end <= t_after):
                fatal(f"click {i}: execution [{start}, {end}] lies outside "
                      f"A's batch [{t_before}, {t_after}]; "
                      f"not a statement issued by this script: {row!r}")
        details, admitted = [], []
        for i, row in enumerate(clicked, 1):
            offset = os.path.getsize(stats_path)
            detail = srv.query("execution_detail", pid=pid_a,
                               start_ns=row["start_ns"], end_ns=row["end_ns"])
            # Read only this request's appended diagnostics. Marker records
            # have duration_ns == 0 and are never admitted: any nonzero count
            # here can only come from the execution's window load.
            with open(stats_path, "rb") as fh:
                fh.seek(offset)
                appended = fh.read().decode("utf-8", errors="replace")
            if "error" in detail or detail.get("fidelity") != "exact":
                fatal(f"click {i}: execution_detail must answer without error "
                      f"and with exact fidelity (a different refusal): {detail!r}")
            reports = STRADDLE_REPORT.findall(appended)
            if not reports:
                fatal(f"click {i}: no straddler diagnostics appended for this "
                      f"request; cannot establish the #479 hazard: {appended!r}")
            admitted.append(sum(int(report[0]) for report in reports))
            details.append(detail)

        print(f"  per-request straddlers admitted: {admitted}")
        if sum(admitted) < 1:
            fatal("the capture did not contain a right-edge straddler, so this "
                  "gate could not have seen #479, and a green result here would "
                  f"mean nothing (per-request admitted: {admitted})")

        # b_parked_ns is AFTER observing ClientRead; b_release_ns is BEFORE
        # sending END. They bound the idle interval from the inside. An
        # observation timestamp AFTER END would not prove when the wait closed.
        check(b_parked_ns < t_before <= t_after <= b_release_ns,
              "B was parked before A began and released after A finished "
              f"({b_parked_ns} < {t_before} <= {t_after} <= {b_release_ns})")
        gaps_s = []
        for i, row in enumerate(clicked, 1):
            start, end = int(row["start_ns"]), int(row["end_ns"])
            gap_s = (b_release_ns - end) / 1e9
            gaps_s.append(gap_s)
            check(b_parked_ns < start <= end < b_release_ns,
                  f"click {i}: B's ClientRead spans execution [{start}, {end}]; "
                  f"its close is at least {gap_s:.6f}s past the right edge "
                  f"(parked={b_parked_ns}, release={b_release_ns})")
        print(f"  straddler close margin past clicked right edges: "
              f"min={min(gaps_s):.6f}s, max={max(gaps_s):.6f}s")

        present = 0
        for i, (row, detail) in enumerate(zip(clicked, details), 1):
            span = detail.get("backend_activity_span")
            if span is None:
                print(f"  click {i} backend_activity_unavailable_reason:")
                print(detail.get("backend_activity_unavailable_reason"))
            check(isinstance(span, dict),
                  f"click {i}: non-null backend_activity_span object")
            if not isinstance(span, dict):
                continue
            present += 1
            check(span.get("pid") == pid_a,
                  f"click {i}: containing span pid == A ({span.get('pid')})")
            check(span.get("measurement") == "measured",
                  f"click {i}: containing span measurement == measured "
                  f"({span.get('measurement')!r})")
            check(span.get("completion") == "closed",
                  f"click {i}: containing span completion == closed "
                  f"({span.get('completion')!r})")
            duration, execution_ms = span.get("completed_duration_ms"), row.get("duration_ms")
            check(isinstance(duration, (int, float))
                  and isinstance(execution_ms, (int, float))
                  and duration >= execution_ms,
                  f"click {i}: containing duration {duration}ms >= "
                  f"execution duration {execution_ms}ms")
            check(bool(detail.get("backend_activity_span_scope")),
                  f"click {i}: backend_activity_span_scope is present")
        print(f"  containing spans present: {present}/{CLICK_N}")


def run(pm_pid, expected_phase):
    trace_dir = tempfile.mkdtemp(prefix="pgwt_exec_span_")
    os.chmod(trace_dir, 0o755)
    proc = err = session_b = pid_b = None
    try:
        out = psql("SELECT pid FROM pg_stat_activity "
                   "WHERE state = 'idle in transaction'")
        pre_existing = {int(p) for p in out.split()}
        session_b = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        session_b.stdin.write(b"BEGIN;\n")
        session_b.stdin.flush()
        pid_b = wait_for_idle_in_tx(pre_existing, timeout=30)
        if pid_b is None:
            fatal("could not park session B idle in transaction in "
                  "Client:ClientRead within 30s; the hazard was not established")
        b_parked_ns = time.time_ns()
        print(f"  B pid={pid_b} parked in Client:ClientRead at {b_parked_ns}")

        proc, err, masks = start_daemon(pm_pid, trace_dir)
        time.sleep(CAPTURE_SETTLE_S)
        t_before = time.time_ns()
        pid_a, _ = psql_batch(["SELECT pg_sleep(0.02);"] * N, "session A")
        t_after = time.time_ns()
        # Hold to provoke the hazard past A's markers; this is no tolerance and nothing retries.
        time.sleep(HOLD_AFTER_A_S)
        b_release_ns = time.time_ns()
        session_b.stdin.write(b"END;\nSELECT 1;\n")
        session_b.stdin.flush()
        wait_for_end(pid_b, timeout=15)
        print(f"  B left idle in transaction; END sent after {b_release_ns}")
        time.sleep(CAPTURE_DRAIN_S)
        if proc.poll() is not None:
            fatal(f"daemon exited before capture drained (rc={proc.returncode})")
        try:
            masks.append(ctl(trace_dir, {"cmd": "status"}).get(
                "exact_probe_attached_mask"))
        except (OSError, ValueError, IndexError) as exc:
            print(f"  post-capture mask could not be read: {exc}")
        print("  mid-window mask samples: %r" % masks)
        stop_daemon(proc, err)
        proc = err = None
        stop_psql(session_b, pid_b)
        session_b = pid_b = None
        assert_spans(trace_dir, pid_a, t_before, t_after,
                     b_parked_ns, b_release_ns, expected_phase, masks)
    finally:
        try:
            if proc is not None:
                stop_daemon(proc, err)
        finally:
            try:
                if session_b is not None:
                    stop_psql(session_b, pid_b)
            finally:
                # Mirror the recorder rule in #421: a refusal keeps its evidence.
                if tests_failed == 0:
                    shutil.rmtree(trace_dir)
                else:
                    print(f"  trace retained for diagnosis: {trace_dir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--pg-version", type=int, help="accepted for runner compatibility; ignored")
    args = ap.parse_args()
    try:
        expected_phase = resolve_declared_phase_mask()
    except ValueError as exc:
        fatal(str(exc))
    print("test_execution_span_live: #479 containing spans with a real straddler")
    if os.geteuid() != 0:
        fatal("requires root (eBPF capture)")
    if sys.platform != "linux":
        fatal("requires Linux (eBPF capture)")
    for binary in (TRACER, SERVER_BIN):
        if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
            fatal(f"binary not built/executable: {binary}")
    try:
        pm_pid = args.pid or find_postmaster()
        if not pm_pid:
            fatal("no PostgreSQL postmaster found")
        print(f"  postmaster pid {pm_pid}, PGPORT={os.environ.get('PGPORT')}")
        run(pm_pid, expected_phase)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, TimeoutError) as exc:
        fatal(f"live test could not complete: {exc}")
    if tests_failed:
        print(f"test_execution_span_live: FAILED ({tests_failed} failures)")
        sys.exit(1)
    print("test_execution_span_live: PASSED")


if __name__ == "__main__":
    main()
