#!/usr/bin/env python3
"""kernel_cpu_arbiter.py — a kernel-side arbiter for "how much CPU did the
backends use", independent of pg_wait_tracer's probes and of PostgreSQL's
pg_stat_activity.

Why this exists (issue #115): three sources disagree about the CPU share of
non-idle backend time. Two of them (our exact tier, our sampled tier) come out
of the same tracer; the third (pg_stat_activity sampled from another session)
is PostgreSQL's self-report. None of them can adjudicate the others. The
kernel's scheduler accounting can, but only if you are careful about WHAT it
counts.

    /proc/<pid>/schedstat is "<sum_exec_runtime> <run_delay> <pcount>":
      field 1  sum_exec_runtime  ns actually EXECUTING on a processor
      field 2  run_delay         ns RUNNABLE but waiting for a processor
                                 (needs kernel.sched_schedstats=1; it reads 0
                                 otherwise, which is why `sample` checks)

"No wait event while inside a statement" -- the quantity our tiers and
pg_stat_activity both report -- is NOT field 1. A backend that is preempted is
still "not waiting on anything PostgreSQL knows about", so it keeps counting
towards our CPU row while the kernel counts it in field 2, not field 1. On a
saturated box the two differ by a lot. The comparable kernel quantity is
therefore field1 + field2, and reporting the two separately is the point:
it decomposes any disagreement into "we mismeasured" versus "we include
run-queue delay and the kernel does not".

utime+stime from /proc/<pid>/stat is also collected, as a cross-check on
field 1 only. It is quantised to clock ticks (usually 10 ms) and is itself
tick-sampled, so it is the weaker of the two; schedstat is nanosecond-exact
scheduler bookkeeping. It does not distinguish run-queue delay at all.

Both counters are cumulative and monotone per task, so this samples them
periodically and takes a *difference at the window boundaries* (linearly
interpolated between the bracketing samples). That is a counter delta, not a
sample of a state: it does not miss short events and it does not care when the
sampler itself gets scheduled -- a late sample still carries the full counter.
That immunity to sampler skew is exactly what a state sampler cannot offer on
a saturated box.

What it deliberately does NOT count: anything outside the given pid set (the
tracer daemon, pgbench, the pg_stat_activity sampler backend, postmaster
auxiliary processes), and any pid that does not span the whole window.

Usage:
  kernel_cpu_arbiter.py sample --postmaster PID --out FILE [--hz 10]
                               [--duration SECS] [--extra-pid PID]...
  kernel_cpu_arbiter.py report --kernel FILE --from NS --to NS
                               [--pids p1,p2,...] [--exclude-pid PID]...
                               [--oracle oracle.csv]

`report` prints backend-seconds over [from, to) restricted to the pid set, for
each kernel counter and (if given) for pg_stat_activity over the very same
interval and the very same pids.
"""
import sys, os, time, csv, re, argparse, collections

CLK_TCK = os.sysconf("SC_CLK_TCK")


def read_children(pm):
    try:
        with open(f"/proc/{pm}/task/{pm}/children") as f:
            return [int(x) for x in f.read().split()]
    except OSError:
        return []


def read_one(pid):
    """(sum_exec_ns, run_delay_ns, utime_ticks, stime_ticks), or None if gone.

    utime and stime are kept apart on purpose. A hardware-watchpoint probe
    fires in the traced backend's own context, so its cost lands in that
    BACKEND's stime, not in the tracer daemon's. Splitting user from system
    time is the cheapest way to see that perturbation without a control run.
    """
    try:
        with open(f"/proc/{pid}/schedstat") as f:
            a = f.read().split()
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
    except OSError:
        return None
    # comm can contain ')' and spaces -> split after the LAST ')'
    try:
        rest = s[s.rindex(")") + 2:].split()
        utime, stime = int(rest[11]), int(rest[12])
    except (ValueError, IndexError):
        return None
    if len(a) < 2:
        return None
    return int(a[0]), int(a[1]), utime, stime


def cmd_sample(args):
    if open("/proc/sys/kernel/sched_schedstats").read().strip() == "0":
        print("kernel_cpu_arbiter: kernel.sched_schedstats=0 -- run_delay will "
              "read 0. sysctl -w kernel.sched_schedstats=1 first.",
              file=sys.stderr)
        return 2
    period = 1.0 / args.hz
    deadline = time.time() + args.duration
    extra = list(args.extra_pid or [])
    with open(args.out, "w", buffering=1 << 20) as out:
        w = csv.writer(out)
        w.writerow(["t_ns", "pid", "exec_ns", "delay_ns", "utime_ticks",
                    "stime_ticks"])
        nxt = time.time()
        while True:
            now = time.time()
            if now >= deadline:
                break
            pids = read_children(args.postmaster) + extra
            t0 = time.time_ns()
            rows = []
            for p in pids:
                r = read_one(p)
                if r:
                    rows.append((p, r))
            t1 = time.time_ns()
            t = (t0 + t1) // 2
            for p, (e, d, u, sy) in rows:
                w.writerow([t, p, e, d, u, sy])
            nxt += period
            slp = nxt - time.time()
            if slp > 0:
                time.sleep(slp)
            else:
                nxt = time.time()
    return 0


NFIELD = 4  # exec_ns, delay_ns, utime_ticks, stime_ticks


def interp(samples, t):
    """samples: sorted [(t_ns, exec, delay, utime, stime)]. Linear
    interpolation of each cumulative counter at wall time t. None if t is
    outside the pid's sample coverage."""
    if not samples or t < samples[0][0] or t > samples[-1][0]:
        return None
    lo, hi = 0, len(samples) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if samples[mid][0] < t:
            lo = mid + 1
        else:
            hi = mid
    if samples[lo][0] == t or lo == 0:
        return samples[lo][1:]
    a, b = samples[lo - 1], samples[lo]
    span = b[0] - a[0]
    if span <= 0:
        return b[1:]
    f = (t - a[0]) / span
    return tuple(a[i] + (b[i] - a[i]) * f for i in range(1, NFIELD + 1))


def cmd_report(args):
    want = set(int(x) for x in args.pids.split(",") if x.strip()) if args.pids else None
    excl = set(args.exclude_pid or [])
    wf, wt = args.__dict__["from"], args.to
    win_s = (wt - wf) / 1e9

    per = collections.defaultdict(list)
    with open(args.kernel) as f:
        rd = csv.reader(f)
        next(rd, None)
        for row in rd:
            p = int(row[1])
            if p in excl or (want is not None and p not in want):
                continue
            per[p].append((int(row[0]), int(row[2]), int(row[3]), int(row[4]),
                           int(row[5])))
    exec_ns = delay_ns = ut_ns = st_ns = 0.0
    covered, partial = [], []
    for p, s in sorted(per.items()):
        s.sort()
        a, b = interp(s, wf), interp(s, wt)
        if a is None or b is None:
            partial.append(p)
            continue
        covered.append(p)
        exec_ns += b[0] - a[0]
        delay_ns += b[1] - a[1]
        ut_ns += (b[2] - a[2]) * 1e9 / CLK_TCK
        st_ns += (b[3] - a[3]) * 1e9 / CLK_TCK

    print(f"KERNEL window={win_s:.3f}s pids={len(covered)} "
          f"(not spanning the window, excluded: {sorted(partial) or 'none'})")
    print(f"  schedstat f1 sum_exec_runtime (on a processor)   = {exec_ns/1e9:8.3f} s")
    print(f"  schedstat f2 run_delay        (runnable, queued) = {delay_ns/1e9:8.3f} s")
    print(f"  f1+f2  (on a processor OR wanting one)           = {(exec_ns+delay_ns)/1e9:8.3f} s")
    print(f"  /proc/stat utime (tick-quantised)                = {ut_ns/1e9:8.3f} s")
    print(f"  /proc/stat stime (tick-quantised; probe cost)    = {st_ns/1e9:8.3f} s")
    print(f"  /proc/stat utime+stime (f1 cross-check)          = {(ut_ns+st_ns)/1e9:8.3f} s")
    print(f"  pids covered: {','.join(str(p) for p in covered)}")

    if args.oracle:
        cpu = tot = 0
        iters = set()
        per_ev = collections.Counter()
        with open(args.oracle) as f:
            for row in csv.reader(f):
                if len(row) < 6:
                    continue
                it, ts, pid, state, wet, we = (int(row[0]), int(row[1]), int(row[2]),
                                               row[3], row[4], row[5])
                if ts < wf or ts >= wt:
                    continue
                iters.add(it)
                if pid in excl or (want is not None and pid not in want):
                    continue
                if state != "active" or wet == "Activity" or we == "ClientRead":
                    continue
                tot += 1
                if we == "":
                    cpu += 1
                    per_ev["CPU"] += 1
                else:
                    per_ev[f"{wet}:{we}"] += 1
        n = len(iters)
        w = win_s / n if n else 0.0
        print(f"PGSA  iters={n} ({n/win_s if win_s else 0:.1f} Hz) samples={tot} "
              f"cpu_samples={cpu} share={100.0*cpu/tot if tot else 0:.1f}%")
        print(f"  cpu backend_s  = {cpu*w:8.3f} s   (active total {tot*w:.3f} s, "
              f"wait {(tot-cpu)*w:.3f} s)")
        for k, v in per_ev.most_common(8):
            print(f"    {k:26s} {100.0*v/tot if tot else 0:6.1f}%  {v*w:8.2f}s")
    return 0


def parse_bt(path, mono_ns, wall_ns):
    """Parse the dump stream of tests/sched.bt.

    bpftrace stamps CLOCK_MONOTONIC (`nsecs`); the caller passes one
    simultaneous (monotonic, realtime) reading so the dumps can be placed on
    the same wall clock the trace files and pg_stat_activity use.
    Returns {map_name: {pid: [(t_wall_ns, cumulative_value)]}}.
    """
    off = wall_ns - mono_ns
    out = collections.defaultdict(lambda: collections.defaultdict(list))
    t = None
    with open(path) as f:
        for line in f:
            if line.startswith("TS "):
                t = int(line[3:]) + off
                continue
            if t is None or not line.startswith("@"):
                continue
            try:
                key, val = line.rsplit(":", 1)
                key = key[1:]
                if "[" in key:
                    name, pid = key.split("[", 1)
                    pid = int(pid.rstrip("]"))
                else:
                    name, pid = key, 0   # scalar map, e.g. "@sn: 1234"
                out[name][pid].append((t, int(val)))
            except ValueError:
                continue
    return out


def delta(series, wf, wt):
    """Cumulative series [(t, v)] -> v(wt) - v(wf), or None if not covered."""
    if not series:
        return 0.0
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
        a, b = s[lo - 1], s[lo]
        if b[0] == a[0]:
            return float(b[1])
        return a[1] + (b[1] - a[1]) * (t - a[0]) / (b[0] - a[0])

    return at(wt) - at(wf)


def cmd_sched(args):
    wf, wt = args.__dict__["from"], args.to
    win_s = (wt - wf) / 1e9
    pids = [int(x) for x in args.pids.split(",") if x.strip()]
    maps = parse_bt(args.bt, args.mono_ns, args.wall_ns)
    tot = {}
    missing = []
    for m in ("on", "rq", "off", "syncw", "synco"):
        acc = 0.0
        for p in pids:
            d = delta(maps[m].get(p, []), wf, wt)
            if d is None:
                missing.append((m, p))
                d = 0.0
            acc += d
        tot[m] = acc
    wall = win_s * len(pids)
    print(f"SCHEDTRACE window={win_s:.3f}s backends={len(pids)} "
          f"wall={wall:.3f} backend-s" +
          (f"  INCOMPLETE COVERAGE: {missing}" if missing else ""))
    print(f"  on-CPU  (executing)                = {tot['on']/1e9:8.3f} s")
    print(f"  run-queue delay (runnable, queued) = {tot['rq']/1e9:8.3f} s")
    print(f"  off-CPU (blocked / sleeping)       = {tot['off']/1e9:8.3f} s")
    s = (tot['on'] + tot['rq'] + tot['off']) / 1e9
    print(f"  sum                                = {s:8.3f} s "
          f"({100.0*s/wall if wall else 0:.1f}% of wall; the remainder is the "
          f"slice in progress at each dump boundary)")
    print(f"  fsync/fdatasync wall               = {tot['syncw']/1e9:8.3f} s")
    print(f"  ... of which EXECUTING on a cpu    = {tot['synco']/1e9:8.3f} s "
          f"({100.0*tot['synco']/tot['syncw'] if tot['syncw'] else 0:.1f}% -- "
          f"on-CPU time PostgreSQL reports as the IO:WalSync wait)")
    return 0


def cmd_sampbias(args):
    """Is the sampler scheduled preferentially when few backends are queued?
    Compares the TIME-weighted distribution of "backends on a processor or
    runnable" against the distribution seen at the instants the sampler was
    actually switched in. Equal means no scheduling bias; a lower sampler
    mean means the sampler systematically under-observes the very state it
    is being used to measure.

    --n-map / --obs-map select which observer's counters to score, so the
    same instrument grades the pg_stat_activity poller (sn/sobs), the
    pg_wait_sampling collector over every switch-in (cn/cobs) and the same
    collector over only the wakeup-triggered switch-ins that actually
    precede probe_waits() (cnw/cobsw)."""
    wf, wt = args.__dict__["from"], args.to
    win = (wt - wf) / 1e9
    pids = [int(x) for x in args.pids.split(",") if x.strip()]
    maps = parse_bt(args.bt, args.mono_ns, args.wall_ns)
    nmap, obsmap, tag = args.n_map, args.obs_map, args.tag
    sn = delta(maps[nmap].get(0, []), wf, wt)
    if not sn:
        print(f"SAMPBIAS[{tag}]: @{nmap} never advanced during the window")
        return 1
    oncpu = delta(maps[args.on_map].get(0, []), wf, wt) if args.on_map else None
    print(f"SAMPBIAS[{tag}]  backends={len(pids)}  window={win:.3f}s  "
          f"sampler switch-ins in window={sn:.0f} "
          f"({sn/win:.1f}/s)" +
          (f"  observer on-CPU={oncpu/1e9:.3f}s "
           f"({100.0*oncpu/1e9/win:.2f}% of one processor)"
           if oncpu else ""))
    print(f"  {'pid':>7} {'by time %':>10} {'as sampler saw it %':>21} {'delta pp':>9}")
    et = es = 0.0
    for p in pids:
        on = delta(maps["on"].get(p, []), wf, wt) or 0.0
        rq = delta(maps["rq"].get(p, []), wf, wt) or 0.0
        ft = 100.0 * (on + rq) / 1e9 / win
        fs = 100.0 * (delta(maps[obsmap].get(p, []), wf, wt) or 0.0) / sn
        et += ft
        es += fs
        print(f"  {p:>7} {ft:>9.1f}% {fs:>20.1f}% {fs-ft:>+8.2f}")
    n = len(pids)
    et /= n
    es /= n
    print(f"  mean fraction of backends on a processor OR runnable:")
    print(f"    by time                 = {et:.2f}%")
    print(f"    at sampler switch-ins   = {es:.2f}%")
    if es < et - 0.5:
        verdict = "sampler UNDER-observes the on-cpu-or-runnable state"
    elif es > et + 0.5:
        verdict = "sampler OVER-observes it"
    else:
        verdict = "no material bias"
    print(f"  SCHEDULING BIAS[{tag}] = {es-et:+.2f} pp ({verdict})")
    return 0


def cmd_delays(args):
    """The END-of-run histograms sched3.bt prints: the observer's own
    wakeup->running delay and, for the collector, its on-CPU burst. Prints
    the count-weighted mean and the 50th/90th percentile bucket.

    bpftrace gotcha (issue #115): unless every histogram map is clear()d in
    the END block, bpftrace's own default end-of-program dump reprints every
    map that still holds data -- silently, right after our own explicit
    printf+print() of the same histogram, with no repeated "SDELAY_US"-style
    header of ours in front of it, just its own bare "@sdelay: ..." line. A
    naive line-by-line bucket parser then keeps appending that second dump's
    buckets onto the same header, doubling every count and skewing every
    percentile computed from them.

    Guard: a header's underlying map name is derived from it (SDELAY_US ->
    "sdelay", by the naming convention every .bt(.tmpl) here uses). Once a
    header has been seen, ANY later line that looks like bpftrace's own
    default per-map banner for that same map name -- "@sdelay:" or
    "@sdelay[key]:", with or without an intervening blank line -- means the
    map was printed twice with no clear() between the two, which is exactly
    the doubling bug; this fails loudly instead of silently re-attributing
    (or silently dropping) the second dump's buckets.
    """
    want = set(args.which.split(","))
    cur = None
    seen = {}     # header text -> its map name, e.g. "SDELAY_US" -> "sdelay"
    opened = set()  # map names whose OWN opening "@name:" banner was consumed
    buckets = collections.defaultdict(list)
    with open(args.bt) as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            m = re.match(r"^([A-Z]+_US) ", line)
            if m:
                name = m.group(1)
                if name in seen:
                    sys.exit(
                        f"kernel_cpu_arbiter.py delays: {args.bt}:{lineno}: "
                        f"header '{name}' printed a second time. bpftrace "
                        "reprints every populated map at exit unless it is "
                        "clear()d in the END block; add clear(@...) for "
                        "every histogram this script prints, or the counts "
                        "here are silently doubled.")
                seen[name] = name.split("_")[0].lower()
                cur = name
                continue
            bare = re.match(r"^@(\w+)(\[[^\]]*\])?:\s*$", line)
            if bare and bare.group(1) in seen.values():
                if bare.group(1) in opened:
                    sys.exit(
                        f"kernel_cpu_arbiter.py delays: {args.bt}:{lineno}: "
                        f"bpftrace's own default dump of '@{bare.group(1)}' "
                        "reappeared after its histogram was already printed "
                        "once -- same doubling bug: clear() every histogram "
                        "map in the .bt script's END block after printing "
                        "it.")
                opened.add(bare.group(1))
            if not line.strip():
                cur = None
                continue
            if cur is None or not line.strip().startswith("["):
                continue
            mm = re.match(r"^\s*\[([0-9KMG.]+),?\s*([0-9KMG.]*)\)?\]?\s+(\d+)\s", line)
            if not mm:
                continue
            buckets[cur].append((mm.group(1), mm.group(2), int(mm.group(3))))

    def scale(s):
        s = s.strip()
        mul = 1.0
        if s.endswith("K"):
            mul, s = 1e3, s[:-1]
        elif s.endswith("M"):
            mul, s = 1e6, s[:-1]
        elif s.endswith("G"):
            mul, s = 1e9, s[:-1]
        try:
            return float(s) * mul
        except ValueError:
            return 0.0

    for name in sorted(buckets):
        if want and name not in want:
            continue
        bs = buckets[name]
        tot = sum(c for _, _, c in bs)
        if not tot:
            continue
        acc = 0.0
        p50 = p90 = None
        wsum = 0.0
        for lo, hi, c in bs:
            l, h = scale(lo), scale(hi) if hi else scale(lo) * 2
            wsum += c * (l + h) / 2.0
            acc += c
            if p50 is None and acc >= 0.5 * tot:
                p50 = (lo, hi)
            if p90 is None and acc >= 0.9 * tot:
                p90 = (lo, hi)
        print(f"{name} n={tot} mean={wsum/tot:.1f}us "
              f"p50=[{p50[0]},{p50[1]}) p90=[{p90[0]},{p90[1]})")
    return 0


def cmd_torn(args):
    """Rows where pg_stat_activity reported an active backend that was also
    waiting on ClientRead: state and wait_event come from different places in
    the backend's status entry, so the pair can be torn."""
    tot = torn = idle_nowait = 0
    with open(args.oracle) as f:
        for row in csv.reader(f):
            if len(row) < 6:
                continue
            ts, state, we = int(row[1]), row[3], row[5]
            if ts < args.__dict__["from"] or ts >= args.to:
                continue
            tot += 1
            if state == "active" and we == "ClientRead":
                torn += 1
            if state != "active" and we == "":
                idle_nowait += 1
    print(f"TORN rows={tot}  active+ClientRead={torn} ({100.0*torn/tot if tot else 0:.2f}%)"
          f"  non-active+no-wait-event={idle_nowait} "
          f"({100.0*idle_nowait/tot if tot else 0:.2f}%)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sample")
    s.add_argument("--postmaster", type=int, required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--hz", type=float, default=10.0)
    s.add_argument("--duration", type=float, default=120.0)
    s.add_argument("--extra-pid", type=int, action="append")
    s.set_defaults(fn=cmd_sample)
    r = sub.add_parser("report")
    r.add_argument("--kernel", required=True)
    r.add_argument("--from", dest="from", type=int, required=True)
    r.add_argument("--to", type=int, required=True)
    r.add_argument("--pids")
    r.add_argument("--exclude-pid", type=int, action="append")
    r.add_argument("--oracle")
    r.set_defaults(fn=cmd_report)
    b = sub.add_parser("sched")
    b.add_argument("--bt", required=True, help="tests/sched.bt's dump stream")
    b.add_argument("--from", dest="from", type=int, required=True)
    b.add_argument("--to", type=int, required=True)
    b.add_argument("--pids", required=True)
    b.add_argument("--mono-ns", type=int, required=True,
                   help="CLOCK_MONOTONIC reading taken with --wall-ns")
    b.add_argument("--wall-ns", type=int, required=True)
    b.set_defaults(fn=cmd_sched)
    sb = sub.add_parser("sampbias")
    sb.add_argument("--bt", required=True)
    sb.add_argument("--from", dest="from", type=int, required=True)
    sb.add_argument("--to", type=int, required=True)
    sb.add_argument("--pids", required=True)
    sb.add_argument("--mono-ns", type=int, required=True)
    sb.add_argument("--wall-ns", type=int, required=True)
    sb.add_argument("--n-map", default="sn",
                    help="scalar map counting the observer's switch-ins")
    sb.add_argument("--obs-map", default="sobs",
                    help="per-pid map counting on-or-runnable at those instants")
    sb.add_argument("--on-map", default=None,
                    help="scalar map holding the observer's own on-CPU ns")
    sb.add_argument("--tag", default="pgsa")
    sb.set_defaults(fn=cmd_sampbias)
    dl = sub.add_parser("delays")
    dl.add_argument("--bt", required=True)
    dl.add_argument("--which", default="SDELAY_US,CDELAY_US,CBURST_US")
    dl.set_defaults(fn=cmd_delays)
    tn = sub.add_parser("torn")
    tn.add_argument("--oracle", required=True)
    tn.add_argument("--from", dest="from", type=int, required=True)
    tn.add_argument("--to", type=int, required=True)
    tn.set_defaults(fn=cmd_torn)
    a = ap.parse_args()
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
