INFERRED — **This is a product defect. Choose (a) as the smallest fix:** retain the last accepted resolved window and notice during an ordinary Live refresh; replace them when the next valid response arrives. An “updating” indicator is optional and cannot substitute for consistent labels.

VERIFIED — I opened `diag18-raw/el8-v2/ui-load-{1,2,3}/guard-08-panel-queries-capture_start-at-timeout.png`: runs 1 and 3 show a plain 15.0m header beside a 4.1m badge, with “Incomplete capture” and a coverage-unavailable pane. Run 2’s screenshot already shows the restored capture-start notice and a 4.2m badge; it does not itself capture the contradiction.

VERIFIED — Run 1’s corresponding JSON, line 1, contains DOM snapshots at 10/30/59.8 seconds: the notice is resolved/plain/resolved respectively, while the coverage-unavailable pane remains present and its intervals advance. This establishes a transient header defect rather than a missing pane.

VERIFIED — At `b15ec1d2`, `app.js:1472–1474` reanchors and redraws; `state.js:164` clears `resolvedLive`; `app.js:1191` falls back to requested bounds. `CLAUDE.md:24–34` explicitly classifies disagreeing views and contradictory labels as defects.

INFERRED — Preserve a **display snapshot**, separate from the advancing request anchor. Include resolved bounds, clip reason and request/boot identity; keep the displayed duration consistent with it. Retaining only the clipped text is insufficient for an unclipped resolved window.

VERIFIED — `state.js:206–216` returns no special label when `clip` is absent; Live requests themselves use `live_span`, not display bounds (`state.js:189–203`).

INFERRED — Immediately invalidate the retained Live snapshot on a user range/context change, leaving Live, an observed boot change, or a current request’s error. Render an explicit pending/unavailable/error state as appropriate; any retained historical data must be marked stale. Ignore superseded responses and errors, and prevent pre-invalidation replies from restoring the old label. A valid coverage-withheld response with resolved bounds still supplies window evidence.

INFERRED — **Poll invariant:** after the first accepted resolution for the current Live request context and boot, every poll must show mutually consistent resolved window labels through ordinary refreshes, until replacement by a valid matching response or explicit invalidation, which must remove the old current-window claim.

INFERRED — Unit tests should resolve a clipped window, advance the server clock, reanchor with the same span, and assert the retained bounds/notice before supplying the next response; today’s `state.js:164` makes that assertion fail. Also cover unclipped windows, replacement, each invalidation, and late responses after invalidation.

INFERRED — Assert the invariant on **every poll**, independently of numeric availability, and latch the first violation so recovery cannot erase failure. Keep `numeric_loss`’s evidence checks; its final notice read may remain supplementary.

VERIFIED — Today the numeric wait checks only panel success (`tests/ui_live_smoke.py:810–815`), and `numeric_loss` samples the notice once (`:867–873`).

INFERRED — For deterministic mock-tier failure, establish one resolved paint, allow the next tick’s `info` response, then hold **both AAS and tab window replies** behind a test-controlled barrier until the browser has sampled the pending state. Assert, release, and verify replacement. Exercise both reply orders and capture-start/restart clips; a fixed sleep alone is weaker.

INFERRED — For the restart failure, measure monotonic elapsed time from replacement-daemon launch to publication of its first qualifying watermark, identified by the new boot, continuing observation beyond 15 seconds. Compare that latency with the agreed restart-readiness requirement: exceeding the harness timeout alone cannot distinguish a product defect from an unsuitable timeout.

VERIFIED — The present predicate is new boot plus `watermark_wall > boot_start_wall`, under a 15-second deadline (`tests/ui_live_smoke.py:978–995`).

INFERRED — Restart diagnosis is independent of the label fix and need not expand that implementation unit. It does not authorize merging through an unexplained failed required gate: retain the failure, obtain the measurement and disposition, and satisfy existing merge requirements without silently increasing the timeout.