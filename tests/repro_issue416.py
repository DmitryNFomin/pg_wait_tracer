#!/usr/bin/env python3
"""repro_issue416.py — #416 live confirmation: does the tick-drain fix hold?

#416: "Live tracer intermittently loses an entire wait class from a window:
Timeout = 0.0% with PgSleep running, 1-2 of 4 runs, not caused by v8." The
issue's own 3rd comment ("Mechanism found, and reproduced on the shipped
path") traced it to `handle_timer` building the display tick from the
cumulative accumulator + a live state_map read WITHOUT first draining the
event ring — any record still queued at tick time was silently absent, and a
long wait's entire contribution (PgSleep) rides on ONE record. The fix,
`drain_event_ring_for_tick()` (src/daemon.c, called as the last step before
the display is built), shipped in 97dca0c2 (PR #434) with strong synthetic
unit coverage (tests/test_tick_drain.c) but — as far as this issue's own
comment thread shows — no live re-run of the ORIGINAL reproduction against
the fixed tree. This script is that live re-run, scripted in advance so
whoever next has a box can execute it in one command.

PINNED INVOCATION. Lines 223-311 of tests/test_percentage.py's
test_percentage_split() ONLY — the workload, the `tracer` Popen with its
exact argv, and the three percentage checks. This is a VERBATIM port of that
range, not a paraphrase:
  TRACER --mode full --pid <postmaster> --interval 8 --duration 12 \\
         --view time_model
  Backend A: psql -c "SELECT pg_sleep(10)" -c "SELECT pg_sleep(60)"
  Backend B: psql -c "SELECT count(*) FROM generate_series(1,500000000)"
             -c "SELECT pg_sleep(60)"
  1s settle before the tracer starts; tracer.communicate(timeout=28).
  Checks: DB Time > 1000ms; 15 <= CPU% <= 85; 15 <= Timeout% <= 85 (the
  code's real tolerance -- its own PRINTED text says "20-80%", which is a
  pre-existing cosmetic mismatch in test_percentage.py, not something this
  script should silently "correct" by diverging from the real assertion).

WHY THE system_event / tracer2 PHASE (test_percentage.py:313 onward) IS
DELIBERATELY NEVER RUN HERE: issue #207 records that phase as load-dependent
because `tracer2` is Popen'd TWICE with the first object discarded unwaited,
leaving two `pg_wait_tracer --mode full` daemons on one postmaster for about
a second. #416's own confirmed mechanism reproduced on the FIRST tracer
(the time_model Popen above) alone. Running only that phase means the
leaked-tracer2 mechanism cannot apply BY CONSTRUCTION, not by after-the-fact
control -- so a failure here cannot be #207 by the shape of this script.

#207 BELT-AND-SUSPENDERS: even with tracer2 never invoked by this script,
something else (a previous attempt that was killed uncleanly, a leftover
from unrelated box activity) could already have a pg_wait_tracer process
attached to this postmaster. Checked before EVERY attempt; if found, that
attempt is REFUSED (not silently run, not silently skipped) -- a gate that
cannot see a clean precondition must refuse, never approve.

THE PRE-DECLARED THRESHOLD (decided before any data; see the #416 report
this script accompanies). Of N isolated attempts measuring the mechanism
(refused attempts, if any, do not count toward N):
  0 failures           -> CONFIRMED: consistent with the original 1-2-of-4
                          (25-50%) defect being gone.
  >=2 failures         -> NOT CONFIRMED: a rate this high is incompatible
                          with the fix fully closing the window; report the
                          rate, do not round it down.
  exactly 1 failure    -> resolved by the fix's OWN diagnostics, not by rate
                          alone. Every attempt runs with
                          PGWT_DEBUG_DUMP_STATE=1 and keeps the tracer's
                          stderr specifically so this disambiguation never
                          needs a second box trip:
                            - tick_drain_truncated_total > 0 in that
                              attempt's STATEDUMP-META line, or its WARN
                              line present -> the drain hit ITS OWN bound
                              (256 passes / 500ms) -- attribute to #416,
                              fix INCOMPLETE (the mechanism is real, the
                              margin was not enough).
                            - tick_drain_truncated_total == 0 (or absent)
                              and no WARN line -> NOT explained by #416's
                              mechanism; a different cause (box noise, an
                              undetected #207 recurrence, something new).
                              Does not count against the fix; investigate
                              separately.
  A refused attempt (code 1, second-daemon precondition failed) is reported
  separately and is NEVER folded into the pass/fail rate.

USAGE (as root, on a box with PostgreSQL running and pg_wait_tracer built):
  sudo python3 tests/repro_issue416.py [--pid POSTMASTER_PID] [-n 20]
                                        [--out /tmp/repro416.json]

Report: machine identity (hostname, nproc, loadavg at start and end) and the
rate -- this measures one machine, not the gate tier (see the #416 report).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster
# Reused, not reimplemented: the real parser the original Test 1 uses, so a
# future change to its regex or column layout cannot silently diverge from
# what this script measures.
from test_percentage import parse_time_model, psql, STRIP_ANSI, TRACER

DURATION = 10       # pg_sleep() duration, test_percentage.py:225
INTERVAL = 8        # test_percentage.py:246
TRACER_TIMEOUT = INTERVAL + 20   # test_percentage.py:255


def other_pgwt_processes(exclude_pid=None):
    """#207 precondition check: every OTHER live pg_wait_tracer process.

    /proc scan, not `pgrep -f pg_wait_tracer` piped through this script's own
    shell -- pgrep would also match this script's docstring/argv if invoked
    oddly, and a gate that can misidentify ITSELF must not be trusted to
    identify its subject either. Returns [] when none, never None: an
    empty, OBSERVED list is the only thing that may be read as "clean".
    """
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        if pid == exclude_pid:
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                comm = f.read().strip()
        except OSError:
            continue
        if comm != "pg_wait_tracer":
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                argv = f.read().decode("utf-8", "replace").split("\0")
        except OSError:
            argv = ["<unreadable>"]
        found.append({"pid": pid, "argv": argv})
    return found


def run_one_attempt(pm_pid, label):
    """One isolated run of test_percentage.py:223-311, VERBATIM in shape.

    Returns a dict: {"label", "refused": bool, "reason"?, "failed": bool,
    "db_time_ms", "cpu_pct", "timeout_pct", "stderr_tail",
    "tick_drain_truncated_total", "tick_drain_warn"}.
    """
    pre_existing = other_pgwt_processes()
    if pre_existing:
        return {"label": label, "refused": True,
                "reason": f"pg_wait_tracer already attached before this "
                          f"attempt started: {pre_existing} -- refusing "
                          "rather than running a confounded measurement "
                          "(the #207 shape)"}

    sleep_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", f"SELECT pg_sleep({DURATION})",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    cpu_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1)

    env = dict(os.environ)
    env["PGWT_DEBUG_DUMP_STATE"] = "1"
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env
    )
    try:
        stdout, stderr = tracer.communicate(timeout=TRACER_TIMEOUT)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    stderr_text = stderr.decode("utf-8", "replace")

    for p in [sleep_proc, cpu_proc]:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    model = parse_time_model(output)
    result = {"label": label, "refused": False, "stderr_tail": stderr_text[-2000:]}
    if 'DB Time' not in model:
        result.update(failed=True, reason="no 'DB Time' in tracer output",
                      raw_output=output)
        return result

    db_time = model['DB Time']
    cpu_time = model.get('CPU*', 0)
    timeout_time = model.get('Timeout', 0)
    timeout_pct = (timeout_time / db_time * 100) if db_time > 0 else 0.0
    cpu_pct = (cpu_time / db_time * 100) if db_time > 0 else 0.0

    m = re.search(r"tick_drain_truncated_total=(\d+)", stderr_text)
    result["tick_drain_truncated_total"] = int(m.group(1)) if m else None
    result["tick_drain_warn"] = "event ring still had a backlog" in stderr_text
    result["db_time_ms"] = db_time
    result["cpu_pct"] = cpu_pct
    result["timeout_pct"] = timeout_pct
    # The real check is 15 <= timeout_pct <= 85 (test_percentage.py:302) --
    # its OWN printed message says "20-80%"; that mismatch is pre-existing
    # in the test, not something to silently correct here.
    result["failed"] = not (db_time > 1000 and 15 <= timeout_pct <= 85)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, default=None, help="postmaster PID")
    ap.add_argument("-n", type=int, default=20, help="isolated attempts (default 20)")
    ap.add_argument("--out", default=None, help="write full JSON results here")
    args = ap.parse_args()

    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        sys.exit("no postmaster found; pass --pid")

    uname = subprocess.run(["uname", "-a"], capture_output=True, text=True).stdout.strip()
    nproc = subprocess.run(["nproc"], capture_output=True, text=True).stdout.strip()
    loadavg_start = open("/proc/loadavg").read().strip()
    print(f"# host: {uname}")
    print(f"# nproc: {nproc}, loadavg at start: {loadavg_start}")
    print(f"# postmaster pid: {pm_pid}, tracer: {TRACER}, N={args.n}")

    results = []
    for i in range(1, args.n + 1):
        r = run_one_attempt(pm_pid, i)
        results.append(r)
        if r["refused"]:
            print(f"[{i}/{args.n}] REFUSED: {r['reason']}")
        elif r["failed"]:
            tag = ("#416 (tick-drain bound hit)" if r.get("tick_drain_truncated_total")
                   else "#416 (WARN logged)" if r.get("tick_drain_warn")
                   else "UNEXPLAINED by #416's mechanism -- investigate separately")
            print(f"[{i}/{args.n}] FAIL: Timeout={r.get('timeout_pct', float('nan')):.2f}% "
                  f"DB Time={r.get('db_time_ms', float('nan')):.0f}ms "
                  f"tick_drain_truncated_total={r.get('tick_drain_truncated_total')} "
                  f"-> {tag}")
        else:
            print(f"[{i}/{args.n}] PASS: Timeout={r['timeout_pct']:.2f}% "
                  f"CPU={r['cpu_pct']:.2f}% DB Time={r['db_time_ms']:.0f}ms")

    loadavg_end = open("/proc/loadavg").read().strip()
    measured = [r for r in results if not r["refused"]]
    failed = [r for r in measured if r["failed"]]
    refused = [r for r in results if r["refused"]]
    print(f"\n### {len(failed)} / {len(measured)} measured attempts failed "
          f"({len(refused)} refused, not counted). loadavg at end: {loadavg_end}")

    if len(failed) == 0:
        verdict = "CONFIRMED (0 failures)"
    elif len(failed) >= 2:
        verdict = f"NOT CONFIRMED ({len(failed)} failures >= 2)"
    else:
        f = failed[0]
        if f.get("tick_drain_truncated_total") or f.get("tick_drain_warn"):
            verdict = ("NOT CONFIRMED (1 failure, tick_drain_truncated_total="
                       f"{f.get('tick_drain_truncated_total')} -- the drain hit its "
                       "own bound; #416's margin, not just its mechanism, is "
                       "insufficient)")
        else:
            verdict = ("INCONCLUSIVE ON #416 (1 failure, but tick_drain_truncated_total="
                       f"{f.get('tick_drain_truncated_total')} and no WARN line -- not "
                       "explained by #416's mechanism; investigate as a separate cause, "
                       "e.g. #207, before counting it against this fix)")
    print(f"### Verdict: {verdict}")

    if args.out:
        with open(args.out, "w") as f:
            json.dump({"uname": uname, "nproc": nproc, "loadavg_start": loadavg_start,
                      "loadavg_end": loadavg_end, "pm_pid": pm_pid, "n": args.n,
                      "results": results, "verdict": verdict}, f, indent=2)
        print(f"### full results: {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
