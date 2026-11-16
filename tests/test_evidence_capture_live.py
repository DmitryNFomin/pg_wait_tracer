#!/usr/bin/env python3
"""Linux/root pilot B acceptance; run on Ubuntu and EL8 after A+B are joined.

The opt-in arm lasts 30 seconds. A new, tagged backend sleeps and exits; the
production codec/replay test binary inspects its file, and the pilot server
route must return the documented refusals. Other arms exercise opt-out, the
size bound and actual publication failure, while reading each arm's v4 bytes.
All captures/logs live in a temporary directory, outside the repository.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from server_harness import ServerHarness
from test_init_hint_recovery import stop, wait_for
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / "pg_wait_tracer"
INSPECTOR = ROOT / "tests/test_evidence_capture"
PSQL = ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt",
        "-v", "ON_ERROR_STOP=1"]


def sql(statement):
    return subprocess.check_output([*PSQL, "-c", statement], text=True, timeout=10).strip()


def checkpointer(pm_pid):
    children = subprocess.check_output(
        ["ps", "-o", "pid=,args=", "--ppid", str(pm_pid)], text=True)
    pids = [int(line.split()[0]) for line in children.splitlines()
            if re.search(r"postgres:.*\bcheckpointer\b", line)]
    assert len(pids) == 1, ("expected one pre-existing checkpointer", children)
    return pids[0]


def sleeping_backend(label, daemon, env):
    app = "pgwt_evidence_capture_" + label
    load = subprocess.Popen(
        [*PSQL, "-c", "SELECT pg_backend_pid(); SELECT pg_sleep(4);"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**env, "PGAPPNAME": app, "PGCONNECT_TIMEOUT": "10"})
    try:
        statement = ("SELECT pid FROM pg_stat_activity WHERE application_name = '" + app +
                     "' AND state = 'active' AND wait_event = 'PgSleep'")
        deadline = time.monotonic() + 10
        pid = ""
        while time.monotonic() < deadline:
            assert daemon.poll() is None, ("daemon exited during stimulus", daemon.returncode)
            pid = sql(statement)
            if pid.isdigit():
                break
            assert load.poll() is None, "sleeping backend exited before observation"
            time.sleep(0.05)
        assert pid.isdigit(), "sleeping backend not observed"
        # The target executes just one sleep. These two server observations
        # bracket a monotonic query window entirely inside that sleep.
        from_ns = time.monotonic_ns()
        time.sleep(0.01)
        to_ns = time.monotonic_ns()
        assert sql(statement) == pid, "backend left sleep before window was bracketed"
        out, err = load.communicate(timeout=15)
        assert load.returncode == 0 and out.splitlines()[0] == pid, (out, err)
        return int(pid), from_ns, to_ns
    finally:
        if load.poll() is None:
            load.kill()
            load.communicate(timeout=5)


def metric_refusal(response, expected):
    assert response.get("pilot") is True and "error" not in response, response
    for name in ("wait_time", "completed_waits"):
        result = response[name]
        assert result["value"] is None and result["completeness"] == "unavailable", response
        assert result["reasons"] == [expected], response
    storage = response["storage"]
    assert storage["codec_failed"] is False and storage["codec_stops"] == [], response
    assert storage["replay_stop"] is None and storage["capture_ambiguous"] is False, response


def v4_readable(directory, pid):
    path = directory / "current.trace"
    header = path.read_bytes()[:6]
    assert header[:4] == b"PGWT" and int.from_bytes(header[4:6], "little") == 4, path
    with ServerHarness(str(directory)) as server:
        response = server.query("top_events", filters={"pid": pid}, timeout=10)
        assert "error" not in response and response.get("rows"), response
        assert any("PgSleep" in row.get("name", "") for row in response["rows"]), response
    print("same-run v4 header/reader/PgSleep: PASS", flush=True)


def run_arm(pm_pid, unknown_pid, label):
    duration = 30 if label == "optin" else 8
    with tempfile.TemporaryDirectory(prefix="pgwt-evidence-live-") as temporary:
        base = Path(temporary)
        directory = base / "trace"  # The legacy writer must create it first.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("PGWT_EVIDENCE_CAPTURE", "PGWT_TEST_"))}
        if label != "optout":
            env["PGWT_EVIDENCE_CAPTURE"] = "1"
        if label == "tiny":
            env["PGWT_EVIDENCE_CAPTURE_MAX_BYTES"] = "1"
        if label == "unwritable":
            # Root cannot create arbitrary files in procfs. Only evidence uses
            # this path; never rename or redirect the live v4 directory/meta.
            env["PGWT_EVIDENCE_CAPTURE_DIR"] = "/proc/self"
        log = base / "daemon.stderr"
        command = [str(TRACER), "--mode", "full", "--pid", str(pm_pid),
                   "-T", str(directory), "-i", "1", "--duration", str(duration), "-q"]
        print(f"capture live {label}: {' '.join(command)}", flush=True)
        with log.open("w") as err:
            daemon = subprocess.Popen(command, stderr=err, stdout=subprocess.DEVNULL, env=env)
            try:
                wait_for(daemon, lambda: (directory / "pgwt.sock").exists(), 45,
                         "capture live control socket not ready")
                pid, from_ns, to_ns = sleeping_backend(label, daemon, env)
                daemon.wait(timeout=duration + 15)
                assert daemon.returncode == 0, (daemon.returncode, log.read_text())
            finally:
                stop(daemon)
        text = log.read_text()
        evidence = sorted(directory.glob("evidence-*.trace"))
        assert not list(directory.glob(".evidence-*")), "partial temporary capture leaked"
        if label == "optin":
            assert "EVIDENCE CAPTURE REFUSED" not in text, text
            assert len(evidence) == 1, (evidence, text)
            inspected = subprocess.check_output(
                [str(INSPECTOR), "--inspect", str(evidence[0]), str(pid), str(unknown_pid)],
                text=True, timeout=15)
            report = json.loads(inspected)
            assert int(report["fork"]) <= from_ns < to_ns <= int(report["exit"]), report
            print("capture live decoded: " + inspected.strip(), flush=True)
            query = {"event": "PgSleep", "from": str(from_ns), "to": str(to_ns)}
            with ServerHarness(str(directory)) as server:
                for target, expected in (
                    ({"capture": report["capture"], "pid": pid, "fork": report["fork"]},
                     "coverage_incomplete"),
                    ({"capture": report["capture"], "pid": unknown_pid, "fork": None},
                     "evidence_invalid"),
                ):
                    response = server.query("pilot_wait_metrics", target=target, query=query, timeout=15)
                    metric_refusal(response, expected)
                    print("capture live route: " + json.dumps(response, sort_keys=True), flush=True)
        else:
            assert evidence == [], ("unexpected v5 publication", evidence)
            if label == "optout":
                assert "EVIDENCE CAPTURE REFUSED" not in text, text
            else:
                reason = "size cap reached" if label == "tiny" else "evidence output directory unwritable"
                assert "EVIDENCE CAPTURE REFUSED: " + reason in text, text
                print("capture live explicit refusal: " + reason, flush=True)
        v4_readable(directory, pid)
        print(f"capture live {label}: PASS", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    assert sys.platform.startswith("linux") and os.geteuid() == 0, "Linux/root live test only"
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "no postmaster for capture live test"
    assert INSPECTOR.is_file(), "build tests/test_evidence_capture before live tier"
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip() or "unknown (no .git: box-check syncs the tree without it)"
    print(f"capture live commit={commit}", flush=True)
    unknown = checkpointer(pm_pid)
    for label in ("optout", "optin", "tiny", "unwritable"):
        run_arm(pm_pid, unknown, label)
    print("test_evidence_capture_live: PASS (4 arms, v4 capture readable in each)", flush=True)


if __name__ == "__main__":
    main()
