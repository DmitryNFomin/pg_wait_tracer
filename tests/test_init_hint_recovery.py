#!/usr/bin/env python3
"""Default-path INIT recovery, PG13+ (MyProc+offset and direct-pointer paths).

Three cohorts: long-lived backends with discarded hints, 20s of 16 concurrent
short-connection loops with discarded hints and production tick recovery ON,
then a lost control discarding both hints and stores. Only the long-lived arm
and lost control disable address-based tick recovery to isolate stored INITs.
Missing stimulus, metrics or attachment fails; no retries of a failed cohort.
Owner decision #871 comment 6863203475 excludes counted no-opportunity losses
from integrity; every other loss must be zero in recovery arms. The store-skipped
control must instead produce a with-opportunity loss and fail the integrity gate.
Run only on Linux as root: tests/run_all.sh registers this in its live tier.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter, deque
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

from testutil import find_postmaster
from demo_rehearsal_lib import daemon_integrity_ok

TRACER = Path(__file__).resolve().parents[1] / "pg_wait_tracer"
TICK_SECONDS = 1
ATTACH_COUNTERS = ("wp_attach_failures_total", "wp_attach_exit_race_total",
                   "wp_attach_failures_live_total", "wp_attach_failures_unknown_total",
                   "wp_attach_failures_gone_total")
EXIT_BEFORE_ATTACH = "lifecycle_exit_before_attach_total"
EXIT_CLASSES = {key: f"lifecycle_exit_before_attach_{key}_total" for key in
                ("with_opportunity", "no_opportunity", "unknown")}
DIAGNOSTIC_LINES = 40
DIAGNOSTIC_WIDTH = 512
# Temporary #933 exception; remove with the A2 PR.
# max observed 1 per run in 20 runs on master bc554a73 (Ubuntu 6.8 PG18,
# tests/results/session-20261117/i933-churn-cap); cap 2 = observed max + 1;
# regression tripwire, not a rate bound.
KNOWN_CLASS_CAP = 2
KNOWN_CLASS_EXIT = 93


def print_failure_diagnostics(text, after, failing_pids=()):
    # Bound general streams; failing PIDs below retain every matching line.
    for prefix, title in (("INIT-LOST:", "INIT loss diagnostics (CLOCK_MONOTONIC)"),
                          ("WP-ATTACH-FAIL:", "Watchpoint attach diagnostics"),
                          ("EXIT-BEFORE-ATTACH:", "Exit before attach diagnostics"),
                          ("ERROR: cannot validate lifecycle target", "Lifecycle target refusals")):
        lines = deque(maxlen=DIAGNOSTIC_LINES)
        total = 0
        for line in text.splitlines():
            if line.startswith(prefix):
                total += 1
                lines.append(line[:DIAGNOSTIC_WIDTH])
        print(f"{title} (last {len(lines)} of {total}, max {DIAGNOSTIC_WIDTH} chars/line):")
        print("\n".join(lines))
    if after is not None:
        print({name: after[name] for name in (
            "lifecycle_init_lost_total", "lifecycle_init_lost_with_opportunity_total",
            "lifecycle_init_lost_no_opportunity_total",
            "lifecycle_init_lost_untraced_ns_total", "init_state_read_fail_total",
            EXIT_BEFORE_ATTACH, *EXIT_CLASSES.values(), *ATTACH_COUNTERS)})
    # One scan regardless of cohort size. Generic failures have no identified
    # PID; only their bounded streams above are printed.
    selected = set(failing_pids)
    matching = {}
    for line in text.splitlines():
        for pid in set(map(int, re.findall(r"(?:\bpid=|\bPID )(\d+)\b", line))) & selected:
            matching.setdefault(pid, []).append(line)
    for pid in sorted(matching):
        print(f"Daemon lines for failing PID {pid}:")
        print("\n".join(matching[pid]))


class CohortFailure(AssertionError):
    def __init__(self, message, pids):
        super().__init__(f"{message}: {sorted(pids)[:DIAGNOSTIC_LINES]} (total {len(pids)})")
        self.pids = set(pids)


def exit_race_diagnostics(text):
    counts = Counter()
    for line in text.splitlines():
        if not line.startswith("WP-ATTACH-FAIL:"):
            continue
        fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
        if (fields.get("errno") == "3" and fields.get("exit_proof") in ("1", "2") and
                fields.get("liveness") in ("gone", "exiting")):
            counts[int(fields["pid"])] += 1
    return counts


def attach_failure_pids(text):
    # Preserve real failures even when the same PID also has an exit-race
    # operation. Validation/allocation/reuse refusals have no WP line.
    failures = Counter(map(int, re.findall(r"^WP-ATTACH-FAIL: pid=(\d+) ", text, re.M)))
    failures.subtract(exit_race_diagnostics(text))
    result = {pid for pid, count in failures.items() if count > 0}
    for pattern in (r"ERROR: cannot validate lifecycle target PID (\d+):",
                    r"WARN: max backends \(\d+\) reached, cannot track PID (\d+)",
                    r"ERROR: live FORK PID (\d+) conflicts with retained attach exit race"):
        result.update(map(int, re.findall(pattern, text)))
    return result


def retirement_diagnostics(text):
    result = {key: Counter() for key in EXIT_CLASSES}
    labels = {"with": "with_opportunity", "no": "no_opportunity", "unknown": "unknown"}
    for line in text.splitlines():
        if line.startswith("EXIT-BEFORE-ATTACH:"):
            fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
            key = labels.get(fields.get("opportunity"), "unknown")
            result[key][int(fields["pid"])] += 1
    return result


def check_cohort(text, pids, short_lived, before, after):
    """Opportunity evidence decides acceptance; lifetime never excuses loss."""
    def require(ok, message, failing):
        if not ok:
            raise CohortFailure(message, failing)

    attached = Counter(map(int, re.findall(r"INFO: PID (\d+) initialized \(", text)))
    init_classes = [Counter(map(int, re.findall(
        rf"INIT-LOST: [^\n]*pid=(\d+) [^\n]*no_opportunity={value}(?: |$)", text)))
        for value in (0, 1)]
    with_init, no_init = init_classes
    retirement = retirement_diagnostics(text)
    before_attach = sum(retirement.values(), Counter())
    race = exit_race_diagnostics(text)
    for counts in (attached, *init_classes, race, before_attach):
        duplicates = {pid for pid in pids if counts[pid] > 1}
        require(not duplicates, "duplicate outcome diagnostic", duplicates)
    # Exit race is a reason, independent of the loss/opportunity outcome.
    # In particular retained INIT + exit proof is legal per PID.
    outcomes = (attached, *init_classes, before_attach)
    overlap = {pid for pid in pids if sum(pid in c for c in outcomes) > 1}
    overlap |= pids & set(attached) & set(race)
    require(not overlap, "overlapping retirement classes", overlap)
    require(not (pids & set(with_init)), "with-opportunity INIT loss", pids & set(with_init))
    for key in ("with_opportunity", "unknown"):
        require(not (pids & set(retirement[key])), f"{key} retirement is gated",
                pids & set(retirement[key]))
        delta = after[EXIT_CLASSES[key]] - before[EXIT_CLASSES[key]]
        require(delta == 0, f"{key} retirement delta must be zero", set(retirement[key]))
    missing = pids - set(attached) - set(no_init) - set(retirement["no_opportunity"])
    require(not missing, "backend has no counted outcome", missing)
    for counts, counter in ((no_init, "lifecycle_init_lost_no_opportunity_total"),
                            (with_init, "lifecycle_init_lost_with_opportunity_total"),
                            (race, "wp_attach_exit_race_total"),
                            (before_attach, EXIT_BEFORE_ATTACH),
                            *((retirement[key], name) for key, name in EXIT_CLASSES.items())):
        total = sum(counts.values())
        delta = after[counter] - before[counter]
        # A numerical mismatch alone cannot identify a guilty PID. Do not
        # feed the whole cohort to the failure printer.
        require(total - before[counter] == delta and delta >= 0,
                f"{counter} delta does not match PID diagnostics", set())
    return attached, no_init, race, before_attach


def classify_churn(text, deltas, pids, short_lived):
    """Pure, fail-closed classification; validate every remaining cohort check.

    Only the two complete measured signatures may be removed from the strict
    cohort check. Counter reconciliation covers the entire daemon stream, so
    an unrelated PID or an undiagnosed increment cannot hide behind them.
    """
    records = {prefix: {} for prefix in
               ("INIT-LOST:", "WP-ATTACH-FAIL:", "EXIT-BEFORE-ATTACH:")}
    for line in text.splitlines():
        prefix = line.split(" ", 1)[0]
        if prefix not in records:
            continue
        pairs = re.findall(r"(\w+)=([^ ]+)", line)
        fields = dict(pairs)
        if (len(pairs) != len(fields) or not fields.get("pid", "").isdigit()):
            raise CohortFailure("partial or ambiguous diagnostic", set())
        pid = int(fields["pid"])
        records[prefix].setdefault(pid, []).append(fields)

    retirement = retirement_diagnostics(text)
    init_with = Counter(map(int, re.findall(
        r"INIT-LOST: [^\n]*pid=(\d+) [^\n]*no_opportunity=0(?: |$)", text)))
    k1 = set(retirement["with_opportunity"])
    k2 = set(init_with)
    known = k1 | k2
    if not known <= pids & short_lived or k1 & k2:
        raise CohortFailure("unknown or overlapping known-class outcome", known)
    if len(known) > KNOWN_CLASS_CAP:
        raise CohortFailure("known-class cap exceeded", known)

    def exact(fields, expected, numeric=()):
        return (all(fields.get(key) == value for key, value in expected.items()) and
                all(fields.get(key, "").isdigit() for key in numeric))

    for pid in known:
        init = records["INIT-LOST:"].get(pid, [])
        wp = records["WP-ATTACH-FAIL:"].get(pid, [])
        retired = records["EXIT-BEFORE-ATTACH:"].get(pid, [])
        valid = False
        if pid in k1 and len(retired) == 1 and not init and not wp:
            fields = retired[0]
            valid = exact(fields, {
                "opportunity": "with", "entry_present": "1", "bootstrap_armed": "1",
                "post_arm_checked": "1", "addr_shared": "0", "resolve_errno": "0",
                "shared_errno": "0", "counter_deltas_valid": "1",
                "lifecycle_init_no_state_delta": "0", "wait_read_fail_delta": "0"})
            valid = valid and bool(re.fullmatch(r"0x[0-9a-fA-F]+", fields.get("post_arm_ptr", "")))
            valid = valid and int(fields["post_arm_ptr"], 16) > 0
        elif pid in k2 and len(init) == len(wp) == 1 and not retired:
            fields = init[0]
            valid = exact(fields, {"clock": "CLOCK_MONOTONIC", "no_opportunity": "0",
                                   "exit_ts_source": "record"},
                          ("init_ts", "exit_ts", "last_sweep_start_ns", "batch_start_ns",
                           "batch_records", "since_epoll_return_ns"))
            valid = valid and 0 < int(fields["init_ts"]) < int(fields["exit_ts"])
            valid = valid and exact(wp[0], {
                "site": "watchpoint_open", "errno": "3", "resolve_errno": "0",
                "liveness": "exiting", "liveness_result": "1", "liveness_errno": "0",
                "exit_proof": "2"})
        if not valid:
            raise CohortFailure("missing, partial or duplicate known-class diagnostic", {pid})
    attached = set(map(int, re.findall(r"INFO: PID (\d+) initialized \(", text)))
    if known & attached:
        raise CohortFailure("overlapping attached and known-class outcome", known & attached)
    failures = attach_failure_pids(text)
    if failures:
        raise CohortFailure("watchpoint attach failure", failures)
    for key, value in deltas.items():
        if key.startswith("ringbuf_") and key.endswith("drops_total") and value != 0:
            raise CohortFailure(f"{key} must be zero", set())
    for key in (*ATTACH_COUNTERS[:1], *ATTACH_COUNTERS[2:],
                "ringbuf_lifecycle_fork_drops_total", "ringbuf_lifecycle_exit_drops_total",
                "state_map_full_total", "init_state_read_fail_total"):
        if deltas[key] != 0:
            raise CohortFailure(f"{key} must be zero", set())
    # Require exact agreement before subtracting any diagnosed outcome.
    for key, count in ((EXIT_CLASSES["with_opportunity"], len(k1)),
                       ("lifecycle_init_lost_with_opportunity_total", len(k2))):
        if deltas[key] != count:
            raise CohortFailure(f"{key} does not match known-class PIDs", known)
    if deltas["lifecycle_init_lost_total"] != (
            deltas["lifecycle_init_lost_with_opportunity_total"] +
            deltas["lifecycle_init_lost_no_opportunity_total"]):
        raise CohortFailure("INIT loss counters disagree", set())
    remaining = dict(deltas)
    for key, count in ((EXIT_BEFORE_ATTACH, len(k1)),
                       (EXIT_CLASSES["with_opportunity"], len(k1)),
                       ("lifecycle_init_lost_total", len(k2)),
                       ("lifecycle_init_lost_with_opportunity_total", len(k2)),
                       ("wp_attach_exit_race_total", len(k2))):
        remaining[key] -= count
    kept = []
    for line in text.splitlines():
        prefix = line.split(" ", 1)[0]
        match = re.search(r"\bpid=(\d+)\b", line)
        if prefix in records and match and int(match[1]) in known:
            continue
        kept.append(line)
    check_cohort("\n".join(kept), pids - known, short_lived - known,
                 dict.fromkeys(remaining, 0), remaining)
    return {"K1": k1, "K2": k2}


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
                "lifecycle_init_lost_with_opportunity_total",
                "lifecycle_init_lost_no_opportunity_total",
                "lifecycle_init_lost_untraced_ns_total",
                "ringbuf_lifecycle_fork_drops_total", "ringbuf_lifecycle_exit_drops_total",
                "state_map_full_total", "init_state_read_fail_total",
                EXIT_BEFORE_ATTACH, *EXIT_CLASSES.values(), *ATTACH_COUNTERS):
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


def short_cohort():
    # Equivalent to pgbench -C -c16 -j4, with the PID returned by every NEW
    # connection so the gate checks every attachment rather than a log count.
    # 20ms sleep makes lifetimes much shorter than the minimum 1s tick.
    started = time.monotonic()
    deadline = started + 20

    def worker():
        pids = []
        while time.monotonic() < deadline:
            connected_at = time.monotonic()
            session = subprocess.Popen(
                ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt",
                 "-v", "ON_ERROR_STOP=1", "-c",
                 "SELECT pg_backend_pid(); SELECT pg_sleep(0.02);"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env={**os.environ, "PGCONNECT_TIMEOUT": "10"})
            try:
                out, err = session.communicate(timeout=15)
                assert session.returncode == 0, (session.returncode, err)
                lines = out.strip().splitlines()
                assert lines and lines[0].isdigit(), (out, err)
                pids.append((int(lines[0]), time.monotonic() - connected_at))
            finally:
                if session.poll() is None:
                    session.kill()
                session.wait(timeout=5)
        assert pids, "short-connection worker never connected"
        assert any(elapsed < TICK_SECONDS for _, elapsed in pids), "worker connections all outlived a tick"
        return pids

    with ThreadPoolExecutor(max_workers=16) as pool:
        futures = [pool.submit(worker) for _ in range(16)]
        connections = [row for future in futures for row in future.result()]
    assert time.monotonic() - started >= 20, "churn stimulus was shorter than 20s"
    pids = [pid for pid, _ in connections]
    assert len(pids) == len(set(pids)), "PID reused during the short cohort"
    return set(pids), {pid for pid, elapsed in connections if elapsed < TICK_SECONDS}


def run_arm(pm_pid, skip_store, churn=False):
    known = {"K1": set(), "K2": set()}
    with tempfile.TemporaryDirectory(prefix="pgwt_init_hint_") as tmp:
        directory = Path(tmp)
        log = directory / "daemon.stderr"
        env = dict(os.environ)
        env["PGWT_TEST_DROP_INIT_HINT"] = "1"
        env.pop("PGWT_TEST_NO_RECOVERY", None)
        if not churn:
            env["PGWT_TEST_NO_RECOVERY"] = "1"
        env.pop("PGWT_TEST_SKIP_INIT_STORE", None)
        if skip_store:
            env["PGWT_TEST_SKIP_INIT_STORE"] = "1"
        with log.open("w") as err:
            proc = subprocess.Popen(
                [str(TRACER), "--mode", "full", "--pid", str(pm_pid),
                 "-T", str(directory), "-i", str(TICK_SECONDS), "--duration", "90", "-q", "-v"],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            after = None
            pids = set()
            try:
                wait_for(proc, lambda: (directory / "pgwt.sock").exists(), 45,
                         "daemon control socket did not become ready")
                before = ctl(directory)
                pids, short_lived = short_cohort() if churn else (cohort(), set())
                # Await the already-generated EXIT acknowledgements, not a
                # second stimulus or a re-capture after a failed assertion.
                unretired = set()
                def retired():
                    nonlocal unretired
                    text = log.read_text()
                    exited = {int(pid) for pid in re.findall(r"INFO: PID (\d+) exited \(", text)}
                    raced = set(exit_race_diagnostics(text))
                    before_attach = {int(pid) for pid in re.findall(r"EXIT-BEFORE-ATTACH: pid=(\d+) ", text)}
                    init_lost = set(map(int, re.findall(r"INIT-LOST: [^\n]*pid=(\d+) ", text)))
                    unretired = pids - exited - raced - before_attach - init_lost
                    return not unretired
                try:
                    wait_for(proc, retired, 10, "not every cohort EXIT was handled")
                except AssertionError as error:
                    if unretired:
                        raise CohortFailure(str(error), unretired) from error
                    raise
                after = ctl(directory)
                text = log.read_text()
                assert "PGWT_TEST_DROP_INIT_HINT" in text, "missing INIT hint-drop hook diagnostic"
                hint_delta = after["lifecycle_init_hint_drops_total"] - before["lifecycle_init_hint_drops_total"]
                lost_delta = after["lifecycle_init_lost_total"] - before["lifecycle_init_lost_total"]
                with_opportunity_delta = (after["lifecycle_init_lost_with_opportunity_total"] -
                                          before["lifecycle_init_lost_with_opportunity_total"])
                assert after["lifecycle_init_lost_total"] == (
                    after["lifecycle_init_lost_with_opportunity_total"] +
                    after["lifecycle_init_lost_no_opportunity_total"]), after
                assert hint_delta > 0, "no bootstrap INIT hint was actually suppressed"
                if churn and not skip_store:
                    known = classify_churn(text, {key: after[key] - before[key] for key in before
                                                  if type(before[key]) is int}, pids, short_lived)
                for key in ("ringbuf_lifecycle_fork_drops_total", "ringbuf_lifecycle_exit_drops_total",
                            "state_map_full_total", "init_state_read_fail_total",
                            "wp_attach_failures_total"):
                    if after[key] != 0:
                        failing = set()
                        if key == "wp_attach_failures_total":
                            failing = attach_failure_pids(text)
                        error = AssertionError((key, after[key]))
                        error.pids = failing
                        raise error
                if not (churn and not skip_store):
                    for key in ("with_opportunity", "unknown"):
                        if after[EXIT_CLASSES[key]] - before[EXIT_CLASSES[key]] != 0:
                            raise CohortFailure(f"{key} retirement delta must be zero",
                                                set(retirement_diagnostics(text)[key]))
                attach_counts = Counter(int(pid) for pid in re.findall(r"INFO: PID (\d+) initialized \(", text))
                attached = set(attach_counts)
                pending = {int(pid) for pid in re.findall(r"fork detected PID (\d+), bootstrap watchpoint attached", text)}
                assert pids & pending, "no cohort backend entered pending bootstrap"
                if churn:
                    assert short_lived & pending, "no pending bootstrap backend lived for less than one tick"
                if skip_store:
                    assert "PGWT_TEST_SKIP_INIT_STORE" in text, "missing INIT store-skip hook diagnostic"
                    assert with_opportunity_delta > 0, "skipping INIT stores did not raise with-opportunity losses"
                    ok, detail = daemon_integrity_ok(after)
                    assert not ok and "lifecycle_init_lost_with_opportunity_total=" in detail, detail
                    assert pids - attached, "control attached everything despite discarding all INIT facts"
                else:
                    if not churn:
                        if after["lifecycle_init_lost_with_opportunity_total"] != 0:
                            raise CohortFailure("with-opportunity INIT loss", set(map(int, re.findall(
                                r"INIT-LOST: [^\n]*pid=(\d+) [^\n]*no_opportunity=0(?: |$)", text))))
                        check_cohort(text, pids, short_lived, before, after)
                    # Every attached member attaches once, including delayed hints.
                    for pid in pids & attached:
                        assert attach_counts[pid] == 1, pid
                arm = "store skipped" if skip_store else "short churn" if churn else "long-lived recovery"
                print(f"  {arm}: "
                      f"{len(pids & attached)}/{len(pids)} attached, "
                      f"hint drops +{hint_delta}, INIT lost +{lost_delta}, "
                      f"attach exit races {after['wp_attach_exit_race_total']}, "
                      f"exit before attach {after[EXIT_BEFORE_ATTACH]}, "
                      f"retirement classes { {key: after[name] for key, name in EXIT_CLASSES.items()} }, "
                      f"with opportunity {after['lifecycle_init_lost_with_opportunity_total']}, "
                      f"no opportunity {after['lifecycle_init_lost_no_opportunity_total']}, "
                      f"untraced ns {after['lifecycle_init_lost_untraced_ns_total']}, "
                      f"{len(short_lived & pending)} pending backends shorter than a tick")
            except Exception as error:
                text = log.read_text()
                print_failure_diagnostics(text, after, getattr(error, "pids", ()))
                raise
            finally:
                stop(proc)
    return known


def run_arms(pm_pid):
    run_arm(pm_pid, skip_store=False)
    known = run_arm(pm_pid, skip_store=False, churn=True)
    run_arm(pm_pid, skip_store=True)
    count = len(known["K1"] | known["K2"])
    if count:
        print(f"KNOWN-CLASS #933 arm=churn K1={sorted(known['K1'])} "
              f"K2={sorted(known['K2'])} count={count} cap={KNOWN_CLASS_CAP}")
        return KNOWN_CLASS_EXIT
    print("test_init_hint_recovery: PASS")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    assert os.geteuid() == 0, "live test requires root"
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "PostgreSQL postmaster not found"
    return run_arms(pm_pid)


if __name__ == "__main__":
    sys.exit(main())
