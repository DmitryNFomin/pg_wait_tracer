#!/usr/bin/env python3
"""test_percentage.py — Controlled 50/50 workload split (sleep + CPU).

Spec 0A.1: One backend does pg_sleep (Timeout), another does CPU burn.
Verify each accounts for ~50% of DB Time.

Requires: root, running PostgreSQL 18.
Usage: sudo python3 tests/test_percentage.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

TRACER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "pg_wait_tracer")
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
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
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
                'avg_us': float(m.group(4)),
                'max_us': float(m.group(5)),
                'pct': float(m.group(6)),
            })
    return events


def start_system_event_client(command, application_name):
    """Keep client stderr off a pipe so a chatty psql cannot block the workload."""
    stderr_file = tempfile.TemporaryFile()
    client = {'process': None, 'stderr_file': stderr_file, 'start_error': None,
              'backend_seen': False}
    try:
        client['process'] = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=stderr_file,
            env={**os.environ, 'PGAPPNAME': application_name})
    except OSError as exc:
        client['start_error'] = str(exc)
    return client


def finish_system_event_client(client):
    process = client['process']
    client['exit_before_cleanup'] = process.poll() if process else None
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    client['exit_code'] = process.returncode if process else None
    stderr_file = client['stderr_file']
    stderr_file.seek(0, os.SEEK_END)
    stderr_file.seek(max(0, stderr_file.tell() - 2048))
    client['stderr_tail'] = stderr_file.read().decode('utf-8', errors='replace')
    stderr_file.close()


def system_event_client_status(name, client):
    before = client['exit_before_cleanup']
    before_status = before if before is not None else (
        'running' if client['process'] else 'n/a')
    return (f"{name}: started={'yes' if client['process'] else 'no'}, "
            f"backend_seen={'yes' if client['backend_seen'] else 'no'}, "
            f"exit_before_cleanup={before_status}, "
            f"final_exit={client['exit_code']}, "
            f"start_error={client['start_error']!r}, "
            f"stderr_tail={client['stderr_tail']!r}")


def wait_for_system_event_sleeper(sleeper, cpu, sleep_app, cpu_app, deadline):
    """Check the sleeper up to the original capture start, without delaying it."""
    sql = ("SELECT application_name, pid, wait_event_type, wait_event "
           "FROM pg_stat_activity WHERE application_name IN "
           f"('{sleep_app}', '{cpu_app}')")
    last_observation = 'no matching backend'
    ready = False
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.1:
            if remaining > 0:
                time.sleep(remaining)
            break
        try:
            result = subprocess.run(
                ['psql', '-U', 'postgres', '-d', 'postgres', '-tAc', sql],
                capture_output=True, text=True, timeout=remaining)
            if result.returncode:
                ready = False
                last_observation = (f"pg_stat_activity query exit={result.returncode}, "
                                    f"stderr_tail={result.stderr[-512:]!r}")
            else:
                ready = False
                last_observation = result.stdout.strip() or 'no matching backend'
                for row in result.stdout.splitlines():
                    fields = row.split('|')
                    if len(fields) != 4:
                        continue
                    app, _pid, wait_type, wait_event = fields
                    if app == cpu_app:
                        cpu['backend_seen'] = True
                    if app == sleep_app:
                        sleeper['backend_seen'] = True
                        if wait_type == 'Timeout' and wait_event == 'PgSleep':
                            ready = True
        except (OSError, subprocess.TimeoutExpired) as exc:
            ready = False
            last_observation = f"pg_stat_activity query error: {exc!r}"
        time.sleep(min(0.1, max(0, deadline - time.monotonic())))

    # A missing precondition fails at the old one-second capture start. A
    # longer readiness timeout could move the capture past a failing window.
    return ready, last_observation


def finish_preliminary_tracer(probe_tracer):
    timed_out = False
    try:
        _, stderr = probe_tracer.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        timed_out = True
        probe_tracer.kill()
        _, stderr = probe_tracer.communicate()
    print(f"  preliminary system_event tracer: exit={probe_tracer.returncode}, "
          f"timed_out={timed_out}, "
          f"stderr_tail={stderr[-512:].decode('utf-8', errors='replace')!r}")


def test_percentage_split(pm_pid):
    """Backend A: pg_sleep(10) → Timeout:PgSleep.
    Backend B: CPU burn ~10s.
    Each should be ~50% of DB Time.

    Both start BEFORE the tracer so initial scan finds them already active.
    """
    print("--- Test 1: Controlled 50/50 split ---")

    DURATION = 10

    # Backend A: sleep
    sleep_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", f"SELECT pg_sleep({DURATION})",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    # Backend B: CPU burn (~10s of generate_series)
    cpu_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)  # let both backends start their work

    # Start tracer — initial scan finds both active
    INTERVAL = 8
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, _ = tracer.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Cleanup
    for p in [sleep_proc, cpu_proc]:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    model = parse_time_model(output)

    check('DB Time' in model,
          f"DB Time found (keys: {list(model.keys())})")
    if 'DB Time' not in model:
        return

    db_time = model['DB Time']
    cpu_time = model.get('CPU*', 0)
    timeout_time = model.get('Timeout', 0)

    check(db_time > 1000,
          f"DB Time = {db_time:.0f}ms (expected > 1000ms)")

    # Each component should be roughly half. ±15% tolerance.
    if db_time > 0:
        cpu_pct = cpu_time / db_time * 100
        timeout_pct = timeout_time / db_time * 100

        check(15 <= cpu_pct <= 85,
              f"CPU = {cpu_pct:.1f}% of DB Time (expected 20-80%)")

        check(15 <= timeout_pct <= 85,
              f"Timeout = {timeout_pct:.1f}% of DB Time (expected 20-80%)")

        # Together they should account for a significant portion of DB Time
        # (other classes like Extension, IO, Client may also contribute)
        combined = cpu_pct + timeout_pct
        check(combined > 50,
              f"CPU + Timeout = {combined:.1f}% (expected > 50% of DB Time)")

    # Also check system_event view
    probe_tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "1", "--duration", "1",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    # Rerun with workloads for system_event verification
    app_base = f"pgwt_percentage_{os.getpid()}_{time.monotonic_ns() % 1000000000}"
    sleep_app = f"{app_base}_sleep"
    cpu_app = f"{app_base}_cpu"
    sleep_client = start_system_event_client(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", f"SELECT pg_sleep({DURATION})",
         "-c", "SELECT pg_sleep(60)"],
        sleep_app)
    cpu_client = start_system_event_client(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        cpu_app)

    ready, observation = wait_for_system_event_sleeper(
        sleep_client, cpu_client, sleep_app, cpu_app,
        time.monotonic() + 1)

    if not ready:
        finish_system_event_client(sleep_client)
        finish_system_event_client(cpu_client)
        finish_preliminary_tracer(probe_tracer)
        diagnostics = (system_event_client_status('sleeper', sleep_client) + '; ' +
                       system_event_client_status('CPU', cpu_client))
        print("  system_event raw tracer output: not captured (sleeper not ready)")
        check(False, f"system_event: sleeper not ready in Timeout:PgSleep "
              f"within 1s (last pg_stat_activity={observation!r}; {diagnostics})")
        return

    try:
        tracer2 = subprocess.Popen(
            [TRACER, "--mode", "full", "--pid", str(pm_pid),
             "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
             "--view", "system_event"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
    except OSError as exc:
        finish_system_event_client(sleep_client)
        finish_system_event_client(cpu_client)
        finish_preliminary_tracer(probe_tracer)
        diagnostics = (system_event_client_status('sleeper', sleep_client) + '; ' +
                       system_event_client_status('CPU', cpu_client))
        print("  system_event raw tracer output: not captured (tracer did not start)")
        check(False, f"system_event: tracer failed to start: {exc}; {diagnostics}")
        return

    tracer_timed_out = False
    try:
        stdout2, tracer_stderr = tracer2.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer_timed_out = True
        tracer2.kill()
        stdout2, tracer_stderr = tracer2.communicate()

    print("  --- system_event raw tracer output begin ---")
    print(stdout2.decode('utf-8', errors='replace'), end='\n')
    print("  --- system_event raw tracer output end ---", flush=True)
    output2 = STRIP_ANSI.sub('', stdout2.decode('utf-8', errors='replace'))
    events = parse_system_events(output2)

    finish_system_event_client(sleep_client)
    finish_system_event_client(cpu_client)
    finish_preliminary_tracer(probe_tracer)
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass

    diagnostics = (f"readiness=Timeout:PgSleep observed "
                   f"(last pg_stat_activity={observation!r}); " +
                   system_event_client_status('sleeper', sleep_client) + '; ' +
                   system_event_client_status('CPU', cpu_client) + '; ' +
                   f"tracer: exit={tracer2.returncode}, timed_out={tracer_timed_out}, "
                   f"stderr_tail={tracer_stderr[-512:].decode('utf-8', errors='replace')!r}")

    pg_sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']
    cpu_ev = [e for e in events if e['name'] == 'CPU*']

    if pg_sleep_ev:
        sleep_ok = pg_sleep_ev[0]['total_ms'] > 2000
        check(sleep_ok,
              f"system_event: PgSleep total = {pg_sleep_ev[0]['total_ms']:.0f}ms "
              f"(> 2000ms){'' if sleep_ok else '; ' + diagnostics}")
    else:
        check(False, f"PgSleep not in system_event "
              f"(events: {[e['name'] for e in events]}); {diagnostics}")

    if cpu_ev:
        cpu_ok = cpu_ev[0]['total_ms'] > 2000
        check(cpu_ok,
              f"system_event: CPU total = {cpu_ev[0]['total_ms']:.0f}ms "
              f"(> 2000ms){'' if cpu_ok else '; ' + diagnostics}")
    else:
        check(False, f"CPU not in system_event "
              f"(events: {[e['name'] for e in events]}); {diagnostics}")


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

    pm_pid = args.pid
    if not pm_pid:
        pm_pid = find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_percentage (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_percentage_split(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
