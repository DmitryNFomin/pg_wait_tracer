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
import sys

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
    check(G.issue_for("time_model/row/Client/ms", "straddle_130s")
          == "UNEXPLAINED",
          "§13 #324 cannot produce a divergence in this unfiltered window; "
          "an unexplained one must REFUSE via unexplained()/--record, "
          "not inherit #324's inapplicable mechanism")
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

    print("--- %d checks, %d failed ---" % (CHECKS[0], len(FAILURES)))
    for f in FAILURES:
        print("  failed: %s" % f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
