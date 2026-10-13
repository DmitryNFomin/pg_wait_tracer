#!/usr/bin/env python3
"""schedstat_lib.py -- pure parsing/aggregation for the Off-CPU* residual
decomposition (issue #386).

WHY THIS EXISTS
---------------
`Off-CPU*` in the time model is a RESIDUAL: src/compute.c computes it as
`max(0, DB Time - CPU* - Sigma waits)`. A rehearsal's `offcpu_cap` check
failed at 10.04% and 11.48% against a 10.0% bound, and the stated cause --
"contention from the load increase" -- was an INFERENCE, never a
measurement. Two candidate causes produce the identical residual:

  (a) run-queue delay: the backend was runnable but not on a CPU, so wall
      time advanced while its measured cpu_ns did not; and
  (b) an instrumentation gap: the backend was genuinely blocked in-kernel
      (major fault, writeback, ...) inside an execution, in a state the
      daemon attributes to no wait class.

Only (a) is measurable independently of this project's own instrumentation,
from `/proc/<pid>/schedstat` **field 2** (`run_delay`, cumulative
nanoseconds a task spent runnable-but-not-running; kernel
`kernel/sched/stats.c`). Subtracting it gives the quantity actually worth
bounding:

    unexplained_ms = offcpu_ms - run_delay_ms

which, unlike the raw `offcpu_ms / db_time_ms` percentage, does not move
when the box gets busier. The current 10% cap is a proxy for how contended
the box is (docs/DEMO_REHEARSAL_CRITERIA.md section 3 says as much about
its own provenance); `unexplained_ms` is not.

NOTHING HERE GATES ANYTHING (deliberate, issue #386: "Do NOT add a gate on
it yet"). This module parses and decomposes; the rehearsal records the
decomposition as informational output. Registering a bound in the same
change that first measures it is what docs/DEMO_REHEARSAL_CRITERIA.md
exists to prevent.

FAIL-SAFE RULE, AND THE ONE FAILURE MODE THAT MATTERS
-----------------------------------------------------
`run_delay` is only accumulated when the kernel's schedstats are ON
(`CONFIG_SCHEDSTATS` built in AND `/proc/sys/kernel/sched_schedstats` ==
1). Measured 2026-10-13: it is **0 on both gate boxes by default**. With it
off, every `/proc/<pid>/schedstat` field 2 sits at 0 forever -- and a
0 here reads exactly like "measured: no run-queue delay at all", which
would make the whole residual look unexplained on no evidence whatsoever.
That is the false negative this module is built to refuse: every function
below returns an explicit refusal (`run_delay_ms=None` + a `reason`) rather
than a 0 whenever it cannot actually see the quantity -- no samples, one
sample, no backends, all reads unreadable, schedstats off, a window its
samples do not cover, a clock domain it cannot align. A gate that cannot
see must refuse, never approve.

Pure: no /proc access, no threads, no subprocesses, no sockets, no clock
reads. The impure reader/sampler lives in tests/schedstat_probe.py; the
unit tests (tests/test_schedstat_lib.py) cover both, the reader against a
fake /proc tree so it runs on the Mac.
"""

# ── Thresholds (reporting only -- none of these gate a verdict) ──────────
#
# A window needs at least two samples of the same task to form a delta at
# all; one sample is a value, not a rate.
MIN_SAMPLES_PER_WINDOW = 2

# How much of the requested window the bracketing samples must actually
# span before the delta is reported as that window's run_delay. The
# rehearsal's own Off-CPU* figure covers a fixed 60 s trailing window
# (demo_rehearsal_lib.RECENT_WINDOW_S); a 5 s sampling interval brackets it
# to >= ~91%. 80% is the floor below which the two numbers are measuring
# visibly different spans and the subtraction stops meaning anything --
# refused, not quietly reported against a shorter span. The ACTUAL covered
# span is always reported alongside, so no figure here depends on this
# constant being generous.
MIN_WINDOW_COVERAGE_PCT = 80.0

# The trace's event timestamps and `info.now_ns` are CLOCK_REALTIME
# nanoseconds (src/server.c handle_info: `clock_gettime(CLOCK_REALTIME)`),
# and so are this module's sample timestamps -- same host, same clock. This
# bound exists so that if either of those two facts ever stops being true
# (a different clock domain in the trace, or a sampler running somewhere
# else), window alignment REFUSES instead of silently subtracting two
# unrelated intervals.
CLOCK_SKEW_BOUND_S = 5.0


def _refuse(reason, **extra):
    d = {"run_delay_ms": None, "refused": True, "reason": reason}
    d.update(extra)
    return d


# ── /proc parsers ────────────────────────────────────────────────────────

def parse_schedstat(text):
    """/proc/<pid>/schedstat -> (run_delay_ns, None) | (None, reason).

    Format (kernel/sched/stats.c proc_sched_show_task / show_schedstat):
    three space-separated unsigned numbers --
        field 1  sum_exec_runtime  (ns on a CPU)
        field 2  run_delay         (ns runnable but NOT on a CPU)  <-- this
        field 3  pcount            (number of times scheduled in)
    Returns the whole triple's field 2 only. A short line, a non-numeric
    field or a negative value is a refusal, never a 0: an unparseable
    counter that reads as 0 would be indistinguishable from a genuinely
    idle task.
    """
    if text is None:
        return None, "schedstat unreadable (None)"
    fields = text.split()
    if len(fields) < 3:
        return None, f"schedstat has {len(fields)} field(s), expected >= 3: {text.strip()!r}"
    try:
        values = [int(f) for f in fields[:3]]
    except ValueError:
        return None, f"schedstat field is not an integer: {text.strip()!r}"
    if any(v < 0 for v in values):
        return None, f"schedstat field is negative: {text.strip()!r}"
    return values[1], None


def parse_sched_schedstats(text):
    """/proc/sys/kernel/sched_schedstats -> (enabled_bool, None) | (None, reason).

    The runtime switch that decides whether field 2 above moves AT ALL.
    Measured 0 on both gate boxes (2026-10-13), which is why this is read
    every tick and not assumed. A missing file means the kernel was built
    without CONFIG_SCHEDSTATS -- also a refusal, for the same reason.
    """
    if text is None:
        return None, ("/proc/sys/kernel/sched_schedstats unreadable -- kernel "
                      "built without CONFIG_SCHEDSTATS, or /proc not mounted; "
                      "run_delay cannot be measured")
    s = text.strip()
    if s == "1":
        return True, None
    if s == "0":
        return False, ("kernel.sched_schedstats=0 -- the kernel is not "
                       "accumulating run_delay, so every /proc/<pid>/schedstat "
                       "field 2 is pinned at 0; enable with "
                       "`sysctl -w kernel.sched_schedstats=1`")
    return None, f"sched_schedstats value not 0/1: {s!r}"


def parse_ppid(text):
    """/proc/<pid>/stat -> (ppid, None) | (None, reason).

    `comm` (field 2) is parenthesised and may itself contain spaces AND
    parentheses, so the split is on the LAST ')' -- a naive `.split()[3]`
    mis-reads any backend whose comm has a space in it, which on PostgreSQL
    is every single one ("postgres: postgres postgres ..." truncated to
    TASK_COMM_LEN, e.g. "(postgres: 18/)").
    """
    if text is None:
        return None, "stat unreadable (None)"
    close = text.rfind(")")
    if close < 0 or "(" not in text:
        return None, f"stat has no parenthesised comm: {text[:60]!r}"
    rest = text[close + 1:].split()
    if len(rest) < 2:
        return None, f"stat truncated after comm: {text[:60]!r}"
    try:
        return int(rest[1]), None
    except ValueError:
        return None, f"stat ppid field is not an integer: {rest[1]!r}"


def parse_proc_stat_cpu_ticks(text):
    """/proc/<pid>/stat -> (utime+stime in clock ticks, None) | (None, reason).

    Fields 14 (utime) and 15 (stime), 1-based, i.e. offsets 11 and 12 after
    the parenthesised comm. Same last-')' rule as parse_ppid.
    """
    if text is None:
        return None, "stat unreadable (None)"
    close = text.rfind(")")
    if close < 0 or "(" not in text:
        return None, f"stat has no parenthesised comm: {text[:60]!r}"
    rest = text[close + 1:].split()
    # rest[0] = state (field 3); utime is field 14 -> rest[11].
    if len(rest) < 13:
        return None, f"stat has {len(rest)} post-comm fields, need >= 13"
    try:
        utime = int(rest[11])
        stime = int(rest[12])
    except ValueError:
        return None, f"utime/stime not integers: {rest[11]!r}/{rest[12]!r}"
    if utime < 0 or stime < 0:
        return None, f"utime/stime negative: {utime}/{stime}"
    return utime + stime, None


def parse_loadavg(text):
    """/proc/loadavg -> (dict, None) | (None, reason).

    "0.05 0.13 0.11 1/234 2089392" -> load1/load5/load15 plus the
    runnable/total task counts, which is the run-QUEUE LENGTH at the
    instant of the read (`runnable`), the series an adviser asked for
    alongside the daemon's own %CPU to separate a real contention step from
    an observer effect.
    """
    if text is None:
        return None, "loadavg unreadable (None)"
    f = text.split()
    if len(f) < 4:
        return None, f"loadavg has {len(f)} field(s), expected >= 4: {text.strip()!r}"
    try:
        load1, load5, load15 = float(f[0]), float(f[1]), float(f[2])
    except ValueError:
        return None, f"loadavg averages not numeric: {text.strip()!r}"
    if "/" not in f[3]:
        return None, f"loadavg runnable/total field malformed: {f[3]!r}"
    r, _, t = f[3].partition("/")
    try:
        runnable, total = int(r), int(t)
    except ValueError:
        return None, f"loadavg runnable/total not integers: {f[3]!r}"
    return {"load1": load1, "load5": load5, "load15": load15,
            "runnable": runnable, "total_tasks": total}, None


# ── Backend enumeration ──────────────────────────────────────────────────

def backend_pids_from_ppid_map(ppid_map, pm_pid):
    """Direct children of the postmaster = the PG backends (plus auxiliary
    processes: checkpointer, walwriter, io workers...). `ppid_map` is
    {pid: ppid} as scanned from /proc; pure so the enumeration is testable
    against a fixture tree rather than only against a live box.

    The postmaster ITSELF is excluded: it executes no statements, so it
    contributes nothing to DB Time and its run_delay would be noise added
    to the left side of a subtraction whose right side (Off-CPU*) only
    counts in-execution time.

    Returns a sorted list -- deterministic order so a recorded snapshot
    diffs cleanly between runs.
    """
    if not ppid_map or pm_pid is None:
        return []
    return sorted(pid for pid, ppid in ppid_map.items()
                  if ppid == pm_pid and pid != pm_pid)


# ── Window aggregation ───────────────────────────────────────────────────

def window_run_delay(samples, from_ns, to_ns,
                     min_samples=MIN_SAMPLES_PER_WINDOW,
                     min_coverage_pct=MIN_WINDOW_COVERAGE_PCT):
    """Sum of per-backend `run_delay` DELTAS across [from_ns, to_ns].

    `samples` is a list of dicts, ordered by `t_ns`, each:
        {"t_ns": int,                      # CLOCK_REALTIME ns of the read
         "run_delay_ns": {pid: int},       # cumulative counter per backend
         "unreadable": {pid: reason},      # enumerated, could not be read
         "schedstats_enabled": True/False/None}

    PIDS COME AND GO (the demo workload opens and closes sessions all the
    way through), so every task is classified and COUNTED, never silently
    dropped:
      tracked   -- present in the first and last sample of the window;
                   delta = last - first.
      vanished  -- present at the start, gone before the end; counted UP TO
                   its last observation (issue #386's explicit requirement),
                   and reported as such so the reader knows the window's
                   total has a truncated contributor in it.
      joined    -- absent at the start, present at the end; counted from its
                   FIRST observation. Its run_delay before that observation
                   is unknowable from sampling, so this UNDERCOUNTS, and the
                   count of such pids is reported for exactly that reason.
      transient -- appeared and vanished strictly inside the window; counted
                   between its own first/last observation.
      single    -- observed exactly once: no delta exists. Contributes 0 and
                   is counted under its own reason, not folded into
                   "tracked" (which would make a 0 look like a measurement).
      reset     -- the counter went BACKWARDS, which means pid reuse (a new
                   task on the same number) and not a 0-delta. Contributes
                   0, counted separately; the cumulative counters of two
                   different tasks are not subtractable.

    Returns either {"run_delay_ms": float, ...} or a refusal
    ({"run_delay_ms": None, "refused": True, "reason": ...}).
    """
    if samples is None:
        return _refuse("no sample list at all (sampler never started?)")
    if from_ns is None or to_ns is None:
        return _refuse(f"window bounds missing (from_ns={from_ns!r}, to_ns={to_ns!r})")
    requested_s = (to_ns - from_ns) / 1e9
    if requested_s <= 0:
        return _refuse(f"non-positive window: from_ns={from_ns} to_ns={to_ns}")

    sel = [s for s in samples
           if s.get("t_ns") is not None and from_ns <= s["t_ns"] <= to_ns]
    if len(sel) < min_samples:
        return _refuse(
            f"only {len(sel)} sample(s) inside the window, need {min_samples} "
            f"(sampler has {len(samples)} sample(s) in total)",
            samples_in_window=len(sel), samples_total=len(samples))
    sel = sorted(sel, key=lambda s: s["t_ns"])

    # Schedstats must have been ON for the whole window. Off -> every
    # counter is pinned at 0; toggled mid-window -> the delta spans two
    # different accounting regimes. Both refuse.
    states = {s.get("schedstats_enabled") for s in sel}
    if states != {True}:
        _, off_reason = parse_sched_schedstats("0")
        if False in states:
            return _refuse(f"schedstats not enabled for the whole window: {off_reason}",
                           schedstats_states=sorted(str(x) for x in states))
        return _refuse(
            "schedstats state unknown for at least one sample in the window "
            "-- run_delay cannot be trusted",
            schedstats_states=sorted(str(x) for x in states))

    covered_s = (sel[-1]["t_ns"] - sel[0]["t_ns"]) / 1e9
    coverage_pct = covered_s / requested_s * 100.0
    if coverage_pct < min_coverage_pct:
        return _refuse(
            f"samples span only {covered_s:.2f}s of the {requested_s:.2f}s "
            f"window ({coverage_pct:.1f}% < {min_coverage_pct}%)",
            covered_s=covered_s, requested_s=requested_s,
            coverage_pct=coverage_pct, samples_in_window=len(sel))

    first_map = sel[0].get("run_delay_ns") or {}
    last_map = sel[-1].get("run_delay_ns") or {}

    per_pid = {}        # pid -> (first_value, last_value, n_observations)
    for s in sel:
        for pid, val in (s.get("run_delay_ns") or {}).items():
            if pid in per_pid:
                f, _, n = per_pid[pid]
                per_pid[pid] = (f, val, n + 1)
            else:
                per_pid[pid] = (val, val, 1)

    counts = {"tracked": 0, "vanished": 0, "joined": 0, "transient": 0,
              "single": 0, "reset": 0}
    contrib_ns = {"tracked": 0, "vanished": 0, "joined": 0, "transient": 0}
    total_ns = 0
    deltas_counted = 0
    for pid, (first, last, n) in per_pid.items():
        if n < 2:
            counts["single"] += 1
            continue
        if last < first:
            counts["reset"] += 1
            continue
        in_first = pid in first_map
        in_last = pid in last_map
        if in_first and in_last:
            kind = "tracked"
        elif in_first:
            kind = "vanished"
        elif in_last:
            kind = "joined"
        else:
            kind = "transient"
        delta = last - first
        counts[kind] += 1
        contrib_ns[kind] += delta
        total_ns += delta
        deltas_counted += 1

    if deltas_counted == 0:
        return _refuse(
            f"no backend was observed twice inside the window "
            f"({len(per_pid)} pid(s) seen: {counts['single']} once only, "
            f"{counts['reset']} with a counter reset) -- nothing to difference",
            pid_counts=counts, samples_in_window=len(sel))

    unreadable = {}
    for s in sel:
        for pid, reason in (s.get("unreadable") or {}).items():
            unreadable.setdefault(reason, 0)
            unreadable[reason] += 1

    return {
        "run_delay_ms": total_ns / 1e6,
        "refused": False,
        "reason": None,
        "samples_in_window": len(sel),
        "requested_s": requested_s,
        "covered_s": covered_s,
        "coverage_pct": coverage_pct,
        "pid_counts": counts,
        "contrib_ms": {k: v / 1e6 for k, v in contrib_ns.items()},
        "pids_counted": deltas_counted,
        "unreadable_reads": unreadable,
    }


def clock_domain_ok(trace_now_ns, wall_now_ns, bound_s=CLOCK_SKEW_BOUND_S):
    """The trace's `info.now_ns` against this process's own CLOCK_REALTIME
    read, taken at the same moment. Same host, same clock -> ~0. Anything
    past `bound_s` means the window bounds this module is handed are not in
    the domain its sample timestamps are in, and the subtraction would be
    between two unrelated intervals -> refuse."""
    if trace_now_ns is None or wall_now_ns is None:
        return False, (f"cannot compare clock domains "
                       f"(trace_now_ns={trace_now_ns!r}, wall_now_ns={wall_now_ns!r})")
    skew_s = abs(trace_now_ns - wall_now_ns) / 1e9
    if skew_s > bound_s:
        return False, (f"trace clock and sampler clock differ by {skew_s:.2f}s "
                       f"(bound {bound_s}s) -- window bounds are not in the "
                       f"sampler's CLOCK_REALTIME domain")
    return True, f"clock domains agree within {skew_s:.3f}s (bound {bound_s}s)"


def cpu_pct(prev_ticks, cur_ticks, span_s, clk_tck):
    """Daemon (or any process) %CPU over a window, from /proc/<pid>/stat
    utime+stime deltas. One core fully busy = 100.0. Returns
    (pct, None) | (None, reason) -- a missing endpoint, a backwards counter
    (pid reuse) or a non-positive span refuses rather than reporting 0.0,
    which would read as "the daemon used no CPU"."""
    if prev_ticks is None or cur_ticks is None:
        return None, (f"missing utime+stime endpoint "
                      f"(prev={prev_ticks!r}, cur={cur_ticks!r})")
    if clk_tck is None or clk_tck <= 0:
        return None, f"invalid clock tick rate: {clk_tck!r}"
    if span_s is None or span_s <= 0:
        return None, f"non-positive span: {span_s!r}"
    if cur_ticks < prev_ticks:
        return None, (f"utime+stime went backwards ({prev_ticks} -> {cur_ticks}) "
                      f"-- pid reuse, not a 0 delta")
    return (cur_ticks - prev_ticks) / clk_tck / span_s * 100.0, None


# ── The decomposition itself ─────────────────────────────────────────────

def decompose(offcpu_ms, run_delay_result):
    """Off-CPU* = Sigma run_delay + unexplained.

    `run_delay_result` is window_run_delay()'s return value (or any dict
    with run_delay_ms/refused/reason). The two sides come from INDEPENDENT
    sources by construction -- `offcpu_ms` from pgwt-server's time model,
    `run_delay_ms` from the kernel's own per-task counters -- which is the
    whole point: a decomposition whose two sides came from the same sum
    could not fail.

    Refuses (unexplained_ms=None) whenever either side is unavailable. In
    particular it NEVER falls back to "run_delay = 0, so everything is
    unexplained": that is the conclusion the measurement exists to test,
    and reaching it by default would make the instrument confirm its own
    hypothesis.

    `degenerate` marks offcpu_ms ~= 0: the subtraction is then arithmetically
    fine but carries no information about whether run_delay explains the
    residual, and must not be read as agreement.

    HOW THE RESULT MAY BE READ -- the asymmetry matters, and it is the
    reason this measurement can settle the question at all:

      Sigma run_delay here is an UPPER BOUND on the run-queue share that
      could be inside Off-CPU*, not an exact match for it. The kernel
      counter accrues whenever the task is runnable-but-waiting, including
      while the backend is idle, in `ClientRead`, or already inside some
      other wait event -- and none of that time is in Off-CPU* (DB Time
      counts active execution, and run-queue delay during an open wait is
      booked under that wait; docs/DEMO_REHEARSAL_CRITERIA.md section 3,
      point 3 makes the same distinction).

      So:
      * unexplained_ms > 0 is the STRONG direction. Even the upper bound of
        run-queue delay does not cover the residual, so something else --
        an instrumentation gap: in-execution blocking the daemon attributes
        to no wait class (major fault, writeback, ...) -- must account for
        the difference.
      * unexplained_ms <= 0 is the WEAK direction. It says run-queue delay
        is large enough that it COULD explain the residual; it does not
        prove that this particular delay landed inside Off-CPU* rather than
        inside a wait event or idle time.

      A bound belongs on the strong direction. Reported either way, never
      clamped (see the max(0, ...) mutation in the test suite): clamping a
      negative remainder to 0 would erase exactly the overlap that tells a
      reader which direction they are in.
    """
    out = {"offcpu_ms": offcpu_ms, "run_delay_ms": None,
           "unexplained_ms": None, "explained_pct": None,
           "refused": True, "reason": None, "degenerate": False}
    if offcpu_ms is None:
        out["reason"] = "offcpu_ms unavailable from the time model"
        return out
    if not isinstance(run_delay_result, dict):
        out["reason"] = f"no run_delay measurement supplied ({run_delay_result!r})"
        return out
    rd = run_delay_result.get("run_delay_ms")
    if rd is None:
        out["reason"] = ("run_delay unavailable: "
                         + str(run_delay_result.get("reason")))
        return out
    out["run_delay_ms"] = rd
    out["unexplained_ms"] = offcpu_ms - rd
    out["refused"] = False
    if abs(offcpu_ms) < 1e-9:
        out["degenerate"] = True
        out["reason"] = ("offcpu_ms is ~0 -- nothing to decompose; this is "
                         "NOT evidence that run_delay explains the residual")
    else:
        out["explained_pct"] = rd / offcpu_ms * 100.0
    return out


def format_decomposition(d):
    """One human line per window, showing the decomposition rather than a
    verdict (issue #386: "so a reader can see the decomposition rather than
    a verdict")."""
    if not isinstance(d, dict):
        return f"offcpu decomposition: UNAVAILABLE ({d!r})"
    if d.get("refused") and d.get("unexplained_ms") is None:
        off = d.get("offcpu_ms")
        off_s = "n/a" if off is None else f"{off:.2f}"
        return (f"offcpu_ms={off_s} run_delay_ms=n/a unexplained_ms=n/a "
                f"-- REFUSED: {d.get('reason')}")
    line = (f"offcpu_ms={d['offcpu_ms']:.2f} "
            f"run_delay_ms={d['run_delay_ms']:.2f} "
            f"unexplained_ms={d['unexplained_ms']:.2f}")
    if d.get("explained_pct") is not None:
        line += f" ({d['explained_pct']:.1f}% of Off-CPU* explained by run_delay)"
    if d.get("degenerate"):
        line += f" -- {d.get('reason')}"
    return line
