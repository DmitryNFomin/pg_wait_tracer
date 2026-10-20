# 2026-10-20 — #466: #464's canonical execution boundary plus the four video-review web fixes

THREE baselines REGENERATED. SEVEN further cells in the same CI artifact
differ and are **deliberately NOT regenerated** — they are #263 for the fifth
time, see the last section.

| baseline | change | piece / issue |
|---|---|---|
| `table_queries.png` | 1280x325 -> 1280x**400** (+75px) | horizontal fit, review finding on #455 wording: the `Backend activity (observed span)` column is removed and its text folded into the query-identity cell, and the scope note becomes a three-line header block above the table. The header block is the growth; dropping the ~200px column is what takes `scrollWidth` 2159 -> 1710 at 1710x981 DPR2 |
| `waterfall_execution.png` | 1280x460 -> 1280x**507** (+47px) | Waterfall continuity/fit: the execution-context block was clipped to a few-pixel sliver and is now fully visible — three lines (`PID · Query · Executor interval`, `Containing backend activity`, `Bars show executor phase only…`) |
| `gallery/table-configs-queries-hostile-sql.png` | 650x245 -> 650x**368** (+123px) | the same identity-cell restructuring in the fixture view, **plus the #468 fixture fix in commit `3b6879c`** — see below |

The first two failed the compare job on a **size** change (run `39382668078`,
job `118495120541`; the artifact held `table_queries-actual.png`,
`waterfall_execution-actual.png` and a diff for the hostile-SQL cell, and
nothing else). Attribution came from `make ui-gallery BASE=48e1bce` run on
this branch **before** regeneration: `3 changed, 0 added, 0 removed, 44
unchanged`, exactly these three rows.

## The hostile-SQL cell was regenerated TWICE-over, on purpose

The first `snapshots` failure on this branch rendered that cell with the new
sub-line **run together with the query text** — `...WHERE aid = $2Backend
span unavailable: Not measured` and `—Backend span unavailable: Not
measuredPID unavailable`. The served app rendered it correctly stacked the
whole time. Cause (#468): `web/static/dev/gallery.html` hand-maintains a
**subset** of `style.css` and did not carry `.query-backend-span { display:
block }` or `.query-backend-span > span { display: block }`.

Regenerating at that point would have frozen the misrendering into this
directory as the asserted-correct appearance, and then cost a second
regeneration once the fixture was fixed. So the baselines were **not** taken
from that first artifact. Commit `3b6879c` copies both rules from
`style.css` verbatim and adds
`assert_gallery_query_span_parity()` to `tests/test_web_ui_snapshots.py`,
which runs BEFORE the snapshot is captured and therefore also gates
`--update-snapshots`: a divergent fixture can no longer become a baseline.
That commit was pushed first; only then was the regeneration dispatched.

The gate measures the **served** Queries view and asserts that measurement is
itself the stacked shape before comparing the fixture to it, so a
both-broken fixture and app cannot agree their way to green. Its two negative
controls delete each rule in turn and require the same parity assertion to
fail. From the regeneration run's own log:

    PASS: removing .query-backend-span fails the parity check
    PASS: removing .query-backend-span > span fails the parity check
    PASS: gallery query span matches served app: {'display': 'block', 'lines': ['block', 'block'], 'stacked': True}
    WROTE: gallery/table-configs-queries-hostile-sql.png (46503 bytes)

Note the order: the baseline is written only after the parity check passes.

The structural fix for #468 — have the gallery load `style.css` directly
instead of hand-copying a subset — is deliberately NOT in this PR. The audit
on that issue found `.queries-scope`, the backend-activity styles,
`.waterfall-context`/`[data-context]`, the concurrency panel styles and
`.queries-table` all missing from the subset too; several are harmless only
because those cells do not currently render those wrappers, which is luck
rather than design.

## How these were produced

CI `snapshots` job via `gh workflow run CI --ref agent/release-integration-3
-f update_snapshots=true` (run **39386912782**, snapshots job `118508842778`,
conclusion success), dispatched **after** the #468 fixture-fix commit
`3b6879c` was pushed, so the artifact reflects this branch's final rendering.
Never a local Playwright run, per RULE 2.

Every one of the artifact's 47 PNGs was compared against the committed
baseline before anything was copied: **37 identical, 10 differing, 0 new, 0
missing**. Only the three explained cells were taken. sha256 of the committed
files:

    table_queries.png                              45707e3f30a88ff971ea1ac95cf278eb96d43ef9ccb02a517b0669d1a5e03344
    waterfall_execution.png                        e710d67fd3691e511be14bfbe54f4704f061a9c0bc36ab6cf23fc64bb2bb5a36
    gallery/table-configs-queries-hostile-sql.png  78da98738bf9f91c3fd4d866142f8f591e6ce9c5f008d280df9ae3064f4dd314

Pins unchanged: `playwright==1.60.0`, chromium build 1223, container
`mcr.microsoft.com/playwright:v1.60.0-noble`. No `VERSION` change.

## Seven cells left on the pre-#213 rendering — #263 for the FIFTH time

The artifact's other differing PNGs are `aas_chart_overview`,
`fidelity_sampled_shading`, and `gallery/uplot-aas-{compare-ghostdiff,
dense-events,dense,live-ticks-tick4,sampled}` — the same seven, in the same
rect, already tabulated in `2026-10-01-222-waterfall-slowest-default.md`,
`2026-10-13-384-concurrency-panel-labels.md`,
`2026-10-17-449-integration-baselines.md` and
`2026-10-19-463-six-piece-integration.md`. Each differs by 10–51 bytes, all
PASSED the compare job inside tolerance, and `make ui-gallery BASE=48e1bce`
reports every one of them **unchanged**.

This is **#263** (the "4 CPUs" overlay chip rendering outside its chip;
#213's fix at `801793f` never reached these baselines). Neither piece in this
PR touches AAS, overlay-chip or uPlot code. Not regenerated here, same
standing policy as all four prior entries: copying them would smuggle #213's
fix into this PR under #464's and the web findings' issue numbers, which is
how a real regression hides inside mechanical churn.
