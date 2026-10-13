#!/usr/bin/env python3
"""test_data_tag_refusal.py — #363: a refused tagging pass must reach EVERY
consuming route of the raw loader, not just the JSON handlers.

WHAT THE C UNIT TEST CANNOT REACH. tests/test_tag_refusal.c drives
pgwt_tag_events directly and pins its 1/0 contract, including both failure
paths and the real per-pid bound. The CALL SITE is in
server_load_events_fi_mode (src/server.c), which no C unit can link (the
same limitation tests/test_server_alloc_tune.py exists for). Three things
are only observable there:

  1. the failure is routed into the loader's EXISTING info->allocation_failed
     flag, so it reaches BOTH consumers: reject_overload (every raw JSON
     handler) and dump_summary, which reads allocation_failed DIRECTLY. A
     new flag handled only inside reject_overload would have left `--dump`
     printing a full, plausible, partially-tagged report with nothing red —
     which is why --dump is its own case here and not an afterthought;
  2. the flag is SET, never assigned from the tagging result, so a failure
     from an EARLIER loader stage cannot be cleared by a later tagging
     success. `merge_initial` is exactly that case: the merge fails,
     allocation_failed is already 1, and the tagging pass that follows
     SUCCEEDS. A call site that wrote `allocation_failed = !ok` would serve
     that window as a success with a partial merged array;
  3. the array itself is discarded (NULL, count 0) and not merely flagged,
     so no consumer can read a partially-tagged array before it reads the
     flag.

NON-VACUITY. Every assertion here is satisfied by a server that errors on
everything, so each injected case is paired with a control on the same trace
and the same command, and the control must answer with real rows. The
injection points are NAMED, so an unrelated name must change nothing — that
is what shows the refusals came from the named allocation and not from the
variable merely being present in the server's environment.

NOT ASSERTED HERE, deliberately: `transitions`. It can answer from the
per-block aggregate without the raw loader, so its behaviour under a
tagging injection is a question about #357's fast-path gating, not about
this call site. tests/test_data_window_bound.py owns that assertion.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, SERVER_BIN, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE_TS = 10_000_000_000_000
NUM_PIDS = 4
CMDS_PER_PID = 20
MS = 1_000_000
QID = 4242

MARKER_CMD_START = 0xFFFFFFF6
MARKER_CMD_END = 0xFFFFFFF7

# The two injection points inside pgwt_tag_events (src/compute.c). Both
# allocations happen before the first record is read, so the refusal is
# independent of what the window contains.
TAG_POINTS = ("tag_fwd", "tag_ht")

# Raw-path commands that reach the event array through reject_overload.
# This is the list tests/test_data_window_bound.py already proves refuses on
# the other branch of the same function (the max_events bound), so a command
# that answered here would be a genuine hole and not a guess about routing.
RAW_CMDS = ("time_model", "aas", "top_events", "top_sessions", "top_queries",
            "heatmap", "session_timeline", "fingerprints", "lock_chains",
            "interference", "concurrency", "variants")


def build_scenario():
    """Commands with CMD_START/CMD_END markers around them, so the tagging
    pass has real work to do: the we==0 record AFTER each CMD_END is the one
    the pass rewrites to NONCMD_CPU, and the query_id 0 record inside each
    command is the one it back-fills. Markers are ordinary records whose
    old/new are the 0xFFFFFFFx sentinels (same shape as
    tests/test_data_markers.py)."""
    events = []
    for p in range(NUM_PIDS):
        pid = 1000 + p
        ts = BASE_TS + p * 1_000
        for _ in range(CMDS_PER_PID):
            events.append({"pid": pid, "ts": ts, "dur": 0,
                           "old": MARKER_CMD_START, "new": MARKER_CMD_START,
                           "qid": 0})
            ts += MS
            # In-command wait that closed before the query id was reported.
            events.append({"pid": pid, "ts": ts, "dur": MS,
                           "old": IO_DATA_FILE_READ, "new": CPU, "qid": 0})
            ts += MS
            # The record that reports the id (the back-fill's source).
            events.append({"pid": pid, "ts": ts, "dur": MS,
                           "old": IO_DATA_FILE_READ, "new": CPU, "qid": QID})
            ts += MS
            events.append({"pid": pid, "ts": ts, "dur": 0,
                           "old": MARKER_CMD_END, "new": MARKER_CMD_END,
                           "qid": 0})
            # Post-command CPU: majority-outside the command window, so the
            # tagged path relabels it and the untagged path counts it as CPU.
            ts += 6 * MS
            events.append({"pid": pid, "ts": ts, "dur": 4 * MS,
                           "old": CPU, "new": IO_DATA_FILE_READ, "qid": 0})
            ts += 2 * MS
    events.sort(key=lambda e: e["ts"])
    return {
        "backends": [{"pid": 1000 + p, "type": "client", "user": "u",
                      "db": "d"} for p in range(NUM_PIDS)],
        "queries": [{"id": QID, "text": "SELECT 1"}],
        "events": events,
    }


def run_dump(trace_dir, env=None):
    """pgwt-server --dump <dir> — the dump_summary route. It never goes
    through reject_overload; it reads linfo.allocation_failed itself."""
    e = dict(os.environ)
    if env:
        e.update({k: str(v) for k, v in env.items()})
    return subprocess.run([SERVER_BIN, "--dump", trace_dir],
                          capture_output=True, text=True, env=e, timeout=180)


def main():
    t = TestRunner("tag_refusal")
    trace_dir = generate_traces(build_scenario())
    try:
        # ── 0. Controls. Without these, every assertion below is satisfied
        # by a server that refuses everything unconditionally. ───────────
        with ServerHarness(trace_dir) as srv:
            for cmd in RAW_CMDS:
                resp = srv.query(cmd)
                t.check(resp.get("code") != "allocation_failed",
                        f"control: {cmd} does not report an allocation "
                        f"failure ({resp.get('code')})")
            rows = srv.query("time_model").get("rows") or []
            t.check(len(rows) > 0,
                    "control: time_model returns real rows, so the refusals "
                    "below are a CHANGE in behaviour and not the only thing "
                    "this trace can produce (%d rows)" % len(rows))

        ctl = run_dump(trace_dir)
        t.check(ctl.returncode == 0, "control: --dump exits 0")
        t.check("Time Model" in ctl.stdout,
                "control: --dump prints the summary report")
        t.check("memory allocation failed" not in ctl.stdout,
                "control: --dump reports no allocation failure")

        # ── 1. Each tagging failure point, through reject_overload. The two
        # points are separate cases: one is the per-record forward array,
        # the other the per-pid state table. ─────────────────────────────
        for point in TAG_POINTS:
            with ServerHarness(trace_dir,
                               env={"PGWT_TEST_LOAD_ALLOC_FAIL": point}) as srv:
                for cmd in RAW_CMDS:
                    resp = srv.query(cmd)
                    t.check_eq(resp.get("code"), "allocation_failed",
                               f"{point}: {cmd} refuses with the "
                               f"allocation_failed code")
                    t.check("rows" not in resp and "max_events" not in resp,
                            f"{point}: {cmd} carries neither partial data nor "
                            f"a range refusal — a tagging failure is not a "
                            f"window_too_large")

        # ── 2. The dump_summary route, its own case: it reads the flag
        # directly, so a per-route status check would have missed it. ────
        for point in TAG_POINTS:
            r = run_dump(trace_dir, {"PGWT_TEST_LOAD_ALLOC_FAIL": point})
            t.check("memory allocation failed" in r.stdout,
                    f"{point}: --dump says the allocation failed")
            t.check("Time Model" not in r.stdout,
                    f"{point}: --dump prints NO report — a partially tagged "
                    f"capture is never summarised")

        # ── 3. An earlier stage's failure survives a later tagging SUCCESS.
        # merge_initial fails in the merge; the tagging pass that follows it
        # completes normally. A call site that assigned the flag from the
        # tagging result would serve this window as a success. ───────────
        with ServerHarness(
                trace_dir,
                env={"PGWT_TEST_LOAD_ALLOC_FAIL": "merge_initial"}) as srv:
            resp = srv.query("time_model")
            t.check_eq(resp.get("code"), "allocation_failed",
                       "merge_initial: an earlier failure is NOT cleared by "
                       "the tagging pass that succeeds after it")
            t.check("rows" not in resp,
                    "merge_initial: no partial rows alongside the refusal")
        r = run_dump(trace_dir, {"PGWT_TEST_LOAD_ALLOC_FAIL": "merge_initial"})
        t.check("memory allocation failed" in r.stdout,
                "merge_initial: --dump refuses too")

        # ── 4. Bypass suite: the ways this check could pass without
        # checking anything. ─────────────────────────────────────────────

        # 4a. An injection name that does not exist must change NOTHING, on
        # BOTH routes. Otherwise sections 1-3 could be passing because the
        # variable was merely set in the server's environment.
        with ServerHarness(
                trace_dir,
                env={"PGWT_TEST_LOAD_ALLOC_FAIL": "tag_fwd_misspelled"}) as srv:
            resp = srv.query("time_model")
            t.check(resp.get("code") != "allocation_failed",
                    "an unknown injection point does not refuse: the "
                    "refusals above are name-matched")
            t.check(len(resp.get("rows") or []) > 0,
                    "and the window still answers with rows")
        r = run_dump(trace_dir,
                     {"PGWT_TEST_LOAD_ALLOC_FAIL": "tag_fwd_misspelled"})
        t.check("Time Model" in r.stdout,
                "an unknown injection point leaves --dump answering")

        # 4b. The refusal must be REPEATABLE and non-fatal: a refusal that
        # killed the server, or that only happened once because something
        # cached the half-tagged array, would read as a pass for every
        # later case in this file.
        with ServerHarness(trace_dir,
                           env={"PGWT_TEST_LOAD_ALLOC_FAIL": "tag_fwd"}) as srv:
            t.check_eq(srv.query("time_model").get("code"),
                       "allocation_failed", "tag_fwd: first request refuses")
            t.check_eq(srv.query("time_model").get("code"),
                       "allocation_failed",
                       "tag_fwd: the SECOND request refuses too — nothing "
                       "cached the partially tagged array")
            t.check("error" not in srv.query("info"),
                    "and the server still answers commands that do not load "
                    "the raw window")
    finally:
        cleanup_traces(trace_dir)

    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
