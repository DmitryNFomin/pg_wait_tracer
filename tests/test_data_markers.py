#!/usr/bin/env python3
"""test_data_markers.py — T1/FID-4: lifecycle + escalation markers must
never leak into wait accounting.

Markers (EXEC/PLAN_START/END, ESCALATE_START/END) are structural records:
duration 0, old_event = new_event = a 0xFFFFFFFx sentinel, and — for
escalation markers — a PGWT_ESC_PACK(secs, reason) payload in query_id.

Before the fix:
  * the summary writer accumulated markers (event_stream pushed them before
    its own PGWT_IS_MARKER check): exec/plan markers inflated per-query
    counts (skewing avg_us low) and escalation markers inserted bogus
    query_ids into summaries;
  * in raw compute only transitions/variants filtered markers — top_events,
    top_queries, heatmap, sessions, timeline did not. T0's live run showed
    the symptom directly: "Unknown:id=1677720x" rows in top_events.

The fix filters markers at two chokepoints (pgwt_filter_matches for all raw
compute; summary accum_event for summaries) while variants and the exec/plan
lifecycle stats — which legitimately CONSUME markers — keep working.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE = 10_000_000_000_000
S = 1_000_000_000
MS = 1_000_000

MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END   = 0xFFFFFFF1
MARKER_PLAN_START = 0xFFFFFFF2
MARKER_PLAN_END   = 0xFFFFFFF3
MARKER_ESC_START  = 0xFFFFFFF4
MARKER_ESC_END    = 0xFFFFFFF5

ESC_QID_START = (60 << 8) | 1   # PGWT_ESC_PACK(60, ANOMALY)
ESC_QID_END   = (60 << 8) | 2   # PGWT_ESC_PACK(60, EXPIRED)

# Marker event ids as they'd appear mislabeled in top_events:
# WE_EVENT(0xFFFFFFF0) = 16777200 .. 16777205 → "Unknown:id=1677720x"
PHANTOM_ID_LOW = 0xFFFFFFF0 & 0x00FFFFFF


def marker(pid, ts, mk, qid):
    return {"pid": pid, "ts": ts, "dur": 0, "old": mk, "new": mk, "qid": qid}


def build_scenario():
    """3 real IO waits of 10ms inside an exec span, plus every marker type."""
    events = [
        marker(0, BASE - 5 * MS, MARKER_ESC_START, ESC_QID_START),
        marker(1000, BASE - 2 * MS, MARKER_PLAN_START, 100),
        marker(1000, BASE - 1 * MS, MARKER_PLAN_END, 100),
        marker(1000, BASE, MARKER_EXEC_START, 100),
        {"pid": 1000, "ts": BASE + 10 * MS, "dur": 10 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        {"pid": 1000, "ts": BASE + 30 * MS, "dur": 10 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        {"pid": 1000, "ts": BASE + 50 * MS, "dur": 10 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        marker(1000, BASE + 60 * MS, MARKER_EXEC_END, 100),
        marker(0, BASE + 70 * MS, MARKER_ESC_END, ESC_QID_END),
    ]
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT io()"}],
        "events": events,
    }


def check_views(t, srv, label, frm=None, to=None):
    kw = {}
    if frm is not None:
        kw = {"from_": frm, "to_": to}

    ev = srv.query("top_events", **kw)
    phantom = [r for r in ev["rows"]
               if r["event_id"] >= 0xFFFFFFF0
               or "Unknown:id=1677720" in r["name"]]
    t.check(not phantom,
            f"[{label}] no phantom marker rows in top_events "
            f"(got: {[r['name'] for r in phantom]})")
    evrows = {r["name"]: r for r in ev["rows"]}
    io = evrows.get("IO:DataFileRead", {})
    t.check_eq(io.get("count"), 3, f"[{label}] IO count = 3 (not inflated)")
    t.check_approx(io.get("total_ms", -1), 30.0, 0.001,
                   f"[{label}] IO total = 30ms")
    t.check_approx(ev.get("db_time_ms", -1), 30.0, 0.001,
                   f"[{label}] db_time = 30ms (markers contribute nothing)")

    qr = srv.query("top_queries", **kw)
    qids = {r["query_id"] for r in qr["rows"]}
    t.check(str(ESC_QID_START) not in qids and str(ESC_QID_END) not in qids,
            f"[{label}] no phantom PGWT_ESC_PACK query_ids in top_queries")
    qrow = next((r for r in qr["rows"] if r["query_id"] == "100"), {})
    t.check_eq(qrow.get("count"), 3,
               f"[{label}] qid 100 wait count = 3 (markers don't inflate)")
    t.check_approx(qrow.get("avg_us", -1), 10_000.0, 0.001,
                   f"[{label}] qid 100 avg = 10ms (not skewed low)")


def test_raw_path(t):
    print("\n### 1. Raw compute path: markers filtered everywhere ###")
    trace_dir = generate_traces(build_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            check_views(t, srv, "raw")

            hm = srv.query("heatmap")
            t.check_eq(hm.get("total_events"), 3,
                       "heatmap counts only the 3 real waits")

            tl = srv.query("session_timeline")
            bad = [e for e in tl["events"] if e["e"] >= 0xFFFFFFF0]
            t.check(not bad, "session_timeline has no marker bars")

            fp = srv.query("fingerprints")
            frow = next((r for r in fp["rows"]
                         if str(r["query_id"]) == "100"), {})
            t.check_eq(frow.get("transitions"), 3,
                       "fingerprints transitions = 3 (marker hops excluded)")

            sess = srv.query("top_sessions")
            t.check(all(r["pid"] != 0 for r in sess["rows"]),
                    "no pid-0 (escalation marker) session row")

            # Consumers that legitimately need markers still work:
            va = srv.query("variants")
            t.check_eq(va.get("exec", {}).get("total"), 1,
                       "variants still sees the exec span (1 execution)")
            variant = va.get("exec", {}).get("variants", [{}])[0]
            t.check_eq(variant.get("query_text"), "SELECT io()",
                       "variants retains an unambiguous qid-only text preview")
            qr = srv.query("top_queries")
            qrow = next((r for r in qr["rows"]
                         if r["query_id"] == "100"), {})
            t.check_eq(qrow.get("exec_count"), 1,
                       "top_queries lifecycle exec_count still = 1")
            t.check_eq(qrow.get("plan_count"), 1,
                       "top_queries lifecycle plan_count still = 1")
    finally:
        cleanup_traces(trace_dir)


def test_summary_path(t):
    print("\n### 2. Summary path: markers never accumulated ###")
    trace_dir = generate_traces(build_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            # Force the summary fast path with a >= 120 s window around the
            # data (the trace is exact-only, so summaries stay authoritative).
            frm, to = BASE - 100 * S, BASE + 100 * S
            check_views(t, srv, "summary", frm=frm, to=to)
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_markers")
    print(f"=== {t.name} ===")
    test_raw_path(t)
    test_summary_path(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
