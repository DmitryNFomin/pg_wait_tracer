#!/usr/bin/env python3
"""test_capture_smoke.py — end-to-end capture smoke test (Phase T0: TST-1/2).

The one test whose entire purpose is: if the tracer silently records
NOTHING from a real PostgreSQL, this must go red. All four field escapes
(#8, #24, #30, #31) lived in the capture/discovery slice that no CI job
executed; this test (driven by tests/ci_smoke.sh in the CI capture-smoke
job) closes that hole.

Deterministic workload, asserted with bounded values:
  1. pg_sleep(3)              -> Timeout:PgSleep, total within tolerance
  2. blocked LOCK TABLE pair  -> Lock:relation wait, bounded duration
  3. short pgbench run        -> query_event view shows query_ids that
                                 cross-check against pg_stat_statements

Wait events are asserted in BOTH:
  - the live view output (--view system_event / query_event), and
  - the written trace file, read back through pgwt-server (the same JSON
    protocol the web UI uses; --replay is NOT used because replay of
    tiered traces is fidelity-broken until T1/FID-5).

Regression coverage (referenced by PR number, per the Trust Milestone):
  - PR #30: in sampled/tiered mode the sampler must feed the live
    accumulator — the tiered live-view assertions here are empty if that
    regresses.
  - PR #31: the query-attribution uprobe must actually fire (on PG13 it
    must probe standard_ExecutorStart) — the query-attribution assertions
    here go empty if it regresses. The assertion cross-checks captured
    query_ids against pg_stat_statements.queryid, so junk/phantom ids
    (e.g. marker leakage, FID-4) cannot satisfy it.
  - PR #24 (load base) would make this whole test capture zero events on
    non-PIE layouts; the unit-level guard is tests/test_discovery*.

Environment requirements (ci.yml configures both; fail loudly otherwise):
  - pg_stat_statements in shared_preload_libraries (PG13: required for
    query attribution at all; PG14+: activates compute_query_id=auto).
  - pgbench + psql for the workload, connecting as "postgres" via local
    trust auth.

Usage: sudo python3 tests/test_capture_smoke.py --mode {full,tiered}
           [--pid POSTMASTER_PID]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")
SERVER = os.path.join(PROJECT_DIR, "pgwt-server")
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


def psql(sql, timeout=15):
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
        # T8: a still-running pure-CPU DO backend (active, so not caught above)
        # would keep burning a core and contaminate later phases' CPU — kill it.
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE 'DO %'")
    except subprocess.TimeoutExpired:
        pass
    time.sleep(1)


def pgss_query_ids():
    """query_ids (as unsigned 64-bit) that pg_stat_statements recorded for
    the pgbench workload — the ground truth for attribution asserts."""
    out = psql("SELECT queryid FROM pg_stat_statements "
               "WHERE queryid IS NOT NULL AND calls >= 3")
    ids = set()
    for line in out.split('\n'):
        line = line.strip()
        if re.fullmatch(r'-?\d+', line):
            ids.add(int(line) & 0xFFFFFFFFFFFFFFFF)
    return ids


# ── deterministic workload ─────────────────────────────────────────

class Workload:
    """Three psql sessions held open on stdin pipes:
         sleeper — runs SELECT pg_sleep(3) when fire()d, then sits idle
                   (idle = Client:ClientRead, which is hidden — so the
                   session's PgSleep total stays exactly ~3s; an extra
                   keepalive pg_sleep would contaminate the assertion)
         holder  — BEGIN; LOCK TABLE ... ACCESS EXCLUSIVE (idle-in-txn,
                   holds the lock for the whole phase)
         waiter  — SELECT on the table when fire()d -> blocks on
                   Lock:relation until the phase ends

    In phases 1-3, sessions are opened BEFORE the tracer starts (the
    initial backend scan finds them — same technique as
    test_deterministic.py). Phase 4 (fork-after-attach) opens them AFTER
    the tracer attaches: backends forked post-attach used to be subject to
    the fork->attach race (bootstrap watchpoint armed after the child had
    already written its pointer -> never fires -> zero events, silently;
    observed live on Ubuntu 24.04/kernel 6.8 by the T0 CI run, fixed in T4
    by re-checking the pointer right after arming). The waits are fired
    AFTER the tracer attaches so their durations are fully observed.
    release() commits the holder so the lock wait ENDS inside the
    observation window — in full mode a transition is only written to the
    trace when the wait ends, so a never-ending lock wait would be absent
    from the trace file (the ESC-2/FID-class open-interval gap, owned by
    T1/T3)."""

    LOCK_TABLE = "_smoke_lock_wait"

    def __init__(self):
        self.sleeper = None
        self.holder = None
        self.waiter = None

    def _session(self):
        return subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True)

    def open_sessions(self):
        psql(f"CREATE TABLE IF NOT EXISTS {self.LOCK_TABLE} (id int)")
        self.sleeper = self._session()
        self.holder = self._session()
        self.waiter = self._session()
        self.holder.stdin.write(
            f"BEGIN; LOCK TABLE {self.LOCK_TABLE} IN ACCESS EXCLUSIVE MODE;\n")
        self.holder.stdin.flush()
        time.sleep(1.5)   # sessions connected, lock held
        held = psql(
            f"SELECT count(*) FROM pg_locks "
            f"WHERE relation = '{self.LOCK_TABLE}'::regclass AND granted")
        check(held != "" and int(held) >= 1,
              f"workload: holder acquired AccessExclusiveLock (held={held!r})")

    def fire(self, sleep_s=3):
        """Start the observable waits (call after the tracer attached)."""
        self.sleeper.stdin.write(f"SELECT pg_sleep({sleep_s});\n")
        self.sleeper.stdin.flush()
        self.waiter.stdin.write(f"SELECT count(*) FROM {self.LOCK_TABLE};\n")
        self.waiter.stdin.flush()
        time.sleep(1.5)
        waiters = psql(
            f"SELECT count(*) FROM pg_locks "
            f"WHERE relation = '{self.LOCK_TABLE}'::regclass AND NOT granted")
        check(waiters != "" and int(waiters) >= 1,
              f"workload: waiter blocked on {self.LOCK_TABLE} (waiters={waiters!r})")

    def release(self):
        """Commit the holder -> the waiter's lock wait ends (and gets a
        transition record in full mode)."""
        self.holder.stdin.write("COMMIT;\n")
        self.holder.stdin.flush()
        time.sleep(0.5)

    def stop(self):
        for p in (self.sleeper, self.holder, self.waiter):
            if p is None:
                continue
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        self.sleeper = self.holder = self.waiter = None
        try:
            psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                 "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
                 f"AND query LIKE '%{self.LOCK_TABLE}%'")
            time.sleep(1)
            psql(f"DROP TABLE IF EXISTS {self.LOCK_TABLE}")
        except subprocess.TimeoutExpired:
            pass


# ── output parsers (same formats test_deterministic/test_query_event use) ──

def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        m = re.match(
            r'^(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+|—)%?',
            line.strip())
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
            })
    return events


def parse_query_event_ids(output):
    ids = set()
    for line in output.split('\n'):
        m = re.match(r'^(-?\d+)\s+(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s',
                     line.strip())
        if m:
            qid = int(m.group(1))
            if qid != 0:
                ids.add(qid & 0xFFFFFFFFFFFFFFFF)
    return ids


def run_tracer(args, timeout):
    proc = subprocess.Popen([TRACER] + args,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    return out, err


# ── pgwt-server trace-file reader ──────────────────────────────────

def server_query(trace_dir, cmd, extra=None):
    """One-shot pgwt-server JSON query against a trace dir."""
    proc = subprocess.Popen([SERVER, trace_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    try:
        proc.stdin.write(json.dumps({"id": 1, "cmd": "info"}) + "\n")
        proc.stdin.flush()
        info = json.loads(proc.stdout.readline())
        req = {"id": 2, "cmd": cmd,
               "from": max(0, info.get("from_ns", 0) - 30_000_000_000),
               "to": info.get("to_ns", 0) + 30_000_000_000}
        if extra:
            req.update(extra)
        proc.stdin.write(json.dumps(req) + "\n")
        proc.stdin.flush()
        resp = json.loads(proc.stdout.readline())
    finally:
        proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return resp


def parse_time_model(output):
    """Parse the tracer's live time_model text output → {row_name: ms}. The
    last occurrence of each row wins (later display snapshots overwrite
    earlier), so the returned dict is the most-recent live view."""
    model = {}
    for line in STRIP_ANSI.sub('', output).split('\n'):
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line.strip())
        if m:
            model[m.group(1).strip()] = float(m.group(2))
    return model


def proc_oncpu_ns(pid):
    """Total on-CPU nanoseconds (utime+stime) of a pid from /proc/<pid>/stat —
    the same quantity the tracer measures via se.sum_exec_runtime. utime is the
    14th field and stime the 15th; the comm field (2nd) may contain spaces and
    parens, so index from the LAST ')'. Returns None if the backend is gone."""
    try:
        with open("/proc/%d/stat" % pid, "rb") as f:
            data = f.read().decode("latin-1")
    except (FileNotFoundError, ProcessLookupError, TypeError):
        return None
    rp = data.rfind(')')
    if rp < 0:
        return None
    fields = data[rp + 2:].split()
    try:
        utime, stime = int(fields[11]), int(fields[12])
    except (IndexError, ValueError):
        return None
    return (utime + stime) * 1_000_000_000 // os.sysconf("SC_CLK_TCK")


def find_active_do_backend():
    """PID of the client backend running the pure-CPU DO block (state=active).
    None if not found (the loop may not have reached 'active' yet)."""
    out = psql("SELECT pid FROM pg_stat_activity WHERE state='active' "
               "AND backend_type='client backend' AND query LIKE 'DO %' "
               "AND pid <> pg_backend_pid() ORDER BY query_start LIMIT 1")
    out = (out or "").strip()
    return int(out) if out.isdigit() else None


# ── shared assertions ──────────────────────────────────────────────

def assert_wait_events(events, source, sleep_hi=6000, core=False):
    """Deterministic-wait assertions, applied to live view or trace rows.

    sleep_hi: 6000 for every caller. Before T3, tiered live callers had to pass
    7000 because the live accumulator was fed by BOTH the sampler and the exact
    stream during an escalation window, inflating a 3s sleep up to ~5.7s (the
    ESC-3 double-count). T3 gates the sampler's contribution for pids under a
    live watchpoint (pgwt_sampler_accumulate), so the live view now matches the
    post-hoc trace view within the same tolerance — this tightened bound is the
    ESC-3 regression proof.

    core: the container capture-smoke (nightly) proves the capture PATH works
    on each distro but runs where hardware watchpoints do not actually fire —
    a single 3s pg_sleep is then sampled-only (~300ms), never escalated to
    exact. In core mode the pg_sleep magnitude floor is relaxed to
    presence-only; the exact-duration gate is validated on the T0 hosted
    runner and the live EL8/EL9 boxes, not here."""
    names = [e['name'] for e in events]

    sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']
    check(len(sleep_ev) > 0, f"{source}: Timeout:PgSleep present (saw: {names})")
    if sleep_ev:
        total = sleep_ev[0]['total_ms']
        # One pg_sleep(3), fired after attach, session idle afterwards.
        # Sampled tier quantizes at the sample period; bounded either way:
        # zero and wildly-wrong both fail. core mode drops the exact-tier
        # floor (unreachable without firing watchpoints) but still catches a
        # wildly-wrong total.
        lo = 100 if core else 1200
        hi = 30000 if core else sleep_hi
        check(lo <= total <= hi,
              f"{source}: PgSleep total = {total:.0f}ms "
              f"(expect {'present, sampled' if core else '~3000'} ±tol)")

    lock_ev = [e for e in events if e['name'] == 'Lock:relation']
    check(len(lock_ev) > 0, f"{source}: Lock:relation present (saw: {names})")
    if lock_ev:
        total = lock_ev[0]['total_ms']
        # The waiter blocks from fire() until the phase ends (>= ~8s of
        # observation); open-interval accounting reports it at tick time.
        # core mode drops the floor: without firing watchpoints the trace holds
        # only sparse SAMPLES of the wait (observed ~300ms on EL8/PG13), so the
        # exact >= 4000 floor is unreachable — presence is the portable gate.
        lock_lo = 100 if core else 4000
        check(total >= lock_lo, f"{source}: Lock:relation total = {total:.0f}ms "
              f"(expect >= {lock_lo}{'  [core: sampled floor]' if core else ''})")
        check(total <= 25000, f"{source}: Lock:relation total = {total:.0f}ms (expect <= 25000)")


# ── phases ─────────────────────────────────────────────────────────

def phase_live_system_event(pm_pid, mode, core=False):
    print(f"--- Phase 1: live view (system_event, --mode {mode}) ---")
    wl = Workload()
    wl.open_sessions()
    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "--interval", "12", "--duration", "15",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2.5)   # BPF load + initial scan + (full) watchpoint attach
    try:
        wl.fire(sleep_s=3)     # t≈2.5: pg_sleep(3) + lock wait begin
        time.sleep(6.5)
        wl.release()           # t≈10.5: lock wait ends (~8s) before the tick
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    finally:
        wl.stop()

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    events = parse_system_events(out)
    # THE zero-event guard: an empty view here means capture is dead (#30).
    check(len(events) > 0,
          "live view shows at least one event" if events else
          f"live view shows at least one event (stderr tail: {err[-300:]!r})")
    # ESC-3: the live view must now match the trace view within the tight
    # tolerance (no sampler+exact double-count during escalation). core mode
    # relaxes the pg_sleep magnitude floor where watchpoints don't fire.
    assert_wait_events(events, f"live/{mode}", core=core)


def phase_live_query_event(pm_pid, mode):
    """PR #31 regression: the query-attribution path works end to end —
    query_event view ids must intersect pg_stat_statements.queryid."""
    print(f"--- Phase 2: query attribution (query_event, --mode {mode}) ---")

    pgb_init = subprocess.run(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-i", "-s", "1"],
        capture_output=True, text=True)
    check(pgb_init.returncode == 0,
          "pgbench -i succeeded" if pgb_init.returncode == 0 else
          f"pgbench -i succeeded: {pgb_init.stderr[-200:]}")
    psql("SELECT pg_stat_statements_reset()")

    # pgbench starts FIRST so its backends are found by the initial scan
    # (keeps this phase deterministic; the fork-after-attach path has its
    # own dedicated phase 4).
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "4", "-T", "14"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "--interval", "10", "--duration", "12",
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    pgbench.wait(timeout=30)

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    view_ids = parse_query_event_ids(out)
    truth = pgss_query_ids()
    check(len(truth) > 0,
          f"pg_stat_statements recorded the pgbench queries ({len(truth)} ids)")
    matched = view_ids & truth
    check(len(matched) > 0,
          f"query_event view ids cross-check against pg_stat_statements "
          f"(view={len(view_ids)}, pgss={len(truth)}, matched={len(matched)}) "
          f"[PR #31 regression]")


def phase_trace_file(pm_pid, mode, core=False):
    print(f"--- Phase 3: written trace file (pgwt-server read, --mode {mode}) ---")
    trace_dir = tempfile.mkdtemp(prefix=f"pgwt_smoke_{mode}_")
    os.chmod(trace_dir, 0o755)

    wl = Workload()
    wl.open_sessions()
    psql("SELECT pg_stat_statements_reset()")
    # pgbench starts first so its backends are found by the initial scan
    # (the fork-after-attach path is covered by phase 4).
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "2", "-T", "16"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "18", "--quiet",
         "--interval", "5"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2.5)
    try:
        wl.fire(sleep_s=3)     # t≈2.5 after attach
        time.sleep(8)
        wl.release()           # lock wait ends (~10s) inside the trace window
        _, stderr = tracer.communicate(timeout=45)
        err = stderr.decode('utf-8', errors='replace')
        pgbench.wait(timeout=30)

        resp = server_query(trace_dir, "top_events")
        rows = resp.get("rows", [])
        check(len(rows) > 0,
              f"trace file has events (top_events rows={len(rows)})" if rows
              else f"trace file has events (daemon stderr tail: {err[-300:]!r})")
        events = [{'name': r.get('name'), 'count': r.get('count', 0),
                   'total_ms': r.get('total_ms', 0.0)} for r in rows]
        assert_wait_events(events, f"trace/{mode}", core=core)

        # Query attribution must land in the trace too — and cross-check
        # against pg_stat_statements so phantom ids can't satisfy it.
        qresp = server_query(trace_dir, "top_queries")
        trace_ids = set()
        for r in qresp.get("rows", []):
            try:
                qid = int(str(r.get("query_id", "0")), 10)
            except ValueError:
                continue
            if qid != 0:
                trace_ids.add(qid & 0xFFFFFFFFFFFFFFFF)
        truth = pgss_query_ids()
        matched = trace_ids & truth
        check(len(matched) > 0,
              f"trace file query attribution cross-checks against "
              f"pg_stat_statements (trace={len(trace_ids)}, pgss={len(truth)}, "
              f"matched={len(matched)}) [PR #31 regression]")
    finally:
        wl.stop()
        subprocess.run(["rm", "-rf", trace_dir])


def phase_fork_after_attach(pm_pid, mode):
    """T4/CAP-8 regression: backends forked AFTER the tracer attached must
    record events. The fork->attach race (bootstrap watchpoint armed after
    the child already wrote its my_wait_event_info/MyProc pointer -> the
    watchpoint never fires -> the backend records nothing for its whole
    life, silently) was observed live in --mode full by the T0 CI run; the
    T4 fix re-checks the pointer immediately after arming the bootstrap
    watchpoint, closing the gap. In tiered mode the same phase guards the
    sampler's lazy-resolve path for post-attach forks."""
    print(f"--- Phase 4: fork-after-attach (--mode {mode}) ---")

    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "--interval", "14", "--duration", "17",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2.5)   # BPF load + initial scan complete — NOW fork backends

    wl = Workload()
    try:
        wl.open_sessions()     # all three backends fork post-attach
        wl.fire(sleep_s=3)
        time.sleep(5)
        wl.release()
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    finally:
        wl.stop()

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    events = parse_system_events(out)
    names = [e['name'] for e in events]
    check(len(events) > 0,
          "fork-after-attach: events captured from post-attach backends"
          if events else
          f"fork-after-attach: events captured from post-attach backends "
          f"(stderr tail: {err[-300:]!r})")

    sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']
    check(len(sleep_ev) > 0,
          f"fork-after-attach: Timeout:PgSleep from a post-attach backend "
          f"(saw: {names}) [fork->attach race regression]")
    if sleep_ev:
        total = sleep_ev[0]['total_ms']
        check(1200 <= total <= 7000,
              f"fork-after-attach: PgSleep total = {total:.0f}ms "
              f"(expect ~3000 ±tol)")

    lock_ev = [e for e in events if e['name'] == 'Lock:relation']
    check(len(lock_ev) > 0,
          f"fork-after-attach: Lock:relation from a post-attach backend "
          f"(saw: {names})")


class CpuStorm:
    """N psql sessions each executing a long run of SHORT CPU-bound
    statements (~30-100 ms each, fully cached): pg_stat_activity shows
    them state='active' nearly continuously, and the real command
    boundaries exercise the T2 command-open gate exactly like production
    OLTP (one long DO block would hide the gate)."""

    def __init__(self, n):
        self.sessions = [subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True) for _ in range(n)]
        time.sleep(1.5)   # connected (and, pre-attach, scanned)

    def fire(self, reps=400):
        stmt = "SELECT count(*) FROM generate_series(1,300000);\n"
        for s in self.sessions:
            s.stdin.write(stmt * reps)
            s.stdin.flush()

    def stop(self):
        for s in self.sessions:
            s.terminate()
            try:
                s.wait(timeout=5)
            except subprocess.TimeoutExpired:
                s.kill()
        cleanup_stale_backends()


def sample_active_sessions(duration_s, interval=0.5):
    """pg_stat_activity 1-per-interval ground truth: count of client
    backends with state='active', excluding the sampling connection."""
    counts = []
    end = time.time() + duration_s
    while time.time() < end:
        out = psql("SELECT count(*) FROM pg_stat_activity "
                   "WHERE state = 'active' "
                   "AND backend_type = 'client backend' "
                   "AND pid != pg_backend_pid()", timeout=10)
        if re.fullmatch(r'\d+', out or ''):
            counts.append(int(out))
        time.sleep(interval)
    return counts


def aas_bucket_total(bucket):
    return sum(v for k, v in bucket.items()
               if k not in ("t", "total", "cat") and isinstance(v, (int, float)))


def ctl_request(trace_dir, obj):
    """One arbitrary JSON request over the daemon control socket."""
    import socket
    path = os.path.join(trace_dir, "pgwt.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(path)
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            c = s.recv(4096)
            if not c:
                break
            buf += c
        return json.loads(buf.decode().split("\n")[0])
    finally:
        s.close()


def ctl_query(trace_dir, cmd):
    """One JSON request over the daemon control socket."""
    return ctl_request(trace_dir, {"cmd": cmd})


def phase_cpu_straddle(pm_pid, mode):
    """Regression for the edge-vs-level command-gate bug (found in EL9 live
    validation, latent on ALL platforms, BOTH tiers). The on_report_activity
    uprobe sets cmd_open only at command START; a command already IN FLIGHT
    when the tracer first seeds the backend misses that edge, so cmd_open
    stays 0 for the whole command and every we==0 (CPU) reading is dropped
    as non-command churn — CPU* collapses to 0 for that command. Ubuntu CI
    missed it because the other CPU phases fire their work AFTER attach (edge
    caught) and CpuStorm uses short re-firing statements. This phase does the
    opposite: ONE long compute statement is already running before the tracer
    starts. sampled tier is fixed by the per-tick debug_query_string gate;
    exact tier by seeding cmd_open from debug_query_string at preseed. Pre-fix
    both asserted CPU ~0."""
    print(f"--- Phase: CPU straddle, --mode {mode} (command in flight at attach) ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_straddle_")
    os.chmod(trace_dir, 0o755)

    # One long, single-statement compute query — starts BEFORE the tracer.
    hog = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True)
    hog.stdin.write(
        "SELECT count(*) FROM generate_series(1,600000000) g "
        "WHERE (g*g) % 7 = 0;\n")
    hog.stdin.flush()
    time.sleep(2.5)   # the command is now in flight; its start-edge is past

    argv = [TRACER, "--mode", mode, "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "12", "--quiet", "--interval", "5"]
    if mode == "tiered":
        argv += ["--anomaly-aas-factor", "1000000"]  # keep it pure sampled
    tracer = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(3.0)          # BPF load + scan/attach finds the in-flight backend
        win_from = time.time_ns()
        time.sleep(7.0)          # window over the straddling command
        win_to = time.time_ns()
        _, stderr = tracer.communicate(timeout=40)
        err = stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        tracer.kill()
        _, stderr = tracer.communicate()
        err = stderr.decode('utf-8', errors='replace')
    finally:
        try:
            hog.stdin.write("\\q\n"); hog.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        hog.terminate()
        cleanup_stale_backends()

    resp = server_query(trace_dir, "aas",
                        extra={"from": win_from, "to": win_to, "buckets": 7})
    buckets = resp.get("buckets", [])
    check(len(buckets) > 0,
          "aas view has buckets over the straddle window" if buckets else
          f"aas view has buckets (stderr tail: {err[-300:]!r})")
    if buckets:
        cpu_mean = sum(b.get("cpu", 0.0) for b in buckets) / len(buckets)
        # The single straddling backend is ~1 CPU-active session the whole
        # window. Pre-fix (edge-only gate) this was ~0 — the exact defect.
        check(cpu_mean >= 0.5,
              f"in-flight command's CPU is counted (--mode {mode}): "
              f"cpu AAS = {cpu_mean:.2f} (edge-vs-level regression; pre-fix ~0)")


def phase_pure_cpu_straddle(pm_pid, mode):
    """T8 (docs/ROADMAP_AND_STATUS.md) — THE measured-CPU acceptance
    test. A PL/pgSQL loop over NO data fires ZERO wait events for its entire
    life, so the watchpoint never emits a boundary. Pre-T8 this produced an
    EMPTY trace in --mode full (the on-CPU stretch was never closed) and 0%
    live CPU (symptoms #1/#2). Post-T8 the measured se.sum_exec_runtime
    accumulator makes it visible LIVE (open-interval schedstat delta) and the
    terminal flush writes its on-CPU stretch to the trace at daemon shutdown.

    Asserted in BOTH tiers: CPU present in the live view AND in the trace, with
    the magnitude cross-checked against the backend's /proc on-CPU delta over
    the same [attach, shutdown] window (±20%). The loop count is huge so it
    straddles capture end on any box; it starts BEFORE the tracer so its
    command start-edge is already past at attach."""
    # DIAG (diag-straddle-livecpu branch): loop the live-view scenario HERE,
    # in-context (after the preceding phases — the isolated top-of-run loop did
    # not reproduce, the in-context real phase did), taking many shots + dumping
    # the time_model blocks on the first catch.
    if mode == 'full' and os.environ.get("PGWT_STRADDLE_DEBUG"):
        phase_straddle_livecpu_loop(pm_pid)
        return

    print(f"--- Phase: PURE-CPU straddle, --mode {mode} (no waits — empty-trace "
          f"repro) ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_purecpu_")
    os.chmod(trace_dir, 0o755)

    hog = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True)
    # Nested int4 loops: ~1e11 iterations of pure compute — minutes of runtime,
    # guaranteed to still be running at daemon shutdown so the terminal flush
    # has an open on-CPU stretch to close. Both bounds fit int4 (a single
    # 1..2e10 loop would exceed the int4 loop-variable range and error out
    # instantly); x resets each outer pass so the bigint accumulator never
    # overflows. NO trailing pg_sleep: a wait would hand the watchpoint a
    # boundary and defeat the point (this must be waitless end to end).
    hog.stdin.write(
        "DO $$ DECLARE x bigint := 0; BEGIN "
        "FOR o IN 1..100000 LOOP "
        "FOR i IN 1..1000000 LOOP x := x + i; END LOOP; x := 0; "
        "END LOOP; END $$;\n")
    hog.stdin.flush()
    time.sleep(2.5)   # the command is in flight; its start-edge is past

    argv = [TRACER, "--mode", mode, "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "12", "--interval", "5",
            "--view", "time_model"]
    if mode == "tiered":
        argv += ["--anomaly-aas-factor", "1000000"]   # keep it pure sampled

    be_pid = None
    oncpu_before = oncpu_after = None
    win_from = win_shutdown = 0
    _dbg_env = {**os.environ}
    if os.environ.get("PGWT_STRADDLE_DEBUG"):
        _dbg_env["PGWT_DBG_LIVEREAD"] = "1"   # temporary: diagnose live CPU*=0
    tracer = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=_dbg_env)
    try:
        time.sleep(3.0)                 # BPF load + scan/attach seeds the backend
        be_pid = find_active_do_backend()
        win_from = time.time_ns()
        oncpu_before = proc_oncpu_ns(be_pid) if be_pid else None  # ≈ the seed instant
        # Let the tracer run out its 12s duration and flush its open interval.
        stdout, stderr = tracer.communicate(timeout=45)
        win_shutdown = time.time_ns()
        oncpu_after = proc_oncpu_ns(be_pid) if be_pid else None    # ≈ the flush instant
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
        win_shutdown = time.time_ns()
        oncpu_after = proc_oncpu_ns(be_pid) if be_pid else None
    finally:
        try:
            hog.stdin.write("\\q\n"); hog.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        hog.terminate()
        cleanup_stale_backends()
    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')

    # 1) LIVE VIEW: the periodic time_model print must show measured CPU>0.
    #    Pre-T8 this row was 0.0 for a waitless command (the exact symptom).
    live = parse_time_model(out)
    live_cpu = live.get('CPU*', 0.0)
    if live_cpu <= 0.0 and os.environ.get("PGWT_STRADDLE_DEBUG"):
        # Dump the full tracer stdout+stderr (incl. LIVEREAD debug) so the
        # failing tick's state_map read is inspectable. Temporary diagnostic.
        with open("/tmp/straddle_fail_out.txt", "w") as f:
            f.write(out)
        with open("/tmp/straddle_fail_err.txt", "w") as f:
            f.write(err)
        print("=== STRADDLE LIVE CPU*=0 — LIVEREAD dump (be_pid=%s) ===" % be_pid)
        print("\n".join(l for l in err.splitlines() if "LIVEREAD" in l)[-4000:])
    check(live_cpu > 0.0,
          f"pure-CPU straddle live view shows CPU* > 0 (--mode {mode}): "
          f"CPU* = {live_cpu:.0f}ms (pre-T8 ~0; stderr {err[-160:]!r})")

    # 2) TRACE (terminal flush): whole-trace time_model must carry CPU.
    #    In --mode full this proves the flush closed the open on-CPU stretch;
    #    pre-T8 the trace was EMPTY (no wait boundary ever fired).
    resp = server_query(trace_dir, "time_model")
    trace_cpu_ms = float(resp.get("cpu_ms", 0.0) or 0.0)
    check(trace_cpu_ms > 0.0,
          f"pure-CPU straddle trace carries CPU after terminal flush "
          f"(--mode {mode}): cpu_ms = {trace_cpu_ms:.0f} "
          f"(has_measured_cpu={resp.get('has_measured_cpu')})")

    # 3) MAGNITUDE: the DO backend's measured CPU over [win_from, shutdown] must
    #    match its /proc on-CPU delta over the SAME span (both read the same
    #    se.sum_exec_runtime counter). Isolate the backend with a pid filter and
    #    integrate the aas cpu (avg-sessions × window = cpu-seconds): the integral
    #    is window-length-invariant, so querying THROUGH shutdown+margin includes
    #    the --mode full flush record (timestamped at shutdown — a sub-window
    #    ending earlier would time-prune its block) while still measuring only
    #    the CPU it actually spent. ±20% absorbs attach/seed timing slack and
    #    sampled-tier estimator noise.
    if be_pid and oncpu_before is not None and oncpu_after is not None \
            and oncpu_after > oncpu_before and win_shutdown > win_from:
        proc_ms = (oncpu_after - oncpu_before) / 1e6
        win_to = win_shutdown + 3_000_000_000   # margin so the flush block is in range
        aresp = server_query(trace_dir, "aas",
                             extra={"from": win_from, "to": win_to,
                                    "buckets": 12, "filters": {"pid": be_pid}})
        abuckets = aresp.get("buckets", [])
        cpu_mean = (sum(b.get("cpu", 0.0) for b in abuckets) / len(abuckets)
                    if abuckets else 0.0)
        trace_win_ms = cpu_mean * (win_to - win_from) / 1e9 * 1000.0
        ratio = trace_win_ms / proc_ms if proc_ms > 0 else 0.0
        check(0.8 <= ratio <= 1.2,
              f"pure-CPU straddle measured CPU {trace_win_ms:.0f}ms within ±20% "
              f"of /proc on-CPU {proc_ms:.0f}ms over [attach,shutdown] "
              f"(ratio {ratio:.2f}, --mode {mode})")
    else:
        # Never silently pass the magnitude gate — record why it could not run.
        check(be_pid is not None,
              f"pure-CPU straddle: located the DO backend for the /proc "
              f"cross-check (pid={be_pid}, before={oncpu_before}, "
              f"after={oncpu_after})")

    subprocess.run(["rm", "-rf", trace_dir])


def phase_straddle_recovery(pm_pid, mode):
    """DETERMINISTIC regression for the initial-scan straddle race
    (pgwt_recover_unattached_backends, docs/ROADMAP_AND_STATUS.md). phase_pure_cpu_
    straddle above exercises the same edge but the miss is a TIMING race — it
    reproduced only intermittently on 2-CPU CI runners (PG13), never on a real
    box. PGWT_TEST_STRADDLE_SKIP forces the one-shot scan to skip the in-flight
    backend, reproducing that transient-resolve miss ON EVERY RUN. The
    level-triggered recovery (startup settle + per-tick backstop) must attach it
    anyway, so a waitless pure-CPU straddler that the scan lost still reads
    CPU*>0. Without the recovery (PGWT_TEST_NO_RECOVERY) this reads ~0 — that is
    the exact CI failure. Runs -v so the scan-skip WARN and the recovery INFO
    are both visible and asserted."""
    print(f"--- Phase: straddle recovery, --mode {mode} (scan forced to miss "
          f"the in-flight backend) ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_recover_")
    os.chmod(trace_dir, 0o755)

    hog = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True)
    hog.stdin.write(
        "DO $$ DECLARE x bigint := 0; BEGIN "
        "FOR o IN 1..100000 LOOP "
        "FOR i IN 1..1000000 LOOP x := x + i; END LOOP; x := 0; "
        "END LOOP; END $$;\n")
    hog.stdin.flush()
    time.sleep(2.5)   # the command is in flight; its start-edge is past

    argv = [TRACER, "--mode", mode, "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "12", "--interval", "5",
            "--view", "time_model", "-v"]
    if mode == "tiered":
        argv += ["--anomaly-aas-factor", "1000000"]
    env = {**os.environ, "PGWT_TEST_STRADDLE_SKIP": "1"}
    tracer = subprocess.Popen(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env)
    try:
        stdout, stderr = tracer.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    finally:
        try:
            hog.stdin.write("\\q\n"); hog.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        hog.terminate()
        cleanup_stale_backends()
    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')

    # The hook must actually have fired (else the test proves nothing).
    check("PGWT_TEST_STRADDLE_SKIP" in err,
          "scan was forced to skip the in-flight backend (test hook active)")
    # The recovery — not the scan — must have attached it.
    check("recovered unattached backend" in err,
          "pgwt_recover_unattached_backends attached the scan-missed straddler "
          f"(stderr {err[-200:]!r})")
    # And the payoff: its CPU is visible despite the scan miss.
    live = parse_time_model(out)
    live_cpu = live.get('CPU*', 0.0)
    check(live_cpu > 0.0,
          f"scan-missed pure-CPU straddler still shows CPU* > 0 via recovery "
          f"(--mode {mode}): CPU* = {live_cpu:.0f}ms")

    subprocess.run(["rm", "-rf", trace_dir])


def phase_straddle_livecpu_loop(pm_pid, iters=40):
    """DIAG only. Run the pure-CPU straddle live-view scenario `iters` times;
    on the FIRST time the live time_model shows CPU* <= 0, dump the per-tick
    LIVEREAD state and all time_model blocks so the failing read is
    inspectable, then fail. ~10 shots per CI run raise the odds of catching
    the intermittent 2-vCPU flake in one run."""
    print(f"--- DIAG: straddle live-CPU loop ({iters}x) ---")
    for it in range(iters):
        trace_dir = tempfile.mkdtemp(prefix="pgwt_diag_")
        os.chmod(trace_dir, 0o755)
        hog = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True)
        hog.stdin.write(
            "DO $$ DECLARE x bigint := 0; BEGIN "
            "FOR o IN 1..100000 LOOP "
            "FOR i IN 1..1000000 LOOP x := x + i; END LOOP; x := 0; "
            "END LOOP; END $$;\n")
        hog.stdin.flush()
        time.sleep(2.5)
        env = {**os.environ, "PGWT_DBG_LIVEREAD": "1"}
        tracer = subprocess.Popen(
            [TRACER, "--mode", "full", "--pid", str(pm_pid), "-T", trace_dir,
             "--duration", "12", "--interval", "5", "--view", "time_model"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        be_pid = None
        try:
            time.sleep(3.0)
            be_pid = find_active_do_backend()
            stdout, stderr = tracer.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            tracer.kill()
            stdout, stderr = tracer.communicate()
        finally:
            try:
                hog.stdin.write("\\q\n"); hog.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass
            hog.terminate()
            cleanup_stale_backends()
        out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
        err = stderr.decode('utf-8', errors='replace')
        live = parse_time_model(out)
        cpu = live.get('CPU*', 0.0)
        print(f"  iter {it}: live CPU*={cpu:.0f}ms  be_pid={be_pid}")
        subprocess.run(["rm", "-rf", trace_dir])
        if cpu <= 0.0:
            print(f"=== CAUGHT: live CPU*=0 at iter {it} (be_pid={be_pid}) ===")
            # Per-block CPU* across all printed time_model blocks: reveals
            # whether the LAST block is 0 (print/parse) or ALL are 0 (read).
            blocks = re.split(r'-{3,}\s*\d{4}-\d\d-\d\d', out)
            for bi, blk in enumerate(blocks):
                mm = [l for l in blk.splitlines() if 'CPU*' in l]
                if mm:
                    print(f"  block {bi}: {mm[0].strip()}")
            print("--- FULL time_model stdout ---")
            print(out)
            print("--- stderr tail (last 1500 chars) ---")
            print(err[-1500:])
            check(False, f"DIAG caught straddle live CPU*=0 at iter {it}")
            return
    check(True, f"DIAG: all {iters} straddle iters had CPU*>0 (not caught)")


def phase_sampled_aas_truth(pm_pid):
    """T2 (AAS-1) definition-of-done: sampled AAS — now CPU-inclusive —
    must match pg_stat_activity 1s-sampling ground truth. A CPU-bound
    storm (3 hogs) + a pg_sleep session; pre-T2 the sampler skipped all
    we==0 and reported AAS ~ 0.0x for exactly this shape (study Q4:
    -98%%). Anomaly escalation is disabled (huge factor) so the window is
    PURE sampled tier."""
    print("--- Phase 5: sampled CPU-inclusive AAS vs pg_stat_activity ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_aas_")
    os.chmod(trace_dir, 0o755)

    storm = CpuStorm(3)
    sleeper = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True)
    time.sleep(1.0)

    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "17", "--quiet",
         "--interval", "5", "--anomaly-aas-factor", "1000000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(3.0)   # BPF load + scan + first ticks
    try:
        win_from = time.time_ns()
        storm.fire()
        sleeper.stdin.write("SELECT pg_sleep(10);\n")
        sleeper.stdin.flush()
        time.sleep(0.5)
        psa = sample_active_sessions(9.0, interval=0.5)
        win_to = time.time_ns()
        _, stderr = tracer.communicate(timeout=40)
        err = stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        tracer.kill()
        _, stderr = tracer.communicate()
        err = stderr.decode('utf-8', errors='replace')
    finally:
        storm.stop()
        sleeper.terminate()

    psa_mean = sum(psa) / len(psa) if psa else 0.0
    check(psa_mean >= 2.0,
          f"ground truth: pg_stat_activity mean active = {psa_mean:.2f} "
          f"(storm running; n={len(psa)})")

    resp = server_query(trace_dir, "aas",
                        extra={"from": win_from, "to": win_to, "buckets": 9})
    buckets = resp.get("buckets", [])
    check(len(buckets) > 0,
          "aas view has buckets over the storm window" if buckets else
          f"aas view has buckets (stderr tail: {err[-300:]!r})")
    if buckets:
        totals = [aas_bucket_total(b) for b in buckets]
        tracer_mean = sum(totals) / len(totals)
        cpu_mean = sum(b.get("cpu", 0.0) for b in buckets) / len(buckets)
        tol = max(0.9, 0.35 * psa_mean)
        check(abs(tracer_mean - psa_mean) <= tol,
              f"sampled AAS matches pg_stat_activity ground truth: "
              f"tracer={tracer_mean:.2f} vs psa={psa_mean:.2f} (tol ±{tol:.2f}) "
              f"[AAS-1 definition of done]")
        check(cpu_mean >= 1.0,
              f"CPU class carries the storm: cpu AAS = {cpu_mean:.2f} "
              f"(pre-T2 sampler reported ~0 here)")
        check(resp.get("fidelity") == "sampled",
              f"window is pure sampled tier (fidelity={resp.get('fidelity')!r})")

    subprocess.run(["rm", "-rf", trace_dir])


def phase_cpu_storm_escalation(pm_pid):
    """T2 definition-of-done: a SELECT-storm CPU incident raises sampled
    AAS enough to trigger anomaly escalation (the engine was blind to CPU
    before — AAS-1), and the AAS chart shows no step artifact across the
    sampled->exact tier switch."""
    print("--- Phase 6: CPU storm triggers escalation; no AAS step ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_esc_")
    os.chmod(trace_dir, 0o755)

    storm = CpuStorm(3)
    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "20", "--quiet",
         "--interval", "5"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(7.0)   # BPF load + scan + anomaly baseline warmup (~5 s idle)
    try:
        storm_from = time.time_ns()
        storm.fire()
        time.sleep(5.0)
        metrics = {}
        try:
            metrics = ctl_query(trace_dir, "metrics")
        except OSError as e:
            print(f"  (control socket: {e})")
        storm_active = sample_active_sessions(3.0, interval=0.5)
        storm_to = time.time_ns()
        _, stderr = tracer.communicate(timeout=40)
        err = stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        tracer.kill()
        _, stderr = tracer.communicate()
        err = stderr.decode('utf-8', errors='replace')
    finally:
        storm.stop()

    fires = metrics.get("anomaly_fires_total", 0)
    windows = metrics.get("escalation_windows_total", 0)
    check(fires >= 1 and windows >= 1,
          f"CPU storm triggered anomaly escalation "
          f"(anomaly_fires_total={fires}, escalation_windows_total={windows}, "
          f"tier={metrics.get('tier')!r}) [AAS-1: engine no longer CPU-blind]"
          + ("" if fires >= 1 else f" (tracer stderr tail: {err[-300:]!r})"))

    # No step artifact: every interior 1s bucket across the tier switch
    # must show the storm. The anomaly fires ~0.3-1 s into the storm, so
    # starting the window at +0.7 s puts the sampled->exact switch INSIDE
    # the asserted range. Pre-T2, sampled buckets read ~0.0 while exact
    # (escalated) buckets read ~3 — a hard step.
    resp = server_query(trace_dir, "aas",
                        extra={"from": storm_from + 700_000_000,
                               "to": storm_to - 1_000_000_000,
                               "buckets": 7})
    buckets = resp.get("buckets", [])
    check(len(buckets) >= 3, f"aas buckets across the tier switch "
          f"(got {len(buckets)}, fidelity={resp.get('fidelity')!r})")
    if buckets:
        totals = [aas_bucket_total(b) for b in buckets]
        lo = min(totals)
        truth = (sum(storm_active) / len(storm_active)) if storm_active else 3.0
        check(lo >= 1.2,
              f"no AAS step artifact at the tier switch: min bucket = "
              f"{lo:.2f}, buckets = {[f'{t:.1f}' for t in totals]}, "
              f"psa during storm = {truth:.1f}")

    subprocess.run(["rm", "-rf", trace_dir])


def phase_escalation_billing(pm_pid):
    """T3 (ESC-1/2): a MANUAL escalation window bills its full-fidelity time
    honestly (budget drops by ~the window length), and a wait that is still
    OPEN when the window closes is flushed into the trace exactly once (ESC-2)
    instead of vanishing into an end-of-window hole."""
    print("--- Phase 7: manual escalation billing + de-escalation flush ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_esc2_")
    os.chmod(trace_dir, 0o755)

    # A lock wait held OPEN across the whole phase (never released) so it is
    # still in-flight at de-escalation — the exact case ESC-2 must not drop.
    wl = Workload()
    wl.open_sessions()

    budget = 60
    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "22", "--quiet", "--interval", "2",
         "--escalation-budget", str(budget),
         # Disable anomaly auto-escalation so the ONLY window is the manual one
         # we drive (clean billing accounting).
         "--anomaly-aas-factor", "1000000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(3.0)   # BPF load + scan
    try:
        wl.fire(sleep_s=1)   # start the lock wait (waiter blocks, stays blocked)
        time.sleep(1.0)

        budget_before = ctl_query(trace_dir, "status").get(
            "escalation_budget_remaining_s", budget)

        # Manually escalate for 5s.
        esc = ctl_request(trace_dir, {"cmd": "escalate", "duration_s": 5,
                                      "reason": "manual"})
        check(esc.get("ok") is True and esc.get("granted_s", 0) >= 4,
              f"manual escalate granted a ~5s window (resp={esc})")
        tier = ctl_query(trace_dir, "status").get("tier")
        check(tier == "escalated",
              f"tier flipped to escalated during the window (got {tier!r})")

        time.sleep(3.0)   # capture exact transitions mid-window
        # De-escalate WHILE the lock wait is still open (ESC-2 flush path).
        deesc = ctl_request(trace_dir, {"cmd": "deescalate"})
        check(deesc.get("ok") is True,
              f"manual deescalate acknowledged (resp={deesc})")

        m = ctl_query(trace_dir, "metrics")
        budget_after = m.get("escalation_budget_remaining_s", budget)
        windows = m.get("escalation_windows_total", 0)
        spent = budget_before - budget_after
        check(windows >= 1,
              f"escalation_windows_total >= 1 (got {windows})")
        # Billed time must be > 0 and no more than the window we ran (~3-5s),
        # i.e. billing tracks the actual open window, never over-charges.
        check(0.5 <= spent <= 6.0,
              f"budget billed ~= window length: spent {spent:.1f}s of a "
              f"~3-5s window [ESC-1 honest billing]")

        _, stderr = tracer.communicate(timeout=40)
        err = stderr.decode('utf-8', errors='replace')

        # ESC-2: the lock wait, open across the window boundary, must be in the
        # trace (the flush recorded its exact portion; without ESC-2 the
        # end-of-window hole dropped it).
        resp = server_query(trace_dir, "top_events")
        events = [{'name': r.get('name'), 'count': r.get('count', 0),
                   'total_ms': r.get('total_ms', 0.0)}
                  for r in resp.get("rows", [])]
        names = [e['name'] for e in events]
        lock_ev = [e for e in events if e['name'] == 'Lock:relation']
        check(len(lock_ev) > 0,
              f"Lock:relation spanning the window boundary is in the trace "
              f"(saw {names}) [ESC-2 flush]"
              + ("" if lock_ev else f" (stderr tail: {err[-300:]!r})"))
    except subprocess.TimeoutExpired:
        tracer.kill()
        tracer.communicate()
    finally:
        wl.stop()
        subprocess.run(["rm", "-rf", trace_dir])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    parser.add_argument('--mode', required=True, choices=['full', 'tiered'])
    parser.add_argument('--capture-core', action='store_true',
                        help='Container/cross-distro mode: prove the capture '
                             'path works (live events + attribution, trace '
                             'events + attribution) and LOUDLY skip the '
                             'watchpoint-fidelity + CPU-storm phases, which '
                             'need hardware watchpoints to actually fire and '
                             'precise multi-core scheduling (validated on the '
                             'T0 hosted runner + live EL8/EL9 boxes).')
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)
    for binpath in (TRACER, SERVER):
        if not os.path.exists(binpath):
            print(f"ERROR: {binpath} not built")
            sys.exit(1)

    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_capture_smoke --mode {args.mode} (postmaster PID {pm_pid}) ===")

    # Environment gate: query-attribution assertions need query_ids to be
    # computed at all. Fail LOUDLY rather than skipping (this test exists
    # to prevent vacuous greens).
    preload = psql("SHOW shared_preload_libraries")
    if "pg_stat_statements" not in preload:
        print("ERROR: pg_stat_statements is not in shared_preload_libraries "
              f"(got: {preload!r}).\n"
              "  PG13 needs it for query attribution; PG14+ need it (or "
              "compute_query_id=on) for query_ids.\n"
              "  ci.yml configures this — do the same on this box.")
        sys.exit(1)
    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")

    cleanup_stale_backends()

    core = args.capture_core
    phase_live_system_event(pm_pid, args.mode, core=core)
    phase_live_query_event(pm_pid, args.mode)
    phase_trace_file(pm_pid, args.mode, core=core)

    if core:
        # The remaining phases need hardware watchpoints to actually FIRE
        # (exact escalation intervals) and precise multi-core CPU-storm
        # scheduling — neither holds in a nested container even when the
        # watchpoint probe can open a breakpoint. Skip them LOUDLY; they are
        # the T0 hosted runner's and the live EL8/EL9 boxes' gate.
        print("--- SKIPPED in --capture-core (container): fork-after-attach, "
              "sampled-AAS ground truth, CPU-storm escalation, escalation "
              "billing ---")
        print("    These assert watchpoint-fidelity / precise CPU-storm timing "
              "the nested container does not provide; validated on the T0 "
              "hosted runner + live EL8/EL9 boxes (run_all.sh --require-live).")
    else:
        phase_fork_after_attach(pm_pid, args.mode)
        # CPU straddle (command in flight at attach) — both tiers; the fix is
        # per-tick for sampled and preseed-seeded for exact.
        phase_cpu_straddle(pm_pid, args.mode)
        # T8: PURE-CPU straddle (waitless DO-loop) — the measured-CPU
        # acceptance test. Live + trace + /proc magnitude cross-check.
        phase_pure_cpu_straddle(pm_pid, args.mode)
        # Deterministic straddle-recovery guard: force the scan to miss the
        # in-flight backend; the level-triggered recovery must attach it anyway.
        # Full mode only — the recovery is a watchpoint-mode feature; in
        # sampled/tiered the straddler is found by the sampler's lazy discovery
        # (covered by phase_pure_cpu_straddle above), not this recovery.
        if args.mode == 'full':
            phase_straddle_recovery(pm_pid, args.mode)
        if args.mode == 'tiered':
            # T2 (AAS semantics): CPU-inclusive sampled AAS vs pg_stat_activity
            # ground truth, and the CPU-storm escalation + tier-switch
            # continuity checks (docs/AAS_SEMANTICS_DECISION.md).
            phase_sampled_aas_truth(pm_pid)
            phase_cpu_storm_escalation(pm_pid)
            # T3: manual escalation billing (ESC-1) + de-escalation flush (ESC-2).
            phase_escalation_billing(pm_pid)

    print(f"\n{tests_passed}/{tests_run} checks passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
