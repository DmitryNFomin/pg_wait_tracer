#!/usr/bin/env python3
"""test_summary_raw_parity.py — THE SUMMARY-vs-RAW PARITY GATE.

The same capture, the same unfiltered window, answered through fresh-summary,
missing-summary raw, and stale-version raw arms. See the library for what is
compared and for every refuse-rather-than-approve rule; this is the live half.

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

A third copy retains its summaries but stamps every file header to
PGWT_SUMMARY_VERSION - 1, following test_open_cpu_accounting.c's set_version.
Its long-window route must be raw, with the ACCOUNTING-VERSION PREFLIGHT INFO
marker (never the no-file marker), and EVERY compared field must equal the
SUMMARY-LESS RAW REFERENCE -- exactly, with no expected-divergence table and
no holes. The version refusal puts this arm on the raw path, so the raw answer
is the one it owes; comparing it against the fresh SUMMARY answer instead would
re-ask arm one's question, inherit arm one's open #410/straddle divergences as
NEW ones, and -- the part that matters -- PASS a future bug that made the
refusal leak the summary path's numbers. The fresh-summary answer is still
required to route to summaries, which is what proves this window was
summary-eligible and therefore that the stamping is what forced the fallback.

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
--sabotage-stale-fallback injects an incomplete numeric answer after the REAL
stale fallback and oracle have run: it zeroes only that answer's measured CPU
scalar. Routes, preflight markers, rows and DB Time stay intact, so the new
arm must reject it on numeric completeness rather than refusal or vacuity.

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
import re
import shutil
import signal
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (                                    # noqa: E402
    ServerHarness, generate_traces, cleanup_traces,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE,
    make_event_id, CLASS_CLIENT, CLASS_LOCK, SERVER_BIN, GEN_BIN,
)
import summary_raw_parity_lib as P                              # noqa: E402
# The stale arm's detector self-test lives in the FALSE-NEGATIVE suite, which
# `make check` runs in seconds; this file only runs in run_all.sh's
# synthetic-data tier. One table drives both, so they cannot drift.
from test_summary_raw_parity_lib import (                       # noqa: E402
    run_stale_detector_selftest,
)

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
    # Time Model-only route oracle for this dir/window; one event allowed.
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
#   #379 — FIXED and CLOSED (v8, dd93d523): the writer used to attribute a
#          record's whole duration to the second it closed in while raw clips
#          it to the window. No pin should point here any more.
#   #324 — v8's deliberate residual of the above: only the DURATION is
#          spread, so queries[] still carries a straddling record's full
#          duration at its closing second. OPEN; its queries/* pins are gone
#          because handle_top_queries now computes from raw. No pin points here.
#   #319 — `top_wait` means "largest total" on raw and "longest single wait"
#          on the summary path.
#   #410 — the four divergences THIS gate found on its first run: max_aas
#          (peak over seconds vs over buckets), backfilled_ms and
#          unattributed_available (no unattributed accounting on the summary
#          path) and io_worker_busy_pct (absent there).
#
# DELETING AN ENTRY IS PART OF LANDING THE FIX. When #408, #324, #319 or #410
# is fixed, its pinned fields start agreeing and this gate reports them as
# `xpass ... delete this entry` and still passes. Removing those lines from
# tests/summary_raw_parity_expected.json is the last step of that fix, not an
# edit to be nervous about: leaving a stale entry behind only costs the gate
# its grip on that field.
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


# Divergences this gate FOUND on its first run, filed together as #410:
#   * aas/max_aas — the summary path's peak is the max over SECONDS, the raw
#     path's is the max over the requested buckets. The same
#     two-meanings-one-field shape as #319.
#   * time_model/io_worker_busy_pct — absent from the summary response.
# Listed here rather than falling through to the per-window rules so that a
# NEW divergence in one of these endpoints cannot inherit #410 by accident.
#
# queries/backfilled_ms and queries/unattributed_available WERE here and are
# gone: #598 fixed them, this gate reported them `xpass ... delete this
# entry`, and removing the pins was the last step of landing that fix. They
# are deliberately NOT left behind "just in case": a field with no rule falls
# through to UNEXPLAINED, which refuses, whereas a stale entry would let a
# brand-new divergence in those two endpoints inherit a closed item of #410 —
# the exact accident the sentence above exists to prevent. #410 itself stays
# OPEN for the two below.
FOUND_BY_THIS_GATE = {
    "aas/max_aas": 410,
    "time_model/io_worker_busy_pct": 410,
}

CPU_SCALARS = ("time_model/has_measured_cpu", "time_model/cpu_ms",
               "time_model/offcpu_ms", "time_model/cpu_group_ms",
               "time_model/rowset")

# #562, Top Sessions' measured-CPU split: the RAW path now reports measured
# on-CPU time with its runqueue remainder (agent/laneC-raw-compute), while the
# SUMMARY path still reports the unsplit gap WALL in cpu_pct and no remainder
# at all. laneC's own test_window_clip section 13d documents this as a
# registered HOLE rather than a fix, because closing it needs the summary
# writer to carry the measurement -- that is Lane S's format epoch, in flight,
# and not fixable from the raw side.
#
# CLOSING CONDITION: when the epoch lands the summary-side measured split,
# these fields start AGREEING, this gate reports them as
# `xpass ... delete this entry`, and deleting them from
# summary_raw_parity_expected.json is the last step of landing that fix.
#
# The match is deliberately SHAPE-EXACT, not "anything under sessions/".
# A blanket sessions/ rule would let the next, unrelated Top Sessions
# divergence inherit #562 just by sitting next to these -- the exact failure
# issue_for() exists to prevent, and the one that put eight pins on closed
# #379. Only the four field shapes #562 actually produces match:
#   sessions/has_measured_cpu, sessions/offcpu_ms,
#   sessions/row/<pid>/cpu_pct, sessions/row/<pid>/offcpu_ms
SESSIONS_MEASURED_CPU_562 = re.compile(
    r"^sessions/(has_measured_cpu|offcpu_ms"
    r"|row/\d+/(cpu_pct|offcpu_ms))$")


def issue_for(field, window):
    """Which open issue owns this divergence.

    Attribution is by rule rather than by hand so that a NEW field appearing
    in a window cannot inherit an issue number just by sitting next to one.
    """
    if field in FOUND_BY_THIS_GATE:
        return FOUND_BY_THIS_GATE[field]
    if field.endswith("/top_wait") or field.endswith("/top_wait_id"):
        return 319                      # two definitions of one column
    if "CPU" in field or field in CPU_SCALARS:
        return 408                      # has_measured_cpu=false on summaries
    if SESSIONS_MEASURED_CPU_562.match(field):
        return 562                      # Top Sessions raw measures, summary walls
    # No straddle_130s catch-all for #324: handle_top_queries always uses raw
    # (server.c:5579). The remaining production readers of the full-duration
    # per-query table are tm_summary_visitor (compute.c:2886, gate :2892)
    # and aas_summary_visitor (:2717, gate :2731); both require query_id != 0
    # (AAS sets has_query_filter from that predicate at :2825). All four CMDS
    # above are requested UNFILTERED, so #324 cannot cause a divergence here.
    # An unmatched field must remain UNEXPLAINED for unexplained()/--record
    # to refuse it, rather than laundering a new regression into "known".
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


def set_version(trace_dir, current_version):
    """Port the C set_version helper: validate and rewrite only file headers.

    pgwt_trace_file_header is packed (event_writer.h), with uint32 magic,
    uint16 version/flags, uint32 pg_version and two uint64 clock fields.
    Summary blocks and all raw files remain byte-identical to the fresh copy.
    """
    header = struct.Struct("=IHHIQQ")
    paths = sorted(glob.glob(os.path.join(trace_dir, "*.summary")))
    if not paths:
        raise RuntimeError("no summary headers to stamp stale")
    if glob.glob(os.path.join(trace_dir, "*.summary.lz4")):
        raise RuntimeError("fixture has archived summaries; refuse to leave "
                           "an unstamped header")
    for path in paths:
        with open(path, "r+b") as f:
            values = list(header.unpack(f.read(header.size)))
            if values[0] != 0x53574750 or values[1] != current_version:
                raise RuntimeError("not a current summary header: %s" % path)
            values[1] = current_version - 1
            f.seek(0)
            f.write(header.pack(*values))
    return [os.path.basename(p) for p in paths]


def main():
    record = "--record" in sys.argv
    sabotage = "--sabotage" in sys.argv
    sabotage_stale = "--sabotage-stale-fallback" in sys.argv

    tool_problems = P.require_tools([SERVER_BIN, GEN_BIN])
    if tool_problems:
        for p in tool_problems:
            print("FAIL dependency: %s" % p)
        print("=== test_summary_raw_parity: FAIL (the gate could not run; "
              "that is a failure, not a skip) ===")
        return 1

    # Before measuring a capture, prove the stale arm's detectors can still
    # SEE. A blinded detector must not get the chance to approve a live run.
    self_ok, self_lines = run_stale_detector_selftest()
    if not self_ok:
        for l in self_lines:
            print(l)
        print("=== test_summary_raw_parity: FAIL (the stale arm's own "
              "detectors are blind; refusing to report on a capture) ===")
        return 1

    os.makedirs(RESULT_DIR, exist_ok=True)
    sum_dir = generate_traces(build_capture())
    raw_dir = sum_dir + "_rawref"
    stale_dir = sum_dir + "_stalever"
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
              "sabotage_stale_fallback": sabotage_stale,
              "summary_format_version": summary_format_version(),
              "summary_files_present": sorted(
                  os.path.basename(p) for p in glob.glob(
                      os.path.join(sum_dir_used, "*.summary*"))),
              "windows": {}}
    expected = load_expected()
    recorded = {}
    # The detector self-test's verdict is part of the printed log and the
    # artifact, so a reader can see it ran rather than trusting that it did.
    lines = list(self_lines)
    report["stale_detector_selftest_ok"] = self_ok
    report["stale_detector_selftest"] = self_lines
    try:
        version = int(summary_format_version())
        if version <= 1:
            raise RuntimeError("summary version must have a predecessor")
        shutil.copytree(sum_dir, stale_dir)
        stamped = set_version(stale_dir, version)
        report["stale_summary_version"] = version - 1
        report["stale_summary_files"] = stamped
        for name, (frm, to) in list(WINDOWS.items()) + [CONTROL_WINDOW]:
            is_control = (name == CONTROL_WINDOW[0])
            r_ans, r_oracle, r_err = ask(
                raw_dir, frm, to, os.path.join(RESULT_DIR, "err_%s_raw" % name))
            s_ans, s_oracle, s_err = ask(
                sum_dir_used, frm, to,
                os.path.join(RESULT_DIR, "err_%s_sum" % name))
            t_ans, t_oracle, t_err = ask(
                stale_dir, frm, to,
                os.path.join(RESULT_DIR, "err_%s_stale" % name))
            if sabotage_stale and not is_control:
                if t_ans["time_model"].get("cpu_ms", 0) <= 0:
                    raise RuntimeError("stale sabotage needs nonzero measured CPU")
                t_ans["time_model"]["cpu_ms"] = 0
                lines.append("SABOTAGE stale fallback answer: cpu_ms=0")
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
                w_lines.append("  Time Model dir routes: raw-side=%s summary-side=%s"
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
            t_routes = {"raw": P.classify_route(
                t_oracle, t_err, marker_expected=not is_control),
                "summary": routes["summary"]}
            # The stale arm's NUMERIC oracle is the summary-less raw dir, not
            # the fresh-summary answer: the version refusal puts this arm on
            # the raw path, so the raw answer is what it owes. s_fields is
            # still passed, for the one thing only it can prove -- that this
            # window routes to summaries when the headers are fresh, so the
            # stamping is what forced the fallback. See P.stale_verdict.
            t_ok, t_lines, t_findings = P.stale_verdict(
                name, t_routes, P.extract_all(t_ans), s_fields, t_err,
                version, len(stamped), is_control=is_control,
                raw_fields=r_fields)
            ok = ok and t_ok
            lines.extend(t_lines)
            report["windows"][name]["stale"] = {
                "ok": t_ok, "routes": t_routes,
                "fallback": P.fallback_reason(t_err),
                "findings": [f.as_dict() for f in t_findings
                             if f.status != P.AGREE],
                "fields_compared": len(t_findings),
            }
            if not is_control:
                tbl = P.record_table(findings)
                for field, entry in tbl.items():
                    entry["issue"] = issue_for(field, name)
                recorded[name] = tbl
    finally:
        cleanup_traces(sum_dir)
        cleanup_traces(raw_dir)
        cleanup_traces(stale_dir)

    print("=== test_summary_raw_parity ===")
    for l in lines:
        print(l)
    report["ok"] = ok
    with open(os.path.join(RESULT_DIR, "summary.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    print("artifact: %s" % os.path.join(RESULT_DIR, "summary.json"))

    if record:
        # Recording may update the historical raw/summary pins, never excuse
        # a broken third arm (or a failed route/premise control).
        if not ok:
            print("REFUSING to record: parity or stale-version proof failed")
            return 1
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
    # Includes generation and ServerHarness.start()'s initial blocking read,
    # which a per-query timeout would miss. Missing tools already FAIL above.
    def deadline(signum, frame):
        raise TimeoutError("parity gate exceeded 120 seconds")

    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(120)
    try:
        sys.exit(main())
    except Exception as exc:
        print("FAIL summary/raw parity: %s" % exc)
        sys.exit(1)
    finally:
        signal.alarm(0)
