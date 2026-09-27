#!/usr/bin/env python3
"""test_daemon_server.py — Daemon trace files ≈ pgwt-server analysis.

Spec 0A.10: Run daemon with trace recording for 30s under pgbench.
Then run pgwt-server on the same trace files. Verify daemon CLI output
agrees with pgwt-server computed results.

Requires: root, running PostgreSQL 18, pgbench initialized.
Usage: sudo python3 tests/test_daemon_server.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import json
import tempfile
import shutil
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

TRACER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "pg_wait_tracer")
SERVER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "pgwt-server")
STRIP_ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def psql(sql, timeout=10):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def cleanup_stale_backends():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND backend_type = 'client backend' AND state != 'active'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            model[m.group(1).strip()] = float(m.group(2))
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%',
            line
        )
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
            })
    return events


def server_query(proc, cmd_dict):
    """Send a JSON command to pgwt-server stdin, read JSON response from stdout."""
    line = json.dumps(cmd_dict) + '\n'
    proc.stdin.write(line.encode())
    proc.stdin.flush()
    resp_line = proc.stdout.readline()
    if not resp_line:
        return None
    return json.loads(resp_line.decode('utf-8'))


def test_daemon_server(pm_pid):
    """Run daemon with --trace-dir, then verify pgwt-server agrees."""
    print("--- Test 1: Daemon CLI ≈ pgwt-server ---")

    trace_dir = tempfile.mkdtemp(prefix='pgwt_test_')

    CLIENTS = 4
    BENCH_SEC = 25
    INTERVAL = 20

    # Start pgbench before tracer
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    # Run daemon with --trace-dir to capture trace files AND CLI output
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 5),
         "--trace-dir", trace_dir,
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout_cli, stderr_cli = tracer.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout_cli, _ = tracer.communicate()

    pgbench.wait()

    cli_output = STRIP_ANSI.sub('', stdout_cli.decode('utf-8', errors='replace'))
    cli_model = parse_time_model(cli_output)

    # Verify trace files were created
    trace_files = [f for f in os.listdir(trace_dir) if f.endswith('.trace')]
    check(len(trace_files) > 0,
          f"Trace files created in {trace_dir} (found {len(trace_files)})")

    if not trace_files:
        shutil.rmtree(trace_dir, ignore_errors=True)
        return

    # Now run pgwt-server on the trace files
    srv = subprocess.Popen(
        [SERVER, trace_dir],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Query info to get time range
    info_resp = server_query(srv, {"id": 1, "cmd": "info"})
    check(info_resp is not None and 'error' not in info_resp,
          f"pgwt-server info command succeeds")

    if not info_resp or 'error' in info_resp:
        srv.kill()
        shutil.rmtree(trace_dir, ignore_errors=True)
        return

    from_ns = info_resp.get('from_ns', 0)
    to_ns = info_resp.get('to_ns', 0)

    check(to_ns > from_ns,
          f"Trace covers time range {from_ns} -> {to_ns}")

    # Query time_model from pgwt-server
    tm_resp = server_query(srv, {
        "id": 2, "cmd": "time_model",
        "from_ns": from_ns, "to_ns": to_ns
    })

    srv_db_time = 0
    srv_cpu_time = 0
    if tm_resp and 'rows' in tm_resp:
        for row in tm_resp['rows']:
            if row['name'] == 'DB Time':
                srv_db_time = row['ms']
            # #187: has_measured_cpu splits this row into "CPU (running)"
            # (real v3 cpu_ns, as a real live capture always has); the
            # legacy/no-split name "CPU*" is kept for completeness (some
            # captures may lack measured cpu_ns).
            elif row['name'] in ('CPU*', 'CPU (running)'):
                srv_cpu_time = row['ms']

    # Query top_events from pgwt-server
    te_resp = server_query(srv, {
        "id": 3, "cmd": "top_events",
        "from_ns": from_ns, "to_ns": to_ns
    })

    srv.stdin.close()
    srv.wait(timeout=5)

    # Compare CLI and server results
    cli_db_time = cli_model.get('DB Time', 0)
    cli_cpu_time = cli_model.get('CPU*', 0)

    check(cli_db_time > 0,
          f"CLI DB Time = {cli_db_time:.0f}ms (> 0)")
    check(srv_db_time > 0,
          f"Server DB Time = {srv_db_time:.0f}ms (> 0)")

    # The CLI snapshot and server use the same underlying events,
    # but CLI captures one interval while server reads the full trace.
    # They should be in the same ballpark.
    if cli_db_time > 0 and srv_db_time > 0:
        ratio = srv_db_time / cli_db_time
        check(0.3 <= ratio <= 3.0,
              f"DB Time ratio server/CLI = {ratio:.2f} "
              f"(server={srv_db_time:.0f}, CLI={cli_db_time:.0f})")

    if cli_cpu_time > 0 and srv_cpu_time > 0:
        ratio = srv_cpu_time / cli_cpu_time
        check(0.3 <= ratio <= 3.0,
              f"CPU Time ratio server/CLI = {ratio:.2f} "
              f"(server={srv_cpu_time:.0f}, CLI={cli_cpu_time:.0f})")

    # Server time_model should be internally consistent
    if tm_resp and 'rows' in tm_resp:
        WAIT_CLASSES = {'IO', 'LWLock', 'Lock', 'Client', 'IPC',
                        'BufferPin', 'Timeout', 'Extension'}
        wait_sum = sum(row['ms'] for row in tm_resp['rows']
                      if row['name'] in WAIT_CLASSES)
        # DB Time = CPU (running) + CPU (waiting for a core) + Σ wait classes
        # (S3/#187: the "waiting for a core" row is the measured runqueue/
        # unaccounted remainder of the on-CPU gaps, a first-class component
        # of DB Time — must be in the partition). Both the pre-#187 name
        # ("Off-CPU*") and the current one are accepted.
        off_cpu = sum(row['ms'] for row in tm_resp['rows']
                      if row['name'] in ('Off-CPU*', 'CPU (waiting for a core)'))
        reconstructed = srv_cpu_time + wait_sum + off_cpu
        if srv_db_time > 0:
            error_pct = abs(reconstructed - srv_db_time) / srv_db_time * 100
            check(error_pct < 1.0,
                  f"Server partition: CPU({srv_cpu_time:.0f}) + "
                  f"Waits({wait_sum:.0f}) = {reconstructed:.0f} vs "
                  f"DB Time {srv_db_time:.0f} (error {error_pct:.1f}%)")

    # Top events should have entries
    if te_resp and 'rows' in te_resp:
        check(len(te_resp['rows']) > 0,
              f"Server top_events has {len(te_resp['rows'])} events")

        # Server event counts should be positive
        total_count = sum(row['count'] for row in te_resp['rows'])
        check(total_count > 0,
              f"Server total event count = {total_count} (> 0)")

    # Cleanup
    shutil.rmtree(trace_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)

    if not os.path.exists(TRACER):
        print(f"ERROR: tracer binary not found at {TRACER}")
        sys.exit(1)

    if not os.path.exists(SERVER):
        print(f"ERROR: pgwt-server binary not found at {SERVER}")
        sys.exit(1)

    pm_pid = args.pid
    if not pm_pid:
        pm_pid = find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_daemon_server (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_daemon_server(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
