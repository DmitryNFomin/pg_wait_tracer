# Demo runbook

How the demo is actually started, verified, walked, recovered, and shut
down. This is the only document that says that — `docs/DEMO_REHEARSAL_CRITERIA.md`
defines what a *clean rehearsal* is, and `docs/DEMO_MAC_WALK_CHECKLIST.md` is
the sign-off form for a walk — neither one is a "how to start it" sequence,
and until now that only existed in an orchestrating agent's own session.

Every command below is traced to a file, cited beside it. Anything that
isn't in the repo is marked **UNKNOWN — needs owner input** rather than
guessed at.

## 1. Topology

```
[Demo box: pgwt-stage]                    [Presenter's Mac]
PostgreSQL 18 (already running)
pg_wait_tracer --daemon  ───(unix socket:  <trace-dir>/pgwt.sock)
  writes trace files to <trace-dir>
pgwt-server (spawned by the bridge          web/pgwt (Go bridge)
  over ssh, one process per bridge            ├─ spawns: ssh <user>@pgwt-stage
  session, reads the trace files,             │     pgwt-server <trace-dir>
  computes aggregates, JSON lines               ├─ localhost:8384 HTTP + WebSocket
  on stdin/stdout)                              └─ real Chrome, dedicated demo
                                                    profile, full screen
```

- The daemon and PostgreSQL run **on the demo box** (`pgwt-stage`, a
  dedicated PG 18 `cx33`, not an ephemeral VM —
  `docs/DEMO_REHEARSAL_CRITERIA.md` "The demo machine"). Nothing eBPF runs
  on macOS (`CLAUDE.md`), so the capture is necessarily remote.
- The **Go bridge and the browser run on the Mac** (owner, 2026-09-28: "Demo
  will be using my Mac where you are running now" —
  `docs/DEMO_REHEARSAL_CRITERIA.md` "The demo configuration"). The bridge
  itself opens `ssh <target>` and runs `pgwt-server <trace-dir>` on the far
  end as a child process, talking JSON lines over that ssh session's
  stdin/stdout (README.md "Architecture" diagram, `## Web Investigation
  Client (pgwt)`); the browser talks to the bridge only over
  `localhost:8384` (default port, `web/main.go`; `README.md` flags table).
  No database credentials are needed — the bridge reads trace files, not
  the database (README.md).
- `pgwt-server` is spawned fresh by the bridge for each session; it is not a
  long-running service you start by hand.

## 2. Start-up sequence

Run steps 1–3 **on `pgwt-stage`** (ssh in first). Run steps 4–6 **on the
Mac**.

### 2.1 PostgreSQL

`pgwt-stage` is a dedicated, pre-provisioned box (owner, 2026-09-28: "we
will provision dedicated node couple days in advance" —
`docs/DEMO_REHEARSAL_CRITERIA.md`), so PostgreSQL should already be running.
Confirm, and (re)start via the cluster tooling the provisioning script
itself uses if it is not (`tests/provision-runner.sh`, which calls
`pg_ctlcluster "$V" main start` / `restart` for Ubuntu-managed clusters):

```bash
pg_lsclusters                    # confirm PG 18 is "online"
sudo pg_ctlcluster 18 main start # only if it is down
```

**UNKNOWN — needs owner input**: `pgwt-stage`'s exact OS/PG install layout
(Ubuntu `pg_ctlcluster` vs. a `systemctl postgresql` unit) is not pinned
anywhere in the repo — `tests/provision-runner.sh` only provisions Ubuntu
boxes. Confirm the actual service-management command on `pgwt-stage` once
it exists.

### 2.2 The daemon (`pg_wait_tracer --daemon`)

Default capture tier is **`tiered`** (low-overhead always-on sampler,
escalates to exact on demand — README.md "Capture Tiers"). The demo instead
needs **`--mode full`**, the expensive always-on-exact tier (6–30% overhead,
README.md): Waterfall's executions view needs the plan/execute USDT probes,
which are gated on `--mode full` only (`tests/demo_rehearsal.sh`, the
`# ── 2. Daemon` comment: *"see tests/ui_live_smoke.sh's DEVIATION note for
why full, not tiered: plan/execute USDT probes, which Waterfall's
executions query needs, are gated on --mode full only"*).

```bash
sudo ./pg_wait_tracer --daemon -T /var/lib/pgwt/traces --mode full -v
```

(`-T`/`--trace-dir` enables recording and picks the directory, README.md
"Trace Recording"; `-v`/`--verbose` prints diagnostics to stderr, README.md;
`--daemon` auto-discovers the single running postmaster when neither
`--pid` nor `--pgdata` is given — README.md "Auto-discovery". If
auto-discovery reports more than one instance, add `--pid <PM_PID>`.) This
matches `tests/demo_rehearsal.sh`'s own daemon invocation (`"$TRACER"
--daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" --mode full -v`), minus the
harness's `-i 1`/`--pid` (those are for its own scripted polling, not
needed interactively) and its `mktemp` trace directory (use a fixed,
memorable path for a demo instead — see the open question below).

Confirm it came up before moving on (the control socket only appears once
the daemon is ready): `tests/demo_rehearsal.sh` polls for
`<trace-dir>/pgwt.sock` up to 30 s before declaring the daemon dead.

```bash
ls -la /var/lib/pgwt/traces/pgwt.sock   # must exist, socket, mode 0600
```

**UNKNOWN — needs owner input**: whether the daemon should run inside
`tmux`/`screen`, or via the `nohup ... setsid -w` pattern
`scripts/demo-rehearsal-remote-run.sh` uses to survive its own ssh session
ending, so an accidental ssh drop during the live demo doesn't kill the
capture mid-walk. No repo script does this for an *interactively started*
daemon — the automated harnesses only detach their own wrapping script, not
a daemon a human starts by hand.

**UNKNOWN — needs owner input**: whether `/var/lib/pgwt/traces` should be
emptied before each demo session (old trace files accumulate;
`--trace-retention <H>` defaults to 24 h — README.md "Trace Recording") or
left to retention.

### 2.3 The workload

The demo's realism comes from `tests/live_loop_workload.py` (five
persistent sessions producing `Lock:relation`, `Timeout:PgSleep`,
`Lock:transactionid`, and query-id/duration variety across Queries,
Histogram, Waterfall and Scatter — `docs/DEMO_REHEARSAL_CRITERIA.md` "The
workload the criteria are written against") plus throttled pgbench, exactly
as `tests/demo_rehearsal.sh` starts them:

```bash
pgbench -U postgres -d postgres -c 4 -T <duration_s> --rate=25 &
python3 tests/live_loop_workload.py <duration_s> &
```

Both take a fixed duration in seconds (`tests/demo_rehearsal.sh`); pick a
duration that comfortably outlasts the demo, or restart them if either one
finishes early (see §5 "the workload dies"). `pgbench` needs its tables
already provisioned (`-i` is never re-run against a shared box's data —
`tests/live_daemon_lib.sh`'s `check_pgbench_provisioned`); confirm with
`psql -U postgres -d postgres -c 'select count(*) from pgbench_accounts'`
first if unsure.

### 2.4 `pgwt-server`

Not started by hand — the bridge spawns it over ssh for you (§1, README.md
"Architecture"). Just confirm the binary is on `pgwt-stage`'s `PATH` (or
know its path for `--server-path` below): `which pgwt-server` or
`ls /usr/local/bin/pgwt-server` (INSTALL.md "Web Investigation Client"
shows `scp pgwt-server root@db-server:/usr/local/bin/`).

### 2.5 The Go bridge — **run this on the Mac**

```bash
./pgwt --trace-dir /var/lib/pgwt/traces --server-path pgwt-server \
       root@pgwt-stage
```

(README.md "Web Investigation Client (pgwt)"; flags table: `--trace-dir`
and `--server-path` name the *remote* directory/binary, not anything local;
`--port` defaults to 8384.) Omit `--server-path` if `pgwt-server` is
already on the remote `PATH`.

The bridge calls macOS's `open <url>` on startup (`web/main.go`,
`openBrowser`), which launches your **default** browser/profile — not
necessarily the dedicated demo Chrome profile from §2.6. Either close that
auto-opened tab, or set the demo profile as default beforehand; either way,
do the actual walk from the profile in §2.6.

### 2.6 Chrome — 1710×981 CSS @ DPR 2, built-in display

Per the pinned client configuration
(`docs/DEMO_REHEARSAL_CRITERIA.md` "The client, pinned", raw measurement in
`docs/chrome-demo-viewport-2026-09-28.md`):

1. Detach any second display; use the Mac's built-in `Color LCD` as **Main
   Display**.
2. Launch the dedicated demo profile (`docs/chrome-demo-viewport-2026-09-28.md`
   "Environment"): same Chrome binary, `--user-data-dir=$HOME/.pgwt-demo-chrome-profile
   --no-first-run --no-default-browser-check --disable-session-crashed-bubble`.
3. Navigate to `http://localhost:8384/`, page zoom 100% (`Cmd+0`).
4. Enter full screen (green button or `Cmd+Ctrl+F`) and let the animation
   settle. Read `innerWidth`/`innerHeight`/`devicePixelRatio` **twice**; both
   must read **1710 x 981 CSS at DPR 2** before you consider the client
   ready — a single reading right after the transition can catch a
   transient, wrong value (`docs/DEMO_REHEARSAL_CRITERIA.md`, "A viewport
   measurement is two identical readings taken after the full-screen
   animation settles").

## 3. Verification before the audience arrives

Each check below tells you the component is *producing data*, not just
running — running-but-mute is the failure mode this section exists to
catch.

1. **Daemon**: control socket answers.
   ```bash
   echo '{"cmd":"status"}'  | nc -U /var/lib/pgwt/traces/pgwt.sock
   echo '{"cmd":"metrics"}' | nc -U /var/lib/pgwt/traces/pgwt.sock
   ```
   (README.md "Control Socket".) In `status`: `tier` should read
   `escalated`/`full` behavior consistent with `--mode full`, `backends` > 0.
   In `metrics`: `events_total` and `trace_events_written_total` climbing
   between two calls a few seconds apart is "actually tracing", not just
   "process alive". `ringbuf_drops_total`, `state_map_full_total`, and
   `seen_query_ids_full_total` should all read 0 — these are the
   lost-trace-event counters the demo's correctness rests on
   (`docs/DEMO_REHEARSAL_CRITERIA.md` §6 "Daemon integrity").
2. **Quick data sanity, no browser needed**:
   ```bash
   pgwt-server --dump /var/lib/pgwt/traces
   ```
   (README.md / INSTALL.md "Text Dump".) Confirms DB Time, top events, top
   sessions and top queries are non-empty straight from the trace files —
   the same floors `docs/DEMO_REHEARSAL_CRITERIA.md` §2 checks
   automatically (samples > 0, DB Time > 0, both `Lock:relation` and
   `Timeout:PgSleep` present).
3. **Bridge**: `curl -s http://localhost:8384/session` should return 200
   (this is the exact readiness probe `tests/demo_rehearsal.sh` and
   `tests/live_daemon_lib.sh`'s `url_ready` use before starting the walk).
4. **In the browser**: open Overview. A healthy state is: no error chip in
   the header, the AAS chart populated (not a flat empty axis), and the
   daemon self-metrics panel (README.md "Daemon self-metrics panel")
   showing a live tier and non-zero events/s. Watch two successive live
   ticks — the chart should visibly advance (a frozen chart is a real
   failure mode, not just "boring"; see §5 for the specific bridge-drop
   case).

## 4. The walk itself

Visit the 11 tabs **in the order the UI lists them**
(`web/static/index.html`, `<nav id="tabs">`):

1. Overview
2. Events
3. Sessions
4. Queries
5. Histogram
6. **Timeline** — reached **only** through a Sessions-row drill-down, never
   a bare tab click. Clicking the Timeline tab directly shows a "select a
   session" prompt, not a chart
   (`tests/ui_live_smoke.py`'s `_navigate_to_tab`: *"Timeline has no
   standalone entry point with data (a bare tab click shows the 'select a
   session' prompt) — it is reached by drilling into a session row"*; the
   same walk order and the same warning are already in
   `docs/DEMO_MAC_WALK_CHECKLIST.md` §2). From Sessions, click any session
   row; that row's Timeline is what renders.
7. Transitions
8. Concurrency
9. Waterfall
10. Scatter
11. Matrix

Drilling into a row (Sessions → Timeline, or Queries → Waterfall's
`query_id` pivot) pauses the live auto-refresh as a side effect
(`tests/ui_live_smoke.py`: *"EVERY drill gesture (P4) calls
stopAutoRefresh()"*). If you want live ticks to resume on a tab after a
drill, click the `#live-btn` in the header — it will show as not-`active`
until you do.

For each tab, the same four things `docs/DEMO_MAC_WALK_CHECKLIST.md` §2
grades: no console errors, no blank panel, data actually present, and it
paints without a visible spinner hanging.

## 5. Recovery

### The bridge drops mid-demo

Observed behavior, not just code inspection (`docs/DEMO_REHEARSAL_CRITERIA.md`
"Recovery after the bridge drops"): a tab left open against a bridge that
went away previously showed an error chip and eleven blank panels. The
code path is designed to recover — `connect()` re-fetches the session token
on every reconnect attempt, so the per-process token rotating on bridge
restart should not strand an open tab, with an exponential backoff capped
at 16 s — but that citation is inspection, not a verified observation; the
criteria doc is explicit that "inspection is not observation" here.

1. Restart the bridge (`web/pgwt ...`, §2.5) if it died, or simply wait if
   it's still running and only the network blipped.
2. **Do not reload the tab first** — give it up to ~16–20 s to reconnect on
   its own. If it hasn't recovered by then, reload.
3. If you do have to reload, expect to lose any in-progress drill-down
   state (you'll land back on Overview); the trace files and the daemon are
   unaffected.

### A panel is empty

- First check whether it's an honest "requires full-fidelity data" state,
  not a bug: in `tiered` mode, exact-required views (histogram,
  transitions, fingerprints, lock chains, interference, concurrency,
  executions/waterfall, scatter, transition matrix) report that message
  over windows with no escalation, rather than silently returning nothing
  (README.md, "Capture Tiers"). This should not happen during the demo
  since §2.2 starts the daemon in `--mode full`, always-exact — if you see
  it, the daemon is not actually running full mode; check `status` (§3.1).
- If it's genuinely blank with no message: check the daemon and bridge are
  both still alive (§3.1, §3.3) and reload the tab.

### The workload dies

`pgbench` or `tests/live_loop_workload.py` (§2.3) exit after their fixed
duration, or can die early. If the demo view goes quiet (AAS drops toward
idle, no new sessions in Sessions), restart whichever process ended, using
the same commands from §2.3 with a fresh duration.

### An orphaned Waterfall row pins to the top

If a query is cancelled with Ctrl-C mid-execution, its row in Waterfall's
executions table can be left open (never closes at the expected marker) and
then dominate the tab's default "longest running first" ordering, since an
open row's elapsed time keeps growing for as long as it's shown. **Click
"Show latest first"** (the sort toggle above the executions table) to fall
back to recency ordering and get the orphaned row out of the way.

This depends on the default-slice / orphan-row fix tracked in issue #222
(`web/static/lib/builders/waterfall.js`, `executionsSortToggleLabel` — the
toggle's states are "Show latest first" / "Show longest running first").
**As of this writing that fix is on `agent/waterfall-slowest`, not yet
merged to `master`** (`docs/DEMO_DELIVERY_QUEUE.md` §1.2, status READY —
listed as a demo blocker that must merge before the tag). Confirm it has
landed before relying on this recovery step; on current `master` there is
no sort toggle at all and Waterfall only ever shows the latest 100
executions by start time (issue #222's own description).

## 6. Shutdown

Tear down in the reverse order of §2, matching `tests/demo_rehearsal.sh`'s
own `cleanup()` (workload → pgbench → bridge → daemon, each a bounded
signal-then-poll-then-KILL wait — `stop_pid` in `tests/live_daemon_lib.sh`):

```bash
# On pgwt-stage:
kill <live_loop_workload.py PID>     # or let its fixed duration expire
kill <pgbench PID>                   # or let it expire
kill -INT <pg_wait_tracer --daemon PID>   # daemon: SIGINT, not TERM
```

```bash
# On the Mac:
# Ctrl-C the ./pgwt process (or kill it) — this closes the ssh session and
# ends the remote pgwt-server child with it (README.md architecture: the
# bridge OWNS the ssh session pgwt-server runs inside).
```

Leave PostgreSQL running — `pgwt-stage` is a persistent, dedicated box
(§2.1), not a throwaway VM; nothing in this repo stops PostgreSQL as part
of a demo teardown.

**UNKNOWN — needs owner input**: whether to delete `/var/lib/pgwt/traces`
after each session or let `--trace-retention`/`--retention-gb` (README.md)
age it out — no script in the repo makes this decision for a *demo* box
(the rehearsal harness's `cleanup()` always deletes its own `mktemp`
directory, but that's a throwaway-VM pattern, not necessarily right for a
box meant to keep running).

## 7. Known rough edges a presenter should not be surprised by

From `gh issue list` (2026-09-30), filtered to ones with a symptom visible
during the walk above, not test-only findings:

- **#222** — Waterfall's default view is the latest 100 executions by
  start time, not the slowest, so a slow query can be invisible on a busy
  system unless you know to pivot from Queries → click a row → Waterfall.
  See §5's orphan-row recovery for the same issue's fix and its merge
  status.
- **#172** — Transitions tab reproducibly exceeds the UI test suite's blink
  threshold by a small margin (ratio ~0.0018–0.0024 vs. a 0.001 gate). On
  stage this reads as a faint, brief flicker on that one tab, not a broken
  panel.
- **#160** — Queries tab shows a real repaint blink (~2.18% of pixels)
  specifically on the **first pass** of a long capture, when the query set
  is still small and changing shape — i.e., right when a demo viewer first
  looks at it. It settled clean on later passes of the same capture in the
  rehearsal that found it.
- **#168** — The Timeline chart can show a degenerate, sub-pixel wait bar
  pinned at the left axis edge for a session whose window got clamped; a
  cosmetic edge artifact, not missing data.
- **#197** — No time-to-first-paint bound is set yet for any tab
  (`docs/DEMO_REHEARSAL_CRITERIA.md` §4 still has an open row for this).
  Don't be alarmed by a tab taking a couple of seconds to paint; there is
  currently no defined threshold for "too slow."
- **#215** — The first several live ticks after startup can look like the
  view isn't advancing yet before genuinely new data has accumulated; give
  it a few ticks before judging freshness, especially right after §2–§3.
- **#242** — A previous diagnostic walk's claim that Timeline itself takes
  30 s to paint is **retracted**: the probe was timing the "select a
  session" prompt from a bare tab click (see §4), not a chart. There is no
  known Timeline paint defect. The same issue's Waterfall-coverage finding
  is explained by the same #222 recency-slice bug, not a missing capture.

## Open questions for the owner (collected)

1. `pgwt-stage`'s exact OS/service-manager for PostgreSQL (§2.1).
2. Whether the daemon should run detached (`tmux`/`nohup+setsid`) so an ssh
   drop during the live demo can't kill the capture (§2.2).
3. Whether `/var/lib/pgwt/traces` should be cleared before each session
   (§2.2, §6).
4. Whether to delete trace files after each demo session or let retention
   age them out (§6).
