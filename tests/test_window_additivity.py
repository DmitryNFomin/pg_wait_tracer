#!/usr/bin/env python3
"""test_window_additivity.py — THE #424 RIGHT-EDGE ADMISSION GATE.

WHAT IS BROKEN (established before this file was written; see the issue)
─────────────────────────────────────────────────────────────────────────────
pgwt-server's raw loader (`load_file_range_mono` / `server_load_events_fi_mode`,
src/server.c) admits a non-sample record only when its CLOSE timestamp falls in
[from, to]:

        if (ts < from_m) continue;          /* close before the window  */
        if (ts > to_m)   continue;          /* close after  the window  */

The correct admission rule for an INTERVAL is OVERLAP:

        close >= from  AND  start <= to

The right half is implemented nowhere. `event_window_ns` (src/compute.c) clips
correctly, but it only ever sees what admission let in. So a wait still open at
the window's right edge contributes 0, and a wait spanning the entire window is
dropped entirely.

WHY THIS GATE NEEDS NO REFERENCE PATH
─────────────────────────────────────────────────────────────────────────────
Clipping is ADDITIVE: if a window is cut into adjacent parts, every record's
in-window time is split across the parts and nothing is created or lost, so

        Σ over parts of (time in part)  ==  time in the whole window

for every class, event and session, exactly. That identity is a property of
clipping itself — it needs no second code path, no summary file and no oracle
to disagree with. The raw path violates it, because every internal boundary
drops the pre-boundary part of each record that closes just after it. The
deficit therefore GROWS with the boundary count, which noise cannot do.

The occurrence half of the same identity is the guard against a HALF FIX:

        Σ over parts of (count in part)  ==  count in the whole window

A partition assigns each close timestamp to exactly one part, so counts are
additive today and must stay additive. An admission-only fix — one that widens
admission to overlap without making count/max/histogram select on the record's
own close timestamp (src/compute.c:1587-1596, :1883-1886) — makes the same
record an occurrence in every part it overlaps, and THAT goes red here while
the time half goes green. A gate that only checked time would wave the half
fix through, swapping an undercount for an overcount.

WHAT IS *NOT* CLAIMED
─────────────────────────────────────────────────────────────────────────────
Only HISTORICAL windows. A wait that is still open RIGHT NOW has no closing
record in the trace at all, so no admission rule can recover it; that needs
trace-format work. Nothing here asserts anything about the live right edge.

WHY THE EXISTING PARITY GATE CANNOT SEE #424
─────────────────────────────────────────────────────────────────────────────
tests/test_summary_raw_parity.py is structurally blind to it, twice over:
its generator closes every interval at the shutdown flush, and its `QUIET`
tuple deliberately places every window edge in a data hole ("so that every
window edge below falls in a data hole"). Reasonable for #379, which is a LEFT
straddle; it is exactly what hides #424. Section C below reuses that same
fixture with an edge placed deliberately NOT in a hole.

REFUSE, NEVER SKIP
─────────────────────────────────────────────────────────────────────────────
Every way this harness can fail to see is an exit-2 REFUSAL, never a pass: a
missing server binary (exec would be 127), a non-executable one (126), a
missing generator, a server that dies or answers unparseably, a response whose
route cannot be determined, or a fixture self-check that fails. A gate that
cannot see must refuse.

EXIT CODES   0 PASS   1 FAIL (the product disagreed)   2 REFUSE

WHERE THIS RUNS
─────────────────────────────────────────────────────────────────────────────
tests/run_all.sh's synthetic-data tier (needs pgwt-server + gen_test_traces),
i.e. every `make box-check` and the CI Linux job — not tests/unit_tests.list,
which has no server to query. The compute-layer half of the same rule is
pinned by tests/test_window_clip.c section 6.
"""

import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (                                     # noqa: E402
    ServerHarness, generate_traces, cleanup_traces,
    CPU, IO_DATA_FILE_READ, make_event_id, CLASS_LOCK,
    SERVER_BIN, GEN_BIN,
)

S = 1_000_000_000
MS = 1_000_000
BASE = 10_000_000_000_000          # second-aligned synthetic mono/wall base
LOCK_REL = make_event_id(CLASS_LOCK, 0)

RESULT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "results", "issue424-gate")

# The busy backend's cycle: a 12 s Lock wait then a 1 s CPU gap, period 13 s.
# 13 is chosen so that the partition boundaries below land INSIDE a record at
# a different offset every time — a period that divided the window evenly
# would put every boundary on a record close, where #424 cannot fire, which
# is the parity gate's QUIET hole in another form.
PERIOD = 13
LOCK_DUR = 12
BUSY_PID = 2000
BUSY_QID = 200
FIRST_CLOSE = PERIOD               # first Lock close, seconds after BASE
LAST_CLOSE = 520

# The window under test: 300 s wide, offset by half a second so that NO
# partition boundary below can coincide with a record's close timestamp.
# That coincidence is a separate defect (section B3: a record closing exactly
# on the shared edge of two adjacent windows is an occurrence in BOTH), and
# mixing the two would make the additivity failure here ambiguous.
WIN_FROM_S, WIN_TO_S = 100.5, 400.5
WIN_FROM_NS = BASE + 100 * S + 500 * MS
WIN_TO_NS = BASE + 400 * S + 500 * MS
PARTS = (1, 2, 4, 8)

# A pgwt-server that never answers must FAIL this gate, not hang the
# box run: tests/run_all.sh wraps nothing in a timeout, so an
# unbounded readline() here would stall the whole synthetic tier. The
# value is far above any measured response for a capture this size
# (whole runs take seconds), so it can only fire on a real stall.
Q_TIMEOUT_S = int(os.environ.get("PGWT_ADDITIVITY_TIMEOUT_S", "120"))

failures = []
log = []


def say(kind, msg):
    line = "%-7s %s" % (kind, msg)
    log.append(line)
    print(line, flush=True)


class Refuse(Exception):
    pass


def fail(msg):
    failures.append(msg)
    say("FAIL", msg)


def ok(msg):
    say("ok", msg)


# ── The fixture ────────────────────────────────────────────────────────────

def build_capture():
    """One backend that is BUSY FOR THE ENTIRE WINDOW.

    Lock spans are [PERIOD*j - LOCK_DUR, PERIOD*j]; the CPU gap fills
    [PERIOD*j, PERIOD*j + 1]. There is no hole, so the window's DB Time has a
    reference-free value: the window width itself.
    """
    ev = []
    for close in range(FIRST_CLOSE, LAST_CLOSE + 1, PERIOD):
        ev.append({"pid": BUSY_PID, "ts": BASE + close * S, "dur": LOCK_DUR * S,
                   "old": LOCK_REL, "new": CPU, "qid": BUSY_QID, "cpu": 0})
        ev.append({"pid": BUSY_PID, "ts": BASE + (close + 1) * S, "dur": S,
                   "old": CPU, "new": LOCK_REL, "qid": BUSY_QID, "cpu": 0})
    return {
        "cpu_measured": 0,
        "interleave": 1,
        "backends": [{"pid": BUSY_PID, "type": "client", "user": "u",
                      "db": "d"}],
        "queries": [{"id": BUSY_QID, "text": "q%d" % BUSY_QID}],
        "events": sorted(ev, key=lambda e: e["ts"]),
    }


def intervals():
    """(start_s, close_s) for every record in the fixture, derived from the
    SAME constants the scenario is built from — used by the oracles below."""
    out = []
    for close in range(FIRST_CLOSE, LAST_CLOSE + 1, PERIOD):
        out.append((close - LOCK_DUR, close))      # Lock
        out.append((close, close + 1))             # CPU gap
    return out


def oracle_overlap_ms(frm_s, to_s):
    """Correct answer: Σ over records of the record's overlap with [frm,to]."""
    total = 0.0
    for st, cl in intervals():
        total += max(0.0, min(cl, to_s) - max(st, frm_s))
    return total * 1000.0


def oracle_close_only_ms(frm_s, to_s):
    """What the CLOSE-ONLY admission rule produces: the same clipping, but
    only for records whose CLOSE lies inside the window. This is a model of
    the defect, written from the source, and is used ONLY to prove the
    fixture can expose it (section A) — never as an expected value."""
    total = 0.0
    for st, cl in intervals():
        if not (frm_s <= cl <= to_s):
            continue
        total += max(0.0, min(cl, to_s) - max(st, frm_s))
    return total * 1000.0


def edges_ns(nparts):
    """Partition edges in absolute ns. Exact integer arithmetic: the span is
    300e9 ns, divisible by every count in PARTS, so no edge is rounded."""
    span = WIN_TO_NS - WIN_FROM_NS
    assert span % nparts == 0, "partition would round an edge"
    return [WIN_FROM_NS + span * i // nparts for i in range(nparts + 1)]


def to_s(ns):
    """Absolute ns → seconds after BASE. Exact for every value here (all are
    well under 2**53 and land on halves of a second)."""
    return (ns - BASE) / 1e9


def edges(nparts):
    return [to_s(e) for e in edges_ns(nparts)]


# ── Route discrimination: three independent layers ─────────────────────────
#
# This gate measures the RAW path. "the summary route answered instead" is the
# exact false negative that would make every number here meaningless, so the
# route is established three ways, none of which is "the test assumed so".
#
# 1. STRUCTURAL (require_no_summaries): the queried directory holds no summary
#    file at all, so src/server.c's own documented fallback ("no summary file
#    covers this window -- recomputing from raw events") is the only route
#    left. Checked once; it is a fact about the filesystem.
# 2. PER RESPONSE, time_model only: "categories" is emitted inside
#    `if (!from_summaries)` ("Raw path only -- summaries carry no category
#    data"). Present => RAW; absent with rows+db_time_ms => SUMMARY; neither
#    => refuse. "fidelity":"exact" is NOT used as a route signal — #402
#    returned "exact" from a raw fallback. Only time_model emits categories,
#    which is why this layer cannot stand alone.
# 3. POSITIVE ORACLE (route_by_oracle), per command, on the measured window:
#    same binary, dir and window re-run with PGWT_LOAD_MAX_EVENTS=1. The raw
#    path must materialise the window's events and so refuses with
#    code=window_too_large; the summary path loads no events and answers
#    normally. Intrinsic to the route, and it works for commands that emit no
#    categories array.

def require_no_summaries(trace_dir):
    strays = [n for n in os.listdir(trace_dir) if ".summary" in n]
    if strays:
        raise Refuse("the queried dir %s still holds summary files %s; the "
                     "summary route could answer, and this gate measures raw"
                     % (trace_dir, strays))


def route_by_oracle(trace_dir, cmd, frm, to, filters=None):
    """PGWT_LOAD_MAX_EVENTS=1: raw must refuse, summary answers anyway."""
    kw = {"from_": frm, "to_": to}
    if filters:
        kw["filters"] = filters
    try:
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
            r = srv.query(cmd, timeout=120, **kw)
    except (OSError, RuntimeError, TimeoutError) as e:
        raise Refuse("route oracle for %s could not run: %s" % (cmd, e))
    if not isinstance(r, dict):
        return "unknown"
    err = r.get("error") if isinstance(r.get("error"), dict) else {}
    for src in (r, err):
        if src.get("code") == "window_too_large" and src.get("max_events") == 1:
            return "raw"
    if "rows" in r and "error" not in r:
        return "summary"
    return "unknown"


def require_raw_oracle(trace_dir, cmd, frm, to, filters=None):
    r = route_by_oracle(trace_dir, cmd, frm, to, filters)
    if r != "raw":
        raise Refuse("the %s route oracle says %r, not 'raw'; a window this "
                     "gate measures must be answered from raw events"
                     % (cmd, r))
    ok("route oracle: %s on the measured window is answered from RAW events"
       % cmd)


def answer(resp, what):
    """A usable response, for a command with no in-payload route marker. The
    route for these is established by require_no_summaries() plus
    require_raw_oracle() on the same dir and window."""
    if not isinstance(resp, dict):
        raise Refuse("%s: response is not a JSON object: %r" % (what, resp))
    if "error" in resp:
        raise Refuse("%s: server refused the request: %r"
                     % (what, resp["error"]))
    if "rows" not in resp:
        raise Refuse("%s: response carries no rows: keys=%s"
                     % (what, sorted(resp.keys())))
    return resp


def route_by_categories(resp):
    if not isinstance(resp, dict):
        raise Refuse("response is not a JSON object: %r" % (resp,))
    if "error" in resp:
        raise Refuse("server refused the request: %r" % resp["error"])
    if "categories" in resp:
        return "raw"
    if "rows" in resp and "db_time_ms" in resp:
        return "summary"
    raise Refuse("cannot determine which route answered: keys=%s"
                 % sorted(resp.keys()))


def require_raw(resp, what):
    """Only for time_model responses — the one command with a positive raw
    marker in its own payload."""
    r = route_by_categories(resp)
    if r != "raw":
        raise Refuse("%s was answered by the %s route; this gate measures "
                     "the RAW path" % (what, r))
    return resp


def db_time_ms(resp):
    v = resp.get("db_time_ms")
    if v is None:
        raise Refuse("response carries no db_time_ms: keys=%s"
                     % sorted(resp.keys()))
    return float(v)


def class_ms(resp, name):
    for row in resp.get("rows", []):
        if row.get("name") == name and row.get("indent") == 1:
            return float(row.get("ms") or 0.0)
    return 0.0


def event_row(resp, event_name_contains):
    for row in resp.get("rows", []):
        if event_name_contains in (row.get("name") or ""):
            return row
    return None


def raw_only_copy(trace_dir):
    """A copy of the capture with its summary files removed, so the product's
    OWN supported fallback ("no summary file covers this window") picks the
    raw route. Same trace bytes, no second binary, no patched constant."""
    dst = tempfile.mkdtemp(prefix="i424_raw_")
    for name in os.listdir(trace_dir):
        src = os.path.join(trace_dir, name)
        if os.path.isfile(src) and ".summary" not in name:
            shutil.copy2(src, os.path.join(dst, name))
    if not os.listdir(dst):
        raise Refuse("raw-only copy of %s is empty — nothing to query"
                     % trace_dir)
    return dst


def preflight():
    """Every dependency, checked before anything is measured. 127/126 are the
    two exit codes a missing or non-executable binary would produce, and both
    must refuse rather than skip."""
    for path, what in ((SERVER_BIN, "pgwt-server"),
                       (GEN_BIN, "gen_test_traces")):
        if not os.path.exists(path):
            raise Refuse("%s not found at %s (exec would be 127); build it "
                         "before running this gate" % (what, path))
        if not os.access(path, os.X_OK):
            raise Refuse("%s at %s is not executable (exec would be 126)"
                         % (what, path))


# ── A. Fixture capability (no product involved) ────────────────────────────

def section_a():
    """Before measuring anything, prove the fixture CAN expose #424 — and by
    how much at each boundary count. All of this is arithmetic on the
    scenario constants; the server is not running yet. If any of it fails the
    gate REFUSES, because a fixture that cannot straddle makes every
    assertion below vacuous."""
    say("", "=== A. fixture capability (arithmetic only, no server) ===")

    whole = oracle_overlap_ms(WIN_FROM_S, WIN_TO_S)
    width = (WIN_TO_S - WIN_FROM_S) * 1000.0
    if abs(whole - width) > 1e-6:
        raise Refuse("fixture does not cover the window: overlap oracle says "
                     "%.1f ms, window is %.1f ms wide" % (whole, width))
    ok("the backend is busy for the whole window: %.1f ms == window width"
       % whole)

    # Records crossing the RIGHT edge, and crossing each internal boundary.
    def crossers(t):
        return [(st, cl) for st, cl in intervals() if st < t < cl]

    right = crossers(WIN_TO_S)
    if not right:
        raise Refuse("no record crosses the window's right edge — this "
                     "fixture cannot see #424 (the parity gate's QUIET hole)")
    ok("%d record(s) cross the right edge at %ss: %s"
       % (len(right), WIN_TO_S, right))

    deficits = {}
    for n in PARTS:
        e = edges(n)
        parts_sum = sum(oracle_close_only_ms(e[i], e[i + 1]) for i in range(n))
        whole_broken = oracle_close_only_ms(WIN_FROM_S, WIN_TO_S)
        deficits[n] = parts_sum - whole_broken
        internal = e[1:-1]
        if n > 1 and not any(crossers(t) for t in internal):
            raise Refuse("no internal boundary of the %d-part partition falls "
                         "inside a record; the partition cannot expose #424"
                         % n)
    ok("close-only model's deficit by part count: %s"
       % ", ".join("%d parts %+.1f ms" % (n, deficits[n]) for n in PARTS))

    seq = [abs(deficits[n]) for n in PARTS]
    if seq[0] != 0.0:
        raise Refuse("the 1-part 'partition' must be a no-op, got %+.1f ms"
                     % seq[0])
    for a, b in zip(seq, seq[1:]):
        if not b > a:
            raise Refuse("the close-only model's deficit is not strictly "
                         "monotone in the boundary count (%s) — the fixture "
                         "does not carry #424's signature" % seq)
    ok("…strictly monotone in the boundary count: %s"
       % " < ".join("%.1f" % v for v in seq))
    return {"oracle_whole_ms": whole,
            "oracle_close_only_whole_ms":
                oracle_close_only_ms(WIN_FROM_S, WIN_TO_S),
            "oracle_close_only_deficit_ms": deficits}


# ── B. The product ─────────────────────────────────────────────────────────

def section_b(raw_dir):
    say("", "=== B. the product on the same window ===")
    out = {}
    frm, to = WIN_FROM_NS, WIN_TO_NS

    require_no_summaries(raw_dir)
    ok("the queried dir holds no summary file: raw is the only route")
    require_raw_oracle(raw_dir, "time_model", frm, to)
    require_raw_oracle(raw_dir, "top_events", frm, to)

    with ServerHarness(raw_dir) as srv:
        whole = require_raw(srv.query("time_model", timeout=Q_TIMEOUT_S, from_=frm, to_=to),
                            "the whole-window time_model")
        whole_ev = answer(srv.query("top_events", timeout=Q_TIMEOUT_S, from_=frm, to_=to,
                                    limit=100),
                          "the whole-window top_events")

        # B1. The absolute number, reference-free: a backend busy for the
        # whole window must account for exactly the window width.
        got = db_time_ms(whole)
        want = (WIN_TO_S - WIN_FROM_S) * 1000.0
        out["whole_db_time_ms"] = got
        if abs(got - want) > 1e-6:
            fail("whole-window DB Time — got %.1f ms, want %.1f ms "
                 "(deficit %+.1f ms; the fixture is busy end to end)"
                 % (got, want, got - want))
        else:
            ok("whole-window DB Time == the window width (%.1f ms)" % got)

        lock_got = class_ms(whole, "Lock")
        lock_want = sum(min(cl, WIN_TO_S) - max(st, WIN_FROM_S)
                        for st, cl in intervals()[0::2]
                        if min(cl, WIN_TO_S) > max(st, WIN_FROM_S)) * 1000.0
        out["whole_lock_ms"] = lock_got
        if abs(lock_got - lock_want) > 1e-6:
            fail("whole-window Lock time — got %.1f ms, want %.1f ms (%+.1f)"
                 % (lock_got, lock_want, lock_got - lock_want))
        else:
            ok("whole-window Lock time == %.1f ms" % lock_got)

        # B2. Additivity of TIME and of COUNTS over 1/2/4/8 parts.
        whole_counts = {r["name"]: int(r.get("count") or 0)
                        for r in whole_ev.get("rows", [])}
        out["whole_counts"] = whole_counts
        out["parts"] = {}
        for n in PARTS:
            e, en = edges(n), edges_ns(n)
            sum_db, sum_lock = 0.0, 0.0
            sum_counts = {}
            for i in range(n):
                pf, pt = en[i], en[i + 1]
                tm = require_raw(srv.query("time_model", timeout=Q_TIMEOUT_S, from_=pf, to_=pt),
                                 "part %d/%d of the time_model partition"
                                 % (i + 1, n))
                te = answer(srv.query("top_events", timeout=Q_TIMEOUT_S, from_=pf, to_=pt,
                                      limit=100),
                            "part %d/%d of the top_events partition"
                            % (i + 1, n))
                sum_db += db_time_ms(tm)
                sum_lock += class_ms(tm, "Lock")
                for r in te.get("rows", []):
                    sum_counts[r["name"]] = (sum_counts.get(r["name"], 0)
                                             + int(r.get("count") or 0))
            out["parts"][n] = {"db_time_ms": sum_db, "lock_ms": sum_lock,
                               "counts": sum_counts}

            d = sum_db - out["whole_db_time_ms"]
            if abs(d) > 1e-6:
                fail("TIME not conserved over %d parts — Σ parts %.1f ms vs "
                     "whole %.1f ms, deficit %+.1f ms"
                     % (n, sum_db, out["whole_db_time_ms"], d))
            else:
                ok("TIME conserved over %d parts (%.1f ms)" % (n, sum_db))

            dl = sum_lock - out["whole_lock_ms"]
            if abs(dl) > 1e-6:
                fail("Lock time not conserved over %d parts — Σ parts %.1f ms "
                     "vs whole %.1f ms, deficit %+.1f ms"
                     % (n, sum_lock, out["whole_lock_ms"], dl))
            else:
                ok("Lock time conserved over %d parts (%.1f ms)"
                   % (n, sum_lock))

            # Counts. Additive today; a fix that widens admission without
            # making occurrences select on the close timestamp breaks THIS
            # while leaving the time legs green.
            names = set(whole_counts) | set(sum_counts)
            bad = {k: (sum_counts.get(k, 0), whole_counts.get(k, 0))
                   for k in names
                   if sum_counts.get(k, 0) != whole_counts.get(k, 0)}
            if bad:
                fail("COUNTS not conserved over %d parts — %s"
                     % (n, ", ".join("%s Σparts %d vs whole %d"
                                     % (k, v[0], v[1])
                                     for k, v in sorted(bad.items()))))
            else:
                ok("COUNTS conserved over %d parts (%s)"
                   % (n, ", ".join("%s=%d" % kv
                                   for kv in sorted(sum_counts.items()))))

        # B3. The SHARED EDGE. A SEPARATE defect from #424, found while
        # placing this gate's boundaries: admission is closed on both ends
        # (`ts < from -> skip`, `ts > to -> skip`), so a record closing
        # EXACTLY on the edge two adjacent windows share is admitted — and
        # counted — in BOTH. Its time is not double-counted (the second
        # window clips it to zero width), so only the occurrence half moves.
        # Measured here on purpose rather than left to collide with B2: B2's
        # window is offset by half a second so no boundary can coincide.
        cut = FIRST_CLOSE + PERIOD * 20                 # a Lock close, exactly
        a, b = cut - 5 * PERIOD, cut + 5 * PERIOD
        parts, whole_c = {}, {}
        for lo, hi, into in ((a, cut, parts), (cut, b, parts),
                             (a, b, whole_c)):
            te = answer(srv.query("top_events", timeout=Q_TIMEOUT_S, from_=BASE + lo * S,
                                  to_=BASE + hi * S, limit=100),
                        "shared-edge window [%d,%d]" % (lo, hi))
            for r in te.get("rows", []):
                into[r["name"]] = (into.get(r["name"], 0)
                                   + int(r.get("count") or 0))
        out["shared_edge"] = {"parts": parts, "whole": whole_c, "cut_s": cut}
        # Exactly how many records close ON the shared edge, from the fixture
        # constants. Each such record is admitted by BOTH windows, because
        # both ends of the admission test are inclusive, so the excess is
        # exactly that number — not "about" it.
        on_edge = len([1 for _st, cl in intervals() if cl == cut])
        excess = {k: parts.get(k, 0) - whole_c.get(k, 0)
                  for k in set(parts) | set(whole_c)}
        bad = {k: v for k, v in excess.items() if v not in (0, on_edge)}
        if bad:
            fail("SHARED EDGE at %ds: the count excess is neither 0 (edge "
                 "ownership resolved) nor %d (one per record closing exactly "
                 "on the edge) — %s. A third value means something other "
                 "than edge ownership moved"
                 % (cut, on_edge,
                    ", ".join("%s excess %+d" % (k, v)
                              for k, v in sorted(bad.items()))))
        elif any(v == on_edge for v in excess.values()):
            # DECLARED DIVERGENCE, reported and pinned, not a failure: this
            # is NOT #424. Admission is inclusive at both ends (src/server.c,
            # the exact-wins merge: "INCLUSIVE at both ends, and that is
            # correct, not a bug"), so a record closing exactly on the edge
            # two adjacent windows share is an occurrence in both. Its TIME
            # is not double-counted (the second window clips it to zero
            # width). Whether window selection should become half-open is a
            # product decision with its own blast radius — the same comment
            # records that making it half-open zeroed test_data_aas and
            # test_data_categories. Measured here so a change in its
            # magnitude cannot pass silently.
            say("DECLARED", "shared edge at %ds: counts over-report by "
                            "exactly %d per edge-closing record (%s) — the "
                            "closed-on-both-ends selection rule, NOT #424"
                % (cut, on_edge,
                   ", ".join("%s +%d" % (k, v)
                             for k, v in sorted(excess.items()) if v)))
        else:
            ok("shared edge at %ds: counts conserved, edge ownership is "
               "resolved (%s)"
               % (cut, ", ".join("%s=%d" % kv for kv in sorted(parts.items()))))
    return out


# ── C. The parity fixture, with an edge NOT in a data hole ─────────────────

def section_c():
    """tests/test_summary_raw_parity.py's own capture has pid 1001's 40 s
    IO:DataFileRead wait spanning [200, 240]. Every window in that gate's
    WINDOWS dict has its edges inside a QUIET hole, so no edge ever cuts it.
    Cut it here: [100, 220] must report 20 s of it."""
    say("", "=== C. the parity fixture with a right edge inside a wait ===")
    import test_summary_raw_parity as P                           # noqa: E402

    scenario = P.build_capture()

    def spans():
        """(start_s, close_s) for every record in the parity fixture,
        read off its own scenario array — not re-derived by hand."""
        return [((e["ts"] - BASE - e["dur"]) / 1e9, (e["ts"] - BASE) / 1e9)
                for e in scenario["events"]]

    def crossing(t_s):
        return [(st, cl) for st, cl in spans() if st < t_s < cl]

    # C0. THE FIXTURE SELF-CHECK. The parity gate's five windows each place
    # BOTH edges in a QUIET hole on purpose, so none of them has a record
    # open across its right edge and none of them can see #424. Reported as
    # an inventory, computed from the scenario array with no server running.
    # It is a statement about that gate's coverage, not a product failure,
    # so it does not fail here — but the windows THIS section measures must
    # have a non-empty right-edge straddler set, and that DOES fail, because
    # otherwise every assertion below would be measuring a hole.
    blind = [w for w, (f_ns, t_ns) in sorted(P.WINDOWS.items())
             if not crossing((t_ns - BASE) / 1e9)]
    say("DECLARED",
        "parity gate's own windows with an EMPTY right-edge straddler set "
        "(structurally blind to #424): %d of %d — %s"
        % (len(blind), len(P.WINDOWS), ", ".join(blind) or "none"))

    trace_dir = generate_traces(scenario)
    raw_dir = None
    try:
        raw_dir = raw_only_copy(trace_dir)
        filt = {"pid": 1001, "event_id": IO_DATA_FILE_READ}
        cases = [
            # (name, from_s, to_s, expected_ms, red_today)
            ("cut_at_220", 100, 220, 20_000.0, True),
            ("contains_close_260", 100, 260, 40_000.0, False),
            ("spans_whole_window", 205, 235, 30_000.0, True),
        ]
        for name, a, b, want, red in cases:
            if red and not crossing(b):
                raise Refuse("window %s [%ds,%ds] has no record open across "
                             "its right edge; it cannot see #424 and its "
                             "expectation of %.1f ms would be measuring a "
                             "hole" % (name, a, b, want))
        ok("every cut window below really has a record open across its right "
           "edge (%s)"
           % "; ".join("%s: %s" % (n, crossing(b))
                       for n, _a, b, _w, r in cases if r))
        with ServerHarness(raw_dir) as srv:
            for name, a, b, want, _red in cases:
                resp = require_raw(
                    srv.query("time_model", timeout=Q_TIMEOUT_S, from_=BASE + a * S,
                              to_=BASE + b * S, filters=filt),
                    "parity-fixture window %s" % name)
                got = db_time_ms(resp)
                if abs(got - want) > 1e-6:
                    fail("parity fixture %s [%ds,%ds] pid1001 IO — got %.1f "
                         "ms, want %.1f ms (%+.1f)"
                         % (name, a, b, got, want, got - want))
                else:
                    ok("parity fixture %s [%ds,%ds] pid1001 IO == %.1f ms"
                       % (name, a, b, got))
    finally:
        if raw_dir:
            shutil.rmtree(raw_dir, ignore_errors=True)
        cleanup_traces(trace_dir)


# ── D. False negatives: every way this gate could stop checking ────────────

def section_d(raw_dir):
    say("", "=== D. false-negative suite ===")
    frm, to = WIN_FROM_NS, WIN_TO_NS

    with ServerHarness(raw_dir) as srv:
        # D1. A window with NOTHING in it conserves vacuously. Pin that the
        # gate's own window is not such a window — i.e. that the numbers
        # compared in B are large, not zero.
        empty = require_raw(
            srv.query("time_model", timeout=Q_TIMEOUT_S, from_=BASE + (LAST_CLOSE + 100) * S,
                      to_=BASE + (LAST_CLOSE + 160) * S),
            "the empty-window control")
        if db_time_ms(empty) != 0.0:
            fail("empty control window is not empty: %.1f ms"
                 % db_time_ms(empty))
        else:
            ok("empty control window is 0.0 ms — conservation there is vacuous")
        busy = require_raw(srv.query("time_model", timeout=Q_TIMEOUT_S, from_=frm, to_=to),
                           "the non-vacuous control")
        if db_time_ms(busy) < 100_000.0:
            fail("the measured window is nearly empty (%.1f ms); section B "
                 "would conserve vacuously" % db_time_ms(busy))
        else:
            ok("the measured window carries %.1f ms — not vacuous"
               % db_time_ms(busy))

        # D2. A partition whose boundaries all fall on record CLOSES cannot
        # expose #424 — it is the QUIET hole in another form. It must still
        # conserve, and the gate must not mistake that green for coverage.
        holes = [FIRST_CLOSE + PERIOD * i for i in (8, 16, 24)]
        s = 0.0
        for a, b in zip(holes, holes[1:]):
            r = require_raw(srv.query("time_model", timeout=Q_TIMEOUT_S, from_=BASE + a * S,
                                      to_=BASE + b * S),
                            "aligned-boundary part [%d,%d]" % (a, b))
            s += db_time_ms(r)
        whole_aligned = db_time_ms(require_raw(
            srv.query("time_model", timeout=Q_TIMEOUT_S, from_=BASE + holes[0] * S,
                      to_=BASE + holes[-1] * S), "aligned-boundary whole"))
        if abs(s - whole_aligned) > 1e-6:
            fail("control: a partition cut ON record closes should conserve "
                 "even today — Σ %.1f vs whole %.1f" % (s, whole_aligned))
        else:
            ok("control: cutting ON record closes conserves (%.1f ms) — which "
               "is why edge placement, not the identity, is what makes this "
               "gate able to see" % s)

        # D3. The unbounded request (no from/to) must be untouched by any
        # admission change: every record is in.
        unb = require_raw(srv.query("time_model", timeout=Q_TIMEOUT_S, from_=0, to_=0),
                          "the unbounded request")
        want_unb = sum(cl - st for st, cl in intervals()) * 1000.0
        if abs(db_time_ms(unb) - want_unb) > 1e-6:
            fail("unbounded request — got %.1f ms, want %.1f ms (the whole "
                 "capture, every record whole)"
                 % (db_time_ms(unb), want_unb))
        else:
            ok("unbounded request == the whole capture (%.1f ms)"
               % db_time_ms(unb))

        # D4. A zero-width window: a real zero, not an unexamined one.
        zw = require_raw(srv.query("time_model", timeout=Q_TIMEOUT_S, from_=frm, to_=frm),
                         "the zero-width window")
        if db_time_ms(zw) != 0.0:
            fail("zero-width window is not 0: %.1f ms" % db_time_ms(zw))
        else:
            ok("zero-width window == 0.0 ms")

    # D5. The route discriminator itself must be able to say "not raw" and
    # "cannot tell" — otherwise require_raw() approves everything.
    for probe, expect in (({"categories": [], "rows": [], "db_time_ms": 0},
                           "raw"),
                          ({"rows": [], "db_time_ms": 0}, "summary")):
        if route_by_categories(probe) != expect:
            fail("route_by_categories() misclassified %r as %r" % (probe, route_by_categories(probe)))
    for probe in ({}, {"rows": []}, {"db_time_ms": 0}):
        try:
            route_by_categories(probe)
        except Refuse:
            continue
        fail("route_by_categories() returned a route for an undeterminable response %r"
             % probe)
    try:
        route_by_categories({"error": "window too large"})
    except Refuse:
        ok("route discriminator refuses an error, an unknown shape and a "
           "summary answer")
    else:
        fail("route_by_categories() accepted an error response")

    # D6. The preflight must refuse a missing / non-executable dependency.
    import unittest.mock as _mock
    with _mock.patch.object(sys.modules[__name__], "SERVER_BIN",
                            "/nonexistent/pgwt-server"):
        try:
            preflight()
        except Refuse:
            ok("preflight refuses a missing server binary (would be exit 127)")
        else:
            fail("preflight accepted a missing server binary")
    noexec = tempfile.NamedTemporaryFile(prefix="i424_noexec_", delete=False)
    noexec.close()
    os.chmod(noexec.name, 0o644)
    with _mock.patch.object(sys.modules[__name__], "SERVER_BIN", noexec.name):
        try:
            preflight()
        except Refuse:
            ok("preflight refuses a non-executable binary (would be exit 126)")
        else:
            fail("preflight accepted a non-executable binary")
    os.unlink(noexec.name)

    # D7. The fixture-capability oracles must themselves be falsifiable: a
    # record set with no straddler must make section A refuse.
    mod = sys.modules[__name__]
    real = mod.intervals
    try:
        mod.intervals = lambda: [(WIN_FROM_S + 10, WIN_FROM_S + 11)]
        try:
            section_a()
        except Refuse:
            ok("section A refuses a fixture with no right-edge straddler")
        else:
            fail("section A approved a fixture that cannot expose #424")
    finally:
        mod.intervals = real


def main():
    os.makedirs(RESULT_DIR, exist_ok=True)
    report = {}
    trace_dir = raw_dir = None
    try:
        preflight()
        report["A"] = section_a()
        trace_dir = generate_traces(build_capture())
        raw_dir = raw_only_copy(trace_dir)
        report["B"] = section_b(raw_dir)
        section_c()
        section_d(raw_dir)
    except (Refuse, TimeoutError, RuntimeError, OSError) as e:
        # A stall, a dead server or an unreadable pipe is a REFUSAL, never a
        # pass and never a traceback: the harness could not see what it is
        # gating. Demonstrated with PGWT_ADDITIVITY_TIMEOUT_S=0, which makes
        # every query time out and the gate exit 2.
        say("REFUSE", "%s: %s" % (type(e).__name__, e))
        report["refused"] = str(e)
        _write(report)
        print("=== test_window_additivity: REFUSE ===")
        return 2
    finally:
        if raw_dir:
            shutil.rmtree(raw_dir, ignore_errors=True)
        if trace_dir:
            cleanup_traces(trace_dir)

    report["failures"] = failures
    report["ok"] = not failures
    _write(report)
    print("=== test_window_additivity: %s (%d failure%s) ==="
          % ("PASS" if not failures else "FAIL", len(failures),
             "" if len(failures) == 1 else "s"))
    return 0 if not failures else 1


def _write(report):
    report["log"] = log
    path = os.path.join(RESULT_DIR, "window_additivity.json")
    with open(path, "w") as f:
        json.dump(report, f, indent=1, sort_keys=True, default=str)
    print("artifact: %s" % path)


if __name__ == "__main__":
    sys.exit(main())
