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
  3. short pgbench run        -> query_event shows PG14+ pgss query ids or
                                 PG13 versioned synthetic text groups

Wait events are asserted in BOTH:
  - the live view output (--view system_event / query_event), and
  - the written trace file, read back through pgwt-server (the same JSON
    protocol the web UI uses; --replay is NOT used because replay of
    tiered traces is fidelity-broken until T1/FID-5).

Regression coverage (referenced by PR number, per the Trust Milestone):
  - PR #30: in sampled/tiered mode the sampler must feed the live
    accumulator — the tiered live-view assertions here are empty if that
    regresses.
  - PR #31: full/exact query attribution must capture real query ids. In
    sampled mode, Stage 4 deliberately detaches that uprobe: PG14+ reads the
    in-core id and resolves text lazily through pgss; PG13 reads activity and
    produces a context-keyed synthetic group.
  - PR #24 (load base) would make this whole test capture zero events on
    non-PIE layouts; the unit-level guard is tests/test_discovery*.

Environment requirements (ci.yml configures both; fail loudly otherwise):
  - pg_stat_statements in shared_preload_libraries (required for PG13 exact
    numeric ids and PG14+ sampled text; PG14+ also needs query ids enabled).
  - pgbench + psql for the workload, connecting as "postgres" via local
    trust auth.

Usage: sudo python3 tests/test_capture_smoke.py --mode {full,tiered}
           [--pid POSTMASTER_PID]
"""
import argparse
import json
import os
import re
import secrets
import select
import signal
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


def wait_for_tagged_active_backend(application_name, timeout_s=30):
    """Return a tagged backend PID only after PostgreSQL reports it active."""
    quoted = application_name.replace("'", "''")
    deadline = time.monotonic() + timeout_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        try:
            out = psql(
                "SELECT pid FROM pg_stat_activity "
                f"WHERE application_name = '{quoted}' AND state = 'active'",
                timeout=min(10, max(1, remaining)))
        except subprocess.TimeoutExpired:
            out = ""
        if re.fullmatch(r'\d+', out or ''):
            return int(out)
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))


def wait_for_tagged_backend_count(application_name, predicate, timeout_s=30,
                                  active_only=False):
    """Wait for a valid pg_stat_activity count; SQL errors never mean zero."""
    quoted = application_name.replace("'", "''")
    deadline = time.monotonic() + timeout_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        try:
            output = psql(
                "SELECT count(*) FROM pg_stat_activity "
                f"WHERE application_name = '{quoted}' "
                "AND datname = 'postgres'" +
                (" AND state = 'active'" if active_only else ""),
                timeout=min(10, max(1, remaining)))
        except subprocess.TimeoutExpired:
            output = ""
        if re.fullmatch(r'\d+', output or ''):
            count = int(output)
            if predicate(count):
                return count
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))


def proc_start_ticks(pid):
    """Return /proc starttime for PID-reuse-safe targeted cleanup."""
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as stat_file:
            tail = stat_file.read().rsplit(") ", 1)[1].split()
        return int(tail[19])  # field 22; tail[0] is field 3
    except (OSError, IndexError, ValueError):
        return None


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


def pgbench_pgss_query_ids():
    """Real pgbench query IDs, excluding this lookup from its own result."""
    out = psql(
        "SELECT queryid FROM pg_stat_statements "
        "WHERE queryid IS NOT NULL AND calls >= 3 "
        "AND query NOT LIKE '%pg_stat_statements%' "
        "AND (query LIKE '%pgbench_accounts%' "
        "OR query LIKE '%pgbench_branches%' "
        "OR query LIKE '%pgbench_tellers%' "
        "OR query LIKE '%pgbench_history%')")
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
        self.tag_base = f"pgwt_wl_{secrets.token_hex(8)}"
        self.backend_pids = {}

    def _session(self, role):
        env = os.environ.copy()
        env["PGAPPNAME"] = f"{self.tag_base}_{role}"
        return subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True, env=env)

    def open_sessions(self, discover_pids=False):
        psql(f"CREATE TABLE IF NOT EXISTS {self.LOCK_TABLE} (id int)")
        self.sleeper = self._session("sleeper")
        self.holder = self._session("holder")
        self.waiter = self._session("waiter")
        self.holder.stdin.write(
            f"BEGIN; LOCK TABLE {self.LOCK_TABLE} IN ACCESS EXCLUSIVE MODE;\n")
        self.holder.stdin.flush()
        time.sleep(1.5)   # sessions connected, lock held
        held = psql(
            f"SELECT count(*) FROM pg_locks "
            f"WHERE relation = '{self.LOCK_TABLE}'::regclass AND granted")
        check(held != "" and int(held) >= 1,
              f"workload: holder acquired AccessExclusiveLock (held={held!r})")

        def discover_backend_pids():
            apps = [f"{self.tag_base}_{role}"
                    for role in ("sleeper", "holder", "waiter")]
            quoted_apps = ",".join(
                "'" + app.replace("'", "''") + "'" for app in apps)
            rows = psql(
                "SELECT application_name, pid FROM pg_stat_activity "
                f"WHERE application_name IN ({quoted_apps}) "
                "AND datname = 'postgres'")
            found = {}
            for row in rows.splitlines():
                parts = row.split('|', 1)
                if len(parts) != 2 or not parts[1].isdigit():
                    continue
                role = parts[0].rsplit('_', 1)[-1]
                if role in ("sleeper", "holder", "waiter"):
                    found[role] = int(parts[1])
            return found if len(found) == 3 else None

        if discover_pids:
            # PID identities let exact-mode assertions distinguish the
            # controlled waits from unrelated background events on a shared
            # PostgreSQL. Opt-in matters: Phase 4 opens these sessions inside
            # an already-running short tracer window.
            self.backend_pids = wait_for_observation(
                self.holder, discover_backend_pids, 30, interval_s=0.2) or {}

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


def temp_output_text(stream):
    """Read a subprocess TemporaryFile without moving its shared file offset."""
    size = os.fstat(stream.fileno()).st_size
    data = os.pread(stream.fileno(), size, 0) if size else b""
    return data.decode('utf-8', errors='replace')


def wait_for_observation(proc, observe, timeout_s, interval_s=0.05):
    """Poll an observable until it becomes truthy or a bounded deadline ends."""
    deadline = time.monotonic() + timeout_s
    last = None
    while True:
        try:
            last = observe()
        except (OSError, subprocess.TimeoutExpired):
            last = None
        if last or proc.poll() is not None:
            return last
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return last
        time.sleep(min(interval_s, remaining))


def wait_for_control_socket(proc, trace_dir, timeout_s=30):
    """Wait until the daemon run loop services a control round-trip.

    The socket pathname is created before the sampled provider starts.  Path
    existence alone therefore leaves a runner-stall window where a short test
    workload can finish before capture is live.
    """
    status = wait_for_control_observation(
        proc, trace_dir, "status", lambda value: value.get("ok") is True,
        timeout_s=timeout_s)
    return status.get("ok") is True


def terminate_and_wait(proc, timeout_s=30):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def wait_for_natural_exit(proc, timeout_s=60):
    """Allow the tracer to finalize its trace; kill only after the hard cap."""
    try:
        proc.wait(timeout=timeout_s)
        return True
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        return False


# ── pgwt-server trace-file reader ──────────────────────────────────

def server_query(trace_dir, cmd, extra=None, timeout_s=None):
    """One-shot pgwt-server JSON query against a trace dir."""
    proc = subprocess.Popen([SERVER, trace_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    deadline = (time.monotonic() + timeout_s
                if timeout_s is not None else None)

    def read_response():
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select(
                    [proc.stdout], [], [], remaining)[0]:
                raise subprocess.TimeoutExpired(proc.args, timeout_s)
        return json.loads(proc.stdout.readline())

    try:
        proc.stdin.write(json.dumps({"id": 1, "cmd": "info"}) + "\n")
        proc.stdin.flush()
        info = read_response()
        req = {"id": 2, "cmd": cmd,
               "from": max(0, info.get("from_ns", 0) - 30_000_000_000),
               "to": info.get("to_ns", 0) + 30_000_000_000}
        if extra:
            req.update(extra)
        proc.stdin.write(json.dumps(req) + "\n")
        proc.stdin.flush()
        resp = read_response()
    finally:
        try:
            proc.stdin.close()
        except BrokenPipeError:
            pass
        try:
            proc.wait(timeout=1 if deadline is not None else 5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    return resp


QUERY_TEXT_SOURCE_PRIORITY = {"pgss": 1, "synthetic": 2, "full": 3}


def query_text_key(obj):
    """Canonical context key shared by top_queries and query_texts.jsonl."""
    try:
        return (int(obj.get("d", obj.get("databaseid", 0))),
                int(obj.get("u", obj.get("userid", 0))),
                int(str(obj.get("q", obj.get("query_id", "0"))), 10))
    except (TypeError, ValueError):
        return None


def winning_query_text_sources(records):
    """Return the highest-priority persisted text source for each context."""
    winners = {}
    for record in records:
        key = query_text_key(record)
        source = record.get("s")
        priority = QUERY_TEXT_SOURCE_PRIORITY.get(source, 0)
        if key and key[2] and record.get("t") and priority > \
                QUERY_TEXT_SOURCE_PRIORITY.get(winners.get(key), 0):
            winners[key] = source
    return winners


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


def time_model_cpu_snapshot_count(output):
    return sum(1 for line in STRIP_ANSI.sub('', output).splitlines()
               if re.match(r'^CPU\*\s{2,}[\d.]+\s+', line.strip()))


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
        # Live output may contain several cumulative display snapshots.  A
        # runner stall can let the workload straddle the first tick, so verify
        # the most complete observation rather than whichever row printed
        # first.  Trace-file output has one snapshot and is unchanged.
        total = max(e['total_ms'] for e in sleep_ev)
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
        total = max(e['total_ms'] for e in lock_ev)
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
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_live_")
    os.chmod(trace_dir, 0o755)
    tracer_stdout = tempfile.TemporaryFile()
    tracer_stderr = tempfile.TemporaryFile()
    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "-T", trace_dir,
         "--interval", "12", "--duration", "60",
         "--view", "system_event"],
        stdout=tracer_stdout, stderr=tracer_stderr)
    # A fixed 2.5s launch delay raced a cold BPF load on overloaded runners:
    # the workload could mostly finish before capture was ready.  The startup
    # control socket is created only after discovery/attach, so wait for that
    # real boundary instead. The 180s hard cap covers the observed loaded
    # startup even though controlled observations now receive independent
    # budgets; the product retains a separate finite outer bound.
    tracer_ready = wait_for_control_socket(tracer, trace_dir, timeout_s=180)
    check(tracer_ready,
          f"live/{mode}: tracer reached the post-attach startup boundary")
    try:
        wl.fire(sleep_s=3)
        time.sleep(6.5)
        wl.release()
        # Wait for a display snapshot that carries the asserted lower bounds,
        # not merely an early snapshot where both rows have just appeared.
        # The second tick is bounded headroom for the observed ~20s runner
        # stall.  If accounting is broken and the totals never materialize,
        # the unchanged assertions below still fail.
        wait_for_observation(
            tracer,
            lambda: (lambda events: (
                max((e['total_ms'] for e in events
                     if e['name'] == 'Timeout:PgSleep'), default=0) >=
                (100 if core else 1200) and
                max((e['total_ms'] for e in events
                     if e['name'] == 'Lock:relation'), default=0) >=
                (100 if core else 4000)
            ))(parse_system_events(temp_output_text(tracer_stdout))),
            30)
    finally:
        terminate_and_wait(tracer)
        wl.stop()

    out = STRIP_ANSI.sub('', temp_output_text(tracer_stdout))
    err = temp_output_text(tracer_stderr)
    tracer_stdout.close()
    tracer_stderr.close()
    subprocess.run(["rm", "-rf", trace_dir])
    events = parse_system_events(out)
    # THE zero-event guard: an empty view here means capture is dead (#30).
    check(len(events) > 0,
          "live view shows at least one event" if events else
          f"live view shows at least one event (stderr tail: {err[-300:]!r})")
    # ESC-3: the live view must now match the trace view within the tight
    # tolerance (no sampler+exact double-count during escalation). core mode
    # relaxes the pg_sleep magnitude floor where watchpoints don't fire.
    assert_wait_events(events, f"live/{mode}", core=core)


def phase_live_query_event(pm_pid, mode, pg_major, core=False):
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

    # pgbench starts FIRST and remains live until query output is observed, so
    # a slow validator cannot let the fixed workload expire before capture.
    pgbench_tag = f"pgwt_query_{secrets.token_hex(8)}"
    pgbench_env = os.environ.copy()
    pgbench_env["PGAPPNAME"] = pgbench_tag
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "4",
         # Safety cap exceeds the 180s startup + 40s output deadlines; the
         # process is explicitly stopped as soon as query output is proven.
         "-T", "300"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env=pgbench_env)
    pgbench_ready = wait_for_tagged_backend_count(
        pgbench_tag, lambda count: count >= 4, timeout_s=30,
        active_only=True)
    check(pgbench_ready is not None,
          "query-attribution workload is active before tracer attach")

    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_query_")
    os.chmod(trace_dir, 0o755)
    tracer_stdout = tempfile.TemporaryFile()
    tracer_stderr = tempfile.TemporaryFile()
    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "-T", trace_dir, "--interval", "10", "--duration", "60",
         "--view", "query_event"],
        stdout=tracer_stdout, stderr=tracer_stderr)
    try:
        tracer_ready = wait_for_control_socket(
            tracer, trace_dir, timeout_s=180)
        check(tracer_ready,
              f"query/{mode}: tracer reached the post-attach startup boundary")
        wait_for_observation(
            tracer,
            lambda: len(parse_query_event_ids(
                temp_output_text(tracer_stdout))) > 0,
            timeout_s=40)
    finally:
        terminate_and_wait(pgbench)
        terminate_and_wait(tracer)

    out = STRIP_ANSI.sub('', temp_output_text(tracer_stdout))
    err = temp_output_text(tracer_stderr)
    tracer_stdout.close()
    tracer_stderr.close()
    subprocess.run(["rm", "-rf", trace_dir])
    view_ids = parse_query_event_ids(out)
    truth = pgss_query_ids()
    check(len(truth) > 0,
          f"pg_stat_statements recorded the pgbench queries ({len(truth)} ids)")
    matched = view_ids & truth
    if pg_major == 13 and mode == "tiered":
        check(len(view_ids) > 0,
              f"PG13 sampled query_event has synthetic grouping keys "
              f"(view={len(view_ids)}; intentionally not joined to pgss ids)"
              + ("" if view_ids else f" (stderr tail: {err[-300:]!r})"))
    elif core:
        check(not view_ids or len(matched) > 0,
              f"[core] query_event view ids, if any, cross-check against "
              f"pg_stat_statements — no phantoms (view={len(view_ids)}, "
              f"pgss={len(truth)}, matched={len(matched)})")
    else:
        check(len(matched) > 0,
              f"query_event view ids cross-check against pg_stat_statements "
              f"(view={len(view_ids)}, pgss={len(truth)}, matched={len(matched)}) "
              f"[PR #31 regression]")


def phase_trace_file(pm_pid, mode, pg_major, core=False):
    print(f"--- Phase 3: written trace file (pgwt-server read, --mode {mode}) ---")
    trace_dir = tempfile.mkdtemp(prefix=f"pgwt_smoke_{mode}_")
    os.chmod(trace_dir, 0o755)

    pg13_sampled_text = pg_major == 13 and mode == "tiered"

    synthetic_witness_text = (
        "SELECT sum(i),?,? FROM generate_series(?,?)AS g(i)")

    def synthetic_text_persisted():
        """Observe the PG13 worker's durable output, tolerating a partial tail."""
        try:
            with open(os.path.join(trace_dir, "query_texts.jsonl"),
                      encoding="utf-8") as qtf:
                for line in qtf:
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if (rec.get("s") == "synthetic" and
                            rec.get("v") == "pg13-synth-v1" and
                            rec.get("t") == synthetic_witness_text):
                        return True
        except OSError:
            pass
        return False

    def pgbench_text_persisted():
        """A PG13 synthetic record tied specifically to pgbench tables."""
        try:
            with open(os.path.join(trace_dir, "query_texts.jsonl"),
                      encoding="utf-8") as qtf:
                for line in qtf:
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if (rec.get("s") == "synthetic" and
                            rec.get("v") == "pg13-synth-v1" and
                            "pgbench_" in rec.get("t", "")):
                        return True
        except OSError:
            pass
        return False

    wl = Workload()
    pgbench = None
    synthetic_source = None
    synthetic_tag = None
    synthetic_backend_pid = None
    synthetic_backend_start = None
    tracer = None
    pgbench_truth = set()
    persisted_pgbench_matches = set()
    query_match_deadline = None

    def trace_pgss_match_persisted():
        """Return real sampled query IDs only after they reach the trace.

        Aggregate sample progress is not a query-attribution barrier: on a
        starved runner the first serviced tick can sample an unrelated backend,
        after which stopping pgbench removes the only repeatable query-ID
        producer.  Poll the durable trace against independent pgss truth while
        that producer is still alive.  This is deliberately tiered/PG14+ only;
        PG13 uses its synthetic-text barrier and full mode was not flaky.
        """
        nonlocal persisted_pgbench_matches
        remaining = query_match_deadline - time.monotonic()
        if remaining <= 0:
            return None
        try:
            # One attempt may span the observed 20s runner stall, but can
            # never outlive the enclosing 60s barrier.
            qresp = server_query(trace_dir, "top_queries",
                                 timeout_s=min(25, remaining))
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError,
                BrokenPipeError):
            return None
        trace_ids = set()
        for row in qresp.get("rows", []):
            try:
                query_id = int(str(row.get("query_id", "0")), 10)
            except ValueError:
                continue
            if query_id:
                trace_ids.add(query_id & 0xFFFFFFFFFFFFFFFF)
        persisted_pgbench_matches = trace_ids & pgbench_truth
        return persisted_pgbench_matches

    def cleanup_phase():
        # Stop producers first: the tracer's final drain is deliberately
        # exhaustive and must not race a pgbench process still adding events.
        if synthetic_tag is not None:
            quoted_synthetic_tag = synthetic_tag.replace("'", "''")
            try:
                psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                     f"WHERE application_name = '{quoted_synthetic_tag}'",
                     timeout=30)
            except subprocess.TimeoutExpired:
                pass
            stopped = wait_for_tagged_backend_count(
                synthetic_tag, lambda count: count == 0, timeout_s=10)
            # SQL itself can be starved beyond the measured ~20s stall. The
            # exact tagged PID plus its /proc starttime makes the fallback
            # targeted and PID-reuse safe; never leave the 2B-row witness on
            # the shared validation box after an early assertion/exception.
            if (stopped is None and synthetic_backend_pid and
                    proc_start_ticks(synthetic_backend_pid) ==
                    synthetic_backend_start):
                try:
                    os.kill(synthetic_backend_pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                wait_for_tagged_backend_count(
                    synthetic_tag, lambda count: count == 0, timeout_s=10)
        if synthetic_source is not None:
            terminate_and_wait(synthetic_source)
        if pgbench is not None:
            terminate_and_wait(pgbench)
        if tracer is not None:
            terminate_and_wait(tracer)
        wl.stop()
        subprocess.run(["rm", "-rf", trace_dir])

    try:
        wl.open_sessions(discover_pids=(mode == "full"))
        if mode == "full":
            check(set(wl.backend_pids) == {"sleeper", "holder", "waiter"},
                  f"trace/full: controlled backend identities are known "
                  f"(pids={wl.backend_pids})")
        psql("SELECT pg_stat_statements_reset()")
        # pgbench starts first so its backends are found by the initial scan
        # (the fork-after-attach path is covered by phase 4).
        pgbench_tag = f"pgwt_trace_{secrets.token_hex(8)}"
        pgbench_env = os.environ.copy()
        pgbench_env["PGAPPNAME"] = pgbench_tag
        pgbench = subprocess.Popen(
            ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "2",
             # Safety cap exceeds every bounded startup/progress/text poll;
             # the process is explicitly stopped after pgbench-specific
             # persisted text is proven.
             "-T", "420"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=pgbench_env)
        pgbench_ready = wait_for_tagged_backend_count(
            pgbench_tag, lambda count: count >= 2, timeout_s=30,
            active_only=True)
        check(pgbench_ready is not None,
              ("PG13 sampled-text" if pg13_sampled_text else "trace query") +
              " workload is active before tracer attach")
        if mode == "tiered" and pg_major >= 14 and not core:
            # Freeze independent pgbench-only ground truth before the tracer
            # exists. The lookup excludes pg_stat_statements itself, so neither
            # the barrier nor the final assertion can match their observer SQL.
            pgbench_truth = wait_for_observation(
                pgbench, pgbench_pgss_query_ids, 30, interval_s=0.5) or set()
            check(bool(pgbench_truth),
                  f"trace/tiered: pg_stat_statements recorded pgbench ground "
                  f"truth before tracer startup (ids={len(pgbench_truth)})")
        if pg13_sampled_text:
            # PG13's st_activity_raw layout is validated from coherent live
            # rows during startup. pgbench micro-statements can all cross a
            # command boundary during that scan under load, making validation
            # correctly degrade and leaving no activity-text source. Keep one
            # stable, exact text witness active across tracer attach so the
            # test establishes the capability it later asserts.
            synthetic_tag = f"pgwt_synth_{secrets.token_hex(8)}"
            synthetic_env = os.environ.copy()
            synthetic_env["PGAPPNAME"] = synthetic_tag
            synthetic_env["PGOPTIONS"] = "-c max_parallel_workers_per_gather=0"
            synthetic_source = subprocess.Popen(
                ["psql", "-U", "postgres", "-d", "postgres", "-c",
                 "SELECT sum(i), 'literal', 8675309 "
                 "FROM generate_series(1, 2000000000) AS g(i) "
                 "/* pgwt synthetic smoke */"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=synthetic_env)
            synthetic_backend_pid = wait_for_tagged_active_backend(
                synthetic_tag, timeout_s=30)
            synthetic_backend_start = proc_start_ticks(synthetic_backend_pid)
            check(synthetic_backend_pid is not None and
                  synthetic_backend_start is not None,
                  "PG13 persistent synthetic-text witness is active across "
                  "tracer attach")
        tracer = subprocess.Popen(
            [TRACER, "--mode", mode, "--pid", str(pm_pid),
             "-T", trace_dir,
             # Hard cap only: the phase explicitly terminates after its
             # post-workload control barrier. 360s covers the complete bounded
             # sequence (five 30s control/lifecycle polls, PG13's two 60s text
             # polls, and the controlled workload) for every version/mode.
             "--duration", "360", "--quiet",
             "--interval", "5"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        tracer_ready = wait_for_control_socket(
            tracer, trace_dir, timeout_s=180)
        check(tracer_ready,
              ("PG13 sampled-text" if pg13_sampled_text else
               f"trace/{mode}") +
              " tracer reached the post-attach startup boundary")
        before_workload_metrics = wait_for_control_observation(
            tracer, trace_dir, "metrics",
            lambda value: value.get("ok") is True, timeout_s=30)
        check(before_workload_metrics.get("ok") is True,
              f"trace/{mode}: baseline capture metrics are observable")
        progress_field = "events_total" if mode == "full" else "samples_total"
        query_capture_metrics = wait_for_control_observation(
            tracer, trace_dir, "metrics",
            lambda value: (
                value.get(progress_field, 0) >
                before_workload_metrics.get(progress_field, 0)),
            timeout_s=30)
        check(query_capture_metrics.get(progress_field, 0) >
              before_workload_metrics.get(progress_field, 0),
              f"trace/{mode}: pre-existing query workloads advanced capture")
        if pg13_sampled_text:
            # Aggregate samples can be supplied by the stable layout witness;
            # require a durable pgbench-table statement before stopping
            # pgbench so that witness cannot mask broken OLTP attribution.
            pgbench_text_ready = wait_for_observation(
                tracer, pgbench_text_persisted, 60)
            check(pgbench_text_ready,
                  "PG13 sampled trace persisted pgbench-specific normalized "
                  "text before pgbench teardown")
        elif mode == "tiered" and pg_major >= 14 and not core:
            # Three observed ~20s event-loop stalls still leave multiple
            # opportunities for a sampled tick and worker flush.  The exact
            # pgss intersection must appear; a dead/phantom attribution path
            # remains absent until the bounded deadline and fails here.
            query_match_deadline = time.monotonic() + 60
            matched_ready = wait_for_observation(
                tracer, trace_pgss_match_persisted, 60, interval_s=1)
            check(bool(matched_ready),
                  f"trace/tiered: a pg_stat_statements query ID persisted "
                  f"before producer teardown (matched="
                  f"{len(persisted_pgbench_matches)})")
        # Query-attribution coverage has now crossed the capture boundary.
        # Stop pgbench before the PG13 durable-text witness: its microstatement
        # churn can otherwise monopolize a severely starved sampler/worker.
        # Full mode also needs the high-rate NMI producer gone before its exact
        # controlled-wait interval.
        terminate_and_wait(pgbench)
        query_workload_stopped = wait_for_tagged_backend_count(
            pgbench_tag, lambda count: count == 0, timeout_s=30)
        check(query_workload_stopped is not None,
              f"trace/{mode}: query producers stopped after capture proof")
        if pg13_sampled_text:
            # The stable pre-attach row removes the validation race; bounded
            # polling still allows three observed 20s event-loop stalls for a
            # coherent sampled tick and durable worker flush. A broken/absent
            # text path reaches the deadline and fails every exact assertion.
            text_ready = wait_for_observation(
                tracer, synthetic_text_persisted, 60)
            check(text_ready,
                  "PG13 sampled trace persisted synthetic text before "
                  "query-workload teardown")
            quoted_synthetic_tag = synthetic_tag.replace("'", "''")
            psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                 f"WHERE application_name = '{quoted_synthetic_tag}'")
            terminate_and_wait(synthetic_source)
            synthetic_stopped = wait_for_tagged_backend_count(
                synthetic_tag, lambda count: count == 0, timeout_s=30)
            check(synthetic_stopped is not None,
                  "PG13 persistent synthetic-text witness stopped before "
                  "controlled waits")
        # Full-tier watchpoints run from perf-event/NMI context.  The shared
        # kernel ring can fail a reservation on lock contention even with free
        # capacity, so a high-rate pgbench burst must not overlap the small
        # deterministic waits whose exact magnitudes this phase verifies.
        # Preserve query-attribution coverage above and open the controlled
        # interval only after all high-rate producers are gone.
        # ringbuf_drops_total increments in producer BPF context, so this
        # snapshot is precise even while userspace drains older records.
        controlled_baseline_metrics = wait_for_control_observation(
            tracer, trace_dir, "metrics",
            lambda value: value.get("ok") is True, timeout_s=30)
        check(controlled_baseline_metrics.get("ok") is True,
              f"trace/{mode}: controlled-interval loss baseline is observable")
    except BaseException:
        cleanup_phase()
        raise
    try:
        wl.fire(sleep_s=3)
        time.sleep(8)
        wl.release()           # lock wait ends (~10s) inside the trace window
        post_workload_metrics = wait_for_control_observation(
            tracer, trace_dir, "metrics",
            lambda value: (
                value.get(progress_field, 0) >
                controlled_baseline_metrics.get(progress_field, 0)),
            timeout_s=30)
        check(post_workload_metrics.get(progress_field, 0) >
              controlled_baseline_metrics.get(progress_field, 0),
              f"trace/{mode}: daemon consumed post-workload events before "
              f"teardown")
        if mode == "full":
            drops_before = controlled_baseline_metrics.get(
                "ringbuf_drops_total", -1)
            drops_after = post_workload_metrics.get(
                "ringbuf_drops_total", -2)
            check(drops_before >= 0 and drops_after >= drops_before,
                  f"trace/full: event-ring loss counter is observable and "
                  f"monotonic (drops={drops_before}->{drops_after})")
            if drops_after != drops_before:
                print(f"  INFO: trace/full: global ring drops advanced "
                      f"{drops_before}->{drops_after}; PID-scoped exact "
                      f"checks below determine whether controlled records "
                      f"were lost")
        if tracer.poll() is None:
            tracer.terminate()
        _, stderr = tracer.communicate(timeout=45)
        err = stderr.decode('utf-8', errors='replace')

        clean_finalization = tracer.returncode == 0
        check(clean_finalization,
              f"trace/{mode}: single-shot tracer finalized cleanly "
              f"(rc={tracer.returncode})" if clean_finalization else
              f"trace/{mode}: single-shot tracer finalized cleanly "
              f"(rc={tracer.returncode}, stderr tail={err[-300:]!r})")
        if not clean_finalization:
            return

        resp = server_query(trace_dir, "top_events")
        rows = resp.get("rows", [])
        check(len(rows) > 0,
              f"trace file has events (top_events rows={len(rows)})" if rows
              else f"trace file has events (daemon stderr tail: {err[-300:]!r})")
        events = [{'name': r.get('name'), 'count': r.get('count', 0),
                   'total_ms': r.get('total_ms', 0.0)} for r in rows]
        assert_wait_events(events, f"trace/{mode}", core=core)

        if mode == "full" and set(wl.backend_pids) == {
                "sleeper", "holder", "waiter"}:
            # ringbuf_drops_total is global: background checkpointer/autovacuum
            # events can lose an NMI-context reservation without touching this
            # phase's records. Prove the real invariant directly—each tagged
            # backend contributes its one exact wait, at the exact magnitude.
            sleeper_resp = server_query(
                trace_dir, "top_events",
                extra={"filters": {"pid": wl.backend_pids["sleeper"]}})
            waiter_resp = server_query(
                trace_dir, "top_events",
                extra={"filters": {"pid": wl.backend_pids["waiter"]}})
            sleeper_rows = [
                row for row in sleeper_resp.get("rows", [])
                if row.get("name") == "Timeout:PgSleep"]
            waiter_rows = [
                row for row in waiter_resp.get("rows", [])
                if row.get("name") == "Lock:relation"]
            sleeper_count = sum(int(row.get("count", 0))
                                for row in sleeper_rows)
            waiter_count = sum(int(row.get("count", 0))
                               for row in waiter_rows)
            sleeper_ms = sum(float(row.get("total_ms", 0.0))
                             for row in sleeper_rows)
            waiter_ms = sum(float(row.get("total_ms", 0.0))
                            for row in waiter_rows)
            check(sleeper_count == 1,
                  f"trace/full: controlled sleeper has exactly one PgSleep "
                  f"record (pid={wl.backend_pids['sleeper']}, "
                  f"count={sleeper_count})")
            check(1200 <= sleeper_ms <= 6000,
                  f"trace/full: controlled sleeper PgSleep is ~3000ms "
                  f"(pid={wl.backend_pids['sleeper']}, total={sleeper_ms:.0f}ms)")
            # A single SELECT can expose more than one relation-lock edge in
            # PostgreSQL (observed count=2); the long blocking interval is the
            # invariant. If that record is lost, the unchanged 4s duration
            # floor below cannot be met by the additional short edge.
            check(waiter_count >= 1,
                  f"trace/full: controlled waiter has relation-lock records "
                  f"(pid={wl.backend_pids['waiter']}, "
                  f"count={waiter_count})")
            check(4000 <= waiter_ms <= 25000,
                  f"trace/full: controlled waiter lock is complete "
                  f"(pid={wl.backend_pids['waiter']}, total={waiter_ms:.0f}ms)")

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
        truth = (pgbench_truth
                 if mode == "tiered" and pg_major >= 14 and not core
                 else pgss_query_ids())
        matched = trace_ids & truth
        text_rows = [r for r in qresp.get("rows", []) if r.get("text")]
        if pg_major == 13 and mode == "tiered":
            check(len(trace_ids) > 0,
                  f"PG13 trace carries synthetic query grouping keys "
                  f"(trace={len(trace_ids)}; intentionally not pgss ids)")
            check(len(text_rows) > 0,
                  f"PG13 synthetic groups carry normalized activity text "
                  f"(text rows={len(text_rows)})")
            sidecar_path = os.path.join(trace_dir, "query_texts.jsonl")
            sidecar = []
            try:
                with open(sidecar_path, encoding="utf-8") as qtf:
                    sidecar = [json.loads(line) for line in qtf if line.strip()]
            except (OSError, json.JSONDecodeError):
                sidecar = []
            # Tiered mode may escalate and append FULL/raw text for real PG13
            # query IDs. Only the winning synthetic source carries synth quality.
            winning_sources = winning_query_text_sources(sidecar)
            sampled_rows = [
                row for row in text_rows
                if winning_sources.get(query_text_key(row)) == "synthetic"
            ]
            tagged_sampled_rows = [
                row for row in sampled_rows
                if row.get("attribution_quality") == "pg13-synth-v1"
            ]
            check(len(sampled_rows) > 0 and
                  len(tagged_sampled_rows) == len(sampled_rows),
                  f"every PG13 synthetic-source text row exposes "
                  f"pg13-synth-v1 attribution quality "
                  f"(synthetic={len(sampled_rows)}, "
                  f"tagged={len(tagged_sampled_rows)}, "
                  f"other-source={len(text_rows) - len(sampled_rows)})")
            synth_records = [
                rec for rec in sidecar
                if rec.get("s") == "synthetic" and
                   rec.get("v") == "pg13-synth-v1" and
                   int(rec.get("d", 0)) > 0 and int(rec.get("u", 0)) > 0
            ]
            check(len(synth_records) > 0,
                  "PG13 sidecar pins synthetic source/version and db/user context")
            pgbench_records = [
                rec for rec in synth_records if "pgbench_" in rec.get("t", "")
            ]
            check(len(pgbench_records) > 0,
                  "PG13 sidecar retains a pgbench-specific sampled record")
            normalized_ok = any("?" in rec.get("t", "")
                                for rec in synth_records) and all(
                "/*" not in rec.get("t", "") and
                "--" not in rec.get("t", "") and
                not re.search(r"'(?:''|[^'])*'|\b\d+(?:\.\d+)?\b",
                              rec.get("t", ""))
                for rec in synth_records)
            check(normalized_ok,
                  "PG13 sampled text strips comments and literal constants")
        elif core:
            # --core (nested container): live and trace query-id capture are both
            # best-effort under sampled, PG13 header-offset attribution. Presence
            # is therefore not gated for either, but each keeps the phantom-id
            # guard: any id that DOES appear must be real (matched). The hosted
            # runner + live boxes remain strict through the non-core assertions.
            check(not trace_ids or len(matched) > 0,
                  f"[core] trace query ids, if any, cross-check against "
                  f"pg_stat_statements — no phantoms (trace={len(trace_ids)}, "
                  f"pgss={len(truth)}, matched={len(matched)})")
        else:
            check(len(matched) > 0,
                  f"trace file query attribution cross-checks against "
                  f"pg_stat_statements (trace={len(trace_ids)}, pgss={len(truth)}, "
                  f"matched={len(matched)}) [PR #31 regression]")
            if mode == "tiered" and pg_major >= 14:
                check(len(text_rows) > 0,
                      f"PG{pg_major} sampled query text resolved lazily from "
                      f"pg_stat_statements (text rows={len(text_rows)})")
    finally:
        cleanup_phase()


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
    """N tagged psql sessions for the sampled-CPU live tests.

    fire() deliberately exercises command boundaries for the command-gate
    coverage phase. fire_continuous() is the saturation-policy stimulus: one
    bounded, non-spilling command that stays on CPU for the whole window.
    """

    def __init__(self, n):
        # The process PID is shared by Phase 5 and Phase 6. Add a per-instance
        # nonce so even overlapping cleanup cannot make one phase discover the
        # other's sessions.
        self.application_prefix = (
            f"pgwt_cpu_storm_{os.getpid()}_{secrets.token_hex(8)}_")
        self.application_names = [f"{self.application_prefix}{i}"
                                  for i in range(n)]
        self.sessions = [subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True,
            env={**os.environ, "PGAPPNAME": app_name})
            for app_name in self.application_names]
        time.sleep(1.5)   # connected (and, pre-attach, scanned)

    def backend_pids(self):
        quoted_names = [name.replace("'", "''")
                        for name in self.application_names]
        application_names = ",".join(f"'{name}'" for name in quoted_names)
        rows = psql(
            "SELECT pid FROM pg_stat_activity "
            f"WHERE application_name IN ({application_names}) "
            "ORDER BY application_name")
        return [int(row) for row in rows.splitlines()
                if re.fullmatch(r'\d+', row)]

    def fire(self, reps=400):
        stmt = "SELECT count(*) FROM generate_series(1,300000);\n"
        for s in self.sessions:
            s.stdin.write(stmt * reps)
            s.stdin.flush()

    def fire_continuous(self, duration_s=15):
        """One CPU-only command per backend for a fixed wall-clock window."""
        duration_s = int(duration_s)
        if duration_s <= 0:
            raise ValueError("duration_s must be positive")
        stmt = (
            "DO $pgwt_cpu$\n"
            "DECLARE\n"
            f"  stop_at timestamptz := clock_timestamp() + "
            f"interval '{duration_s} seconds';\n"
            "  x bigint := 0;\n"
            "BEGIN\n"
            "  WHILE clock_timestamp() < stop_at LOOP\n"
            "    x := (x + 1) % 1000003;\n"
            "  END LOOP;\n"
            "  PERFORM x;\n"
            "END\n"
            "$pgwt_cpu$;\n")
        for s in self.sessions:
            s.stdin.write(stmt)
            s.stdin.flush()

    def stop(self):
        for s in self.sessions:
            s.terminate()
            try:
                s.wait(timeout=5)
            except subprocess.TimeoutExpired:
                s.kill()
        cleanup_stale_backends()


def sample_active_sessions(expected_pids, target_samples=12,
                           deadline_s=30, interval=0.5):
    """Collect a bounded quorum for one fixed pg_stat_activity population."""
    counts = []
    pid_list = ",".join(str(int(pid)) for pid in expected_pids)
    deadline = time.monotonic() + deadline_s
    while len(counts) < target_samples and time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            out = psql("SELECT count(*) FROM pg_stat_activity "
                       "WHERE state = 'active' "
                       "AND backend_type = 'client backend' "
                       f"AND pid IN ({pid_list})",
                       timeout=min(5, max(1, remaining)))
        except subprocess.TimeoutExpired:
            out = ""
        if re.fullmatch(r'\d+', out or ''):
            counts.append(int(out))
            if len(counts) >= target_samples:
                break
        time.sleep(min(interval, max(0.0, deadline - time.monotonic())))
    return counts


def cpu_storm_state(expected_pids, timeout=10):
    """Exact pg_stat_activity state for the named Phase-6 backends."""
    if not expected_pids:
        return []
    pid_list = ",".join(str(pid) for pid in expected_pids)
    out = psql(
        "SELECT pid, state, COALESCE(wait_event_type, 'CPU'), "
        "COALESCE(wait_event, 'CPU'), "
        "(extract(epoch FROM state_change) * 1000000000)::bigint "
        "FROM pg_stat_activity "
        f"WHERE pid IN ({pid_list}) ORDER BY pid", timeout=timeout)
    rows = []
    for line in out.splitlines():
        fields = line.split('|')
        if (len(fields) == 5 and re.fullmatch(r'\d+', fields[0]) and
                re.fullmatch(r'\d+', fields[4])):
            rows.append((int(fields[0]), fields[1], fields[2], fields[3],
                         int(fields[4])))
    return rows


def cpu_storm_state_is_sustained(rows, expected_pids):
    return (cpu_storm_state_is_active(rows, expected_pids)
            and all(cpu_storm_row_is_waitless(row) for row in rows))


def cpu_storm_state_is_active(rows, expected_pids):
    return ({row[0] for row in rows} == set(expected_pids)
            and len(rows) == len(expected_pids)
            and all(row[1] == 'active' for row in rows))


def cpu_storm_row_is_waitless(row):
    return row[1:4] == ('active', 'CPU', 'CPU')


def cpu_storm_state_is_idle(rows, expected_pids):
    return ({row[0] for row in rows} == set(expected_pids)
            and len(rows) == len(expected_pids)
            and all(row[1:4] == ('idle', 'Client', 'ClientRead')
                    for row in rows))


def sample_cpu_storm_state(expected_pids, duration_s, interval=0.5,
                           timeout=10):
    snapshots = []
    end = time.time() + duration_s
    while time.time() < end:
        snapshots.append(cpu_storm_state(expected_pids, timeout=timeout))
        time.sleep(interval)
    return snapshots


def wait_for_cpu_storm_state(proc, expected_pids, timeout_s):
    def observe():
        rows = cpu_storm_state(expected_pids, timeout=timeout_s)
        return rows if cpu_storm_state_is_sustained(
            rows, expected_pids) else None

    return wait_for_observation(proc, observe, timeout_s)


def aas_bucket_total(bucket):
    return sum(v for k, v in bucket.items()
               if k not in ("t", "total", "cat") and isinstance(v, (int, float)))


def ctl_request(trace_dir, obj, timeout=5):
    """One arbitrary JSON request over the daemon control socket."""
    import socket
    path = os.path.join(trace_dir, "pgwt.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
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


def ctl_query(trace_dir, cmd, timeout=5):
    """One JSON request over the daemon control socket."""
    return ctl_request(trace_dir, {"cmd": cmd}, timeout=timeout)


def wait_for_control_observation(proc, trace_dir, cmd, predicate,
                                 timeout_s=30, interval_s=0.05):
    """Bounded-poll a control response, tolerating transient runner stalls."""
    deadline = time.monotonic() + timeout_s
    last = {}
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or proc.poll() is not None:
            return last
        if os.path.exists(os.path.join(trace_dir, "pgwt.sock")):
            try:
                last = ctl_query(trace_dir, cmd,
                                 timeout=min(5, max(0.1, remaining)))
                if predicate(last):
                    return last
            except (OSError, ValueError, json.JSONDecodeError):
                pass
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return last
        time.sleep(min(interval_s, remaining))


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

    subprocess.run(["rm", "-rf", trace_dir])


def phase_pure_cpu_straddle(pm_pid, mode):
    """T8 (docs/ROADMAP_AND_STATUS.md) — THE measured-CPU acceptance
    test. A PL/pgSQL loop over NO data fires ZERO wait events for its entire
    life, so the watchpoint never emits a boundary. Pre-T8 this produced an
    EMPTY trace in --mode full (the on-CPU stretch was never closed) and 0%
    live CPU (symptoms #1/#2). Post-T8 the measured se.sum_exec_runtime
    accumulator makes it visible LIVE (open-interval schedstat delta) and the
    terminal flush writes its on-CPU stretch to the trace at daemon shutdown.

    Asserted in BOTH tiers: CPU present in the live view AND in the trace. Full
    mode's measured CPU is cross-checked against the backend's /proc on-CPU
    delta (±20%). Sampled mode is ASH demand (we==0 includes runnable time), so
    its magnitude is cross-checked against the command's active wall interval;
    comparing it with consumed /proc CPU falsely fails under CPU contention.
    The loop count is huge so it straddles capture end on any box; it starts
    BEFORE the tracer so its command start-edge is already past at attach."""
    print(f"--- Phase: PURE-CPU straddle, --mode {mode} (no waits — empty-trace "
          f"repro) ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_purecpu_")
    os.chmod(trace_dir, 0o755)

    hog_app = f"pgwt_purecpu_{os.getpid()}_{secrets.token_hex(8)}"
    hog = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True,
        env={**os.environ, "PGAPPNAME": hog_app})
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
    be_pid = wait_for_tagged_active_backend(hog_app, timeout_s=30)
    check(be_pid is not None,
          f"pure-CPU straddle backend is active before tracer attach "
          f"(--mode {mode}, pid={be_pid})")

    argv = [TRACER, "--mode", mode, "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "35", "--interval", "5",
            "--view", "time_model"]
    if mode == "tiered":
        argv += ["--anomaly-aas-factor", "1000000"]   # keep it pure sampled

    oncpu_before = oncpu_after = None
    win_from = win_shutdown = 0
    natural_exit = False
    # Self-diagnosis (docs/ROADMAP_AND_STATUS.md "REOPENED 2026-08-04 — a
    # FOURTH straddle live-CPU mode exists"): this phase's live-CPU check is
    # the flake's only witness, so ALWAYS arm the daemon's shutdown state_map
    # dump (STATEDUMP:/STATEDUMP-META: stderr lines, pgwt_debug_dump_state_map
    # in src/daemon.c). A passing run keeps the dump inside the captured
    # stderr and prints nothing extra; a failing run surfaces it below, so
    # the next CI hit hands us the hog's actual end-state instead of another
    # inference cycle.
    tracer_stdout = tempfile.TemporaryFile()
    tracer_stderr = tempfile.TemporaryFile()
    tracer = subprocess.Popen(
        argv, stdout=tracer_stdout, stderr=tracer_stderr,
        env={**os.environ, "PGWT_DEBUG_DUMP_STATE": "1"})
    try:
        wait_for_control_socket(tracer, trace_dir, timeout_s=180)
        win_from = time.time_ns()
        oncpu_before = proc_oncpu_ns(be_pid) if be_pid else None  # ≈ the seed instant
        # A 12s process lifetime could end wholly inside an observed 20s runner
        # stall, leaving no display tick even though the terminal trace and
        # /proc accounting were correct.  Wait up to 35s for the live invariant
        # itself, then stop early and exercise the same terminal flush.
        wait_for_observation(
            tracer,
            lambda: parse_time_model(
                temp_output_text(tracer_stdout)).get('CPU*', 0.0),
            timeout_s=35)
        natural_exit = wait_for_natural_exit(tracer, timeout_s=50)
        win_shutdown = time.time_ns()
        oncpu_after = proc_oncpu_ns(be_pid) if be_pid else None    # ≈ the flush instant
    finally:
        if tracer.poll() is None:
            tracer.kill()
            tracer.wait(timeout=5)
        try:
            hog.stdin.write("\\q\n"); hog.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        hog.terminate()
        cleanup_stale_backends()
    out = STRIP_ANSI.sub('', temp_output_text(tracer_stdout))
    err = temp_output_text(tracer_stderr)
    tracer_stdout.close()
    tracer_stderr.close()

    check(natural_exit and tracer.returncode == 0,
          f"pure-CPU tracer reached its bounded natural shutdown "
          f"(--mode {mode}, rc={tracer.returncode})")

    # 1) LIVE VIEW: the periodic time_model print must show measured CPU>0.
    #    Pre-T8 this row was 0.0 for a waitless command (the exact symptom).
    live = parse_time_model(out)
    live_cpu = live.get('CPU*', 0.0)
    if live_cpu <= 0.0:
        # THE flake fired (see the Popen env above): print the hog pid and
        # the daemon's shutdown state_map dump BEFORE check() records the
        # failure, so the CI log itself answers which end-state produced
        # live CPU* = 0. Raw daemon lines, unfiltered by content — the dump
        # is the diagnostic payload, never interpreted here.
        print(f"=== STRADDLE LIVE CPU* = 0 — shutdown state_map dump "
              f"(hog be_pid={be_pid}) ===")
        for _line in err.splitlines():
            if _line.startswith("STATEDUMP"):
                print(_line)
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

    # 3) MAGNITUDE: isolate the DO backend and integrate AAS CPU
    #    (avg-sessions × window = CPU/demand seconds). Querying through a
    #    shutdown margin includes the full-mode terminal-flush block.
    if be_pid and win_shutdown > win_from:
        win_to = win_shutdown + 3_000_000_000   # margin so the flush block is in range
        aresp = server_query(trace_dir, "aas",
                             extra={"from": win_from, "to": win_to,
                                    "buckets": 12, "filters": {"pid": be_pid}})
        abuckets = aresp.get("buckets", [])
        cpu_mean = (sum(b.get("cpu", 0.0) for b in abuckets) / len(abuckets)
                    if abuckets else 0.0)
        trace_win_ms = cpu_mean * (win_to - win_from) / 1e9 * 1000.0
        if mode == "full":
            proc_ms = ((oncpu_after - oncpu_before) / 1e6
                       if oncpu_before is not None and oncpu_after is not None
                       and oncpu_after > oncpu_before else 0.0)
            ratio = trace_win_ms / proc_ms if proc_ms > 0 else 0.0
            check(0.8 <= ratio <= 1.2,
                  f"pure-CPU straddle measured CPU {trace_win_ms:.0f}ms within "
                  f"±20% of /proc on-CPU {proc_ms:.0f}ms over "
                  f"[attach,shutdown] (ratio {ratio:.2f}, --mode {mode})")
        else:
            demand_ms = (win_shutdown - win_from) / 1e6
            ratio = trace_win_ms / demand_ms if demand_ms > 0 else 0.0
            check(0.8 <= ratio <= 1.2,
                  f"pure-CPU straddle sampled demand {trace_win_ms:.0f}ms "
                  f"within ±20% of active wall time {demand_ms:.0f}ms "
                  f"(ratio {ratio:.2f}; runnable time intentionally counts)")
    else:
        # Never silently pass the magnitude gate — record why it could not run.
        check(be_pid is not None,
              f"pure-CPU straddle: located the DO backend for the /proc "
              f"cross-check (pid={be_pid}, before={oncpu_before}, "
              f"after={oncpu_after})")

    subprocess.run(["rm", "-rf", trace_dir])


def run_event_ring_stall_capture(pm_pid, extra_env):
    """Run the mode-4 forced-stall workload and return all three witnesses.

    One waitless DO backend supplies the straddling CPU interval. Eight
    backends emit pg_sleep transitions faster than a deliberately slowed
    event callback can consume them. On an unbounded libbpf consume, the ring
    never becomes empty while those producers run, so the display timer cannot
    run. The delay hook is capped in the daemon; the SQL producers and both
    subprocess timeouts are bounded here as well.
    """
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_event_drain_")
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

    # Eight finite producers remove dependence on per-backend scheduling and
    # pg_sleep granularity; together they comfortably outrun the hook's
    # 50-callback/s consumer for the whole negative capture.
    floods = []
    for _ in range(8):
        flood = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True)
        flood.stdin.write(
            "SELECT pg_sleep(0.001) FROM generate_series(1, 20000);\n")
        flood.stdin.flush()
        floods.append(flood)
    time.sleep(0.3)

    argv = [TRACER, "--mode", "full", "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "8", "--interval", "2",
            "--view", "time_model"]
    env = {**os.environ, "PGWT_DEBUG_DUMP_STATE": "1",
           "PGWT_TEST_EVENT_CALLBACK_DELAY_US": "20000", **extra_env}
    tracer = subprocess.Popen(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env)
    hog_pid = None
    oncpu_before = oncpu_after = None
    win_from = time.time_ns()
    try:
        time.sleep(3.0)
        hog_pid = find_active_do_backend()
        producer_pids = psql(
            "SELECT string_agg(pid::text, ',' ORDER BY pid) "
            "FROM pg_stat_activity WHERE state='active' "
            "AND query LIKE 'SELECT pg_sleep(0.001)%'")
        print(f"    forced producer pids={producer_pids!r}; "
              f"psql_statuses={[proc.poll() for proc in floods]}")
        oncpu_before = proc_oncpu_ns(hog_pid) if hog_pid else None
        stdout, stderr = tracer.communicate(timeout=50)
        oncpu_after = proc_oncpu_ns(hog_pid) if hog_pid else None
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
        oncpu_after = proc_oncpu_ns(hog_pid) if hog_pid else None
    finally:
        for proc in (*floods, hog):
            try:
                proc.stdin.write("\\q\n")
                proc.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass
            proc.terminate()
        cleanup_stale_backends()

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    proc_ms = ((oncpu_after - oncpu_before) / 1e6
               if oncpu_before is not None and oncpu_after is not None
               and oncpu_after > oncpu_before else 0.0)
    hog_trace_ms = 0.0
    if hog_pid:
        win_to = time.time_ns() + 3_000_000_000
        aresp = server_query(trace_dir, "aas",
                             extra={"from": win_from, "to": win_to,
                                    "buckets": 12,
                                    "filters": {"pid": hog_pid}})
        buckets = aresp.get("buckets", [])
        cpu_mean = (sum(b.get("cpu", 0.0) for b in buckets) / len(buckets)
                    if buckets else 0.0)
        hog_trace_ms = cpu_mean * (win_to - win_from) / 1e9 * 1000.0

    subprocess.run(["rm", "-rf", trace_dir])
    return out, err, hog_pid, proc_ms, hog_trace_ms


def phase_event_ring_drain_budget(pm_pid):
    """Mode-4 deterministic regression: a busy event ring must yield to ticks.

    PGWT_TEST_EVENT_CALLBACK_DELAY_US makes a sustained transition producer
    outrun the trace callback. libbpf 1.4.7's ring_buffer__consume() reloads
    producer_pos until no new record arrived, so without the daemon-side work
    budget this single drain spans the capture: timer_ticks=0, live CPU*=0,
    while the terminal flush and /proc both show the waitless hog's CPU.
    """
    print("--- Phase: event-ring drain budget DETERMINISTIC "
          "(slow callback + sustained producer) ---")
    out, err, hog_pid, proc_ms, hog_trace_ms = run_event_ring_stall_capture(
        pm_pid, {})

    for line in err.splitlines():
        if (line.startswith("STATEDUMP-BLOCK:")
                or line.startswith("STATEDUMP-META:")
                or (line.startswith("STATEDUMP:")
                    and hog_pid is not None and f"pid={hog_pid} " in line)
                or "PGWT_TEST_EVENT_CALLBACK_DELAY_US" in line):
            print(f"    {line}")

    check("PGWT_TEST_EVENT_CALLBACK_DELAY_US=20000" in err,
          "bounded slow-callback hook active")
    check(re.search(r"STATEDUMP-BLOCK: op=event_ring_(?:consume|drain).*"
                    r"callbacks=[1-9][0-9]*", err) is not None,
          f"slow drain reports callback count and attribution "
          f"(stderr tail {err[-300:]!r})")
    timer_match = re.search(r"STATEDUMP-META: .*timer_ticks=(\d+)", err)
    timer_ticks = int(timer_match.group(1)) if timer_match else 0
    yield_match = re.search(r"STATEDUMP-META: .*event_drain_yields=(\d+)",
                            err)
    drain_yields = int(yield_match.group(1)) if yield_match else 0
    check(drain_yields > 0,
          f"event drain hit its callback budget and yielded "
          f"(yields={drain_yields})")
    check(timer_ticks > 0,
          f"display timer kept running during the forced drain "
          f"(timer_ticks={timer_ticks})")
    live_cpu = parse_time_model(out).get('CPU*', 0.0)
    check(live_cpu > 500.0,
          f"forced-drain straddler stays visible live: CPU*={live_cpu:.0f}ms")
    check(hog_pid is not None and proc_ms > 500.0,
          f"/proc confirms the waitless hog burned CPU "
          f"(pid={hog_pid}, delta={proc_ms:.0f}ms)")
    check(hog_trace_ms > 500.0,
          f"terminal trace flush preserves the hog CPU "
          f"(pid={hog_pid}, trace={hog_trace_ms:.0f}ms)")

    # Negative: same bounded fault and producer, but explicitly restore the
    # pre-fix unbounded libbpf call. It must recreate all three mode-4
    # witnesses on demand: no display tick/live CPU, positive /proc CPU, and a
    # correct terminal trace flush. This is intentionally test-only; the loud
    # hook warning prevents accidental production use.
    neg_out, neg_err, neg_hog_pid, neg_proc_ms, neg_trace_ms = \
        run_event_ring_stall_capture(
            pm_pid, {"PGWT_TEST_NO_EVENT_DRAIN_BUDGET": "1"})
    for line in neg_err.splitlines():
        if (line.startswith("STATEDUMP-BLOCK:")
                or line.startswith("STATEDUMP-META:")
                or "PGWT_TEST_NO_EVENT_DRAIN_BUDGET" in line):
            print(f"    NEGATIVE {line}")
    check("PGWT_TEST_NO_EVENT_DRAIN_BUDGET" in neg_err,
          "negative restored the pre-fix unbounded consume")
    neg_timer_match = re.search(
        r"STATEDUMP-META: .*timer_ticks=(\d+)", neg_err)
    neg_timer_ticks = (int(neg_timer_match.group(1))
                       if neg_timer_match else -1)
    check(neg_timer_ticks == 0,
          f"unbounded negative starves the display timer "
          f"(timer_ticks={neg_timer_ticks})")
    neg_live_cpu = parse_time_model(neg_out).get('CPU*', 0.0)
    check(neg_live_cpu == 0.0,
          f"unbounded negative reproduces live CPU*=0 "
          f"(CPU*={neg_live_cpu:.0f}ms)")
    check(neg_hog_pid is not None and neg_proc_ms > 500.0,
          f"negative /proc witness remains positive "
          f"(pid={neg_hog_pid}, delta={neg_proc_ms:.0f}ms)")
    check(neg_trace_ms > 500.0,
          f"negative terminal trace remains correct "
          f"(pid={neg_hog_pid}, trace={neg_trace_ms:.0f}ms)")


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


def phase_straddle_livecpu_deterministic(pm_pid):
    """DETERMINISTIC regression for the live-view CPU*=0 straddle flake (the
    on_watchpoint on_cpu_ts open, src/bpf/pg_wait_tracer.bpf.c). phase_pure_cpu_
    straddle above exercises the same edge, but the miss is a TIMING race: it
    needs the backend to (a) be in a wait at the attach instant so its seed
    leaves on_cpu_ts=0, AND (b) then run uninterrupted so no sched_switch opens
    on_cpu_ts — reproducing only ~2% of runs on 2-vCPU CI (PG13), never on a
    real box. The live read then measures exact==cpu_ns_total (flat) and reports
    CPU*=0 for the whole on-CPU stretch, while the terminal flush records it
    correctly (the observed "live=0 but trace+magnitude pass").

    This forces BOTH conditions so the miss is reproduced ON EVERY RUN:
      (a) the backend is in Timeout:PgSleep when the tracer seeds it (a real
          wait → current_wei != 0 → on_cpu_ts seeded 0), then runs a WAITLESS
          pure-CPU loop; and
      (b) PGWT_TEST_NO_SCHED_ONCPU makes sched_switch inert AND forces the seed
          on_cpu_ts to 0, so on_cpu_ts can be established ONLY by a watchpoint
          fire.
    The single pg_sleep→loop transition is that fire: WITH the fix it opens
    on_cpu_ts and the live view reads CPU* ≈ wall-since-loop-start; WITHOUT it
    on_cpu_ts stays 0 and CPU* is exactly 0 — the exact CI failure, made
    deterministic. Full mode only (needs watchpoints + cpu_accounting)."""
    print("--- Phase: straddle live-CPU DETERMINISTIC (pg_sleep→loop, "
          "sched_switch inert) ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_detcpu_")
    os.chmod(trace_dir, 0o755)

    # pg_sleep long enough that the tracer's BPF-load + scan seeds the backend
    # WHILE it is still in the wait (so current_wei != 0 → on_cpu_ts=0), then a
    # waitless pure-CPU loop (same int4-safe nested loop as the straddle hog).
    hog = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True)
    hog.stdin.write(
        "SELECT pg_sleep(8);\n"
        "DO $$ DECLARE x bigint := 0; BEGIN "
        "FOR o IN 1..100000 LOOP "
        "FOR i IN 1..1000000 LOOP x := x + i; END LOOP; x := 0; "
        "END LOOP; END $$;\n")
    hog.stdin.flush()
    time.sleep(0.3)   # backend is entering pg_sleep before the tracer attaches

    argv = [TRACER, "--mode", "full", "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "20", "--interval", "4",
            "--view", "time_model"]
    env = {**os.environ, "PGWT_TEST_NO_SCHED_ONCPU": "1"}
    tracer = subprocess.Popen(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env)
    try:
        stdout, stderr = tracer.communicate(timeout=55)
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

    # The hook must actually have armed (else the test proves nothing).
    check("PGWT_TEST_NO_SCHED_ONCPU" in err,
          "sched_switch on_cpu_ts open suppressed (test hook active)")
    # The payoff: the pg_sleep→loop watchpoint fire opened on_cpu_ts, so the
    # waitless loop's ongoing on-CPU stretch is measured LIVE — several seconds
    # of it by the final tick (its on-CPU time from pg_sleep end to trace end,
    # ~8s, independent of box speed since the loop is CPU-bound). WITHOUT the fix
    # the open interval reads 0 and CPU* is only a fixed ~tens-of-ms background
    # floor (idle bgworkers' brief closed on-CPU intervals, counted at wall);
    # measured 9027ms vs 66ms on PG13 — a >130x separation. The 2000ms gate sits
    # ~4x below the multi-second signal and ~30x above that floor: a
    # discriminator, not a tuned pass line. (A CPU/DB-Time ratio can't be used —
    # DB Time also carries the 8s pg_sleep wait, diluting even the fixed run to
    # ~27%.)
    live = parse_time_model(out)
    live_cpu = live.get('CPU*', 0.0)
    check(live_cpu > 2000.0,
          f"pg_sleep→loop straddler shows MULTI-SECOND live CPU* via the "
          f"watchpoint on_cpu_ts open (sched_switch inert): CPU* = {live_cpu:.0f}ms "
          f"(pre-fix ~0 loop CPU, only a ~tens-of-ms background floor; "
          f"stderr {err[-120:]!r})")

    subprocess.run(["rm", "-rf", trace_dir])


def run_stale_seed_capture(pm_pid, extra_env):
    """One tracer run for phase_stale_seed_sweep: a waitless pure-CPU hog
    already in flight at attach (the int4-safe nested DO loop), traced with
    PGWT_TEST_STALE_SEED=1 + PGWT_TEST_NO_SCHED_ONCPU=1 (+ extra_env).

    sched_switch must be inert (the phase_straddle_livecpu_deterministic house
    pattern): on a busy host a single preemption of the hog opens on_cpu_ts
    and the NEGATIVE reads multi-second CPU* with the sweep disabled —
    observed on the EL8 box mid-ci_smoke (CPU* = 12430ms) while the same
    negative read 0-23ms on the quiet box. Under the hook the stretch can be
    opened ONLY by a watchpoint fire — which the poisoned waitless hog
    guarantees never comes — or by the sweep's repair reseed (the one
    sanctioned non-fire opener; see preseed_state_map), so the CPU*
    discriminator isolates the sweep as the repairing agent in BOTH runs.
    Returns output, diagnostics, tagged PID, pid-scoped CPU, observation
    readiness, and clean bounded completion."""
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_stale_")
    os.chmod(trace_dir, 0o755)

    hog_app = f"pgwt_staleseed_{os.getpid()}_{secrets.token_hex(8)}"
    hog = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True,
        env={**os.environ, "PGAPPNAME": hog_app})
    hog.stdin.write(
        "DO $$ DECLARE x bigint := 0; BEGIN "
        "FOR o IN 1..100000 LOOP "
        "FOR i IN 1..1000000 LOOP x := x + i; END LOOP; x := 0; "
        "END LOOP; END $$;\n")
    hog.stdin.flush()
    hog_pid = wait_for_tagged_active_backend(hog_app, timeout_s=30)

    argv = [TRACER, "--mode", "full", "--pid", str(pm_pid),
            "-T", trace_dir, "--duration", "35", "--interval", "4",
            "--view", "time_model"]
    env = {**os.environ, "PGWT_TEST_STALE_SEED": "1",
           "PGWT_TEST_NO_SCHED_ONCPU": "1", **extra_env}
    win_from = time.time_ns()
    tracer_stdout = tempfile.TemporaryFile()
    tracer_stderr = tempfile.TemporaryFile()
    tracer = subprocess.Popen(argv, stdout=tracer_stdout,
                              stderr=tracer_stderr, env=env)
    hog_cpu_ms = 0.0
    observation_ready = False
    natural_exit = False
    try:
        wait_for_control_socket(tracer, trace_dir, timeout_s=180)
        observation_deadline = time.monotonic() + 35
        if "PGWT_TEST_NO_STALE_SWEEP" in extra_env:
            # Prove the negative across three real display ticks; wall time
            # alone says nothing when the event loop itself is starved.
            observation_ready = bool(wait_for_observation(
                tracer,
                lambda: (time_model_cpu_snapshot_count(
                    temp_output_text(tracer_stdout)) >= 3),
                timeout_s=max(0.0, observation_deadline - time.monotonic())))
        else:
            # Wait for the actual repair counter, then for a later live snapshot
            # to carry multiple seconds of the repaired CPU interval.  Both are
            # bounded, and either stays absent when the product regression is
            # reintroduced.
            reseed_metrics = wait_for_control_observation(
                tracer, trace_dir, "metrics",
                lambda metrics: metrics.get("state_reseeds_total", 0) > 0,
                timeout_s=max(0.0, observation_deadline - time.monotonic()))
            live_ready = wait_for_observation(
                tracer,
                lambda: (parse_time_model(temp_output_text(
                    tracer_stdout)).get('CPU*', 0.0) > 2000.0),
                timeout_s=max(0.0, observation_deadline - time.monotonic()))
            observation_ready = (
                reseed_metrics.get("state_reseeds_total", 0) > 0 and
                bool(live_ready))
        natural_exit = wait_for_natural_exit(tracer, timeout_s=50)
    finally:
        if tracer.poll() is None:
            tracer.kill()
            tracer.wait(timeout=5)
        try:
            hog.stdin.write("\\q\n"); hog.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        hog.terminate()
        cleanup_stale_backends()
    out = STRIP_ANSI.sub('', temp_output_text(tracer_stdout))
    err = temp_output_text(tracer_stderr)
    tracer_stdout.close()
    tracer_stderr.close()
    # Pid-scoped trace CPU for THE HOG (the phase_pure_cpu_straddle integral
    # pattern): busy CI runners inflate the GLOBAL live CPU* through OTHER
    # firing backends — under the sched-inert hook, any backend's we==0 gap
    # between fires reads as wall-as-CPU (measured on CI: global 31176ms
    # positive / 13074ms negative while the box read 7990/24ms) — so the
    # discriminator must isolate the hog's own contribution via the trace.
    if hog_pid:
        win_to = time.time_ns() + 3_000_000_000   # include the terminal flush
        aresp = server_query(trace_dir, "aas",
                             extra={"from": win_from, "to": win_to,
                                    "buckets": 12,
                                    "filters": {"pid": hog_pid}})
        abuckets = aresp.get("buckets", [])
        cpu_mean = (sum(b.get("cpu", 0.0) for b in abuckets) / len(abuckets)
                    if abuckets else 0.0)
        hog_cpu_ms = cpu_mean * (win_to - win_from) / 1e9 * 1000.0
    subprocess.run(["rm", "-rf", trace_dir])
    return (out, err, hog_pid, hog_cpu_ms, observation_ready,
            natural_exit and tracer.returncode == 0)


def phase_stale_seed_sweep(pm_pid):
    """DETERMINISTIC regression for the seed→arm race (pgwt_sweep_stale_state,
    src/backend.c; docs/ROADMAP_AND_STATUS.md). The two prior straddle fixes
    (#52 recovery, #56 fire-time on_cpu_ts open) both need a watchpoint FIRE or
    an attach to repair from; this race produces neither: preseed_state_map
    reads wait_event_info microseconds before PERF_EVENT_IOC_ENABLE, the seeded
    wait ends inside that window, and the wei write to 0 lands before the arm —
    no fire, ever. The entry stays frozen (last_event = a wait that ended long
    ago, on_cpu_ts=0), so the live reader attributes the whole open interval to
    the dead wait and a waitless pure-CPU straddler reads live CPU* = 0 for the
    entire capture while the terminal flush and /proc magnitude cross-check
    stay correct — the three 2026-07-31 CI hits (PG13/17/18).

    PGWT_TEST_STALE_SEED forces that exact end state ON EVERY RUN: the attach
    seed carries a FAKE nonzero wait (Timeout:PgSleep) with on_cpu_ts=0
    regardless of the actual reading — a missed arm-window fire, made
    deterministic. PGWT_TEST_NO_SCHED_ONCPU (both runs; see
    run_stale_seed_capture) holds sched_switch inert so a busy host cannot
    ambiently open the hog's on-CPU stretch — under it, only a watchpoint
    fire (never comes: the hog is waitless) or the sweep's repair reseed can.
    The per-tick sweep must then observe the stable /proc-vs-state mismatch
    on a frozen entry and RESEED (repair at the first ~4s tick; live CPU*
    accrues for the rest of the capture — measured ~8-10s of signal on both
    the quiet and busy EL8 box, attributable ONLY to the sweep).

    NEGATIVE (mirrors phase_straddle_recovery's documented negative): with
    PGWT_TEST_NO_STALE_SWEEP=1 the same poisoned sched-inert run keeps the
    recovery (PR #52) fully active yet THE HOG contributes nothing — proving
    (a) this test detects the bug class and (b) the sweep, not the recovery,
    is the repairing agent. The negative's discriminator is PID-SCOPED (the
    hog's own aas cpu integral from the trace): the global time_model CPU*
    is not usable on busy hosts — under the sched-inert hook every OTHER
    firing backend's we==0 gap reads as wall-as-CPU, and 2-vCPU CI runners
    inflated the global figure to 13074ms (positive: 31176ms) while the quiet
    EL8 box read 24ms (7990ms) for the identical build. The hog-scoped gates
    (>2000ms positive, <500ms negative vs the measured ~8-13s signal) hold on
    both. Full mode only (needs watchpoints + cpu_accounting)."""
    print("--- Phase: stale-seed sweep DETERMINISTIC (seed→arm race, "
          "poisoned attach seed) ---")

    # Positive: sweep enabled — it must repair the poisoned seed.
    out, err, hog_pid, hog_cpu_ms, observed, completed = \
        run_stale_seed_capture(pm_pid, {})
    check(hog_pid is not None,
          f"positive: stale-seed hog active before tracer attach (pid={hog_pid})")
    check(observed,
          "positive: reseed counter and repaired live CPU appeared before deadline")
    check(completed,
          "positive: stale-seed tracer reached bounded natural shutdown")
    check("PGWT_TEST_STALE_SEED" in err,
          "attach seed poisoned with a fake stale wait (test hook active)")
    check("PGWT_TEST_NO_SCHED_ONCPU" in err,
          "sched_switch on_cpu_ts open suppressed (isolation hook active)")
    check("reseeded stale state" in err,
          f"pgwt_sweep_stale_state repaired the frozen entry "
          f"(stderr {err[-200:]!r})")
    live_cpu = parse_time_model(out).get('CPU*', 0.0)
    check(live_cpu > 2000.0,
          f"stale-seeded pure-CPU straddler shows MULTI-SECOND live CPU* via "
          f"the sweep reseed: CPU* = {live_cpu:.0f}ms")
    check(hog_pid is not None and hog_cpu_ms > 2000.0,
          f"positive: THE HOG's own trace CPU is multi-second (pid-scoped "
          f"aas integral, ambient-noise-immune): {hog_cpu_ms:.0f}ms "
          f"(pid={hog_pid})")

    # Negative: sweep disabled (ONLY the sweep — recovery stays active) — the
    # poisoned entry must stay frozen. DISCRIMINATOR IS PID-SCOPED: on busy
    # CI runners other firing backends inflate the GLOBAL CPU* to multi-second
    # values under the sched-inert hook (wall-as-CPU in their we==0 gaps; CI
    # measured 13074ms global while the hog contributed nothing), so the
    # global time_model figure cannot discriminate — the hog's own pid-scoped
    # trace CPU can, deterministically on any host.
    out, err, hog_pid, hog_cpu_ms, observed, completed = run_stale_seed_capture(
        pm_pid, {"PGWT_TEST_NO_STALE_SWEEP": "1"})
    check(hog_pid is not None,
          f"negative: stale-seed hog active before tracer attach (pid={hog_pid})")
    check(observed,
          "negative: three real display snapshots appeared before deadline")
    check(completed,
          "negative: stale-seed tracer reached bounded natural shutdown")
    check("PGWT_TEST_STALE_SEED" in err,
          "negative: attach seed poisoned (test hook active)")
    check("PGWT_TEST_NO_SCHED_ONCPU" in err,
          "negative: sched_switch on_cpu_ts open suppressed (isolation hook "
          "active)")
    check("reseeded stale state" not in err,
          "negative: sweep disabled — no repair line")
    check(hog_pid is not None and hog_cpu_ms < 500.0,
          f"negative: without the sweep THE HOG's trace CPU stays at the "
          f"floor (pid-scoped): {hog_cpu_ms:.0f}ms (pid={hog_pid}; the frozen "
          f"entry attributes its whole run to the dead wait — the exact CI "
          f"failure, isolated from ambient-backend CPU)")


def phase_sampled_aas_truth(pm_pid):
    """T2 (AAS-1) CPU-observability contract, independent of escalation
    policy: sampled AAS — now CPU-inclusive — must match pg_stat_activity
    ground truth. A CPU-bound
    storm (3 hogs) + a pg_sleep session; pre-T2 the sampler skipped all
    we==0 and reported AAS ~ 0.0x for exactly this shape (study Q4:
    -98%%). Anomaly escalation is disabled (huge factor) so the window is
    PURE sampled tier. The legacy huge-factor switch also disables the CPU
    saturation guard, so this phase cannot pass because of escalation."""
    print("--- Phase 5: CPU observability in pure sampled AAS ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_aas_")
    os.chmod(trace_dir, 0o755)
    storm = None
    sleeper = None
    tracer = None
    resources_cleaned = False

    def cleanup_resources():
        nonlocal resources_cleaned
        if resources_cleaned:
            return
        if tracer is not None:
            try:
                terminate_and_wait(tracer)
            except (OSError, subprocess.TimeoutExpired):
                pass
        if sleeper is not None:
            try:
                terminate_and_wait(sleeper)
            except (OSError, subprocess.TimeoutExpired):
                pass
        if storm is not None:
            try:
                storm.stop()
            except OSError:
                pass
        resources_cleaned = True

    expected_pids = []
    psa = []
    win_from = win_to = time.time_ns()
    err = ""
    tracer_completed = False

    try:
        storm = CpuStorm(3)
        sleeper_app = f"pgwt_aas_sleep_{os.getpid()}_{secrets.token_hex(8)}"
        sleeper = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True,
            env={**os.environ, "PGAPPNAME": sleeper_app})

        def controlled_backend_pids():
            app_names = storm.application_names + [sleeper_app]
            quoted_apps = ",".join(
                "'" + app.replace("'", "''") + "'"
                for app in app_names)
            rows = psql(
                "SELECT pid FROM pg_stat_activity "
                f"WHERE application_name IN ({quoted_apps}) "
                "AND datname = 'postgres' ORDER BY application_name",
                timeout=5)
            pids = sorted({int(row) for row in rows.splitlines()
                           if re.fullmatch(r'\d+', row)})
            return pids if len(pids) == 4 else None

        expected_pids = wait_for_observation(
            sleeper, controlled_backend_pids, 30) or []
        check(len(expected_pids) == 4,
              f"sampled-AAS workload has four tagged backends "
              f"(pids={expected_pids})")

        tracer = subprocess.Popen(
            [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
             "-T", trace_dir, "--duration", "150", "--quiet",
             "--interval", "5", "--anomaly-aas-factor", "1000000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        tracer_ready = wait_for_control_socket(
            tracer, trace_dir, timeout_s=180)
        check(tracer_ready,
              "sampled-AAS tracer reached the post-attach startup boundary")

        storm.fire_continuous(160)
        sleeper.stdin.write("SELECT pg_sleep(160);\n")
        sleeper.stdin.flush()
        expected_set = set(expected_pids)

        def wait_for_exact_active(timeout_s):
            deadline = time.monotonic() + timeout_s
            last = set()
            while time.monotonic() < deadline and tracer.poll() is None:
                if not expected_set:
                    return set()
                remaining = deadline - time.monotonic()
                pid_list = ",".join(str(pid) for pid in expected_set)
                try:
                    rows = psql(
                        "SELECT pid FROM pg_stat_activity "
                        "WHERE state = 'active' "
                        "AND backend_type = 'client backend' "
                        f"AND pid IN ({pid_list})",
                        timeout=min(5, max(1, remaining)))
                except subprocess.TimeoutExpired:
                    rows = ""
                last = {int(row) for row in rows.splitlines()
                        if re.fullmatch(r'\d+', row)}
                if last == expected_set:
                    return last
                time.sleep(min(0.05, max(0.0,
                                         deadline - time.monotonic())))
            return last

        active_pids = wait_for_exact_active(30)
        check(active_pids == expected_set and len(expected_set) == 4,
              f"all four sampled-AAS backends became active "
              f"(active={sorted(active_pids)})")

        win_from = time.time_ns()
        start_metrics = wait_for_control_observation(
            tracer, trace_dir, "metrics",
            lambda m: isinstance(m.get("samples_total"), (int, float)), 30)
        samples_at_start = int(start_metrics.get("samples_total", 0))
        check("samples_total" in start_metrics,
              "sampled-AAS baseline metrics are observable")

        psa = sample_active_sessions(expected_pids, interval=0.5)
        progress_metrics = wait_for_control_observation(
            tracer, trace_dir, "metrics",
            lambda m: int(m.get("samples_total", 0)) >=
                      samples_at_start + 4,
            30)
        samples_after = int(progress_metrics.get("samples_total", 0))
        check(samples_after >= samples_at_start + 4,
              f"sampled-AAS tracer advanced after the window opened "
              f"(samples={samples_at_start}->{samples_after})")
        end_active = wait_for_exact_active(10)
        check(end_active == expected_set and len(expected_set) == 4,
              f"all four sampled-AAS backends remained active through the "
              f"observation window (active={sorted(end_active)})")
        win_to = time.time_ns()

        try:
            _, stderr = tracer.communicate(timeout=180)
            natural_exit = True
        except subprocess.TimeoutExpired:
            tracer.kill()
            _, stderr = tracer.communicate()
            natural_exit = False
        err = stderr.decode('utf-8', errors='replace')
        tracer_completed = natural_exit and tracer.returncode == 0

        cleanup_resources()
        check(tracer_completed,
              f"sampled-AAS tracer finalized cleanly (rc={tracer.returncode})")

        psa_mean = sum(psa) / len(psa) if psa else 0.0
        check(len(psa) >= 8 and all(count == 4 for count in psa),
              f"ground truth: four tagged backends stayed active across a "
              f"bounded quorum (mean={psa_mean:.2f}, n={len(psa)}, "
              f"counts={psa})")

        responses = [
            server_query(
                trace_dir, "aas",
                extra={"from": win_from, "to": win_to, "buckets": 9,
                       "filters": {"pid": pid}})
            for pid in expected_pids
        ]
        bucket_sets = [resp.get("buckets", []) for resp in responses]
        have_pid_buckets = (len(bucket_sets) == 4 and
                            all(len(buckets) > 0 for buckets in bucket_sets))
        check(have_pid_buckets,
              "AAS view has buckets for all four controlled backends"
              if have_pid_buckets else
              f"AAS view has per-PID buckets "
              f"(sets={list(map(len, bucket_sets))}, "
              f"stderr tail: {err[-300:]!r})")
        if have_pid_buckets:
            tracer_mean = sum(
                sum(aas_bucket_total(b) for b in buckets) / len(buckets)
                for buckets in bucket_sets)
            cpu_mean = sum(
                sum(b.get("cpu", 0.0) for b in buckets) / len(buckets)
                for buckets in bucket_sets)
            tol = max(0.9, 0.35 * psa_mean)
            check(abs(tracer_mean - psa_mean) <= tol,
                  f"sampled AAS matches pg_stat_activity ground truth: "
                  f"tracer={tracer_mean:.2f} vs psa={psa_mean:.2f} "
                  f"(tol ±{tol:.2f}) [AAS-1 definition of done]")
            check(cpu_mean >= 1.0,
                  f"CPU class carries the storm: cpu AAS = {cpu_mean:.2f} "
                  f"(pre-T2 sampler reported ~0 here)")
            fidelities = [resp.get("fidelity") for resp in responses]
            check(all(fidelity == "sampled" for fidelity in fidelities),
                  f"window is pure sampled tier (fidelity={fidelities!r})")
    finally:
        cleanup_resources()
        subprocess.run(["rm", "-rf", trace_dir])


def phase_cpu_storm_escalation(pm_pid):
    """AAS-1 Stage 3 saturation-policy contract. The target capacity is an
    explicit C=2 override, so three CPU-demand sessions are deterministically
    saturating regardless of runner size. Assert the distinct CPU CUSUM rule,
    full-tier transition, and continuity across the sampled->exact switch."""
    print("--- Phase 6: CPU saturation CUSUM escalation; no AAS step ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_esc_")
    os.chmod(trace_dir, 0o755)

    # Timing invariant: C=2 reaches h in 1.5/(1.25-0.80) = 3.34s. The 45s
    # command covers the 5s readiness allowance, the 10s/0.5s ground-truth
    # sweep, and a full fresh 3.34s evidence horizon after a transient exact
    # setup rollback even across the observed ~20s event-loop stall.  The
    # window assertion polls that real outcome for at most 30s; it still fails
    # if no exact window opens. Completion has another 10s grace, all within
    # the existing 75s tracer lifetime.
    burn_duration_s = 45
    readiness_timeout_s = 5
    ground_truth_duration_s = 10
    state_query_timeout_s = 5
    completion_grace_s = 10
    tracer_duration_s = 75

    storm = CpuStorm(3)
    storm_pids = storm.backend_pids()
    check(len(storm_pids) == 3,
          f"saturation workload has exactly three tagged backends "
          f"(pids={storm_pids})")
    tracer_argv = [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", str(tracer_duration_s), "--quiet",
         "--interval", "5", "--sample-rate", "60",
         "--anomaly-cpu-capacity", "2",
         # Isolate the CPU rule without crossing the >=1e6 legacy switch that
         # deliberately disables it too. The pre-warmup storm gives the AAS
         # baseline a positive value, making this factor unreachable.
         "--anomaly-aas-factor", "999999"]
    debug_cpu_ticks = bool(os.environ.get("PGWT_DEBUG_ANOMALY_CPU_TICK"))
    tracer_stderr = tempfile.TemporaryFile() if debug_cpu_ticks \
        else subprocess.PIPE
    tracer = subprocess.Popen(
        tracer_argv,
        stdout=subprocess.PIPE, stderr=tracer_stderr,
        env={**os.environ, "PGWT_TEST_EXACT_PRESEED_FAIL_ONCE": "1"})
    tracer_ready = wait_for_control_socket(tracer, trace_dir, timeout_s=180)
    startup_metrics = wait_for_control_observation(
        tracer, trace_dir, "metrics",
        lambda value: (
            value.get("tier") == "sampled" and
            value.get("effective_cpu_capacity_cores") == 2 and
            value.get("effective_cpu_capacity_source") == "override"),
        timeout_s=30)
    check(tracer_ready and startup_metrics.get("tier") == "sampled",
          "CPU-saturation tracer reached sampled/capacity-ready startup")
    storm_from = time.time_ns()
    storm_to = storm_from
    storm_states = []
    before_metrics_state = []
    metrics = {}
    status = {}
    escalated_state = []
    try:
        storm.fire_continuous(duration_s=burn_duration_s)
        ready_state = []
        ready_deadline = time.monotonic() + readiness_timeout_s
        burn_ready_at = ready_deadline
        while time.monotonic() < ready_deadline:
            readiness_remaining_s = ready_deadline - time.monotonic()
            if readiness_remaining_s <= 0:
                break
            ready_state = cpu_storm_state(
                storm_pids,
                timeout=min(state_query_timeout_s, readiness_remaining_s))
            if cpu_storm_state_is_sustained(ready_state, storm_pids):
                burn_ready_at = time.monotonic()
                break
            time.sleep(min(0.05, max(
                0.0, ready_deadline - time.monotonic())))
        check(cpu_storm_state_is_sustained(ready_state, storm_pids),
              f"all three saturation backends entered one CPU-only command "
              f"(state={ready_state})")

        storm_from = time.time_ns()
        # C=2 reaches h after roughly 3.3s at capped demand. Ground-truth
        # snapshots must show all three exact PIDs staying in their one active
        # command, with waitless demand at or above C across the sweep. Unlike
        # the old discrete generate_series burst, this proves the test supplied
        # a sustained saturation incident rather than assuming it did.
        storm_states = sample_cpu_storm_state(
            storm_pids, ground_truth_duration_s, interval=0.5,
            timeout=state_query_timeout_s)
        storm_to = time.time_ns()
        before_metrics_state = wait_for_cpu_storm_state(
            tracer, storm_pids, state_query_timeout_s)
        try:
            metrics = wait_for_control_observation(
                tracer, trace_dir, "metrics",
                lambda value: (
                    value.get("anomaly_cpu_saturation_fires_total", 0) >= 1
                    and value.get("escalation_windows_total", 0) >= 1
                    and value.get("tier") == "escalated"),
                timeout_s=30)
            status = ctl_query(trace_dir, "status")
            escalated_state = wait_for_cpu_storm_state(
                tracer, storm_pids, state_query_timeout_s)
        except (OSError, ValueError, json.JSONDecodeError) as e:
            print(f"  (control socket: {e})")

        # Exact-tier CPU is an interval closed by the command's transition to
        # ClientRead. Wait for that real boundary before fixing the trace-query
        # end time; otherwise the one long CPU interval is still open/on-CPU
        # and cannot yet exist in the persisted trace. All commands were active
        # at burn_ready_at, so they must finish by burn_ready_at + burn duration;
        # the completion gate gives that latest expected finish another 10s.
        completed_state = []
        completion_deadline = (burn_ready_at + burn_duration_s
                               + completion_grace_s)
        while time.monotonic() < completion_deadline:
            completed_state = cpu_storm_state(
                storm_pids, timeout=state_query_timeout_s)
            if cpu_storm_state_is_idle(completed_state, storm_pids):
                break
            time.sleep(0.1)
        check(cpu_storm_state_is_idle(completed_state, storm_pids),
              f"all three CPU commands closed into ClientRead before the "
              f"trace query (state={completed_state})")
        # End the AAS window at PostgreSQL's actual active->ClientRead state
        # boundary, not when this potentially delayed observer returned.  A
        # small inclusion margin covers ordering between the activity edge and
        # PgBackendStatus publication; edge buckets are excluded below.
        state_change_times = [row[4] for row in completed_state]
        if state_change_times:
            storm_to = max(state_change_times) + 100_000_000
        time.sleep(1.0)  # let the closed exact intervals drain to the writer
        _, stderr = tracer.communicate(timeout=40)
        if debug_cpu_ticks:
            tracer_stderr.seek(0)
            stderr = tracer_stderr.read()
        err = stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        tracer.kill()
        _, stderr = tracer.communicate()
        if debug_cpu_ticks:
            tracer_stderr.seek(0)
            stderr = tracer_stderr.read()
        err = stderr.decode('utf-8', errors='replace')
    finally:
        storm.stop()
        if debug_cpu_ticks:
            tracer_stderr.close()

    if debug_cpu_ticks:
        print("--- PGWT_DEBUG_ANOMALY_CPU_TICK tracer stderr ---")
        print(err)

    active_states = [state for state in storm_states
                     if cpu_storm_state_is_active(state, storm_pids)]
    joint_waitless_states = [state for state in storm_states
                             if cpu_storm_state_is_sustained(
                                 state, storm_pids)]
    waitless_by_pid = {
        pid: sum(any(row[0] == pid and cpu_storm_row_is_waitless(row)
                     for row in state)
                 for state in storm_states)
        for pid in storm_pids
    }
    waitless_slots = sum(waitless_by_pid.values())
    capacity_slot_quorum = 2 * len(storm_states)
    per_backend_quorum = max(3, (len(storm_states) + 2) // 3)
    # PostgreSQL may briefly enter a housekeeping wait such as ProcArray even
    # inside this CPU-only command.  Joint-waitless snapshot ratios varied from
    # 10/18 to 17/17 under load even though all three commands stayed active and
    # the CPU rule/window fired.  Assert the saturation invariant directly:
    # every command stays active; waitless slots average at least the configured
    # C=2; and every backend contributes across at least one third of the sweep.
    # The bounded start/window gates immediately below still require all three
    # to be jointly waitless at two distinct milestones.  A missing command, a
    # permanently stuck backend, or demand below capacity therefore still fails.
    check(len(storm_states) > 0 and len(active_states) == len(storm_states),
          f"all three tagged CPU commands stayed active on every storm poll "
          f"(active={len(active_states)}/{len(storm_states)})")
    check(waitless_slots >= capacity_slot_quorum and
          all(count >= per_backend_quorum
              for count in waitless_by_pid.values()),
          f"storm poll quorum averaged at least C=2 waitless demand and every "
          f"backend contributed (slots={waitless_slots}/"
          f"{3 * len(storm_states)}, required={capacity_slot_quorum}; "
          f"per_pid={waitless_by_pid}, required_each={per_backend_quorum}; "
          f"joint={len(joint_waitless_states)}/{len(storm_states)})")
    check(cpu_storm_state_is_sustained(before_metrics_state or [], storm_pids),
          f"CPU-only saturation reaches metrics collection "
          f"(before={before_metrics_state})")
    check(cpu_storm_state_is_sustained(escalated_state or [], storm_pids),
          f"tagged CPU saturation still held when the exact window appeared "
          f"(state={escalated_state})")

    cpu_fires = metrics.get("anomaly_cpu_saturation_fires_total", 0)
    windows = metrics.get("escalation_windows_total", 0)
    check(metrics.get("effective_cpu_capacity_cores") == 2 and
          metrics.get("effective_cpu_capacity_source") == "override",
          f"saturation test reports C=2 override "
          f"(capacity={metrics.get('effective_cpu_capacity_cores')!r}, "
          f"source={metrics.get('effective_cpu_capacity_source')!r})")
    check(cpu_fires >= 1 and windows >= 1,
          f"CPU saturation rule opened a full-fidelity window "
          f"(cpu_fires={cpu_fires}, escalation_windows_total={windows}, "
          f"tier={metrics.get('tier')!r})"
          + ("" if cpu_fires >= 1 else
             f" (tracer stderr tail: {err[-300:]!r})"))
    check(status.get("tier") == "escalated" and
          status.get("escalation_reason") == "cpu_saturation",
          f"CPU-rule window is full tier with distinct reason "
          f"(tier={status.get('tier')!r}, "
          f"reason={status.get('escalation_reason')!r})")
    check("rule=cpu-saturation" in err,
          "CPU saturation fire is distinct in the daemon log")
    check("PGWT_TEST_EXACT_PRESEED_FAIL_ONCE" in err and
          "retry 1/3 after backoff and fresh evidence" in err and
          "anomaly AUTO-escalation: rule=cpu-saturation" in err,
          "one-shot exact preseed rollback retried through the full product path")

    # No sustained step artifact: at least 95% of the interior ~1s buckets,
    # with no adjacent misses, and the aggregate window must show the storm.
    # Requiring EVERY wall bucket falsely failed when an overloaded runner
    # starved one sampler interval even though the tagged commands stayed
    # active and both tiers otherwise reported them.  The tight miss budget
    # still rejects sustained or broadly intermittent loss across the tier
    # switch; Phase 5 independently guards sampled CPU attribution.  With C=2
    # the CUSUM switch occurs in this window without relying on a sub-second
    # firing assumption.
    aas_from = storm_from + 500_000_000
    aas_range_ns = max(1, storm_to - aas_from)
    aas_bucket_count = max(
        3, min(90, (aas_range_ns + 999_999_999) // 1_000_000_000))
    resp = server_query(trace_dir, "aas",
                        extra={"from": aas_from, "to": storm_to,
                               "buckets": aas_bucket_count})
    buckets = resp.get("buckets", [])
    check(len(buckets) >= 5 and resp.get("fidelity") == "mixed",
          f"~1s AAS buckets span both tiers (got {len(buckets)}, "
          f"fidelity={resp.get('fidelity')!r})")
    if len(buckets) >= 3:
        totals = [aas_bucket_total(b) for b in buckets]
        interior_totals = totals[1:-1]
        lo = min(interior_totals)
        mean = sum(interior_totals) / len(interior_totals)
        good = sum(value >= 1.2 for value in interior_totals)
        required = (95 * len(interior_totals) + 99) // 100
        bad_run = max_bad_run = 0
        for value in interior_totals:
            bad_run = 0 if value >= 1.2 else bad_run + 1
            max_bad_run = max(max_bad_run, bad_run)
        truth = min((len(state) for state in storm_states), default=0)
        check(good >= required and max_bad_run <= 1 and mean >= 1.2,
              f"no sustained AAS step artifact at the tier switch: "
              f"quorum={good}/{len(interior_totals)} (required={required}), "
              f"longest miss run={max_bad_run}, mean={mean:.2f}, "
              f"min={lo:.2f}; minimum tagged "
              f"pg_stat_activity count = {truth}")

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
    parser.add_argument('--event-drain-only', action='store_true',
                        help='run only the deterministic mode-4 event-drain phase')
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

    version_output = subprocess.check_output(
        [f"/proc/{pm_pid}/exe", "--version"], text=True)
    version_match = re.search(r"PostgreSQL\)?\s+(\d+)", version_output)
    if not version_match:
        print(f"ERROR: cannot identify PostgreSQL major from {version_output!r}")
        sys.exit(1)
    pg_major = int(version_match.group(1))

    print(f"=== test_capture_smoke --mode {args.mode} (postmaster PID {pm_pid}, "
          f"PG{pg_major}) ===")

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

    if args.event_drain_only:
        if args.mode != 'full':
            parser.error('--event-drain-only requires --mode full')
        phase_event_ring_drain_budget(pm_pid)
        print(f"\n{tests_passed}/{tests_run} checks passed")
        sys.exit(0 if tests_failed == 0 else 1)

    core = args.capture_core
    phase_live_system_event(pm_pid, args.mode, core=core)
    phase_live_query_event(pm_pid, args.mode, pg_major, core=core)
    phase_trace_file(pm_pid, args.mode, pg_major, core=core)

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
            phase_event_ring_drain_budget(pm_pid)
            phase_straddle_recovery(pm_pid, args.mode)
            # Deterministic regression for the live-view CPU*=0 straddle flake:
            # a pg_sleep→loop straddler with sched_switch forced inert reads
            # multi-second live CPU* only if the watchpoint fire opens on_cpu_ts
            # (the fix). Full mode only (needs watchpoints + cpu_accounting).
            phase_straddle_livecpu_deterministic(pm_pid)
            # Deterministic regression for the seed→arm race: a poisoned attach
            # seed (fake stale wait, no fire ever) must be repaired by the
            # per-tick pgwt_sweep_stale_state — positive AND negative run.
            phase_stale_seed_sweep(pm_pid)
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
