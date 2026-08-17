#!/usr/bin/env python3
"""test_data_esc_coverage.py — T1/FID-1/6/7: escalation markers are the
exact-wins coverage authority.

The rework's headline claim — "the exact-wins merge prevents double
counting" — was violated whenever a long wait spanned an escalation window:
transition timestamps are wait-END times, so a 60 s lock wait emits nothing
until it ends. Coverage derived from TRANSITIONS-block [first_ts, last_ts]
ranges therefore had a hole exactly over the wait, the interior samples
survived the merge, AND the exact 60 s interval landed → up to 2×.

The fix derives coverage from the PGWT_MARKER_ESCALATE_START/END markers the
daemon already writes:

  * closed window  [START_ts, END_ts]
  * START with no END (daemon crash / window still open): coverage extends to
    the last TRANSITIONS-block timestamp actually committed — never beyond.
  * END with no START (window opened before this file / older file deleted):
    coverage starts at the file's first block timestamp.
  * boundary rule (FID-6): a sample represents (ts − period, ts]; exact
    coverage is subtracted from that interval. Fully covered samples vanish;
    boundary-straddling samples retain only their uncovered weight. This is
    exact even when a delayed SMP-3 tick carries seconds of elapsed time.
  * files with transitions and no SAMPLES blocks (--mode full) are covered
    whole-file; files with samples+transitions but no markers anywhere fall
    back to per-block spans (legacy traces).
  * the merge runs in the monotonic clock domain (FID-7): per-file mono→wall
    offsets (NTP steps between file opens) cannot shift coverage against
    samples from another file of the same daemon run.

All scenarios use interleave=1 so the trace has the block structure the real
tiered daemon produces (transitions blocks split around sample blocks).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION,
)

BASE = 10_000_000_000_000   # 10000 s in ns
S = 1_000_000_000
PERIOD = 100_000_000        # 100 ms = 10 Hz

MARKER_ESC_START = 0xFFFFFFF4
MARKER_ESC_END   = 0xFFFFFFF5


def esc_pack(secs, reason):
    return (secs << 8) | reason


def esc_marker(ts, marker, secs=60, reason=0):
    return {"pid": 0, "ts": ts, "dur": 0, "old": marker, "new": marker,
            "qid": esc_pack(secs, reason)}


def sample_at(pid, ts, event, qid=0):
    return {"pid": pid, "ts": ts, "event": event, "qid": qid}


def sample_range(pid, start_ts, end_ts, event, qid=0):
    """Samples every PERIOD in (start_ts, end_ts] inclusive of both ends
    where they land on the grid."""
    out = []
    ts = start_ts
    while ts <= end_ts:
        out.append(sample_at(pid, ts, event, qid))
        ts += PERIOD
    return out


def uncovered_sample_ns(sample, spans, period=PERIOD):
    """Sample weight left after subtracting normalized exact spans."""
    start = sample["ts"] - period
    end = sample["ts"]
    covered = sum(max(0, min(end, e) - max(start, s)) for s, e in spans)
    return max(0, period - covered)


def dropped_by_rule(samples, spans):
    return sum(uncovered_sample_ns(smp, spans) == 0 for smp in samples)


def lock_ms(resp):
    rows = {r["name"]: r for r in resp["rows"]}
    return rows.get("Lock", {}).get("ms", 0.0)


# ── 1. FID-1: long wait spanning a whole escalation window ────────────

def test_long_wait_window(t):
    print("\n### 1. FID-1: 60s lock inside an escalation window (no 2x) ###")
    w0 = BASE + 1 * S
    w1 = w0 + 60 * S      # END marker ts

    events = [
        esc_marker(w0, MARKER_ESC_START, 60, 0),
        # The long wait: ONE transition at wait-END time, dur = 60 s.
        {"pid": 1000, "ts": w1, "dur": 60 * S,
         "old": LOCK_RELATION, "new": CPU, "qid": 100},
        esc_marker(w1 + 1000, MARKER_ESC_END, 60, 2),
    ]
    # Sampler runs through the window: pid 1000's lock is sampled the whole
    # time. These samples MUST be dropped (the exact interval covers them).
    in_window = sample_range(1000, w0 + PERIOD, w1, LOCK_RELATION, 100)
    # Samples after the window (sampled-only period) MUST survive.
    after = sample_range(1001, w1 + 5 * S, w1 + 10 * S, LOCK_RELATION, 200)
    samples = sorted(in_window + after, key=lambda s: s["ts"])

    n_after = len(after)
    expected_lock_ms = 60_000.0 + n_after * (PERIOD / 1e6)
    double_counted_ms = expected_lock_ms + len(in_window) * (PERIOD / 1e6)

    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"},
                     {"pid": 1001, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"},
                    {"id": 200, "text": "SELECT 2"}],
        "events": events,
        "sample_period_ns": PERIOD,
        "samples": samples,
        "interleave": 1,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            tm = srv.query("time_model")
            got = lock_ms(tm)
            t.check_approx(got, expected_lock_ms, 0.001,
                           f"Lock total = {expected_lock_ms:.0f}ms "
                           f"(60s exact + {n_after} post-window samples), "
                           f"NOT {double_counted_ms:.0f}ms (2x bug)")
            t.check_eq(tm.get("fidelity"), "mixed", "window fidelity = mixed")

            ev = srv.query("top_events")
            rows = {r["name"]: r for r in ev["rows"]}
            t.check_approx(rows.get("Lock:relation", {}).get("total_ms", -1),
                           expected_lock_ms, 0.001,
                           "top_events Lock:relation matches ground truth")
    finally:
        cleanup_traces(trace_dir)


# ── 2. FID-6: boundary rule at window edges ───────────────────────────

def test_boundary_rule(t):
    print("\n### 2. FID-6: exact sample clipping at window edges ###")
    w0 = BASE + 10 * S
    w1 = w0 + 10 * S

    events = [
        esc_marker(w0, MARKER_ESC_START, 10, 0),
        # One small exact interval inside the window (different class so the
        # sampled event's total is isolated).
        {"pid": 1000, "ts": w0 + 1 * S, "dur": 500_000_000,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        esc_marker(w1, MARKER_ESC_END, 10, 2),
    ]
    # Samples straddling both edges: from w0-2s to w1+2s.
    samples = sample_range(1001, w0 - 2 * S, w1 + 2 * S, LOCK_RELATION, 200)

    n_dropped = dropped_by_rule(samples, [(w0, w1)])
    n_kept = len(samples) - n_dropped
    expected_lock_ms = n_kept * (PERIOD / 1e6)

    # A sample AT w0 lies wholly before the window; the first whole interval
    # inside is dropped; the first interval after w1 is kept.
    t.check(dropped_by_rule([sample_at(1, w0, 0)], [(w0, w1)]) == 0,
            "rule: sample exactly at window start is kept (mass before w0)")
    t.check(dropped_by_rule([sample_at(1, w0 + PERIOD, 0)], [(w0, w1)]) == 1,
            "rule: first sample inside the window is dropped")
    t.check(dropped_by_rule([sample_at(1, w1 + PERIOD, 0)], [(w0, w1)]) == 0,
            "rule: first sample after window end is kept")

    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"},
                     {"pid": 1001, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "Q1"}, {"id": 200, "text": "Q2"}],
        "events": events,
        "sample_period_ns": PERIOD,
        "samples": samples,
        "interleave": 1,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            tm = srv.query("time_model")
            t.check_approx(lock_ms(tm), expected_lock_ms, 0.001,
                           f"Lock = {expected_lock_ms:.0f}ms "
                           f"({n_kept} samples kept after exact clipping)")
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_approx(rows.get("IO", {}).get("ms", -1), 500.0, 0.001,
                           "exact IO interval inside the window = 500ms")
    finally:
        cleanup_traces(trace_dir)


def test_delayed_tick_clipping(t):
    print("\n### 2b. SMP-3 delayed tick is clipped, not wholly dropped ###")
    w0 = BASE + 30 * S
    w1 = w0 + 4 * S
    # Every observation has its midpoint inside exact coverage, so the old
    # all-or-nothing rule dropped its entire elapsed weight. Exercise either
    # boundary and one tick spanning the whole exact window (two output
    # fragments).
    cases = [
        ("start", w0 + 3 * S, 5 * S),
        ("end", w1 + 2 * S, 5 * S),
        ("both", w1 + 2 * S, 8 * S),
    ]
    for edge, tick, delayed_period in cases:
        # Same PID as the exact interval exercises chronological merge order,
        # not just additive totals from unrelated sessions.
        sample = sample_at(1000, tick, LOCK_RELATION, 200)
        expected_lock_ns = uncovered_sample_ns(
            sample, [(w0, w1)], delayed_period)
        scenario = {
            "backends": [
                {"pid": 1000, "type": "client", "user": "u", "db": "d"},
                {"pid": 1001, "type": "client", "user": "u", "db": "d"},
            ],
            "queries": [{"id": 100, "text": "Q1"},
                        {"id": 200, "text": "Q2"}],
            "events": [
                esc_marker(w0, MARKER_ESC_START, 4, 0),
                {"pid": 1000, "ts": w1, "dur": 4 * S,
                 "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
                esc_marker(w1, MARKER_ESC_END, 4, 2),
            ],
            "sample_period_ns": delayed_period,
            "samples": [sample],
            "interleave": 1,
        }
        trace_dir = generate_traces(scenario)
        try:
            with ServerHarness(trace_dir) as srv:
                query_from = w0 - 3 * S
                query_to = w1 + 3 * S
                tm = srv.query("time_model", from_=query_from, to_=query_to)
                t.check_eq(tm.get("fidelity"), "mixed",
                           f"{edge}-straddling delayed tick stays mixed")
                t.check_approx(lock_ms(tm), expected_lock_ns / 1e6, 0.001,
                               f"{edge}-straddling delayed tick keeps exactly "
                               f"{expected_lock_ns / 1e6:.0f}ms uncovered")
                rows = {r["name"]: r for r in tm["rows"]}
                t.check_approx(rows.get("IO", {}).get("ms", -1),
                               4000.0, 0.001,
                               f"{edge}-straddling exact interval remains 4s")

                top_events = srv.query(
                    "top_events", from_=query_from, to_=query_to)
                event_rows = {r["name"]: r
                              for r in top_events.get("rows", [])}
                t.check_eq(event_rows.get("Lock:relation", {}).get("count"),
                           1, f"{edge}-straddling tick counts one observation")

                if edge == "both":
                    top_queries = srv.query(
                        "top_queries", from_=query_from, to_=query_to)
                    query_rows = {r["query_id"]: r
                                  for r in top_queries.get("rows", [])}
                    t.check_eq(query_rows.get("200", {}).get("count"), 1,
                               "two fragments count as one query observation")
                    t.check_approx(
                        query_rows.get("200", {}).get("total_ms", -1),
                        expected_lock_ns / 1e6, 0.001,
                        "two fragments conserve query sampled weight")

                    transitions = srv.query(
                        "transitions", from_=query_from, to_=query_to)
                    t.check_eq(transitions.get("total"), 1,
                               "mixed transition view ignores sample fragments")
                    fingerprints = srv.query(
                        "fingerprints", from_=query_from, to_=query_to)
                    fp_rows = {r["query_id"]: r
                               for r in fingerprints.get("rows", [])}
                    t.check_eq(sorted(fp_rows), [100],
                               "mixed fingerprints exclude sampled query")
                    t.check_eq(fp_rows.get(100, {}).get("transitions"), 1,
                               "mixed fingerprint counts only exact edge")

                    timeline = srv.query(
                        "session_timeline", from_=query_from, to_=query_to)
                    visible = timeline.get("events", [])
                    t.check_eq(len(visible), 3,
                               "same-PID split timeline has three ordered bars")
                    if len(visible) == 3:
                        expected = [
                            (w0 - 2 * S, 2 * S, LOCK_RELATION),
                            (w0, 4 * S, IO_DATA_FILE_READ),
                            (w1, 2 * S, LOCK_RELATION),
                        ]
                        actual = [(e["s"], e["d"], e["e"])
                                  for e in visible]
                        t.check_eq(actual, expected,
                                   "split fragments retain chronological order")

                    # The left slice clips a pre-window fragment at `from`;
                    # the right slice requires loading a delayed sample whose
                    # physical timestamp is AFTER `to`.
                    for side, narrow_from, narrow_to in [
                        ("left", w0 - S, w0 + S),
                        ("right", w1, w1 + S),
                    ]:
                        narrow = srv.query(
                            "top_events", from_=narrow_from, to_=narrow_to)
                        narrow_rows = {r["name"]: r
                                       for r in narrow.get("rows", [])}
                        lock = narrow_rows.get("Lock:relation", {})
                        t.check_approx(lock.get("total_ms", -1), 1000.0,
                                       0.001,
                                       f"{side} request slice clips delayed "
                                       "sample weight to 1s")
                        t.check_eq(lock.get("count"), 1,
                                   f"{side} request slice counts one sample")
        finally:
            cleanup_traces(trace_dir)


def test_lookahead_respects_load_bound(t):
    print("\n### 2c. delayed-sample lookahead excludes exact records ###")
    w0 = BASE + 50 * S
    w1 = w0 + 4 * S
    events = [
        esc_marker(w0, MARKER_ESC_START, 4, 0),
        {"pid": 1000, "ts": w1, "dur": 4 * S,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        esc_marker(w1, MARKER_ESC_END, 4, 2),
    ]
    # These records fall outside the tiny request but inside its 8s sample
    # lookahead. They must be rejected before PGWT_LOAD_MAX_EVENTS accounting.
    for i in range(10):
        events.append({
            "pid": 1000,
            "ts": w1 + 2 * S + i * 100_000_000,
            "dur": 50_000_000,
            "old": IO_DATA_FILE_READ,
            "new": CPU,
            "qid": 100,
        })
    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "Q1"},
                    {"id": 200, "text": "Q2"}],
        "events": events,
        "sample_period_ns": 8 * S,
        "samples": [sample_at(1000, w1 + 2 * S, LOCK_RELATION, 200)],
        "interleave": 1,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(
                trace_dir, env={"PGWT_LOAD_MAX_EVENTS": "5"}) as srv:
            resp = srv.query("top_events", from_=w1, to_=w1 + S)
            t.check("error" not in resp,
                    "lookahead-only exact records do not false-refuse range")
            rows = {r["name"]: r for r in resp.get("rows", [])}
            t.check_approx(rows.get("Lock:relation", {}).get("total_ms", -1),
                           1000.0, 0.001,
                           "lookahead still loads and clips overlapping sample")
    finally:
        cleanup_traces(trace_dir)


# ── 3. Crash mid-window: START with no END ────────────────────────────

def test_crash_mid_window(t):
    print("\n### 3. START with no END (crash): clamp to last exact data ###")
    w0 = BASE + 5 * S
    t_last = w0 + 10 * S   # last committed exact transition

    # Exact IO chain w0 .. t_last (100 intervals of 100ms)
    events = [esc_marker(w0, MARKER_ESC_START, 60, 1)]
    ts = w0 + PERIOD
    while ts <= t_last:
        events.append({"pid": 1000, "ts": ts, "dur": PERIOD,
                       "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
        ts += PERIOD
    # NO END marker — daemon crashed. Samples continue for 20 more seconds
    # (in reality the sampler died too, but data already on disk past t_last
    # must survive: beyond the last exact evidence, samples are the truth).
    samples = sample_range(1001, w0 + PERIOD, t_last + 20 * S,
                           LOCK_RELATION, 200)

    n_dropped = dropped_by_rule(samples, [(w0, t_last)])
    n_kept = len(samples) - n_dropped
    expected_lock_ms = n_kept * (PERIOD / 1e6)

    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"},
                     {"pid": 1001, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "Q1"}, {"id": 200, "text": "Q2"}],
        "events": events,
        "sample_period_ns": PERIOD,
        "samples": samples,
        "interleave": 1,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            tm = srv.query("time_model")
            t.check_approx(lock_ms(tm), expected_lock_ms, 0.001,
                           f"unclosed window clamps at last exact ts: "
                           f"Lock = {expected_lock_ms:.0f}ms ({n_kept} kept)")
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_approx(rows.get("IO", {}).get("ms", -1), 10_000.0, 0.001,
                           "exact IO chain = 10000ms")
    finally:
        cleanup_traces(trace_dir)


# ── 4. Multiple windows per file ──────────────────────────────────────

def test_multiple_windows(t):
    print("\n### 4. Two windows in one file; between-window samples survive ###")
    a0, a1 = BASE + 10 * S, BASE + 20 * S
    b0, b1 = BASE + 40 * S, BASE + 50 * S

    events = [
        esc_marker(a0, MARKER_ESC_START, 10, 0),
        {"pid": 1000, "ts": a1, "dur": 10 * S,
         "old": LOCK_RELATION, "new": CPU, "qid": 100},
        esc_marker(a1, MARKER_ESC_END, 10, 2),
        esc_marker(b0, MARKER_ESC_START, 10, 0),
        {"pid": 1000, "ts": b1, "dur": 10 * S,
         "old": LOCK_RELATION, "new": CPU, "qid": 100},
        esc_marker(b1, MARKER_ESC_END, 10, 2),
    ]
    # Continuous sampling of pid 1000's lock over the whole range.
    samples = sample_range(1000, a0 + PERIOD, b1 + 10 * S, LOCK_RELATION, 100)

    n_dropped = dropped_by_rule(samples, [(a0, a1), (b0, b1)])
    n_kept = len(samples) - n_dropped
    expected_lock_ms = 20_000.0 + n_kept * (PERIOD / 1e6)

    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "Q1"}],
        "events": events,
        "sample_period_ns": PERIOD,
        "samples": samples,
        "interleave": 1,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            tm = srv.query("time_model")
            t.check_approx(lock_ms(tm), expected_lock_ms, 0.001,
                           f"two windows: Lock = {expected_lock_ms:.0f}ms "
                           f"(2x10s exact + {n_kept} between/after samples)")
    finally:
        cleanup_traces(trace_dir)


# ── 5. FID-7: window spans rotation, per-file offsets disagree ────────

def test_cross_file_offsets(t):
    print("\n### 5. FID-7: rotation mid-window + NTP step between files ###")
    m0 = BASE + 10 * S        # START (file A)
    m_rot = m0 + 10 * S       # rotation boundary
    m1 = m0 + 20 * S          # END (file B)

    # File A (rotated hour): START marker, exact IO 5s, samples up to m_rot.
    # Its header gets a mono→wall offset of +5s (NTP stepped back 5s after
    # this file was opened).
    ev_a = [esc_marker(m0, MARKER_ESC_START, 20, 0)]
    ts = m0 + PERIOD
    while ts <= m0 + 5 * S:
        ev_a.append({"pid": 1000, "ts": ts, "dur": PERIOD,
                     "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
        ts += PERIOD
    smp_a = sample_range(1001, m0 + PERIOD, m_rot - PERIOD, LOCK_RELATION, 200)

    # File B (current): window continues — exact IO again, END marker at m1,
    # then a sampled-only tail. Offset 0.
    ev_b = []
    ts = m_rot + PERIOD
    while ts <= m_rot + 5 * S:
        ev_b.append({"pid": 1000, "ts": ts, "dur": PERIOD,
                     "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
        ts += PERIOD
    ev_b.append(esc_marker(m1, MARKER_ESC_END, 20, 2))
    smp_b = (sample_range(1001, m_rot, m1, LOCK_RELATION, 200) +
             sample_range(1001, m1 + PERIOD, m1 + 5 * S, LOCK_RELATION, 200))
    smp_b.sort(key=lambda s: s["ts"])

    # Ground truth in the MONO domain: lock-sample weight inside [m0, m1] is
    # covered by the window — regardless of its file or that file's wall
    # offset. File A's unclosed portion is closed by file B's END marker (same
    # clock domain). Grid alignment makes each sample wholly kept or dropped.
    all_samples = smp_a + smp_b
    n_dropped = dropped_by_rule(all_samples, [(m0, m1)])
    n_kept = len(all_samples) - n_dropped
    expected_lock_ms = n_kept * (PERIOD / 1e6)
    expected_io_ms = (len(ev_a) - 1 + len(ev_b) - 1) * (PERIOD / 1e6)

    backends = [{"pid": 1000, "type": "client", "user": "u", "db": "d"},
                {"pid": 1001, "type": "client", "user": "u", "db": "d"}]
    queries = [{"id": 100, "text": "Q1"}, {"id": 200, "text": "Q2"}]

    trace_dir = generate_traces(
        {"backends": backends, "queries": queries, "events": ev_a,
         "sample_period_ns": PERIOD, "samples": smp_a, "interleave": 1},
        wall_offset_ns=5 * S, rotate="2025-01-01_10")
    generate_traces(
        {"backends": backends, "queries": queries, "events": ev_b,
         "sample_period_ns": PERIOD, "samples": smp_b, "interleave": 1},
        output_dir=trace_dir, wall_offset_ns=0)

    try:
        with ServerHarness(trace_dir) as srv:
            tm = srv.query("time_model")
            t.check_approx(lock_ms(tm), expected_lock_ms, 0.001,
                           f"cross-file merge in mono domain: Lock = "
                           f"{expected_lock_ms:.0f}ms ({n_kept} samples kept)")
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_approx(rows.get("IO", {}).get("ms", -1),
                           expected_io_ms, 0.001,
                           f"exact IO across both files = {expected_io_ms:.0f}ms")

            # Re-anchored wall mapping: with the canonical (newest-file)
            # offset, wall == mono for the whole generation, so a query
            # sliced at the window end must see exactly the sampled tail.
            ev = srv.query("top_events",
                           from_=m1 + PERIOD // 2, to_=m1 + 6 * S)
            evrows = {r["name"]: r for r in ev["rows"]}
            # The first post-window sample represents [m1,m1+PERIOD]; this
            # request starts halfway through it, so only half its weight is in
            # range. The remaining tail samples contribute their full period.
            tail = [s for s in smp_b if s["ts"] > m1]
            expected_tail_ms = (len(tail) - 0.5) * (PERIOD / 1e6)
            t.check_approx(evrows.get("Lock:relation", {}).get("total_ms", -1),
                           expected_tail_ms, 0.001,
                           "post-window slice sees only the sampled tail "
                           "(consistent re-anchored wall mapping)")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_esc_coverage")
    print(f"=== {t.name} ===")
    test_long_wait_window(t)
    test_boundary_rule(t)
    test_delayed_tick_clipping(t)
    test_lookahead_respects_load_bound(t)
    test_crash_mid_window(t)
    test_multiple_windows(t)
    test_cross_file_offsets(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
