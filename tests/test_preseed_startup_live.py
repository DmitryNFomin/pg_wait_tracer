#!/usr/bin/env python3
"""PG17 unpublished status: cold wait cache, startup lock and red controls.

Linux/root command: sudo python3 tests/test_preseed_startup_live.py --pid PM_PID
Other PG majors: SKIP test_preseed_startup_live: requires PG17 (exit 77).
The live runner excludes those majors before invoking this test.
--self-test exercises the duration/attribution oracle on any host; no BPF runs.
Startup arms suppress sampler wait-address resolution (TEST ONLY, default off).
Red C additionally disables preseed refresh: that same hold must roll back.
Red D holds the startup backend's status change counter odd (TEST ONLY hook,
default off) for longer than the paced torn re-read budget: it must roll back
and count exact_preseed_torn_exhausted_total.
Expected PG17 lines:
PASS: PG17 cold-cache refresh, unpublished escalation, query-less startup Lock duration, first pg_sleep attribution
PASS: red control A denied and fully rolled back
PASS: red control B caught by startup Lock duration assertion
PASS: red control C disabled refresh denied and fully rolled back
PASS: red control D held odd change counter denied, counted and fully rolled back
test_preseed_startup_live: PASS (cold-cache startup hold, red controls A/B/C/D)

Source ordering verified in PostgreSQL REL_17_STABLE:
https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/utils/init/postinit.c
InitPostgres calls pgstat_beinit, then GetDatabaseTuple opens pg_database with
AccessShareLock, then InitializeSession and pgstat_bestart publish the row.
https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/utils/activity/backend_status.c
pgstat_beinit installs the own MyBEEntry; pgstat_bestart publishes st_procpid.
ACCESS EXCLUSIVE on pg_database therefore holds a real shared Lock:relation
wait before status publication. Both holder and observer connect BEFORE the
lock; observation never needs a connection that would block behind it.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time

from test_sampler_batch_live import IdleConnections
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / "pg_wait_tracer"
TOLERANCE_NS = 75_000_000  # new test: scheduling/lock-release bracket allowance
HOLD_SECONDS = 1.2
SLEEP_SQL = "SELECT pg_sleep(0.3) /* pgwt616_first_statement */"
CMD_START = 0xFFFFFFF6
CMD_END = 0xFFFFFFF7

READER = r'''
#include "event_reader.h"
#include <inttypes.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    uint32_t pid = (uint32_t)strtoul(argv[1], NULL, 10);
    for (int f=2; f<argc; f++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, argv[f])) return 2;
        for (int b=0; b<r.num_blocks; b++) {
            struct pgwt_trace_event ev[PGWT_BLOCK_EVENTS];
            struct pgwt_block_info info;
            int n = pgwt_reader_decode_block_info(&r, b, ev, PGWT_BLOCK_EVENTS, &info);
            if (n < 0 || (uint32_t)n != info.num_events) return 2;
            if (info.block_type != PGWT_BLOCK_TRANSITIONS) continue;
            for (int i=0; i<n; i++) {
                if (ev[i].pid != pid) continue;
                printf("{\"pid\":%u,\"end\":%"PRIu64",\"duration\":%"PRIu64
                       ",\"event\":%u,\"query\":%"PRIu64",\"flags\":%u}\n",
                       ev[i].pid, ev[i].timestamp_ns, ev[i].duration_ns,
                       ev[i].old_event, ev[i].query_id, ev[i].flags);
            }
        }
        pgwt_reader_close(&r);
    }
    return 0;
}
'''


def build_reader(directory):
    source, binary = directory / "reader.c", directory / "reader"
    source.write_text(READER)
    command = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall",
               "-Wextra", "-Werror", *shlex.split(os.environ.get("CFLAGS", "")),
               "-I" + str(ROOT / "src"), str(source), str(ROOT / "src/event_reader.c"),
               str(ROOT / "src/event_writer.c"), "-llz4", "-o", str(binary)]
    if sys.platform == "darwin":
        prefix = subprocess.check_output(["brew", "--prefix", "lz4"], text=True).strip()
        command[1:1] = ["-I" + prefix + "/include", "-L" + prefix + "/lib",
                         "-include", str(ROOT / "demos/replay-compat.h")]
    subprocess.run(command, check=True, capture_output=True, text=True)
    return binary


def request(directory, cmd, **kwargs):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(30)
        client.connect(str(directory / "pgwt.sock"))
        client.sendall((json.dumps(dict(cmd=cmd, **kwargs)) + "\n").encode())
        data = b""
        while b"\n" not in data:
            part = client.recv(65536)
            assert part, "control reply ended without newline"
            data += part
    return json.loads(data.splitlines()[0])


def until(proc, predicate, reason, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert proc.poll() is None, ("daemon exited", proc.returncode)
        result = predicate()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError(reason)


def command_spans(events):
    """One pid's command windows; READER emits timestamp_ns as `end`."""
    spans = []
    start = None
    for event in sorted(events, key=lambda e: e["end"]):
        if event["event"] == CMD_START and start is None:
            start = event["end"]
        elif event["event"] == CMD_END and start is not None:
            spans.append((start, event["end"]))
            start = None
    if start is not None:
        # No close marker: retain the command through the captured tail.
        spans.append((start, max(e["end"] for e in events)))
    return spans


def startup_duration(events, clocks):
    """Independent wall-clock brackets; never use product totals as truth."""
    send, armed, release_before, release_after = clocks
    lock_indices = [i for i, e in enumerate(events) if e["event"] >> 24 == 3]
    # PG17 ProcSleep re-enters WaitLatch after CheckDeadLock: one held lock
    # can report two Lock segments with a short on-CPU interval between them.
    assert 1 <= len(lock_indices) <= 2, (
        "startup Lock duration: expected one or two completed Lock segments")
    locks = [events[i] for i in lock_indices]
    between = events[lock_indices[0] + 1:lock_indices[-1]]
    assert all(e["event"] == 0 for e in between), (
        "startup Lock duration: non-CPU record between Lock segments", between)
    assert sum(e["duration"] for e in between) <= 10_000_000, (
        "startup Lock duration: CPU gap exceeds 10 ms", between)
    start = locks[0]["end"] - locks[0]["duration"]
    end = locks[-1]["end"]
    span = end - start
    low = release_before - armed - TOLERANCE_NS
    high = release_after - send + TOLERANCE_NS
    assert low > 0 and low <= span <= high, (
        "startup Lock duration outside independent hold bounds", locks, low, high)
    assert send - TOLERANCE_NS <= start <= armed + TOLERANCE_NS, (
        "startup Lock duration starts outside escalation bracket", locks, clocks)
    assert release_before - TOLERANCE_NS <= end <= release_after + TOLERANCE_NS, (
        "startup Lock duration ends outside release bracket", locks, clocks)
    assert all(end <= cmd_start or cmd_end <= start
               for cmd_start, cmd_end in command_spans(events)), (
        "startup Lock run must be command-closed", locks, command_spans(events))
    for event in locks:
        assert event["query"] == 0, (
            "startup Lock must be query-less", event)
    return sum(e["duration"] for e in locks)


def first_statement(events, query_id, sleep_event):
    sleeps = [e for e in events if e["event"] == sleep_event and e["duration"] > 0]
    assert sleeps and query_id != 0, "first pg_sleep statement has no wait/query witness"
    locks = [e for e in events if e["event"] >> 24 == 3]
    assert locks, "first pg_sleep statement has no startup Lock witness"
    lock_end = max(e["end"] for e in locks)
    spans = command_spans(events)
    assert all(e["query"] == query_id for e in sleeps), (
        "first pg_sleep attributed to its statement", sleeps, query_id)
    assert all(any(lock_end < start <= e["end"] - e["duration"]
                   and e["end"] <= end for start, end in spans)
               for e in sleeps), (
        "first pg_sleep must lie inside a command starting after startup Lock", sleeps, spans)


def startup_waiter(query, relation, excluded):
    """The one pg_database waiter with NO published status row, else None.

    Another backend can block behind the test's ACCESS EXCLUSIVE lock too,
    e.g. the PG17 autovacuum launcher (REL_17_STABLE autovacuum.c:1817,
    get_database_list: table_open(DatabaseRelationId, AccessShareLock)).
    Such a backend already has a published row; the startup target does not.
    pg_stat_get_activity(NULL), unlike the view, does not block behind the lock.
    Published waiters are recorded in `excluded` (pid -> backend_type) and
    printed once. Two unpublished waiters are genuinely ambiguous.
    """
    rows = query(f"SELECT l.pid, a.pid IS NOT NULL, coalesce(a.backend_type, '') FROM pg_locks l "
                 f"LEFT JOIN pg_stat_get_activity(NULL) a ON a.pid = l.pid "
                 f"WHERE l.locktype='relation' AND l.relation={relation} "
                 f"AND l.mode='AccessShareLock' AND NOT l.granted")
    assert all(len(row) == 3 and row[1] in ("t", "f") for row in rows), ("malformed waiter rows", rows)
    for pid, published, backend_type in rows:
        if published == "t" and int(pid) not in excluded:
            excluded[int(pid)] = backend_type
            print(f"INFO: excluded published pg_database waiter PID {pid} "
                  f"backend_type={backend_type or '<empty>'}", flush=True)
    unpublished = [int(pid) for pid, published, _ in rows if published == "f"]
    assert len(unpublished) <= 1, ("ambiguous startup waiter", rows)
    return unpublished[0] if unpublished else None


def host_checks():
    import contextlib
    import io
    from unittest.mock import patch
    clocks = (1_000_000_000, 1_050_000_000, 2_250_000_000, 2_260_000_000)
    lock = dict(event=0x03000001, end=2_255_000_000, duration=1_230_000_000,
                query=0, flags=0)
    sleep = dict(event=0x09000001, end=2_600_000_000, duration=300_000_000,
                 query=123, flags=0)

    def marker(kind, end):
        return dict(event=kind, end=end, duration=0, query=0, flags=0)

    # Like g3/g4: startup Lock, a short SET command, then the pg_sleep command.
    set_start = marker(CMD_START, 2_270_000_000)
    set_end = marker(CMD_END, 2_280_000_000)
    cmd_start = marker(CMD_START, 2_290_000_000)
    cmd_end = marker(CMD_END, 2_610_000_000)
    events = [lock, set_start, set_end, cmd_start, sleep, cmd_end]
    assert command_spans(events) == [(set_start["end"], set_end["end"]),
                                     (cmd_start["end"], cmd_end["end"])]
    assert startup_duration(events, clocks) == 1_230_000_000
    first_statement(events, 123, 0x09000001)
    open_events = events[:-1]
    assert command_spans(open_events)[-1] == (cmd_start["end"], sleep["end"])
    assert startup_duration(open_events, clocks) == 1_230_000_000
    first_statement(open_events, 123, 0x09000001)
    split_clocks = (5528624706083, 5528649686083, 5529853069965, 5529854663041)
    first_lock = dict(event=0x03000000, end=5529617624997, duration=976_843_403,
                      query=0, flags=0)
    cpu = dict(event=0, end=5529617677334, duration=52_337, query=0, flags=0)
    second_lock = dict(event=0x03000000, end=5529854606858, duration=236_929_524,
                       query=0, flags=0)
    split_tail = [marker(CMD_START, 5529856707746), marker(CMD_END, 5529856906765),
                  marker(CMD_START, 5529857078624),
                  dict(sleep, end=5530158157720, duration=300395399),
                  marker(CMD_END, 5530158261821)]
    split_events = [first_lock, cpu, second_lock, *split_tail]
    assert startup_duration(split_events, split_clocks) == 1_213_772_927
    first_statement(split_events, 123, 0x09000001)
    red = 0
    for bad_events, message in (
            (events[1:], "startup Lock duration"),
            ([dict(lock, duration=1), *events[1:]], "startup Lock duration"),
            ([dict(lock, query=123), *events[1:]], "startup Lock must be query-less"),
            ([marker(CMD_START, lock["end"] - 1), lock, cmd_end],
             "startup Lock run must be command-closed")):
        try:
            startup_duration(bad_events, clocks)
        except AssertionError as error:
            assert message in str(error), error
            red += 1
        else:
            raise AssertionError("duration/attribution control survived")
    long_gap = 10_000_001
    for bad_events, message in (
            ([first_lock, dict(cpu, event=0x09000001), second_lock, *split_tail],
             "non-CPU record between Lock segments"),
            ([dict(first_lock, end=cpu["end"] - long_gap,
                   duration=first_lock["duration"] - (long_gap - cpu["duration"])),
              dict(cpu, duration=long_gap), second_lock, *split_tail], "CPU gap exceeds 10 ms"),
            ([first_lock, cpu,
              dict(second_lock, end=second_lock["end"] - 1,
                   duration=second_lock["duration"] - 1),
              dict(second_lock, duration=1), *split_tail], "expected one or two completed Lock segments"),
            ([first_lock, cpu, dict(second_lock, query=123), *split_tail],
             "startup Lock must be query-less"),
            # A command wholly in the CPU gap still overlaps the whole Lock run.
            ([first_lock, marker(CMD_START, first_lock["end"] + 1),
              marker(CMD_END, cpu["end"] - 1), cpu, second_lock, *split_tail],
             "non-CPU record between Lock segments")):
        try:
            startup_duration(bad_events, split_clocks)
        except AssertionError as error:
            assert message in str(error), error
            red += 1
        else:
            raise AssertionError("split Lock control survived")
    span_message = "inside a command starting after startup Lock"
    for bad_events, message in (
            ([lock, cmd_start, dict(sleep, query=0), cmd_end], "attributed to its statement"),
            ([lock, set_start, set_end, sleep], span_message),
            ([marker(CMD_START, lock["end"] - 1), lock, sleep, cmd_end], span_message),
            ([lock, dict(cmd_start, end=2_310_000_000), sleep, cmd_end], span_message),
            ([lock, cmd_start, dict(cmd_end, end=2_590_000_000), sleep], span_message),
            ([*events, dict(sleep, end=2_920_000_000)], span_message)):
        try:
            first_statement(bad_events, 123, 0x09000001)
        except AssertionError as error:
            assert message in str(error), error
            red += 1
        else:
            raise AssertionError("first-statement attribution control survived")
    launcher = ["8397", "t", "autovacuum launcher"]
    target = ["46666", "f", ""]
    waiter_cases = 0
    sql = []

    def fixed(rows):
        return lambda statement: sql.append(statement) or rows

    excluded = {}
    with contextlib.redirect_stdout(io.StringIO()) as shown:
        assert startup_waiter(fixed([launcher, target]), 1262, excluded) == 46666
        assert startup_waiter(fixed([launcher, target]), 1262, excluded) == 46666
    assert excluded == {8397: "autovacuum launcher"}, excluded
    assert shown.getvalue() == (
        "INFO: excluded published pg_database waiter PID 8397 backend_type=autovacuum launcher\n")
    assert all("pg_stat_get_activity(NULL)" in s and "relation=1262" in s and "NOT l.granted" in s
               for s in sql), sql
    waiter_cases += 1
    try:
        startup_waiter(fixed([target, ["46667", "f", ""]]), 1262, {})
    except AssertionError as error:
        assert "ambiguous startup waiter" in str(error), error
        waiter_cases += 1
    else:
        raise AssertionError("two unpublished waiters must be ambiguous")
    polls = iter([[launcher], [launcher], [launcher, target]])
    alive = type("Alive", (), {"poll": lambda self: None})()
    excluded = {}
    with contextlib.redirect_stdout(io.StringIO()):
        assert startup_waiter(lambda statement: [launcher], 1262, excluded) is None
        assert until(alive, lambda: startup_waiter(lambda s: next(polls), 1262, excluded),
                     "waiter", 5) == 46666
    assert next(polls, None) is None and excluded == {8397: "autovacuum launcher"}
    waiter_cases += 1
    with tempfile.TemporaryDirectory(prefix="pgwt616-reader-host-") as temporary:
        reader = build_reader(Path(temporary))
        assert subprocess.run([str(reader)], capture_output=True).returncode == 2
    for version in (13, 18):
        output = io.StringIO()
        with patch.object(sys, "argv", ["test_preseed_startup_live.py", "--pid", "7"]), \
             patch.object(sys, "platform", "linux"), patch.object(os, "geteuid", return_value=0), \
             patch.object(subprocess, "check_output", return_value=str(version * 10000)), \
             patch.object(subprocess, "Popen", side_effect=AssertionError("non-PG17 must skip before capture")), \
             contextlib.redirect_stdout(output):
            assert main() == 77
        assert output.getvalue().strip() == f"SKIP test_preseed_startup_live: requires PG17 (running PG{version})"
    for label in ARMS:
        with patch.dict(os.environ, {"PGWT_TEST_UNRELATED": "1"}):
            env = arm_environment(label, Path("/tmp/selected-pid"))
        assert "PGWT_TEST_UNRELATED" not in env
        assert ("PGWT_TEST_SKIP_SAMPLER_WAIT_ADDR" in env) == (label != "failed-read")
        assert ("PGWT_TEST_SKIP_PRESEED_WAIT_ADDR_REFRESH" in env) == (label == "disabled-refresh")
        assert ("PGWT_TEST_PRESEED_READ_FAILURE_PID_FILE" in env) == (label == "failed-read")
        assert ("PGWT_TEST_SKIP_UNPUBLISHED_WAIT_SEED" in env) == (label == "skip-wait-seed")
        assert ("PGWT_TEST_PRESEED_TORN_PID_FILE" in env) == (label == "torn-hold")
        if label == "torn-hold":
            assert env["PGWT_TEST_PRESEED_TORN_PID_FILE"] == "/tmp/selected-pid"
    with patch.object(sys, "argv", ["test_preseed_startup_live.py", "--pid", "7"]), \
         patch.object(sys, "platform", "linux"), patch.object(os, "geteuid", return_value=0), \
         patch.object(subprocess, "check_output", return_value="170000"), \
         patch(__name__ + ".build_reader", return_value=Path("/tmp/reader")), \
         patch(__name__ + ".run_arm") as arm, contextlib.redirect_stdout(io.StringIO()):
        assert main() == 0
    assert [call.args[3] for call in arm.call_args_list] == [
        "startup", "failed-read", "skip-wait-seed", "disabled-refresh", "torn-hold"]
    print(f"startup live oracle host: 6 positive, {red} red controls; decoder build PASS")
    print(f"startup waiter selection: {waiter_cases} cases (published waiter excluded, "
          "two unpublished ambiguous, published-only keeps waiting) PASS")
    print("startup live skip contract: PG13/PG18 exit 77 before capture PASS")
    print("startup live arm wiring: 5 isolated hook environments; PG17 runs startup and red controls A/B/C/D PASS")


ARMS = ("startup", "failed-read", "skip-wait-seed", "disabled-refresh", "torn-hold")
DENIED_ARMS = {"failed-read": "A", "disabled-refresh": "C", "torn-hold": "D"}


def arm_environment(label, selector):
    env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
    if label == "failed-read":
        env["PGWT_TEST_PRESEED_READ_FAILURE_PID_FILE"] = str(selector)
    if label == "skip-wait-seed":
        env["PGWT_TEST_SKIP_UNPUBLISHED_WAIT_SEED"] = "1"
    if label != "failed-read":
        env["PGWT_TEST_SKIP_SAMPLER_WAIT_ADDR"] = "1"
    if label == "disabled-refresh":
        env["PGWT_TEST_SKIP_PRESEED_WAIT_ADDR_REFRESH"] = "1"
    if label == "torn-hold":
        env["PGWT_TEST_PRESEED_TORN_PID_FILE"] = str(selector)
    return env


def run_arm(pm_pid, base, reader, label, tracer=TRACER):
    directory = base / label
    directory.mkdir()
    clients = IdleConnections()
    clients.open(2, "pgwt616_observer")
    observer, holder = clients.handles
    query = lambda statement: clients.query(observer, statement)
    relation = int(query("SELECT 'pg_catalog.pg_database'::regclass::oid")[0][0])
    selector = directory / "fail-pid"
    selector.write_text("0\n")
    env = arm_environment(label, selector)
    daemon = None
    lock_held = False
    pool = ThreadPoolExecutor(max_workers=1)
    worker = None
    log = directory / "daemon.log"
    try:
        with log.open("w") as err:
            daemon = subprocess.Popen(
                [str(tracer), "--pid", str(pm_pid), "--mode", "tiered", "-T", str(directory),
                 "--sample-rate", "100", "--escalation-budget", "120", "-i", "1", "-q"],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            until(daemon, lambda: (directory / "pgwt.sock").exists(), "control socket not ready", 90)
            before = request(directory, "metrics")
            assert before.get("ok") is True and before["tier"] == "sampled", before
            assert request(directory, "status")["pgbackend_layout_validation"] == "validated"

            def new_client(sleep_seconds):
                target = IdleConnections()
                try:
                    target.open(1, "pgwt616_target")
                    handle = target.handles[0]
                    target.query(handle, "SET compute_query_id=on")
                    statement = SLEEP_SQL if sleep_seconds == 0.3 else "SELECT pg_sleep(5) /* pgwt616_red */"
                    target.query(handle, statement)
                finally:
                    target.close()

            if label != "failed-read":
                clients.query(holder, "BEGIN; LOCK pg_catalog.pg_database IN ACCESS EXCLUSIVE MODE")
                lock_held = True
                worker = pool.submit(new_client, 0.3)
                excluded = {}
                pid = until(daemon, lambda: startup_waiter(query, relation, excluded),
                            "new connection did not block before publication")
                until(daemon, lambda: request(directory, "metrics")["backends_tracked"] > before["backends_tracked"],
                      "startup backend not enrolled before escalation")
                # Observe a real lock, not a delay/title heuristic.
                # The pg_stat_activity VIEW joins pg_database and would itself
                # block behind our lock. Its underlying function needs no
                # catalog relation lock and still exposes publication.
                assert query(f"SELECT count(*) FROM pg_stat_get_activity(NULL) WHERE pid={pid}")[0][0] == "0", (
                    "startup waiter unexpectedly has a published status row", pid)
                until(daemon, lambda: f"TEST ONLY skip sampler wait address PID {pid}" in log.read_text(),
                      "sampler cold-cache hook did not reach startup PID")
                if label == "torn-hold":
                    selector.write_text(f"{pid}\n")
            else:
                worker = pool.submit(new_client, 5)
                def established():
                    rows = query("SELECT pid FROM pg_stat_activity WHERE application_name='pgwt616_target' "
                                 "AND state='active' AND wait_event='PgSleep'")
                    return int(rows[0][0]) if len(rows) == 1 else None
                pid = until(daemon, established, "red control A not inside established pg_sleep")
                selector.write_text(f"{pid}\n")

            send = time.monotonic_ns()
            reply = request(directory, "escalate", duration_s=15, reason="manual")
            armed = time.monotonic_ns()
            if label in DENIED_ARMS:
                control = DENIED_ARMS[label]
                assert reply.get("ok") is False and reply.get("error") == (
                    "coherent exact preseed failed; escalation rolled back"), (
                    f"red control {control}: preseed must deny escalation", reply)
                metrics = request(directory, "metrics")
                assert metrics["tier"] == "sampled" and not metrics["escalation_active"], metrics
                assert metrics["exact_probe_attached_mask"] == 0 and not metrics["exact_cpu_active"], metrics
                assert metrics["exact_preseed_entries"] == metrics["exact_preseed_no_command_entries"] == 0, metrics
                assert metrics["exact_preseed_failures_total"] == before["exact_preseed_failures_total"] + 1, metrics
                assert metrics["escalation_windows_total"] == before["escalation_windows_total"], metrics
                assert metrics["exact_preseed_wait_addr_refreshed_total"] == before["exact_preseed_wait_addr_refreshed_total"], metrics
                torn = before["exact_preseed_torn_exhausted_total"] + (label == "torn-hold")
                assert metrics["exact_preseed_torn_exhausted_total"] == torn, metrics
                assert request(directory, "status")["exact_preseed_torn_exhausted_total"] == torn
                if label == "failed-read":
                    assert f"TEST ONLY forced attribution read failure PID {pid}" in log.read_text()
                    print("PASS: red control A denied and fully rolled back")
                    selector.write_text("0\n")
                else:
                    assert f"denied: cannot coherently preseed PID {pid}" in log.read_text()
                    if label == "torn-hold":
                        assert f"TEST ONLY held status change counter odd PID {pid}" in log.read_text()
                        print("PASS: red control D held odd change counter denied, counted and fully rolled back")
                        selector.write_text("0\n")
                    else:
                        print("PASS: red control C disabled refresh denied and fully rolled back")
                    clients.query(holder, "COMMIT")
                    lock_held = False
                worker.result(timeout=10)
                return
            assert reply.get("ok") is True, ("startup escalation denied", reply)
            metrics = request(directory, "metrics")
            assert metrics["exact_preseed_unpublished_total"] >= before["exact_preseed_unpublished_total"] + 1, metrics
            assert request(directory, "status")["exact_preseed_unpublished_total"] == metrics["exact_preseed_unpublished_total"]
            assert metrics["exact_preseed_wait_addr_refreshed_total"] >= before["exact_preseed_wait_addr_refreshed_total"] + 1, metrics
            assert request(directory, "status")["exact_preseed_wait_addr_refreshed_total"] == metrics["exact_preseed_wait_addr_refreshed_total"]
            assert metrics["exact_preseed_failures_total"] == before["exact_preseed_failures_total"], metrics
            assert metrics["exact_preseed_torn_exhausted_total"] == before["exact_preseed_torn_exhausted_total"], metrics
            if label == "startup":
                snapshot = request(directory, "open_intervals")
                assert any(row["pid"] == pid and row["event_id"] >> 24 == 3
                           for row in snapshot["pids"]), ("startup watchpoint not armed on Lock wait", snapshot)
            else:
                assert f"TEST ONLY skip unpublished wait seed PID {pid}" in log.read_text(), (
                    "red control B did not fire", log.read_text())
            time.sleep(HOLD_SECONDS)
            release_before = time.monotonic_ns()
            clients.query(holder, "COMMIT")
            release_after = time.monotonic_ns()
            lock_held = False
            def sleeping_query():
                rows = query(f"SELECT query_id FROM pg_stat_activity WHERE pid={pid} AND state='active' "
                             "AND wait_event='PgSleep' AND query LIKE '%pgwt616_first_statement%'")
                return int(rows[0][0]) % (1 << 64) if rows and rows[0][0] else None
            query_id = until(daemon, sleeping_query, "first pg_sleep query ID not observed", 3)
            worker.result(timeout=10)
            request(directory, "deescalate")
            daemon.terminate()
            daemon.wait(timeout=15)
            assert daemon.returncode == 0, (daemon.returncode, log.read_text())
            paths = sorted(directory.glob("current.trace")) + sorted(directory.glob("????-??-??_??*.trace.lz4"))
            assert paths, "startup capture has no trace files"
            decoded = subprocess.check_output([str(reader), str(pid), *map(str, paths)], text=True, timeout=15)
            events = [json.loads(line) for line in decoded.splitlines()]
            clocks = send, armed, release_before, release_after
            print(json.dumps(dict(arm=label, pid=pid, clocks=clocks, events=events), sort_keys=True))
            if label == "skip-wait-seed":
                try:
                    startup_duration(events, clocks)
                except AssertionError as error:
                    assert "startup Lock duration" in str(error), error
                    print("PASS: red control B caught by startup Lock duration assertion")
                else:
                    raise AssertionError("red control B survived: skipped wait seed must fail duration assertion")
            else:
                startup_duration(events, clocks)
                names = json.loads((directory / "wait_event_names.json").read_text())
                sleep_event = (9 << 24) | names["Timeout"].index("PgSleep")
                first_statement(events, query_id, sleep_event)
                print("PASS: PG17 cold-cache refresh, unpublished escalation, query-less startup Lock duration, first pg_sleep attribution")
    except BaseException:
        if log.exists():
            print(log.read_text()[-16000:], file=sys.stderr, flush=True)
        raise
    finally:
        if lock_held:
            clients.query(holder, "ROLLBACK")
        if daemon is not None and daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=15)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=5)
        pool.shutdown(wait=True)
        clients.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--tracer", type=Path, default=TRACER,
                        help="candidate or separately built m1 mutant daemon")
    args = parser.parse_args()
    if args.self_test:
        host_checks()
        return 0
    assert sys.platform.startswith("linux") and os.geteuid() == 0, "Linux/root live test only"
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "no postmaster"
    version = int(subprocess.check_output(
        ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt", "-c", "SHOW server_version_num"],
        text=True).strip()) // 10000
    if version != 17:
        print(f"SKIP test_preseed_startup_live: requires PG17 (running PG{version})")
        return 77
    with tempfile.TemporaryDirectory(prefix="pgwt616-startup-live-") as temporary:
        base = Path(temporary)
        reader = build_reader(base)
        for label in ARMS:
            run_arm(pm_pid, base, reader, label, args.tracer.resolve())
    print("test_preseed_startup_live: PASS (cold-cache startup hold, red controls A/B/C/D)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
