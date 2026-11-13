#!/usr/bin/env python3
"""Default-path INIT recovery, PG13+ (MyProc+offset and direct-pointer paths).

Two fixed backend-churn cohorts: discard INIT hints, then discard both hints
and stores. The ordinary address-based recovery is disabled in both arms so
it cannot hide a missing INIT sweep. Each connection stays alive across ticks.
Missing stimulus, metrics or attachment fails; no retries of a failed cohort.
Run only on Linux as root: tests/run_all.sh registers this in its live tier.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from testutil import find_postmaster

TRACER = Path(__file__).resolve().parents[1] / "pg_wait_tracer"


def ctl(directory):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(directory / "pgwt.sock"))
        client.sendall(b'{"cmd":"metrics"}\n')
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                raise RuntimeError("metrics connection closed without a reply")
            data += chunk
    metrics = json.loads(data.splitlines()[0])
    assert metrics.get("ok") is True, metrics
    for key in ("lifecycle_init_hint_drops_total", "lifecycle_init_lost_total",
                "ringbuf_lifecycle_fork_drops_total", "ringbuf_lifecycle_exit_drops_total",
                "state_map_full_total", "wp_attach_failures_total"):
        assert type(metrics.get(key)) is int and metrics[key] >= 0, (key, metrics)
    return metrics


def wait_for(proc, predicate, seconds, reason):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert proc.poll() is None, f"daemon exited: {proc.returncode}"
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(reason)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("daemon failed to stop within 15 seconds")


def cohort():
    # Three waves of eight NEW connections. A 3s wait lets an INIT stored
    # without a hint survive multiple 1s ticks before the backend exits.
    pids = set()
    for _ in range(3):
        sessions = []
        try:
            for _ in range(8):
                sessions.append(subprocess.Popen(
                    ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt",
                     "-v", "ON_ERROR_STOP=1", "-c",
                     "SELECT pg_backend_pid(); SELECT pg_sleep(3);"],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    env={**os.environ, "PGCONNECT_TIMEOUT": "10"}))
            for session in sessions:
                out, err = session.communicate(timeout=20)
                assert session.returncode == 0, (session.returncode, err)
                lines = out.strip().splitlines()
                assert lines and lines[0].isdigit(), (out, err)
                pid = int(lines[0])
                assert pid not in pids, f"cohort reused pid {pid}"
                pids.add(pid)
        finally:
            for session in sessions:
                if session.poll() is None:
                    session.kill()
                session.wait(timeout=5)
    assert len(pids) == 24, pids
    return pids


def run_arm(pm_pid, skip_store):
    with tempfile.TemporaryDirectory(prefix="pgwt_init_hint_") as tmp:
        directory = Path(tmp)
        log = directory / "daemon.stderr"
        env = dict(os.environ)
        env["PGWT_TEST_DROP_INIT_HINT"] = "1"
        env["PGWT_TEST_NO_RECOVERY"] = "1"
        env.pop("PGWT_TEST_SKIP_INIT_STORE", None)
        if skip_store:
            env["PGWT_TEST_SKIP_INIT_STORE"] = "1"
        with log.open("w") as err:
            proc = subprocess.Popen(
                [str(TRACER), "--mode", "full", "--pid", str(pm_pid),
                 "-T", str(directory), "-i", "1", "--duration", "90", "-q", "-v"],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            try:
                wait_for(proc, lambda: (directory / "pgwt.sock").exists(), 45,
                         "daemon control socket did not become ready")
                before = ctl(directory)
                pids = cohort()
                # Await the already-generated EXIT acknowledgements, not a
                # second stimulus or a re-capture after a failed assertion.
                def retired():
                    text = log.read_text()
                    return all(f"INFO: PID {pid} exited (" in text for pid in pids)
                wait_for(proc, retired, 10, "not every cohort EXIT was handled")
                after = ctl(directory)
                text = log.read_text()
                assert "PGWT_TEST_DROP_INIT_HINT" in text, text
                hint_delta = after["lifecycle_init_hint_drops_total"] - before["lifecycle_init_hint_drops_total"]
                lost_delta = after["lifecycle_init_lost_total"] - before["lifecycle_init_lost_total"]
                assert hint_delta > 0, "no bootstrap INIT hint was actually suppressed"
                for key in ("ringbuf_lifecycle_fork_drops_total", "ringbuf_lifecycle_exit_drops_total",
                            "state_map_full_total", "wp_attach_failures_total"):
                    assert after[key] == 0, (key, after[key])
                attached = {int(pid) for pid in re.findall(r"INFO: PID (\d+) initialized \(", text)}
                pending = {int(pid) for pid in re.findall(r"fork detected PID (\d+), bootstrap watchpoint attached", text)}
                assert pids & pending, "no cohort backend entered pending bootstrap"
                if skip_store:
                    assert "PGWT_TEST_SKIP_INIT_STORE" in text, text
                    assert lost_delta > 0, "skipping INIT stores did not raise the integrity loss counter"
                    assert pids - attached, "control attached everything despite discarding all INIT facts"
                else:
                    assert after["lifecycle_init_lost_total"] == 0, after
                    assert pids <= attached, f"backends not attached: {sorted(pids - attached)}"
                    # Every member attaches once, including any delayed hint.
                    for pid in pids:
                        assert text.count(f"INFO: PID {pid} initialized (") == 1, pid
                print(f"  {'store skipped' if skip_store else 'sweep recovery'}: "
                      f"{len(pids & attached)}/{len(pids)} attached, "
                      f"hint drops +{hint_delta}, INIT lost +{lost_delta}")
            except Exception:
                print(log.read_text())
                raise
            finally:
                stop(proc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    assert os.geteuid() == 0, "live test requires root"
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "PostgreSQL postmaster not found"
    run_arm(pm_pid, skip_store=False)
    run_arm(pm_pid, skip_store=True)
    print("test_init_hint_recovery: PASS")


if __name__ == "__main__":
    main()
