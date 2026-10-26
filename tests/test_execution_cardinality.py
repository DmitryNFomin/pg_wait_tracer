#!/usr/bin/env python3
"""test_execution_cardinality.py — #464: one execution per execution.

THE DEFECT. PostgreSQL emits the query__execute__start / query__execute__done
USDT probes from TWO code sites for plannable non-SELECT work: once around
PortalRun, and again inside the static helper PortalRunMulti calls. The
daemon attached them by probe NAME, which covers both, so every DML
statement produced two nested EXEC_START/EXEC_END pairs. A SELECT never
reaches the second site, which is why the archived rehearsal recorded Execs
368 against Plans 184 for the DML query and 91 against 91 for the SELECT.

WHY THIS FILE EXISTS. Both counted demo rehearsals PASSED with the double
count present. Nothing anywhere checked execution CARDINALITY — durations,
conservation and cross-tab agreement are all invariant under a doubled
count, so the defect was invisible to every gate in the tree. The fix
without this test would be a fix nobody could show working, and the next
one would be invisible again.

WHAT MAKES IT A GATE RATHER THAN A RATIO. The counts pgwt reports and the
counts pgwt reports cannot check each other: a defect that scales execs and
plans together satisfies `exec == plan`, and `plan <= exec` — the only
inequality a prepared-protocol cohort can assert — is satisfied by ANY
multiple. So the authority here is N, the number of statements THIS SCRIPT
sent, a Python integer that never passes through PostgreSQL or the
daemon. Every equality below is against that.

EVERY RUN PROVES IT CAN SEE THE DEFECT. The script captures the same
cohort twice: once with the shipped filter, and once with
PGWT_TEST_EXEC_SITE_ADMIT_ALL=1, which republishes every declared probe
site and reproduces the pre-fix double count through the shipped code path.
Phase 1 must agree with N; phase 2 must disagree with it by exactly a
factor of two on the DML cohorts, be UNCHANGED on the SELECT cohort (the
control that cannot see the defect), and be rejected by the shared
evaluator demo_rehearsal_lib.evaluate_execution_cardinality. A run where
phase 2 passes is reported as a FAILURE: that is the gate having gone
blind, and it is the failure mode that let #464 ship.

Nothing here is tolerant. A missing cohort row, a truncated response, a
degraded fidelity, a refusing daemon, a psql error or a capture window that
does not fully contain the cohort all FAIL. "I could not look" is never a
pass.

Requires: root, running PostgreSQL (>= 14 for compute_query_id — the
cohorts are matched by normalised statement text), pg_wait_tracer and
pgwt-server built.

Usage: sudo python3 tests/test_execution_cardinality.py [--pid PM_PID]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo_rehearsal_lib as drlib
from server_harness import ServerHarness
from testutil import find_postmaster
from phase_probe_capability import require_phase_probes

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")

# Capture duration. The cohorts below are a few hundred fast statements; the
# daemon is stopped as soon as they have drained, so this is only the upper
# bound that keeps a wedged run from hanging the suite forever.
CAPTURE_DURATION_S = 120
# Established BEFORE the cohort starts and drained AFTER it ends — the
# whole point of the containment assertion further down. These two waits are
# the only sleeps in the file and neither one decides a verdict: the capture
# window is then CHECKED against the recorded cohort timestamps, so a box
# too slow for the margin fails the containment check explicitly instead of
# silently measuring half a cohort.
CAPTURE_SETTLE_S = 2.0
CAPTURE_DRAIN_S = 3.0

tests_run = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_failed
    tests_run += 1
    if cond:
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def fatal(msg):
    """A precondition this test cannot proceed without. Exits non-zero —
    never 'skip', because every caller of this script has already decided
    root and PostgreSQL are present (tests/run_all.sh gates the whole live
    tier on both)."""
    print(f"  FAIL: {msg}")
    print("test_execution_cardinality: FAILED (precondition)")
    sys.exit(1)


# ── The cohorts ──────────────────────────────────────────────────────────
#
# One table per cohort, because compute_query_id normalises literals: two
# UPDATEs differing only in a WHERE constant collapse to ONE query id and
# the two cohorts would silently merge into one row carrying the sum of
# their counts. Distinct relations keep the ids distinct.
#
# N differs per cohort on purpose: a bug that returns a constant, or that
# mixes two cohorts up, has to disagree with at least one of them.
#
# Every statement here SUCCEEDS. That is a scoping requirement, not tidiness
# — a statement that errors mid-execution emits EXEC_START with no
# EXEC_DONE, so it contributes a plan and no completed execution, and this
# cohort was briefly written with an INSERT that violated a NOT NULL
# constraint: exec_count 0 against plan_count 12, in BOTH phases. The error
# path is tested deliberately in ERRORS below, where that is the point.
COHORTS = {
    # Simple protocol, plannable SELECT. The CONTROL: a SELECT never goes
    # through PortalRunMulti, so it was correct before the fix and must be
    # correct after it, in BOTH phases. If this number ever moves with the
    # mutant, the fix is suppressing executions rather than deduplicating a
    # probe site.
    "select": {"table": "pgwt464_s", "n": 16, "dml": False, "pk": True,
               "sql": "SELECT count(*) FROM pgwt464_s WHERE v >= 0;"},
    # Simple protocol, plannable UPDATE: the shape that doubled.
    "update": {"table": "pgwt464_u", "n": 16, "dml": True, "pk": True,
               "sql": "UPDATE pgwt464_u SET v = v + 1 WHERE id = 7;"},
    # Two more DML shapes, so the result is a property of plannable
    # non-SELECT work rather than of UPDATE specifically.
    "insert": {"table": "pgwt464_i", "n": 12, "dml": True, "pk": False,
               "sql": "INSERT INTO pgwt464_i(v) VALUES (1);"},
    "delete": {"table": "pgwt464_d", "n": 10, "dml": True, "pk": True,
               "sql": "DELETE FROM pgwt464_d WHERE id = 3;"},
    # Extended (Parse/Bind/Execute) protocol, via psql's \bind. The
    # requirement here is the weaker one — Plans <= Execs, Execs == N —
    # because a bound statement may reuse a plan. Measured on gate-1
    # 2026-10-19 it is 14/14, but the assertion stays the inequality plus
    # the independent N, which is the pair that actually holds for any
    # plan-caching decision PostgreSQL makes. This cohort is the reason the
    # independent N is load-bearing: the inequality alone is satisfied by
    # ANY multiple of N.
    "bind": {"table": "pgwt464_b", "n": 14, "dml": True, "pk": True,
             "sql": "UPDATE pgwt464_b SET v = v + 1 WHERE id = $1 \\bind 7 \\g"},
}

# Genuine nested PortalRun, which the fix MUST NOT flatten.
#
# `EXECUTE p(...)` is itself a utility statement: it runs in its own portal
# (PortalRun), and ExecuteQuery then creates a second portal for the
# prepared UPDATE and runs that (PortalRun again). Measured on gate-1,
# fixed build: two rows per EXECUTE, the SAME query id, the inner one
# strictly inside the outer, e.g.
#
#     qid=7123880983096715337 start=...002416954 end=...003705502 plan_ms=None
#     qid=7123880983096715337 start=...003330332 end=...003694233 plan_ms=0.85
#
# This is why a six-microsecond deduplicator, a same-query-id collapse and
# an unconditional outermost-only counter were all ruled out: every one of
# them would also destroy THIS, which is a real pair of executions.
NESTED = {"table": "pgwt464_p", "n": 10,
          "levels": 2}   # PortalRun invocations per EXECUTE

# Error cohort: successful statements on both sides of four failures. The
# failures must stay out of the completed statistics AND must not disturb
# the pairing of the commands that follow — the second half of the OK
# statements is that evidence, because a poisoned per-pid stack would make
# the total fall short of 2 * n_ok_each_side.
#
# The failing statement uses its OWN table, because the cohort lookup is by
# table name: an error statement naming pgwt464_e would match the same table
# as the successful one and the two rows would be reported as a
# non-isolated cohort (it was, on the first live run).
ERRORS = {"table": "pgwt464_e", "n_ok_each_side": 8, "n_err": 4,
          "err_table": "pgwt464_f",
          "err_sql": "UPDATE pgwt464_f SET v = v / 0 WHERE id = 5;"}


def psql_batch(statements, label):
    """Run `statements` as one psql session and return its backend pid.

    One session per cohort is what makes the raw-executions leg checkable:
    `executions` can be filtered by pid, so a cohort's rows can be counted
    without going through the query-id attribution the top_queries leg
    already tests. The pid comes from the session's OWN pg_backend_pid() --
    never from pg_stat_activity, which would be a different session
    answering about this one. (That first statement is itself a plannable
    SELECT and shows up as an execution, which is exactly why every
    executions query below is filtered by query_id as well as pid.)"""
    script = "SELECT pg_backend_pid();\n" + "\n".join(statements) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as fh:
        fh.write(script)
        path = fh.name
    try:
        res = subprocess.run(
            ["psql", "-U", "postgres", "-d", "postgres", "-q", "-t", "-A",
             "-f", path],
            capture_output=True, text=True, timeout=180)
    finally:
        os.unlink(path)
    lines = (res.stdout or "").strip().splitlines()
    if not lines or not lines[0].strip().isdigit():
        fatal(f"{label}: could not read the cohort backend pid from psql "
              f"(rc={res.returncode}, stdout={res.stdout[:200]!r}, "
              f"stderr={res.stderr[-400:]!r})")
    return int(lines[0].strip()), res


def setup_tables():
    ddl = []
    specs = list(COHORTS.values()) + [NESTED, ERRORS,
                                      {"table": ERRORS["err_table"]}]
    for spec in specs:
        t = spec["table"]
        pk = "primary key" if spec.get("pk", True) else ""
        ddl.append(f"DROP TABLE IF EXISTS {t};")
        ddl.append(f"CREATE TABLE {t}(id int {pk}, v int);")
        ddl.append(f"INSERT INTO {t} SELECT g, 0 FROM generate_series(1,200) g;")
    res = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-q", "-v",
         "ON_ERROR_STOP=1", "-c", " ".join(ddl)],
        capture_output=True, text=True, timeout=180)
    if res.returncode != 0:
        fatal(f"cohort table setup failed: {res.stderr[-600:]!r}")


def ctl(trace_dir, request, timeout=5):
    path = os.path.join(trace_dir, "pgwt.sock")
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    try:
        client.connect(path)
        client.sendall((json.dumps(request) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        return json.loads(data.splitlines()[0])
    finally:
        client.close()


def start_daemon(pm_pid, admit_all):
    trace_dir = tempfile.mkdtemp(prefix="pgwt_exec_card_")
    os.chmod(trace_dir, 0o755)
    env = os.environ.copy()
    if admit_all:
        env["PGWT_TEST_EXEC_SITE_ADMIT_ALL"] = "1"
    else:
        env.pop("PGWT_TEST_EXEC_SITE_ADMIT_ALL", None)
    err_path = os.path.join(trace_dir, "daemon.err")
    err = open(err_path, "w")
    proc = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--trace-dir", trace_dir, "--duration", str(CAPTURE_DURATION_S),
         "--interval", "1", "--quiet"],
        stdout=subprocess.DEVNULL, stderr=err, env=env)
    # Readiness is the control socket ANSWERING, not a sleep: a daemon that
    # never comes up must fail HERE with its stderr attached, not stall
    # until the cohort has already run uncaptured and the counts come back
    # as zeroes nobody can explain. Bounded, and a dead process is detected
    # rather than waited on.
    deadline = time.monotonic() + 60
    ready = False
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        if os.path.exists(os.path.join(trace_dir, "pgwt.sock")):
            try:
                if ctl(trace_dir, {"cmd": "status"}).get("ok") is True:
                    ready = True
                    break
            except (OSError, ValueError, IndexError):
                pass
        time.sleep(0.1)
    if not ready:
        err.close()
        try:
            log = open(err_path).read()[-1500:]
        except OSError:
            log = "(no log)"
        proc.kill()
        fatal(f"daemon never became ready (admit_all={admit_all}); "
              f"stderr tail:\n{log}")
    return proc, trace_dir, err, err_path


def stop_daemon(proc, err):
    proc.terminate()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
    err.close()


def run_cohorts():
    """Issue every cohort and return {name: pid} plus the wall-clock span
    the cohorts occupied, in ns."""
    pids = {}
    start_ns = time.time_ns()

    for name, spec in COHORTS.items():
        pids[name], _ = psql_batch([spec["sql"]] * spec["n"], name)

    t = NESTED["table"]
    stmts = [f"PREPARE p464(int) AS UPDATE {t} SET v = v + 1 WHERE id = $1;"]
    stmts += [f"EXECUTE p464({i % 5 + 1});" for i in range(NESTED["n"])]
    pids["nested"], _ = psql_batch(stmts, "nested")

    t = ERRORS["table"]
    ok = f"UPDATE {t} SET v = v + 1 WHERE id = 3;"
    stmts = ([ok] * ERRORS["n_ok_each_side"]
             + [ERRORS["err_sql"]] * ERRORS["n_err"]
             + [ok] * ERRORS["n_ok_each_side"])
    pids["errors"], _ = psql_batch(stmts, "errors")

    return pids, start_ns, time.time_ns()


def rows_by_table(top_queries_rows):
    """Cohort row lookup by table name.

    A table matching more than one row means the cohort was NOT isolated
    (normalisation merged something, or an unrelated statement touches the
    table). The ambiguity is returned rather than resolved: picking a row
    would make an isolation failure read as a count."""
    out = {}
    for row in top_queries_rows or []:
        text = row.get("text") or row.get("query_text") or ""
        for spec in list(COHORTS.values()) + [NESTED, ERRORS]:
            t = spec["table"]
            if t in text:
                out.setdefault(t, []).append(row)
    return out


def capture_phase(pm_pid, admit_all):
    """One full capture: start the daemon, run the cohorts, drain, stop,
    then collect everything the assertions need."""
    proc, trace_dir, err, err_path = start_daemon(pm_pid, admit_all)
    try:
        time.sleep(CAPTURE_SETTLE_S)
        pids, cohort_from_ns, cohort_to_ns = run_cohorts()
        time.sleep(CAPTURE_DRAIN_S)
    finally:
        stop_daemon(proc, err)

    with open(err_path) as fh:
        daemon_log = fh.read()

    with ServerHarness(trace_dir) as srv:
        info = srv.query("info")
        tq = srv.query("top_queries", limit=200)
        found = rows_by_table(tq.get("rows"))
        # Raw executions, scoped by pid AND query_id. The query_id half
        # matters: the session's own `SELECT pg_backend_pid()` is a
        # plannable SELECT and contributes a real execution, so a pid-only
        # filter is off by exactly one -- the sort of off-by-one that gets
        # "fixed" by widening an assertion instead of scoping it.
        execs = {}
        for name, spec in list(COHORTS.items()) + [("nested", NESTED),
                                                   ("errors", ERRORS)]:
            rows = found.get(spec["table"]) or []
            if len(rows) != 1:
                execs[name] = None       # ambiguity is reported, not queried
                continue
            qid = rows[0].get("query_id")
            execs[name] = srv.query(
                "executions", limit=1000,
                filters={"pid": pids[name], "query_id": qid})
        # The error statement's own rows, to show the failures contribute no
        # completed execution.
        err_rows, err_row = None, None
        for row in tq.get("rows") or []:
            if ERRORS["err_table"] in (row.get("text") or ""):
                err_row = row
                err_rows = srv.query("executions", limit=1000,
                                     filters={"pid": pids["errors"],
                                              "query_id": row.get("query_id")})
    return {
        "admit_all": admit_all,
        "trace_dir": trace_dir,
        "daemon_log": daemon_log,
        "pids": pids,
        "cohort_from_ns": cohort_from_ns,
        "cohort_to_ns": cohort_to_ns,
        "info": info,
        "top_queries": tq,
        "found": found,
        "executions": execs,
        "error_executions": err_rows,
        "error_row": err_row,
    }


# ── Assertions ───────────────────────────────────────────────────────────

def assert_capture_is_usable(phase):
    """Preconditions. Each of these, left unchecked, is a way for the
    cardinality assertions to pass without having examined a real capture."""
    tag = "mutant" if phase["admit_all"] else "fixed"
    log = phase["daemon_log"]
    site_lines = [l for l in log.splitlines() if "site(s)" in l]

    check("REFUSED" not in log,
          f"{tag}: daemon did not refuse execution telemetry")
    if phase["admit_all"]:
        check("PGWT_TEST_EXEC_SITE_ADMIT_ALL=1" in log,
              f"{tag}: the mutant hook really was active")
        check("4 site(s) admitted" in log,
              f"{tag}: all four declared probe sites published ({site_lines})")
    else:
        check("2 site(s) admitted" in log and "2 excluded" in log,
              f"{tag}: exactly 2 sites admitted and 2 excluded ({site_lines})")

    # The window must FULLY contain the cohort. A capture that started late
    # or stopped early measures a fraction of N, and a fraction that happens
    # to land near N/2 is indistinguishable from the defect.
    frm = int(phase["info"].get("from_ns") or 0)
    to = int(phase["info"].get("to_ns") or 0)
    check(0 < frm <= phase["cohort_from_ns"],
          f"{tag}: capture began before the cohort "
          f"(trace from {frm}, cohort from {phase['cohort_from_ns']})")
    check(to >= phase["cohort_to_ns"],
          f"{tag}: capture ended after the cohort "
          f"(trace to {to}, cohort to {phase['cohort_to_ns']})")

    tq = phase["top_queries"]
    check("error" not in tq, f"{tag}: top_queries answered (no error)")
    check(tq.get("fidelity") == "exact",
          f"{tag}: fidelity is 'exact', got {tq.get('fidelity')!r}")
    check(not tq.get("truncated", False), f"{tag}: top_queries not truncated")

    for spec in list(COHORTS.values()) + [NESTED, ERRORS]:
        t = spec["table"]
        matches = phase["found"].get(t, [])
        # Missing is a FAILURE, not a skip: "the statements I issued
        # produced no row" is what a refusing or mis-attached capture looks
        # like, and it must never read as agreement.
        check(len(matches) == 1,
              f"{tag}: exactly one top_queries row for {t} (got {len(matches)})")
        check(phase["executions"].get(
                  _cohort_name_for_table(t)) is not None,
              f"{tag}: raw executions answered for {t}")


def _cohort_name_for_table(table):
    for name, spec in COHORTS.items():
        if spec["table"] == table:
            return name
    if NESTED["table"] == table:
        return "nested"
    return "errors"


def assert_fixed_phase(phase):
    found, execs = phase["found"], phase["executions"]
    tq_rows = phase["top_queries"].get("rows") or []

    # 1. Simple protocol, successful plannable statements: Plans = Execs = N.
    for name, spec in COHORTS.items():
        if name == "bind":
            continue
        t, n = spec["table"], spec["n"]
        row = (found.get(t) or [{}])[0]
        check(row.get("exec_count") == n,
              f"fixed/{name}: exec_count == N ({row.get('exec_count')} vs {n})")
        check(row.get("plan_count") == n,
              f"fixed/{name}: plan_count == N ({row.get('plan_count')} vs {n})")

    # 2. Extended protocol: Plans <= Execs, and Execs == N. The inequality
    #    alone cannot catch doubling; the independent N is what does.
    spec = COHORTS["bind"]
    row = (found.get(spec["table"]) or [{}])[0]
    bexec, bplan = row.get("exec_count"), row.get("plan_count")
    check(bexec == spec["n"],
          f"fixed/bind: exec_count == N ({bexec} vs {spec['n']})")
    check(isinstance(bplan, int) and isinstance(bexec, int) and bplan <= bexec,
          f"fixed/bind: plan_count <= exec_count ({bplan} <= {bexec})")

    # 3. Error/cancel: the successful statements on both sides of the
    #    failures still pair, and the failures add no completed execution.
    n_ok = ERRORS["n_ok_each_side"] * 2
    row = (found.get(ERRORS["table"]) or [{}])[0]
    check(row.get("exec_count") == n_ok,
          f"fixed/errors: {n_ok} successful statements paired, exec_count "
          f"{row.get('exec_count')} -- later commands still pair")
    check(row.get("plan_count") == n_ok,
          f"fixed/errors: plan_count == {n_ok} ({row.get('plan_count')})")
    er, erow = phase["error_executions"], phase["error_row"]
    check(er is not None and erow is not None,
          "fixed/errors: the failing statement has its own top_queries row")
    if er is not None and erow is not None:
        # Out of the COMPLETED STATISTICS: exec_count is what the Queries
        # tab shows as Execs, and a statement that never reached
        # EXEC_DONE contributes none.
        check(erow.get("exec_count") in (0, None),
              f"fixed/errors: the failing statement's exec_count is 0/absent "
              f"({erow.get('exec_count')})")
        # Its raw rows exist (the executions view is a timeline, not a
        # statistic) but every one of them is LABELLED end_inferred, so no
        # consumer can mistake a CMD_END-closed orphan for a real
        # completion.
        rows = er.get("rows") or []
        inferred = [r for r in rows if r.get("end_inferred")]
        check(len(rows) == ERRORS["n_err"],
              f"fixed/errors: {ERRORS['n_err']} raw rows for the failing "
              f"statement (got {len(rows)})")
        check(rows and len(inferred) == len(rows),
              f"fixed/errors: every failing row is labelled end_inferred "
              f"({len(inferred)}/{len(rows)})")

    # 4. Raw executions, per cohort, through a different server command and
    #    a different filter than the counts above.
    for name, spec in COHORTS.items():
        n, resp = spec["n"], execs[name]
        check(not resp.get("truncated", False),
              f"fixed/{name}: executions response not truncated")
        check(resp.get("completed_count") == n,
              f"fixed/{name}: raw completed executions == N "
              f"({resp.get('completed_count')} vs {n})")
        check(resp.get("open_count") == 0,
              f"fixed/{name}: no execution left open ({resp.get('open_count')})")

    # 5. Each canonical execution keeps its applicable plan: the plan
    #    boundary survived instead of being consumed by a phantom row.
    for name in ("update", "select", "insert", "delete"):
        rows = execs[name].get("rows") or []
        with_plan = [r for r in rows if r.get("plan_ms") is not None]
        check(len(rows) == COHORTS[name]["n"],
              f"fixed/{name}: {COHORTS[name]['n']} raw rows returned "
              f"(got {len(rows)})")
        check(rows and len(with_plan) == len(rows),
              f"fixed/{name}: every raw execution carries its plan "
              f"({len(with_plan)}/{len(rows)})")
        # And every one closed on a real EXEC_DONE rather than being swept
        # up by CMD_END: an inferred close here would mean the canonical
        # DONE site is not the partner of the canonical START site.
        real = [r for r in rows if not r.get("end_inferred")]
        check(rows and len(real) == len(rows),
              f"fixed/{name}: every raw execution closed on a real "
              f"EXEC_DONE, none inferred ({len(real)}/{len(rows)})")

    # 6. GENUINE nesting survives. Each EXECUTE is two real PortalRun
    #    invocations with the SAME query id, the inner strictly inside the
    #    outer. A deduplicator, a same-id collapse or an outermost-only
    #    counter would each reduce this to N.
    nrows = sorted((execs["nested"].get("rows") or []),
                   key=lambda r: int(r["start_ns"]))
    want = NESTED["n"] * NESTED["levels"]
    check(execs["nested"].get("completed_count") == want,
          f"fixed/nested: {NESTED['n']} EXECUTEs produce {want} nested "
          f"PortalRun executions ({execs['nested'].get('completed_count')})")
    qids = {r.get("query_id") for r in nrows}
    check(len(qids) == 1,
          f"fixed/nested: all nested rows share one query id ({qids})")
    contained = 0
    for i, outer in enumerate(nrows):
        if outer.get("end_ns") is None:
            continue
        o_s, o_e = int(outer["start_ns"]), int(outer["end_ns"])
        for inner in nrows[i + 1:]:
            if inner.get("end_ns") is None:
                continue
            if o_s <= int(inner["start_ns"]) and int(inner["end_ns"]) <= o_e:
                contained += 1
                break
    check(contained == NESTED["n"],
          f"fixed/nested: {NESTED['n']} outer executions each strictly "
          f"contain an inner one (got {contained})")

    # 7. The shared evaluator -- the SAME function the demo rehearsal now
    #    gates on -- must agree, with the independent N supplied.
    expected = {spec["table"]: spec["n"] for spec in COHORTS.values()}
    expected[ERRORS["table"]] = ERRORS["n_ok_each_side"] * 2
    verdict = drlib.evaluate_execution_cardinality(tq_rows, expected=expected)
    check(verdict["ok"],
          f"fixed: evaluate_execution_cardinality PASSES ({verdict['detail']})")
    check(verdict["evaluated"] >= len(expected),
          f"fixed: the evaluator looked at at least {len(expected)} rows "
          f"({verdict['evaluated']})")


def assert_mutant_phase(phase):
    """Negative control 1, on EVERY run: with both probe sites published the
    cohort must produce the pre-fix double count and the gate must say so.
    A clean mutant phase is a FAILING test -- that is the gate having gone
    blind, which is exactly the state in which #464 shipped."""
    found, execs = phase["found"], phase["executions"]
    tq_rows = phase["top_queries"].get("rows") or []

    for name, spec in COHORTS.items():
        t, n = spec["table"], spec["n"]
        row = (found.get(t) or [{}])[0]
        want = 2 * n if spec["dml"] else n
        shape = "doubles" if spec["dml"] else "is UNCHANGED"
        check(row.get("exec_count") == want,
              f"mutant/{name}: exec_count {shape} with both sites live "
              f"({row.get('exec_count')} vs {want})")
        # Planning is a single site in every PostgreSQL checked, so the
        # mutant must move execs ONLY. If plans moved too, the mutant is not
        # the defect it claims to reproduce.
        check(row.get("plan_count") == n,
              f"mutant/{name}: plan_count unaffected "
              f"({row.get('plan_count')} vs {n})")
        resp = execs[name]
        check(resp.get("completed_count") == want,
              f"mutant/{name}: raw completed executions {want} "
              f"({resp.get('completed_count')})")

    # Nesting gains the inner helper pair on the inner portal: the outer
    # EXECUTE portal is a utility portal (no PortalRunMulti), the inner
    # UPDATE portal gets two. Strictly more than the fixed phase's 2N --
    # pinned as an inequality rather than an exact 3N because what is being
    # asserted is "the mutant also corrupts nesting", and the exact figure
    # depends on how PostgreSQL routes a prepared UPDATE's portal.
    nested_completed = execs["nested"].get("completed_count")
    check(isinstance(nested_completed, int)
          and nested_completed > NESTED["n"] * NESTED["levels"],
          f"mutant/nested: nested executions inflate beyond "
          f"{NESTED['n'] * NESTED['levels']} ({nested_completed})")

    # The evaluator must reject it. Without this, every equality above could
    # be satisfied by a capture that simply never recorded anything.
    expected = {spec["table"]: spec["n"] for spec in COHORTS.values()}
    verdict = drlib.evaluate_execution_cardinality(tq_rows, expected=expected)
    check(not verdict["ok"],
          f"mutant: evaluate_execution_cardinality FAILS as it must "
          f"({verdict['detail']})")
    check(any(COHORTS["update"]["table"] in f for f in verdict["failures"]),
          f"mutant: the doubled DML cohort is named in the failure "
          f"({verdict['failures'][:3]})")
    # And the ratio leg alone -- no independent N -- must also catch it,
    # because that is the leg the demo rehearsal runs.
    ratio_only = drlib.evaluate_execution_cardinality(tq_rows)
    check(not ratio_only["ok"],
          f"mutant: the ratio leg alone also FAILS, which is what the "
          f"rehearsal check uses ({ratio_only['detail']})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--pg-version", type=int, default=0)
    args = ap.parse_args()

    print("test_execution_cardinality: #464 one execution per execution")

    pm_pid = args.pid or find_postmaster(args.pg_version or None)
    require_phase_probes("test_execution_cardinality", pm_pid)
    if os.geteuid() != 0:
        fatal("requires root (eBPF capture)")
    if not os.path.exists(TRACER):
        fatal(f"pg_wait_tracer not built at {TRACER}")

    if not pm_pid:
        fatal("no postmaster found")
    print(f"  postmaster pid {pm_pid}, PGPORT={os.environ.get('PGPORT')}")

    setup_tables()

    print("--- phase 1: shipped filter (canonical PortalRun site only) ---")
    fixed = capture_phase(pm_pid, admit_all=False)
    assert_capture_is_usable(fixed)
    assert_fixed_phase(fixed)

    print("--- phase 2: PGWT_TEST_EXEC_SITE_ADMIT_ALL=1 (the #464 mutant) ---")
    mutant = capture_phase(pm_pid, admit_all=True)
    assert_capture_is_usable(mutant)
    assert_mutant_phase(mutant)

    print(f"\ntest_execution_cardinality: {tests_run - tests_failed}/"
          f"{tests_run} checks passed")
    if tests_failed:
        print(f"test_execution_cardinality: FAILED ({tests_failed})")
        print(f"  fixed trace:  {fixed['trace_dir']}")
        print(f"  mutant trace: {mutant['trace_dir']}")
        return 1
    print("test_execution_cardinality: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
