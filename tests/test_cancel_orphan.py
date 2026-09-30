#!/usr/bin/env python3
"""test_cancel_orphan.py — a REAL cancelled statement must not leave a zombie.

This is the live proof behind src/compute.c's orphan-execution close. A
statement that is cancelled (the presenter's Ctrl-C, pg_cancel_backend,
statement_timeout, an ERROR, a disconnect) never reaches
query__execute__done: PostgreSQL longjmps past it, so the EXEC_END marker
for that execution is never emitted. Its Waterfall row used to stay
"in progress" for the rest of the capture, with an elapsed-so-far that only
grows — and, once the Waterfall ranks by duration, pinned above every
genuinely slow query for the whole session.

What this test does, in a full-mode capture against a real PostgreSQL:
  1. runs a long statement on a dedicated connection,
  2. waits (by polling pg_stat_activity, not by sleeping) until that backend
     is actually 'active' on it,
  3. cancels it with pg_cancel_backend and REQUIRES PostgreSQL to report
     "canceling statement due to user request" — if the statement merely
     finished, there is no defect to reproduce and this test FAILS rather
     than passing on the wrong scenario,
  4. proves from the RAW TRACE (tests/dump_exec_markers) that the backend
     emitted an EXEC_START and NO EXEC_END for it, and that it then went
     idle — so the row can only be closed by deduction,
  5. asserts pgwt-server reports that row CLOSED, at that idle boundary,
     with a bounded duration rather than one that stretches to the end of
     the capture.

Every one of those steps is a refusal point: a missing tool, an empty trace,
a capture with no executions at all, a statement that was not actually
cancelled, or a pid the marker stream never saw all FAIL. "Nothing to check"
is never a pass.

Requires: root, a running PostgreSQL, pg_wait_tracer + pgwt-server +
tests/dump_exec_markers built.
Usage: sudo python3 tests/test_cancel_orphan.py [--pid POSTMASTER_PID]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(ROOT, "pg_wait_tracer")
SERVER = os.path.join(ROOT, "pgwt-server")
DUMP = os.path.join(ROOT, "tests", "dump_exec_markers")

# The statement is long enough that the cancel lands well inside it, and
# short enough that a missed cancel cannot stall the suite.
STATEMENT_SECONDS = 20
# Capture long enough to hold: backend connect, statement start, cancel, and
# the post-cancel idle transition.
CAPTURE_SECONDS = 30
MARKER = "pgwt_cancel_orphan_probe"

tests_run = tests_failed = 0


def check(cond, msg):
    global tests_run, tests_failed
    tests_run += 1
    if cond:
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")
    return bool(cond)


def fail(msg):
    return check(False, msg)


def psql(sql, timeout=15):
    r = subprocess.run(["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
                       capture_output=True, text=True, timeout=timeout)
    return r


class Victim:
    """A dedicated psql connection running one long statement."""

    def __init__(self):
        self.proc = None
        self.stdout = ""
        self.stderr = ""
        self._t = None

    def start(self):
        # The comment marker makes the backend findable in pg_stat_activity
        # without depending on query-text normalisation.
        sql = f"SELECT pg_sleep({STATEMENT_SECONDS}) /* {MARKER} */"
        self.proc = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        def _wait():
            self.stdout, self.stderr = self.proc.communicate()

        self._t = threading.Thread(target=_wait, daemon=True)
        self._t.start()

    def backend_pid(self, deadline):
        """Poll until this backend is ACTIVE on our statement. No fixed sleep:
        cancelling before the executor starts would test nothing."""
        while time.time() < deadline:
            r = psql("SELECT pid FROM pg_stat_activity WHERE state = 'active' "
                     f"AND query LIKE '%{MARKER}%' AND pid <> pg_backend_pid()")
            pid = r.stdout.strip().splitlines()
            if pid and pid[0].isdigit():
                return int(pid[0])
            time.sleep(0.2)
        return None

    def join(self, timeout):
        if self._t:
            self._t.join(timeout)
        return self._t is not None and not self._t.is_alive()

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()


def server_query(proc, cmd):
    proc.stdin.write((json.dumps(cmd) + "\n").encode())
    proc.stdin.flush()
    line = proc.stdout.readline()
    return json.loads(line.decode()) if line else None


def dump_markers(trace_dir, pid):
    """Raw-trace lifecycle records for one pid. A missing or non-executable
    helper is a FAILURE (exit 126/127), never a skip."""
    if not os.path.exists(DUMP):
        fail(f"{DUMP} is missing — build tests first (make -C tests)")
        return None
    r = subprocess.run([DUMP, trace_dir, str(pid)],
                       capture_output=True, text=True, timeout=60)
    if r.returncode in (126, 127):
        fail(f"{DUMP} could not run (exit {r.returncode})")
        return None
    rows = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        rec = {"kind": parts[0]}
        for p in parts[1:]:
            k, _, v = p.partition("=")
            rec[k] = int(v)
        rows.append(rec)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    args = ap.parse_args()

    print("=== test_cancel_orphan: a cancelled statement leaves no zombie ===")

    if os.geteuid() != 0:
        fail("must run as root")
        return 1
    for path in (TRACER, SERVER):
        if not os.path.exists(path):
            fail(f"{path} is missing — build first")
            return 1

    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        fail("no postmaster found")
        return 1

    trace_dir = tempfile.mkdtemp(prefix="pgwt_cancel_")
    victim = Victim()
    srv = None
    try:
        tracer = subprocess.Popen(
            [TRACER, "--mode", "full", "--verbose", "--pid", str(pm_pid),
             "--interval", str(CAPTURE_SECONDS),
             "--duration", str(CAPTURE_SECONDS),
             "--trace-dir", trace_dir, "--view", "time_model"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Drain stderr INCREMENTALLY: a single blocking .read() would only
        # return at daemon exit, long after the readiness line it carries.
        tracer_err = []

        def _drain():
            for raw in iter(tracer.stderr.readline, b""):
                tracer_err.append(raw.decode(errors="replace"))

        threading.Thread(target=_drain, daemon=True).start()

        # Readiness gate, not a blind sleep. The victim backend must be
        # forked AFTER the fork tracepoint is live, or its EXEC_START is
        # simply not in the trace and this test would refuse on step 4 for
        # the wrong reason. "attached to N existing backends" is printed
        # (under --verbose) once the tracepoints are attached and the
        # existing-backend scan is done. A daemon that never gets there FAILS
        # here on a deadline — it never stalls the suite.
        ready_deadline = time.time() + 45
        while time.time() < ready_deadline:
            if tracer.poll() is not None:
                break
            if any("attached to" in chunk for chunk in tracer_err):
                break
            time.sleep(0.25)
        ready = any("attached to" in chunk for chunk in tracer_err)
        if not check(ready, "the daemon reported it is attached and capturing"):
            print("    tracer stderr: " + "".join(tracer_err)[-500:])
            return 1

        victim.start()
        deadline = time.time() + 15
        victim_pid = victim.backend_pid(deadline)
        if not check(victim_pid is not None,
                     "the long statement reached state='active' in a backend"):
            return 1
        print(f"  (victim backend pid {victim_pid})")

        # Give the executor a visible slice of runtime before the cancel, so
        # the row has a duration worth checking.
        time.sleep(3)
        r = psql(f"SELECT pg_cancel_backend({victim_pid})")
        check(r.stdout.strip() == "t",
              f"pg_cancel_backend({victim_pid}) returned true")

        finished = victim.join(20)
        check(finished, "the cancelled psql returned (did not hang)")
        cancelled = "canceling statement due to user request" in victim.stderr
        if not check(cancelled,
                     "PostgreSQL reported the statement CANCELLED — the "
                     "defect's own scenario, not a normal completion"):
            print(f"    victim stderr: {victim.stderr.strip()[:300]}")
            return 1

        # The post-cancel idle transition only lands in the trace once the
        # backend has gone back to waiting for its client; keep the capture
        # running past that, then let the daemon finish its own duration.
        time.sleep(3)
        try:
            tracer.wait(timeout=CAPTURE_SECONDS + 30)
        except subprocess.TimeoutExpired:
            tracer.kill()
            tracer.wait(timeout=10)
        check(tracer.returncode in (0, None),
              f"tracer exited cleanly (rc={tracer.returncode})")

        # ---- raw-trace evidence: no EXEC_END exists for this execution ----
        recs = dump_markers(trace_dir, victim_pid)
        if recs is None:
            return 1
        kinds = [r_["kind"] for r_ in recs]
        print(f"  (raw lifecycle records for pid {victim_pid}: {kinds})")
        if not check("EXEC_START" in kinds,
                     "the cancelled statement DID emit an EXEC_START — "
                     "there is a row to close (not vacuously absent)"):
            return 1
        # The orphan is the EXEC_START with no EXEC_END anywhere after it.
        # Taking the first EXEC_START blindly would grade a connection-setup
        # statement instead, and that one DOES have an EXEC_END.
        orphan_idx = None
        for i, rec in enumerate(recs):
            if rec["kind"] != "EXEC_START":
                continue
            if not any(x["kind"] == "EXEC_END" for x in recs[i + 1:]):
                orphan_idx = i
                break
        if not check(orphan_idx is not None,
                     "an EXEC_START with NO EXEC_END after it exists — the "
                     "cancel really did skip query__execute__done"):
            return 1
        start_ts = recs[orphan_idx]["ts"]
        after = recs[orphan_idx + 1:]
        boundaries = [r_["ts"] for r_ in after if r_["kind"] in ("IDLE", "EXIT")]
        if not check(boundaries,
                     "an idle/exit boundary DOES arrive for the cancelled "
                     "backend in a full-mode capture (the premise)"):
            return 1
        print(f"  (orphan EXEC_START ts={start_ts}; boundaries after it: "
              f"{[(b - start_ts) / 1e6 for b in boundaries]} ms)")

        # ---- the server's answer ----
        srv = subprocess.Popen([SERVER, trace_dir], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        info = server_query(srv, {"id": 1, "cmd": "info"})
        if not check(info and "error" not in info, "pgwt-server info succeeded"):
            return 1
        from_ns, to_ns = int(info.get("from_ns", 0)), int(info.get("to_ns", 0))
        if not check(to_ns > from_ns, "the trace covers a real time range"):
            return 1

        ex = server_query(srv, {"id": 2, "cmd": "executions",
                                "from_ns": from_ns, "to_ns": to_ns})
        if not check(ex and "error" not in ex, "executions query succeeded"):
            return 1
        rows = ex.get("rows", [])
        if not check(rows, "the capture contains execution rows at all"):
            return 1
        mine = [r_ for r_ in rows if int(r_["pid"]) == victim_pid]
        if not check(mine, f"an execution row exists for pid {victim_pid}"):
            return 1
        row = min(mine, key=lambda r_: abs(int(r_["start_ns"]) - start_ts))
        check(int(row["start_ns"]) == start_ts,
              "the server row matches the orphan EXEC_START exactly")

        if not check(not row.get("in_progress"),
                     "the cancelled statement's row is CLOSED, not left in "
                     "progress"):
            print("    (this is the defect: an orphan row whose elapsed-so-far "
                  "grows for the rest of the capture)")
            return 1

        end_ns = int(row["end_ns"])
        check(end_ns in boundaries,
              f"the end ({(end_ns - start_ts) / 1e6:.1f} ms in) is EXACTLY an "
              "observed idle/exit boundary — a real timestamp, not invented")
        dur_ms = float(row["duration_ms"])
        capture_ms = (to_ns - from_ns) / 1e6
        check(dur_ms < capture_ms - 1000,
              f"duration {dur_ms:.1f} ms is bounded well inside the "
              f"{capture_ms:.0f} ms capture — it did not keep growing")
        # #222 renders this as "80.0ms *": the flag is set by src/compute.c.
        # It only reaches the client once the executions response serializes
        # it (src/server.c, owned by the #222 branch); assert only when the
        # field is actually on the wire, and say so loudly when it is not.
        if "end_inferred" in row:
            check(row["end_inferred"] is True,
                  "the deduced end is LABELLED end_inferred on the wire")
        else:
            print("  NOTE: end_inferred is not serialized by this build; the "
                  "label is pinned by tests/test_compute_orphan_close.c")
    finally:
        victim.kill()
        if srv:
            srv.kill()
        try:
            psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                 f"WHERE query LIKE '%{MARKER}%' AND pid <> pg_backend_pid()")
        except Exception:
            pass
        shutil.rmtree(trace_dir, ignore_errors=True)

    print(f"\n=== {tests_run} checks, {tests_failed} failed ===")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
