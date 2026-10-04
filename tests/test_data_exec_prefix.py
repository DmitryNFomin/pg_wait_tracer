#!/usr/bin/env python3
"""test_data_exec_prefix.py — #274: the executions/exec_scatter pre-window
prefix must be read from the retained-marker index and produce EXACTLY the
answer the full pre-window decode produced.

WHAT CHANGED. `executions` and `exec_scatter` need every structural marker
from trace start to the window's left edge — a pre-window EXEC_START becomes
a `started_before_window` row, a pre-window CMD_END closes a zombie, and an
open execution can have started arbitrarily early, so a bounded look-back is
WRONG and is not what this is. They used to get those markers by LZ4-decoding
the whole pre-window prefix on EVERY request, so the same "last 15 minutes"
view got steadily slower as the capture aged (1398 ms at 98 s of prefix,
4104 ms at 4058 s, with the in-window volume flat). The markers are now
retained as each committed block is first decoded, once ever.

So the question this file answers is not "does it work" but "is it the SAME
ANSWER". Every case runs the identical request twice — once with the index
(default) and once with PGWT_EXEC_PREFIX_INDEX=0, which forces the old full
decode — and compares the whole JSON response.

THE FIXTURE IS BUILT TO MAKE THE PREFIX MATTER. It contains, all starting
before the compared window: a long-running execution that ends inside it
(started_before_window + a real end), a zombie closed by a pre-window
CMD_END, a zombie with no end at all, a nested pair (two EXEC_STARTs, one
EXEC_END), a plan completed before the window whose EXEC_START is inside it,
and a pid reused for a second statement after CMD_END. A fixture without
those is one the comparison cannot fail on, which is why section 1 asserts
the prefix is non-empty and that the window really does contain
started_before_window and in-progress rows.

SECTION 3 IS ABOUT FALSE NEGATIVES — every way this comparison could be
satisfied without comparing anything:
  * the two runs silently taking the SAME path (the comparison then has one
    side); pinned by reading pgwt-server's own `pgwt-prefix: path=` line and
    REFUSING when the paths are not index/legacy as intended;
  * a window with no prefix at all (from == trace start), where both paths
    read zero markers and any implementation passes; asserted to be empty on
    purpose so nobody promotes it to THE differential window;
  * the index never being built because retention is off — same as above,
    caught by the path line;
  * the index being INCOMPLETE rather than wrong: a block that could not be
    queued or an array that could not grow. Injected with
    PGWT_TEST_LOAD_ALLOC_FAIL=exec_mark_grow, and with a byte budget small
    enough to trip. In both cases the server must fall back and still return
    the exact answer — a marker index that cannot see everything must refuse,
    never answer short;
  * the first request being right while later ones drift, because the decode
    is incremental across requests; the same request is issued three times in
    one process and all three must match.

Runs anywhere pgwt-server runs. Synthetic traces, fixed timestamps, no clock,
no live PostgreSQL, no sleeps.
"""
import copy
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE,
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
MARKER_CMD_START  = 0xFFFFFFF6
MARKER_CMD_END    = 0xFFFFFFF7

# The compared window opens here — well after the prefix traffic below.
WIN_FROM = BASE + 60 * S
WIN_TO   = BASE + 90 * S


def marker(pid, ts, mk, qid=0):
    return {"pid": pid, "ts": ts, "dur": 0, "old": mk, "new": mk, "qid": qid}


def wait(pid, ts, dur, qid, ev=IO_DATA_FILE_READ):
    return {"pid": pid, "ts": ts, "dur": dur, "old": ev, "new": CPU,
            "qid": qid}


def build_scenario():
    """A prefix worth reading, then a window that depends on it.

    Everything before WIN_FROM is prefix. pgwt-server's two paths must agree
    on all of it.
    """
    ev = []

    # ── Bulk prefix: 400 ordinary completed executions across 40 pids, each
    # with its own PLAN/CMD markers. These are the markers whose decode cost
    # grew with capture age; none of them should reach the window's output.
    for i in range(400):
        pid = 2000 + (i % 40)
        qid = 500 + (i % 7)
        t = BASE + i * 100 * MS
        ev.append(marker(pid, t, MARKER_CMD_START, qid))
        ev.append(marker(pid, t + 1 * MS, MARKER_PLAN_START, qid))
        ev.append(marker(pid, t + 2 * MS, MARKER_PLAN_END, qid))
        ev.append(marker(pid, t + 3 * MS, MARKER_EXEC_START, qid))
        ev.append(wait(pid, t + 20 * MS, 10 * MS, qid))
        ev.append(marker(pid, t + 30 * MS, MARKER_EXEC_END, qid))
        ev.append(marker(pid, t + 31 * MS, MARKER_CMD_END, qid))

    # An escalation pair, so the index's other consumer is exercised too.
    ev.append(marker(0, BASE + 1 * S, MARKER_ESC_START, (60 << 8) | 1))
    ev.append(marker(0, BASE + 2 * S, MARKER_ESC_END, (60 << 8) | 2))

    # ── 1. A long-running execution: starts at BASE+10s, ends INSIDE the
    # window. Only the prefix carries its EXEC_START, so without it the row
    # is absent (or mispaired) entirely. This is what a bounded look-back
    # loses.
    ev.append(marker(3001, BASE + 10 * S, MARKER_CMD_START, 901))
    ev.append(marker(3001, BASE + 10 * S + MS, MARKER_EXEC_START, 901))
    ev.append(wait(3001, BASE + 30 * S, 5 * S, 901, LWLOCK_WAL_WRITE))
    ev.append(wait(3001, BASE + 70 * S, 5 * S, 901, LWLOCK_WAL_WRITE))
    ev.append(marker(3001, BASE + 75 * S, MARKER_EXEC_END, 901))
    ev.append(marker(3001, BASE + 75 * S + MS, MARKER_CMD_END, 901))

    # ── 2. A zombie closed by a PRE-WINDOW CMD_END: EXEC_START at +12s, no
    # EXEC_END, CMD_END at +40s. Both still before the window. If the prefix
    # CMD_END is lost the row stays in_progress and leaks into the window.
    ev.append(marker(3002, BASE + 12 * S, MARKER_EXEC_START, 902))
    ev.append(wait(3002, BASE + 20 * S, 1 * S, 902))
    ev.append(marker(3002, BASE + 40 * S, MARKER_CMD_END, 902))

    # ── 3. A zombie with NO end and NO CMD_END: must still be in_progress
    # inside the window, and must be attributed the window's waits.
    ev.append(marker(3003, BASE + 15 * S, MARKER_EXEC_START, 903))
    ev.append(wait(3003, BASE + 65 * S, 2 * S, 903))
    ev.append(wait(3003, BASE + 80 * S, 2 * S, 903))

    # ── 4. Nesting: two EXEC_STARTs before the window, one EXEC_END inside
    # it. The inner row closes; the outer stays open.
    ev.append(marker(3004, BASE + 18 * S, MARKER_EXEC_START, 904))
    ev.append(marker(3004, BASE + 19 * S, MARKER_EXEC_START, 905))
    ev.append(wait(3004, BASE + 66 * S, 1 * S, 905))
    ev.append(marker(3004, BASE + 70 * S, MARKER_EXEC_END, 905))
    ev.append(wait(3004, BASE + 78 * S, 1 * S, 904))

    # ── 5. plan_ready carried across the boundary: PLAN_END before the
    # window, the EXEC_START it belongs to inside it. The row must come back
    # with has_plan and the pre-window plan span.
    ev.append(marker(3005, BASE + 50 * S, MARKER_PLAN_START, 906))
    ev.append(marker(3005, BASE + 52 * S, MARKER_PLAN_END, 906))
    ev.append(marker(3005, BASE + 62 * S, MARKER_EXEC_START, 906))
    ev.append(wait(3005, BASE + 68 * S, 1 * S, 906))
    ev.append(marker(3005, BASE + 72 * S, MARKER_EXEC_END, 906))

    # ── 6. PID reuse: the same pid runs a second statement after CMD_END.
    ev.append(marker(3006, BASE + 20 * S, MARKER_EXEC_START, 907))
    ev.append(marker(3006, BASE + 25 * S, MARKER_CMD_END, 907))
    ev.append(marker(3006, BASE + 64 * S, MARKER_EXEC_START, 908))
    ev.append(wait(3006, BASE + 69 * S, 1 * S, 908))
    ev.append(marker(3006, BASE + 74 * S, MARKER_EXEC_END, 908))

    # Tail traffic so the window's right edge is not the file's last event.
    for i in range(20):
        t = BASE + 95 * S + i * 100 * MS
        ev.append(marker(2500, t, MARKER_EXEC_START, 600))
        ev.append(wait(2500, t + 20 * MS, 10 * MS, 600))
        ev.append(marker(2500, t + 30 * MS, MARKER_EXEC_END, 600))

    ev.sort(key=lambda e: e["ts"])

    pids = sorted({e["pid"] for e in ev if e["pid"] != 0})
    return {
        "backends": [{"pid": p, "type": "client", "user": "u", "db": "d"}
                     for p in pids],
        "queries": [{"id": q, "text": f"SELECT q{q}()"}
                    for q in sorted({e["qid"] for e in ev if e["qid"]})],
        "events": ev,
    }


def normalize(resp):
    """Drop the per-request id; everything else must match byte for byte."""
    r = copy.deepcopy(resp)
    r.pop("id", None)
    return r


def read_lines(stderr_path):
    """pgwt-server's own record of each prefix pass: which path served it,
    how many markers it read, and the retained-marker byte accounting."""
    out = []
    if not os.path.exists(stderr_path):
        return out
    with open(stderr_path) as f:
        for line in f:
            if line.startswith("pgwt-prefix: "):
                parts = dict(kv.split("=", 1)
                             for kv in line.split(None, 1)[1].split())
                out.append({
                    "path": parts.get("path"),
                    "markers": int(parts.get("markers", -1)),
                    "xm_bytes": int(parts.get("xm_bytes", -1)),
                    "xm_peak": int(parts.get("xm_peak", -1)),
                    "budget": int(parts.get("budget", -1)),
                })
    return out


def read_paths(stderr_path):
    """(path, markers) pairs — the shape most sections want."""
    return [(d["path"], d["markers"]) for d in read_lines(stderr_path)]


def run_requests(trace_dir, env, tmpdir, tag, requests):
    """Issue `requests` against one server process; return responses and the
    prefix-path lines pgwt-server emitted."""
    err = os.path.join(tmpdir, f"stderr-{tag}.txt")
    full_env = {"PGWT_EXEC_PREFIX_DEBUG": "1"}
    full_env.update(env or {})
    out = []
    with ServerHarness(trace_dir, env=full_env, stderr_path=err) as srv:
        for cmd, kw in requests:
            out.append(srv.query(cmd, **kw))
    run_requests.last_lines = read_lines(err)
    return out, read_paths(err)


WINDOWS = [
    ("mid",   WIN_FROM,            WIN_TO),
    ("late",  BASE + 70 * S,       BASE + 100 * S),
    ("short", BASE + 71 * S,       BASE + 73 * S),
    ("wide",  BASE + 30 * S,       BASE + 100 * S),
]

CASES = [(cmd, name, frm, to)
         for cmd in ("executions", "exec_scatter")
         for (name, frm, to) in WINDOWS]


def test_differential(t, trace_dir, tmpdir):
    print("\n### 1. index vs full decode: identical JSON, four windows ###")

    reqs = [(cmd, {"from_": frm, "to_": to}) for (cmd, _n, frm, to) in CASES]

    idx_resp, idx_paths = run_requests(trace_dir, {}, tmpdir, "index", reqs)
    leg_resp, leg_paths = run_requests(trace_dir, {"PGWT_EXEC_PREFIX_INDEX": "0"},
                                       tmpdir, "legacy", reqs)

    # The comparison is worthless if both sides took the same path.
    t.check_eq(len(idx_paths), len(reqs),
               "the index run reported a prefix path for every request")
    t.check_eq(len(leg_paths), len(reqs),
               "the legacy run reported a prefix path for every request")
    t.check(all(p == "index" for p, _ in idx_paths),
            f"index run really used the index (got {[p for p,_ in idx_paths]})")
    t.check(all(p == "legacy" for p, _ in leg_paths),
            f"legacy run really used the full decode "
            f"(got {[p for p,_ in leg_paths]})")
    t.check(all(n > 0 for _, n in idx_paths),
            f"the prefix is non-empty in every window (markers "
            f"{[n for _, n in idx_paths]}) — a zero prefix cannot fail")
    t.check_eq([n for _, n in idx_paths], [n for _, n in leg_paths],
               "both paths read the SAME number of prefix markers")

    for (cmd, name, _f, _to), a, b in zip(CASES, idx_resp, leg_resp):
        t.check(normalize(a) == normalize(b),
                f"{cmd}[{name}]: index output identical to the full decode")
        if normalize(a) != normalize(b):
            print("  index :", json.dumps(normalize(a))[:600])
            print("  legacy:", json.dumps(normalize(b))[:600])

    # The fixture has to exercise what the prefix is FOR.
    mid = idx_resp[0]
    rows = mid.get("rows", [])
    t.check(any(r.get("started_before_window") for r in rows),
            "the compared window has started_before_window rows")
    t.check(any(r.get("in_progress") for r in rows),
            "the compared window has an in-progress (zombie) row")
    t.check(any((r.get("plan_ms") or 0) > 0 for r in rows),
            "the compared window has a row whose plan completed pre-window")
    closed_pre = [r for r in rows if str(r.get("pid")) == "3002"]
    t.check(not closed_pre or all(not r.get("in_progress") for r in closed_pre),
            "the zombie closed by a PRE-WINDOW CMD_END does not leak in "
            "as still running")


def test_no_prefix_window_is_blind(t, trace_dir, tmpdir):
    print("\n### 2. a window starting at trace start has no prefix ###")
    # Both paths read zero markers here, so ANY implementation agrees. The
    # point of asserting it is that nobody later uses this window as the
    # differential and calls the result evidence.
    reqs = [("executions", {"from_": BASE - 10 * S, "to_": BASE + 100 * S})]
    _resp, paths = run_requests(trace_dir, {}, tmpdir, "noprefix", reqs)
    # `from` at or before the trace's first event: load_execution_rows does
    # not run a prefix pass at all, so there is no path line and nothing
    # either implementation could get wrong.
    t.check(all(n == 0 for _p, n in paths),
            f"zero prefix markers — this window cannot see the defect "
            f"(lines: {paths})")


def test_incremental_across_requests(t, trace_dir, tmpdir):
    print("\n### 3. the incremental decode is stable across requests ###")
    # The index is filled block by block as coverage refreshes, so a later
    # request must not see a different marker set from the first.
    reqs = [("executions", {"from_": WIN_FROM, "to_": WIN_TO})] * 3
    resp, paths = run_requests(trace_dir, {}, tmpdir, "repeat", reqs)
    t.check_eq([n for _, n in paths], [paths[0][1]] * 3,
               "all three requests read the same prefix marker count")
    t.check(normalize(resp[0]) == normalize(resp[1]) == normalize(resp[2]),
            "three identical requests give three identical answers")


def test_refusals(t, trace_dir, tmpdir):
    print("\n### 4. an index that cannot see everything REFUSES ###")
    reqs = [("executions", {"from_": WIN_FROM, "to_": WIN_TO}),
            ("exec_scatter", {"from_": WIN_FROM, "to_": WIN_TO})]

    truth, truth_paths = run_requests(
        trace_dir, {"PGWT_EXEC_PREFIX_INDEX": "0"}, tmpdir, "truth", reqs)
    t.check(all(p == "legacy" for p, _ in truth_paths),
            "the reference answers came from the full decode")

    # 4.1 The retained-marker array cannot grow.
    resp, paths = run_requests(
        trace_dir, {"PGWT_TEST_LOAD_ALLOC_FAIL": "exec_mark_grow"},
        tmpdir, "allocfail", reqs)
    t.check(all(p == "legacy" for p, _ in paths),
            f"allocation failure makes the index refuse "
            f"(got {[p for p, _ in paths]})")
    for (cmd, _kw), a, b in zip(reqs, resp, truth):
        t.check(normalize(a) == normalize(b),
                f"{cmd}: the refusal still returns the exact answer")

    # 4.2 The retained markers exceed their memory budget.
    resp, paths = run_requests(
        trace_dir, {"PGWT_EXEC_MARK_MAX_BYTES": "2048"},
        tmpdir, "budget", reqs)
    t.check(all(p == "legacy" for p, _ in paths),
            f"a budget the markers overflow makes the index refuse "
            f"(got {[p for p, _ in paths]})")
    for (cmd, _kw), a, b in zip(reqs, resp, truth):
        t.check(normalize(a) == normalize(b),
                f"{cmd}: the budget refusal still returns the exact answer")

    # 4.3 ...and a budget that is NOT exceeded must NOT refuse, or 4.2 would
    # pass for any value at all.
    resp, paths = run_requests(
        trace_dir, {"PGWT_EXEC_MARK_MAX_BYTES": str(64 * 1024 * 1024)},
        tmpdir, "budget_ok", reqs)
    t.check(all(p == "index" for p, _ in paths),
            f"a budget the markers fit in does NOT refuse "
            f"(got {[p for p, _ in paths]})")


def test_rotation(t, tmpdir):
    print("\n### 5. markers spanning two trace files ###")
    # Rotation is wall-clock top-of-hour: the prefix routinely lives in a
    # rotated, immutable file while the window lives in current.trace. The
    # index is per file and converts with that file's generation-canonical
    # offset, so a two-file dir is where getting that wrong shows up.
    trace_dir = tempfile.mkdtemp(prefix="pgwt_prefix_rot_", dir=tmpdir)
    sc = build_scenario()
    early = {k: v for k, v in sc.items()}
    early["events"] = [e for e in sc["events"] if e["ts"] < BASE + 55 * S]
    late = {k: v for k, v in sc.items()}
    late["events"] = [e for e in sc["events"] if e["ts"] >= BASE + 55 * S]

    generate_traces(early, output_dir=trace_dir, rotate="2025-01-01_10")
    generate_traces(late, output_dir=trace_dir)

    reqs = [("executions", {"from_": WIN_FROM, "to_": WIN_TO}),
            ("exec_scatter", {"from_": WIN_FROM, "to_": WIN_TO})]
    idx, idx_paths = run_requests(trace_dir, {}, tmpdir, "rot_index", reqs)
    leg, leg_paths = run_requests(trace_dir, {"PGWT_EXEC_PREFIX_INDEX": "0"},
                                  tmpdir, "rot_legacy", reqs)
    t.check(all(p == "index" for p, _ in idx_paths),
            "two-file dir: the index served the request")
    t.check(all(n > 0 for _, n in idx_paths),
            f"two-file dir: the prefix is non-empty "
            f"(markers {[n for _, n in idx_paths]})")
    for (cmd, _kw), a, b in zip(reqs, idx, leg):
        t.check(normalize(a) == normalize(b),
                f"{cmd} across a rotation: identical to the full decode")


def test_budget_released_on_rotation(t, tmpdir):
    print("\n### 6. the byte budget is RELEASED when a file is dropped ###")
    # The budget is a server-wide byte count. current.trace is rewritten
    # whenever the daemon rotates at the top of the hour or restarts, and the
    # coverage entry is reset — so the markers that file held are freed. If
    # the counter is not credited back, an hours-long capture refuses the
    # index for memory it no longer holds, which is exactly the capture
    # length the demo runs at. The budget here fits ONE copy of the
    # fixture's markers and not two.
    trace_dir = tempfile.mkdtemp(prefix="pgwt_prefix_budget_", dir=tmpdir)
    sc = build_scenario()
    generate_traces(sc, output_dir=trace_dir)

    err = os.path.join(tmpdir, "stderr-rotate.txt")
    env = dict(PGWT_EXEC_PREFIX_DEBUG="1",
               PGWT_EXEC_MARK_MAX_BYTES=str(150_000))
    kw = {"from_": WIN_FROM, "to_": WIN_TO}
    with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
        first = srv.query("executions", **kw)
        # Rotate: current.* is renamed to a timestamped immutable file,
        # exactly as src/event_writer.c does at the top of the hour. The old
        # coverage entry goes away and a new one is built for the renamed
        # file, so the SAME markers are retained under a different entry.
        # The byte count must end up where it started, not doubled.
        # Same renaming tests/gen_test_traces.c's --rotate does, which is
        # what src/event_writer.c does at the top of the hour.
        os.rename(os.path.join(trace_dir, "current.trace"),
                  os.path.join(trace_dir, "2025-01-01_11.trace.lz4"))
        os.unlink(os.path.join(trace_dir, "current.trace.meta"))
        if os.path.exists(os.path.join(trace_dir, "current.summary")):
            os.rename(os.path.join(trace_dir, "current.summary"),
                      os.path.join(trace_dir, "2025-01-01_11.summary.lz4"))
        if os.path.exists(os.path.join(trace_dir, "current.summary.meta")):
            os.unlink(os.path.join(trace_dir, "current.summary.meta"))
        second = srv.query("executions", **kw)
    paths = read_paths(err)

    t.check_eq(len(paths), 2, f"two prefix path lines (got {paths})")
    if len(paths) == 2:
        t.check_eq(paths[0][0], "index",
                   "the first request fits the budget and uses the index")
        t.check_eq(paths[1][0], "index",
                   "after the rotation the budget is credited back and "
                   "the index is still used")
        t.check_eq(paths[1][1], paths[0][1],
                   "the rebuilt index holds the same marker count")
    t.check(normalize(first) == normalize(second),
            "the answer is unchanged across the rotation")


def test_budget_is_a_real_bound(t, trace_dir, tmpdir):
    print("\n### 7. the budget bounds BYTES HELD, not just when it latches ###")
    # The check that matters is against the size the allocation will
    # actually REACH. Gating on "does one more marker fit" and then
    # DOUBLING the array is not a bound: with the budget nearly full the
    # doubling asks for roughly the whole budget again, and the process is
    # far past it before the next marker trips refusal. On an 8 GB box
    # shared with four PostgreSQL clusters and a 25%-of-RAM file cache,
    # that is not a rounding error.
    #
    # 30,000 bytes is deliberately NOT a multiple of the 1024-marker first
    # block (24,576 bytes), so the second growth must be CLAMPED to the
    # 5,424 bytes that remain rather than refused outright or taken in
    # full. That makes the three outcomes distinguishable:
    #   peak == 24,576  -> refused instead of clamping (a bound, but lossy)
    #   peak == 30,000  -> clamped exactly to the budget   (what we want)
    #   peak == 49,152  -> doubled past it                 (the defect)
    budget = 30_000
    first_block = 1024 * 24
    reqs = [("executions", {"from_": WIN_FROM, "to_": WIN_TO}),
            ("exec_scatter", {"from_": WIN_FROM, "to_": WIN_TO})]

    truth, _ = run_requests(trace_dir, {"PGWT_EXEC_PREFIX_INDEX": "0"},
                            tmpdir, "bound_truth", reqs)
    resp, _ = run_requests(trace_dir,
                           {"PGWT_EXEC_MARK_MAX_BYTES": str(budget)},
                           tmpdir, "bound", reqs)
    lines = run_requests.last_lines

    t.check(len(lines) == len(reqs),
            f"a prefix line per request (got {len(lines)})")
    t.check(all(d["budget"] == budget for d in lines),
            f"pgwt-server is using the budget under test "
            f"({[d['budget'] for d in lines]})")
    peak = max((d["xm_peak"] for d in lines), default=-1)
    t.check(peak >= 0, f"peak retained bytes were reported (got {peak})")
    # THE assertion: the bytes actually held never exceeded the budget.
    t.check(peak <= budget,
            f"peak retained-marker bytes {peak} <= budget {budget}")
    t.check(peak > first_block,
            f"the growth was CLAMPED to the remaining budget, not refused "
            f"at the first block (peak {peak} > {first_block})")
    t.check(all(d["path"] == "legacy" for d in lines),
            f"and once full the index refuses "
            f"({[d['path'] for d in lines]})")
    for (cmd, _kw), a, b in zip(reqs, resp, truth):
        t.check(normalize(a) == normalize(b),
                f"{cmd}: the bounded refusal still returns the exact answer")

    # A budget the markers fit in must stay under it too, and must NOT
    # refuse — otherwise the assertion above would hold for any budget.
    big = 64 * 1024 * 1024
    run_requests(trace_dir, {"PGWT_EXEC_MARK_MAX_BYTES": str(big)},
                 tmpdir, "bound_ok", reqs)
    ok = run_requests.last_lines
    t.check(all(d["path"] == "index" for d in ok),
            f"a budget the markers fit in does not refuse "
            f"({[d['path'] for d in ok]})")
    t.check(all(0 < d["xm_peak"] <= big for d in ok),
            f"...and still holds within it "
            f"({[d['xm_peak'] for d in ok]} <= {big})")


def main():
    t = TestRunner("exec prefix index (#274)")
    print(f"=== {t.name} ===")
    tmpdir = tempfile.mkdtemp(prefix="pgwt_prefix_")
    trace_dir = generate_traces(build_scenario())
    try:
        test_differential(t, trace_dir, tmpdir)
        test_no_prefix_window_is_blind(t, trace_dir, tmpdir)
        test_incremental_across_requests(t, trace_dir, tmpdir)
        test_refusals(t, trace_dir, tmpdir)
        test_rotation(t, tmpdir)
        test_budget_released_on_rotation(t, tmpdir)
        test_budget_is_a_real_bound(t, trace_dir, tmpdir)
    finally:
        cleanup_traces(trace_dir)
        cleanup_traces(tmpdir)
    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
