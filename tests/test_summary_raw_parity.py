#!/usr/bin/env python3
"""Summaries-off production gate: one binary and byte-identical captures
with fresh, missing and stale summary files must all answer long EXACT
windows from raw events, and refuse at a one-event load limit.

The original capture, window shapes, historical pin helpers and their
anti-vacuity tests remain for the later summary rebuild. Production no
longer consumes or rewrites expected-divergence pins. Numeric agreement
here checks summary-file independence; it is not numerical ground truth.
"""

import glob
import json
import os
import shutil
import signal
import struct
import sys
import tempfile

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
    EXACT-covered and long enough to catch accidental summary re-enabling."""
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

CMDS = ("time_model", "aas", "top_sessions", "top_queries", "top_events", "heatmap")


def ask(trace_dir, frm, to, stderr_path):
    """Both the five answers and the route evidence for one (dir, window)."""
    answers = {}
    with ServerHarness(trace_dir, stderr_path=stderr_path) as srv:
        for cmd in CMDS:
            answers[cmd] = srv.query(cmd, from_=frm, to_=to, buckets=10,
                                     limit=100)
    # Every formerly summary-capable handler must enforce the raw event limit.
    with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
        oracles = {cmd: srv.query(cmd, from_=frm, to_=to, buckets=10, limit=100)
                   for cmd in CMDS}
    return answers, oracles


# ── Expected divergences ───────────────────────────────────────────────────
#
# Each entry: the field, the OPEN ISSUE it belongs to, and the exact pair
# measured on master (c27bf7d) with this fixture. --record only normalizes a
# passing table: NEW and CHANGED-DIVERGENCE fail before recording can run.
# A deliberate fixture change needs reviewed, hand-edited pins and a rerun.
# XPASS reports agreement, not proof that the owning issue is fixed.
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
#          path) and io_worker_busy_pct (explicitly declared unavailable there).
#
# Before changing a pin after XPASS, prove the fix with the owning honesty
# gate (tests/test_data_summary_honesty.py for IO worker availability).
# Matching values alone can be matching fabrications. --record cannot ADD a
# pin on a passing run: only KNOWN entries survive; XPASS entries are omitted.
# Review the honesty evidence before using --record to remove a pin.
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
#   * time_model/io_worker_busy_pct — null and declared unavailable on summary.
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


def issue_for(field, window):
    """Which open issue owns this divergence.

    Attribution is by rule rather than by hand so that a NEW field appearing
    in a window cannot inherit an issue number just by sitting next to one.
    """
    if field in FOUND_BY_THIS_GATE:
        return FOUND_BY_THIS_GATE[field]
    if field.endswith("/top_wait") or field.endswith("/top_wait_id"):
        return 319                      # two definitions of one column
    # No CPU rule: #408 (summaries reporting has_measured_cpu=false) and
    # #562 (Top Sessions raw measuring while summary reported wall) are both
    # CLOSED by the v9 epoch. The per-second record now carries the
    # per-session measured-CPU split and sessions routing reports it, making
    # all 32 sessions/* pins xpass and be deleted. Mapping a future CPU
    # divergence to a closed issue is WORSE than no rule. NEW and
    # CHANGED-DIVERGENCE are in FAIL_STATUSES, so --record returns at
    # `if not ok` BEFORE unexplained() is consulted. An unattributable CPU
    # divergence cannot be regenerated into "known" for that stronger reason;
    # unexplained() is an additional attribution guard on a passing run.
    # Top Events is compared too: extract_events walks its response scalars
    # and rows. #573 reads the existing v7 record CPU totals for the
    # unfiltered summary route, so it reports the same split as raw without
    # a format change. The per-query table still has no measurement, but
    # server.c routes that drill to raw. #818 fixed the eight window-scoped
    # Top Events divergences; their pins and attribution rule are gone.
    # Only time_model/io_worker_busy_pct remains pinned in straddle_130s.
    # A future unattributable events/* divergence must remain UNEXPLAINED;
    # its NEW or CHANGED-DIVERGENCE verdict prevents recording.
    # No straddle_130s catch-all for #324: handle_top_queries always uses raw
    # in server.c. The remaining production readers of the full-duration
    # per-query table are tm_summary_visitor and aas_summary_visitor in
    # compute.c; both require query_id != 0 (AAS sets has_query_filter from
    # that predicate). All five CMDS
    # above are requested UNFILTERED, so #324 cannot cause a divergence here.
    # An unmatched field must remain UNEXPLAINED rather than inherit a
    # refuted cause. Its failing verdict already prevents recording.
    return "UNEXPLAINED"


def unexplained(recorded):
    """Entries a regeneration could not attribute to anything.

    NEW and CHANGED-DIVERGENCE already fail the verdict; --record returns
    at `if not ok` before this additional attribution guard is consulted.
    A passing run can only retain KNOWN pins, never add a new divergence.
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
    if "--record" in sys.argv:
        print("REFUSING to record: summaries are off; historical pins stay frozen")
        return 1
    problems = P.require_tools([SERVER_BIN, GEN_BIN])
    if problems:
        for problem in problems:
            print("FAIL dependency: " + problem)
        return 1
    self_ok, self_lines = run_stale_detector_selftest()
    for line in self_lines:
        print(line)
    if not self_ok:
        return 1

    fresh = generate_traces(build_capture())
    try:
        with tempfile.TemporaryDirectory(prefix="pgwt-summaries-off-") as work:
            missing = os.path.join(work, "missing")
            stale = os.path.join(work, "stale")
            shutil.copytree(fresh, missing)
            shutil.copytree(fresh, stale)
            removed = glob.glob(os.path.join(missing, "*.summary*"))
            if not removed:
                raise RuntimeError("fresh fixture needs real summary files")
            for path in removed:
                os.remove(path)
            set_version(stale, int(summary_format_version()))
            dirs = {"fresh": fresh, "missing": missing, "stale": stale}
            if "--sabotage" in sys.argv:
                # The fresh-file premise must be proved, never assumed.
                dirs["fresh"] = missing
            if not glob.glob(os.path.join(dirs["fresh"], "*.summary")):
                raise RuntimeError("fresh arm lost its summary-file premise")
            ok = True
            for name, (frm, to) in list(WINDOWS.items()) + [CONTROL_WINDOW]:
                fields_by_arm, routes = {}, {}
                for arm, trace_dir in dirs.items():
                    answers, oracles = ask(trace_dir, frm, to,
                                           os.path.join(work, name + arm + ".log"))
                    if arm == "stale" and "--sabotage-stale-fallback" in sys.argv:
                        if answers["time_model"].get("cpu_ms", 0) <= 0:
                            raise RuntimeError("stale sabotage requires measured CPU")
                        answers["time_model"]["cpu_ms"] = 0
                    if arm == "fresh" and "--sabotage-io-worker-value" in sys.argv:
                        answers["time_model"]["io_worker_busy_pct"] = 0.0
                        answers["time_model"]["io_worker_busy_pct_unavailable"] = True
                    for cmd in CMDS:
                        answer, oracle = answers[cmd], oracles[cmd]
                        # Raw refusals have max_events=1; a normal summary
                        # response or any unrelated failure cannot prove raw.
                        routes[arm + "/" + cmd] = (
                            P.ROUTE_RAW if oracle.get("code") == "window_too_large"
                            and oracle.get("max_events") == 1 else P.ROUTE_UNKNOWN)
                        if "error" in answer or answer.get("fidelity") != "exact":
                            raise RuntimeError("%s/%s/%s needs a successful EXACT answer"
                                               % (name, arm, cmd))
                    fields = P.extract_all(answers)
                    # Heatmap is outside the historical extractors. Include
                    # its entire payload, so missing cells cannot pass parity.
                    fields["heatmap/payload"] = json.dumps(answers["heatmap"], sort_keys=True)
                    fields_by_arm[arm] = fields
                passed, lines = P.summaries_off_verdict(name, routes, fields_by_arm, CMDS)
                for line in lines:
                    print(line)
                ok = ok and passed
            print("=== test_summary_raw_parity (summaries off): %s ==="
                  % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
    finally:
        cleanup_traces(fresh)


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
