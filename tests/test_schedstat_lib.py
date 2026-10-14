#!/usr/bin/env python3
"""test_schedstat_lib.py -- unit + bypass suite for the Off-CPU* residual
decomposition (issue #386): tests/schedstat_lib.py (pure) and
tests/schedstat_probe.py's /proc reader (driven against a FIXTURE /proc
tree in a tmpdir, so it runs on the Mac and in `make check`, not only on a
Linux box).

The suite is written around one hazard, because the true-positive path was
never the broken one. `run_delay` is read from /proc/<pid>/schedstat field
2, and that counter **does not move** unless the kernel's schedstats are
on -- measured 2026-10-13, `/proc/sys/kernel/sched_schedstats` is **0 on
both gate boxes by default**. A reader that reports 0 in that state is not
measuring "no run-queue delay", it is measuring nothing, and the
decomposition built on it would credit 100% of the residual to
"unexplained" with no evidence at all. So every way this measurement can
be made blind -- schedstats off, schedstats unknown, schedstats toggled
mid-window, no samples, one sample, no backends, a /proc that is not there,
an unparseable field, a pid that vanished, a reused pid, a window the
samples do not cover, a clock domain that does not align -- has a case
here, and each must REFUSE (None + a reason), never return 0.

Also pins the thing this change deliberately does NOT do: nothing in the
decomposition may change any rehearsal verdict (issue #386: "Do NOT add a
gate on it yet").

Usage: python3 tests/test_schedstat_lib.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schedstat_lib as lib
import schedstat_probe as probe
import demo_rehearsal_lib as drlib

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


MS = 1_000_000
S = 1_000_000_000


def approx(value, want, tol=1e-6):
    """None-safe: a refusal (None) must come back as a clean FAIL, never a
    TypeError that aborts the rest of the suite -- found while mutating the
    vanished-pid accounting, where the broken version refuses and the test
    then crashed instead of reporting red."""
    return value is not None and abs(value - want) < tol


def sample(t_ns, run_delay_ns, enabled=True, unreadable=None,
           daemon_cpu_ticks=None, load=None):
    return {"t_ns": t_ns, "run_delay_ns": dict(run_delay_ns),
            "schedstats_enabled": enabled, "unreadable": unreadable or {},
            "daemon_cpu_ticks": daemon_cpu_ticks, "load": load}


# ── parse_schedstat ──────────────────────────────────────────────────────

def test_parse_schedstat_picks_field_two():
    # field 1 is sum_exec_runtime (on-CPU) and field 3 is pcount; reading
    # either one instead of run_delay would produce a plausible-looking
    # number that answers a different question entirely.
    val, reason = lib.parse_schedstat("111111 22222 333\n")
    check(val == 22222 and reason is None,
          f"parse_schedstat returns field 2 (run_delay), got {val!r}/{reason!r}")


def test_parse_schedstat_short_line_refuses():
    val, reason = lib.parse_schedstat("111 222\n")
    check(val is None and "2 field" in reason,
          f"a 2-field schedstat refuses instead of reading 0: {val!r}/{reason!r}")


def test_parse_schedstat_unparseable_field_refuses():
    val, reason = lib.parse_schedstat("111 notanumber 333\n")
    check(val is None and "not an integer" in reason,
          f"an unparseable run_delay refuses: {val!r}/{reason!r}")


def test_parse_schedstat_negative_refuses():
    val, reason = lib.parse_schedstat("111 -5 333\n")
    check(val is None and "negative" in reason,
          f"a negative counter refuses: {val!r}/{reason!r}")


def test_parse_schedstat_missing_file_refuses():
    val, reason = lib.parse_schedstat(None)
    check(val is None and reason is not None,
          f"an unreadable schedstat (vanished pid) refuses: {val!r}/{reason!r}")


def test_parse_schedstat_empty_refuses():
    val, reason = lib.parse_schedstat("")
    check(val is None, f"an empty schedstat refuses: {val!r}/{reason!r}")


# ── parse_sched_schedstats: the central false-negative guard ─────────────

def test_sched_schedstats_disabled_is_not_a_zero():
    enabled, reason = lib.parse_sched_schedstats("0\n")
    check(enabled is False and "sysctl" in reason,
          f"sched_schedstats=0 reports DISABLED with the fix in the reason: "
          f"{enabled!r}/{reason!r}")


def test_sched_schedstats_enabled():
    enabled, reason = lib.parse_sched_schedstats("1\n")
    check(enabled is True and reason is None, f"sched_schedstats=1 -> enabled ({enabled!r})")


def test_sched_schedstats_missing_refuses():
    enabled, reason = lib.parse_sched_schedstats(None)
    check(enabled is None and "CONFIG_SCHEDSTATS" in reason,
          f"a missing sched_schedstats is unknown, not disabled and not "
          f"enabled: {enabled!r}/{reason!r}")


def test_sched_schedstats_garbage_refuses():
    enabled, reason = lib.parse_sched_schedstats("maybe\n")
    check(enabled is None and "not 0/1" in reason,
          f"an unparseable sched_schedstats refuses: {enabled!r}/{reason!r}")


# ── parse_ppid / parse_proc_stat_cpu_ticks ───────────────────────────────

def _stat_line(pid, comm, ppid, utime, stime):
    post = ["S", str(ppid), "100", "100", "0", "-1", "4194304",
            "1234", "0", "7", "0", str(utime), str(stime),
            "0", "0", "20", "0", "1", "0"]
    return f"{pid} ({comm}) " + " ".join(post) + "\n"


def test_parse_ppid_survives_spaces_and_parens_in_comm():
    # Every PostgreSQL backend's comm has a space in it, and the daemon's
    # can carry parens; a naive split()[3] reads the wrong field for all of
    # them and would silently enumerate zero backends.
    text = _stat_line(4242, "postgres: 18/m (x)", 999, 10, 5)
    ppid, reason = lib.parse_ppid(text)
    check(ppid == 999 and reason is None,
          f"ppid parsed past a comm containing ' ' and ')': {ppid!r}/{reason!r}")


def test_parse_ppid_no_comm_refuses():
    ppid, reason = lib.parse_ppid("4242 nocomm S 1\n")
    check(ppid is None and reason is not None,
          f"a stat line with no parenthesised comm refuses: {ppid!r}/{reason!r}")


def test_parse_cpu_ticks_fields_14_15():
    text = _stat_line(7, "pg_wait_tracer", 1, 310, 95)
    ticks, reason = lib.parse_proc_stat_cpu_ticks(text)
    check(ticks == 405 and reason is None,
          f"utime+stime read from fields 14/15: {ticks!r}/{reason!r}")


def test_parse_cpu_ticks_truncated_refuses():
    ticks, reason = lib.parse_proc_stat_cpu_ticks("7 (x) S 1 1 1\n")
    check(ticks is None and reason is not None,
          f"a truncated stat line refuses rather than reporting 0 CPU: "
          f"{ticks!r}/{reason!r}")


# ── parse_loadavg ────────────────────────────────────────────────────────

def test_parse_loadavg_ok():
    load, reason = lib.parse_loadavg("0.05 0.13 0.11 3/234 2089392\n")
    check(load == {"load1": 0.05, "load5": 0.13, "load15": 0.11,
                   "runnable": 3, "total_tasks": 234} and reason is None,
          f"loadavg parsed incl. run-queue length: {load!r}/{reason!r}")


def test_parse_loadavg_malformed_runnable_refuses():
    load, reason = lib.parse_loadavg("0.05 0.13 0.11 3-234 1\n")
    check(load is None and "runnable/total" in reason,
          f"a malformed runnable/total field refuses: {load!r}/{reason!r}")


def test_parse_loadavg_missing_refuses():
    load, reason = lib.parse_loadavg(None)
    check(load is None and reason is not None,
          f"a missing /proc/loadavg refuses: {load!r}/{reason!r}")


# ── backend enumeration ──────────────────────────────────────────────────

def test_backend_pids_direct_children_only():
    ppid_map = {1: 0, 100: 1, 101: 100, 102: 100, 103: 101, 104: 7}
    pids = lib.backend_pids_from_ppid_map(ppid_map, 100)
    check(pids == [101, 102],
          f"children of the postmaster only -- no grandchild (103), no "
          f"unrelated pid (104), not the postmaster itself: {pids}")


def test_backend_pids_empty_map_is_empty_not_an_error():
    check(lib.backend_pids_from_ppid_map({}, 100) == [],
          "an empty ppid map enumerates no backends (which refuses upstream)")
    check(lib.backend_pids_from_ppid_map({100: 1}, None) == [],
          "a missing postmaster pid enumerates no backends")


# ── window_run_delay: the happy path ─────────────────────────────────────

def test_window_sums_per_pid_deltas():
    samples = [sample(1000 * S, {10: 5 * MS, 11: 1 * MS}),
               sample(1030 * S, {10: 9 * MS, 11: 1 * MS}),
               sample(1060 * S, {10: 25 * MS, 11: 4 * MS})]
    r = lib.window_run_delay(samples, 1000 * S, 1060 * S)
    # pid 10: 25-5 = 20 ms, pid 11: 4-1 = 3 ms -> 23 ms
    check(not r["refused"] and approx(r["run_delay_ms"], 23.0),
          f"Sigma run_delay = sum of per-pid deltas (expected 23.0 ms): {r}")
    check(r.get("pid_counts", {}).get("tracked") == 2 and r.get("pids_counted") == 2,
          f"both pids classified tracked: {r.get('pid_counts')}")
    check(approx(r.get("coverage_pct"), 100.0),
          f"coverage reported, not assumed: {r.get('coverage_pct')}")


def test_window_counts_vanished_pid_up_to_disappearance():
    # pid 11 is gone from the last sample: its 1 -> 4 ms delta must still be
    # counted (issue #386: "count its delta up to disappearance or drop it
    # with a counted reason, not silently").
    samples = [sample(0, {10: 0, 11: 1 * MS}),
               sample(30 * S, {10: 2 * MS, 11: 4 * MS}),
               sample(60 * S, {10: 5 * MS})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(approx(r["run_delay_ms"], 8.0),
          f"vanished pid's 3 ms counted alongside the survivor's 5 ms: {r}")
    check(r["pid_counts"].get("vanished") == 1 and
          approx(r["contrib_ms"].get("vanished") if r.get("contrib_ms") else None, 3.0),
          f"the vanished pid is counted AND its contribution is reported "
          f"separately: {r['pid_counts']}/{r['contrib_ms']}")


def test_window_counts_joined_and_transient_pids():
    samples = [sample(0, {10: 0}),
               sample(20 * S, {10: 1 * MS, 12: 10 * MS}),   # transient appears
               sample(40 * S, {10: 2 * MS, 12: 12 * MS, 13: 100 * MS}),
               sample(60 * S, {10: 3 * MS, 13: 107 * MS})]  # 12 gone, 13 joined
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r.get("pid_counts") == {"tracked": 1, "vanished": 0, "joined": 1,
                              "transient": 1, "single": 0, "reset": 0},
          f"joined/transient classified separately from tracked: {r['pid_counts']}")
    # 10: 3ms, 12: 2ms (transient), 13: 7ms (joined) = 12ms
    check(approx(r["run_delay_ms"], 12.0),
          f"all three counted from their own first observation: {r}")


def test_window_single_observation_pid_contributes_zero_but_is_counted():
    samples = [sample(0, {10: 1 * MS}),
               sample(60 * S, {10: 3 * MS, 99: 500 * MS})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(approx(r["run_delay_ms"], 2.0),
          f"a pid seen once contributes no delta (its 500 ms cumulative "
          f"value is NOT a delta): {r}")
    check(r.get("pid_counts", {}).get("single") == 1,
          f"and it is counted under its own reason: {r['pid_counts']}")


def test_window_counter_reset_is_counted_not_negative():
    # Same pid number, new task (pid reuse): the cumulative counters of two
    # different tasks are not subtractable, and a negative delta would
    # silently shrink Sigma run_delay.
    samples = [sample(0, {10: 500 * MS}),
               sample(30 * S, {10: 501 * MS}),
               sample(60 * S, {10: 2 * MS})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["refused"] and "no backend was observed twice" in r["reason"],
          f"a window whose only pid reset refuses rather than reporting a "
          f"negative or 0 total: {r}")
    samples.append(sample(60 * S, {10: 2 * MS, 11: 1 * MS}))
    samples.append(sample(61 * S, {10: 3 * MS, 11: 5 * MS}))
    r = lib.window_run_delay(samples, 0, 61 * S)
    check(r.get("pid_counts", {}).get("reset") == 1 and approx(r["run_delay_ms"], 4.0),
          f"with another live pid present the reset contributes 0 and is "
          f"counted: {r}")


def test_window_aggregates_unreadable_reads_by_reason():
    samples = [sample(0, {10: 0}, unreadable={11: "pid vanished"}),
               sample(60 * S, {10: 5 * MS}, unreadable={12: "pid vanished"})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r.get("unreadable_reads") == {"pid vanished": 2},
          f"unreadable reads are counted by reason, not dropped: {r.get('unreadable_reads')}")


# ── window_run_delay: the bypass suite (each must REFUSE) ────────────────

def test_bypass_empty_sample_list():
    r = lib.window_run_delay([], 0, 60 * S)
    check(r["run_delay_ms"] is None and r["refused"],
          f"no samples at all refuses (it does NOT report 0 ms): {r}")


def test_bypass_sampler_never_ran():
    r = lib.window_run_delay(None, 0, 60 * S)
    check(r["run_delay_ms"] is None and "never started" in r["reason"],
          f"a missing sample list refuses: {r}")


def test_bypass_single_sample():
    r = lib.window_run_delay([sample(0, {10: 5 * MS})], 0, 60 * S)
    check(r["run_delay_ms"] is None and "1 sample" in r["reason"],
          f"one sample is a value, not a rate -- refuses: {r}")


def test_bypass_samples_all_outside_the_window():
    # The partial-range case: the sampler ran, but not during the window the
    # Off-CPU* figure covers (e.g. it started late, or the window is old).
    samples = [sample(0, {10: 0}), sample(10 * S, {10: 1 * MS})]
    r = lib.window_run_delay(samples, 1000 * S, 1060 * S)
    check(r["run_delay_ms"] is None and "0 sample(s) inside" in r["reason"],
          f"samples outside the window refuse rather than being reused: {r}")


def test_bypass_schedstats_disabled_refuses_not_zero():
    # THE case: on both gate boxes sched_schedstats is 0 by default, so
    # every counter reads 0 forever. Reporting 0 ms here would credit the
    # entire residual to "unexplained" on no evidence.
    samples = [sample(0, {10: 0, 11: 0}, enabled=False),
               sample(60 * S, {10: 0, 11: 0}, enabled=False)]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["run_delay_ms"] is None and "schedstats" in r["reason"],
          f"schedstats off refuses even though every delta is a clean 0: {r}")


def test_bypass_schedstats_state_unknown_refuses():
    samples = [sample(0, {10: 0}, enabled=None),
               sample(60 * S, {10: 7 * MS}, enabled=None)]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["run_delay_ms"] is None and "unknown" in r["reason"],
          f"an unknown schedstats state refuses even with a nonzero delta: {r}")


def test_bypass_schedstats_toggled_mid_window_refuses():
    samples = [sample(0, {10: 0}, enabled=False),
               sample(30 * S, {10: 0}, enabled=True),
               sample(60 * S, {10: 9 * MS}, enabled=True)]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["run_delay_ms"] is None and "whole window" in r["reason"],
          f"a mid-window toggle spans two accounting regimes -- refuses: {r}")


def test_bypass_partial_coverage_refuses():
    # Both samples are inside the window but bracket only 10s of 60s: the
    # subtraction would be presented against a window it never covered.
    samples = [sample(0, {10: 0}), sample(10 * S, {10: 5 * MS})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["run_delay_ms"] is None and "span only" in r["reason"],
          f"partial coverage refuses and reports the real span: {r}")
    check(abs(r["covered_s"] - 10.0) < 1e-6 and abs(r["requested_s"] - 60.0) < 1e-6,
          f"the actual and requested spans are both reported: {r}")


def test_bypass_only_single_observation_pids_refuses():
    # Two samples, but no pid appears in BOTH of them (every session opened
    # and closed between ticks). Each pid's first == last, so a naive
    # implementation sums a tidy 0.0 ms and calls it a measurement; there is
    # no delta here at all.
    samples = [sample(0, {10: 400 * MS}), sample(60 * S, {11: 700 * MS})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["run_delay_ms"] is None and "no backend was observed twice" in r["reason"],
          f"a window where no pid is seen twice refuses instead of summing "
          f"to 0 ms: {r}")
    check(r.get("pid_counts", {}).get("single") == 2,
          f"and both single-observation pids are counted: {r.get('pid_counts')}")


def test_bypass_no_backends_enumerated_refuses():
    # The "thing being checked is absent rather than wrong" case: the
    # sampler ran fine, PostgreSQL just had no children visible to it (bad
    # postmaster pid, a container's pid namespace, ...).
    samples = [sample(0, {}), sample(60 * S, {})]
    r = lib.window_run_delay(samples, 0, 60 * S)
    check(r["run_delay_ms"] is None and "no backend was observed twice" in r["reason"],
          f"zero enumerated backends refuses instead of summing to 0: {r}")


def test_bypass_non_positive_window_refuses():
    samples = [sample(0, {10: 0}), sample(60 * S, {10: 1 * MS})]
    r = lib.window_run_delay(samples, 60 * S, 60 * S)
    check(r["run_delay_ms"] is None and "non-positive" in r["reason"],
          f"a zero-length window refuses: {r}")
    r = lib.window_run_delay(samples, None, 60 * S)
    check(r["run_delay_ms"] is None and "missing" in r["reason"],
          f"absent window bounds refuse: {r}")


# ── clock domain ─────────────────────────────────────────────────────────

def test_clock_domain_agreement_and_refusal():
    ok, detail = lib.clock_domain_ok(1_000_000_000_000, 1_000_100_000_000)
    check(ok, f"a 0.1 s skew between the trace clock and ours agrees: {detail}")
    ok, detail = lib.clock_domain_ok(1_000_000_000_000, 1_600_000_000_000)
    check(not ok and "not in the sampler's" in detail,
          f"a 600 s skew refuses -- the two intervals are unrelated: {detail}")
    ok, detail = lib.clock_domain_ok(None, 1)
    check(not ok, f"a missing now_ns refuses: {detail}")


# ── daemon %CPU ──────────────────────────────────────────────────────────

def test_cpu_pct_and_its_refusals():
    pct, reason = lib.cpu_pct(1000, 1100, 10.0, 100)
    check(abs(pct - 10.0) < 1e-9 and reason is None,
          f"100 ticks at 100 Hz over 10 s = 10% of one core: {pct!r}")
    pct, reason = lib.cpu_pct(1100, 1000, 10.0, 100)
    check(pct is None and "backwards" in reason,
          f"a backwards counter refuses rather than reporting 0% CPU: {reason}")
    pct, reason = lib.cpu_pct(None, 1000, 10.0, 100)
    check(pct is None, f"a missing endpoint refuses: {reason}")
    pct, reason = lib.cpu_pct(1000, 1100, 0.0, 100)
    check(pct is None and "span" in reason, f"a zero span refuses: {reason}")
    pct, reason = lib.cpu_pct(1000, 1100, 10.0, 0)
    check(pct is None and "clock tick" in reason,
          f"an unusable SC_CLK_TCK refuses: {reason}")


# ── decompose ────────────────────────────────────────────────────────────

def test_decompose_reports_all_three_figures():
    r = lib.decompose(100.0, {"run_delay_ms": 40.0, "refused": False})
    check(abs(r["unexplained_ms"] - 60.0) < 1e-9 and
          abs(r["explained_pct"] - 40.0) < 1e-9 and not r["refused"],
          f"offcpu 100 - run_delay 40 = 60 unexplained (40% explained): {r}")
    line = lib.format_decomposition(r)
    check("offcpu_ms=100.00" in line and "run_delay_ms=40.00" in line
          and "unexplained_ms=60.00" in line,
          f"the printed line shows the decomposition, not a verdict: {line}")


def test_decompose_refuses_when_run_delay_refused():
    # The important one: an unavailable run_delay must NOT become 0, which
    # would silently credit 100% of the residual to "unexplained" and make
    # the instrument confirm the hypothesis it exists to test.
    refusal = {"run_delay_ms": None, "refused": True,
               "reason": "kernel.sched_schedstats=0"}
    r = lib.decompose(250.0, refusal)
    check(r["unexplained_ms"] is None and r["refused"]
          and "sched_schedstats=0" in r["reason"],
          f"a refused run_delay propagates as a refusal: {r}")
    check(r["unexplained_ms"] != 250.0,
          "and specifically does NOT come out equal to offcpu_ms")
    check("REFUSED" in lib.format_decomposition(r),
          f"the printed line says REFUSED: {lib.format_decomposition(r)}")


def test_decompose_refuses_without_offcpu():
    r = lib.decompose(None, {"run_delay_ms": 40.0, "refused": False})
    check(r["unexplained_ms"] is None and "offcpu_ms unavailable" in r["reason"],
          f"a missing offcpu_ms refuses: {r}")
    r = lib.decompose(10.0, None)
    check(r["unexplained_ms"] is None and "no run_delay measurement" in r["reason"],
          f"no run_delay measurement at all refuses: {r}")


def test_decompose_zero_offcpu_is_flagged_degenerate():
    r = lib.decompose(0.0, {"run_delay_ms": 0.0, "refused": False})
    check(r["unexplained_ms"] == 0.0 and r["degenerate"]
          and r["explained_pct"] is None,
          f"offcpu ~0 is marked degenerate -- 0 - 0 = 0 is not agreement: {r}")


def test_decompose_over_explained_window_is_reported_as_negative():
    # run_delay > Off-CPU* is real and informative (run-queue delay accrued
    # while the backend was inside a wait event is booked to that event, not
    # to the residual). Reported as a negative remainder, never clamped to
    # 0 -- a clamp would hide exactly that overlap.
    r = lib.decompose(10.0, {"run_delay_ms": 25.0, "refused": False})
    check(abs(r["unexplained_ms"] + 15.0) < 1e-9 and not r["refused"],
          f"an over-explained window reports -15 ms, not 0: {r}")


# ── ProcReader against a fixture /proc tree ──────────────────────────────

def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def _fake_proc(root, schedstats="1\n", with_loadavg=True,
               child_schedstat=True):
    """postmaster 100, backends 101/102, grandchild 103, unrelated 104,
    daemon 200."""
    _write(os.path.join(root, "100", "stat"),
           _stat_line(100, "postgres", 1, 1, 1))
    _write(os.path.join(root, "101", "stat"),
           _stat_line(101, "postgres: 18/main", 100, 10, 2))
    _write(os.path.join(root, "102", "stat"),
           _stat_line(102, "postgres: 18/main", 100, 20, 3))
    _write(os.path.join(root, "103", "stat"),
           _stat_line(103, "child of 101", 101, 1, 1))
    _write(os.path.join(root, "104", "stat"),
           _stat_line(104, "unrelated", 1, 1, 1))
    _write(os.path.join(root, "200", "stat"),
           _stat_line(200, "pg_wait_tracer", 1, 310, 95))
    _write(os.path.join(root, "101", "schedstat"), "900 1500000 42\n")
    if child_schedstat:
        _write(os.path.join(root, "102", "schedstat"), "800 2500000 37\n")
    _write(os.path.join(root, "103", "schedstat"), "1 999999999 1\n")
    if schedstats is not None:
        _write(os.path.join(root, "sys", "kernel", "sched_schedstats"),
               schedstats)
    if with_loadavg:
        _write(os.path.join(root, "loadavg"), "1.50 0.90 0.40 5/300 104\n")
    # non-numeric noise that must be ignored by the scan
    _write(os.path.join(root, "self", "stat"), _stat_line(1, "init", 0, 1, 1))


def test_reader_snapshot_on_fixture_proc():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _fake_proc(root)
        r = probe.ProcReader(root, clk_tck=100)
        s = r.snapshot(100, daemon_pid=200, t_ns=1234)
        check(s["schedstats_enabled"] is True, f"fixture schedstats read as on: {s['schedstats_enabled']}")
        check(sorted(s["run_delay_ns"]) == [101, 102],
              f"only the postmaster's own children sampled -- not the "
              f"grandchild 103 whose run_delay is huge: {sorted(s['run_delay_ns'])}")
        check(s["run_delay_ns"].get(101) == 1500000
              and s["run_delay_ns"].get(102) == 2500000,
              f"field 2 taken from each backend: {s['run_delay_ns']}")
        check(s["daemon_cpu_ticks"] == 405,
              f"daemon utime+stime recorded: {s['daemon_cpu_ticks']}")
        check(s["load"]["runnable"] == 5,
              f"run-queue length recorded: {s['load']}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_reader_counts_a_pid_whose_schedstat_is_gone():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _fake_proc(root, child_schedstat=False)   # 102 enumerated, unreadable
        s = probe.ProcReader(root, clk_tck=100).snapshot(100)
        check(102 in s["unreadable"] and 102 not in s["run_delay_ns"],
              f"a pid that vanished between enumeration and read is counted "
              f"as unreadable, not as 0: {s['unreadable']}")
        check(s["backends_enumerated"] == 2,
              f"the enumeration count still reports both: {s['backends_enumerated']}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_reader_refuses_when_schedstats_sysctl_absent():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _fake_proc(root, schedstats=None)
        s = probe.ProcReader(root, clk_tck=100).snapshot(100)
        check(s["schedstats_enabled"] is None
              and "CONFIG_SCHEDSTATS" in s["schedstats_reason"],
              f"no sched_schedstats file -> unknown state recorded: {s['schedstats_reason']}")
        r = lib.window_run_delay([s, dict(s, t_ns=s["t_ns"] + 60 * S)],
                                 s["t_ns"], s["t_ns"] + 60 * S)
        check(r["run_delay_ms"] is None,
              f"and the window built from it refuses: {r['reason']}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_reader_missing_proc_root_refuses():
    # No Linux / no /proc at all (a Mac, or a wrong --proc-root): must come
    # back empty with a reason, never as a clean zero reading.
    r = probe.ProcReader("/nonexistent-proc-root", clk_tck=100)
    pids, reason = r.backend_pids(100)
    check(pids == [] and reason and "cannot list" in reason,
          f"an absent /proc enumerates nothing and says why: {pids}/{reason}")
    s = r.snapshot(100)
    check(s["run_delay_ns"] == {} and s["schedstats_enabled"] is None,
          f"the snapshot is empty and the schedstats state is unknown: {s}")


def test_reader_proc_without_numeric_entries_refuses():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _write(os.path.join(root, "self", "stat"), _stat_line(1, "init", 0, 1, 1))
        pids, reason = probe.ProcReader(root, clk_tck=100).backend_pids(100)
        check(pids == [] and "no numeric pid entries" in reason,
              f"a /proc with no pid dirs says so: {pids}/{reason}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_reader_unparseable_stat_is_counted_not_fatal():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _fake_proc(root)
        _write(os.path.join(root, "102", "stat"), "garbage with no parens\n")
        pids, reason = probe.ProcReader(root, clk_tck=100).backend_pids(100)
        check(pids == [101] and reason and "unreadable during the scan" in reason,
              f"an unparseable /proc/<pid>/stat drops that pid WITH a counted "
              f"reason and keeps going: {pids}/{reason}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ── sampler thread ───────────────────────────────────────────────────────

def test_sampler_records_its_own_crash_instead_of_raising():
    # The "one component failing makes this hang or skip rather than fail"
    # case: a sampler that died must leave windows REFUSING for lack of
    # samples, and must never take the rehearsal down with it.
    s = probe.SchedstatSampler(100, interval_s=0.01, proc_root="/nonexistent")

    def boom(*a, **k):
        raise RuntimeError("proc read exploded")
    s.reader.snapshot = boom
    check(s.sample_once() is None, "a crashing snapshot returns None, not a sample")
    check(s.errors and "exploded" in s.errors[0],
          f"and the crash is recorded: {s.errors}")
    r = s.window(0, 60 * S)
    check(r["run_delay_ms"] is None,
          f"windows over a dead sampler refuse: {r['reason']}")


def test_sampler_collects_and_windows_on_fixture_proc():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _fake_proc(root)
        s = probe.SchedstatSampler(100, daemon_pid=200, interval_s=0.01,
                                   proc_root=root)
        t0 = None
        for i in range(3):
            snap = s.sample_once()
            if t0 is None:
                t0 = snap["t_ns"]
            # advance the fixture counters so the delta is nonzero and the
            # timestamps span the window
            snap["t_ns"] = t0 + i * 30 * S
            snap["run_delay_ns"][101] = 1500000 + i * 1000000
        r = s.window(t0, t0 + 60 * S)
        check(not r["refused"] and approx(r["run_delay_ms"], 2.0),
              f"the sampler's own series windows to pid 101's 2 ms delta "
              f"(102 flat): {r}")
        ld = s.load_series(t0, t0 + 60 * S)
        check(ld.get("max_runnable") == 5 and approx(ld.get("mean_load1"), 1.5, 1e-9),
              f"the load series comes back over the same window: {ld}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_sampler_series_is_bounded():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    try:
        _fake_proc(root)
        s = probe.SchedstatSampler(100, interval_s=0.01, proc_root=root,
                                   max_samples=4)
        for _ in range(9):
            s.sample_once()
        check(len(s.snapshot_list()) == 4,
              f"the sample series stays bounded: {len(s.snapshot_list())}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ── the CLI: refusal must be visible in the exit code ────────────────────

def test_cli_exits_2_when_it_cannot_measure():
    # The dependency-missing case (the analogue of a tool exiting 126/127):
    # the probe is asked to read a /proc that is not there. It must not
    # print a tidy 0 and exit 0.
    cli = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "schedstat_probe.py")
    p = subprocess.run([sys.executable, cli, "--parent-pid", "100",
                        "--duration", "0.05", "--interval", "0.01",
                        "--proc-root", "/nonexistent-proc-root",
                        "--offcpu-ms", "123.0"],
                       capture_output=True, text=True, timeout=60)
    check(p.returncode == 2,
          f"the CLI exits 2 (could not measure), not 0: rc={p.returncode} "
          f"stderr={p.stderr[-300:]}")
    try:
        out = json.loads(p.stdout)
    except Exception as e:
        out = None
        check(False, f"the CLI still emits parseable JSON on a refusal: {e}")
    if out is not None:
        check(out["run_delay"]["run_delay_ms"] is None,
              f"and the JSON carries a refusal, not a 0: {out['run_delay']}")
        check(out["decomposition"]["unexplained_ms"] is None,
              f"the decomposition refuses too: {out['decomposition']}")


def test_cli_measures_on_a_fixture_proc():
    root = tempfile.mkdtemp(prefix="pgwt_fakeproc_")
    cli = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "schedstat_probe.py")
    try:
        _fake_proc(root)
        p = subprocess.run([sys.executable, cli, "--parent-pid", "100",
                            "--duration", "0.2", "--interval", "0.02",
                            "--proc-root", root, "--daemon-pid", "200"],
                           capture_output=True, text=True, timeout=60)
        check(p.returncode == 0,
              f"the CLI exits 0 when it could measure: rc={p.returncode} "
              f"stderr={p.stderr[-300:]}")
        out = json.loads(p.stdout)
        check(out["run_delay"]["run_delay_ms"] == 0.0,
              f"a static fixture has a real 0 ms delta -- reported as 0, not "
              f"refused: {out['run_delay']}")
        check(out["run_delay"]["pid_counts"]["tracked"] == 2,
              f"both fixture backends tracked: {out['run_delay']['pid_counts']}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ── record_window: reporting code must never be able to fail a run ───────

class _FakeSampler:
    """Minimal stand-in for SchedstatSampler with scriptable answers."""

    def __init__(self, window=None, raises=False, pct=(12.5, None),
                 load=None, errors=()):
        self._window = window or {"run_delay_ms": 40.0, "refused": False,
                                  "reason": None, "covered_s": 58.0}
        self._raises = raises
        self._pct = pct
        self._load = load or {"points": [], "max_runnable": 3,
                              "mean_load1": 1.25, "reason": None}
        self.errors = list(errors)

    def window(self, f, t):
        if self._raises:
            raise RuntimeError("sampler blew up mid-window")
        return self._window

    def daemon_cpu_pct(self, f, t):
        return self._pct

    def load_series(self, f, t):
        return self._load


def test_record_window_writes_all_three_figures():
    root = tempfile.mkdtemp(prefix="pgwt_rec_")
    try:
        out = os.path.join(root, "offcpu_decomposition.jsonl")
        now = 1_700_000_000 * S
        lines = []
        rec = probe.record_window(_FakeSampler(), now - 60 * S, now,
                                  offcpu_ms=100.0, has_measured_cpu=True,
                                  trace_now_ns=now, wall_now_ns=now,
                                  offset_s=120.0, out_path=out,
                                  printer=lines.append)
        d = rec["decomposition"]
        check(d["offcpu_ms"] == 100.0 and d["run_delay_ms"] == 40.0
              and d["unexplained_ms"] == 60.0,
              f"all three figures recorded for the window: {d}")
        check(rec["daemon_cpu_pct"] == 12.5 and rec["load_max_runnable"] == 3
              and rec["load_mean_1min"] == 1.25,
              f"daemon %CPU and the load/run-queue series recorded too: {rec}")
        check(rec["recorder_error"] is None, f"no recorder error: {rec['recorder_error']}")
        with open(out) as f:
            written = [json.loads(l) for l in f if l.strip()]
        check(len(written) == 1 and written[0]["decomposition"]["unexplained_ms"] == 60.0,
              f"and the window is already on disk (flushed per window, so a "
              f"killed run keeps it): {written}")
        check(lines and "does NOT gate the verdict" in lines[0],
              f"the printed line says it is informational: {lines[:1]}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_record_window_survives_a_crashing_sampler():
    # Without this guard the exception would propagate into
    # demo_rehearsal.py's _conservation_sampler_loop, which records ANY
    # exception as a FAILING sample -- i.e. reporting-only code could fail
    # an otherwise clean rehearsal. It must come back as a refusal instead.
    lines = []
    rec = probe.record_window(_FakeSampler(raises=True), 0, 60 * S,
                              offcpu_ms=100.0, has_measured_cpu=True,
                              trace_now_ns=1, wall_now_ns=1, offset_s=1.0,
                              printer=lines.append)
    check(rec["recorder_error"] and "blew up" in rec["recorder_error"],
          f"the crash is recorded: {rec['recorder_error']}")
    check(rec["decomposition"]["unexplained_ms"] is None
          and rec["decomposition"]["refused"],
          f"and the decomposition refuses rather than inventing a number: "
          f"{rec['decomposition']}")


def test_record_window_refuses_on_a_foreign_clock_domain():
    # The window bounds come from the trace, the samples from our own
    # CLOCK_REALTIME. If those drift apart the subtraction is between two
    # unrelated intervals.
    now = 1_700_000_000 * S
    rec = probe.record_window(_FakeSampler(), now - 60 * S, now,
                              offcpu_ms=100.0, has_measured_cpu=True,
                              trace_now_ns=now, wall_now_ns=now + 600 * S,
                              offset_s=1.0, printer=lambda *_: None)
    check(rec["decomposition"]["unexplained_ms"] is None
          and "clock" in rec["run_delay"]["reason"],
          f"a mismatched clock domain refuses: {rec['run_delay']}")


def test_record_window_unwritable_path_does_not_raise():
    rec = probe.record_window(_FakeSampler(), 0, 60 * S, offcpu_ms=1.0,
                              has_measured_cpu=True, trace_now_ns=1,
                              wall_now_ns=1, offset_s=1.0,
                              out_path="/nonexistent-dir/x.jsonl",
                              printer=lambda *_: None)
    check(rec["recorder_error"] and "write" in rec["recorder_error"],
          f"an unwritable artifact path is recorded, not raised: "
          f"{rec['recorder_error']}")


# ── the gate that must NOT exist ─────────────────────────────────────────

def test_decomposition_does_not_gate_the_rehearsal_verdict():
    """issue #386 is explicit: report the numbers, do NOT add a bound yet.
    A later change that quietly wires the decomposition into the verdict
    has to fail this test first."""
    clean_sample = {
        "ok": True, "offset_s": 10.0, "offcpu": {"available": True},
        "offcpu_decomposition": {
            "offcpu_ms": 5000.0, "run_delay_ms": 1.0,
            "unexplained_ms": 4999.0, "explained_pct": 0.02,
            "refused": False, "degenerate": False, "reason": None},
    }
    check_result = drlib.build_demo_conservation_check(
        [clean_sample], {"ok": True})
    check(check_result["ok"],
          f"a window with a 4999 ms unexplained remainder still passes -- "
          f"nothing gates on the decomposition: {check_result}")
    refused_sample = dict(clean_sample, offcpu_decomposition={
        "offcpu_ms": 5000.0, "run_delay_ms": None, "unexplained_ms": None,
        "refused": True, "reason": "kernel.sched_schedstats=0"})
    check_result = drlib.build_demo_conservation_check(
        [refused_sample], {"ok": True})
    check(check_result["ok"],
          f"and a REFUSED decomposition does not fail the run either (it is "
          f"not a gate in either direction): {check_result}")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            print(f"--- {name} ---")
            fn()
    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
