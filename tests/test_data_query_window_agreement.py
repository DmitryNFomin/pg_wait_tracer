#!/usr/bin/env python3
"""test_data_query_window_agreement.py — #684: ONE WINDOW, ONE BASIS, under a
query filter.

THE DEFECT THIS GATES
─────────────────────────────────────────────────────────────────────────────
A query drill over a window of 120 s or more used to show two different
measurements of one quantity on one screen:

  * Events (top_events) and Queries (top_queries) read RAW events, clipped to
    [from, to);
  * Time Model (handle_time_model) and AAS (handle_aas) read the per-second
    SUMMARY table, whose per-query rows carry the record's FULL duration at
    the second it closes in (src/summary_writer.c's
    `accum_query_add(acc, query_id, old_ev, 1, dur, dur)`).

So a wait crossing a window edge was counted whole on two tabs and clipped on
the other two, and nothing on the screen said which was right. #526 and #598
had already forced Events and Queries onto raw; #684 added the same
`req->filter.query_id == 0` conjunct to handle_aas and handle_time_model,
which is the behaviour this file pins.

WHAT IS COMPARED, AND WHY IT IS THE SAME QUANTITY
─────────────────────────────────────────────────────────────────────────────
The fixture gives query 777 nothing but Lock:relation waits — no CPU records
(no `old: 0` entry exists, and the raw CPU* row is built from such records,
never synthesised from inter-record gaps) and no idle events. For that query,
therefore, "DB Time in the window" is one number, and each tab must report it:

  time_model   db_time_ms
  aas          Sigma over buckets of Sigma class AAS x bucket seconds
  top_events   Sigma rows' total_ms
  top_queries  the query's row total_ms

The four come from four separate computes, so agreement here is a real
cross-check and not one number printed four times.

THE FIXTURE STRADDLES ON PURPOSE — TWO SHAPES, TWO DIRECTIONS
─────────────────────────────────────────────────────────────────────────────
One 100 s Lock:relation wait spans [t0+100 s, t0+200 s). The window shapes
below cut it in the two ways that matter, because a single shape would catch
only one sign of the error (the same lesson as test_summary_raw_parity.py's
WINDOWS table):

  interior_300s        [t0+150, t0+450): the record CLOSES inside the window
                       while starting before it. The summary table files all
                       100 s at second 200, which is in the window, so the
                       summary route OVER-reports by 50 s.
  close_outside_130s   [t0+60, t0+190): the record OVERLAPS the window for
                       90 s but closes after it. Its summary row sits at
                       second 200, outside, so the summary route sees nothing
                       of it at all and UNDER-reports by 90 s.

Each shape asserts (a) the exact raw-clipped total, (b) that all four tabs
report it, and (c) that it differs from what the summary table would have
said — so the fixture is known to be capable of exposing the defect rather
than merely consistent.

THE ROUTE IS MEASURED, NOT ASSUMED
─────────────────────────────────────────────────────────────────────────────
Each >=120 s EXACT window has current summary files. With
PGWT_LOAD_MAX_EVENTS=1, query-filtered and unfiltered requests must refuse
with window_too_large: summaries are off until rebuilt from the core.
The normal-budget checks require the nonempty, independently clipped totals.

Each refusal names the raw event limit and suggests narrowing the range.
Query filters do not cause raw loading while summaries are off. top_queries
also explains why no filter reduces its all-pids load.

WHERE THIS RUNS
─────────────────────────────────────────────────────────────────────────────
tests/run_all.sh's synthetic-data tier (step 2.5: needs the real pgwt-server
plus gen_test_traces), so every `make box-check` and the CI Linux job.
Named test_data_* so .github/workflows/ci.yml's glob picks it up.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (                                    # noqa: E402
    ServerHarness, generate_traces, cleanup_traces,
    make_event_id, CLASS_LOCK, TestRunner,
)

S = 1_000_000_000
MS = 1_000_000
BASE = 10_000_000_000_000          # second-aligned synthetic mono/wall base
LOCK_REL = make_event_id(CLASS_LOCK, 0)

QID = 777                          # the query under the drill
QPID = 2001
OTHER_QID = 888                    # background load, so the drill filters
OTHER_PID = 2002

# The straddler: one 100 s Lock:relation wait over [BASE+100s, BASE+200s).
# gen_test_traces files a record as "the `old` event lasted `dur` and ended at
# `ts`", so ts is its CLOSE.
STRADDLE_CLOSE_S = 200
STRADDLE_DUR_S = 100

# Interior waits for the same query: 1 s each, every 10 s, all inside
# interior_300s and all after close_outside_130s ends.
INTERIOR_SECS = list(range(210, 440, 10))

CMDS = ("time_model", "aas", "top_events", "top_queries")

# All window refusals name the limit; top_queries additionally explains
# that filters cannot reduce its all-pids load.
WINDOW_HINT_FRAGMENT = "the raw event limit was exceeded; narrow the time range"
ALL_PIDS_HINT_FRAGMENT = "no filter reduces this load"
HINT_FRAGMENT = {
    "time_model":  WINDOW_HINT_FRAGMENT,
    "aas":         WINDOW_HINT_FRAGMENT,
    "top_events":  WINDOW_HINT_FRAGMENT,
    "top_queries": ALL_PIDS_HINT_FRAGMENT,
}


def build_capture():
    """An EXACT capture (no samples) whose every window below is therefore
    summary-eligible, holding Lock:relation waits only."""
    ev = [{"pid": QPID, "ts": BASE + STRADDLE_CLOSE_S * S,
           "dur": STRADDLE_DUR_S * S,
           "old": LOCK_REL, "new": LOCK_REL, "qid": QID, "cpu": 0}]
    for sec in INTERIOR_SECS:
        ev.append({"pid": QPID, "ts": BASE + sec * S, "dur": S,
                   "old": LOCK_REL, "new": LOCK_REL, "qid": QID, "cpu": 0})
    # A second backend on a different query, so the filtered answer is a real
    # subset of the window and not the whole capture under another name.
    for sec in range(5, 460, 5):
        ev.append({"pid": OTHER_PID, "ts": BASE + sec * S, "dur": 500 * MS,
                   "old": LOCK_REL, "new": LOCK_REL, "qid": OTHER_QID,
                   "cpu": 0})
    return {
        "interleave": 1,
        "backends": [{"pid": p, "type": "client", "user": "u", "db": "d"}
                     for p in (QPID, OTHER_PID)],
        "queries": [{"id": QID, "text": "q777"},
                    {"id": OTHER_QID, "text": "q888"}],
        "events": sorted(ev, key=lambda e: e["ts"]),
    }


def _overlap_ms(start_s, end_s, frm_ns, to_ns):
    start_ns = BASE + start_s * S
    end_ns = BASE + end_s * S
    lo = max(start_ns, frm_ns)
    hi = min(end_ns, to_ns)
    return max(0, hi - lo) / 1e6


def expected_clipped_ms(frm, to):
    """Raw's answer: every Lock:relation record of QID, clipped to [frm, to)."""
    total = _overlap_ms(STRADDLE_CLOSE_S - STRADDLE_DUR_S, STRADDLE_CLOSE_S,
                        frm, to)
    for sec in INTERIOR_SECS:
        total += _overlap_ms(sec - 1, sec, frm, to)
    return total


def expected_summary_ms(frm, to):
    """What the per-query summary table would have said: a record's WHOLE
    duration, credited at the second it closes in, and nothing at all for a
    record closing outside the window."""
    total = 0.0
    if frm <= BASE + STRADDLE_CLOSE_S * S < to:
        total += STRADDLE_DUR_S * S / 1e6
    for sec in INTERIOR_SECS:
        if frm <= BASE + sec * S < to:
            total += S / 1e6
    return total


WINDOWS = {
    # name: (from_ns, to_ns) — both >= 120 s, so both are summary-eligible.
    "interior_300s":      (BASE + 150 * S, BASE + 450 * S),
    "close_outside_130s": (BASE + 60 * S,  BASE + 190 * S),
}


def measure(srv, frm, to, filters):
    """One number per tab, each from its own compute."""
    out = {}

    tm = srv.query("time_model", from_=frm, to_=to, filters=filters)
    _require_answer(tm, "time_model")
    out["time_model"] = tm["db_time_ms"]

    aas = srv.query("aas", from_=frm, to_=to, buckets=10, filters=filters)
    _require_answer(aas, "aas")
    bucket_ns = aas["bucket_ns"]
    aas_total = 0.0
    for b in aas["buckets"]:
        for key, val in b.items():
            # "t" is the bucket start; "offcpu" and "cat" are decompositions
            # OF the class AAS, not additional load (src/server.c's T8/T2
            # notes), so summing them would double-count.
            if key in ("t", "offcpu", "cat"):
                continue
            aas_total += val
    out["aas"] = aas_total * bucket_ns / 1e6

    ev = srv.query("top_events", from_=frm, to_=to, limit=50, filters=filters)
    _require_answer(ev, "top_events")
    out["top_events"] = sum(r["total_ms"] for r in ev["rows"])

    tq = srv.query("top_queries", from_=frm, to_=to, limit=50,
                   filters=filters)
    _require_answer(tq, "top_queries")
    out["top_queries"] = sum(r["total_ms"] for r in tq["rows"])

    return out


def _require_answer(resp, cmd):
    if "error" in resp:
        raise RuntimeError(
            f"{cmd} refused where an answer was required: {resp!r}")


def check_window(t, trace_dir, name, frm, to):
    clipped = expected_clipped_ms(frm, to)
    summarised = expected_summary_ms(frm, to)

    t.check((to - frm) >= 120 * S,
            f"{name}: the window is >= 120 s, i.e. summary-eligible at all "
            f"({(to - frm) / S:.0f} s)")
    # (c) above: a fixture whose two bases coincide could not fail.
    t.check(abs(clipped - summarised) > 1000.0,
            f"{name}: the raw-clipped total ({clipped:.3f} ms) and the "
            f"full-duration-at-close total ({summarised:.3f} ms) differ by "
            f"more than 1 s, so this window CAN expose #684")

    with ServerHarness(trace_dir) as srv:
        got = measure(srv, frm, to, {"query_id": QID})

    for cmd, val in sorted(got.items()):
        t.check_approx(val, clipped, 1e-7,
                       f"{name}/{cmd}: reports the raw-clipped total "
                       f"{clipped:.3f} ms (got {val:.3f})")
        t.check(abs(val - summarised) > 1000.0,
                f"{name}/{cmd}: is NOT the full-duration-at-close total "
                f"{summarised:.3f} ms (got {val:.3f})")

    spread = max(got.values()) - min(got.values())
    t.check(spread <= 0.001,
            f"{name}: all four tabs report ONE quantity for this window "
            f"(spread {spread:.6f} ms over {got!r})")


def check_route(t, trace_dir, name, frm, to):
    """The oracle: PGWT_LOAD_MAX_EVENTS=1 makes the raw path refuse and lets
    the summary path answer, so each command's ROUTE is measured rather than
    assumed."""
    with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
        for cmd in CMDS:
            resp = srv.query(cmd, from_=frm, to_=to, buckets=10, limit=50,
                             filters={"query_id": QID})
            t.check_eq(resp.get("code"), "window_too_large",
                       f"{name}/{cmd}: query-filtered, this window is on the "
                       f"RAW route (the oracle refuses) — before #684 aas "
                       f"and time_model answered here from summaries")
            hint = resp.get("hint") or ""
            t.check(WINDOW_HINT_FRAGMENT in hint and
                    "query filter" not in hint.lower() and
                    HINT_FRAGMENT[cmd] in hint,
                    f"{name}/{cmd}: the refusal carries this command's own "
                    f"arm, {HINT_FRAGMENT[cmd]!r} ({hint!r})")

        # Summaries are disabled even for an unfiltered EXACT long window.
        # The normal-budget checks above still require nonempty raw totals.
        for cmd in ("time_model", "aas"):
            resp = srv.query(cmd, from_=frm, to_=to, buckets=10)
            t.check_eq(resp.get("code"), "window_too_large",
                       f"{name}/{cmd}: unfiltered long window also reads raw")



def main():
    t = TestRunner("query_window_agreement")
    trace_dir = generate_traces(build_capture())
    try:
        for name, (frm, to) in sorted(WINDOWS.items()):
            check_window(t, trace_dir, name, frm, to)
            check_route(t, trace_dir, name, frm, to)
    finally:
        cleanup_traces(trace_dir)
    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
