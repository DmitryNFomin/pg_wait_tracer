# Click-to-painted latency, demo topology, 2026-10-07

Measurement only. No `src/` or `web/` change, no bound moved, no tolerance
touched, and `docs/DEMO_REHEARSAL_CRITERIA.md` is deliberately **not**
re-pinned here — another branch owns that, and the re-pin happens from
rehearsal data.

## Why these numbers were retaken

`docs/DEMO_REHEARSAL_CRITERIA.md` pins worst-case tab paint at **2079 ms
against a 3000 ms bound** (1.44x, n=4). Two things changed on 2026-10-07 that
invalidate it on both counts:

1. **PR #295 merged** (`30a20da`): timer-sleep waits leave DB Time, the time
   model gained a named `Idle` row with children, summary files went to v4.
2. **The demo workload was enriched** on `agent/demo-workload-no-sleep`:
   advisory-lock variety (four lock ids, 0.1/0.3/1.0/3.0 s holds), a 788 MB
   side table with random point reads and scattered updates, eight sessions
   firing an unprunable query over 200 partitions for `LWLock:LockManager`,
   and `pg_sleep` dropped from the demo loop.

## Topology — the demo's own, not a proxy

Real Chrome on the owner's Mac (built-in display, full screen, 100 % zoom)
-> the Go bridge `web/pgwt` running **on the Mac** -> `pgwt-server` over ssh
on the gate box -> a live `--mode full` capture of a real PostgreSQL 18 under
the real demo workload.

Headless Chromium on the box is a **different quantity** and is not measured
here. `docs/DEMO_REHEARSAL_CRITERIA.md` already records why one does not
transfer to the other.

| component | where | what |
|---|---|---|
| Chrome 154.0.8037.100 | Mac, macOS 15.6.1 (24G90) | demo profile `~/.pgwt-demo-chrome-profile`, `--remote-debugging-port=9333` for automation only |
| `web/pgwt` | Mac, port 8384 | built from this worktree with go1.26.0 |
| `pgwt-server`, `pg_wait_tracer` | `pgwt-gate-2` (`root@142.132.191.18`), cx33, 4 cores | built on the box from the tree below |
| PostgreSQL 18 | same box, port 5418, `shared_buffers` 128MB | `pgbench_accounts` 1,000,000 rows; `_smoke_io_load` 3,000,000 rows / 788 MB; `_smoke_lockmgr_fanout` 200 partitions |
| workload | same box | `pgbench -c 4 --rate=25` + `tests/live_loop_workload.py`, continuously, across all four runs |
| daemon | same box | `--daemon --pid <postmaster> -i 1 -T <fresh empty dir> --mode full -v` |

**Tree measured** (byte-identical on the Mac and on the box, verified):
`agent/demo-workload-no-sleep` @ `2cbc744`, rebased onto `30a20da`.
`sha256(src + tests + web, LC_ALL=C)` =
`e33c8430628ce7b06fdd954869fbe044ded3dfa8c997e76c100f386a85154817`.
The gate box was not provisioned or re-provisioned by this run; it already
carried every fixture, and the script that checks them refuses rather than
builds.

Each of the four runs got its **own fresh, empty trace dir** and its **own
900 s fill** — PR #295 refuses pre-v4 summary files, and a reused dir would
have fallen back silently to the slow raw path, making every number wrong in
the pessimistic direction. Runs are sequential: a second `--mode full` daemon
on the same 4-core box would have doubled capture overhead and contaminated
whichever run it overlapped.

## Files

| file | what |
|---|---|
| `paint_walk2.py` | the walk: 11 tabs x 3 navigations, readiness from `tests/ui_live_smoke.py`'s own `PANEL_CHECKS`, full page reload before every navigation, timeline reached by Sessions drill-down and walked LAST, Cmd+0 zoom reset, viewport asserted by two agreeing reads against 1710x981 @ DPR 2 |
| `walk-r[1-4].json` | raw per-navigation ms for every tab, plus viewport, Chrome version, active range, console errors, the scraped Overview time model, and the stamped box-side conditions |
| `conditions-r[1-4].txt` | box side per run: capture age, load average, liveness of pgbench / workload / daemon, trace dir listing, whole-trace volume and time model, distinct sessions |
| `paint_analyse.py` | the pure verdict function + its bypass suite (`--selftest`) |
| `red_demo_predecessor.py` | the same bypass cases against the predecessor harness's arithmetic, showing it turn each one into a reassuring margin |
| `redlive-bridge-down.json`, `redlive.txt` | a LIVE red: the real walk with the bridge killed, and the analyser refusing it |
| `red_splice.py`, `red_splice.txt` | the sharpest red, on real data: the REAL dead-bridge record for one tab spliced into the REAL `walk-r1.json`. The predecessor's margin goes **UP**, from 1.86x to 2.00x, because the tab that died was the worst one. The analyser refuses |
| `verdict.txt` | `paint_analyse.py walk-r[1-4].json` — the n=4 primary set |
| `verdict-r5-clean.txt` | r5 alone, the clean-topology confirmation (see `HARNESS_DEFECT.md`) |
| `HARNESS_DEFECT.md` | a defect in this harness found after the four runs: run r1's daemon was never stopped, so r2-r4 ran with two `--mode full` daemons attached. Direction, size, and what was done about it |

`paint_walk2.py` gained `--tabs`, `load_errors` and two `try/except` guards
*after* run r1 and before r2, so that a dead bridge produces a record instead
of a traceback (the live red below needs one). The guards are timing-neutral
on the success path — same calls, same order, same waits — and `walk-r1.json`
simply lacks the two informational fields `tabs_walked` and `load_errors`.

## The false negative this replaces

The harness behind the 2079 ms pin computed its worst case as

```
for t in TABS:
    r = results[t]
    if "error" in r: print("FAILED: ..."); continue   # <-- excluded
    worst = max(worst, r["max"])
```

A tab that never rendered — a dead bridge, a dead daemon, a 15 s readiness
timeout — was **dropped from the worst case, making the margin larger**. The
viewport, the active range and workload liveness were printed but never
gating. `paint_analyse.verdict()` refuses instead: see
`red_demo_predecessor.py`'s table, where all 13 constructed bypasses and one
real dead-bridge walk become a margin under the old arithmetic.
