#!/usr/bin/env python3
"""Status-row syscall gate: 500 idle + 4 pgbench sessions, >=200 traced ticks.

Registered in run_all.sh's live tier. Requires Linux/root, perf and libpq.
Starts a private cluster (PG13: 1100 connections; PG17/18: 540) using the runner's PG
binary, leaving its provisioned cluster untouched. N=500 idle + 4 active
proves two status batch calls on all supported versions.
The syscall evidence is the per-syscall summary of `perf trace -S` over the
daemon (counts by name per sampler tick, tests/sampler_batch_perf.py); it
needs no path names, which neither gate image's perf can resolve.
Master negative control: pass --master-tracer (or set PGWT_866_MASTER_TRACER)
to a tracer built from master on the SAME VM; the SAME assertions must then go
RED at the per-backend file I/O counts, and the test prints which ones.
--require-negative-control enforces that it was supplied.
On PG13 the far-row/short-session arm also runs by default. Select ONLY that
arm with --pg13-far-rows: then --master-tracer accepts the pre-fix candidate-
scan tracer 0b1d7dd6, and the SAME first-attribution assertion must go RED.
Use --syscalls-only for the original syscall/master control in isolation.
The PG13 arm holds 520 initial sessions, retires 32 near the observed anchor,
then opens 500 more; 200 new 50 ms sessions reuse a measured far slot (>513).
It uses 100 Hz, with an unchanged two-period (20 ms) timing bound, a 5 s quiet
window, the computed route and zero scan fallbacks. Persisted trace timestamps
are compared with SQL backend_start, never log-arrival or polling times.
Its syscall entry/exit tracepoints require postmaster readers outside the
explicit wait-word argument-shape exemption and at most two possible discovery
calls per tick. No stack resolution is used; incomplete/lost evidence fails.
--dry-run --pg-version 13 works on the Mac without starting any process under test.
Use --require-huge-pages for the additional huge_pages=on qualification.
All captures/evidence go to a temporary directory outside the repository
(or --evidence-dir). Example:
  sudo python3 tests/test_sampler_batch_live.py --pid PID \
    --master-tracer /tmp/master/pg_wait_tracer --require-negative-control
"""
import argparse
import ctypes
import contextlib
import ctypes.util
import hashlib
import json
import os
import pwd
from pathlib import Path
import re
import signal
import shlex
import socket
import subprocess
import sys
import tempfile
import time

from sampler_batch_perf import (CALLS, SUPPORTED_MAJORS, assess_counts, file_io_reds,
                                parse_summary, assess_pg13_timing, require_pg13_negative_control,
                                assess_pg13_vm_calls, pg13_vm_diagnostics, VM_EVENTS)
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
SHADOW = ("sampled_attr_shadow_total", "sampled_attr_shadow_mismatch_total",
          "sampled_attr_shadow_cmd_open_mismatch_total",
          "sampled_attr_shadow_query_id_mismatch_total")
PG13_RATE = 100  # 50 ms sessions span five ticks; the bound stays TWO periods.
PG13_INITIAL = 520
PG13_RETIRE = 32
PG13_LATER = 500
PG13_CHURN = 200
# 1100 targets: ceil(4N/1024)=5 status chunks, 2 retries; ceil(3N/1024)=4
# gate chunks, 2 retries; 2 discovery + 1 wait = 16 calls/tick. At 100 Hz,
# 2 tracepoints/call + 1 tick marker and <=256 bytes/event (no stacks):
# (16*2+1)*100*256 = 844800 bytes/s.
# 16384 4-KiB pages = 64 MiB per ring, >79 s of that stream (>25 s arm).
# Only readv is recorded; open/pread/close enrollment storms cannot fill it.
# This is capacity planning, NEVER permission to accept a loss record.
PG13_PERF_PAGES = 16384

# PG13's virtualtransaction prefix is the real BackendId, unlike the compact
# pg_stat_get_backend_idset() snapshot index. The self-query holds a virtualxid
# lock while evaluating this expression (sinvaladt.c / lockfuncs.c, REL_13_STABLE).
SESSION_SQL = """SELECT a.pid,
    (extract(epoch FROM a.backend_start) * 1000000000)::bigint,
    split_part(l.virtualtransaction, '/', 1)::int
    FROM pg_stat_activity a JOIN pg_locks l USING (pid)
    WHERE a.pid=pg_backend_pid() AND l.locktype='virtualxid' AND l.granted"""


def session_evidence(connections, handle):
    rows = connections.query(handle, SESSION_SQL)
    assert len(rows) == 1, f"missing independent session identity: {rows}"
    pid, start, slot = map(int, rows[0])
    assert pid == connections.lib.PQbackendPID(handle) and start > 0 and slot > 0
    return {"pid": pid, "backend_start_ns": start, "backend_id": slot}


def build_sample_reader(directory):
    """Use the existing trace decoder, never server aggregates or log arrival.

    The temporary helper emits only the named cohort's persisted SAMPLES,
    converting each point's monotonic timestamp with its own file anchors.
    It rejects undecodable blocks and requires a nonempty sample stream.
    """
    source = directory / "sample-reader.c"
    binary = directory / "sample-reader"
    source.write_text(r'''
#include "event_reader.h"
#include <inttypes.h>
#include <stdlib.h>
static int cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    uint32_t pids[200]; size_t npids = 0; uint64_t seen = 0;
    if (argc < 3) return 2;
    FILE *fp = fopen(argv[1], "r");
    if (!fp) return 2;
    while (npids < 200 && fscanf(fp, "%"SCNu32, &pids[npids]) == 1) npids++;
    fclose(fp);
    if (!npids) return 2;
    qsort(pids, npids, sizeof(*pids), cmp);
    for (int f = 2; f < argc; f++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, argv[f])) return 2;
        for (int b = 0; b < r.num_blocks; b++) {
            struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
            struct pgwt_block_info info;
            int n = pgwt_reader_decode_block_info(&r, b, events, PGWT_BLOCK_EVENTS, &info);
            if (n < 0 || (uint32_t)n != info.num_events) return 2;
            if (info.block_type != PGWT_BLOCK_SAMPLES) continue;
            seen += (uint64_t)n;
            for (int i = 0; i < n; i++) {
                struct pgwt_trace_event *e = &events[i];
                if (!bsearch(&e->pid, pids, npids, sizeof(*pids), cmp)) continue;
                printf("{\"pid\":%u,\"timestamp_ns\":%"PRIu64",\"query_id\":%"PRIu64"}\n",
                    e->pid, pgwt_reader_mono_to_wall(&r, e->timestamp_ns), e->query_id);
            }
        }
        pgwt_reader_close(&r);
    }
    fprintf(stderr, "persisted sample census=%"PRIu64"\n", seen);
    return seen ? 0 : 2;
}
''')
    command = shlex.split(os.environ.get("CC", "cc")) + ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "src"), str(source), str(ROOT / "src/event_reader.c"),
                str(ROOT / "src/event_writer.c"), "-llz4", "-o", str(binary)]
    if sys.platform == "darwin":
        prefix = subprocess.check_output(["brew", "--prefix", "lz4"], text=True).strip()
        command[1:1] = ["-I" + prefix + "/include", "-L" + prefix + "/lib",
                         "-include", str(ROOT / "demos/replay-compat.h")]
    subprocess.run(command, check=True, capture_output=True, text=True)
    return binary


def read_cohort_samples(reader, trace, cohort):
    pidfile = trace / "churn.pids"
    pidfile.write_text("".join(f"{row['pid']}\n" for row in cohort))
    paths = sorted(trace.glob("*.trace")) + sorted(trace.glob("*.trace.lz4"))
    assert paths, "no persisted trace files"
    result = subprocess.run([str(reader), str(pidfile), *map(str, paths)],
                            check=True, capture_output=True, text=True, timeout=60)
    (trace / "sample-reader.stderr").write_text(result.stderr)
    samples = [json.loads(line) for line in result.stdout.splitlines()]
    (trace / "churn.samples.json").write_text(json.dumps(samples) + "\n")
    return samples


def control(directory, request, timeout=5):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(directory / "pgwt.sock"))
        client.sendall(json.dumps(request).encode() + b"\n")
        data = b""
        while b"\n" not in data:
            part = client.recv(65536)
            assert part, "empty control reply"
            data += part
    result = json.loads(data.splitlines()[0])
    assert result.get("ok") is True, result
    return result


def metrics(directory, cmd="metrics"):
    """A control reply. Counters are in `metrics`; layout fields such as
    pgbackend_layout_validation are only in the `status` reply."""
    return control(directory, {"cmd": cmd})


def spawn(command, **kwargs):
    """Popen with SIGINT back at its default action.

    A test launched from a background job (nohup, `&`, a CI step) inherits
    SIGINT as ignored, and an ignored signal survives exec: pgbench then
    never stopped on the SIGINT sent to end it ("child failed to stop").
    """
    return subprocess.Popen(command, preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL),
                            **kwargs)


@contextlib.contextmanager
def private_cluster(source_pid, require_huge_pages):
    """Use that runner's exact PG binary, a private socket and owned data dir."""
    bindir = Path(os.readlink(f"/proc/{source_pid}/exe")).parent
    postgres = pwd.getpwnam("postgres")
    saved = {key: os.environ.get(key) for key in ("PGHOST", "PGPORT", "PATH")}
    # Short Unix socket path even when --evidence-dir is a long pathname.
    with tempfile.TemporaryDirectory(prefix="pgwt866-pg-", dir="/tmp") as tmp:
        directory = Path(tmp)
        os.chown(directory, postgres.pw_uid, postgres.pw_gid)
        data = directory / "data"
        log = directory / "postgres.log"
        as_postgres = ["runuser", "-u", "postgres", "--"]
        subprocess.run(as_postgres + [str(bindir / "initdb"), "-D", str(data),
                       "-U", "postgres", "-A", "trust", "--no-locale"],
                       check=True, stdout=subprocess.DEVNULL, timeout=60)
        version = subprocess.check_output([str(bindir / "postgres"), "--version"], text=True)
        major = int(re.search(r"PostgreSQL\)\s+(\d+)", version)[1])
        max_connections = 1100 if major == 13 else 540
        settings = (f"\nmax_connections={max_connections}\nshared_buffers='32MB'\n"
                    f"unix_socket_directories='{directory}'\nlisten_addresses=''\nport=55486\n"
                    f"autovacuum=off\nhuge_pages={'on' if require_huge_pages else 'try'}\n")
        if major >= 14:
            settings += "compute_query_id=on\n"
        else:
            # PG13 has no core query id: the exact tier needs pg_stat_statements
            # loaded (as the provisioned runner clusters do), or an escalation
            # is refused ("exact probe bundle attach failed").
            settings += "shared_preload_libraries='pg_stat_statements'\n"
        with (data / "postgresql.conf").open("a") as config:
            config.write(settings)
        os.environ.update(PGHOST=str(directory), PGPORT="55486",
                          PATH=str(bindir) + os.pathsep + os.environ["PATH"])
        try:
            subprocess.run(as_postgres + [str(bindir / "pg_ctl"), "-D", str(data),
                           "-l", str(log), "-w", "-t", "60", "start"],
                           check=True, stdout=subprocess.DEVNULL, timeout=70)
            pid = int((data / "postmaster.pid").read_text().splitlines()[0])
            assert int(psql("SHOW max_connections")) == max_connections
            assert int(psql("SHOW server_version_num")) // 10000 == major
            yield pid, major
        finally:
            try:
                if (data / "postmaster.pid").exists():
                    subprocess.run(as_postgres + [str(bindir / "pg_ctl"), "-D", str(data),
                                   "-m", "immediate", "-w", "-t", "30", "stop"],
                                   check=True, stdout=subprocess.DEVNULL, timeout=40)
            finally:
                for key, value in saved.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value


def psql(sql):
    return subprocess.check_output(
        ["psql", "-X", "-U", "postgres", "-d", "postgres", "-qAt",
         "-v", "ON_ERROR_STOP=1", "-c", sql], text=True, timeout=30).strip()


def wait_until(proc, predicate, seconds, reason, poll_seconds=0.1):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert proc.poll() is None, f"{reason}: process exited {proc.returncode}"
        value = predicate()
        if value:
            return value
        time.sleep(poll_seconds)
    raise AssertionError(reason)


def stop(proc, sig=signal.SIGTERM):
    if proc is None:
        return
    if proc.poll() is None:
        proc.send_signal(sig)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("child failed to stop")


class IdleConnections:
    """Use installed libpq, avoiding 500 extra psql processes/dependencies."""
    def __init__(self):
        self.handles = []
        name = ctypes.util.find_library("pq")
        assert name, "installed libpq is required"
        self.lib = ctypes.CDLL(name)
        self.lib.PQconnectdb.argtypes = [ctypes.c_char_p]
        self.lib.PQconnectdb.restype = ctypes.c_void_p
        self.lib.PQstatus.argtypes = [ctypes.c_void_p]
        self.lib.PQerrorMessage.argtypes = [ctypes.c_void_p]
        self.lib.PQerrorMessage.restype = ctypes.c_char_p
        self.lib.PQfinish.argtypes = [ctypes.c_void_p]
        self.lib.PQbackendPID.argtypes = [ctypes.c_void_p]
        self.lib.PQbackendPID.restype = ctypes.c_int
        self.lib.PQexec.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.lib.PQexec.restype = ctypes.c_void_p
        self.lib.PQresultStatus.argtypes = [ctypes.c_void_p]
        self.lib.PQntuples.argtypes = [ctypes.c_void_p]
        self.lib.PQnfields.argtypes = [ctypes.c_void_p]
        self.lib.PQgetvalue.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        self.lib.PQgetvalue.restype = ctypes.c_char_p
        self.lib.PQclear.argtypes = [ctypes.c_void_p]

    def open(self, count=500, application="pgwt866_idle"):
        for _ in range(count):
            handle = self.lib.PQconnectdb(
                f"user=postgres dbname=postgres application_name={application} connect_timeout=10".encode())
            assert handle, "PQconnectdb allocation failed"
            self.handles.append(handle)
            assert self.lib.PQstatus(handle) == 0, self.lib.PQerrorMessage(handle)

    def pids(self):
        return {self.lib.PQbackendPID(handle) for handle in self.handles}

    def query(self, handle, sql):
        result = self.lib.PQexec(handle, sql.encode())
        assert result, self.lib.PQerrorMessage(handle)
        try:
            assert self.lib.PQresultStatus(result) in (1, 2), self.lib.PQerrorMessage(handle)
            return [[self.lib.PQgetvalue(result, row, col).decode()
                     for col in range(self.lib.PQnfields(result))]
                    for row in range(self.lib.PQntuples(result))]
        finally:
            self.lib.PQclear(result)

    def close_handle(self, handle):
        self.lib.PQfinish(handle)
        self.handles.remove(handle)

    def close(self):
        for handle in self.handles:
            self.lib.PQfinish(handle)
        self.handles.clear()


def require_pg13_anchor_status(status, *, negative=False):
    """Confirm validation after observing its anchor pid/backend id via pg_locks."""
    assert status["pgbackend_layout_validation"] == "validated", status
    # 0b1d7dd6 exposes validation but predates the reason field. Its anchor is
    # still observed independently by validation_anchor() before this check.
    if not negative:
        assert "controlled backend retired before capture" in status["pgbackend_layout_reason"], \
            "observed anchor was not the successful controlled validation anchor"


def run_pg13_far_arm(binary, pm_pid, directory, reader, label, negative=False):
    """Far-row churn, with independent SQL identities and persisted timing.

    Keep 520 sessions until validation records an anchor near their high end.
    Retire 32 NEAR that anchor before opening the next 500: peak enrollment
    stays below MAX_BACKENDS=1024. Reuse a LOW slot for each short session;
    PostgreSQL chooses the lowest free BackendId. Assert every actual distance
    rather than assuming this allocation policy succeeded.
    """
    trace = directory / label
    trace.mkdir()
    initial, later, churn = IdleConnections(), IdleConnections(), IdleConnections()
    daemon = perf = decoder = None
    tick_event = None
    cohort = []
    with (trace / "daemon.stderr").open("w") as err, (trace / "perf.stderr").open("w") as perr, \
         (trace / "perf.script.stderr").open("w") as script_err, \
         (trace / "perf.trace").open("w") as perf_out:
        try:
            initial.open(PG13_INITIAL, "pgwt866_anchor_side")
            initial_rows = {handle: session_evidence(initial, handle) for handle in initial.handles}
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            daemon = spawn(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", str(PG13_RATE),
                 "--escalation-budget", "0", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            # Observe the ACTUAL controlled validation backend during its active
            # phase. Its virtualxid lock is independent of daemon row arithmetic.
            app = f"pgwt-layout-validation-{pm_pid}-{daemon.pid}"
            def validation_anchor():
                rows = initial.query(initial.handles[0],
                    "SELECT a.pid, split_part(l.virtualtransaction,'/',1)::int "
                    "FROM pg_stat_activity a JOIN pg_locks l USING(pid) "
                    f"WHERE a.application_name='{app}' AND l.locktype='virtualxid' AND l.granted")
                assert len(rows) <= 1, f"ambiguous validation anchor: {rows}"
                return list(map(int, rows[0])) if rows else None
            anchor_pid, anchor_id = wait_until(daemon, validation_anchor, 90,
                "actual validation anchor was not observed; far-row prerequisite unproven", poll_seconds=.01)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90, "PG13 control socket not ready")
            wait_until(daemon, lambda: metrics(trace).get("sampler_ticks_total", 0) > 0,
                       30, "PG13 sampler not ready")
            status = metrics(trace, "status")
            require_pg13_anchor_status(status, negative=negative)
            assert metrics(trace)["exact_probe_attached_mask"] == 0, "timing arm must stay sampled"
            # No far slot is retired here. Retire only those closest to the
            # measured anchor, leaving the low slots available for far churn.
            retired = sorted(initial.handles,
                             key=lambda h: abs(initial_rows[h]["backend_id"] - anchor_id))[:PG13_RETIRE]
            retired_pids = [initial_rows[handle]["pid"] for handle in retired]
            for handle in retired:
                initial.close_handle(handle)
            wait_until(daemon, lambda: all(not Path(f"/proc/{pid}").exists() for pid in retired_pids),
                       15, "near-anchor setup sessions did not retire")
            spanning_before = metrics(trace)
            perf_path = trace / "perf.trace"
            # An independently observed set of possible wait readers, not a
            # PID inferred from whichever syscall we want to exempt.
            wait_pids = {int(row[0]) for row in initial.query(initial.handles[0],
                         "SELECT pid FROM pg_stat_activity WHERE pid IS NOT NULL")}
            wait_pids.discard(pm_pid)
            # One symbol-entry uprobe per tick, not a stack unwind or a product
            # change. Unique group; no force/overwrite of another run's event.
            event = f"pgwt866_{os.getpid()}_{daemon.pid}:tick"
            subprocess.run(["perf", "probe", "-x", str(binary), "--max-probes=1",
                            "--add", event + "=pgwt_sampler_poll"],
                           check=True, stdout=perr, stderr=subprocess.STDOUT)
            tick_event = event  # delete only after a successful registration
            # Decode the pipe live: actual syscall lines prove attachment.
            # Explicit entry/exit tracepoints retain tid and event timestamp;
            # perf trace's pretty-printer can overwrite a pending entry on EL8.
            perf = spawn(["perf", "record", "-q", "-m", str(PG13_PERF_PAGES),
                          "-p", str(daemon.pid), "-e", VM_EVENTS + "," + tick_event, "-o", "-"],
                         stdout=subprocess.PIPE, stderr=perr)
            decoder = spawn(["perf", "script", "-i", "-", "--ns", "--show-lost-events",
                             "-F", "tid,time,event,trace"],
                            stdin=perf.stdout, stdout=perf_out, stderr=script_err)
            perf.stdout.close()
            wait_until(perf, lambda: tick_event + ":" in perf_path.read_text(),
                       15, "PG13 perf produced no tick marker")
            before = metrics(trace)
            later.open(PG13_LATER, "pgwt866_far_side")
            later_rows = [session_evidence(later, h) for h in later.handles]
            assert min(row["backend_id"] for row in initial_rows.values()) < anchor_id < \
                   max(row["backend_id"] for row in later_rows), "anchor is not inside the row population"
            held = len(initial.handles) + len(later.handles)
            population = wait_until(daemon, lambda: (m if held <= (m := metrics(trace))["sample_read_targets"]
                                                      < 1024 else None),
                                    30, "held population not enrolled below the 1024-process cap")
            far_handle = max(initial.handles,
                             key=lambda h: abs(initial_rows[h]["backend_id"] - anchor_id))
            far_row = initial_rows[far_handle]
            assert abs(far_row["backend_id"] - anchor_id) > 513, "setup has no row farther than 513"
            initial.close_handle(far_handle)
            wait_until(daemon, lambda: not Path(f"/proc/{far_row['pid']}").exists(),
                       15, "far slot did not retire before churn")
            start = time.monotonic()
            for index in range(PG13_CHURN):
                # Fixed workload pacing spreads 200 connections over >=20 s;
                # neither session duration nor timing bounds adapt to results.
                delay = start + index * .1 - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                churn.open(1, "pgwt866_short")
                handle = churn.handles[-1]
                try:
                    row = session_evidence(churn, handle)
                    row["distance"] = abs(row["backend_id"] - anchor_id)
                    assert row["distance"] > 513, f"churn far-row prerequisite failed: {row}, anchor={anchor_id}"
                    cohort.append(row)
                    churn.query(handle, "SELECT pg_sleep(0.05)")
                finally:
                    churn.close_handle(handle)
                wait_until(daemon, lambda: not Path(f"/proc/{row['pid']}").exists(),
                           15, "short session did not exit", poll_seconds=.001)
                assert perf.poll() is None, "PG13 perf stopped during churn"
            # Start the quiet window only after the last exit is reflected in
            # enrollment. This never changes the already recorded churn timing.
            wait_until(daemon, lambda: metrics(trace)["sample_read_targets"] ==
                       population["sample_read_targets"] - 1, 15, "churn enrollment did not retire")
            quiet_before = metrics(trace)
            quiet_start = time.monotonic()
            time.sleep(5)  # the specified no-churn window
            quiet_after = metrics(trace)
            quiet_seconds = time.monotonic() - quiet_start
            quiet_ticks = quiet_after["sampler_ticks_total"] - quiet_before["sampler_ticks_total"]
            assert quiet_ticks > 0, "sampler did not advance in the quiet window"
            status = metrics(trace, "status")
            stop(perf, signal.SIGINT)
            assert perf.returncode in (0, -signal.SIGINT), (trace / "perf.stderr").read_text()
            decoder.wait(timeout=15)
            assert decoder.returncode == 0, (trace / "perf.script.stderr").read_text()
            spanning_after = metrics(trace)
            text = perf_path.read_text()
            perf_errors = ((trace / "perf.stderr").read_text(),
                           (trace / "perf.script.stderr").read_text())
            # The pre-fix control retains lossless parsing, but no syscall gate
            # can substitute for its required first-attribution timing RED.
            syscall = pg13_vm_diagnostics(text, perf_errors) if negative else None
            if not negative:
                syscall = assess_pg13_vm_calls(text, pm_pid, perf_errors, wait_pids=wait_pids,
                    tick_event=tick_event,
                    ticks_inside=quiet_after["sampler_ticks_total"] - before["sampler_ticks_total"],
                    ticks_spanning=spanning_after["sampler_ticks_total"] - spanning_before["sampler_ticks_total"],
                    read_faults=spanning_after["sample_read_faults_total"] - spanning_before["sample_read_faults_total"])
            # Finalization flushes the raw samples, including the LAST session.
            stop(daemon)
            samples = read_cohort_samples(reader, trace, cohort)
            result = assess_pg13_timing(cohort, samples, 1_000_000_000 // PG13_RATE)
            quiet_failures = (quiet_after["sampled_attr_tick_read_failures_total"] -
                              quiet_before["sampled_attr_tick_read_failures_total"])
            fallback_key = "sampled_attr_pg13_scan_fallbacks_total"
            fallback = (quiet_after[fallback_key] - before[fallback_key]
                        if fallback_key in quiet_after and fallback_key in before else None)
            errors = []
            if quiet_failures != 0:
                errors.append(f"quiet read failures={quiet_failures} (required zero over 5 s)")
            if status.get("sampled_attr_pg13_route") != "computed":
                errors.append(f"PG13 route={status.get('sampled_attr_pg13_route')} (required computed)")
            if fallback != 0:
                errors.append(f"PG13 scan-fallback delta={fallback} (required zero)")
            result.update(label=label, binary=str(binary),
                          binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                          anchor_pid=anchor_pid, anchor_backend_id=anchor_id,
                          initial_sessions=PG13_INITIAL, near_anchor_retired=PG13_RETIRE,
                          later_sessions=PG13_LATER, population=population["sample_read_targets"],
                          minimum_churn_distance=min(row["distance"] for row in cohort),
                          quiet_seconds=quiet_seconds, quiet_ticks=quiet_ticks, quiet_read_failures=quiet_failures,
                          scan_fallback_delta=fallback, route=status.get("sampled_attr_pg13_route"),
                          syscall=syscall, other_errors=errors)
            (trace / "churn.starts.json").write_text(json.dumps(cohort, indent=2) + "\n")
            (trace / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, sort_keys=True), flush=True)
            return result
        finally:
            try:
                stop(perf, signal.SIGINT)
            finally:
                try:
                    stop(decoder)
                finally:
                    try:
                        stop(daemon)
                    finally:
                        try:
                            churn.close()
                            later.close()
                            initial.close()
                        finally:
                            if tick_event:
                                subprocess.run(["perf", "probe", "--del", tick_event], check=True,
                                               stdout=perr, stderr=subprocess.STDOUT)


def run_escalation_arm(binary, pm_pid, directory):
    """Fork a named cohort while exact admission is open; prove each resumes."""
    trace = directory / "escalation-born"
    trace.mkdir()
    daemon = None
    cohort = IdleConnections()
    with (trace / "daemon.stderr").open("w") as err:
        try:
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            env["PGWT_DEBUG_SAMPLED_ATTR_SHADOW"] = "1"
            daemon = spawn(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", "10",
                 "--escalation-budget", "120", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90, "escalation socket not ready")
            wait_until(daemon, lambda: metrics(trace).get("sampler_ticks_total", 0) > 0,
                       30, "escalation sampler not ready")
            control(trace, {"cmd": "escalate", "duration_s": 90, "reason": "manual"}, timeout=30)
            before = wait_until(daemon, lambda: (m if (m := metrics(trace)).get("tier") == "escalated"
                                  and m.get("exact_probe_attached_mask", 0) & 3 == 3 else None),
                                30, "exact window did not open")
            cohort.open(4, "pgwt866_escalation_born")
            pids = cohort.pids()
            assert len(pids) == 4 and all(pid > 0 for pid in pids), pids
            wait_until(daemon, lambda: metrics(trace)["backends_tracked"] >= before["backends_tracked"] + 4,
                       30, "escalation-born cohort never entered registry")
            assert metrics(trace)["tier"] == "escalated", "cohort was not forked inside exact window"
            control(trace, {"cmd": "deescalate"}, timeout=30)
            sampled = wait_until(daemon, lambda: (m if (m := metrics(trace)).get("tier") == "sampled"
                                   and m.get("exact_probe_generation") == 0 else None),
                                 30, "de-escalation did not finish")
            offset = (trace / "daemon.stderr").stat().st_size
            def observed():
                text = (trace / "daemon.stderr").read_text()[offset:]
                return pids <= {int(pid) for pid in re.findall(
                    r"SAMPLED-ATTR-IDLE: pid=(\d+) databaseid=[1-9]\d* userid=[1-9]\d*", text)}
            wait_until(daemon, observed, 15, "escalation-born cohort lacks attributed samples after de-escalation")
            assert metrics(trace)["sampled_attr_tick_idle_samples_total"] > sampled["sampled_attr_tick_idle_samples_total"]
            print(f"escalation-born cohort: PASS pids={sorted(pids)} attributed after de-escalation", flush=True)
        finally:
            try:
                stop(daemon)
            finally:
                cohort.close()


def run_late_promotion_arm(binary, pm_pid, directory):
    """Existing sessions are scanned while the layout is degraded. The existing
    validation-failure hook forces the auth-free warmup to be the promoter.
    Both varying query ids and active/idle states are present before startup.
    """
    trace = directory / "late-promotion"
    trace.mkdir()
    script = trace / "vary.sql"
    script.write_text("\\set shape random(0, 1)\n\\if :shape\n"
                      "SELECT pg_sleep(0.02);\n\\else\n"
                      "SELECT pg_sleep(0.02), 42;\n\\endif\n\\sleep 20 ms\n")
    daemon = load = None
    cohort = IdleConnections()
    with (trace / "daemon.stderr").open("w") as err, \
         (trace / "pgbench.stdout").open("w") as out, \
         (trace / "pgbench.stderr").open("w") as load_err:
        try:
            cohort.open(4, "pgwt866_before_promotion")
            pids = cohort.pids()
            assert len(pids) == 4 and all(pid > 0 for pid in pids), pids
            load = spawn(
                ["pgbench", "-n", "-U", "postgres", "-c", "4", "-j", "4",
                 "-T", "120", "-f", str(script), "postgres"], stdout=out, stderr=load_err,
                env={**os.environ, "PGAPPNAME": "pgwt866_promotion_load"})
            wait_until(load, lambda: int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                            "application_name='pgwt866_promotion_load'")) == 4,
                       30, "promotion workload must precede daemon startup")
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            env.update(PGWT_TEST_PGBS_VALIDATION_FAIL="1", PGWT_DEBUG_SAMPLED_ATTR_SHADOW="1")
            daemon = spawn(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", "10",
                 "--escalation-budget", "0", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90,
                       "late-promotion control socket not ready")
            def observed():
                text = (trace / "daemon.stderr").read_text()
                return pids <= {int(pid) for pid in re.findall(
                    r"SAMPLED-ATTR-IDLE: pid=(\d+) databaseid=[1-9]\d* userid=[1-9]\d*", text)}
            wait_until(daemon, observed, 15, "pre-promotion cohort lacks attributed samples")
            layout = metrics(trace, "status")
            text = (trace / "daemon.stderr").read_text()
            assert "forced validation failure (test hook)" in text, text
            assert "bounded uprobe shadow validation completed" in text, text
            assert layout["pgbackend_layout_validation"] == "validated", layout
            assert load.poll() is None, "promotion workload stopped before verification"
            print(f"late layout promotion: PASS pre-existing pids={sorted(pids)} attributed", flush=True)
        finally:
            try:
                stop(daemon)
            finally:
                try:
                    stop(load, signal.SIGINT)
                finally:
                    cohort.close()


def run_arm(binary, pm_pid, major, directory, label, disable_status_batch=False):
    trace = directory / label
    trace.mkdir()
    perf = daemon = None
    with (trace / "daemon.stderr").open("w") as err, (trace / "perf.stderr").open("w") as perr:
        try:
            # Do not let another test's injection alter this production arm.
            env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
            if disable_status_batch:
                env["PGWT_TEST_DISABLE_STATUS_BATCH"] = "1"
            daemon = spawn(
                [str(binary), "--pid", str(pm_pid), "--mode", "tiered", "--sample-rate", "10",
                 "--escalation-budget", "0", "-i", "1", "-q", "-T", str(trace)],
                stdout=subprocess.DEVNULL, stderr=err, env=env)
            wait_until(daemon, lambda: (trace / "pgwt.sock").exists(), 90, "control socket not ready")
            def enrolled():
                value = metrics(trace)
                return value if value.get("sample_read_targets", 0) >= 504 else None
            wait_until(daemon, enrolled, 30, "504 sessions not enrolled")
            # Warm address resolution, metadata and text resolver before perf.
            warm = metrics(trace)["sampler_ticks_total"]
            wait_until(daemon, lambda: metrics(trace)["sampler_ticks_total"] >= warm + 50,
                       30, "warmup did not advance")
            # Sampler ticks bracket perf from both sides. `spanning` starts before
            # perf is launched and ends after it exited, so it contains every tick
            # perf could have seen; `inside` runs between two reads that both fall
            # inside perf's window. Calls are divided by the count that cannot
            # redden a bound by window misalignment (see assess_counts).
            spanning_start = metrics(trace)["sampler_ticks_total"]
            perf_path = trace / "perf.trace"
            # -S: the events as they happen (their arrival proves perf is attached)
            # and, at the end, the per-syscall summary that is the evidence. The
            # summary needs no path names, which neither gate image can resolve.
            perf = spawn(
                ["perf", "trace", "-S", "--no-comm", "-m", "1024", "-p", str(daemon.pid),
                 "-e", CALLS, "-o", str(perf_path)],
                stdout=subprocess.DEVNULL, stderr=perr)
            # Require actual trace data before opening the measured window.
            wait_until(perf, lambda: perf_path.exists() and perf_path.stat().st_size > 0,
                       15, "perf produced no trace")
            before = metrics(trace)
            target_ticks = before["sampler_ticks_total"] + 210
            after = wait_until(daemon, lambda: (m if (m := metrics(trace))["sampler_ticks_total"]
                                               >= target_ticks else None),
                               90, "fewer than 210 sampler ticks")
            assert perf.poll() is None, "perf exited during measurement"
            time.sleep(1)  # drain exits for syscalls entered inside the window
            stop(perf, signal.SIGINT)
            assert perf.returncode in (0, -signal.SIGINT), (trace / "perf.stderr").read_text()
            spanning_end = metrics(trace)["sampler_ticks_total"]
            ticks = after["sampler_ticks_total"] - before["sampler_ticks_total"]
            spanning = spanning_end - spanning_start
            population = min(before["sample_read_targets"], after["sample_read_targets"])
            assert population >= 504, "500 idle + 4 active targets must remain enrolled"
            failures = (after["sampled_attr_tick_read_failures_total"] -
                        before["sampled_attr_tick_read_failures_total"])
            idle_samples = (after.get("sampled_attr_tick_idle_samples_total", 0) -
                            before.get("sampled_attr_tick_idle_samples_total", 0))
            assert before["sample_read_targets"] == after["sample_read_targets"], "population changed"
            assert idle_samples >= 0, "idle success counter moved backwards"
            # Even if EVERY other target were an idle client on EVERY tick,
            # successes above that ceiling must come from our 500 idle pids.
            # This proves cohort success without per-pid logging under perf.
            cohort_idle_samples = max(0, idle_samples - (population - 500) * ticks)
            layout = metrics(trace, "status")
            assert layout["pgbackend_layout_validation"] == "validated", layout
            counts = parse_summary(perf_path.read_text(), (str((trace / "perf.stderr").read_text()),))
            # Use the guaranteed cohort N, excluding version-dependent aux
            # populations with no attribution row (notably PG13).
            result = assess_counts(counts, major, 504, ticks, spanning, failures, cohort_idle_samples)
            result.update(label=label, major=major, read_failures=failures, binary=str(binary),
                          binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                          population=population, ticks=ticks, ticks_spanning=spanning,
                          calls=counts, idle_samples_total=idle_samples,
                          idle_cohort_samples_lower_bound=cohort_idle_samples,
                          shadow_delta={key: after[key] - before[key] for key in SHADOW})
            (trace / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, sort_keys=True), flush=True)
            return result
        finally:
            try:
                stop(perf, signal.SIGINT)
            finally:
                stop(daemon)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--master-tracer", type=Path, default=os.environ.get("PGWT_866_MASTER_TRACER") or None)
    parser.add_argument("--require-negative-control", action="store_true")
    parser.add_argument("--require-huge-pages", action="store_true")
    parser.add_argument("--evidence-dir", type=Path, help="new directory outside the repository")
    arms = parser.add_mutually_exclusive_group()
    arms.add_argument("--pg13-far-rows", action="store_true",
                        help="select only the PG13 far-row arm; --master-tracer is the 0b1d7dd6 tracer")
    arms.add_argument("--syscalls-only", action="store_true", help="select only the original syscall gate")
    parser.add_argument("--dry-run", action="store_true", help="print the plan without Linux, PG or libpq")
    parser.add_argument("--pg-version", type=int, choices=range(13, 19), help="PG major for --dry-run")
    args = parser.parse_args(argv)
    if args.require_negative_control and not args.master_tracer:
        parser.error("--require-negative-control needs --master-tracer")
    if args.pg_version is not None and not args.dry_run:
        parser.error("--pg-version is only for --dry-run (live version comes from the runner)")
    if args.pg13_far_rows and args.pg_version not in (None, 13):
        parser.error("--pg13-far-rows needs PG13")
    return args


def run_pg13_arms(args, pid, directory):
    reader = build_sample_reader(directory)
    candidate = run_pg13_far_arm(ROOT / "pg_wait_tracer", pid, directory, reader, "pg13-far-candidate")
    assert not candidate["timing_errors"] and not candidate["other_errors"], candidate
    if args.pg13_far_rows and args.master_tracer:
        negative = run_pg13_far_arm(args.master_tracer.resolve(), pid, directory, reader,
                                   "pg13-far-pre-fix", negative=True)
        print(require_pg13_negative_control(negative), flush=True)
    else:
        print("PENDING: PG13 timing negative control; select --pg13-far-rows "
              "--master-tracer /path/to/0b1d7dd6/pg_wait_tracer --require-negative-control", flush=True)
    print("PG13 far-row candidate: PASS (200 short sessions, <=2 tick periods, quiet failures=0)", flush=True)


def main():
    args = parse_args()
    if args.dry_run:
        major = args.pg_version or 13
        print(json.dumps({"major": major, "excluded": major not in SUPPORTED_MAJORS,
              "arms": ([] if major not in SUPPORTED_MAJORS else ["pg13-far-rows"] if args.pg13_far_rows else
                       (["pg13-far-rows", "syscalls"] if major == 13 and not args.syscalls_only else ["syscalls"])),
              "max_connections": 1100 if major == 13 else 540,
              "pg13": {"initial_idle": PG13_INITIAL, "retire_near_anchor": PG13_RETIRE,
                       "later_idle": PG13_LATER, "short_sessions": PG13_CHURN,
                       "sleep_s": .05, "sample_rate": PG13_RATE, "minimum_row_distance": 514,
                       "first_attribution_bound_ns": 2_000_000_000 // PG13_RATE,
                       "quiet_window_s": 5, "negative_control_revision": "0b1d7dd6",
                       "perf_pages": PG13_PERF_PAGES, "perf_events": VM_EVENTS,
                       "perf_tick_marker": "pgwt_sampler_poll symbol-entry uprobe"},
              "master_tracer": str(args.master_tracer) if args.master_tracer else None,
              "require_negative_control": args.require_negative_control}, sort_keys=True))
        return
    assert sys.platform == "linux" and os.geteuid() == 0, "Linux/root live test only"
    source_pid = args.pid or find_postmaster()
    assert source_pid, "no runner postmaster from which to select the PG binary"
    version = subprocess.check_output([os.readlink(f"/proc/{source_pid}/exe"), "--version"], text=True)
    major = int(re.search(r"PostgreSQL\)\s+(\d+)", version)[1])
    if major not in SUPPORTED_MAJORS:
        print(f"EXCLUDED: PG{major}; status batch needs PostgreSQL 13, 17 or 18")
        return
    assert not args.pg13_far_rows or major == 13, "--pg13-far-rows needs PG13"
    assert not (major == 13 and args.require_negative_control and not (args.pg13_far_rows or args.syscalls_only)), \
        "select --pg13-far-rows for the required 0b1d7dd6 control; run the syscall/master control separately"
    idle = IdleConnections()
    load = None
    # Deliberately not tests/results/: no product/repository artifacts.
    if args.evidence_dir:
        args.evidence_dir = args.evidence_dir.resolve()
        assert args.evidence_dir != ROOT and ROOT not in args.evidence_dir.parents, \
            "evidence must stay outside the repository"
        args.evidence_dir.mkdir(parents=True, exist_ok=False)
    evidence_context = (contextlib.nullcontext(str(args.evidence_dir)) if args.evidence_dir else
                        tempfile.TemporaryDirectory(prefix="pgwt866-live-"))
    with evidence_context as tmp, private_cluster(source_pid, args.require_huge_pages) as (pid, major):
        huge_pages = psql("SHOW huge_pages")
        print(f"private PG{major} pid={pid} kernel={os.uname().release} "
              f"max_connections={psql('SHOW max_connections')} huge_pages={huge_pages}", flush=True)
        directory = Path(tmp)
        if major == 13 and not args.syscalls_only:
            run_pg13_arms(args, pid, directory)
            if args.pg13_far_rows:
                return
        script = directory / "workload.sql"
        script.write_text("SELECT sum(i) FROM generate_series(1, 10000) AS i;\n")
        try:
            run_escalation_arm(ROOT / "pg_wait_tracer", pid, directory)
            if major >= 17:
                run_late_promotion_arm(ROOT / "pg_wait_tracer", pid, directory)
            else:
                # The existing hook returns before PG13's activity-buffer
                # size is discovered. Warmup cannot enable its tick source
                # with that size unset; the injected registry test covers
                # late promotion for PG13 without changing validation rules.
                print("PG13 late promotion: unit-covered; forced validation failure "
                      "precedes activity-buffer size discovery", flush=True)
            idle.open()
            assert int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                            "application_name='pgwt866_idle' AND state='idle'")) == 500
            with (directory / "pgbench.stdout").open("w") as out, (directory / "pgbench.stderr").open("w") as err:
                load = spawn(
                    ["pgbench", "-n", "-U", "postgres", "-c", "4", "-j", "4",
                     "-R", "100", "-T", "600", "-f", str(script), "postgres"], stdout=out, stderr=err,
                    env={**os.environ, "PGAPPNAME": "pgwt866_active"})
                wait_until(load, lambda: int(psql("SELECT count(*) FROM pg_stat_activity WHERE "
                               "application_name='pgwt866_active'")) == 4,
                           30, "four pgbench sessions not ready")
                candidate = run_arm(ROOT / "pg_wait_tracer", pid, major, directory, "candidate")
                assert not candidate["errors"], candidate["errors"]
                disabled = run_arm(ROOT / "pg_wait_tracer", pid, major, directory,
                                   "status-disabled-negative", disable_status_batch=True)
                # With the batch disabled no row is read, so every unread backend keeps
                # retrying its address resolution: measured 16.7 process_vm_readv per
                # tick (EL8, PG13), above the upper bound rather than below the floor.
                # Either direction is a RED at the process_vm_readv assertion; the
                # floor itself is proven live by the master arm (1 per tick).
                assert any(e.startswith("vm/tick=") for e in disabled["errors"]), disabled
                assert any("idle attributed samples=0" in e for e in disabled["errors"]), disabled
                print("status batch disabled: RED at the syscall assertion and the attribution floor: "
                      + "; ".join(disabled["errors"]), flush=True)
                if args.master_tracer:
                    master = run_arm(args.master_tracer.resolve(), pid, major, directory, "master-negative")
                    # The per-backend file I/O assertion itself must fire, not just
                    # some other bound.
                    file_io = file_io_reds(master["errors"])
                    assert file_io, ("master negative control did not redden the per-backend "
                                     f"file I/O assertion: {master['errors']} per_tick={master['per_tick']}")
                    for key in SHADOW[1:]:
                        assert candidate["shadow_delta"][key] <= master["shadow_delta"][key], \
                            (key, candidate["shadow_delta"], master["shadow_delta"])
                    print("master negative control: RED at " + "; ".join(master["errors"]) +
                          "; shadow deltas no worse")
                    if not master["shadow_delta"][SHADOW[0]]:
                        print("shadow comparison has zero observations (no attribute uprobes in baseline)")
                else:
                    print("PENDING: lead-run master negative control; pass --master-tracer")
                assert load.poll() is None, "pgbench stopped before measurement completed"
                # SIGINT kills pgbench without its report; SIGALRM is the signal -T
                # itself uses, so it ends the run normally and prints the summary.
                stop(load, signal.SIGALRM)
                workload = (directory / "pgbench.stdout").read_text()
                assert re.search(r"number of transactions actually processed:\s*[1-9]", workload), workload
                print("status-row syscall candidate gate: PASS (>=200 ticks, 500 idle + 4 pgbench)")
        finally:
            try:
                stop(load, signal.SIGINT)
            finally:
                idle.close()


if __name__ == "__main__":
    main()
