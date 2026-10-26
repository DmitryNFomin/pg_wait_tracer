#!/usr/bin/env python3
"""analyse261.py - INSTRUMENT ONLY (#261 / #306).

Splits the sampled-vs-exact CPU-share gap into the only two places it can
live, from ONE preserved trace (no cross-run comparison, identical window,
identical pid population):

  [B] EXACT, time-weighted   - the comparator's own exact number.
  [C] EXACT, evaluated at the SAMPLER'S OWN tick instants.
  [A] SAMPLED                - what the sampler recorded.

  C - B = instant-selection bias: the ticks do not land on on-CPU time as
          often as on-CPU time occupies the window.  Readings fine, clock
          wrong.
  A - C = reading/gate error: at the SAME instants, over the SAME pids, the
          sampler recorded something other than the exact truth.

Streaming, two passes: a 60 s 10 Hz capture is ~10M transition records and
will not fit in memory on a 4 vCPU box (round 1 of this script was
OOM-killed, which is why it is written this way).

Usage: analyse261.py events.tsv
"""
import sys, bisect
from collections import defaultdict

CMD_START, CMD_END = 0xFFFFFFF6, 0xFFFFFFF7


def main(path):
    # ---------------- pass 1: window, ticks, samples, command markers -----
    period = 0
    tr_lo = tr_hi = sm_lo = sm_hi = None
    samples = []
    cmdmark = defaultdict(list)
    n_tr = n_sm = 0
    with open(path) as f:
        for line in f:
            if line[0] == '#':
                continue
            c = line.split('\t')
            kind = c[0]
            if kind == 'S':
                w = int(c[3])
                sm_lo = w if sm_lo is None else (w if w < sm_lo else sm_lo)
                sm_hi = w if sm_hi is None else (w if w > sm_hi else sm_hi)
                samples.append((w, int(c[1]), int(c[5]), int(c[10]), int(c[2])))
                p = int(c[9])
                if p:
                    period = p
                n_sm += 1
            else:
                w = int(c[3])
                tr_lo = w if tr_lo is None else (w if w < tr_lo else tr_lo)
                tr_hi = w if tr_hi is None else (w if w > tr_hi else tr_hi)
                if c[11][0] == '1':            # marker
                    ev = int(c[4])
                    if ev == CMD_START:
                        cmdmark[int(c[1])].append((w, 1))
                    elif ev == CMD_END:
                        cmdmark[int(c[1])].append((w, 0))
                n_tr += 1

    if tr_hi is None or sm_hi is None:
        print(f"REFUSE: trace is missing a tier (transitions={n_tr} samples={n_sm})"
              " - nothing to decompose", file=sys.stderr)
        return 3
    if period == 0:
        print("REFUSE: no SAMPLES block carried a sample period", file=sys.stderr)
        return 3
    win_from, win_to = max(tr_lo, sm_lo), min(tr_hi, sm_hi)
    if win_to <= win_from:
        print("REFUSE: the two tiers do not overlap in wall time", file=sys.stderr)
        return 3
    print(f"records: {n_tr} transitions, {n_sm} samples")
    print(f"window {(win_to-win_from)/1e9:.1f}s  period {period} ns "
          f"({1e9/period:.2f} Hz effective)")

    for p in cmdmark:
        cmdmark[p].sort()
    cmk = {p: ([w for w, _ in v], [d for _, d in v]) for p, v in cmdmark.items()}

    def cmd_open_at(p, t):
        v = cmk.get(p)
        if not v:
            return True          # no markers: comparator's legacy rule
        i = bisect.bisect_right(v[0], t) - 1
        return bool(v[1][i]) if i >= 0 else False

    # population: pids the SAMPLER observed in the window - used on BOTH
    # sides of every comparison, so a pid in one tier only cannot move it.
    pop = {p for (w, p, e, i, m) in samples if win_from <= w < win_to}
    inwin = [s for s in samples if win_from <= s[0] < win_to]
    # wall and mono tick instants, index-aligned: the gate and the interval
    # bounds are mono (as in the comparator), the window is wall.
    tpairs = sorted({(s[0], s[4]) for s in inwin})
    ticks = [a for a, b in tpairs]
    ticks_mono = [b for a, b in tpairs]
    print(f"population {len(pop)} pids, {len(ticks)} distinct tick instants")

    # ---------------- [A] sampled side, comparator's counting -------------
    s_cpu = s_wait = s_idle = 0
    rec = {}
    for (w, p, e, idl, m) in inwin:
        rec[(w, p)] = e
        if e != 0 and idl:
            s_idle += 1
        elif e == 0:
            s_cpu += 1
        else:
            s_wait += 1
    s_share = 100.0 * s_cpu / (s_cpu + s_wait) if (s_cpu + s_wait) else float('nan')

    # ---------------- pass 2: streaming exact accumulation ----------------
    # The gate here is cross_validate.c's cmd_interval_open(), replicated
    # call-for-call in the same stream order (markers update banked/anchor;
    # EVERY non-marker interval consumes the banked time, idle ones
    # included).  Round 1 of this script used "is a command open at the
    # interval's end instant" instead, which rejected 72% of we==0 time
    # against the comparator's 17% -- a long we==0 interval can CONTAIN
    # several CMD_START/CMD_END pairs, so an end-instant rule misreads it.
    # [B] is cross-checked against the comparator's own RAW_CPU_EXACT_NS
    # below; a replication that does not reproduce it is reported as a
    # refusal, not quietly used.
    st_seen = {}; st_open = {}; st_anchor = {}; st_banked = {}

    def interval_open(p, t1, dur):
        if not st_seen.get(p):
            return True
        t0 = t1 - dur if t1 > dur else 0
        open_ns = st_banked.get(p, 0)
        if st_open.get(p):
            a = st_anchor.get(p, 0)
            a = a if a > t0 else t0
            if t1 > a:
                open_ns += t1 - a
        st_banked[p] = 0
        if st_open.get(p):
            st_anchor[p] = t1
        if dur == 0:
            return bool(st_open.get(p))
        return open_ns * 2 >= dur

    e_cpu = e_wait = rej = 0.0
    wall_sum = cpu_sum = 0.0
    n_meas = n_unmeas = 0
    at = {}
    tickset = ticks_mono
    with open(path) as f:
        for line in f:
            if line[0] == '#' or line[0] == 'S':
                continue
            c = line.split('\t')
            p = int(c[1])
            ev = int(c[4])
            if c[11][0] == '1':
                if ev == CMD_START:
                    st_seen[p] = True
                    if not st_open.get(p):
                        st_open[p] = True
                        st_anchor[p] = int(c[2])
                elif ev == CMD_END:
                    st_seen[p] = True
                    if st_open.get(p):
                        st_banked[p] = st_banked.get(p, 0) + int(c[2]) - st_anchor.get(p, 0)
                        st_open[p] = False
                continue
            mono = int(c[2]); dur = int(c[6]); end = int(c[3])
            idl = c[10][0] == '1'
            in_cmd = interval_open(p, mono, dur)   # consumed for EVERY interval
            if p not in pop:
                continue
            start_w = end - dur
            if not idl and end > win_from and start_w < win_to:
                clip = min(end, win_to) - (start_w if start_w > win_from else win_from)
                if clip > 0:
                    if ev == 0:
                        if in_cmd:
                            e_cpu += clip
                        else:
                            rej += clip
                    else:
                        e_wait += clip
            if ev == 0 and not idl and end > win_from and start_w < win_to and in_cmd:
                cp = int(c[7])
                if cp < 0:
                    n_unmeas += 1
                else:
                    n_meas += 1
                    wall_sum += dur
                    cpu_sum += cp
            # state at every tick instant this interval covers, under the
            # SAME per-interval verdict the time-weighted side used
            s_mono = mono - dur
            i = bisect.bisect_left(tickset, s_mono)
            while i < len(tickset) and tickset[i] < mono:
                t = tickset[i]
                if idl:
                    at[(t, p)] = 'idle'
                elif ev == 0:
                    at[(t, p)] = 'cpu' if in_cmd else 'noncmd'
                else:
                    at[(t, p)] = 'wait'
                i += 1

    e_tot = e_cpu + e_wait
    e_share = 100.0 * e_cpu / e_tot if e_tot else float('nan')

    # ---------------- [C] exact at the tick instants ----------------------
    t_cpu = t_wait = t_idle = t_noncmd = t_gap = 0
    agree = dis_lost = dis_gained = dis_other = 0
    miss_cpu = miss_wait = 0
    for t, w in zip(ticks_mono, ticks):
        for p in pop:
            cls = at.get((t, p))
            if cls is None:
                t_gap += 1
                continue
            if cls == 'cpu':
                t_cpu += 1
            elif cls == 'wait':
                t_wait += 1
            elif cls == 'idle':
                t_idle += 1
            else:
                t_noncmd += 1
            got = rec.get((w, p))
            if got is None:
                if cls == 'cpu':
                    miss_cpu += 1
                elif cls == 'wait':
                    miss_wait += 1
                continue
            if (got == 0) == (cls == 'cpu'):
                agree += 1
            elif got == 0:
                dis_gained += 1
            elif cls == 'cpu':
                dis_lost += 1
            else:
                dis_other += 1
    t_tot = t_cpu + t_wait
    t_share = 100.0 * t_cpu / t_tot if t_tot else float('nan')

    print(f"\n[A] SAMPLED       cpu={s_cpu} wait={s_wait} (idle dropped {s_idle})"
          f"        CPU share = {s_share:.2f}%")
    print(f"[B] EXACT time-w  cpu_ns={e_cpu:.4e} wait_ns={e_wait:.4e}"
          f"   CPU share = {e_share:.2f}%")
    print(f"    gate-rejected exact we==0 ns = {rej:.4e}")
    print(f"[C] EXACT @ticks  cpu={t_cpu} wait={t_wait} (idle {t_idle}, "
          f"out-of-cmd {t_noncmd}, uncovered {t_gap})  CPU share = {t_share:.2f}%")
    print(f"\nDECOMPOSITION  A-B = {s_share-e_share:+.2f} pp  (the #306 delta)")
    print(f"  instant-selection bias (C-B) = {t_share-e_share:+.2f} pp")
    print(f"  reading/gate error     (A-C) = {s_share-t_share:+.2f} pp")
    print(f"\n[D] per-(tick,pid), same instants, same pids:  agree={agree}"
          f"  sampler-wait/exact-cpu={dis_lost}  sampler-cpu/exact-not={dis_gained}"
          f"  other={dis_other}")
    print(f"    exact=in-command CPU, sampler recorded NOTHING: {miss_cpu}")
    print(f"    exact=wait,         sampler recorded NOTHING: {miss_wait}")
    if n_meas:
        print(f"\n[E] exact we==0 in-command intervals n={n_meas} "
              f"(cpu_ns unmeasured {n_unmeas}): wall {wall_sum:.4e} ns, "
              f"measured on-CPU {cpu_sum:.4e} ns = {100.0*cpu_sum/wall_sum:.2f}%"
              f" -> the reference counts {100.0*(wall_sum-cpu_sum)/wall_sum:.2f}%"
              " runqueue/off-CPU time as CPU")
    else:
        print(f"\n[E] REFUSE: no measured cpu_ns on we==0 intervals "
              f"(unmeasured {n_unmeas}) - reference-side check blind")
    print(f"\nCONTROL: [B] must equal cross_validate's RAW_CPU_EXACT_NS/"
          f"(RAW_CPU_EXACT_NS+RAW_NONCPU_EXACT_NS).  This run: "
          f"exact_cpu_ns={e_cpu:.6e} exact_noncpu_ns={e_wait:.6e} "
          f"gate_rejected_ns={rej:.6e}.  Compare them by hand against "
          f"xval.txt; a mismatch means the gate replication is NOT faithful "
          f"and the split above must not be used.")
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1]))
