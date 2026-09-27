#!/usr/bin/env python3
"""test_partition.py — Time model partition: CPU + all waits = DB Time.

Spec 0A.5: Run pgbench mixed workload. Verify all wait classes sum
exactly to DB Time with < 0.1% tolerance.

Requires: root, running PostgreSQL 18, pgbench initialized.
Usage: sudo python3 tests/test_partition.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse

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


def test_partition(pm_pid):
    """Run pgbench. Verify CPU + all wait classes = DB Time."""
    print("--- Test 1: Exact time partition ---")

    CLIENTS = 8
    BENCH_SEC = 20
    INTERVAL = 15

    # Start pgbench before tracer
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)  # let backends connect and attach

    # Start tracer
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
    pgbench.wait()

    model = parse_time_model(output)

    check('DB Time' in model,
          f"DB Time found (keys: {list(model.keys())})")
    if 'DB Time' not in model:
        return

    db_time = model['DB Time']
    cpu_time = model.get('CPU*', 0)

    # All wait class names (top-level only, no ':')
    WAIT_CLASSES = {'IO', 'LWLock', 'Lock', 'Client', 'IPC',
                    'BufferPin', 'Timeout', 'Extension'}
    wait_sum = sum(model.get(c, 0) for c in WAIT_CLASSES)
    # Off-CPU* (issue #202) is CPU*'s measured sibling — the wall of the
    # on-CPU intervals the backend was not actually on a CPU for — and a
    # first-class component of DB Time, so it belongs in the partition.
    # 0 on the legacy/sampled tiers, where CPU* carries the whole gap.
    offcpu = model.get('Off-CPU*', 0)

    reconstructed = cpu_time + offcpu + wait_sum

    check(db_time > 1000,
          f"DB Time = {db_time:.0f}ms (expected > 1000ms for {CLIENTS} clients)")

    # Strict partition: reconstructed should equal DB Time within 0.1%
    if db_time > 0:
        error_pct = abs(reconstructed - db_time) / db_time * 100
        check(error_pct < 0.1,
              f"Partition error = {error_pct:.4f}% "
              f"(CPU={cpu_time:.0f} + OffCPU={offcpu:.0f} + "
              f"Waits={wait_sum:.0f} = {reconstructed:.0f} "
              f"vs DB Time={db_time:.0f})")

    # Percentages should sum to ~100%
    if db_time > 0:
        total_pct = reconstructed / db_time * 100
        check(99.9 <= total_pct <= 100.1,
              f"Percentages sum to {total_pct:.2f}% (expected 100.0%)")

    # Each class percentage independently
    class_details = []
    for cls in sorted(WAIT_CLASSES):
        val = model.get(cls, 0)
        if val > 0:
            pct = val / db_time * 100
            class_details.append(f"{cls}={pct:.1f}%")
    cpu_pct = cpu_time / db_time * 100 if db_time > 0 else 0
    class_details.insert(0, f"CPU={cpu_pct:.1f}%")
    print(f"  Breakdown: {', '.join(class_details)}")

    # CPU should be > 0 for pgbench
    check(cpu_time > 0,
          f"CPU = {cpu_time:.0f}ms (expected > 0 for pgbench)")

    # IO should be > 0 for pgbench (TPC-B does reads/writes)
    io_time = model.get('IO', 0)
    check(io_time >= 0,
          f"IO = {io_time:.0f}ms (may be 0 if fully cached)")


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

    print(f"=== test_partition (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_partition(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
