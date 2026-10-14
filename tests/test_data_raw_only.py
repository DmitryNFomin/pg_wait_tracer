#!/usr/bin/env python3
"""test_data_raw_only.py — PGWT_SUMMARY_DISABLE (raw-only mode).

`should_use_summaries()` (src/server.c) routes any unfiltered window >= 120 s
to the per-second summary fast path. That path is an acceleration, not the
source of truth, and on a real capture (run.id 1791973533) it differs from the
raw path on an unfiltered 15-minute window: DB Time/AAS +1.14%, the CPU class
+3.54% and collapsed to one row so "CPU (waiting for a core)" disappears
(#408), Lock:relation +4414.5 ms, Sessions' top wait wrong on 26 of 81 rows
(#319), Queries' ranks 7 and 8 swapped (#379). PGWT_SUMMARY_DISABLE makes the
server decline that route for every request.

WHAT THIS FILE PROVES, AND WHY IT IS NOT "THE VARIABLE WAS READ".
A startup diagnostic only shows that getenv() ran. These tests show the ROUTE
changed, by two mechanisms independent of each other and of timing:

  (1) FIDELITY SOURCE. For a >= 120 s window that is exact-COVERED but holds
      no events, the summary route labels the answer from coverage ("exact",
      add_fidelity_window) and the raw route labels it from the load ("none",
      add_fidelity). Same request, same files, two labels — so the label
      names the route.

  (2) THE RAW LOADER'S BOUND. With PGWT_LOAD_MAX_EVENTS tiny, the summary
      route answers normally (it never materialises raw events) while the raw
      route trips reject_overload and refuses with "window_too_large". Only
      the loader produces that refusal, so only the raw route can — and this
      is simultaneously the check that raw-only does NOT turn an overload
      into a silent partial answer.

Plus a BYPASS SUITE: every way this detection could be made unreachable —
summary files absent (nothing left to decline), a window below the 120 s floor
(the route never applies), a typo'd variable name, the documented off values,
an unparseable value, a trace dir with no files, and a missing or
non-executable pgwt-server. A gate that cannot see must refuse, never approve.
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    SERVER_BIN, CPU, IO_DATA_FILE_READ, LOCK_RELATION,
)

BASE = 10_000_000_000_000
S = 1_000_000_000
RAW_ONLY = {"PGWT_SUMMARY_DISABLE": "1"}
TINY_BUDGET = "10"      # events; the 320 s window below holds 300

SCRATCH = tempfile.mkdtemp(prefix="pgwt_rawonly_")


# ── Scenarios ─────────────────────────────────────────────────

def build_busy_320s():
    """150 x IO:DataFileRead 1 ms + 150 x Lock:relation 100 ms, one per
    second over 300 s, fully exact — the pattern test_data_events.py uses to
    reach the summary path. Unfiltered windows over it are >= 120 s and
    EXACT, i.e. exactly what should_use_summaries() routes to summaries."""
    events = []
    ts = BASE
    for _ in range(150):
        ts += S
        events.append({"pid": 1000, "ts": ts, "dur": 1_000_000,
                       "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
        ts += S
        events.append({"pid": 1000, "ts": ts, "dur": 100_000_000,
                       "old": LOCK_RELATION, "new": CPU, "qid": 100})
    return ({
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }, BASE - 10 * S, ts + 10 * S)


def build_gap_450s():
    """Events in the first 50 s and in seconds 400-450, nothing between."""
    events = []
    for i in range(50):
        events.append({"pid": 1000, "ts": BASE + (i + 1) * S, "dur": 1_000_000,
                       "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
    for i in range(50):
        events.append({"pid": 1000, "ts": BASE + (400 + i) * S,
                       "dur": 1_000_000, "old": IO_DATA_FILE_READ,
                       "new": CPU, "qid": 100})
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }


# The covered-but-empty window inside build_gap_450s(): 200 s, >= the floor.
GAP_FROM, GAP_TO = BASE + 150 * S, BASE + 350 * S
# Same shape, 119 s: BELOW the floor, so the route never applies.
SHORT_FROM, SHORT_TO = BASE + 150 * S, BASE + 269 * S


def ask(trace_dir, env, cmd, frm, to, stderr_path=None):
    with ServerHarness(trace_dir, env=env, stderr_path=stderr_path) as srv:
        return srv.query(cmd, from_=frm, to_=to)


# ── 1. Route proof A: the fidelity label names the route ──────

def test_route_fidelity_source(t):
    print("\n### 1. route proof A: fidelity source on a covered-but-empty "
          "200 s window ###")
    t.check((GAP_TO - GAP_FROM) >= 120 * S,
            "window is %.0f s, i.e. >= the 120 s routing floor"
            % ((GAP_TO - GAP_FROM) / 1e9))
    trace_dir = generate_traces(build_gap_450s())
    try:
        t.check(os.path.exists(os.path.join(trace_dir, "current.summary")),
                "summary file exists, so there IS a summary route to decline")
        default = ask(trace_dir, None, "time_model", GAP_FROM, GAP_TO)
        raw = ask(trace_dir, RAW_ONLY, "time_model", GAP_FROM, GAP_TO)
        # Default: coverage-derived label -> the summary route ran.
        t.check_eq(default.get("fidelity"), "exact",
                   "default: fidelity 'exact' (coverage-derived => summary "
                   "route)")
        # Raw-only: load-derived label -> the raw path ran.
        t.check_eq(raw.get("fidelity"), "none",
                   "raw-only: fidelity 'none' (load-derived => raw route)")
        t.check(default.get("fidelity") != raw.get("fidelity"),
                "the same request takes a different route with the variable "
                "set")
        # ... and the difference is the ROUTE, not a broken answer: an empty
        # window is 0 ms of DB Time on both paths.
        t.check_eq(default.get("db_time_ms"), 0.0,
                   "default: empty window = 0 ms DB Time")
        t.check_eq(raw.get("db_time_ms"), 0.0,
                   "raw-only: empty window = 0 ms DB Time (no data lost)")
    finally:
        cleanup_traces(trace_dir)


# ── 2. Route proof B: the raw loader's bound, and the refusal ─

def test_route_loader_bound_and_refusal(t):
    print("\n### 2. route proof B + overload refusal (tiny raw budget) ###")
    scenario, frm, to = build_busy_320s()
    trace_dir = generate_traces(scenario)
    env_d = {"PGWT_LOAD_MAX_EVENTS": TINY_BUDGET}
    env_r = dict(env_d, **RAW_ONLY)
    try:
        # Default: the summary route never materialises raw events, so a
        # 10-event budget does not bother it at all.
        with ServerHarness(trace_dir, env=env_d) as srv:
            d_tm = srv.query("time_model", from_=frm, to_=to)
            d_ev = srv.query("top_events", from_=frm, to_=to)
        t.check_eq(d_tm.get("code"), None,
                   "default: 320 s window answered despite a 10-event raw "
                   "budget (summary route)")
        t.check_approx(d_tm.get("db_time_ms", -1), 15150.0, 0.001,
                       "default: DB Time = 15150 ms")
        t.check_eq(len(d_ev.get("rows", [])), 2,
                   "default: top_events returns both event rows")

        # Raw-only: every one of these goes through the loader, trips the
        # bound, and REFUSES. Never a partial answer.
        with ServerHarness(trace_dir, env=env_r) as srv:
            for cmd in ("time_model", "top_events", "aas", "top_sessions"):
                r = srv.query(cmd, from_=frm, to_=to)
                t.check_eq(r.get("code"), "window_too_large",
                           f"raw-only: {cmd} refuses (window_too_large) => "
                           f"the raw loader ran")
                t.check_eq(r.get("error"), "window too large",
                           f"raw-only: {cmd} refusal carries the error text")
                t.check_eq(len(r.get("rows", [])), 0,
                           f"raw-only: {cmd} refusal has NO rows (not a "
                           f"partial answer)")
                t.check_eq(r.get("db_time_ms"), None,
                           f"raw-only: {cmd} refusal invents no DB Time")
                t.check(r.get("max_events") is not None,
                        f"raw-only: {cmd} refusal names the bound it hit")
    finally:
        cleanup_traces(trace_dir)


# ── 3. Default behaviour is unchanged ─────────────────────────

def test_default_unchanged(t):
    print("\n### 3. without the variable, the summary route is still taken "
          "###")
    scenario, frm, to = build_busy_320s()
    trace_dir = generate_traces(scenario)
    err = os.path.join(SCRATCH, "default.err")
    try:
        with ServerHarness(trace_dir, stderr_path=err) as srv:
            ev = srv.query("top_events", from_=frm, to_=to)
            tm = srv.query("time_model", from_=frm, to_=to)
        rows = {r["name"]: r for r in ev.get("rows", [])}
        t.check_eq(ev.get("fidelity"), "exact", "default: fidelity exact")
        t.check_eq(rows.get("Lock:relation", {}).get("count"), 150,
                   "default: Lock:relation count = 150")
        t.check_approx(rows.get("Lock:relation", {}).get("total_ms", -1),
                       15000.0, 0.001, "default: Lock:relation = 15000 ms")
        t.check_approx(tm.get("db_time_ms", -1), 15150.0, 0.001,
                       "default: DB Time = 15150 ms")
        text = open(err).read()
        t.check("RAW-ONLY" not in text,
                "default: no raw-only line on stderr")
        # The route itself, independently of any value above (see test 2's
        # default leg for the loader-bound half of this).
        gap = generate_traces(build_gap_450s())
        try:
            t.check_eq(ask(gap, None, "time_model",
                           GAP_FROM, GAP_TO).get("fidelity"), "exact",
                       "default: covered-but-empty 200 s window still takes "
                       "the summary route")
        finally:
            cleanup_traces(gap)
    finally:
        cleanup_traces(trace_dir)


# ── 4. Raw-only does not change the numbers it should not ─────

def test_values_agree_where_they_must(t):
    print("\n### 4. raw-only is the same answer on a capture where the two "
          "paths agree ###")
    scenario, frm, to = build_busy_320s()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            d_ev = srv.query("top_events", from_=frm, to_=to)
            d_tm = srv.query("time_model", from_=frm, to_=to)
        with ServerHarness(trace_dir, env=RAW_ONLY) as srv:
            r_ev = srv.query("top_events", from_=frm, to_=to)
            r_tm = srv.query("time_model", from_=frm, to_=to)
        t.check_approx(r_tm.get("db_time_ms", -1),
                       d_tm.get("db_time_ms", -2), 1e-9,
                       "raw-only DB Time == default DB Time on this capture")
        d_rows = {r["name"]: r for r in d_ev.get("rows", [])}
        r_rows = {r["name"]: r for r in r_ev.get("rows", [])}
        t.check_eq(sorted(r_rows), sorted(d_rows),
                   "raw-only returns the same event rows")
        for name in ("Lock:relation", "IO:DataFileRead"):
            t.check_eq(r_rows.get(name, {}).get("count"),
                       d_rows.get(name, {}).get("count"),
                       f"raw-only {name} count unchanged")
            t.check_approx(r_rows.get(name, {}).get("total_ms", -1),
                           d_rows.get(name, {}).get("total_ms", -2), 1e-9,
                           f"raw-only {name} total_ms unchanged")
        t.check_eq(r_rows.get("Lock:relation", {}).get("count"), 150,
                   "raw-only Lock:relation count = 150 (a real answer, not "
                   "an empty one)")
    finally:
        cleanup_traces(trace_dir)


# ── 5. The startup diagnostic ─────────────────────────────────

def test_startup_diagnostic(t):
    print("\n### 5. startup says raw-only is active, and why ###")
    scenario, frm, to = build_busy_320s()
    trace_dir = generate_traces(scenario)
    err = os.path.join(SCRATCH, "rawonly.err")
    try:
        with ServerHarness(trace_dir, env=RAW_ONLY, stderr_path=err) as srv:
            srv.query("time_model", from_=frm, to_=to)
        text = open(err).read()
        t.check("RAW-ONLY MODE" in text, "stderr announces RAW-ONLY MODE")
        t.check("PGWT_SUMMARY_DISABLE" in text,
                "stderr names the variable that caused it")
        for issue in ("#319", "#379", "#408"):
            t.check(issue in text, f"stderr cites {issue} as the reason")
        t.check("window_too_large" in text,
                "stderr states the cost: a too-large window is refused")
    finally:
        cleanup_traces(trace_dir)


# ── 6. BYPASS SUITE ───────────────────────────────────────────

def test_bypass_no_summary_files(t):
    """The ways this detection could pass while checking nothing. #1: no
    summary file at all. Then the default run ALSO takes the raw path and
    there is nothing to decline — test 1's discriminator must stop
    discriminating, which is how we know it reads the route and not a
    constant."""
    print("\n### 6a. bypass: summary files absent => nothing to decline ###")
    trace_dir = generate_traces(build_gap_450s())
    try:
        for name in ("current.summary", "current.summary.meta"):
            p = os.path.join(trace_dir, name)
            if os.path.exists(p):
                os.remove(p)
        d = ask(trace_dir, None, "time_model", GAP_FROM, GAP_TO)
        r = ask(trace_dir, RAW_ONLY, "time_model", GAP_FROM, GAP_TO)
        t.check_eq(d.get("fidelity"), "none",
                   "no summary file: default falls back to raw ('none')")
        t.check_eq(r.get("fidelity"), "none",
                   "no summary file: raw-only is the same raw answer")
        t.check(d.get("fidelity") == r.get("fidelity"),
                "the route discriminator is INSENSITIVE here by "
                "construction -- so test 1 passing means a route really was "
                "declined")
    finally:
        cleanup_traces(trace_dir)


def test_bypass_below_floor(t):
    print("\n### 6b. bypass: 119 s window => the route never applies ###")
    trace_dir = generate_traces(build_gap_450s())
    try:
        t.check((SHORT_TO - SHORT_FROM) < 120 * S,
                "window is %.0f s, i.e. below the 120 s floor"
                % ((SHORT_TO - SHORT_FROM) / 1e9))
        d = ask(trace_dir, None, "time_model", SHORT_FROM, SHORT_TO)
        r = ask(trace_dir, RAW_ONLY, "time_model", SHORT_FROM, SHORT_TO)
        t.check_eq(d.get("fidelity"), "none",
                   "below the floor: default already uses raw")
        t.check_eq(r.get("fidelity"), "none",
                   "below the floor: raw-only changes nothing")
    finally:
        cleanup_traces(trace_dir)


def test_bypass_values(t):
    print("\n### 6c. bypass: variable name and values ###")
    trace_dir = generate_traces(build_gap_450s())
    try:
        # A typo'd NAME must not enable raw-only -- and the startup line is
        # what makes its absence visible to the operator.
        err = os.path.join(SCRATCH, "typo.err")
        d = ask(trace_dir, {"PGWT_SUMMARY_DISABLED": "1"}, "time_model",
                GAP_FROM, GAP_TO, stderr_path=err)
        t.check_eq(d.get("fidelity"), "exact",
                   "typo'd name (PGWT_SUMMARY_DISABLED): summary route, "
                   "unchanged")
        t.check("RAW-ONLY" not in open(err).read(),
                "typo'd name: no raw-only line, so the operator can see it "
                "did not take")
        # The documented off values.
        for val, label in (("0", 'exactly "0"'), ("", "empty")):
            r = ask(trace_dir, {"PGWT_SUMMARY_DISABLE": val}, "time_model",
                    GAP_FROM, GAP_TO)
            t.check_eq(r.get("fidelity"), "exact",
                       f"PGWT_SUMMARY_DISABLE={label} is OFF (summary route)")
        # An unparseable value fails TOWARD the source of truth, loudly.
        err = os.path.join(SCRATCH, "junk.err")
        r = ask(trace_dir, {"PGWT_SUMMARY_DISABLE": "yes please"},
                "time_model", GAP_FROM, GAP_TO, stderr_path=err)
        t.check_eq(r.get("fidelity"), "none",
                   "unparseable value is ON (raw route), never silently "
                   "ignored")
        t.check("RAW-ONLY MODE" in open(err).read(),
                "unparseable value still announces raw-only")
    finally:
        cleanup_traces(trace_dir)


def test_bypass_nothing_to_read(t):
    """A gate that cannot see must refuse. Two absent-input cases: a trace
    dir with no files, and a pgwt-server that cannot be executed (the 126 /
    127 family). Neither may look like a pass."""
    print("\n### 6d. bypass: empty trace dir / unusable binary ###")
    empty = tempfile.mkdtemp(prefix="pgwt_empty_")
    try:
        p = subprocess.run([SERVER_BIN, empty], input="", capture_output=True,
                           text=True, timeout=30)
        t.check(p.returncode != 0,
                f"raw-only would-be server on an empty trace dir exits "
                f"non-zero ({p.returncode}), never serving nothing")
        t.check("no trace files found" in p.stderr,
                "empty trace dir: says why it refused")
    finally:
        shutil.rmtree(empty, ignore_errors=True)

    missing = os.path.join(SCRATCH, "not-a-server")
    try:
        subprocess.run([missing, SCRATCH], input="", capture_output=True,
                       text=True, timeout=30)
        t.check(False, "a MISSING pgwt-server must raise, not pass silently")
    except (FileNotFoundError, OSError) as e:
        t.check(True, f"missing pgwt-server fails loudly ({type(e).__name__})")

    noexec = os.path.join(SCRATCH, "pgwt-server-noexec")
    shutil.copyfile(SERVER_BIN, noexec)
    os.chmod(noexec, 0o000)
    try:
        subprocess.run([noexec, SCRATCH], input="", capture_output=True,
                       text=True, timeout=30)
        t.check(False,
                "a NON-EXECUTABLE pgwt-server must raise, not pass silently")
    except (PermissionError, OSError) as e:
        t.check(True,
                f"non-executable pgwt-server fails loudly "
                f"({type(e).__name__})")
    finally:
        os.chmod(noexec, 0o700)
        os.remove(noexec)


def main():
    t = TestRunner("raw_only")
    try:
        test_route_fidelity_source(t)
        test_route_loader_bound_and_refusal(t)
        test_default_unchanged(t)
        test_values_agree_where_they_must(t)
        test_startup_diagnostic(t)
        test_bypass_no_summary_files(t)
        test_bypass_below_floor(t)
        test_bypass_values(t)
        test_bypass_nothing_to_read(t)
    finally:
        shutil.rmtree(SCRATCH, ignore_errors=True)
    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
