#!/usr/bin/env python3
"""Linux daemon scheduling, refused privilege/quota, and kernel watchdog.

Uses /proc policies independently of reported state. Every arm must capture
new samples after scheduling setup/demotion, including a chrt FIFO launch.
Artifacts live outside the checkout. Registered in run_all.sh's live tier.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

from testutil import find_postmaster
from test_init_hint_recovery import wait_for

ROOT = Path(__file__).resolve().parents[1]
RESET = 0x40000000


def ctl(directory, command="metrics"):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(10)
        client.connect(str(directory / "pgwt.sock"))
        client.sendall((json.dumps({"cmd": command}) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            assert chunk, "daemon closed control reply"
            data += chunk
    result = json.loads(data.splitlines()[0])
    assert result.get("ok") is True, result
    return result


def policies(pid, expected):
    tasks = sorted(Path(f"/proc/{pid}/task").iterdir())
    assert len(tasks) >= 2, "thread isolation check requires a real daemon worker"
    observed = {}
    for task in tasks:
        tid = int(task.name)
        policy = os.sched_getscheduler(tid) & ~RESET
        priority = os.sched_getparam(tid).sched_priority
        observed[tid] = (policy, priority)
        assert (policy, priority) == ((os.SCHED_FIFO, 1) if
                tid == pid and expected == "active" else (os.SCHED_OTHER, 0)), observed
        status = (task / "status").read_text()
        blocked = int(re.search(r"^SigBlk:\s*([0-9a-f]+)", status, re.M)[1], 16)
        # Only main may receive SIGXCPU; a spinning FIFO main must handle it.
        if tid != pid or expected in ("active", "demoted"):
            assert bool(blocked & (1 << (signal.SIGXCPU - 1))) == (tid != pid), observed
    print(f"policies {expected}: {observed}", flush=True)


def run_arm(pm_pid, label, prefix=(), options=(), expected="active", spin=False):
    with tempfile.TemporaryDirectory(prefix="pgwt-rt-live-") as tmp:
        directory = Path(tmp)
        env = dict(os.environ)
        env.pop("PGWT_TEST_RT_SPIN", None)
        if spin:
            env["PGWT_TEST_RT_SPIN"] = "1"
        log = directory / "daemon.log"
        pidfile = directory / "daemon.pid"
        # The shell writes its PID before exec, including inside systemd-run.
        command = [*prefix, "sh", "-c", 'echo $$ > "$1"; shift; exec "$@"',
                   "rt-launch", str(pidfile), str(ROOT / "pg_wait_tracer"),
                   "--pid", str(pm_pid), "--mode", "sampled", "--sample-rate", "100",
                   "-T", str(directory), "-q", "-d", "90", *options]
        with log.open("w") as output:
            proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=output, env=env)
            try:
                wait_for(proc, lambda: (directory / "pgwt.sock").exists(), 45, "RT control not ready")
                pid = int(pidfile.read_text())
                # First reply requires the actual event loop to have started.
                status = ctl(directory, "status")
                metrics = ctl(directory)
                assert status["rt_priority"] == metrics["rt_priority"] == expected, (status, log.read_text())
                assert metrics["rt_priority_demotions_total"] == int(spin), metrics
                policies(pid, expected)
                before = metrics["samples_total"]
                sql = subprocess.run(["psql", "-X", "-U", "postgres", "-d", "postgres",
                                      "-qAt", "-v", "ON_ERROR_STOP=1", "-c", "SELECT pg_sleep(1)"],
                                     capture_output=True, text=True, timeout=15)
                assert sql.returncode == 0, sql.stderr
                after = ctl(directory)
                assert after["samples_total"] > before, (label, "capture did not continue", before, after)
                assert after["rt_priority"] == expected and after["rt_priority_demotions_total"] == int(spin), after
                if spin:
                    # Deliver again after demotion: no second warning or count.
                    os.kill(pid, signal.SIGXCPU)
                    assert ctl(directory)["rt_priority_demotions_total"] == 1
                print(f"live {label}: state={expected} samples={before}->{after['samples_total']} PASS", flush=True)
            finally:
                # systemd-run's outer process can differ from the daemon.
                if pidfile.exists():
                    try:
                        os.kill(int(pidfile.read_text()), signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                    raise AssertionError("RT daemon failed to shut down")
        text = log.read_text()
        assert proc.returncode == 0, (label, proc.returncode, text)
        assert text.count("WARN: rt_priority refused:") == int(expected == "refused"), text
        assert text.count("WARN: rt_priority demoted:") == int(spin), text
        if expected == "refused":
            assert "Operation not permitted" in text and "CAP_SYS_NICE" in text and \
                   "short-lived processes may be missed under load" in text, text
        if spin:
            assert "20000 us" in text and "PGWT_TEST_RT_SPIN" in text, text
            runtime = int(re.search(r"PGWT_TEST_RT_SPIN: demoted after cpu_us=(\d+)", text)[1])
            assert 0 < runtime < 2000000, ("kernel watchdog exceeded hard CPU budget", runtime)
            print(f"kernel spin: cpu_us={runtime} soft_us=20000 (kernel tick accounting)", flush=True)
        for line in text.splitlines():
            if "rt_priority" in line:
                print(line, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    pm_pid = args.pid or find_postmaster()
    assert pm_pid and os.geteuid() == 0, "live RT gate requires root and a postmaster"
    with tempfile.TemporaryDirectory(prefix="pgwt-rt-threads-") as tmp:
        probe = Path(tmp) / "threads"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "src"), str(ROOT / "tests/rt_priority_thread_probe.c"),
                        str(ROOT / "src/rt_priority.c"), str(ROOT / "src/rt_priority_linux.c"),
                        "-pthread", "-o", str(probe)], check=True)
        subprocess.run(["chrt", "-f", "7", str(probe)], check=True, timeout=10)
    run_arm(pm_pid, "default")
    run_arm(pm_pid, "inherited FIFO", prefix=("chrt", "-f", "7"))
    run_arm(pm_pid, "disabled under FIFO", prefix=("chrt", "-f", "7"),
            options=("--no-rt-priority",), expected="disabled")
    run_arm(pm_pid, "missing CAP_SYS_NICE", prefix=("setpriv", "--bounding-set=-sys_nice,-sys_resource"),
            expected="refused")
    run_arm(pm_pid, "kernel watchdog", options=("--rt-soft-us", "20000", "--rt-hard-us", "2000000"),
            expected="demoted", spin=True)
    os_release = Path("/etc/os-release").read_text()
    if re.search(r'^VERSION_ID="?8\.', os_release, re.M):
        run_arm(pm_pid, "EL8 CPUQuota refusal", prefix=("systemd-run", "--quiet", "--scope",
                "--unit=pgwt-989-quota-" + str(os.getpid()), "-p", "CPUQuota=50%"), expected="refused")


if __name__ == "__main__":
    main()
