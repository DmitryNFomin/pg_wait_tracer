Replay over the real UI
=======================

Run on gate-2, with `pgwt-server` and `web/pgwt` built from this checkout and
Playwright Chromium installed. The bridge uses its production SSH path to
`root@127.0.0.1` (batch key authentication must already work). No daemon,
PostgreSQL or new workload is started. The shell entry point holds
`flock /tmp/pgwt-box-check.lock` for its entire run; build under that lock too.

```sh
flock /tmp/pgwt-box-check.lock make -j2 pgwt-server pgwt-client
bash tests/replay_ui.sh --trace-dir /path/to/retained/trace --out-dir /tmp/replay-green
```

This walks all eleven tabs using `ui_live_smoke.run_tab`, including its
Timeline session drill, existing panel checks and viewport check. A paused
URL fixes the window to the last 300 seconds of the capture (`--window-s`
overrides that span). No live ticks, blink measurements or workload liveness
are claimed. All eleven must pass; no known-failing exemptions apply.

Ownership is checked on each browser's own served `info` response before
waiting for any rows and again at comparison time. Production
`info` has no trace-header field. A test-only launcher starts the real server,
reads the uncompressed headers from the directory in that child's actual
argv, and adds `_replay_trace_headers` to `info` only. These include decimal
`start_wall_ns` (on-disk `start_time_ns`) and clock offset for every current
and rotated trace. The browser's captured response must match the fixture's
independently read headers exactly. Missing identity, a one-nanosecond
mismatch, an uncaptured response or a reconnect fails closed. Data responses
pass through verbatim. No production code or assets are modified.

`page.on('websocket')` captures sent requests and received replies. A test
observer also records the IDs resolved by the UI transport, excluding dropped
or superseded responses. After mount, with requests settled and Live off,
the comparator binds the displayed data to the latest consumed response for
that tab's command and checks the request's fixed window. It sends no second
data query. The existing `demo_workload_coverage.TAB_QUERIES` predicates grade
those same responses, including concurrency's real CPU-count threshold.

The independent comparator checks each table row's identity and numeric
cells, nulls, percentile bounds, units, counts and Overview remainders.
Charts check histogram counts and latency labels; timeline literal spans;
transition edges, node times and units; concurrency peaks; execution detail
lanes/spans; scatter coordinates/duration units; and matrix cell counts and
durations. JavaScript's existing epoch-number precision is accounted for;
string nanoseconds stay exact. Timeline density aggregation currently refuses
comparison; choose a narrower fixed window if it is encountered. This is not
a claim to test every pixel, tooltip, disclosure, composition bar or chart
interaction. UI/server agreement catches presentation errors but **cannot
detect a shared accounting error or prove absolute correctness**.

Each tab retains raw `websocket.jsonl`, `display.json`, a screenshot and video;
`summary.json` and `bridge.log` are at the output root. Output must be outside
the checkout. Use a fresh output directory for each run.

Both acceptance REDs are reproducible (each command must exit 1):

```sh
mkdir -p /tmp/replay-empty-trace
bash tests/replay_ui.sh --trace-dir /tmp/replay-empty-trace --out-dir /tmp/replay-empty-red
bash tests/replay_ui.sh --trace-dir /path/to/retained/trace --out-dir /tmp/replay-value-red --perturb-display
```

The empty directory fails during trace validation, before browser startup.
The value RED changes the *rendered DOM cell* for Overview DB Time to
`PERTURBED 999s`, after its real response was captured; it does not change
that response or the expected value. `test_replay_ui.py` also covers unit,
count, identity, missing-response and ownership mutations without a browser.
It runs from `unit_tests.list` and the existing navigation-test entry point
in full `make check`.
