#!/usr/bin/env python3
"""schedstat_probe.py -- the /proc reader + background sampler behind the
Off-CPU* decomposition (issue #386). Pure logic is in
tests/schedstat_lib.py; everything that touches the filesystem, a clock or
a thread is here.

Harness instrumentation only: NO src/ change, nothing here runs inside the
daemon, and nothing here gates any verdict.

Two entry points:

  SchedstatSampler   a daemon thread the rehearsal starts for the whole
                     capture. Every `interval_s` it re-enumerates the
                     postmaster's children and reads each one's
                     /proc/<pid>/schedstat field 2, plus the daemon's own
                     utime+stime and /proc/loadavg. Window deltas are
                     computed afterwards by schedstat_lib.window_run_delay
                     over whatever samples fall inside the window the time
                     model was asked about -- so the two figures describe
                     the same interval, and the actual covered span is
                     reported rather than assumed.

  CLI               `python3 tests/schedstat_probe.py --parent-pid N
                    --duration 10` prints the same decomposition inputs for
                    an arbitrary process tree. This is how the reading is
                    PROVED to move: run it against an idle tree (near-zero
                    Sigma run_delay) and against a deliberately contended
                    one (it must rise). A number you cannot move is a
                    number you cannot trust.

`proc_root` is a parameter everywhere, so the reader is testable against a
fixture /proc tree on a Mac (tests/test_schedstat_lib.py) instead of only
on a live Linux box.
"""
import argparse
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schedstat_lib as sslib

# 5 s: brackets the rehearsal's 60 s Off-CPU* window to >= ~91% (see
# schedstat_lib.MIN_WINDOW_COVERAGE_PCT) while costing ~3 file reads per
# backend per tick -- on the order of a millisecond of work per tick for a
# demo-sized backend count, i.e. far below the daemon's own footprint, so
# the sampler cannot itself become the contention it measures.
DEFAULT_INTERVAL_S = 5.0

# Keep the series bounded: 45 min at 5 s is 540 samples, so 4096 covers any
# rehearsal length with room to spare while refusing to grow without bound
# if someone runs this for hours.
MAX_SAMPLES = 4096


def _read(path):
    """Returns the file's text, or None. A pid that vanished between
    enumeration and read raises FileNotFoundError/ProcessLookupError here --
    that is the normal case for a short-lived backend, not an error, and it
    becomes a COUNTED 'unreadable' entry upstream, never a silent 0."""
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except (OSError, ValueError):
        return None


class ProcReader:
    def __init__(self, proc_root="/proc", clk_tck=None):
        self.proc_root = proc_root
        self.clk_tck = clk_tck or os.sysconf("SC_CLK_TCK")

    def schedstats_enabled(self):
        text = _read(os.path.join(self.proc_root, "sys", "kernel",
                                  "sched_schedstats"))
        return sslib.parse_sched_schedstats(text)

    def ppid_map(self):
        """{pid: ppid} for every numeric entry currently in /proc. Returns
        ({}, reason) when /proc is absent or has no numeric entries -- an
        empty map upstream becomes "no backends", which refuses."""
        try:
            entries = os.listdir(self.proc_root)
        except OSError as e:
            return {}, f"cannot list {self.proc_root}: {e}"
        out = {}
        unparseable = 0
        seen_any = False
        for name in entries:
            if not name.isdigit():
                continue
            seen_any = True
            ppid, reason = sslib.parse_ppid(
                _read(os.path.join(self.proc_root, name, "stat")))
            if ppid is None:
                # Either the pid exited mid-scan (overwhelmingly the common
                # case) or /proc/<pid>/stat is genuinely malformed. Counted
                # either way; never fatal, never silent.
                unparseable += 1
                continue
            out[int(name)] = ppid
        if not seen_any:
            return {}, f"{self.proc_root} contains no numeric pid entries"
        return out, (f"{unparseable} pid(s) unreadable during the scan"
                     if unparseable else None)

    def backend_pids(self, pm_pid):
        ppids, reason = self.ppid_map()
        return sslib.backend_pids_from_ppid_map(ppids, pm_pid), reason

    def cpu_ticks(self, pid):
        return sslib.parse_proc_stat_cpu_ticks(
            _read(os.path.join(self.proc_root, str(pid), "stat")))

    def loadavg(self):
        return sslib.parse_loadavg(_read(os.path.join(self.proc_root,
                                                      "loadavg")))

    def snapshot(self, pm_pid, daemon_pid=None, t_ns=None):
        """One sample: schedstats state, per-backend run_delay, the daemon's
        cumulative CPU ticks, and the instantaneous load/run-queue length.
        `t_ns` is CLOCK_REALTIME ns -- the same domain as the trace's
        event timestamps and `info.now_ns` (src/server.c handle_info)."""
        if t_ns is None:
            t_ns = time.time_ns()
        enabled, enabled_reason = self.schedstats_enabled()
        pids, enum_reason = self.backend_pids(pm_pid)
        run_delay = {}
        unreadable = {}
        for pid in pids:
            val, reason = sslib.parse_schedstat(
                _read(os.path.join(self.proc_root, str(pid), "schedstat")))
            if val is None:
                # Normalised so the same transient cause aggregates into one
                # counted reason instead of N pid-specific strings.
                unreadable[pid] = ("pid vanished or schedstat unreadable"
                                   if reason and "None" in reason else reason)
            else:
                run_delay[pid] = val
        daemon_ticks = None
        daemon_ticks_reason = None
        if daemon_pid is not None:
            daemon_ticks, daemon_ticks_reason = self.cpu_ticks(daemon_pid)
        load, load_reason = self.loadavg()
        return {
            "t_ns": t_ns,
            "schedstats_enabled": enabled,
            "schedstats_reason": enabled_reason,
            "backends_enumerated": len(pids),
            "enumeration_reason": enum_reason,
            "run_delay_ns": run_delay,
            "unreadable": unreadable,
            "daemon_cpu_ticks": daemon_ticks,
            "daemon_cpu_ticks_reason": daemon_ticks_reason,
            "load": load,
            "load_reason": load_reason,
        }


class SchedstatSampler(threading.Thread):
    """Samples until stop() is called. Every exception is RECORDED (in
    .errors, and the sample series simply stops growing, which makes every
    later window refuse with "only N sample(s) inside the window") -- it
    never propagates into the rehearsal and never silently turns into a 0.
    """

    def __init__(self, pm_pid, daemon_pid=None, interval_s=DEFAULT_INTERVAL_S,
                 proc_root="/proc", max_samples=MAX_SAMPLES):
        super().__init__(daemon=True)
        self.pm_pid = pm_pid
        self.daemon_pid = daemon_pid
        self.interval_s = interval_s
        self.reader = ProcReader(proc_root)
        self.max_samples = max_samples
        self.samples = []
        self.errors = []
        # NOT `self._stop` -- threading.Thread has its own private _stop()
        # method, and shadowing it with an Event makes Thread.join() raise
        # "'Event' object is not callable" at interpreter teardown (found by
        # tests/test_schedstat_lib.py's CLI case, which is exactly why the
        # CLI has a test of its own rather than only the pure lib).
        self._stop_event = threading.Event()
        self._lock = threading.Lock()

    def stop(self):
        self._stop_event.set()

    def sample_once(self):
        try:
            s = self.reader.snapshot(self.pm_pid, self.daemon_pid)
        except Exception as e:  # never let the sampler kill the rehearsal
            with self._lock:
                self.errors.append(f"snapshot raised {e!r}")
            return None
        with self._lock:
            self.samples.append(s)
            if len(self.samples) > self.max_samples:
                del self.samples[0]
        return s

    def run(self):
        self.sample_once()
        while not self._stop_event.wait(self.interval_s):
            self.sample_once()

    def snapshot_list(self):
        with self._lock:
            return list(self.samples)

    def window(self, from_ns, to_ns):
        return sslib.window_run_delay(self.snapshot_list(), from_ns, to_ns)

    def daemon_cpu_pct(self, from_ns, to_ns):
        """The daemon's own %CPU over the same window (adviser's observer-
        effect question: does the 'regime step' track the daemon's CPU
        rising with trace size rather than workload contention?)."""
        sel = [s for s in self.snapshot_list()
               if s.get("t_ns") is not None and from_ns <= s["t_ns"] <= to_ns]
        if len(sel) < 2:
            return None, f"only {len(sel)} sample(s) in the window"
        sel.sort(key=lambda s: s["t_ns"])
        span_s = (sel[-1]["t_ns"] - sel[0]["t_ns"]) / 1e9
        return sslib.cpu_pct(sel[0].get("daemon_cpu_ticks"),
                             sel[-1].get("daemon_cpu_ticks"),
                             span_s, self.reader.clk_tck)

    def load_series(self, from_ns, to_ns):
        """Every load/run-queue reading inside the window, plus the window's
        max runnable count -- the second series that separates 'the box got
        contended' from 'the observer got heavier'."""
        sel = [s for s in self.snapshot_list()
               if s.get("t_ns") is not None and from_ns <= s["t_ns"] <= to_ns]
        points = [{"t_ns": s["t_ns"], **s["load"]} for s in sel if s.get("load")]
        if not points:
            return {"points": [], "max_runnable": None, "mean_load1": None,
                    "reason": f"no readable /proc/loadavg sample in the window "
                              f"({len(sel)} sample(s) present)"}
        return {
            "points": points,
            "max_runnable": max(p["runnable"] for p in points),
            "mean_load1": sum(p["load1"] for p in points) / len(points),
            "reason": None,
        }


def record_window(sampler, from_ns, to_ns, offcpu_ms, has_measured_cpu,
                  trace_now_ns, wall_now_ns, offset_s, out_path=None,
                  printer=print):
    """One window's Off-CPU* decomposition: Off-CPU* = Sigma run_delay +
    unexplained (issue #386). Lives here rather than in
    tests/demo_rehearsal.py so it can be unit-tested -- that module imports
    Playwright at module scope and the unit tier cannot load it.

    REPORTING ONLY, and defended as such. Every exception is caught and
    turned into a recorded refusal: the caller attaches the return value to
    a conservation sample whose `ok` was ALREADY decided, and
    tests/demo_rehearsal.py's sampler loop treats any exception out of a
    sample as a FAILING sample -- so without this guard a crash in this
    reporting-only code would fail a rehearsal that is otherwise clean.
    Nothing measured here may change a verdict in either direction
    (pinned by tests/test_schedstat_lib.py).

    Besides the subtraction it records, for the same window: the daemon's
    own %CPU and the box's load/run-queue length (an adviser's point -- a
    "regime step" part-way into a long capture could be the OBSERVER
    getting heavier as the trace grows rather than the workload getting
    contended; these two series separate those), and the span the samples
    actually covered, so a reader can see that the two figures describe the
    same interval instead of taking it on trust.

    `out_path` (JSONL) is appended and flushed per window: a rehearsal
    killed mid-run keeps every window it managed to measure. Two rehearsals
    in a row destroyed the artifacts needed to diagnose their own failures;
    a diagnostic that deletes its evidence is worse than no diagnostic.
    """
    rec = {"offset_s": offset_s, "from_ns": from_ns, "to_ns": to_ns,
           "window_s": ((to_ns - from_ns) / 1e9
                        if (from_ns is not None and to_ns is not None) else None),
           "has_measured_cpu": has_measured_cpu,
           "recorder_error": None}
    try:
        rec["sampler_errors"] = list(sampler.errors)
        clock_ok, clock_detail = sslib.clock_domain_ok(trace_now_ns, wall_now_ns)
        rec["clock_domain"] = clock_detail
        if not clock_ok:
            # Window bounds come from the trace; sample timestamps are our
            # own CLOCK_REALTIME. Not the same domain -> the subtraction
            # would be between two unrelated intervals. Refuse.
            run_delay = {"run_delay_ms": None, "refused": True,
                         "reason": clock_detail}
        else:
            run_delay = sampler.window(from_ns, to_ns)
        rec["run_delay"] = run_delay
        rec["decomposition"] = sslib.decompose(offcpu_ms, run_delay)
        pct, pct_reason = sampler.daemon_cpu_pct(from_ns, to_ns)
        rec["daemon_cpu_pct"] = pct
        rec["daemon_cpu_pct_reason"] = pct_reason
        load = sampler.load_series(from_ns, to_ns)
        rec["load_max_runnable"] = load.get("max_runnable")
        rec["load_mean_1min"] = load.get("mean_load1")
        rec["load_reason"] = load.get("reason")
        rec["load_points"] = load.get("points")
    except Exception as e:
        rec["recorder_error"] = repr(e)
        rec.setdefault("run_delay", {"run_delay_ms": None, "refused": True,
                                     "reason": f"recorder crashed: {e!r}"})
        rec["decomposition"] = sslib.decompose(offcpu_ms, rec["run_delay"])
        rec.setdefault("daemon_cpu_pct", None)
        rec.setdefault("load_max_runnable", None)
        rec.setdefault("load_mean_1min", None)
    try:
        printer(
            f"demo_rehearsal: offcpu_decomposition @{offset_s:.0f}s "
            f"(informational, does NOT gate the verdict): "
            f"{sslib.format_decomposition(rec['decomposition'])}; "
            f"daemon_cpu_pct={rec['daemon_cpu_pct']}, "
            f"load1={rec['load_mean_1min']}, "
            f"max_runnable={rec['load_max_runnable']}, "
            f"covered={rec['run_delay'].get('covered_s')}s of "
            f"{rec['window_s']}s"
            + (f" [recorder_error: {rec['recorder_error']}]"
               if rec["recorder_error"] else ""))
    except Exception as e:  # a printer must never break the rehearsal either
        rec["recorder_error"] = f"{rec['recorder_error']} / print: {e!r}"
    if out_path:
        try:
            with open(out_path, "a") as f:
                f.write(json.dumps(rec, sort_keys=True, default=str) + "\n")
                f.flush()
        except OSError as e:
            rec["recorder_error"] = f"{rec['recorder_error']} / write: {e!r}"
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--parent-pid", type=int, required=True,
                    help="enumerate THIS pid's direct children as the task "
                         "set (the postmaster, for a PG capture)")
    ap.add_argument("--daemon-pid", type=int, default=None,
                    help="also record this pid's utime+stime -> %%CPU")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--proc-root", default="/proc")
    ap.add_argument("--offcpu-ms", type=float, default=None,
                    help="optional: an Off-CPU* figure for the same window, "
                         "to print the full decomposition")
    ap.add_argument("--label", default="probe")
    args = ap.parse_args()

    sampler = SchedstatSampler(args.parent_pid, daemon_pid=args.daemon_pid,
                               interval_s=args.interval,
                               proc_root=args.proc_root)
    t_from = time.time_ns()
    sampler.start()
    time.sleep(args.duration)
    sampler.stop()
    sampler.join(timeout=args.interval * 3 + 5)
    t_to = time.time_ns()

    win = sampler.window(t_from, t_to)
    out = {
        "label": args.label,
        "from_ns": t_from, "to_ns": t_to,
        "duration_s": (t_to - t_from) / 1e9,
        "interval_s": args.interval,
        "parent_pid": args.parent_pid,
        "samples": len(sampler.snapshot_list()),
        "sampler_errors": sampler.errors,
        "run_delay": win,
        "load": sampler.load_series(t_from, t_to),
    }
    if args.daemon_pid is not None:
        pct, reason = sampler.daemon_cpu_pct(t_from, t_to)
        out["daemon_cpu_pct"] = pct
        out["daemon_cpu_pct_reason"] = reason
    if args.offcpu_ms is not None:
        out["decomposition"] = sslib.decompose(args.offcpu_ms, win)
    json.dump(out, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    # Exit 2 when the measurement REFUSED -- a caller scripting this must be
    # able to tell "measured 0" from "could not measure".
    return 2 if win.get("refused") else 0


if __name__ == "__main__":
    sys.exit(main())
