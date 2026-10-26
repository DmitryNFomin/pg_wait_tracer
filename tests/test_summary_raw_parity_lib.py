#!/usr/bin/env python3
"""test_summary_raw_parity_lib.py — the FALSE-NEGATIVE suite for the
summary-vs-raw parity gate's comparison core (tests/summary_raw_parity_lib.py).

Showing that a gate can go red for the defect it was written to catch proves
the true-positive path, and that has never been the broken one in this repo.
Every defect reviewers keep finding here is a FALSE NEGATIVE: the gate was
unreachable, skipped, or satisfied without checking anything — a conservation
check whose two sides came from the same sum, a history guard that passed
because *some* entry changed, a readiness gate that stalled instead of
failing.

So there is one case per way THIS gate's detection could be made unreachable,
and each asserts a FAILURE, never a skip:

  §1  both sides took the same route (the #408 sabotage control's case)
  §2  the summary side fell back to raw via an ACCOUNTING-VERSION refusal —
      the v6 -> v7 bump planned with the #408/#379 fixes
  §3  the route oracle never took effect (env var ignored, no refusal)
  §4  oracle and stderr marker contradict each other
  §5  empty / vacuous answers on either side
  §6  a field present on one side only (absent is not equal)
  §7  an unparseable field (a list where a number belongs, a non-dict row)
  §8  a missing or non-executable dependency (the 126/127 family)
  §9  an EMPTY expected table approves nothing
  §10 a known divergence that changed magnitude still fails
  §11 a known divergence that disappeared is XPASS, reported, and PASSES
  §12 the comparator detects each defect class on its own merits: a wrong
      number, a wrong row SET, a wrong row ORDER, a wrong top_wait name, and
      a null top_wait that is honest on both sides (which must PASS)
  §13 issue attribution by rule, and --record's refusal to launder an
      UNEXPLAINED divergence into "known"
  §14 the STALE-VERSION arm (a summary file stamped with a superseded
      accounting version must be refused AND recomputed from raw to a
      COMPLETE answer): thirteen ways that arm could be blinded -- no oracle,
      an oracle that leaked the summary path's numbers, four wrong/absent
      preflight markers, a fresh side that also went raw, an undeterminable
      route, empty answers, a dropped field, and two premise-control
      blindings. Each must refuse CLEANLY (ok=False AND a stated reason), and
      correct inputs must still PASS
  §15 every pin in the TRACKED table carries the issue issue_for() assigns,
      and none points at a closed issue -- the label is display-only to
      compare(), so nothing else would notice it drifting

Pure Python: no server, no trace files, milliseconds. Runs in `make check`
(scripts/check.sh) and in the box/CI tier (tests/unit_tests.list), because a
regression in the gate's own logic must not need a 40-minute box run to show
up — the same reasoning as tests/test_demo_rehearsal_lib.py.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summary_raw_parity_lib as P                              # noqa: E402

FAILURES = []
CHECKS = [0]

# ── §14 fixtures: the stale arm's detector cases ────────────────────────────
#
# These live HERE, not in tests/test_summary_raw_parity.py, because `make check`
# runs this file in seconds while that one only runs in run_all.sh's
# synthetic-data tier (box-check / CI Linux). A bypass case that takes a
# 40-minute box run to show up is a bypass case nobody sees, and box slots are
# the scarcest resource in this project. test_summary_raw_parity.py imports
# run_stale_detector_selftest() from here for its live pre-flight, so there is
# ONE table driving both -- not a copy that can drift.

_STALE_MARKER = ("INFO: %d of %d summary file(s) covering this window are not "
                 "summary v%d -- recomputing from raw events")
_STALE_VER = 8
_STALE_FILES = 1


def stale_answer(db_time=200000.0, cpu=40044.0):
    """A field dict shaped like a real one: non-vacuous by P's own rules
    (DB Time, >=3 class rows, >=2 sessions, >=2 queries)."""
    return {
        "time_model/db_time_ms": db_time,
        "time_model/cpu_ms": cpu,
        "time_model/rowset": "CPU@1 | IO@1 | Lock@1",
        "sessions/rowset": "1000 | 1001",
        "queries/order": "100 | 101",
    }


def _stale_call(routes=None, stale=None, summary=None, stderr=None,
                version=_STALE_VER, files=_STALE_FILES, raw_ref="default"):
    err = _STALE_MARKER % (_STALE_FILES, _STALE_FILES, _STALE_VER)
    return P.stale_verdict(
        "selftest",
        {"raw": P.ROUTE_RAW, "summary": P.ROUTE_SUMMARY} if routes is None
        else routes,
        stale_answer() if stale is None else stale,
        stale_answer(db_time=312114.0, cpu=0.0) if summary is None else summary,
        err if stderr is None else stderr, version, files,
        raw_fields=(stale_answer() if raw_ref == "default" else raw_ref))


def stale_detector_cases():
    """(name, call, what a PASS would mean) per way the arm could be blinded.

    One table, so the live pre-flight and §14 cannot disagree about what is
    covered.
    """
    v, n = _STALE_VER, _STALE_FILES
    return [
        ("no raw oracle supplied",
         lambda: _stale_call(raw_ref=None),
         "the arm would fall back to some other answer as its oracle"),
        ("stale answer leaks the SUMMARY path's numbers",
         lambda: _stale_call(stale=stale_answer(db_time=312114.0, cpu=0.0)),
         "a broken version preflight that served the stale file would pass"),
        ("preflight marker absent entirely",
         lambda: _stale_call(stderr=""),
         "a raw answer reached by any other route would count"),
        ("the #403 no-file marker instead of the version one",
         lambda: _stale_call(stderr="INFO: no summary file covers this window "
                                    "-- recomputing from raw events"),
         "#403 (no summary written at all) would read as a version refusal"),
        ("marker names a different file count than we stamped",
         lambda: _stale_call(stderr=_STALE_MARKER % (0, 0, v), files=0),
         "a window covered by zero summary files would count as proof"),
        ("marker names a different version",
         lambda: _stale_call(stderr=_STALE_MARKER % (n, n, v + 5)),
         "a refusal for some other version would be accepted"),
        ("fresh-summary side also took the raw path",
         lambda: _stale_call(routes={"raw": P.ROUTE_RAW,
                                     "summary": P.ROUTE_RAW}),
         "the window was never summary-eligible, so stamping proved nothing"),
        ("a route could not be determined",
         lambda: _stale_call(routes={"raw": P.ROUTE_RAW,
                                     "summary": P.ROUTE_UNKNOWN}),
         "an undeterminable route would be treated as a good one"),
        ("the raw oracle is empty",
         lambda: _stale_call(raw_ref={}),
         "an absent oracle would agree with anything"),
        ("the version-refused answer is empty",
         lambda: _stale_call(stale={}),
         "an empty answer would be 'complete'"),
        ("the version-refused answer dropped a field",
         lambda: _stale_call(stale={k: val for k, val in stale_answer().items()
                                    if k != "time_model/cpu_ms"}),
         "a missing column would read as agreement"),
        ("premise control with both routes NOT raw",
         lambda: P.stale_verdict(
             "selftest", {"raw": P.ROUTE_RAW, "summary": P.ROUTE_SUMMARY},
             stale_answer(), stale_answer(), "", v, n, is_control=True,
             raw_fields=stale_answer()),
         "the 59 s control would not actually be on the raw path"),
        ("premise control comparing nothing",
         lambda: P.stale_verdict(
             "selftest", {"raw": P.ROUTE_RAW, "summary": P.ROUTE_RAW},
             {}, {}, "", v, n, is_control=True, raw_fields=stale_answer()),
         "two empty answers would agree perfectly"),
    ]


def stale_positive_control():
    """Correct inputs must PASS, over a non-zero number of fields.

    Without this, every case above could be satisfied by a stale_verdict that
    refuses unconditionally -- which would refuse the real run too, and
    thirteen green REFUSALs would look like thirteen working checks.
    """
    ok, _lines, findings = _stale_call()
    return ok, len(findings)


def stale_case_refuses_cleanly(call):
    """(ok_to_count, why). A CLEAN refusal is required: ok=False AND a stated
    reason. An exception is NOT good enough even though the live main() turns
    one into a failure -- 'refuses by crashing' is how a guard gets deleted as
    redundant and the crash then turns out to rest on an unrelated detail.
    Counting a raise as a refusal is exactly what let four of these cases stay
    green while their own guard was reverted (see the branch's bypass log)."""
    try:
        ok, lines, _ = call()
    except Exception as exc:                                  # noqa: BLE001
        return False, "raised %r instead of returning a refusal" % (exc,)
    if ok:
        return False, "APPROVED -- the detector is blind"
    if not any("FAIL" in l or any(st in l for st in P.FAIL_STATUSES)
               for l in lines):
        return False, "refused without saying why"
    return True, ""


def run_stale_detector_selftest():
    """(ok, lines) -- the live pre-flight used by test_summary_raw_parity.py
    before it measures a capture. Same table as §14."""
    lines = ["--- stale-arm detector self-test ---"]
    ok = True
    pos_ok, nfields = stale_positive_control()
    if not pos_ok or not nfields:
        lines.append("  FAIL positive control: correct inputs must PASS over "
                     "a non-zero field count (ok=%s, fields=%d); a detector "
                     "that always refuses proves nothing" % (pos_ok, nfields))
        ok = False
    else:
        lines.append("  positive control: PASS over %d fields" % nfields)
    cases = stale_detector_cases()
    for name, call, consequence in cases:
        clean, why = stale_case_refuses_cleanly(call)
        if not clean:
            lines.append("  FAIL detector %s: %s -- %s" % (why, name,
                                                           consequence))
            ok = False
    lines.append("  %d blinding cases, all refused cleanly: %s"
                 % (len(cases), "yes" if ok else "NO"))
    return ok, lines




def check(cond, what):
    CHECKS[0] += 1
    if cond:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        FAILURES.append(what)


# ── Fixtures: the shape of a real pair of answers ───────────────────────────

def answers(db_ms=100000.0, cpu_rows="split", top_wait="Lock:relation",
            qorder=("100", "101"), client_ms=70000.0, extra=None,
            sessions=(1000, 1001)):
    """A plausible four-endpoint answer set, parameterised on exactly the
    things the real paths disagree about."""
    rows = [{"name": "DB Time", "ms": db_ms, "aas": db_ms / 1e5, "indent": 0},
            {"name": "Client", "ms": client_ms, "aas": 0.5, "indent": 1},
            {"name": "IO", "ms": 20000.0, "aas": 0.2, "indent": 1},
            {"name": "Lock", "ms": 10000.0, "aas": 0.1, "indent": 1}]
    if cpu_rows == "split":
        rows += [{"name": "CPU (running)", "ms": 6000.0, "aas": 0.06, "indent": 1},
                 {"name": "CPU (waiting for a core)", "ms": 4000.0,
                  "aas": 0.04, "indent": 1}]
    elif cpu_rows == "merged":
        rows += [{"name": "CPU*", "ms": 10000.0, "aas": 0.1, "indent": 1}]
    tm = {"id": 1, "fidelity": "exact", "db_time_ms": db_ms, "rows": rows,
          "has_measured_cpu": cpu_rows == "split"}
    if extra:
        tm.update(extra)
    aas = {"id": 2, "fidelity": "exact", "bucket_ns": 1_000_000_000,
           "buckets": [{"t": 0, "cpu": 1.0, "io": 0.5} for _ in range(10)],
           "max_aas": 1.5}
    sess = {"id": 3, "fidelity": "exact", "rows": [
        {"pid": p, "db_time_ms": 50000.0, "top_wait": top_wait,
         "top_wait_id": 1} for p in sessions]}
    qs = {"id": 4, "fidelity": "exact", "rows": [
        {"query_id": q, "total_ms": 40000.0 - i, "top_wait": top_wait}
        for i, q in enumerate(qorder)]}
    return {"time_model": tm, "aas": aas, "top_sessions": sess,
            "top_queries": qs}


def fields(**kw):
    return P.extract_all(answers(**kw))


BOTH_ROUTES = {"raw": P.ROUTE_RAW, "summary": P.ROUTE_SUMMARY}
ORACLE_RAW = {"error": "window too large", "code": "window_too_large",
              "max_events": 1}
ORACLE_SUM = {"fidelity": "exact", "rows": [{"name": "DB Time", "ms": 1.0}]}
MARKER_NO_FILE = ("INFO: no summary file covers this window -- recomputing "
                  "from raw events\n")
# src/server.c's other raw-fallback diagnostic, verbatim shape:
MARKER_VERSION = ("INFO: 1 of 1 summary file(s) covering this window are not "
                  "summary v7 -- recomputing from raw events\n")


def main():
    print("=== test_summary_raw_parity_lib ===")
    base = fields()

    # §0 the positive control: identical answers, two routes, a sane table.
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, dict(base), {})
    check(ok, "§0 identical answers on two different routes PASS")

    # §1 both sides raw -> the comparison is vacuous even though every field
    #    agrees. This is the exact false negative the #408 probe hit.
    ok, lines, _ = P.verdict("w", {"raw": P.ROUTE_RAW, "summary": P.ROUTE_RAW},
                             base, dict(base), {})
    check(not ok, "§1 same route on both sides FAILS despite perfect agreement")
    check(any("BOTH sides took the raw path" in l for l in lines),
          "§1 ... and says so")

    # §2 the summary side fell back to raw through an ACCOUNTING-VERSION
    #    refusal (v6 -> v7). classify_route must read that as raw, and the
    #    verdict must name the version refusal.
    r = P.classify_route(ORACLE_RAW, MARKER_VERSION)
    check(r == P.ROUTE_RAW, "§2 a version-refusal fallback classifies as raw")
    ok, lines, _ = P.verdict(
        "w", {"raw": P.ROUTE_RAW, "summary": P.ROUTE_RAW}, base, dict(base), {},
        fallbacks={"raw": P.fallback_reason(MARKER_NO_FILE),
                   "summary": P.fallback_reason(MARKER_VERSION)})
    check(not ok, "§2 a version-refused summary side FAILS the gate")
    check(any("not summary v7" in l for l in lines),
          "§2 ... and the refusal reason is reported")

    # §3 the oracle never took effect: no refusal at all, or max_events != 1
    #    (the env var ignored). Must read 'unknown', never 'summary'.
    check(P.classify_route({"rows": []}, MARKER_NO_FILE) == P.ROUTE_UNKNOWN,
          "§3 raw side answering the oracle reads as unknown, not summary")
    check(P.classify_route({"code": "window_too_large", "max_events": 999},
                           MARKER_NO_FILE) == P.ROUTE_UNKNOWN,
          "§3 an oracle refusal with the wrong max_events reads as unknown")
    check(P.classify_route({}, "") == P.ROUTE_UNKNOWN,
          "§3 an empty oracle response reads as unknown")
    ok, lines, _ = P.verdict("w", {"raw": P.ROUTE_UNKNOWN,
                                   "summary": P.ROUTE_SUMMARY},
                             base, dict(base), {})
    check(not ok, "§3 an undeterminable route FAILS (refuses to compare)")

    # §4 the two route signals contradict each other.
    check(P.classify_route(ORACLE_SUM, MARKER_NO_FILE) == P.ROUTE_UNKNOWN,
          "§4 oracle says summary while stderr shows a raw fallback -> unknown")
    check(P.classify_route(ORACLE_RAW, "") == P.ROUTE_UNKNOWN,
          "§4 oracle says raw with no fallback marker -> unknown")
    # Below the 120 s threshold no marker is printed, so there the oracle
    # decides alone -- but a marker contradicting it still refuses.
    check(P.classify_route(ORACLE_RAW, "", marker_expected=False) == P.ROUTE_RAW,
          "§4 sub-120 s control window: oracle alone may classify raw")
    check(P.classify_route(ORACLE_SUM, MARKER_NO_FILE,
                           marker_expected=False) == P.ROUTE_UNKNOWN,
          "§4 ... but a contradicting marker still refuses")

    # §5 vacuous answers. Two empty answers agree perfectly.
    empty = P.extract_all({"time_model": {"rows": [], "db_time_ms": 0},
                           "aas": {"buckets": [], "bucket_ns": 1},
                           "top_sessions": {"rows": []},
                           "top_queries": {"rows": []}})
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, empty, dict(empty), {})
    check(not ok, "§5 two empty answers FAIL instead of agreeing")
    check(sum(1 for l in lines if "FAIL vacuous" in l) >= 4,
          "§5 ... naming DB Time, class rows, sessions and queries")
    thin = fields(db_ms=500.0)
    ok, _, _ = P.verdict("w", BOTH_ROUTES, thin, dict(thin), {})
    check(not ok, "§5 a sub-threshold DB Time FAILS as vacuous")
    one_sess = fields(sessions=(1000,))
    ok, _, _ = P.verdict("w", BOTH_ROUTES, one_sess, dict(one_sess), {})
    check(not ok, "§5 a single-session answer FAILS as vacuous")

    # §6 absence is not equality.
    raw_extra = fields(extra={"io_worker_busy_pct": 0})
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, raw_extra, base, {})
    check(not ok, "§6 a field present on the raw side only FAILS")
    check(any(P.MISSING in l and "io_worker_busy_pct" in l for l in lines),
          "§6 ... reported as MISSING-FIELD, not as a value difference")
    # A null on BOTH sides is an honest 'unknown' and must PASS; a null on
    # one side only must not.
    both_null = fields(top_wait=None)
    ok, _, _ = P.verdict("w", BOTH_ROUTES, both_null, dict(both_null), {})
    check(ok, "§6 top_wait null on BOTH sides is honest and PASSES")
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, fields(), both_null, {})
    check(not ok, "§6 top_wait null on ONE side FAILS")

    # §7 unparseable fields: a list where a number belongs, a non-dict row.
    bad = dict(base)
    bad["time_model/db_time_ms"] = P.UNPARSEABLE + ":list"
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, bad, dict(base), {})
    check(not ok, "§7 an unparseable field FAILS")
    rowjunk = P.extract_time_model({"rows": ["not-a-row"], "db_time_ms": 1e5})
    check(rowjunk["time_model/rowset"] == P.UNPARSEABLE,
          "§7 a non-dict row makes the row set unparseable, not empty")
    aasjunk = P.extract_aas({"buckets": "nope", "bucket_ns": 1})
    check(aasjunk["aas/integral_ms"] == P.UNPARSEABLE,
          "§7 an unparseable bucket list does not integrate to 0")
    noresp = P.extract_time_model("boom")
    check(any(str(v).startswith(P.UNPARSEABLE) for v in noresp.values()),
          "§7 a non-dict response is unparseable, not absent")

    # §8 dependencies: missing (127) and present-but-not-executable (126).
    probs = P.require_tools(["/definitely/not/here/pgwt-server"])
    check(len(probs) == 1 and "127" in probs[0],
          "§8 a missing dependency is a failure naming exit 127")
    import tempfile
    with tempfile.NamedTemporaryFile(suffix="_notexec") as tf:
        os.chmod(tf.name, 0o644)
        probs = P.require_tools([tf.name])
        check(len(probs) == 1 and "126" in probs[0],
              "§8 a non-executable dependency is a failure naming exit 126")
        ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, dict(base), {},
                                 tool_problems=probs)
        check(not ok and any("FAIL dependency" in l for l in lines),
              "§8 ... and the verdict refuses rather than comparing")

    # §9 an empty expected table approves nothing.
    diverged = fields(db_ms=110000.0)
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, diverged, {})
    check(not ok, "§9 with an EMPTY table a divergence FAILS")
    check(any(P.NEW in l for l in lines), "§9 ... as NEW-DIVERGENCE")

    # §10 a known divergence that changed magnitude is NOT excused.
    tbl = {"time_model/db_time_ms": {"issue": 379, "raw": 100000.0,
                                     "summary": 110000.0},
           "time_model/row/DB Time/ms": {"issue": 379, "raw": 100000.0,
                                         "summary": 110000.0},
           "time_model/row/DB Time/aas": {"issue": 379, "raw": 1.0,
                                          "summary": 1.1}}
    ok, _, _ = P.verdict("w", BOTH_ROUTES, base, diverged, tbl)
    check(ok, "§10 a divergence matching its recorded pair PASSES as known")
    worse = fields(db_ms=120000.0)
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, worse, tbl)
    check(not ok, "§10 the SAME field diverging by a NEW amount FAILS")
    check(any(P.CHANGED in l for l in lines), "§10 ... as CHANGED-DIVERGENCE")

    # §11 a known divergence that disappeared is XPASS and PASSES.
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, dict(base), tbl)
    check(ok, "§11 a recorded divergence that now agrees PASSES")
    check(any(P.XPASS in l and "delete this entry" in l for l in lines),
          "§11 ... reported as xpass with 'delete this entry'")

    # §12 each defect class on its own merits -- not merely today's knowns.
    #     a) a wrong NUMBER in one class row
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, fields(client_ms=71000.0), {})
    check(not ok and any("row/Client/ms" in l for l in lines),
          "§12a a single wrong class-row number FAILS")
    #     b) a wrong row SET (#408's shape) with the CPU group total equal
    merged = fields(cpu_rows="merged")
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, merged, {})
    check(not ok and any("rowset" in l for l in lines),
          "§12b a differing row SET FAILS even when the CPU total matches")
    check(P.extract_time_model(answers(cpu_rows="split")["time_model"])
          ["time_model/cpu_group_ms"] ==
          P.extract_time_model(answers(cpu_rows="merged")["time_model"])
          ["time_model/cpu_group_ms"],
          "§12b ... and cpu_group_ms is comparable across both row shapes")
    #     c) a wrong row ORDER with identical numbers
    reordered = fields(qorder=("101", "100"))
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base, reordered, {})
    check(not ok and any("queries/order" in l for l in lines),
          "§12c a reordered Queries table FAILS with identical totals")
    #     d) a wrong top_wait name (#319's shape)
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, base,
                             fields(top_wait="IO:DataFileRead"), {})
    check(not ok and any("top_wait" in l for l in lines),
          "§12d a wrong top_wait name FAILS")
    #     e) a row beyond a default display limit still participates
    many_raw = fields(sessions=(1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007,
                                1008, 1009, 1010, 1011))
    many_sum = P.extract_all(answers(sessions=(1000, 1001, 1002, 1003, 1004,
                                               1005, 1006, 1007, 1008, 1009,
                                               1010)))
    ok, lines, _ = P.verdict("w", BOTH_ROUTES, many_raw, many_sum, {})
    check(not ok and any("sessions/rowset" in l or "1011" in l for l in lines),
          "§12e a 12th session missing on one side FAILS (no display limit)")
    #     f) float formatting slack is tolerated, 0.01% is not
    nudge = dict(base)
    nudge["time_model/db_time_ms"] = base["time_model/db_time_ms"] + 1e-9
    ok, _, _ = P.verdict("w", BOTH_ROUTES, base, nudge, {})
    check(ok, "§12f a 1e-9 float difference is tolerated")
    nudge["time_model/db_time_ms"] = base["time_model/db_time_ms"] * 1.0001
    ok, _, _ = P.verdict("w", BOTH_ROUTES, base, nudge, {})
    check(not ok, "§12f a 0.01% difference is NOT tolerated")
    #     g) a bool and a number are different answers
    check(not P._same(False, 0), "§12g has_measured_cpu=false != 0")

    # §13 the issue attribution itself, and the refusal that stops `--record`
    #     from laundering a brand-new divergence into "known".
    import test_summary_raw_parity as G
    check(G.issue_for("time_model/row/CPU*/ms", "interior_160s") == 408,
          "§13 a CPU row field is attributed to #408")
    check(G.issue_for("sessions/row/1001/top_wait", "interior_160s") == 319,
          "§13 a top_wait field is attributed to #319")
    check(G.issue_for("time_model/row/Client/ms", "straddle_130s") == 324,
          "§13 a straddle-window divergence is attributed to OPEN #324, "
          "not CLOSED #379 (v8's queries[] carve-out, not a regression)")
    check(G.issue_for("aas/max_aas", "interior_160s") == 410,
          "§13 a divergence this gate found is attributed to #410")
    check(G.issue_for("time_model/row/Client/ms", "interior_160s")
          == "UNEXPLAINED",
          "§13 an interior-window class-row divergence is UNEXPLAINED "
          "(no issue covers it -- interior windows must agree)")
    check(G.unexplained({"w": {"f": {"issue": "UNEXPLAINED"}}}) == [("w", "f")],
          "§13 --record refuses to write an UNEXPLAINED entry")
    check(G.unexplained({"w": {"f": {"issue": 408}}}) == [],
          "§13 ... and does not block an attributed one")
    check(G.load_expected.__doc__ and "EMPTY" in G.load_expected.__doc__,
          "§13 a missing expected table is documented as fail-loud")

    # §14 the STALE-VERSION arm's detectors. One check per way its detection
    #     could be made unreachable, so `make check` names the specific
    #     blinding that regressed instead of just "the self-test failed".
    pos_ok, pos_fields = stale_positive_control()
    check(pos_ok and pos_fields > 0,
          "§14 positive control: correct inputs PASS over %d fields (a "
          "detector that always refuses proves nothing)" % pos_fields)
    for name, call, consequence in stale_detector_cases():
        clean, why = stale_case_refuses_cleanly(call)
        check(clean, "§14 refuses cleanly: %s%s"
              % (name, "" if clean else " -- %s (%s)" % (why, consequence)))
    # And the live pre-flight agrees with the per-case verdicts above, so the
    # thing test_summary_raw_parity.py actually calls is the thing tested.
    pre_ok, _pre_lines = run_stale_detector_selftest()
    check(pre_ok, "§14 the live pre-flight run_stale_detector_selftest() "
                  "passes on a healthy tree")

    # §15 the TRACKED table's issue labels must match issue_for()'s rule.
    #     compare() uses an entry's "issue" for DISPLAY only, so a stale label
    #     is invisible to the gate -- and a label pointing at a CLOSED issue
    #     arms a trap, because this gate's own rule is "delete an entry the
    #     moment its issue is fixed". Eight straddle pins pointed at closed
    #     #379 (v8's documented queries[] carve-out, not a #379 regression);
    #     the next reader would have deleted them and blinded the window.
    #     This check is what stops that label drifting back.
    table = G.load_expected()
    check(bool(table), "§15 the tracked expected-divergence table is present "
                       "and non-empty (an empty one would make §15 vacuous)")
    mismatched, labelled = [], 0
    for w, tbl in sorted(table.items()):
        for f, e in sorted(tbl.items()):
            labelled += 1
            want = G.issue_for(f, w)
            if e.get("issue") != want:
                mismatched.append("%s/%s labelled #%s, rule says #%s"
                                  % (w, f, e.get("issue"), want))
    check(labelled > 0 and not mismatched,
          "§15 all %d pinned entries carry the issue issue_for() assigns%s"
          % (labelled, "" if not mismatched else " -- " + "; ".join(mismatched)))
    check(all(e.get("issue") != 379 for tbl in table.values()
              for e in tbl.values()),
          "§15 no pin points at CLOSED #379 (its fix landed in v8; a pin there "
          "invites deletion of a live pin)")

    # ── §16 bucket_diff: the instrument that says WHICH bucket ─────────
    # #410 pins aas/max_aas as a known divergence and does NOT pin
    # aas/integral_ms, i.e. the two paths agree on AREA and differ on PEAK
    # over the SAME layout (buckets=10 over 160 s = 16 s both sides). That is
    # lumpier per-bucket VALUES, not a different peak rule -- and max_aas, one
    # number per window, cannot say WHERE. bucket_diff() does.
    #
    # Every case below asserts that it REFUSES rather than reporting "no
    # difference", because a per-bucket comparison that cannot see its inputs
    # would silently approve the very question it was added to answer. The
    # last case is the one that matters most: two EMPTY bucket arrays agree
    # with each other perfectly.
    def bkt(*totals, **kw):
        w = kw.pop("width", 16000000000)
        return {"bucket_ns": w,
                "buckets": [{"t": 1000 + i, "IO": v} for i, v in
                            enumerate(totals)]}

    d = P.bucket_diff(bkt(1.0, 2.0, 3.0), bkt(1.0, 2.0, 3.0))
    check(d["status"] == P.BUCKET_OK and d["first_divergent"] is None
          and d["n"] == 3,
          "§16a identical non-zero buckets AGREE and are not refused")

    d = P.bucket_diff(bkt(1.0, 2.0, 3.0), bkt(1.0, 5.0, 0.0))
    check(d["status"] == P.BUCKET_OK and d["first_divergent"] == 1
          and len(d["divergent"]) == 2,
          "§16b the lumpy case names the FIRST divergent bucket (got %s)"
          % (d.get("first_divergent"),))

    # The #410 shape itself: equal area, higher peak. Must be detected, and
    # the area line must show the two integrals matching -- otherwise the
    # instrument would blame a magnitude difference for a placement one.
    d = P.bucket_diff(bkt(2.0, 2.0, 2.0), bkt(0.0, 4.0, 2.0))
    check(d["status"] == P.BUCKET_OK
          and abs(d["raw_integral_ms"] - d["sum_integral_ms"]) < 1e-9
          and d["first_divergent"] == 0,
          "§16c equal AREA with a moved peak is reported as placement "
          "(areas %.6f vs %.6f)"
          % (d.get("raw_integral_ms", -1), d.get("sum_integral_ms", -1)))

    # EACH LEG PINS ITS OWN REASON, not merely "refused". The first cut of
    # this suite asserted only the status, and three of its fixtures carried a
    # raw side with bucket_ns=1 against a 16 s summary side -- so they refused
    # on the WIDTH MISMATCH, never reaching the check they were named after.
    # Two mutants (the empty-bucket refusal deleted, the no-class-keys guard
    # deleted) survived the suite because of it. Pinning the reason is what
    # makes each leg reach its own check: W is the shared width every fixture
    # below uses unless it is deliberately testing the width.
    W = 16000000000
    for label, raw, sm, want in (
        ("absent buckets", {"bucket_ns": W}, bkt(1.0),
         "absent or not a list"),
        ("buckets not a list", {"bucket_ns": W, "buckets": {}}, bkt(1.0),
         "absent or not a list"),
        ("EMPTY buckets on the summary side",
         bkt(1.0), {"bucket_ns": W, "buckets": []}, "EMPTY"),
        ("EMPTY buckets on the raw side",
         {"bucket_ns": W, "buckets": []}, bkt(1.0), "EMPTY"),
        ("EMPTY on the summary side while raw carries LOAD "
         "(must not be mistaken for agreement)",
         bkt(5.0, 5.0), {"bucket_ns": W, "buckets": []}, "EMPTY"),
        ("absent bucket_ns", {"buckets": [{"t": 1, "IO": 1.0}]}, bkt(1.0),
         "bucket_ns"),
        ("zero bucket_ns", bkt(1.0, width=0), bkt(1.0), "bucket_ns"),
        ("non-numeric bucket_ns", bkt(1.0, width="wide"), bkt(1.0),
         "bucket_ns"),
        ("DIFFERENT widths (not the same layout)",
         bkt(1.0, width=W), bkt(1.0, width=W // 2), "DIFFERENT bucket widths"),
        ("a bucket that is not a dict",
         {"bucket_ns": W, "buckets": [[1, 2]]}, bkt(1.0), "unparseable"),
        ("a bucket with no numeric class keys",
         {"bucket_ns": W, "buckets": [{"t": 1}]}, bkt(1.0), "unparseable"),
        ("a bucket whose class value is a list",
         {"bucket_ns": W, "buckets": [{"t": 1, "IO": [1]}]}, bkt(1.0),
         "unparseable"),
        ("an `aas` answer that is not an object", None, bkt(1.0),
         "not an object"),
        ("both sides zero everywhere (vacuous agreement)",
         bkt(0.0, 0.0), bkt(0.0, 0.0), "vacuous"),
    ):
        d = P.bucket_diff(raw, sm)
        got = d.get("reason") or ""
        check(d["status"] == P.BUCKET_REFUSED and want in got,
              "§16d %s REFUSES for its OWN reason (wanted %r, got %r)"
              % (label, want, got[:70]))

    # A length mismatch is a FINDING, not a refusal: both sides were asked
    # for the same layout, so reporting the mismatch IS the answer.
    d = P.bucket_diff(bkt(1.0, 2.0, 3.0), bkt(1.0, 2.0))
    check(d["status"] == P.BUCKET_OK and d.get("length_mismatch") == [3, 2]
          and d["n"] == 2,
          "§16e a LENGTH mismatch is reported as a finding, not refused")

    # The renderer must not invent a clean line out of a refusal.
    lines = P.bucket_diff_lines("w", P.bucket_diff(bkt(0.0), bkt(0.0)))
    check(len(lines) == 1 and "REFUSED" in lines[0],
          "§16f the log rendering of a refusal says REFUSED")
    lines = P.bucket_diff_lines("w", P.bucket_diff(bkt(2.0, 2.0),
                                                   bkt(0.0, 4.0)))
    check(any("bucket 0" in l for l in lines)
          and any("delta" in l for l in lines),
          "§16g the log rendering of a finding names the bucket and the delta")

    # §16i THE PER-CLASS BREAKDOWN -- what turns "bucket 6 gained 2.1875"
    # into "which class moved", and with a synthetic fixture the class plus
    # the bucket names the record. The two properties that make it honest:
    #   - a class present on ONE SIDE ONLY is marked <absent>, never read as
    #     0.0, because a 0 blames a magnitude difference for an absence;
    #   - a divergent bucket that NOTHING in the breakdown accounts for is
    #     refused, not printed as though it had been explained.
    # There is deliberately NO "the deltas sum to the bucket delta" assertion:
    # both walks share P._is_class_key, so that identity holds by
    # construction. The first cut asserted it anyway and no mutant could make
    # it fail -- a guard that cannot fire, which is the very defect this
    # suite exists to catch, so the sharing replaced the assertion.
    def b2(*pairs, **kw):
        w = kw.pop("width", 16000000000)
        return {"bucket_ns": w,
                "buckets": [dict(t=1000 + i, **d) for i, d in
                            enumerate(pairs)]}

    d = P.bucket_diff(b2({"IO": 2.0, "Lock": 1.0}, {"IO": 2.0, "Lock": 1.0}),
                      b2({"IO": 0.5, "Lock": 1.0}, {"IO": 3.5, "Lock": 1.0}))
    bd = d.get("breakdown", {})
    check(d["status"] == P.BUCKET_OK
          and [c[0] for c in bd.get(0, [])] == ["IO"]
          and [c[0] for c in bd.get(1, [])] == ["IO"],
          "§16i the breakdown names the class that moved and not the one that "
          "did not (got %s)"
          % {k: [c[0] for c in v] for k, v in bd.items()})

    # ABSENCE, the Q1 input: IO present on raw, missing entirely on summary.
    d = P.bucket_diff(b2({"IO": 2.0, "Lock": 1.0}, {"IO": 2.0, "Lock": 1.0}),
                      b2({"Lock": 1.0}, {"IO": 4.0, "Lock": 1.0}))
    row = d.get("breakdown", {}).get(0, [[None] * 4])[0]
    check(d["status"] == P.BUCKET_OK and row[0] == "IO"
          and row[2] == P.CLASS_ABSENT and row[1] == 2.0,
          "§16i a class present on ONE SIDE ONLY is marked %r, not 0.0 "
          "(got raw=%r summary=%r)" % (P.CLASS_ABSENT, row[1], row[2]))

    # A bucket with no class keys AT ALL is unparseable, and _bucket_total
    # says so before the breakdown is reached -- that is the right answer and
    # the right place for it.
    d = P.bucket_diff({"bucket_ns": 16000000000,
                       "buckets": [{"t": 1, "IO": 3.0}]},
                      {"bucket_ns": 16000000000, "buckets": [{"t": 1}]})
    check(d["status"] == P.BUCKET_REFUSED
          and "unparseable" in (d.get("reason") or ""),
          "§16i a bucket with no class keys is UNPARSEABLE, refused before "
          "the breakdown (got %r)" % (d.get("reason") or d.get("status")))

    # THE UNATTRIBUTABLE CASE IS REAL AND NEEDS THIS FIXTURE. The first cut of
    # this check used the bucket above and passed on the `unparseable` refusal
    # instead, leaving the attribution leg untested and looking covered. The
    # leg is reached when the bucket's TOTAL differs by more than tol while
    # every INDIVIDUAL class delta is within tol -- many tiny per-class
    # differences that add up. Four classes off by exactly tol each: the
    # breakdown skips all four, the total is 4x tol, and a divergence with an
    # empty breakdown must refuse rather than print a cause it does not have.
    # tol=1.0 makes the arithmetic unambiguous: each class moves by exactly
    # 1.0 (skipped, not > tol), four of them, so the bucket total moves 4.0
    # (> tol, hence divergent) with an empty breakdown.
    d = P.bucket_diff(
        {"bucket_ns": 16000000000,
         "buckets": [{"t": 1, "A": 1.0, "B": 1.0, "C": 1.0, "D": 1.0}]},
        {"bucket_ns": 16000000000,
         "buckets": [{"t": 1, "A": 2.0, "B": 2.0, "C": 2.0, "D": 2.0}]},
        tol=1.0)
    check(d["status"] == P.BUCKET_REFUSED
          and "cannot be attributed" in (d.get("reason") or ""),
          "§16i a divergent bucket whose per-class deltas are each within tol "
          "REFUSES rather than reporting an unattributed divergence (got %r)"
          % (d.get("reason") or d.get("status")))

    d = P.bucket_diff({"bucket_ns": 16000000000,
                       "buckets": [{"t": 1, "IO": "lots"}]},
                      b2({"IO": 1.0}))
    check(d["status"] == P.BUCKET_REFUSED,
          "§16i a non-numeric class value REFUSES")

    # Determinism: largest |delta| first, name as tiebreak, so two runs of the
    # gate produce byte-identical logs and a diff of two box runs is readable.
    d = P.bucket_diff(b2({"AAA": 1.0, "ZZZ": 1.0, "MMM": 1.0}),
                      b2({"AAA": 2.0, "ZZZ": 2.0, "MMM": 5.0}))
    order = [c[0] for c in d.get("breakdown", {}).get(0, [])]
    check(order == ["MMM", "AAA", "ZZZ"],
          "§16i the breakdown is ordered by |delta| then name (got %s)"
          % order)

    lines = P.bucket_diff_lines("w", d)
    check(any("MMM" in l and "delta=+4.0" in l for l in lines),
          "§16i the breakdown REACHES the log under its bucket")

    # §16h THE WIRING, not the library. §16a-g all passed while the live
    # gate emitted NOTHING: the first cut collected bucket lines into a
    # `bucket_lines` buffer and appended it AFTER `for l in lines: print(l)`,
    # so a complete box run computed the measurement and printed none of it
    # -- and tests/results/summary_raw_parity/summary.json is not synced back
    # from the box, so the JSON channel was blind in the same run. A library
    # test cannot see that. This does, by pinning the two structural
    # properties that make the defect impossible:
    #   1. the ONLY call site feeds `lines` directly (no deferred buffer to
    #      forget to flush);
    #   2. it sits BEFORE the loop that prints `lines`.
    # It REFUSES rather than passing when it cannot find the gate file or the
    # print loop -- a wiring check that locates neither approves every
    # miswiring there will ever be.
    gate_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "test_summary_raw_parity.py")
    if not os.path.exists(gate_path):
        check(False, "§16h REFUSE: cannot find %s -- nothing to check the "
                     "wiring of" % gate_path)
    else:
        src = open(gate_path).read().splitlines()
        emit = [i for i, l in enumerate(src)
                if "lines.extend(P.bucket_diff_lines(" in l]
        anycall = [i for i, l in enumerate(src)
                   if "P.bucket_diff_lines(" in l]
        printloop = [i for i, l in enumerate(src)
                     if l.strip() == "for l in lines:"]
        check(len(printloop) == 1,
              "§16h REFUSE unless exactly one `for l in lines:` print loop is "
              "locatable (found %d)" % len(printloop))
        check(len(anycall) == 1 and len(emit) == 1,
              "§16h the ONE bucket_diff_lines call site feeds `lines` "
              "directly (calls=%d, feeding-lines=%d) -- a deferred buffer is "
              "what made a whole box run emit nothing"
              % (len(anycall), len(emit)))
        check(bool(emit) and bool(printloop) and emit[0] < printloop[0],
              "§16h the emission (line %s) comes BEFORE the print loop "
              "(line %s)"
              % (emit[0] + 1 if emit else None,
                 printloop[0] + 1 if printloop else None))
        check(not any("bucket_lines" in l and not l.lstrip().startswith("#")
                      for l in src),
              "§16h no `bucket_lines` buffer survives in the gate (the "
              "identifier appears only in the comment explaining why)")

    # §16j THE GENERATOR'S COUNTER CHANNEL. gen_test_traces now prints the
    # writer's open-interval counters (SUMMARY-OPEN-COUNTERS:, #410) so the
    # 36 s of unspread `io` wall can be attributed to a named counter rather
    # than guessed at. That dump goes to STDERR, and server_harness's
    # generate_traces() runs the binary with capture_output=True and surfaces
    # stderr ONLY on a non-zero exit -- so on every successful run, which is
    # every run that matters, a bare fprintf would be discarded. That is the
    # same "computed, never emitted" failure that made a whole box-check
    # produce no measurement at 3f0ddde6, about to be repeated one layer
    # down. So the stderr is retained to a file and read back, and these
    # cases pin the half that can go quiet.
    import server_harness as H
    scratch16 = tempfile.mkdtemp()
    try:
        # 1. the happy path
        with open(os.path.join(scratch16, H.GEN_STDERR_NAME), "w") as f:
            f.write("Wrote 3 events\nSUMMARY-OPEN-COUNTERS: credited_ns=7 "
                    "late_ns=0 expired=0 mismatch=0 fill_capped=0 "
                    "cpu_refused=0 cpu_uncredited_ns=0\n")
        got = H.gen_stderr_lines(scratch16, "SUMMARY-OPEN-COUNTERS:")
        check(len(got) == 1 and "credited_ns=7" in got[0],
              "§16j the retained counter line is read back (got %r)" % got)

        # 2. THE CHANNEL BROKEN: no file at all. Must name the problem, never
        #    return an empty list -- empty reads as "the generator had nothing
        #    to say", which is indistinguishable from "this is blind again".
        got = H.gen_stderr_lines(os.path.join(scratch16, "nope"),
                                 "SUMMARY-OPEN-COUNTERS:")
        check(len(got) == 1 and got[0].startswith("MISSING"),
              "§16j a missing stderr file NAMES the problem instead of "
              "returning nothing (got %r)" % got)

        # 3. file present, dump absent (prefix changed, or it never ran)
        with open(os.path.join(scratch16, H.GEN_STDERR_NAME), "w") as f:
            f.write("Wrote 3 events + 0 samples to /tmp/x\n")
        got = H.gen_stderr_lines(scratch16, "SUMMARY-OPEN-COUNTERS:")
        check(len(got) == 1 and got[0].startswith("NO "),
              "§16j stderr retained but the dump ABSENT names that distinctly "
              "(got %r)" % got)

        # 4. an empty stderr file is the same kind of silence
        open(os.path.join(scratch16, H.GEN_STDERR_NAME), "w").close()
        got = H.gen_stderr_lines(scratch16, "SUMMARY-OPEN-COUNTERS:")
        check(len(got) == 1 and got[0].startswith("NO "),
              "§16j an EMPTY retained stderr names the problem (got %r)" % got)

        # 6. THE RETENTION HALF, behaviourally. Cases 1-5 all write the file
        # by hand and only exercise the READER, so the mutant that matters
        # most -- retaining stderr only when the exit code is non-zero, which
        # IS the original trap -- sailed straight through them (measured: 108
        # checks, 0 failed, G1 survived). generate_traces() is therefore
        # driven here with subprocess stubbed, on a SUCCESSFUL exit, because
        # success is the only case that was ever broken.
        class _Res:
            def __init__(self, rc):
                self.returncode = rc
                self.stdout = ""
                self.stderr = ("Wrote 1 events + 0 samples to /tmp/x\n"
                               "SUMMARY-OPEN-COUNTERS: credited_ns=42 "
                               "late_ns=1 expired=2 mismatch=3 fill_capped=4 "
                               "cpu_refused=5 cpu_uncredited_ns=6\n")
        real_run, real_bin = H.subprocess.run, H.GEN_BIN
        for rc, label in ((0, "exit 0"), (1, "exit 1")):
            outdir = tempfile.mkdtemp()
            try:
                H.subprocess.run = lambda *a, **k: _Res(rc)
                H.GEN_BIN = "/bin/sh"     # must merely exist
                try:
                    H.generate_traces({"events": []}, output_dir=outdir)
                except RuntimeError:
                    pass                  # expected for rc != 0
                got = H.gen_stderr_lines(outdir, "SUMMARY-OPEN-COUNTERS:")
                check(len(got) == 1 and "credited_ns=42" in got[0],
                      "§16j generate_traces RETAINS the counter line on %s "
                      "(got %r)" % (label, got[0][:48]))
            finally:
                H.subprocess.run, H.GEN_BIN = real_run, real_bin
                shutil.rmtree(outdir, ignore_errors=True)
        # 5. a None/'' trace dir must not raise -- it is the shape a failed
        #    generation leaves behind
        got = H.gen_stderr_lines(None, "SUMMARY-OPEN-COUNTERS:")
        check(len(got) == 1 and got[0].startswith("MISSING"),
              "§16j a None trace dir REFUSES in words, without raising")
    finally:
        shutil.rmtree(scratch16, ignore_errors=True)

    # And the wiring, same reasoning as §16h: read into the printed `lines`,
    # before the print loop, with no deferred buffer.
    if os.path.exists(gate_path):
        src = open(gate_path).read().splitlines()
        # Match the ASSIGNMENT, not the bare name: the comment above the call
        # site names the function too, so "gen_stderr_lines(" alone counts
        # two and the check fails for the wrong reason (it did, first run).
        rd = [i for i, l in enumerate(src)
              if l.strip().startswith("oc = gen_stderr_lines(")]
        pl = [i for i, l in enumerate(src) if l.strip() == "for l in lines:"]
        ext = [i for i, l in enumerate(src)
               if "lines.extend(\"  \" + l for l in oc)" in l]
        check(len(rd) == 1 and len(pl) == 1 and len(ext) == 1
              and rd[0] < pl[0] and ext[0] < pl[0],
              "§16j the counter read (line %s) and its emission (line %s) "
              "both precede the print loop (line %s)"
              % (rd[0] + 1 if rd else None, ext[0] + 1 if ext else None,
                 pl[0] + 1 if pl else None))

    print("--- %d checks, %d failed ---" % (CHECKS[0], len(FAILURES)))
    for f in FAILURES:
        print("  failed: %s" % f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
