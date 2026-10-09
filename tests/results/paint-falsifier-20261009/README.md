# Paint-latency falsifier: real box, real trace, current master

## Box
- **gate-2** (`root@142.132.191.18`), not gate-1.
  - gate-1 load at check time: `3.69, 4.61, 6.35` (1/5/15 min), flock held by 8
    PIDs, an active `pgwt-check` run with a live-capture `pg_wait_tracer`
    process + psql load — stayed loaded (`3.69`) when rechecked 3 min later.
  - gate-2 load at check time: `0.88, 2.22, 3.76`, one `cc1` compile at 100%
    CPU (finished quickly); during the runs, load fell to `1.2-1.8` (1-min).
  - **CPU vendor differs between the two boxes** (not previously known to me):
    gate-1 = Intel Xeon Skylake (QEMU), gate-2 = **AMD EPYC-Rome**. Same `cx33`
    plan, different silicon (CLAUDE.md memory already flags this). This is a
    real confound on the absolute numbers below — see "Important caveat".

## Trace
- Source: `/root/paint1_trace_r2` on gate-1, confirmed present (52 MB dir:
  `current.trace` 52,087,149 bytes, `num_events=6955008`, matches the "52 MB"
  / "~6.96M events" description). Copied via gate-1→local tar→gate-2 (no
  direct scp path between boxes), landed at
  `/root/paint1_trace_r2_copy` on gate-2, same size (52M) and same
  `num_events=6955008` reported by the server.

## Build
- Cloned current `origin/master` into a fresh checkout on gate-2 via a git
  bundle (no GitHub network dependency): `f78ab4c` — "Transitions: answer
  from the per-block transition aggregate (paint-latency Phase 1) (#340)" —
  matches `origin/master` HEAD in this worktree.
- `make pgwt-server`: built clean (warnings only, no errors). `DONE
  pgwt-server`.

## Commands
`tests/results/demo_paint_20261008/timecmds.py <server> <trace> <window_s>`,
5 runs per window, `timeout 120` per run (the script's internal per-request
timeout is unbounded but every run returned in well under 120s — see raw
logs). No hang observed anywhere (issue #327 did not reproduce).

## Results (server ms, median of n=5, raw values in `raw_900.txt` / `raw_300.txt`)

| command | @900s median (raw n=5) | @300s median (raw n=5) |
|---|---|---|
| top_queries | 1865.9 (1836.1, 1859.5, 1865.9, 1870.4, 1881.4) | 706.0 (687.9, 696.3, 706.0, 720.1, 731.3) |
| executions | 1824.9 (1767.9, 1823.0, 1824.9, 1835.8, 1868.2) | 1105.6 (1088.5, 1095.9, 1105.6, 1112.9, 1130.9) |
| exec_scatter | 1539.2 (1522.3, 1537.2, 1539.2, 1574.3, 1583.8) | 848.0 (816.5, 844.2, 848.0, 849.8, 860.9) |
| variants | 1442.1 (1429.6, 1440.4, 1442.1, 1452.2, 1476.6) | 536.2 (535.2, 535.5, 536.2, 544.6, 547.9) |
| concurrency | 1315.1 (1299.3, 1310.9, 1315.1, 1322.1, 1348.0) | 504.6 (478.4, 479.7, 504.6, 511.2, 513.4) |
| transitions | 580.2 (563.7, 565.0, 580.2, 580.4, 580.5) | 226.5 (222.2, 225.8, 226.5, 230.9, 235.9) |

No first-run inflation observed (900s run 1 top_queries=1836.1 is actually the
*lowest* of the 5, not inflated).

## Verdict on the ~2200 ms question
**All five of `executions`, `exec_scatter`, `concurrency`, `variants`,
`top_queries` are BELOW ~2200 ms server-side at 900 s on gate-2** (highest is
`top_queries` at 1865.9 ms median, ~335 ms of headroom). Taken at face value,
this says the phases are not demo-critical and the adviser's Mac-scaling
extrapolation (~4.4 s) was wrong in this direction too — but see the caveat
below before accepting that.

## `transitions` / Phase 1
**Confirmed much faster**: 580.2 ms median at 900s, vs the reference's
2710-2944 ms (whichever table is correct, see discrepancy below) — consistent
with the #340 block-aggregate fast path being wired in and active on this
trace, not just on the Mac. No negative finding here.

## Important caveat (must be resolved before using these numbers to drop the phases)
This run used **gate-2 (AMD EPYC-Rome)**, not gate-1 (Intel Skylake), because
gate-1 was loaded. The original 2026-10-08 reference numbers you quoted were
measured on gate-1. I have no same-box, same-code comparison point: I cannot
tell how much of the ~2.4x speedup (top_queries 4491→1866 ms) is (a) code
improvements merged since Oct 8 (#340, #329, #326 all touch `src/`) vs (b)
gate-2's silicon simply being faster per-event than gate-1's. Both boxes are
nominally the same `cx33` plan but different vendor/microarchitecture, which
CLAUDE.md already flags as a known difference (owner declined to swap
2026-09-29). **Before telling the owner "below budget, drop the phases,"
re-run this same harness on gate-1 once it is quiet**, to separate the two
effects. Until then this result falsifies the Mac-extrapolation's direction
(over-budget) but does not yet confirm gate-1 agrees.

## Reference-number discrepancy (as requested, I checked your quote)
Your quoted reference numbers do **not** match `PHASE_SPLIT_CORRECTED.md`
cleanly — they are a mix of its two tables:
- `top_queries` 4491 and `transitions` 2710 match the **first** table (whole
  1110s-trace-bug-fixed, "Corrected: 900s window" section).
- `executions` 4184, `exec_scatter` 3619, `variants` 3547, `concurrency` 3338
  match the **900s column of the second table** ("owner's `5m` lever, now
  MEASURED"), not the first table (which has executions 4217, exec_scatter
  3470, variants 3621, concurrency 3074 for the same nominal 900s window —
  the two tables themselves disagree slightly, presumably two different runs).
So the six numbers you gave me were assembled from two different tables/runs,
not quoted verbatim from one. Flagging this per your request to check the
quote; it does not change today's measurement, but you should know the
original doc also has an internal 900s-vs-900s inconsistency.

## Commands used
```
git bundle create pgwt-master.bundle origin/master   # on Mac, from f78ab4c
scp bundle to gate-2; git init; git fetch bundle refs/remotes/origin/master:refs/heads/tmp-master; git checkout tmp-master
make pgwt-server
for w in 900 300; do for i in 1..5; do
  timeout 120 python3 timecmds.py ./pgwt-server /root/paint1_trace_r2_copy $w
done; done
```
