#!/usr/bin/env python3
"""test_cpu_time.py — Verify CPU time dominance for compute-heavy queries.

Three tests:
  1. system_event: CPU event has highest total_ms for pure-compute query
  2. time_model:   CPU Time > sum of all Wait classes
  3. per-PID switching workload: ungated/gated exact vs schedstat field 1

Requires: root, running PostgreSQL, pg_wait_tracer and cross_validate built.
Usage: sudo python3 tests/test_cpu_time.py [--pid POSTMASTER_PID]
       --conservation-only [--conservation-output DIR] runs just test 3.
"""
import subprocess
import sys
import os
import re
import time
import argparse
import json
import platform
import signal
import tempfile
import copy
import socket
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster
from schedstat_probe import ProcReader
from schedstat_lib import compare_cpu_window

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


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'
            r'(\d+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)%',
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


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            name = m.group(1).strip()
            value = float(m.group(2))
            model[name] = value
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


# Waits by BACKGROUND processes that are NOT user query work: the checkpointer
# throttling between write batches (CheckpointWriteDelay), autovacuum's
# cost-based delay (VacuumDelay), and the pg_wait_sampling collector's
# background worker parked on its latch (Extension:Extension — seen for
# seconds on boxes where that extension is loaded, e.g. EL8). These are idle
# background sleeps that inflate system-wide DB Time; excluded from the "compute
# dominates" comparison so the assertion tracks real query activity, not which
# background worker happened to be waiting during the window.
BACKGROUND_WAITS = ('Timeout:CheckpointWriteDelay', 'Timeout:VacuumDelay',
                    'Extension:Extension')


def is_background_wait(name):
    """BACKGROUND_WAITS, plus any custom Extension id: pg_wait_events gives no
    id for custom extension events (#801), so the collector's wait renders
    as Extension:id=<n> rather than a name."""
    return name in BACKGROUND_WAITS or name.startswith('Extension:id=')


def cleanup():
    """Terminate leftover backends and quiet the box before measuring."""
    for pat in ('%generate_series%', '%pg_sleep%', '%LOOP%'):
        try:
            psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                 f"WHERE pid != pg_backend_pid() AND query LIKE '{pat}'")
        except (subprocess.TimeoutExpired, Exception):
            pass
    # Flush pending dirty pages now (waits for any in-progress checkpoint, then
    # runs an immediate one) so no spread checkpoint throttles during the trace
    # window. The compute query writes no WAL, so nothing re-triggers one.
    try:
        psql("CHECKPOINT", timeout=30)
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def test_cpu_system_event(pm_pid):
    """Run pure-compute query, verify CPU is dominant in system_event."""
    print("--- Test 1: CPU Dominance (system_event) ---")

    # Start the tracer FIRST, then fire the compute AFTER it has attached, so
    # the backend is caught by the fork tracepoint (reliable) rather than the
    # initial-scan straddle path (an intermittent connect/scan race for a
    # pure-compute client backend — see docs/ROADMAP_AND_STATUS.md; the straddle case
    # itself is covered by phase_cpu_straddle). The loop must outlast the whole
    # measured window so the backend is pinned on-CPU for every interval.
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(3)  # tracer scans + arms watchpoints before the backend forks

    # NESTED loop: a plpgsql integer FOR loop binds its counter to int4, so a
    # single `1..3000000000` bound raises "integer out of range" INSTANTLY
    # (3e9 > INT_MAX) — the loop never runs and the backend is pure-idle. The
    # nested form keeps both bounds well under INT_MAX while doing billions of
    # iterations (~minutes of CPU), so the backend stays pinned for the whole
    # window and is killed below. No pg_sleep tail: if the compute ever breaks
    # again, the trace shows ~0 CPU and this test FAILS loudly instead of
    # silently measuring the sleep.
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "DO $$ DECLARE x bigint := 0; BEGIN "
               "FOR i IN 1..100000 LOOP "
               "FOR j IN 1..100000 LOOP x := x + 1; END LOOP; "
               "END LOOP; END $$"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    cleanup()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    cpu_ev = [e for e in events if e['name'] == 'CPU*']

    check(len(cpu_ev) > 0,
          f"CPU event found (events: {[e['name'] for e in events]})")

    if cpu_ev:
        ev = cpu_ev[0]
        # The backend is pinned on-CPU for the whole window; the first report
        # covers ~5s of compute (fired at t=3, first interval ends at t=8), so
        # a correctly-measured CPU* is several thousand ms. >3000ms clears the
        # shared-box floor while still failing hard on the ~0 regression.
        check(ev['total_ms'] > 3000,
              f"CPU total = {ev['total_ms']:.1f}ms > 3000ms")

        # A pure-compute backend must make CPU the DOMINANT event among real
        # query activity — background maintenance sleeps (checkpointer,
        # autovacuum) are excluded (see BACKGROUND_WAITS) since they are idle
        # throttling, not work, and can run for seconds under suite load.
        competing = [e for e in events
                     if not e['name'].startswith('Activity:')
                     and not is_background_wait(e['name'])]
        if competing:
            top_ms = max(e['total_ms'] for e in competing)
            check(ev['total_ms'] >= top_ms * 0.8,
                  f"CPU {ev['total_ms']:.1f}ms is the dominant event "
                  f"(>=80% of top {top_ms:.1f}ms, excl. background maintenance)")


def test_cpu_time_model(pm_pid):
    """Run pure-compute query, verify CPU Time > Wait in time_model."""
    print("--- Test 2: CPU Dominance (time_model) ---")

    # Tracer first, then fire the compute after attach (fork tracepoint —
    # reliable; see test_cpu_system_event above). Nested int4-safe loop, no
    # pg_sleep tail — same rationale as test_cpu_system_event.
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(3)

    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "DO $$ DECLARE x bigint := 0; BEGIN "
               "FOR i IN 1..100000 LOOP "
               "FOR j IN 1..100000 LOOP x := x + 1; END LOOP; "
               "END LOOP; END $$"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    cleanup()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    model = parse_time_model(output)
    print(f"  time_model: {model}")

    cpu_ms = model.get('CPU*', 0)
    check(cpu_ms > 0,
          f"CPU = {cpu_ms:.1f}ms > 0")

    # The backend is pinned on-CPU for the whole window (fork-after-attach); the
    # first report covers ~5s of compute, so a correctly-measured CPU* is
    # several thousand ms. >3000ms fails hard on the ~0 regression (the class
    # this guards: a fork-caught compute backend whose ongoing on-CPU interval
    # was suppressed in the live view — the has_closed_data bug, fixed in
    # map_reader.c) while clearing the shared-box floor.
    check(cpu_ms > 3000,
          f"CPU {cpu_ms:.1f}ms > 3000ms (compute-heavy query captured)")

    # CPU must DOMINATE real DB Time: the only sustained query work on the box
    # is this pure-compute backend. Background maintenance sleeps (checkpointer,
    # autovacuum — see BACKGROUND_WAITS) are subtracted from the denominator
    # since they are idle throttling, not work; under suite load they can add
    # seconds of DB Time. Off-CPU* and minor IO still dilute, so require >40%.
    db_time = model.get('DB Time', 0)
    background = sum(v for w, v in model.items() if is_background_wait(w))
    real_db = db_time - background
    if real_db > 0:
        cpu_pct = cpu_ms / real_db * 100
        check(cpu_pct > 40,
              f"CPU is {cpu_pct:.1f}% of real DB Time "
              f"(DB {db_time:.0f}ms - background {background:.0f}ms; "
              f"compute dominates)")


def test_cpu_conservation(pm_pid, output_dir=None, windows=3, window_s=100.0):
    """#675: eight short-command pgbench clients, matching #671's load.

    Three measured windows total 300s. Stopped boundaries retain exact CPU
    since the last trace transition, so offline gated CPU needs no wall-time
    interpolation. Boundary pauses are included in the actual covered span.
    """
    print("--- Test 3: switching-workload per-PID CPU diagnostic ---", flush=True)
    cross_validate = Path(__file__).resolve().parent / "cross_validate"
    reader = ProcReader()
    clients, pids = [], []
    tracer = None
    snapshots, results = [], []
    output = Path(output_dir) if output_dir else Path(tempfile.mkdtemp(prefix="pgwt-675-"))
    output.mkdir(parents=True, exist_ok=True)
    metadata = {"kernel": platform.release(), "postmaster_pid": pm_pid,
                "workload": "pgbench default read/write, -n -c 8 -j 2; existing populated schema",
                "clients": 8, "sample_rate_hz": 10, "mode": "tiered + manual exact escalation",
                "nominal_window_s": window_s, "windows": windows,
                "boundary": "owned backends SIGSTOP; closed cpu_ns_total; schedstat field 1",
                "limit": "max(1 ms, 1% of field-1 delta); CANNOT detect a 0.9 pp deficit",
                "sched_schedstats": reader.schedstats_enabled()[0],
                "utc_start": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    previous_sigterm = signal.getsignal(signal.SIGTERM)

    def terminated(signum, frame):
        raise SystemExit(128 + signum)

    # A harness timeout must unwind the finally block and resume owned tasks.
    signal.signal(signal.SIGTERM, terminated)

    def stop_owned(owned_pids):
        for pid in owned_pids:
            os.kill(pid, signal.SIGSTOP)
        # Synchronization deadline, never a second measurement attempt. Do
        # not read either counter until every owned task has actually stopped.
        deadline = time.monotonic() + 5
        while any(reader.task_identity(pid)["state"] != "T" for pid in owned_pids):
            if time.monotonic() >= deadline:
                raise RuntimeError("owned backend did not stop within 5s")
            time.sleep(0.001)

    def boundary():
        stop_owned(pids)
        raw = subprocess.run([str(cross_validate), "--conservation-snapshot",
                              str(tracer.pid), *map(str, pids)],
                             check=True, capture_output=True, text=True, timeout=5)
        exact = json.loads(raw.stdout)
        sample = {"map_id": exact["map_id"], "per_pid": {}}
        for pid in pids:
            ident = reader.task_identity(pid)
            if ident["ppid"] != pm_pid or ident["state"] != "T":
                raise RuntimeError(f"pid={pid} no longer a stopped child of {pm_pid}")
            runtime, reason = reader.cpu_runtime(pid)
            if runtime is None:
                raise RuntimeError(reason)
            sample["per_pid"][pid] = {
                "runtime_ns": runtime, **exact["per_pid"][str(pid)],
                **reader.context_switches(pid),
                "starttime_ticks": ident["starttime_ticks"],
                "t_ns": time.monotonic_ns(), "wall_ns": time.time_ns()}
        snapshots.append(sample)
        return sample

    try:
        preload = subprocess.run(
            ["psql", "-X", "-U", "postgres", "-d", "postgres", "-tAc",
             "SHOW shared_preload_libraries"],
            check=True, capture_output=True, text=True, timeout=10)
        metadata["shared_preload_libraries"] = preload.stdout.strip()
        print("CONSERVATION_METADATA " + json.dumps(metadata), flush=True)
        with (output / "daemon.stderr").open("w") as log:
            tracer = subprocess.Popen([TRACER, "--daemon", "--mode", "tiered", "--sample-rate", "10",
                                       "--escalation-budget", "900", "--interval", "1", "--pid", str(pm_pid),
                                       "--quiet", "--trace-dir", str(output / "trace")],
                                      stdout=subprocess.DEVNULL, stderr=log)
            # Attach before fork, as in the existing CPU tests. An unready
            # daemon is a failure below, never an attach/retry loop.
            time.sleep(3)
            if tracer.poll() is not None:
                raise RuntimeError(f"tracer exited {tracer.returncode}; see {output}/daemon.stderr")
            metadata["daemon_scheduler"] = os.sched_getscheduler(tracer.pid)
            metadata["daemon_priority"] = os.sched_getparam(tracer.pid).sched_priority
            rows = int(psql("SELECT count(*) FROM pgbench_accounts"))
            if rows < 1:
                raise RuntimeError("pgbench_accounts missing/empty; seed explicitly before this test")
            metadata["pgbench_accounts_rows"] = rows
            appname = f"pgwt_675_{os.getpid()}"
            with (output / "pgbench.log").open("w") as benchlog:
                client = subprocess.Popen(
                    ["pgbench", "-n", "-U", "postgres", "-d", "postgres", "-c", "8", "-j", "2",
                     "-T", str(int(windows * window_s + 120))],
                    env=dict(os.environ, PGAPPNAME=appname), stdout=benchlog, stderr=benchlog)
            clients.append(client)
            time.sleep(8)  # #671's fixed warmup; no workload retry
            if client.poll() is not None:
                raise RuntimeError("pgbench exited during warmup; see pgbench.log")
            pids = [int(p) for p in psql(
                "SELECT pid FROM pg_stat_activity WHERE backend_type='client backend' "
                f"AND application_name='{appname}' ORDER BY pid").split()]
            if len(pids) != 8 or any(reader.task_identity(p)["ppid"] != pm_pid for p in pids):
                raise RuntimeError(f"expected eight owned backends on postmaster {pm_pid}: {pids}")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as ctl:
                ctl.settimeout(10)
                ctl.connect(str(output / "trace" / "pgwt.sock"))
                ctl.sendall((json.dumps({"cmd": "escalate", "duration_s": int(windows * window_s + 90),
                                        "reason": "675 round 2"}) + "\n").encode())
                reply = b""
                while b"\n" not in reply:
                    part = ctl.recv(65536)
                    if not part:
                        raise RuntimeError("escalation control socket closed without reply")
                    reply += part
            metadata["escalation"] = json.loads(reply.split(b"\n")[0])
            print("CONSERVATION_ESCALATION " + reply.decode().strip(), flush=True)
            if not metadata["escalation"].get("ok"):
                raise RuntimeError(f"exact escalation refused: {metadata['escalation']}")
            time.sleep(1)  # exclude initial attach/seed before baseline
            before = boundary()
            for w in range(windows):
                for pid in pids:
                    os.kill(pid, signal.SIGCONT)
                time.sleep(window_s)
                after = boundary()
                result = compare_cpu_window(before, after, pids)
                result["window"] = w
                results.append(result)
                for row in result["rows"]:
                    print(f"CONSERVATION_ROW window={w} " + json.dumps(row), flush=True)
                # Report ratios. The retained 1% verdict cannot resolve a
                # 0.9 pp deficit; never advertise this experiment as PASS.
                if not result["rows"] or result["reason"]:
                    check(False, f"conservation refused window={w}: {result['reason']}")
                elif not result["ok"]:
                    check(False, f"CPU discrepancy exceeds fixed bound window={w}")
                # Real-state negative control: retain measured counters but
                # request psql instead of its PostgreSQL backend. The
                # comparator must reject this population, never return zero.
                if w == 0:
                    wrong = [clients[0].pid, *pids[1:]]
                    control = compare_cpu_window(before, after, wrong)
                    print(f"CONSERVATION_CONTROL wrong-pid-set RESULT: "
                          f"{'PASS' if control['ok'] else 'FAIL'} reason={control['reason']}", flush=True)
                    check(not control["ok"] and control["reason"] == "pid set mismatch",
                          "real-state wrong-pid-set control fires")
                    # Instrument control against REAL endpoints: suppress the
                    # entire exact increment, leaving G and identity intact.
                    # A fractional removal can accidentally cancel an existing
                    # over-read; this control must not depend on a clean result.
                    # This changes a saved observation, never a live BPF map.
                    lost = copy.deepcopy(after)
                    for pid in pids:
                        lost["per_pid"][pid]["exact_ns"] = before["per_pid"][pid]["exact_ns"]
                    loss = compare_cpu_window(before, lost, pids)
                    print("CONSERVATION_CONTROL lost-exact-cpu " + json.dumps(loss), flush=True)
                    check(not loss["ok"] and loss["reason"] is None and
                          any(not r["ok"] for r in loss["rows"]),
                          "real-endpoint lost-exact-CPU control detects a shortfall")
                    metadata["loss_control"] = loss
                    metadata["control"] = dict(control, expected_pids=wrong,
                                               measured_pids=list(pids))
                    # Also hit the live BPF lookup, not just offline set
                    # validation. psql is ours and stopped but is not in the
                    # daemon's backend map. The helper must return nonzero.
                    try:
                        stop_owned([clients[0].pid])
                        bad = subprocess.run(
                            [str(cross_validate), "--conservation-snapshot",
                             str(tracer.pid), *map(str, wrong)],
                            capture_output=True, text=True, timeout=5)
                    finally:
                        os.kill(clients[0].pid, signal.SIGCONT)
                    print(f"CONSERVATION_CONTROL live-wrong-pid exit={bad.returncode} "
                          f"{bad.stderr.strip()}", flush=True)
                    check(bad.returncode == 6 and not bad.stdout and
                          f"pid={clients[0].pid}" in bad.stderr,
                          "live stopped non-backend PID is rejected by the BPF reader")
                    metadata["live_control"] = {"pid": clients[0].pid,
                                                "exit": bad.returncode,
                                                "stderr": bad.stderr.strip()}
                before = after
            metadata["pids"] = pids
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        detail = str(exc)
        if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
            detail += ": " + exc.stderr.strip()
        check(False, f"conservation unavailable: {detail}")
        metadata["error"] = detail
    finally:
        # Resume before cancellation so stopped PostgreSQL children cannot be
        # stranded by either a failed assertion or an interrupted experiment.
        for pid in pids:
            try:
                os.kill(pid, signal.SIGCONT)
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for client in clients:
            client.terminate()
            try:
                client.wait(timeout=5)
            except subprocess.TimeoutExpired:
                client.kill()
                client.wait()
        if tracer is not None:
            tracer.terminate()
            try:
                tracer.wait(timeout=10)
            except subprocess.TimeoutExpired:
                tracer.kill()
                tracer.wait()
        if len(snapshots) == windows + 1 and len(results) == windows:
            try:
                spec = output / "trace-windows.txt"
                with spec.open("w") as f:
                    for w in range(windows):
                        for pid in pids:
                            a, b = (snapshots[j]["per_pid"][pid] for j in (w, w + 1))
                            f.write(f"{w} {pid} {a['t_ns']} {b['t_ns']} "
                                    f"{a['last_ts']} {a['exact_ns'] - a['last_cpu_ns']} "
                                    f"{b['last_ts']} {b['exact_ns'] - b['last_cpu_ns']}\n")
                raw = subprocess.run([str(cross_validate), "--conservation-trace",
                                      str(output / "trace"), str(spec)],
                                     check=True, capture_output=True, text=True, timeout=120)
                (output / "gated.jsonl").write_text(raw.stdout)
                gated = [json.loads(line) for line in raw.stdout.splitlines()]
                keyed = {(r["window"], r["pid"]): r for r in gated}
                if len(keyed) != windows * len(pids):
                    raise RuntimeError("trace output population mismatch")
                for result in results:
                    for row in result["rows"]:
                        trace = keyed[(result["window"], row["pid"])]
                        row.update(trace)
                        row["gated_ratio"] = trace["gated_cpu_ns"] / row["runtime_ns"]
                        row["comparator_ratio"] = trace["comparator_cpu_wall_ns"] / row["runtime_ns"]
                        row["daemon_ratio"] = trace["daemon_cpu_ns"] / row["runtime_ns"]
                        row["daemon_gated_ratio"] = trace["daemon_gated_cpu_ns"] / row["runtime_ns"]
                        row["trace_minus_map_ns"] = trace["trace_cpu_ns"] - row["exact_ns"]
                        print("CONSERVATION_THREE " + json.dumps(row), flush=True)
                metadata["gated_definition"] = "measured cpu_ns, command gate rejects out-of-command we==0; retains CPU during waits"
                metadata["comparator_definition"] = "we==0 wall ns accepted by existing command-majority sweep (#671 RAW_CPU_EXACT_NS)"
                metadata["daemon_definition"] = "per-PID reconstruction of event_stream.c cpu_ns_total: we==0 CPU, zero delta falls back to wall time; wait CPU counted separately"
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                check(False, f"gated conservation unavailable: {exc}")
                metadata["gated_error"] = str(exc)
                if isinstance(exc, subprocess.CalledProcessError):
                    metadata["gated_stderr"] = exc.stderr
        (output / "conservation.json").write_text(json.dumps(
            {"metadata": metadata, "snapshots": snapshots, "windows": results}, indent=2) + "\n")
        print(f"CONSERVATION_ARTIFACT {output}/conservation.json", flush=True)
        signal.signal(signal.SIGTERM, previous_sigterm)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    parser.add_argument('--conservation-only', action='store_true',
                        help='run only the switching-workload three-quantity diagnostic')
    parser.add_argument('--conservation-output', help='raw snapshot/daemon/trace directory')
    parser.add_argument('--conservation-window-s', type=float, default=100.0,
                        help='seconds per sub-window (three windows; default total 300s)')
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

    print(f"=== test_cpu_time (postmaster PID {pm_pid}) ===")

    if not args.conservation_only:
        cleanup()
        test_cpu_system_event(pm_pid)
        test_cpu_time_model(pm_pid)
    if args.conservation_window_s <= 0 or args.conservation_window_s > 200:
        parser.error('--conservation-window-s must be in (0, 200]')
    test_cpu_conservation(pm_pid, args.conservation_output, window_s=args.conservation_window_s)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
