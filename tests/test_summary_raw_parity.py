#!/usr/bin/env python3
"""test_summary_raw_parity.py — THE SUMMARY-vs-RAW PARITY GATE.

The same capture, the same unfiltered window, answered BOTH ways, asserted to
agree. See tests/summary_raw_parity_lib.py's header for what is compared and
for every refuse-rather-than-approve rule; this file is the live half.

HOW THE RAW REFERENCE IS OBTAINED, AND WHY THIS WAY
─────────────────────────────────────────────────────────────────────────────
There is no supported way to force the raw path for an unfiltered window >=
120 s: `should_use_summaries` (src/server.c) routes it to summaries. #408 got
its raw reference by rebuilding with the 120 s constant raised to 86400 s.
This gate does NOT do that, and deliberately:

  * a second build is a DIFFERENT BINARY. Then every divergence has two
    candidate explanations — the route, or the patch — and the gate can only
    be trusted as far as a human trusts that one-line edit. It also cannot run
    anywhere a compiler and the exact same flags are not available, which is
    most of where this suite runs.
  * a test-only hook in src/server.c would be product code added for a test,
    on a branch whose contract is "tests only"; and an env-var route override
    is itself untested behaviour in the thing being measured.

Instead: ONE binary, ONE generated capture, and the route is chosen by the
PRODUCT's OWN supported fallback. The capture is copied; the copy's summary
files are deleted. src/server.c then refuses the summary path for that window
("no summary file covers this window -- recomputing from raw events", the
#402/#403 fix) and answers from raw events. The two dirs hold byte-identical
trace files, so the raw side's inputs are exactly the summary side's inputs.

That premise is not assumed — it is asserted, by the PREMISE CONTROL below:
at a 59 s window BOTH dirs take the raw path, and their answers must be
IDENTICAL on every compared field. If deleting the summary files changed the
raw answer in any way, that control goes red.

PROOF THAT BOTH PATHS ARE ACTUALLY EXERCISED
─────────────────────────────────────────────────────────────────────────────
"Two raw answers agreeing" is the exact false negative a sabotage control
caught on #408, and it is about to get easier: the v6 -> v7 summary format
bump planned with the #408/#379 fixes makes the server REFUSE old summary
files and recompute from raw, silently. So each side's route is MEASURED
(classify_route): an oracle re-run with PGWT_LOAD_MAX_EVENTS=1 (raw must
refuse with window_too_large, summaries answer anyway) cross-checked against
the server's own raw-fallback stderr markers. Same route on both sides, or an
undeterminable route, FAILS the gate before any number is compared. Run with
--sabotage to see that happen: both sides are pointed at the summary-less dir,
every number then agrees perfectly, and the gate still goes red.

WHERE THIS RUNS
─────────────────────────────────────────────────────────────────────────────
tests/run_all.sh's synthetic-data tier (step 2.5, needs pgwt-server +
gen_test_traces), i.e. every `make box-check` and the CI Linux job — not
tests/unit_tests.list, which is C unit binaries plus pure-Python modules and
would have no server to query. The pure comparison core is separately gated in
`make check` via tests/test_summary_raw_parity_lib.py.
"""

import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (                                    # noqa: E402
    ServerHarness, generate_traces, cleanup_traces,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE,
    make_event_id, CLASS_CLIENT, CLASS_LOCK, SERVER_BIN, GEN_BIN,
)
import summary_raw_parity_lib as P                              # noqa: E402

S = 1_000_000_000
MS = 1_000_000
BASE = 10_000_000_000_000          # second-aligned synthetic mono/wall base
CLIENT_READ = make_event_id(CLASS_CLIENT, 1)
LOCK_REL = make_event_id(CLASS_LOCK, 0)

RESULT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "results", "summary_raw_parity")

# Seconds (relative to BASE) no ordinary record may overlap, so that every
# window edge below falls in a data hole. Without this, every window edge
# creates its own #379 straddle and the per-window expectations would be
# dominated by fixture artefacts instead of the defects under test. The one
# deliberate straddler (pid 1003) is exempt — it IS the #379 probe.
QUIET = ((58, 63), (138, 144), (178, 183), (188, 193), (298, 304))


def _quiet(start_s, end_s):
    return any(start_s < qe and end_s > qs for qs, qe in QUIET)


def build_capture():
    """A synthetic EXACT capture: no samples, so every window below is
    EXACT-covered and therefore eligible for the summary route."""
    ev = []

    # pid 1000 — a 3 s CPU/IO/LWLock cycle with MEASURED cpu_ns, so the raw
    # path can split "CPU (running)" from "CPU (waiting for a core)" (#408).
    sec = 5
    while sec < 350:
        for old, new, qid, cpu in (
                (CPU, IO_DATA_FILE_READ, 100, 600 * MS),
                (IO_DATA_FILE_READ, LWLOCK_WAL_WRITE, 100, 0),
                (LWLOCK_WAL_WRITE, CPU, 101, 0)):
            if not _quiet(sec, sec + 1):
                ev.append({"pid": 1000, "ts": BASE + (sec + 1) * S, "dur": S,
                           "old": old, "new": new, "qid": qid, "cpu": cpu})
            sec += 1

    # pid 1001 — many small Lock waits (the largest per-event TOTAL) plus one
    # long single IO wait fully inside the interior windows (the longest
    # SINGLE wait). The two definitions of `top_wait` disagree here: #319.
    for sec in range(20, 346):
        if _quiet(sec, sec + 1):
            continue
        ev.append({"pid": 1001, "ts": BASE + sec * S + 500 * MS, "dur": 500 * MS,
                   "old": LOCK_REL, "new": CPU, "qid": 102, "cpu": 0})
        ev.append({"pid": 1001, "ts": BASE + sec * S + 501 * MS, "dur": MS,
                   "old": CPU, "new": LOCK_REL, "qid": 102, "cpu": MS})
    ev.append({"pid": 1001, "ts": BASE + 240 * S, "dur": 40 * S,
               "old": IO_DATA_FILE_READ, "new": CPU, "qid": 102, "cpu": 0})

    # pid 1002 — a client session alternating idle and CPU.
    for sec in range(10, 340, 10):
        if _quiet(sec, sec + 4):
            continue
        ev.append({"pid": 1002, "ts": BASE + (sec + 3) * S, "dur": 3 * S,
                   "old": CLIENT_READ, "new": CPU, "qid": 0, "cpu": 0})
        ev.append({"pid": 1002, "ts": BASE + (sec + 4) * S, "dur": S,
                   "old": CPU, "new": CLIENT_READ, "qid": 103, "cpu": 900 * MS})

    # pid 1003 — THE STRADDLER: one 100 s Client:ClientRead closing at t0+100.
    # The summary writer attributes its whole duration to the second it closes
    # in; the raw path clips it to the window (#379). Only the 'straddle'
    # window below contains that closing second without containing the start.
    ev.append({"pid": 1003, "ts": BASE + 100 * S, "dur": 100 * S,
               "old": CLIENT_READ, "new": CPU, "qid": 0, "cpu": 0})
    ev.append({"pid": 1003, "ts": BASE + 101 * S, "dur": S,
               "old": CPU, "new": CLIENT_READ, "qid": 104, "cpu": 500 * MS})

    return {
        "cpu_measured": 1,
        "interleave": 1,
        "backends": [{"pid": p, "type": "client", "user": "u", "db": "d"}
                     for p in (1000, 1001, 1002, 1003)],
        "queries": [{"id": q, "text": "q%d" % q}
                    for q in (100, 101, 102, 103, 104)],
        "events": sorted(ev, key=lambda e: e["ts"]),
    }


# Window shapes. #379's error depends on where the edge falls relative to a
# long record's closing second, so one shape can miss it entirely (adviser
# review, 2026-10-14): an interior window, one ending at capture close, one
# shifted by a single second, one on fractional-second boundaries, and the
# straddle probe.
WINDOWS = {
    "interior_160s":      (BASE + 140 * S,            BASE + 300 * S),
    "to_capture_close":   (BASE + 180 * S,            BASE + 351 * S),
    "shift_1s":           (BASE + 141 * S,            BASE + 301 * S),
    "fractional_bounds":  (BASE + 140 * S + 500 * MS, BASE + 300 * S + 500 * MS),
    "straddle_130s":      (BASE + 60 * S,             BASE + 190 * S),
}

# The PREMISE CONTROL: below 120 s both dirs take the raw path, so the two
# answers must be bit-identical. This is what proves that deleting the summary
# files does not change the raw answer — the one assumption the whole method
# rests on.
CONTROL_WINDOW = ("control_59s_both_raw",
                  (BASE + 200 * S, BASE + 259 * S))

CMDS = ("time_model", "aas", "top_sessions", "top_queries")


def ask(trace_dir, frm, to, stderr_path):
    """Both the four answers and the route evidence for one (dir, window)."""
    answers = {}
    with ServerHarness(trace_dir, stderr_path=stderr_path) as srv:
        for cmd in CMDS:
            answers[cmd] = srv.query(cmd, from_=frm, to_=to, buckets=10,
                                     limit=100)
    # Route oracle: same binary, same dir, same window, one event allowed.
    oracle_err = stderr_path + ".oracle"
    with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1},
                       stderr_path=oracle_err) as srv:
        oracle = srv.query("time_model", from_=frm, to_=to)
    with open(stderr_path) as f:
        err = f.read()
    return answers, oracle, err


# ── Expected divergences ───────────────────────────────────────────────────
#
# Each entry: the field, the OPEN ISSUE it belongs to, and the exact pair
# measured on master (c27bf7d) with this fixture. Regenerate with --record
# ONLY after a deliberate fix or fixture change, and delete an entry the
# moment its issue is fixed — a fixed field shows up as XPASS, which is
# reported and does not fail.
#
#   #408 — the summary path reports has_measured_cpu=false, so it emits one
#          `CPU*` row where raw emits `CPU (running)` + `CPU (waiting for a
#          core)`, and cpu_ms/offcpu_ms read 0.
#   #379 — the writer attributes a record's whole duration to the second it
#          closes in; raw clips it to the window.
#   #319 — `top_wait` means "largest total" on raw and "longest single wait"
#          on the summary path.
#   UNFILED:* — a divergence this gate found that no open issue covers. It is
#          recorded so the gate is green on master, named so it is visible,
#          and listed in the branch report for an issue number.
# PGWT_PARITY_EXPECTED points the gate at a different expected-divergence
# table. It exists for this gate's OWN red demonstrations (an entry with a
# perturbed magnitude must report CHANGED-DIVERGENCE; an empty table must make
# every divergence fail) -- never to relax a run.
EXPECTED_PATH = os.environ.get(
    "PGWT_PARITY_EXPECTED",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "summary_raw_parity_expected.json"))


def load_expected():
    """The table is a tracked JSON file, not a dict in this module, so that a
    regeneration (`--record`) is a one-file, reviewable diff -- and so that an
    EMPTY or MISSING table makes every divergence fail loudly rather than
    quietly approving (a missing table must never read as "nothing is known to
    diverge, so nothing can")."""
    if not os.path.exists(EXPECTED_PATH):
        return {}
    with open(EXPECTED_PATH) as f:
        return json.load(f)


# Divergences this gate FOUND that no open issue covers. Named and recorded
# so the gate is green on master, visible so they are not mistaken for
# accepted behaviour, and reported in the branch report for issue numbers.
UNFILED = {
    "aas/max_aas":
        "UNFILED-max_aas-definition: the summary path's peak is the max over "
        "SECONDS, the raw path's is the max over the requested buckets",
    "queries/backfilled_ms":
        "UNFILED-no-unattributed-accounting: the summary path reports no "
        "backfilled time",
    "queries/unattributed_available":
        "UNFILED-no-unattributed-accounting: the summary path cannot offer "
        "unattributed time at all",
    "time_model/io_worker_busy_pct":
        "UNFILED-io_worker_busy_pct-absent: the summary path omits the "
        "io_worker busy diagnostic",
}

CPU_SCALARS = ("time_model/has_measured_cpu", "time_model/cpu_ms",
               "time_model/offcpu_ms", "time_model/cpu_group_ms",
               "time_model/rowset")


def issue_for(field, window):
    """Which open issue owns this divergence.

    Attribution is by rule rather than by hand so that a NEW field appearing
    in a window cannot inherit an issue number just by sitting next to one.
    """
    if field in UNFILED:
        return UNFILED[field]
    if field.endswith("/top_wait") or field.endswith("/top_wait_id"):
        return 319                      # two definitions of one column
    if "CPU" in field or field in CPU_SCALARS:
        return 408                      # has_measured_cpu=false on summaries
    if window == "straddle_130s":
        # The quiet zones guarantee the ONLY record straddling this window's
        # edges is the deliberate 100 s Client:ClientRead, so every remaining
        # divergence here is the whole-record-into-closing-second mechanism.
        return 379
    return "UNEXPLAINED"


def unexplained(recorded):
    """Entries a regeneration could not attribute to anything.

    --record refuses to write a table containing these. Otherwise the one
    command that exists to record a deliberate change would be the easiest way
    to launder a brand-new divergence into "known": it would land in the table
    with the word UNEXPLAINED next to it and the gate would go green.
    """
    return [(w, f) for w, tbl in recorded.items() for f, e in tbl.items()
            if e.get("issue") == "UNEXPLAINED"]


def summary_format_version():
    """The writer's accounting version, recorded in the artifact so a reader
    can tell WHICH format the summary side actually participated with."""
    hdr = os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "src", "summary_writer.h")
    try:
        with open(hdr) as f:
            for line in f:
                if "define PGWT_SUMMARY_VERSION" in line:
                    return line.split()[-1]
    except OSError:
        pass
    return "unknown"


def main():
    record = "--record" in sys.argv
    sabotage = "--sabotage" in sys.argv

    tool_problems = P.require_tools([SERVER_BIN, GEN_BIN])
    if tool_problems:
        for p in tool_problems:
            print("FAIL dependency: %s" % p)
        print("=== test_summary_raw_parity: FAIL (the gate could not run; "
              "that is a failure, not a skip) ===")
        return 1

    os.makedirs(RESULT_DIR, exist_ok=True)
    sum_dir = generate_traces(build_capture())
    raw_dir = sum_dir + "_rawref"
    shutil.copytree(sum_dir, raw_dir)
    removed = sorted(glob.glob(os.path.join(raw_dir, "*.summary*")))
    for f in removed:
        os.remove(f)
    if not removed:
        print("FAIL: the generated capture had no summary file to remove, so "
              "there is no summary path to compare against")
        return 1
    if sabotage:
        # The control that proves this gate is not satisfiable by two raw
        # answers: point the 'summary' side at the summary-less dir too.
        sum_dir_used = raw_dir
    else:
        sum_dir_used = sum_dir

    ok = True
    report = {"commit_fixture": "synthetic", "sabotage": sabotage,
              "summary_format_version": summary_format_version(),
              "summary_files_present": sorted(
                  os.path.basename(p) for p in glob.glob(
                      os.path.join(sum_dir_used, "*.summary*"))),
              "windows": {}}
    expected = load_expected()
    recorded = {}
    lines = []
    try:
        for name, (frm, to) in list(WINDOWS.items()) + [CONTROL_WINDOW]:
            is_control = (name == CONTROL_WINDOW[0])
            r_ans, r_oracle, r_err = ask(
                raw_dir, frm, to, os.path.join(RESULT_DIR, "err_%s_raw" % name))
            s_ans, s_oracle, s_err = ask(
                sum_dir_used, frm, to,
                os.path.join(RESULT_DIR, "err_%s_sum" % name))
            # Below the 120 s threshold the raw path is taken on length alone
            # and no fallback line is printed, so the control window's routes
            # are classified by the oracle alone (see classify_route).
            routes = {"raw": P.classify_route(r_oracle, r_err,
                                              marker_expected=not is_control),
                      "summary": P.classify_route(s_oracle, s_err,
                                                  marker_expected=not is_control)}
            fallbacks = {"raw": P.fallback_reason(r_err),
                         "summary": P.fallback_reason(s_err)}
            r_fields = P.extract_all(r_ans)
            s_fields = P.extract_all(s_ans)

            if is_control:
                # Both sides raw: every field must agree exactly, and the
                # route check is inverted (same route is REQUIRED here).
                w_ok, w_lines, findings = True, ["--- window %s ---" % name], []
                w_lines.append("  routes: raw-side=%s summary-side=%s"
                               % (routes["raw"], routes["summary"]))
                if routes["raw"] != P.ROUTE_RAW or routes["summary"] != P.ROUTE_RAW:
                    w_lines.append("  FAIL premise control: a 59 s window must "
                                   "take the raw path on BOTH dirs")
                    w_ok = False
                findings = P.compare(r_fields, s_fields, {})
                fields_ok = True
                for f in findings:
                    if f.status != P.AGREE:
                        w_lines.append(f.line())
                        fields_ok = False
                w_ok = w_ok and fields_ok
                if not P.vacuity_problems(r_fields, "raw") == []:
                    w_lines.append("  FAIL premise control: the control "
                                   "window is vacuous: %s"
                                   % P.vacuity_problems(r_fields, "raw"))
                    w_ok = False
                w_lines.append("  %d fields compared; deleting the summary "
                               "files %s change the raw answer"
                               % (len(findings),
                                  "does NOT" if fields_ok else "DOES"))
            else:
                w_ok, w_lines, findings = P.verdict(
                    name, routes, r_fields, s_fields,
                    expected.get(name, {}), fallbacks=fallbacks)
            ok = ok and w_ok
            lines.extend(w_lines)
            report["windows"][name] = {
                "from_ns": frm, "to_ns": to, "ok": w_ok, "routes": routes,
                "fallbacks": fallbacks,
                "findings": [f.as_dict() for f in findings
                             if f.status != P.AGREE],
                "fields_compared": len(findings),
            }
            if not is_control:
                tbl = P.record_table(findings)
                for field, entry in tbl.items():
                    entry["issue"] = issue_for(field, name)
                recorded[name] = tbl
    finally:
        cleanup_traces(sum_dir)
        cleanup_traces(raw_dir)

    print("=== test_summary_raw_parity ===")
    for l in lines:
        print(l)
    report["ok"] = ok
    with open(os.path.join(RESULT_DIR, "summary.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    print("artifact: %s" % os.path.join(RESULT_DIR, "summary.json"))

    if record:
        orphans = unexplained(recorded)
        if orphans:
            for w, field in orphans:
                print("REFUSING to record %s/%s: no issue number and no rule "
                      "explains it" % (w, field))
            return 1
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "summary_raw_parity_expected.json"), "w") as f:
            json.dump(recorded, f, indent=1, sort_keys=True)
        print("RECORDED expected-divergence table "
              "(tests/summary_raw_parity_expected.json)")
        return 0

    print("=== test_summary_raw_parity: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
