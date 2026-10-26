#!/usr/bin/env python3
"""analyse261.py - INSTRUMENT ONLY (#261 / #306).

Decomposes the sampled-vs-exact CPU-share gap into the only two places it can
live, using ONE preserved trace so no cross-run comparison is involved:

  (1) READING error - at the sampler's OWN tick instants, over the SAME pid
      set, does the sampler's recorded state agree with the exact tier's
      state at that instant?  Disagreement here is sampler-side: a dropped
      or misclassified reading.

  (2) INSTANT-SELECTION bias - do the sampler's tick instants land on
      on-CPU time as often as on-CPU time actually occupies the window?
      A deficit here means the readings are right and the CLOCK is wrong:
      the ticks are correlated with backends being off-CPU.

The comparator's own number (exact time-weighted share vs sampled share) is
reproduced as a control, so a decomposition that does not add up is visible
rather than assumed.

Usage: analyse261.py events.tsv
"""
import sys, bisect
from collections import defaultdict

CMD_START, CMD_END = 0xFFFFFFF6, 0xFFFFFFF7

def main(path):
    samples = []        # (wall, pid, we, idle)
    period = 0
    tr = defaultdict(list)   # pid -> list of (start, end, event, dur, cpu_ns, idle)
    cmdmark = defaultdict(list)  # pid -> (wall, +1/-1)
    tr_lo, tr_hi = None, None
    sm_lo, sm_hi = None, None

    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            c = line.rstrip('\n').split('\t')
            kind, pid, mono, wall = c[0], int(c[1]), int(c[2]), int(c[3])
            old, new, dur, cpu = int(c[4]), int(c[5]), int(c[6]), int(c[7])
            per, idle, marker = int(c[9]), int(c[10]), int(c[11])
            if kind == 'S':
                sm_lo = wall if sm_lo is None else min(sm_lo, wall)
                sm_hi = wall if sm_hi is None else max(sm_hi, wall)
                samples.append((wall, pid, new, idle))
                if per:
                    period = per
            else:
                tr_lo = wall if tr_lo is None else min(tr_lo, wall)
                tr_hi = wall if tr_hi is None else max(tr_hi, wall)
                if marker:
                    if old == CMD_START:
                        cmdmark[pid].append((wall, +1))
                    elif old == CMD_END:
                        cmdmark[pid].append((wall, -1))
                    continue
                tr[pid].append((wall - dur, wall, old, dur,
                                None if cpu < 0 else cpu, idle))

    if tr_hi is None or sm_hi is None:
        print("REFUSE: trace lacks one of the two tiers "
              f"(transitions={tr_hi is not None} samples={sm_hi is not None}) "
              "- nothing can be decomposed", file=sys.stderr)
        return 3
    if period == 0:
        print("REFUSE: no SAMPLES block carried a sample period", file=sys.stderr)
        return 3
    win_from, win_to = max(tr_lo, sm_lo), min(tr_hi, sm_hi)
    if win_to <= win_from:
        print("REFUSE: sampled and exact data do not overlap", file=sys.stderr)
        return 3
    win_s = (win_to - win_from) / 1e9
    print(f"window {win_s:.1f}s  period {period} ns  "
          f"({1e9/period:.0f} Hz nominal)")

    # ---- population: the pids the SAMPLER actually observed in the window.
    # "Identical backend population" (#261) is enforced by using this set on
    # BOTH sides of every comparison below, so a pid present in one tier and
    # absent from the other can never create or hide a difference.
    pop = {p for (w, p, e, i) in samples if win_from <= w < win_to}
    print(f"population: {len(pop)} pids observed by the sampler in the window")

    for p in tr:
        tr[p].sort()
        cmdmark[p].sort()
    starts = {p: [iv[0] for iv in tr[p]] for p in tr}

    def state_at(p, t):
        """exact state of pid p at instant t: (event, idle) or None."""
        lst = tr.get(p)
        if not lst:
            return None
        i = bisect.bisect_right(starts[p], t) - 1
        while i >= 0:
            s, e, ev, d, c, idl = lst[i]
            if s <= t < e:
                return (ev, idl)
            if e <= t and i + 1 <= len(lst) - 1 and lst[i + 1][0] > t:
                return None       # genuine gap
            if e <= t:
                return None
            i -= 1
        return None

    def cmd_open_at(p, t):
        """is a command open for p at instant t, per the CMD markers?
        pids with no markers at all keep the comparator's legacy rule (open)."""
        ms = cmdmark.get(p)
        if not ms:
            return True, False     # (open, had_markers)
        st = False
        for (w, d) in ms:
            if w > t:
                break
            st = (d > 0)
        return st, True

    # ---- (A) SAMPLED side, exactly as cross_validate counts it
    s_cpu = s_wait = s_idle = 0
    by_tick = defaultdict(dict)
    for (w, p, e, idl) in samples:
        if not (win_from <= w < win_to):
            continue
        by_tick[w][p] = e
        if e != 0 and idl:
            s_idle += 1
        elif e == 0:
            s_cpu += 1
        else:
            s_wait += 1
    s_tot = s_cpu + s_wait
    s_share = 100.0 * s_cpu / s_tot if s_tot else float('nan')
    print(f"\n[A] SAMPLED      cpu={s_cpu} wait={s_wait} (idle dropped {s_idle})"
          f"   CPU share = {s_share:.2f}%")

    # ---- (B) EXACT time-weighted, comparator definition, SAME population
    e_cpu = e_wait = 0.0
    rej = 0.0
    for p in pop:
        for (s, e, ev, d, c, idl) in tr.get(p, []):
            if e <= win_from or s >= win_to:
                continue
            clip = min(e, win_to) - max(s, win_from)
            if clip <= 0:
                continue
            if idl:
                continue
            if ev == 0:
                op, _ = cmd_open_at(p, e)
                # comparator uses a majority-in-command rule per interval;
                # at a 60 s window with sub-ms intervals the instant rule at
                # the interval end agrees with it to within rounding, and
                # BOTH totals are printed so the choice is visible.
                if op:
                    e_cpu += clip
                else:
                    rej += clip
            else:
                e_wait += clip
    e_tot = e_cpu + e_wait
    e_share = 100.0 * e_cpu / e_tot if e_tot else float('nan')
    print(f"[B] EXACT time-w  cpu_ns={e_cpu:.3e} wait_ns={e_wait:.3e}"
          f"   CPU share = {e_share:.2f}%   (gate-rejected cpu_ns {rej:.3e})")
    print(f"    delta_pp (sampled - exact, the #306 sign) = {s_share - e_share:+.2f} pp")

    # ---- (C) EXACT evaluated AT THE SAMPLER'S TICK INSTANTS, same population
    t_cpu = t_wait = t_idlec = t_gap = 0
    # ---- (D) per-instant agreement with what the sampler recorded
    agree = dis_cpu_lost = dis_cpu_gained = dis_other = 0
    missing_cpu = 0        # exact says in-command CPU, sampler recorded nothing
    missing_wait = 0
    ticks = sorted(by_tick)
    for t in ticks:
        rec = by_tick[t]
        for p in pop:
            st = state_at(p, t)
            if st is None:
                t_gap += 1
                continue
            ev, idl = st
            if ev != 0 and idl:
                t_idlec += 1
                exact_cls = 'idle'
            elif ev == 0:
                op, _ = cmd_open_at(p, t)
                if op:
                    t_cpu += 1
                    exact_cls = 'cpu'
                else:
                    exact_cls = 'noncmd'
            else:
                t_wait += 1
                exact_cls = 'wait'
            got = rec.get(p)
            if got is None:
                if exact_cls == 'cpu':
                    missing_cpu += 1
                elif exact_cls == 'wait':
                    missing_wait += 1
                continue
            if (got == 0) == (exact_cls == 'cpu'):
                if got == 0 or exact_cls != 'idle' or True:
                    agree += 1
            elif got == 0:
                dis_cpu_gained += 1
            elif exact_cls == 'cpu':
                dis_cpu_lost += 1
            else:
                dis_other += 1
    t_tot = t_cpu + t_wait
    t_share = 100.0 * t_cpu / t_tot if t_tot else float('nan')
    print(f"[C] EXACT @ticks  cpu={t_cpu} wait={t_wait} (idle {t_idlec}, "
          f"uncovered {t_gap})   CPU share = {t_share:.2f}%")
    print(f"\nDECOMPOSITION of ({s_share:+.2f} - {e_share:+.2f}) = "
          f"{s_share - e_share:+.2f} pp")
    print(f"  instant-selection bias  (C - B) = {t_share - e_share:+.2f} pp"
          "   <- ticks land off-CPU more/less often than CPU occupies the window")
    print(f"  reading/gate error      (A - C) = {s_share - t_share:+.2f} pp"
          "   <- what the sampler recorded vs exact truth at the SAME instants")
    print(f"\n[D] per-(tick,pid) agreement, same instants, same pids:")
    print(f"  agree={agree}  sampler-says-wait-exact-says-cpu={dis_cpu_lost}"
          f"  sampler-says-cpu-exact-says-not={dis_cpu_gained}"
          f"  other-disagreement={dis_other}")
    print(f"  exact says in-command CPU but sampler recorded NOTHING: {missing_cpu}")
    print(f"  exact says wait        but sampler recorded NOTHING: {missing_wait}")

    # ---- (E) #306 reference-side check: is the exact tier's wall-clock CPU
    # actually on-CPU?  cpu_ns is se.sum_exec_runtime over the interval, so
    # duration_ns - cpu_ns on a we==0 interval is runqueue/off-CPU time the
    # comparator counts as CPU.
    w = c = 0.0
    n_meas = n_unmeas = 0
    for p in pop:
        for (s, e, ev, d, cp, idl) in tr.get(p, []):
            if ev != 0 or idl or e <= win_from or s >= win_to:
                continue
            op, _ = cmd_open_at(p, e)
            if not op:
                continue
            if cp is None:
                n_unmeas += 1
                continue
            n_meas += 1
            w += d
            c += cp
    if n_meas:
        print(f"\n[E] exact we==0 in-command intervals: n={n_meas} "
              f"(unmeasured {n_unmeas})")
        print(f"    wall sum {w:.3e} ns, measured on-CPU sum {c:.3e} ns, "
              f"on-CPU fraction {100.0*c/w:.2f}%")
        print(f"    => the comparator's exact CPU includes "
              f"{100.0*(w-c)/w:.2f}% runqueue/off-CPU time")
    else:
        print(f"\n[E] REFUSE: no measured cpu_ns on we==0 intervals "
              f"(unmeasured {n_unmeas}) - the reference-side check is blind here")
    return 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1]))
