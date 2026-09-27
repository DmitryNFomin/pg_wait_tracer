#!/usr/bin/env python3
"""xtab_join.py — issue #115: decompose the exact tier vs kernel CPU gap.

Intersects, nanosecond by nanosecond and per backend, two timelines over the
SAME window and the SAME pid set:

  ours    the exact tier's own interval stream (xp_gate --dump):
          CPU / WAIT / IDLE, and whether a command was open
  kernel  sched:sched_switch + sched_wakeup transitions (xtab.bt):
          on a processor / runnable-but-queued / blocked

Every backend-nanosecond lands in exactly one (kernel state x our class) cell,
so the decomposition closes by construction rather than by assembly.

The two published shares are, with W = our wait-inside-command seconds:

  kernel share = (on + rq) / (on + rq + W)
  exact  share = (cpu_in_cmd + cpu_out_cmd) / (cpu_in_cmd + cpu_out_cmd + W)

and the numerator difference D = (on + rq) - (cpu_in_cmd + cpu_out_cmd)
decomposes exactly into the cells printed below.
"""
import argparse
import collections
import sys

ON, RQ, OFF, UNK = 0, 1, 2, 3
KNAME = {ON: "on", RQ: "rq", OFF: "off", UNK: "unk"}
CPU, WAIT, IDLE, GAP = 0, 1, 2, 3
CNAME = {CPU: "CPU", WAIT: "WAIT", IDLE: "IDLE", GAP: "GAP"}


def read_dump(path, pids):
    iv = collections.defaultdict(list)
    cm = collections.defaultdict(list)
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "I":
                pid = int(p[1])
                if pid in pids:
                    iv[pid].append((int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6])))
            elif p[0] == "C":
                pid = int(p[1])
                if pid in pids:
                    cm[pid].append((int(p[2]), int(p[3])))
    for d in (iv, cm):
        for k in d:
            d[k].sort()
    return iv, cm


def read_kernel(path, pids, off_ns):
    """K lines -> {pid: [(wall_ns, state)]}, plus the aggregate maps."""
    ev = collections.defaultdict(list)
    agg = collections.defaultdict(lambda: collections.defaultdict(list))
    t = None
    nk = 0
    with open(path) as f:
        for line in f:
            if line[0] == "K":
                p = line.split()
                if p[0] != "K":
                    continue
                pid = int(p[1])
                nk += 1
                if pid in pids:
                    ev[pid].append((int(p[2]) + off_ns, int(p[3])))
                continue
            if line.startswith("TS "):
                t = int(line[3:]) + off_ns
                continue
            if t is None or line[0] != "@":
                continue
            try:
                key, val = line.rsplit(":", 1)
                name, pid = key[1:].split("[", 1)
                agg[name][int(pid.rstrip("]"))].append((t, int(val)))
            except ValueError:
                continue
    for k in ev:
        ev[k].sort()
    return ev, agg, nk


def agg_delta(series, wf, wt):
    if not series:
        return None
    s = sorted(series)
    if wf < s[0][0] or wt > s[-1][0]:
        return None

    def at(t):
        lo, hi = 0, len(s) - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if s[mid][0] < t:
                lo = mid + 1
            else:
                hi = mid
        if s[lo][0] == t or lo == 0:
            return float(s[lo][1])
        t0, v0 = s[lo - 1]
        t1, v1 = s[lo]
        return v0 + (v1 - v0) * (t - t0) / (t1 - t0)

    return at(wt) - at(wf)


def sweep(pid, kev, ivs, cms, wf, wt, cells):
    """Three-way merge over [wf, wt): kernel state x our class x in-command."""
    # kernel state at wf: the last transition at or before wf
    ki = 0
    state = UNK
    while ki < len(kev) and kev[ki][0] <= wf:
        state = kev[ki][1]
        ki += 1
    ii = 0
    ci = 0
    cur = wf
    while cur < wt:
        nxt = wt
        # our class
        while ii < len(ivs) and ivs[ii][1] <= cur:
            ii += 1
        if ii < len(ivs) and ivs[ii][0] <= cur:
            cls = ivs[ii][2]
            nxt = min(nxt, ivs[ii][1])
        else:
            cls = GAP
            if ii < len(ivs):
                nxt = min(nxt, ivs[ii][0])
        # command open?
        while ci < len(cms) and cms[ci][1] <= cur:
            ci += 1
        if ci < len(cms) and cms[ci][0] <= cur:
            incmd = 1
            nxt = min(nxt, cms[ci][1])
        else:
            incmd = 0
            if ci < len(cms):
                nxt = min(nxt, cms[ci][0])
        # kernel
        if ki < len(kev):
            nxt = min(nxt, kev[ki][0])
        if nxt <= cur:
            nxt = cur + 1
        cells[(state, cls, incmd)] += nxt - cur
        cur = nxt
        while ki < len(kev) and kev[ki][0] <= cur:
            state = kev[ki][1]
            ki += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--bt", required=True)
    ap.add_argument("--pids", required=True)
    ap.add_argument("--from", dest="wf", type=int, required=True)
    ap.add_argument("--to", dest="wt", type=int, required=True)
    ap.add_argument("--mono-ns", type=int, required=True)
    ap.add_argument("--wall-ns", type=int, required=True)
    ap.add_argument("--label", default="run")
    a = ap.parse_args()

    pids = set(int(x) for x in a.pids.split(",") if x.strip())
    wf, wt = a.wf, a.wt
    win = (wt - wf) / 1e9
    off_ns = a.wall_ns - a.mono_ns

    iv, cm = read_dump(a.dump, pids)
    kev, agg, nk_all = read_kernel(a.bt, pids, off_ns)

    # -- component 4: backend set and window alignment, verified not assumed --
    dump_pids = set(iv)
    kern_pids = set(kev)
    print(f"XTAB {a.label} window=[{wf},{wt}) secs={win:.3f} backends={len(pids)}")
    print(f"  pid sets: requested={sorted(pids)}")
    print(f"            exact-tier dump={sorted(dump_pids)}  kernel dump={sorted(kern_pids)}")
    if dump_pids != pids or kern_pids != pids:
        print("  FATAL: pid sets differ between the two instruments")
        return 1
    for p in sorted(pids):
        cov = sum(e - s for s, e, *_ in iv[p])
        span = wt - wf
        if abs(cov - span) > span * 0.001:
            print(f"  WARN pid {p}: exact-tier intervals cover {cov/1e9:.3f}s of {span/1e9:.3f}s")

    cells = collections.Counter()
    for p in sorted(pids):
        sweep(p, kev[p], iv[p], cm[p], wf, wt, cells)

    wall = sum(cells.values())
    print(f"  cross-tab total = {wall/1e9:.3f} backend-s "
          f"(exact wall = {len(pids)*win:.3f}; residual "
          f"{wall/1e9 - len(pids)*win:+.6f}s)")

    # -- fail-loud: the dump against bpftrace's own aggregate maps ------------
    ag = {}
    for name, kern in (("on", ON), ("rq", RQ), ("off", OFF)):
        tot = 0.0
        ok = True
        for p in pids:
            d = agg_delta(agg[name].get(p, []), wf, wt)
            if d is None:
                ok = False
                break
            tot += d
        ag[kern] = tot / 1e9 if ok else None
    joined = {k: sum(v for (ks, _, _), v in cells.items() if ks == k) / 1e9
              for k in (ON, RQ, OFF, UNK)}
    print("  DUMP-vs-AGGREGATE (bpftrace's own maps; a lost-event check)")
    for k in (ON, RQ, OFF):
        a_ = ag[k]
        j_ = joined[k]
        if a_ is None:
            print(f"    {KNAME[k]:<4} joined={j_:9.3f}s  aggregate=n/a (window not covered)")
        else:
            d = 100.0 * (j_ - a_) / a_ if a_ else 0.0
            flag = "" if abs(d) < 1.0 else "   <-- MISMATCH >1%"
            print(f"    {KNAME[k]:<4} joined={j_:9.3f}s  aggregate={a_:9.3f}s  {d:+.2f}%{flag}")
    print(f"    unk  joined={joined[UNK]:9.3f}s  "
          f"({100.0*joined[UNK]/(wall/1e9):.3f}% of wall: no kernel transition before the window)")

    # -- the cross-tab -------------------------------------------------------
    print("\n  backend-seconds, kernel state (rows) x exact-tier class (cols)")
    print(f"    {'':<6}{'CPU/cmd':>10}{'CPU/out':>10}{'WAIT/cmd':>10}"
          f"{'WAIT/out':>10}{'IDLE':>10}{'GAP':>10}{'total':>10}")
    cols = [(CPU, 1), (CPU, 0), (WAIT, 1), (WAIT, 0), (IDLE, 1), (IDLE, 0), (GAP, 1), (GAP, 0)]
    show = [(CPU, 1), (CPU, 0), (WAIT, 1), (WAIT, 0), None, None]

    def cell(k, cls, incmd):
        return cells[(k, cls, incmd)] / 1e9

    for k in (ON, RQ, OFF, UNK):
        row = [cell(k, CPU, 1), cell(k, CPU, 0), cell(k, WAIT, 1), cell(k, WAIT, 0),
               cell(k, IDLE, 1) + cell(k, IDLE, 0),
               cell(k, GAP, 1) + cell(k, GAP, 0)]
        print(f"    {KNAME[k]:<6}" + "".join(f"{v:10.3f}" for v in row)
              + f"{sum(row):10.3f}")
    tot_cols = [sum(cell(k, c, ic) for k in (ON, RQ, OFF, UNK)) for c, ic in
                [(CPU, 1), (CPU, 0), (WAIT, 1), (WAIT, 0)]]
    tot_cols.append(sum(cell(k, IDLE, ic) for k in (ON, RQ, OFF, UNK) for ic in (0, 1)))
    tot_cols.append(sum(cell(k, GAP, ic) for k in (ON, RQ, OFF, UNK) for ic in (0, 1)))
    print(f"    {'total':<6}" + "".join(f"{v:10.3f}" for v in tot_cols)
          + f"{sum(tot_cols):10.3f}")

    # -- the equation --------------------------------------------------------
    nb = lambda cls, incmd: (cells[(ON, cls, incmd)] + cells[(RQ, cls, incmd)]) / 1e9
    on_ = joined[ON]
    rq_ = joined[RQ]
    K = on_ + rq_
    cpu_in = tot_cols[0]
    cpu_out = tot_cols[1]
    E = cpu_in + cpu_out
    W = tot_cols[2]          # wait inside a command: the shared denominator term
    W_out = tot_cols[3]

    t_wait_rq = (cells[(RQ, WAIT, 1)] + cells[(RQ, WAIT, 0)]) / 1e9
    t_wait_on = (cells[(ON, WAIT, 1)] + cells[(ON, WAIT, 0)]) / 1e9
    t_idle_rq = (cells[(RQ, IDLE, 1)] + cells[(RQ, IDLE, 0)]) / 1e9
    t_idle_on = (cells[(ON, IDLE, 1)] + cells[(ON, IDLE, 0)]) / 1e9
    t_gap = nb(GAP, 0) + nb(GAP, 1)
    t_cpu_off = -(cells[(OFF, CPU, 1)] + cells[(OFF, CPU, 0)]) / 1e9
    t_unk = (cells[(UNK, WAIT, 1)] + cells[(UNK, WAIT, 0)] + cells[(UNK, IDLE, 0)]
             + cells[(UNK, IDLE, 1)] + cells[(UNK, GAP, 0)] + cells[(UNK, GAP, 1)]
             - cells[(UNK, CPU, 0)] - cells[(UNK, CPU, 1)]) / 1e9

    def share(x):
        return 100.0 * x / (x + W) if (x + W) else 0.0

    terms = [
        ("exact tier numerator E = cpu_in_cmd + cpu_out_cmd", E, None),
        ("  + run-queue delay with a wait event still set", t_wait_rq, "definitional"),
        ("  + on a processor with a wait event still set", t_wait_on, "definitional"),
        ("  + run-queue delay with an IDLE wait event set", t_idle_rq, "definitional"),
        ("  + on a processor with an IDLE wait event set", t_idle_on, "under-capture"),
        ("  + on-or-runnable in a timeline gap (no interval)", t_gap, "under-capture"),
        ("  - off-CPU (blocked) while we call it CPU", t_cpu_off, "over-capture"),
        ("  +- kernel state unknown before first transition", t_unk, "unexplained"),
    ]
    print(f"\n  EQUATION  (W = wait inside a command = {W:.3f} s, the shared "
          f"denominator term)")
    acc = E
    print(f"    {'term':<52}{'sec':>10}{'share%':>9}{'d pp':>8}  kind")
    print(f"    {terms[0][0]:<52}{E:10.3f}{share(E):9.2f}{0.0:8.2f}")
    for name, v, kind in terms[1:]:
        prev = share(acc)
        acc += v
        print(f"    {name:<52}{v:10.3f}{share(acc):9.2f}{share(acc)-prev:8.2f}  {kind}")
    print(f"    {'= kernel numerator K = on + rq':<52}{K:10.3f}{share(K):9.2f}"
          f"{share(K)-share(E):8.2f}")
    print(f"    closure residual: K - (E + terms) = {K - acc:+.6f} s")
    print(f"  HEADLINE  exact={share(E):.2f}%  kernel={share(K):.2f}%  "
          f"gap={share(K)-share(E):.2f} pp")

    # -- supporting sizes the brief asks for ---------------------------------
    cpu_ns_wait = sum(c for s, e, cls, ov, c in
                      [x for p in pids for x in iv[p]] if cls == WAIT and c >= 0)
    cpu_ns_cpu = sum(c for s, e, cls, ov, c in
                     [x for p in pids for x in iv[p]] if cls == CPU and c >= 0)
    print(f"\n  CROSS-CHECK the tracer's own per-interval cpu_ns against the kernel dump")
    print(f"    on-CPU inside WAIT intervals: kernel {(cells[(ON,WAIT,1)]+cells[(ON,WAIT,0)])/1e9:.3f}s"
          f"   tracer cpu_ns {cpu_ns_wait/1e9:.3f}s")
    print(f"    on-CPU inside CPU  intervals: kernel {(cells[(ON,CPU,1)]+cells[(ON,CPU,0)])/1e9:.3f}s"
          f"   tracer cpu_ns {cpu_ns_cpu/1e9:.3f}s")
    print(f"  SIZES  out_of_command_cpu={cpu_out:.3f}s (already inside E; without it "
          f"the exact share would be {share(E-cpu_out):.2f}%, gap "
          f"{share(K)-share(E-cpu_out):.2f} pp)")
    print(f"         wait_outside_command={W_out:.3f}s  idle_wall={tot_cols[4]:.3f}s  "
          f"gap_wall={tot_cols[5]:.3f}s")
    print(f"         blocked-but-called-CPU (wait our tier did not reconstruct) = "
          f"{-t_cpu_off:.3f}s = {100.0*-t_cpu_off/(W+-t_cpu_off):.2f}% of the wait it should have seen")
    print(f"  KERNEL on={on_:.3f}s rq={rq_:.3f}s off={joined[OFF]:.3f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
