# Harness defect found after the fact, and what it does to the numbers

**Found 21:10 UTC, after all four walks.** `mac_chain.sh` stopped each run's
daemon when that run finished, but run **r1**'s daemon was started by hand
*outside* the chain (`box_run_start.sh r1`) and the chain was invoked as
`mac_chain.sh r2 r3 r4`, so nothing ever stopped it. It ran for 4290 s:

```
$ ps -eo pid,etimes,args= | grep "pg_wait_tracer --daemon"
2162010    4290 ./pg_wait_tracer --daemon --pid 1895007 -i 1 \
                   -T /root/paint2_trace_r1 --mode full -v
```

So during **r2, r3 and r4** there were **two** `--mode full` daemons attached
to the same postmaster on a 4-core box, not one. `mac_chain.sh`'s own comment
says a second concurrent daemon would contaminate the run it overlaps — and
then a daemon it did not know about did exactly that.

## Direction, and why it does not rescue the headline

Extra capture overhead on the box makes paint **slower**, so the bias is
pessimistic, not optimistic. And the only **uncontaminated** run, r1, is the
one that produced the worst case:

| run | daemons attached during its walk | worst ms | worst tab |
|---|---|---:|---|
| **r1** | **1 (clean)** | **1616** | scatter |
| r2 | 2 | 1469 | queries |
| r3 | 2 | 1592 | queries |
| r4 | 2 | 1485 | queries |

The headline 1616 ms therefore comes from the clean run, and the three
two-daemon runs are all *faster* than it. A reader should still not treat
r2-r4 as clean-topology data.

## How large was the contamination, measured rather than assumed

| run | box loadavg (1m) at walk end | trace events | trace duration s | events/s |
|---|---|---:|---:|---:|
| r1 (clean) | 0.39 | 4,066,533 | 1055.1 | 3,854 |
| r2 | 0.33 | 3,993,600 | 1030.4 | 3,876 |
| r3 | 0.23 | 3,997,696 | 1031.9 | 3,874 |
| r4 | 0.40 | 4,047,420 | 1028.3 | 3,936 |

Load average and captured event rate are indistinguishable between the
one-daemon and two-daemon runs (loadavg 0.23-0.40 with no ordering by daemon
count; rate within 2.1 %), and every run reported `Fidelity: exact` with 29
backends — so neither daemon lost events to the other. The contamination is
real but small on this box at this load.

## What was done about it

Run **r5** was added as a clean-topology confirmation: r1's daemon was killed
first, verified gone, and r5 ran as the only daemon on the box, same fresh
empty trace dir and same 900 s fill. It is analysed separately in
`verdict-r5-clean.txt` (and `verdict.txt` keeps the n=4 primary set, which
matches the pinned figure's own n for a like-for-like comparison).

r1's volume figures above come from a `pgwt-server --dump` run by hand at
20:14 UTC, one minute after r1's walk ended, recorded in
`conditions-r1-dump-2014utc.txt`; the automated conditions collector only
started recording those lines from r2 onwards.
