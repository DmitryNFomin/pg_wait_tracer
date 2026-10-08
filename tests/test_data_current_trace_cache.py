#!/usr/bin/env python3
"""test_data_current_trace_cache.py — #283: the committed-block cache for current.trace.

Rotated `.trace.lz4` files were cached forever; `current.trace` — the file a
live window actually lives in for the first hour of every capture — was
explicitly not, so every request re-LZ4-decoded the whole window. #283 caches
the COMMITTED blocks of current.trace (the ones `current.trace.meta`'s
high-watermark publishes, which the writer never rewrites) and extends the
entry as new blocks commit.

Speed is not what this test is for. The hazard is a stale cache serving events
from a previous capture — the daemon truncates and rewrites current.trace on
restart — so the sections are, in order of what would hurt:

  1. ANSWERS DO NOT CHANGE. Byte-identical responses with the cache on and off,
     over thirteen windows (including a sliding one, the live shape) and every
     raw-event command. The toggle is PGWT_CURRENT_TRACE_CACHE=0.
  2. REPEATED REQUESTS DECODE NOTHING NEW. Five identical `executions` calls in
     one process: after the first, the committed-block decode count must not
     move. Wall times are printed as measurements, never asserted — a timing
     assertion here would be a flaky test on a shared runner.
  3. A GROWING TRACE STAYS CORRECT. Blocks commit between requests (the meta
     watermark advances, exactly as the daemon advances it); the later request
     must see the new events. A cache that never noticed new commits would be
     fast and wrong.
  4. RESTART SAFETY. current.trace is truncated and rewritten under a live
     server. Not one pre-restart event may survive, and the pre- and
     post-restart event sets are asserted non-empty and disjoint so the check
     cannot pass by both being empty.
  5. BYPASS SUITE. Every way sections 1-4 could be satisfied without looking at
     anything: a differential run where the cache was never consulted, data
     that lives only in a rotated file, a missing/garbage/over-long meta, a
     generator that did not run, a stats line that cannot be parsed, and the
     over-budget drop. A gate that cannot see must refuse, never approve.

WHAT THIS SUITE CANNOT GATE. The cached block descriptor carries the block's
TYPE as well as its sample period, and the type is used for exactly one thing:
whether to read one measured sample period past the window's right edge, so a
delayed tick whose interval overlaps `to` is available for clipping. Measured
2026-10-05: disabling that lookahead on BOTH paths leaves db_time_ms identical
to the nanosecond (18473.999978 and 18525.999978 at two right edges placed
mid-period), because gen_test_traces writes one NOMINAL period per file and the
lookahead only bites for an SMP-3 delayed tick whose measured elapsed exceeds
it. So a cached block that lost its type goes undetected here — not a weak
assertion, a fixture the generator cannot write. Anything that reaches it
through the sample PERIOD is gated: zeroing cc->blk[].sample_period_ns turns 38
of section 1b's 40 responses red and flips fidelity from mixed to exact.

Needs pgwt-server and tests/gen_test_traces built. No PG, no root, no network.
"""
import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server_harness
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, IO_DATA_FILE_WRITE, LWLOCK_WAL_WRITE,
    TIMEOUT_PG_SLEEP,
)

EXEC_START = 0xFFFFFFF0
EXEC_END   = 0xFFFFFFF1
PLAN_START = 0xFFFFFFF2
PLAN_END   = 0xFFFFFFF3

MS = 1_000_000
S  = 1_000_000_000
BASE = 10_000_000_000_000_000      # > 2^53: ns values travel as strings

STATS_ENV = {"PGWT_CURRENT_TRACE_CACHE_STATS": "1"}

# Every command that loads raw events for its window. The four summary-path
# commands (time_model/top_events/heatmap/top_sessions at wide windows) are in
# the list too: the fixture's window is narrow enough that they take the raw
# path, which is the path #283 changed.
RAW_COMMANDS = [
    "info", "aas", "time_model", "top_events", "top_sessions", "top_queries",
    "heatmap", "session_timeline", "executions", "exec_scatter", "transitions",
    "fingerprints", "concurrency", "lock_chains", "variants", "interference",
]

t = TestRunner("test_data_current_trace_cache")


# ── Fixture ──────────────────────────────────────────────────────────────────

def busy_scenario(base=BASE, pid_base=1000, qid_base=100, span_s=36,
                  pids=8, per_pid=4000):
    """A multi-block current.trace: enough events that the writer flushes
    several 4096-event blocks, with lifecycle markers so the execution views
    have rows and several wait classes so the class filters select something."""
    waits = [IO_DATA_FILE_READ, IO_DATA_FILE_WRITE, LWLOCK_WAL_WRITE,
             TIMEOUT_PG_SLEEP]
    step = span_s * S // per_pid
    events = []
    for p in range(pids):
        pid = pid_base + p
        qid = qid_base + (p % 3)
        for i in range(per_pid):
            ts = base + i * step + p * (step // (pids + 1))
            if i % 200 == 0:
                events.append({"pid": pid, "ts": ts - 2, "dur": 0,
                               "old": PLAN_START, "new": PLAN_START,
                               "qid": qid})
                events.append({"pid": pid, "ts": ts - 1, "dur": 0,
                               "old": PLAN_END, "new": PLAN_END, "qid": qid})
                events.append({"pid": pid, "ts": ts, "dur": 0,
                               "old": EXEC_START, "new": EXEC_START,
                               "qid": qid})
            elif i % 200 == 199:
                events.append({"pid": pid, "ts": ts, "dur": 0,
                               "old": EXEC_END, "new": EXEC_END, "qid": qid})
            else:
                w = waits[(i + p) % len(waits)]
                events.append({"pid": pid, "ts": ts,
                               "dur": (1 + (i % 7)) * MS,
                               "old": w if i % 2 else CPU,
                               "new": CPU if i % 2 else w,
                               "qid": qid,
                               "cpu": (i % 3) * 100_000})
    events.sort(key=lambda e: e["ts"])
    return {
        "cpu_measured": 1,
        "backends": [
            {"pid": pid_base + p,
             "type": "client" if p % 4 else "parallel_worker",
             "leader_pid": pid_base if p % 4 == 0 and p else 0,
             "user": "u%d" % (p % 2), "db": "d%d" % (p % 2)}
            for p in range(pids)
        ],
        "queries": [
            {"id": qid_base + i, "text": "SELECT scenario_%d()" % i}
            for i in range(3)
        ],
        "events": events,
    }


# The 32k-event fixture takes ~35 s to generate (a JSON scenario through
# gen_test_traces' own parser), and sections 1-3 all want the same one. Build
# it once; sections that MUTATE it (section 3 advances the meta watermark) get
# a file-level copy instead, which is cheap.
_BIG = None


def big_fixture():
    global _BIG
    if _BIG is None:
        _BIG = generate_traces(busy_scenario())
    return _BIG


def big_fixture_copy():
    src = big_fixture()
    dst = tempfile.mkdtemp(prefix="pgwt_cc_copy_")
    for name in os.listdir(src):
        sp = os.path.join(src, name)
        if os.path.isfile(sp):
            shutil.copyfile(sp, os.path.join(dst, name))
    return dst


def mixed_scenario(base=BASE, span_s=24, pids=4, per_pid=1500,
                   period_ns=100 * MS):
    """A fixture with BOTH block types, interleaved the way a tiered capture
    writes them: SAMPLES blocks land between TRANSITIONS blocks.

    Without this the suite is blind to half of #283. A SAMPLES record is
    normalized at load time (old_event = sampled event, duration_ns = the
    block's sample_period_ns) and then goes through the exact-wins merge, so a
    cache that lost a block's type or its sample period would answer wrongly
    for every sampled view and identically for every exact one. A
    transitions-only fixture cannot tell those apart.
    """
    sc = busy_scenario(base=base, span_s=span_s, pids=pids, per_pid=per_pid)
    waits = [IO_DATA_FILE_READ, LWLOCK_WAL_WRITE, CPU]
    samples = []
    ticks = span_s * S // period_ns
    for k in range(int(ticks)):
        ts = base + k * period_ns
        for p in range(pids):
            samples.append({"pid": 1000 + p, "ts": ts + p,
                            "event": waits[(k + p) % len(waits)],
                            "qid": 100 + (p % 3)})
    samples.sort(key=lambda x: x["ts"])
    sc["sample_period_ns"] = period_ns
    sc["samples"] = samples
    sc["interleave"] = 1
    return sc


# ── Reading the server's own view of its cache ───────────────────────────────

# `cap` is required, not optional: it is the RESIDENT event count against
# `events`, which is only what the shared budget is charged. A build that does
# not report it cannot be checked for the charged-vs-resident property, so the
# parser refuses rather than skipping that assertion.
STAT_KEYS = {"enabled", "lo", "blocks", "events", "cap", "decoded", "served",
             "resets"}

# src/server.c CUR_CACHE_GROW_EVENTS -- the fixed growth step. Resident must
# stay within one step of charged; under the doubling this replaced, resident
# was up to 2x charged and the documented MB/h figure was low by that much.
CUR_CACHE_GROW_EVENTS = 1024 * 1024


def read_curcache(stderr_path):
    """The LAST `curcache` line pgwt-server wrote, as a dict of ints.

    Returns None when there is nothing trustworthy to read: no file, no
    curcache line, a line missing a field, or a field that will not parse.
    None means "cannot see" and every caller must treat it as a failure.
    Returning zeros here instead would turn "the cache was never exercised"
    into "the cache decoded nothing", which is how a gate stops gating.
    """
    if not stderr_path or not os.path.exists(stderr_path):
        return None
    last = None
    with open(stderr_path, errors="replace") as f:
        for line in f:
            if "curcache" not in line:
                continue
            fields = {}
            for tok in line.split():
                if "=" not in tok:
                    continue
                k, v = tok.split("=", 1)
                try:
                    fields[k] = int(v)
                except ValueError:
                    return None
            if not STAT_KEYS <= set(fields):
                return None
            last = fields
    return last


class QuietRunner(TestRunner):
    """A TestRunner whose PASS/FAIL lines are swallowed. Used only where this
    suite tests its OWN guards: the bypass sections drive require_used with
    deliberately blind input and assert that it FAILS, and those expected
    failures must not look like real failures in the log."""

    def check(self, condition, msg):
        if condition:
            self.passed += 1
        else:
            self.failed += 1


# ── The #283 cache PROBE command ────────────────────────────────────────────
#
# This file verifies #283: that the already-cached prefix of current.trace is
# reused as the file grows. `transitions` used to be the probe for that, and it
# has STOPPED BEING A VALID ONE -- the paint-latency block aggregate answers
# `transitions` by merging per-block tables, so interior blocks are never read
# and the raw decoded-block cache is never consulted for them. A probe that
# does not reach the code under test cannot measure it.
#
# So the probe moves to `session_timeline`, which is still a raw-path command
# and still reads every committed block. What this file DEMANDS of #283 is
# unchanged -- same growth assertion, same restart assertion, same served/
# decoded comparisons, same cache-on/off controls at the same strength. Only
# the instrument moved, and only at the sites that measure the cache.
#
# The sites that compare ANSWERS across the cache toggle, and the bypass cases
# that assert require_used() REFUSES a run where the cache was not consulted,
# deliberately keep using `transitions`: they are not measuring the cache, and
# for the first group `transitions` is now the more interesting command
# because its decode hook is what reads current.trace through that cache.
#
# ONE SHAPE CHANGE, flagged rather than folded in: `transitions` publishes its
# count as `total`, `session_timeline` as `total_count`. The count_of() helper
# below exists precisely because mixing those two up once made three growth
# assertions read 0 -> 0 as agreement, so the field name is passed explicitly
# at every call rather than guessed.
PROBE_CMD = "session_timeline"
PROBE_COUNT = "total_count"


def count_of(resp, key):
    """Pull a count field, raising if the key is absent.

    `resp.get("total_count") or 0` silently turns a renamed protocol field into
    "zero events", and a growth check built on that passes while nothing grows.
    (It did, during development: `transitions` publishes `total`, not
    `total_count`, and three growth assertions read 0 -> 0 as agreement.)
    """
    if not isinstance(resp, dict) or key not in resp:
        raise AssertionError("response has no %r field (keys: %s)"
                             % (key, sorted(resp) if isinstance(resp, dict)
                                else type(resp)))
    return int(resp[key])


def require_used(tr, stats, msg):
    """Fail unless the cache actually answered at least one block.

    THE BLINDNESS GUARD. Every differential in section 1 is byte-identical when
    the cache was never consulted — data only in a rotated file, a window that
    selects no block, the feature absent from the binary entirely. Such a run
    is not evidence, so it fails here rather than passing silently.
    """
    if stats is None:
        tr.check(False, msg + " [no parseable curcache line: not observable]")
        return False
    ok = stats["served"] > 0 and stats["blocks"] > 0
    tr.check(ok, msg + " (blocks=%d served=%d decoded=%d)"
             % (stats["blocks"], stats["served"], stats["decoded"]))
    return ok


# ── meta-file helpers (the daemon's committed-block high-watermark) ───────────

def meta_path(trace_dir):
    return os.path.join(trace_dir, "current.trace.meta")


def read_meta(trace_dir):
    with open(meta_path(trace_dir)) as f:
        return int(f.read().strip().split()[0])


def write_meta(trace_dir, committed):
    """Publish a committed-block count the way the writer does: write a temp
    file and rename it over the real one (atomic on POSIX, src/event_writer.c
    write_typed_block)."""
    tmp = meta_path(trace_dir) + ".tmp"
    with open(tmp, "w") as f:
        f.write("%d\n" % committed)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, meta_path(trace_dir))


def fixture_window(base=BASE, span_s=36, pad_s=2):
    """A window that covers the whole fixture and stays narrow enough that the
    raw-event path runs. A multi-minute window would take the pre-aggregated
    summary path for time_model/top_events/heatmap/top_sessions, which #283
    does not touch — a differential run there would prove nothing."""
    return str(base - pad_s * S), str(base + (span_s + pad_s) * S)


def canonical_body(resp):
    """Like canonical(), but also without the request id — for comparing one
    answer against an answer from a different process, where the id counters do
    not line up."""
    if isinstance(resp, dict):
        resp = dict(resp)
        resp.pop("id", None)
    return canonical(resp)


def canonical(resp):
    """Response text for a byte-for-byte comparison, with the one genuinely
    time-dependent field removed. `info.now_ns` is the server's wall clock at
    the moment it answered, so it differs between any two processes; nothing
    else in any response is clock-derived (checked by this suite failing if it
    ever is — a new nondeterministic field shows up as a divergence)."""
    if isinstance(resp, dict):
        resp = dict(resp)
        resp.pop("now_ns", None)
    return json.dumps(resp, sort_keys=True)


def run_sequence(trace_dir, cache_on, stderr_path, steps, env=None):
    """Send `steps` [(cmd, kwargs), ...] to one server process; return the
    canonical response lines. Both sides of a differential send the identical
    sequence in the identical order, so request ids line up and the lines are
    comparable verbatim."""
    full_env = dict(STATS_ENV)
    full_env["PGWT_CURRENT_TRACE_CACHE"] = "1" if cache_on else "0"
    if env:
        full_env.update(env)
    out = []
    with ServerHarness(trace_dir, env=full_env,
                       stderr_path=stderr_path) as srv:
        for cmd, kwargs in steps:
            out.append(canonical(srv.query(cmd, **kwargs)))
    return out


def window_steps(trace_dir, stderr_path):
    """The command/window matrix: a sliding window (the live shape, which is
    what makes the cache extend forward and then serve) plus zoomed and
    filtered variants, against every raw-event command."""
    with ServerHarness(trace_dir, env=STATS_ENV,
                       stderr_path=stderr_path) as srv:
        info = srv.query("info")
    lo, hi = int(info["from_ns"]), int(info["to_ns"])
    span = hi - lo
    windows = []
    # A window walking right in eight steps: each step overlaps the previous
    # one heavily, which is exactly the refresh pattern #283 exists for.
    for k in range(8):
        a = lo + k * span // 16
        windows.append((a, a + span // 2))
    windows += [
        (lo, hi),                                   # everything
        (lo, lo + span // 4),                       # left quarter
        (hi - span // 4, hi),                       # right quarter
        (lo + span // 3, lo + span // 3 + span // 8),
        (hi + span, hi + 2 * span),                 # past the end: empty
    ]
    steps = []
    for (a, b) in windows:
        for cmd in RAW_COMMANDS:
            steps.append((cmd, {"from_": str(a), "to_": str(b)}))
        steps.append(("top_events",
                      {"from_": str(a), "to_": str(b),
                       "filters": {"class": "io"}}))
        steps.append(("executions",
                      {"from_": str(a), "to_": str(b),
                       "filters": {"pid": 1003}}))
        steps.append(("transitions",
                      {"from_": str(a), "to_": str(b),
                       "filters": {"pid": 1000}}))
    return steps


# ── 1. Answers do not change ────────────────────────────────────────────────

def section_differential(tr):
    print("\n### 1. cache on vs off: byte-identical responses ###")
    trace_dir = big_fixture()
    try:
        tr.check(os.path.exists(meta_path(trace_dir)),
                 "fixture publishes a current.trace.meta watermark")
        tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
        try:
            steps = window_steps(trace_dir, os.path.join(tmp, "probe.err"))
            on_err = os.path.join(tmp, "on.err")
            off_err = os.path.join(tmp, "off.err")
            on = run_sequence(trace_dir, True, on_err, steps)
            off = run_sequence(trace_dir, False, off_err, steps)

            tr.check_eq(len(on), len(steps), "every request answered (cache on)")
            tr.check_eq(len(off), len(steps), "every request answered (cache off)")
            diffs = [i for i, (a, b) in enumerate(zip(on, off)) if a != b]
            if diffs:
                i = diffs[0]
                print("    first divergence at step %d (%s %s)"
                      % (i, steps[i][0], steps[i][1]))
                print("    cache on : %s" % on[i][:400])
                print("    cache off: %s" % off[i][:400])
            tr.check_eq(len(diffs), 0,
                        "all %d responses byte-identical across the toggle"
                        % len(steps))

            on_stats = read_curcache(on_err)
            off_stats = read_curcache(off_err)
            require_used(tr, on_stats,
                         "the cache-on run actually served cached blocks")
            if off_stats is not None:
                tr.check_eq(off_stats["enabled"], 0,
                            "PGWT_CURRENT_TRACE_CACHE=0 reports the cache off")
                tr.check_eq(off_stats["served"], 0,
                            "the cache-off run served nothing from cache")
            else:
                tr.check(False, "cache-off run emitted no curcache line")

            # Non-empty answers: a differential over a trace that produced no
            # rows anywhere would also be byte-identical.
            payload = sum(len(line) for line in on)
            tr.check(payload > 50_000,
                     "responses carry real payload (%d bytes)" % payload)
            if on_stats:
                # Charged vs resident on a full-size entry. A doubling from
                # 16384 would land cap at 32768 against 32320 charged here, so
                # this fixture is too small to catch it on its own -- the
                # tight-budget case in the bypass suite is the one with teeth.
                # Stated rather than implied, so nobody reads this line as the
                # proof it is not.
                tr.check(on_stats["cap"] - on_stats["events"]
                         <= CUR_CACHE_GROW_EVENTS,
                         "resident within one growth step of charged "
                         "(cap=%d events=%d)"
                         % (on_stats["cap"], on_stats["events"]))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    finally:
        pass   # the shared fixture is removed in main()


# ── 1b. ...including for a trace with SAMPLES blocks interleaved ────────────

def section_differential_mixed(tr):
    print("\n### 1b. mixed exact+sampled trace: byte-identical responses ###")
    trace_dir = generate_traces(mixed_scenario())
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        w_from, w_to = fixture_window(span_s=24)
        steps = []
        for cmd in RAW_COMMANDS:
            steps.append((cmd, {"from_": w_from, "to_": w_to}))
        # A few narrower windows: a sample interval that overlaps the right
        # edge is read one sample period past `to` and then clipped, which is
        # the one place block-level metadata (sample_period_ns, block type)
        # changes the answer.
        base = int(w_from)
        span = int(w_to) - base
        for k in range(4):
            a = base + k * span // 8
            for cmd in ("aas", "time_model", "top_events", "transitions",
                        "concurrency", "top_sessions"):
                steps.append((cmd, {"from_": str(a),
                                    "to_": str(a + span // 3)}))
        err_on = os.path.join(tmp, "mix_on.err")
        on = run_sequence(trace_dir, True, err_on, steps)
        off = run_sequence(trace_dir, False, os.path.join(tmp, "mix_off.err"),
                           steps)
        diffs = [i for i, (a, b) in enumerate(zip(on, off)) if a != b]
        if diffs:
            i = diffs[0]
            print("    first divergence at step %d (%s %s)"
                  % (i, steps[i][0], steps[i][1]))
            print("    cache on : %s" % on[i][:400])
            print("    cache off: %s" % off[i][:400])
        tr.check_eq(len(diffs), 0,
                    "all %d mixed-fidelity responses byte-identical across "
                    "the toggle" % len(steps))
        require_used(tr, read_curcache(err_on),
                     "the mixed-fidelity run served cached blocks")
        # The fixture must actually CONTAIN both block types, or this section
        # is the transitions-only one again under a different name.
        with ServerHarness(trace_dir, env=STATS_ENV) as srv:
            tm = srv.query("time_model", from_=w_from, to_=w_to)
        tr.check(tm.get("fidelity") in ("mixed", "sampled"),
                 "the fixture really is sampled/mixed (fidelity=%r)"
                 % tm.get("fidelity"))
        tr.check(tm.get("sample_period_ns") is not None,
                 "a sample period reached the response, so SAMPLES blocks "
                 "contributed")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


# ── 1c. The second read of a window is the same answer as the first ─────────

def section_self_consistency(tr):
    """Within ONE process, read each window twice and require the two answers
    to be identical.

    The first read decodes the blocks; the second serves them from the cache.
    So this is the structural form of "the cache changes nothing": any
    per-block metadata the entry failed to carry — the block TYPE, the sample
    period, the records themselves — diverges here, and it does not depend on
    the cache-off differential's windows happening to exercise the difference.
    The right edges are placed deliberately mid-sample-period, because that is
    where the block type matters: a delayed tick whose interval overlaps `to`
    is only read when the block is known to be a SAMPLES block.
    """
    print("\n### 1c. second read of a window == first read ###")
    trace_dir = generate_traces(mixed_scenario())
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        err = os.path.join(tmp, "self.err")
        env = dict(STATS_ENV)
        env["PGWT_CURRENT_TRACE_CACHE"] = "1"
        period = 100 * MS
        windows = []
        for k in (40, 77, 123, 180):
            # right edge half a sample period past a tick, so a tick lands in
            # (to, to + period] and the lookahead decides whether it is read
            windows.append((BASE - 1 * S, BASE + k * period + period // 2))
        cmds = ["aas", "time_model", "top_events", "top_sessions",
                "transitions", "concurrency", "executions", "exec_scatter",
                "heatmap", "top_queries"]
        diverged = []
        with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
            for (a, b) in windows:
                for cmd in cmds:
                    first = canonical_body(
                        srv.query(cmd, from_=str(a), to_=str(b)))
                    second = canonical_body(
                        srv.query(cmd, from_=str(a), to_=str(b)))
                    if first != second:
                        diverged.append((cmd, a, b, first, second))
        if diverged:
            cmd, a, b, first, second = diverged[0]
            print("    first divergence: %s [%d, %d]" % (cmd, a, b))
            print("    read 1 (decoded): %s" % first[:400])
            print("    read 2 (cached) : %s" % second[:400])
        tr.check_eq(len(diverged), 0,
                    "all %d windows answer identically on the cached re-read"
                    % (len(windows) * len(cmds)))
        st = read_curcache(err)
        require_used(tr, st, "the re-reads were actually served from cache")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


# ── 2. Repeated identical requests decode nothing new ───────────────────────

def section_repeat(tr):
    print("\n### 2. five identical requests: no committed block decoded twice ###")
    trace_dir = big_fixture()
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        results = {}
        for cache_on in (False, True):
            err = os.path.join(tmp, "rep_%s.err" % cache_on)
            env = dict(STATS_ENV)
            env["PGWT_CURRENT_TRACE_CACHE"] = "1" if cache_on else "0"
            decoded = []
            served = []
            times = []
            counts = []
            with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
                info = srv.query("info")
                lo, hi = int(info["from_ns"]), int(info["to_ns"])
                for _ in range(5):
                    t0 = time.monotonic()
                    resp = srv.query("executions", from_=str(lo), to_=str(hi))
                    times.append(time.monotonic() - t0)
                    counts.append(resp.get("total_count"))
                    st = read_curcache(err)
                    decoded.append(None if st is None else st["decoded"])
                    served.append(None if st is None else st["served"])
            results[cache_on] = (decoded, served, times, counts)
            print("    cache %-3s decoded=%s served=%s rows=%s ms=%s"
                  % ("on" if cache_on else "off", decoded, served, counts,
                     ["%.0f" % (x * 1000) for x in times]))

        d_on, s_on, t_on, c_on = results[True]
        d_off, s_off, t_off, c_off = results[False]

        tr.check(all(c is not None and c > 0 for c in c_on),
                 "the repeated request returns rows (not an empty window)")
        tr.check_eq(c_on, c_off,
                    "the same five requests return the same row count "
                    "with the cache on and off")
        tr.check(None not in d_on, "cache-on decode counter is observable")
        if None not in d_on:
            tr.check(d_on[-1] == d_on[0],
                     "requests 2-5 decoded no further committed block "
                     "(decoded stayed %s)" % d_on[0])
            tr.check(s_on[-1] > s_on[0],
                     "requests 2-5 were served from the cache "
                     "(served %d -> %d)" % (s_on[0], s_on[-1]))
        if None not in d_off:
            tr.check_eq(d_off, [0, 0, 0, 0, 0],
                        "with the cache off nothing is retained at all")
        # Wall times are reported, never asserted: this suite runs on shared
        # machines and a latency assertion here would be runner noise.
        print("    median ms: on=%.0f off=%.0f (reported, not asserted)"
              % (sorted(t_on)[2] * 1000, sorted(t_off)[2] * 1000))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ── 3. A growing trace stays correct ────────────────────────────────────────

def section_growth(tr):
    print("\n### 3. blocks committing between requests ###")
    trace_dir = big_fixture_copy()   # this section rewrites the meta watermark
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        full = read_meta(trace_dir)
        tr.check(full >= 6,
                 "fixture has enough committed blocks to grow through (%d)"
                 % full)
        half = full // 2
        err = os.path.join(tmp, "grow.err")
        env = dict(STATS_ENV)
        env["PGWT_CURRENT_TRACE_CACHE"] = "1"

        # Roll the watermark back: the blocks past it are bytes on disk the
        # daemon has not published yet, which is precisely the live state.
        write_meta(trace_dir, half)
        w_from, w_to = fixture_window()
        seen = []
        stats = []
        with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
            for committed in (half, half + 1, full):
                write_meta(trace_dir, committed)
                resp = srv.query(PROBE_CMD, from_=w_from, to_=w_to)
                seen.append(count_of(resp, PROBE_COUNT))
                stats.append(read_curcache(err))
                print("    committed=%d %s=%d curcache=%s"
                      % (committed, PROBE_CMD, seen[-1], stats[-1]))

        tr.check(seen[0] > 0, "the first (half-committed) request sees events")
        tr.check(seen[1] > seen[0],
                 "one more committed block adds events (%d -> %d)"
                 % (seen[0], seen[1]))
        tr.check(seen[2] > seen[1],
                 "the rest of the blocks add more (%d -> %d)"
                 % (seen[1], seen[2]))
        if all(s is not None for s in stats):
            tr.check(stats[2]["blocks"] > stats[0]["blocks"],
                     "the cache entry extended rather than being rebuilt "
                     "(%d -> %d blocks)"
                     % (stats[0]["blocks"], stats[2]["blocks"]))
            tr.check_eq(stats[2]["resets"], stats[0]["resets"],
                        "growth never dropped the entry")
            tr.check(stats[2]["served"] > stats[0]["served"],
                     "the already-cached prefix was reused across the growth")
        else:
            tr.check(False, "curcache stats unreadable during growth")

        # The uncached control must agree on every count, or "it saw the new
        # events" could still be the wrong number of them.
        off = []
        with ServerHarness(trace_dir, env={"PGWT_CURRENT_TRACE_CACHE": "0"}) \
                as srv:
            resp = srv.query(PROBE_CMD, from_=w_from, to_=w_to)
            off.append(count_of(resp, PROBE_COUNT))
        tr.check_eq(seen[2], off[0],
                    "the grown cache's final count matches the uncached read")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


# ── 3b. A window that jumps PAST the cached run ─────────────────────────────

def section_forward_gap(tr):
    """The window jumps forward past the end of the retained run, leaving a gap.

    This is the live case, not a corner case: a 120 s window whose tab stopped
    refreshing for longer than the window is wide -- the display slept, the
    presenter talked -- then refreshed. The first block it now needs is past
    `lo_block + n_blocks`, so the run can neither serve it nor extend to it.

    Declining to store without restarting is the trap. The entry then holds up
    to a full hour of events that no request can ever reach, and because
    cache_total_events() counts it, those events evict rotated-file entries as
    well -- that session is strictly WORSE than before #283. So the gap must
    restart the run at the block being read, exactly as the over-budget path
    does.

    The assertions are on the server's own counters rather than on latency:
    after the jump the run must START somewhere past the old run's end (which
    is what proves a gap was there to begin with -- without that the test could
    pass on an overlapping window and prove nothing), and the repeat of the
    jumped-to window must then be served without decoding anything new.
    """
    print("\n### 3b. the window jumps forward past the cached run ###")
    trace_dir = big_fixture()
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        err = os.path.join(tmp, "gap.err")
        env = dict(STATS_ENV)
        env["PGWT_CURRENT_TRACE_CACHE"] = "1"
        # Early and late slices of the same capture, far enough apart that the
        # late one's first block is past the end of the early one's run.
        early = (BASE - 1 * S, BASE + 4 * S)
        late = (BASE + 31 * S, BASE + 37 * S)
        with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
            a_resp = srv.query(PROBE_CMD, from_=str(early[0]),
                               to_=str(early[1]))
            a = read_curcache(err)
            b_resp = srv.query(PROBE_CMD, from_=str(late[0]),
                               to_=str(late[1]))
            b = read_curcache(err)
            c_resp = srv.query(PROBE_CMD, from_=str(late[0]),
                               to_=str(late[1]))
            c = read_curcache(err)
        print("    early %s" % a)
        print("    jump  %s" % b)
        print("    again %s" % c)
        if not (a and b and c):
            tr.check(False, "curcache stats unreadable across the jump")
            return
        tr.check(count_of(a_resp, PROBE_COUNT) > 0,
                 "the early window saw events (%d)"
                 % count_of(a_resp, PROBE_COUNT))
        tr.check(count_of(b_resp, PROBE_COUNT) > 0,
                 "the late window saw events (%d)"
                 % count_of(b_resp, PROBE_COUNT))
        # The gap is real: the run now starts past where the old one ended.
        tr.check(b["lo"] > a["lo"] + a["blocks"],
                 "the jumped-to window's first block (%d) really is past the "
                 "old run's end (%d) -- there was a gap to restart over"
                 % (b["lo"], a["lo"] + a["blocks"]))
        # `blocks > 0` would be true of a FROZEN entry too, so the assertion is
        # that the run MOVED onto the jumped-to blocks. (Checked: with the
        # restart reverted this reads lo 0 -> 0 and goes red, where
        # `blocks > 0` alone passed on the frozen entry. The "events were
        # given back" check that used to sit here passed on the frozen entry
        # for the same reason and is gone -- resets plus a moved lo already
        # prove the old run was released.)
        tr.check(b["lo"] > a["lo"] and b["blocks"] > 0,
                 "the run restarted ON the new blocks instead of freezing "
                 "(lo %d -> %d, blocks=%d)" % (a["lo"], b["lo"], b["blocks"]))
        tr.check(b["resets"] > a["resets"],
                 "the gap dropped the stale run rather than keeping it for "
                 "nothing (resets %d -> %d)" % (a["resets"], b["resets"]))
        # And the restarted run then behaves like any other run.
        tr.check_eq(c["decoded"], b["decoded"],
                    "repeating the jumped-to window decodes nothing new "
                    "(decoded stayed %d)" % b["decoded"])
        tr.check(c["served"] > b["served"],
                 "repeating it is served from the restarted run "
                 "(served %d -> %d)" % (b["served"], c["served"]))
        # Correctness, not just bookkeeping: same answers as the old path.
        with ServerHarness(trace_dir,
                           env={"PGWT_CURRENT_TRACE_CACHE": "0"}) as srv:
            ctl_a = srv.query(PROBE_CMD, from_=str(early[0]),
                              to_=str(early[1]))
            ctl_b = srv.query(PROBE_CMD, from_=str(late[0]),
                              to_=str(late[1]))
        tr.check_eq(canonical_body(a_resp), canonical_body(ctl_a),
                    "the early window matches the uncached read")
        tr.check_eq(canonical_body(c_resp), canonical_body(ctl_b),
                    "the window read from the RESTARTED run matches the "
                    "uncached read")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ── 4. Restart safety ───────────────────────────────────────────────────────

def section_restart(tr):
    print("\n### 4. current.trace truncated and rewritten under a live server ###")
    first = generate_traces(busy_scenario(base=BASE, pid_base=1000,
                                          qid_base=100, span_s=20,
                                          pids=4, per_pid=3000))
    # The second capture occupies the SAME timestamp range with a disjoint pid
    # range. Same range on purpose: if it sat an hour later, a leaked
    # pre-restart block would be filtered out by the window anyway and the
    # section would pass without the identity check doing anything.
    second_src = generate_traces(busy_scenario(base=BASE, pid_base=7000,
                                               qid_base=700, span_s=20,
                                               pids=4, per_pid=3000))
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        err = os.path.join(tmp, "restart.err")
        env = dict(STATS_ENV)
        env["PGWT_CURRENT_TRACE_CACHE"] = "1"
        w_from, w_to = fixture_window(span_s=20)
        with ServerHarness(first, env=env, stderr_path=err) as srv:
            before = srv.query("top_sessions", from_=w_from, to_=w_to)
            pre_pids = {int(r["pid"]) for r in before.get("rows", [])}
            pre_stats = read_curcache(err)

            # The restart: the daemon archives the old file and creates a new
            # current.trace with a fresh header (src/event_writer.c
            # recover_current_trace + open_trace_file). Here the archive step
            # is skipped on purpose — a cache that keeps answering from its old
            # copy is the failure this section exists for, and leaving no
            # archive behind means a surviving pre-restart pid can only have
            # come from the cache.
            for name in ("current.trace", "current.trace.meta"):
                shutil.copyfile(os.path.join(second_src, name),
                                os.path.join(first, name))
            for extra in ("backends.jsonl", "query_texts.jsonl"):
                src = os.path.join(second_src, extra)
                if os.path.exists(src):
                    shutil.copyfile(src, os.path.join(first, extra))
            for name in os.listdir(first):
                if name.startswith("current.summary"):
                    src = os.path.join(second_src, name)
                    if os.path.exists(src):
                        shutil.copyfile(src, os.path.join(first, name))

            after = srv.query("top_sessions", from_=w_from, to_=w_to)
            post_pids = {int(r["pid"]) for r in after.get("rows", [])}
            post_stats = read_curcache(err)

        print("    pre pids=%s post pids=%s" % (sorted(pre_pids),
                                                sorted(post_pids)))
        # Both sides non-empty and disjoint by construction: without these two
        # checks the assertion below passes when the server simply answered
        # nothing after the rewrite, which proves nothing about the cache.
        tr.check(len(pre_pids) > 0, "pre-restart request returned sessions")
        tr.check(len(post_pids) > 0, "post-restart request returned sessions")
        tr.check(all(p < 7000 for p in pre_pids),
                 "pre-restart sessions are the first capture's pids")
        tr.check_eq(sorted(pre_pids & post_pids), [],
                    "not one pre-restart pid survives into the post-restart "
                    "answer")
        tr.check(all(p >= 7000 for p in post_pids),
                 "every post-restart session is from the new capture")
        if pre_stats and post_stats:
            tr.check(post_stats["resets"] > pre_stats["resets"],
                     "the identity check dropped the entry (resets %d -> %d)"
                     % (pre_stats["resets"], post_stats["resets"]))
        else:
            tr.check(False, "curcache stats unreadable across the restart")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(first)
        cleanup_traces(second_src)


# ── 5. Bypass suite: every way the sections above could stop checking ───────

def bypass_cache_never_consulted(tr, trace_dir):
    """A differential where BOTH sides run with the cache off is byte-identical
    and completely meaningless. require_used must refuse it."""
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        err = os.path.join(tmp, "both_off.err")
        steps = [("transitions", {})]
        run_sequence(trace_dir, False, err, steps)
        probe = QuietRunner("probe")
        require_used(probe, read_curcache(err), "cache used")
        tr.check_eq(probe.failed, 1,
                    "a run with the cache disabled is REFUSED as evidence, "
                    "not counted as agreement")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def bypass_data_only_in_rotated_file(tr):
    """All events in a rotated .trace.lz4 and an empty current.trace: the
    differential still passes trivially because the #283 path is never
    reached. require_used must refuse."""
    trace_dir = tempfile.mkdtemp(prefix="pgwt_cc_rot_")
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        generate_traces(busy_scenario(span_s=10, pids=2, per_pid=2000),
                        output_dir=trace_dir, rotate="2025-01-01_10")
        tr.check(not os.path.exists(os.path.join(trace_dir, "current.trace")),
                 "the rotated fixture leaves no current.trace")
        err = os.path.join(tmp, "rot.err")
        on = run_sequence(trace_dir, True, err, [("transitions", {})])
        off = run_sequence(trace_dir, False, os.path.join(tmp, "rot_off.err"),
                           [("transitions", {})])
        tr.check_eq(on, off, "rotated-only data answers identically (it must)")
        probe = QuietRunner("probe")
        require_used(probe, read_curcache(err), "cache used")
        tr.check_eq(probe.failed, 1,
                    "a rotated-only trace dir is REFUSED as evidence for the "
                    "current.trace cache")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


def bypass_missing_meta(tr):
    """No meta file: the reader falls back to the footer, so "committed" means
    something else. Answers must still agree across the toggle."""
    trace_dir = generate_traces(busy_scenario(span_s=12, pids=3, per_pid=2500))
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        os.unlink(meta_path(trace_dir))
        on = run_sequence(trace_dir, True, os.path.join(tmp, "nm.err"),
                          [("transitions", {}), ("top_sessions", {}),
                           ("executions", {})])
        off = run_sequence(trace_dir, False, os.path.join(tmp, "nm_off.err"),
                           [("transitions", {}), ("top_sessions", {}),
                            ("executions", {})])
        tr.check_eq(on, off,
                    "a current.trace with no meta answers identically "
                    "across the toggle")
        tr.check(any('"total_count"' in line for line in on),
                 "the no-meta fixture actually returned counts")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


def bypass_unparseable_meta(tr):
    """A garbage meta file (unparseable committed count) and an over-long one
    (more blocks than exist). Both must answer identically across the toggle —
    the cache must never retain a block the uncached path would not read."""
    for label, content in (("garbage", "not-a-number\n"),
                           ("over-long", "100000\n")):
        trace_dir = generate_traces(busy_scenario(span_s=12, pids=3,
                                                 per_pid=2500))
        tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
        try:
            with open(meta_path(trace_dir), "w") as f:
                f.write(content)
            steps = [("transitions", {}), ("executions", {}), ("aas", {})]
            on = run_sequence(trace_dir, True, os.path.join(tmp, "um.err"),
                              steps)
            off = run_sequence(trace_dir, False,
                               os.path.join(tmp, "um_off.err"), steps)
            tr.check_eq(on, off,
                        "a %s meta answers identically across the toggle"
                        % label)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
            cleanup_traces(trace_dir)


def bypass_truncated_file_under_meta(tr):
    """The meta claims N committed blocks but the file has been truncated below
    them — the partial-range case, the one where the thing being checked is
    ABSENT rather than wrong. A cache holding blocks that no longer exist must
    drop them, and the answer must be whatever a fresh uncached server says:
    a smaller count or a loud refusal, never the retained superset."""
    trace_dir = generate_traces(busy_scenario(span_s=12, pids=3, per_pid=2500))
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        err = os.path.join(tmp, "trunc.err")
        env = dict(STATS_ENV)
        env["PGWT_CURRENT_TRACE_CACHE"] = "1"
        path = os.path.join(trace_dir, "current.trace")
        size = os.path.getsize(path)
        w_from, w_to = fixture_window(span_s=12)
        with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
            before = srv.query(PROBE_CMD, from_=w_from, to_=w_to)
            n_before = count_of(before, PROBE_COUNT)
            pre = read_curcache(err)
            with open(path, "r+b") as f:
                f.truncate(size // 3)
            after = srv.query(PROBE_CMD, from_=w_from, to_=w_to)
            post = read_curcache(err)
        print("    truncation: %d events -> %s" % (n_before, canonical(after)[:160]))
        print("    curcache %s -> %s" % (pre, post))
        tr.check(n_before > 0, "the pre-truncation request saw events")
        refused = "unavailable" in after or "error" in after
        tr.check(refused or count_of(after, PROBE_COUNT) < n_before,
                 "a truncated current.trace refuses or returns FEWER events, "
                 "never the cached superset")
        if pre and post:
            tr.check(post["resets"] > pre["resets"],
                     "the vanished blocks were dropped from the entry "
                     "(resets %d -> %d)" % (pre["resets"], post["resets"]))
            tr.check(post["blocks"] <= pre["blocks"],
                     "the entry did not keep blocks the file no longer has "
                     "(%d -> %d)" % (pre["blocks"], post["blocks"]))
        else:
            tr.check(False, "curcache stats unreadable across the truncation")
        with ServerHarness(trace_dir,
                           env={"PGWT_CURRENT_TRACE_CACHE": "0"}) as srv:
            ctl = srv.query("transitions", from_=w_from, to_=w_to)
        tr.check_eq(canonical_body(after), canonical_body(ctl),
                    "the truncated read matches a fresh uncached read exactly")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


def bypass_over_budget(tr):
    """Past its memory budget the entry is dropped and rebuilt. That path must
    still answer identically — a budget drop that half-kept its blocks would be
    the worst of both."""
    trace_dir = generate_traces(busy_scenario(span_s=12, pids=3, per_pid=2500))
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        err = os.path.join(tmp, "budget.err")
        tight = {"PGWT_CURRENT_TRACE_CACHE_MAX_EVENTS": "5000"}
        steps = [("transitions", {}), ("executions", {}), ("top_sessions", {}),
                 ("aas", {})]
        on = run_sequence(trace_dir, True, err, steps, env=tight)
        off = run_sequence(trace_dir, False,
                           os.path.join(tmp, "budget_off.err"), steps)
        tr.check_eq(on, off,
                    "an entry repeatedly dropped by its budget answers "
                    "identically to the uncached path")
        st = read_curcache(err)
        if st is None:
            tr.check(False, "curcache stats unreadable under a tight budget")
        else:
            tr.check(st["resets"] > 0,
                     "the tight budget actually fired the drop path "
                     "(resets=%d)" % st["resets"])
            tr.check(st["events"] <= 5000 + 4096,
                     "the charged entry stayed inside its cap (events=%d)"
                     % st["events"])
            # Holds at every size, but cannot DISCRIMINATE at this one: with
            # ~3.5k events a doubling and a fixed step both give the same cap.
            # bypass_growth_step_is_a_bound is the instance with teeth.
            tr.check(st["cap"] - st["events"] <= CUR_CACHE_GROW_EVENTS,
                     "resident is within one growth step of charged "
                     "(cap=%d events=%d delta=%d, step=%d; scale-blind here)"
                     % (st["cap"], st["events"], st["cap"] - st["events"],
                        CUR_CACHE_GROW_EVENTS))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


def bypass_growth_step_is_a_bound(tr):
    """Resident (cc->cap) must stay within ONE growth step of charged
    (cc->count) -- the property the fixed-step growth exists for, and the one
    that makes the header comment's MB/h figure describe the allocation rather
    than only the accounting.

    This needs a fixture whose event count sits just ABOVE a power of two.
    Below one, a doubling and a fixed step land on the SAME cap and the
    assertion cannot tell them apart: measured at 1,500,000 events both give
    cap=2,097,152. Just above 2^21 they diverge -- at 2,200,000 events the
    doubling gives cap=4,194,304 (slack 1,994,304 > the 1 Mi step) where the
    fixed step gives 3,145,728 (slack 945,728). Rather than carry a 2.2M-event
    (105 MB) fixture, the step is shrunk with
    PGWT_CURRENT_TRACE_CACHE_GROW_EVENTS so a 33k-event one does the same job.
    """
    # ~33k cached events: just above 2^15 = 32768, which is what makes the
    # doubling overshoot visible (it jumps to 65536).
    trace_dir = generate_traces(busy_scenario(pids=8, per_pid=4100))
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        step = 2048
        err = os.path.join(tmp, "step.err")
        env = dict(STATS_ENV)
        env["PGWT_CURRENT_TRACE_CACHE"] = "1"
        env["PGWT_CURRENT_TRACE_CACHE_GROW_EVENTS"] = str(step)
        w_from, w_to = fixture_window()
        with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
            srv.query("transitions", from_=w_from, to_=w_to)
        st = read_curcache(err)
        if st is None:
            tr.check(False, "curcache stats unreadable for the growth step")
            return
        print("    step=%d charged=%d resident=%d slack=%d"
              % (step, st["events"], st["cap"], st["cap"] - st["events"]))
        # The fixture must be past a power of two, or the assertion below is
        # blind: a count just under one makes every growth policy agree.
        pow2 = 1
        while pow2 < st["events"]:
            pow2 *= 2
        tr.check(pow2 - st["events"] > step,
                 "a doubling WOULD be caught on this fixture: it would reach "
                 "cap=%d for %d events, overshooting by %d > step %d"
                 % (pow2, st["events"], pow2 - st["events"], step))
        tr.check(st["cap"] - st["events"] <= step,
                 "resident is within one growth step of charged "
                 "(cap=%d events=%d slack=%d step=%d)"
                 % (st["cap"], st["events"], st["cap"] - st["events"], step))
        tr.check(st["cap"] >= st["events"],
                 "resident covers charged (cap=%d >= events=%d)"
                 % (st["cap"], st["events"]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        cleanup_traces(trace_dir)


def bypass_stats_parser(tr):
    """read_curcache must refuse, not invent zeros, when it cannot see:
    missing file, no curcache line, a missing field, an unparseable field."""
    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
    try:
        missing = os.path.join(tmp, "nope.err")
        tr.check_eq(read_curcache(missing), None,
                    "a missing stderr file parses as None, not zeros")
        tr.check_eq(read_curcache(None), None,
                    "no stderr path at all parses as None")
        p = os.path.join(tmp, "a.err")
        with open(p, "w") as f:
            f.write("pgwt-server: loaded 3 backend metadata entries\n")
        tr.check_eq(read_curcache(p), None,
                    "stderr with no curcache line parses as None")
        with open(p, "w") as f:
            f.write("pgwt-server: curcache enabled=1 lo=0 blocks=2\n")
        tr.check_eq(read_curcache(p), None,
                    "a curcache line missing fields parses as None")
        with open(p, "w") as f:
            f.write("pgwt-server: curcache enabled=1 lo=0 blocks=two "
                    "events=1 cap=1 decoded=1 served=1 resets=0\n")
        tr.check_eq(read_curcache(p), None,
                    "an unparseable field parses as None")
        # A build that reports everything EXCEPT cap must refuse too: without
        # cap the charged-vs-resident assertions silently stop running, which
        # is the shape of a gate that quietly goes blind after a format change.
        with open(p, "w") as f:
            f.write("pgwt-server: curcache enabled=1 lo=0 blocks=2 events=8 "
                    "decoded=2 served=2 resets=0\n")
        tr.check_eq(read_curcache(p), None,
                    "a line with no cap= field parses as None, so the "
                    "resident-memory assertions cannot be skipped silently")
        with open(p, "w") as f:
            f.write("pgwt-server: curcache enabled=1 lo=0 blocks=2 events=8 "
                    "cap=16 decoded=2 served=0 resets=0\n")
            f.write("pgwt-server: curcache enabled=1 lo=0 blocks=2 events=8 "
                    "cap=16 decoded=2 served=2 resets=0\n")
        got = read_curcache(p)
        tr.check(got is not None and got["served"] == 2,
                 "the LAST curcache line is the one reported")
        # And the guard built on it refuses every unreadable shape.
        for bad in (None, {"enabled": 1, "lo": 0, "blocks": 0, "events": 0,
                           "cap": 0, "decoded": 0, "served": 0, "resets": 0}):
            probe = QuietRunner("probe")
            require_used(probe, bad, "x")
            tr.check_eq(probe.failed, 1,
                        "require_used refuses %s"
                        % ("None" if bad is None else "an all-zero entry"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def bypass_dependencies(tr):
    """A missing or non-executable generator must FAIL this test, never skip
    it. tests/run_all.sh treats exit 126/127 as a broken invocation for the
    same reason."""
    tr.check(os.path.exists(server_harness.SERVER_BIN)
             and os.access(server_harness.SERVER_BIN, os.X_OK),
             "pgwt-server exists and is executable")
    tr.check(os.path.exists(server_harness.GEN_BIN)
             and os.access(server_harness.GEN_BIN, os.X_OK),
             "gen_test_traces exists and is executable")
    # A generator given nothing must produce nothing -- never a
    # plausible-looking fixture. Measured: it writes two empty .jsonl sidecars
    # and NO trace file. The assertion is on that absence, because the version
    # of this check that ended in `os.path.getsize(...) >= 0` was true of every
    # file that exists and so could not fail at all.
    empty = tempfile.mkdtemp(prefix="pgwt_cc_empty_")
    try:
        raised = False
        try:
            generate_traces({"events": []}, output_dir=empty)
        except Exception:
            raised = True
        traces = sorted(f for f in os.listdir(empty)
                        if f == "current.trace" or f.endswith(".trace.lz4"))
        tr.check(raised or not traces,
                 "an empty scenario raises or produces no trace file at all "
                 "(found %s)" % (traces or "none"))
        # An empty trace dir: the server refuses to start rather than answering
        # zeros, and the harness surfaces that as an exception. An empty input
        # must never read as "the differential agreed".
        probe_dir = tempfile.mkdtemp(prefix="pgwt_cc_void_")
        tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")
        try:
            err = os.path.join(tmp, "void.err")
            refused = False
            why = ""
            try:
                run_sequence(probe_dir, True, err, [("transitions", {})])
            except Exception as exc:
                refused = True
                why = str(exc)
            tr.check(refused,
                     "an empty trace dir is refused outright, never answered "
                     "as an agreeing differential")
            # Separately, and this one can fail: the refusal must say WHY. The
            # previous version computed `"no trace files" in str(exc) or True`,
            # so it accepted any exception at all -- including one from a bug
            # in this test.
            tr.check("no trace files" in why,
                     "and the refusal names the reason (%r)" % why[:90])
            probe = QuietRunner("probe")
            require_used(probe, read_curcache(err), "cache used")
            tr.check_eq(probe.failed, 1,
                        "and it is REFUSED as cache evidence as well")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
            shutil.rmtree(probe_dir, ignore_errors=True)
    finally:
        shutil.rmtree(empty, ignore_errors=True)


def section_bypass(tr):
    print("\n### 5. bypass suite: ways this test could stop checking ###")
    bypass_dependencies(tr)
    bypass_stats_parser(tr)
    trace_dir = generate_traces(busy_scenario(span_s=10, pids=2, per_pid=2000))
    try:
        bypass_cache_never_consulted(tr, trace_dir)
    finally:
        cleanup_traces(trace_dir)
    bypass_data_only_in_rotated_file(tr)
    bypass_missing_meta(tr)
    bypass_unparseable_meta(tr)
    bypass_truncated_file_under_meta(tr)
    bypass_over_budget(tr)
    bypass_growth_step_is_a_bound(tr)


# ── main ────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    try:
        section_differential(t)
        section_differential_mixed(t)
        section_self_consistency(t)
        section_repeat(t)
        section_growth(t)
        section_forward_gap(t)
        section_restart(t)
        section_bypass(t)
    finally:
        if _BIG:
            cleanup_traces(_BIG)
    sys.exit(0 if t.summary() else 1)
