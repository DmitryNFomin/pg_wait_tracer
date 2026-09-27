#!/usr/bin/env python3
"""repro202.py — issue #202 live reproduction + mechanism measurement.

Runs test_multi_window's Test 3 shape (pgbench, --window 5s --interval 5
--count 3, view time_model) N times and, for every tick, reports BOTH:

  observed_excess = Sigma(top-level ':'-free rows) - DB Time
  predicted       = deficit(prev tick) - deficit(this tick)

where deficit(t) = Sigma over the open we==0 state_map stretches at tick t of
(open_ns - cpu_open) — the off-CPU remainder that the OPEN path keeps out of
the CPU* row while the CLOSED record files the whole wall under it
(src/map_reader.c cpu_open vs src/event_stream.c .cpu_ns = dur).

deficit is read from the daemon's own PGWT_DEBUG_DUMP_STATE tick trace
(STATEDUMP-TICK lines), so the excess is MEASURED, not inferred.

Usage: sudo python3 repro202.py [--pid PM_PID] [-n 3]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TRACER = os.environ.get("PGWT_TRACER", "./pg_wait_tracer")
STRIP_ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')
TICK_RE = re.compile(r'^--- \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ---$')
STATEDUMP_TICK = re.compile(
    r'STATEDUMP-TICK: pid=(\d+) open_ns=(\d+) cpu_ns_total=\d+ '
    r'on_cpu_age=-?\d+ exact=\d+ last_cpu_ns=\d+ cpu_open=(\d+)')
ROW_RE = re.compile(r'^\s+(\S[^\d]*?)\s+([\d.]+)\s+[\d.]+%')


def find_postmaster():
    out = subprocess.run(["pgrep", "-x", "postgres"], capture_output=True)
    pids = [int(p) for p in out.stdout.split()]
    if not pids:
        raise SystemExit("no postgres running")
    # postmaster = the one whose ppid is not another postgres
    for p in pids:
        try:
            ppid = int(open(f"/proc/{p}/stat").read().split()[3])
        except OSError:
            continue
        if ppid not in pids:
            return p
    return min(pids)


def parse_ticks(stdout_text):
    """[(rows_dict, db_time_ms)] per printed tick, first window column."""
    ticks, cur = [], []
    for line in stdout_text.split('\n'):
        if TICK_RE.match(line.strip()):
            if cur:
                ticks.append(cur)
            cur = []
        else:
            cur.append(line)
    if cur:
        ticks.append(cur)

    out = []
    for block in ticks:
        rows = {}
        for line in block:
            if not line.startswith('  ') or line.strip().startswith('-'):
                continue
            m = ROW_RE.match(line)
            if not m:
                continue
            name = m.group(1).strip()
            if not name or name.startswith('('):
                continue
            rows[name] = float(m.group(2))
        if 'DB Time' in rows:
            out.append(rows)
    return out


WRAP = 1 << 64
UNDERFLOW_FLOOR = 1 << 63   # above this, `now - last_ts` underflowed


def parse_deficits(stderr_text):
    """Per printed STATEDUMP tick: (sum(open_ns - cpu_open) in ms, n_wrapped).

    STATEDUMP-SCAN marks the end of one pgwt_read_state_map pass, so the
    TICK lines BETWEEN two SCAN lines belong to one tick.

    An open_ns above 2^63 is src/map_reader.c's unguarded `now -
    sval.last_ts` having underflowed (state_map's BPF timestamp is a few
    tens of microseconds AHEAD of the userspace CLOCK_MONOTONIC read).
    Counted and reported separately, never summed into the deficit.
    """
    deficits, cur, wrapped, nwrap, seen_any = [], 0.0, [], 0, False
    for line in stderr_text.split('\n'):
        m = STATEDUMP_TICK.search(line)
        if m:
            open_ns, cpu_open = int(m.group(2)), int(m.group(3))
            if open_ns >= UNDERFLOW_FLOOR:
                nwrap += 1
                seen_any = True
                continue
            cur += max(0, open_ns - cpu_open) / 1e6
            seen_any = True
            continue
        if 'STATEDUMP-SCAN' in line and 'state_fd=' in line:
            deficits.append(cur)
            wrapped.append(nwrap)
            cur, nwrap, seen_any = 0.0, 0, False
    if seen_any:
        deficits.append(cur)
        wrapped.append(nwrap)
    return deficits, wrapped


def underflow_rows(stderr_text):
    """Every underflowed open_ns, as (pid, ns_in_the_future)."""
    out = []
    for line in stderr_text.split('\n'):
        m = STATEDUMP_TICK.search(line)
        if m and int(m.group(2)) >= UNDERFLOW_FLOOR:
            out.append((int(m.group(1)), WRAP - int(m.group(2))))
    return out


def one_run(pm_pid, clients=4, bench_sec=30, interval=5, count=3):
    pgb = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(clients), "-T", str(bench_sec)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    env = dict(os.environ, PGWT_DEBUG_DUMP_STATE="1")
    cmd = [TRACER, "--mode", "full", "--pid", str(pm_pid),
           "--interval", str(interval), "--window", "5s",
           "--count", str(count), "--view", "time_model"]
    r = subprocess.run(cmd, capture_output=True, env=env,
                       timeout=interval * count + 25)
    pgb.wait()
    return (STRIP_ANSI.sub('', r.stdout.decode('utf-8', 'replace')),
            STRIP_ANSI.sub('', r.stderr.decode('utf-8', 'replace')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=None)
    ap.add_argument("-n", type=int, default=3)
    ap.add_argument("--out", default="repro202.json")
    a = ap.parse_args()
    pm = a.pid or find_postmaster()

    results = []
    for run in range(1, a.n + 1):
        so, se = one_run(pm)
        ticks = parse_ticks(so)
        deficits, wrapped = parse_deficits(se)
        uf = underflow_rows(se)
        print(f"=== run {run}: {len(ticks)} printed window(s), "
              f"{len(deficits)} state_map tick(s) ===")
        for i, rows in enumerate(ticks):
            db = rows['DB Time']
            top = {k: v for k, v in rows.items()
                   if ':' not in k and k not in ('DB Time', 'Idle')}
            s = sum(top.values())
            excess = s - db
            err = 100.0 * abs(excess) / db if db else 0.0
            # the state_map tick that produced THIS window's current snapshot
            # is the last one; the previous window boundary is len(deficits)
            # minus (number of remaining windows) ticks back.
            print(f"  window {i}: DB={db:.0f}ms sum={s:.0f}ms "
                  f"excess={excess:+.0f}ms err={err:.1f}% rows={sorted(top)}")
            results.append(dict(run=run, window=i, db_ms=db, sum_ms=s,
                                excess_ms=excess, err_pct=err,
                                rows={k: v for k, v in top.items()}))
        print(f"  deficits per state_map tick (ms): "
              f"{[round(d, 1) for d in deficits]}")
        drops = [deficits[i - 1] - deficits[i] for i in range(1, len(deficits))]
        print(f"  deficit drops (ms):               {[round(d, 1) for d in drops]}")
        print(f"  underflowed open_ns per tick:     {wrapped}"
              f"  (max {max([u[1] for u in uf], default=0) / 1e3:.1f} us "
              f"into the future, n={len(uf)})")
        if results:
            results[-1]['deficits_ms'] = [round(d, 1) for d in deficits]
            results[-1]['deficit_drops_ms'] = [round(d, 1) for d in drops]
            results[-1]['underflows_per_tick'] = wrapped
            results[-1]['underflow_max_ns'] = max([u[1] for u in uf], default=0)

    fails = [r for r in results if r['err_pct'] >= 2.0]
    print(f"\nRATE: {len(fails)}/{len(results)} windows over the 2% threshold "
          f"({a.n} runs)")
    with open(a.out, "w") as f:
        json.dump(results, f, indent=1)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
