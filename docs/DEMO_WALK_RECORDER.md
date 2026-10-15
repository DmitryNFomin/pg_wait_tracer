# Chrome demo walk recorder

Run from this worktree with the same Python environment used by `make check`
(Playwright, NumPy, Pillow and websockets). Installed Google Chrome is required;
the recorder always launches `chromium` with `channel="chrome"`.

```sh
python3 scripts/demo-walk-record.py --mock --run-id mock-review
python3 scripts/demo-walk-record.py --url http://localhost:8384/ --run-id pre-tag-SHA
python3 scripts/demo-walk-record.py --url http://localhost:8384/ --archived --run-id mac-walk-of-archived-capture
```

`--archived` (default off, `--url` only): sets the window to the served
trace's own `[from_ns, to_ns)` span (the "All" quick-range, `data-range="0"`)
instead of the live 15-minute quick-range. For a capture already older than
15 minutes relative to Mac wall-clock — e.g. replaying a retained capture
whose box-side bridge was torn down — the stock live gesture shows every
panel empty, because the trace's data ends before the live window starts.
This is not the live-demo gesture; `walk["window"]` records which one ran,
and `analysis.md` says so explicitly so a reader cannot mistake a full-span
archived recording for a recording of the live rolling-window gesture.
`--mock` always serves data anchored to wall-clock "now", so `--archived`
with `--mock` is refused rather than silently doing nothing useful.

**`--archived` cannot produce a GIF, by design.** The trace's own span is
fixed, so nothing in the window advances: the Overview AAS chart's pixels
never visibly change, and `chart_change()`'s motion gate (the same gate a
live walk relies on as animation evidence) correctly refuses before any GIF
is cut. Confirmed end to end against a real archived capture (2026-10-14,
`tests/results/demo_walk/dryrun-401-e2e.refusal.json`): every one of the
eleven tabs is recorded and the continuous Queries → Waterfall drill-down
completes with zero console errors, then the walk refuses at exactly this
motion gate — `--archived` is for *inspecting* a retained capture (panel
content, headline numbers, `walk.json`), never for recording its demo GIFs.
Record real GIFs against a capture a daemon is still writing to (no
`--archived`).

For the same reason, `check_overview_wait_mix()` skips (never fakes passing)
the live-only "Overview's visible wait mix must change" assertion under
`--archived`. The skip is never silent: it is recorded verbatim on the
Overview tab (`walk["tabs"][0]["wait_mix_check"]`) and in `analysis.md` as
`"wait-mix assertion skipped: archived window (fixed span, nothing
advances)"` — a live walk instead records `"wait-mix assertion: changed
across the dwell"`. A skipped check that reads like a passed one is a defect
class this project has hit repeatedly; this one says so in the artifact.

`--headed` displays the dedicated window. Browser content is always 1710×981
CSS pixels at deviceScaleFactor 2; two DOM geometry readings must agree.
Screencast images are Chrome's native CSS-resolution compositor frames,
1710×981, with their actual CDP timestamps. The outer window is adjusted by
the measured compositor clipping difference, without changing content geometry.

The runbook's numbered order is parsed and checked against the smoke library's
tab list. It contains **no dwell times**. The explicit fallback is ten seconds
per tab (two live tick intervals). `--dwells durations.json` accepts an object
with all eleven lowercase tab names and durations of at least five seconds.
The order never changes. Timeline returns to Sessions and clicks a session;
flow variants are never loaded. Range stays 15 minutes and compare stays off.
The existing smoke navigator resumes live after tab entry/drills. Filters from
Timeline persist during the walk, matching the runbook's literal gestures.

The connection wait is the same `#status.connected` gate used in
`ui_live_smoke.run_tab`. Reused browser readiness helpers from
`tests/ui_live_smoke.py`: `_navigate_to_tab`, `_wait_for_mount_at_or_after`,
`_poll_render_check`, `_READY_SELECTOR` and `PANEL_CHECKS`. Reused pure helpers
from `tests/ui_live_smoke_lib.py`: `render_check_ok`, `viewport_mismatch_reason`
and `TABS`. That library has no browser wait helper; the driver does.

CDP screencast was chosen for [per-frame capture timestamps](https://chromedevtools.github.io/devtools-protocol/tot/Page/#type-ScreencastFrameMetadata).
Static pages emit sparse frames, so each capture cadence restarts screencast
to request a fresh compositor frame. No synthetic frames fill capture gaps.
Cadence pulses run at twice the minimum to leave room for DOM inspection.
An exact duplicate CDP delivery (same timestamp and SHA-256) is acknowledged
but not counted; changed pixels at the same timestamp or regressed timestamps
still refuse with code 5.
Every dwell must have at least floor(duration × fps) actual frames and no gap,
including its edges, over two frame intervals. DOM observations use epoch
seconds too; recording/video offsets derive from the original CDP timestamps.
`--fps` controls this minimum cadence (default 12, range 1–16). GIFs sample
at 10 fps; a lower capture cadence can therefore trigger the motion refusal.

A separate Chrome page encodes those saved JPEGs through WebCodecs VP8. A small
WebM muxer preserves their timestamps; Chrome must successfully decode the
result and report the recorded dimensions and duration. No ffmpeg executable,
Python ffmpeg wrapper, or additional encoder package is used.

Output is `tests/results/demo_walk/<run-id>/`: `frames/*.jpg`, `walk.webm`,
`walk.json`, `analysis.md`, `headline.gif` and `drill-down.gif`. A successful
run is published by renaming its staging directory only after **all** checks.
On refusal the staging directory is deleted, including media; only
`<run-id>.refusal.json` remains. Existing successful runs are never overwritten.
The committed `.gitignore` in the output directory ignores all generated files.

Every tab's JSON records the dwell bounds, coverage, frame indices, visible
DOM summary metrics, table headers/rows, panel readouts and console errors.
Only elements intersecting the viewport are included. Chart readiness probes
read chart options; headline numbers are DOM text, never OCR. `analysis.md`
is generated from this JSON's observations, including unchanged values.
Console errors include **every** error, including `[pgwt]`, page exceptions
and unhandled rejections. No error is whitelisted.

After the eleven tabs, supplementary setup clears the previous session filter.
The continuous clip opens Queries, ranks
rows by visible p95 execution duration (average breaks ties), clicks the
visible `Executions →` cell, selects the execution row, then clicks a drawn
wait bar. The wait name, PID and duration must appear in the DOM readout.
This uses the real cell pivot; ordinary query-row clicks open Events.
Overview must visibly change table values during its dwell for GIF 1 to pass.
During Overview the walk also sweeps the native AAS crosshair across buckets,
showing the UI's time, wait and AAS readout. The headline takes about five
seconds of that inspection, including a live tick. Before the query pivot the
walk inspects AAS, then sweeps across the clicked Waterfall wait bar to show
its native tooltip. These are recorded mouse gestures, with the normal
five-second live polling unchanged. The JSON records the inspection paths.

The mock fixture in `tests/demo_walk_mock.py` wraps `tests/mock_server.py`,
serves its unchanged UI, and changes the CPU/IO mix on successive Overview
requests and aligns the first session PID with the execution fixture. Both the
JSON and analysis explicitly label this **mock evidence**;
it does not validate the final real bridge, workload or pre-tag SHA. The mock
also answers Chrome's implicit favicon request with 204, matching the real
bridge's `/favicon.ico` route (#401) rather than diverging from it.

[Pillow writes the GIFs](https://pillow.readthedocs.io/en/stable/handbook/image-file-formats.html#gif).
Each must be strictly below 10,000,000 bytes and pass both refusal gates
**before the writer runs**: at least max(ceil(4 × fps), ceil(source span × fps))
sampled frames (at least 40 at the default 10 fps), and the motion gate below.
The same motion gate runs again after palette quantization (color reduction
cannot erase the evidence) and again on the decoded GIF after encoding.
Frame durations, source span, actual decoded count and all measured deltas
are saved in `walk.json`.

**The motion gate measures motion across the whole clip, not between every
pair of adjacent samples** (issue #411). The live UI ticks every 5 seconds
(`app.js` `startAutoRefresh`'s `setTimeout(.., 5000)`), and each clip is
designed to cover roughly one tick (see `drill()`/`dwell()`'s own comments,
and "about five seconds of inspection, including a live tick" above) — so a
CORRECT recording of this product looks like two static states joined by a
single step, not continuous motion: between ticks, consecutive ~100ms-apart
sampled frames are pixel-identical *by construction*, because the data they
draw has not changed yet. An earlier version of this gate demanded every
consecutive sampled pair individually clear **0.1%** changed pixels at a
**24**-luminance-level delta, which refused every correct recording of a
live Overview clip, not just broken ones — measured on four independent
live runs against a real demo-workload capture (three fixed early 5s
windows plus one exhaustive scan of the whole recorded Overview dwell for
the best-possible window), all landing at ~0.016% per consecutive pair,
about 6x under that floor, while two *other*, independent checks on the
same dwell confirmed the data genuinely does move:
`check_overview_wait_mix`'s DOM text (the wait-mix numbers visibly change
every run) and `chart_change()` (best later frame vs. frame 0, needs ≥1%,
which already passes every run). The content was never the problem;
demanding smoothness from a step function was.

The gate now checks two things, both at **1%** changed pixels (the same
"visibly changed" bar `chart_change()` already uses and already clears on
every live run — not a new number invented to let a known-small number
through), both required (neither is an average, so one cannot paper over
the other failing):
1. **first vs. last frame of the whole clip** — the clip must look visibly
   different end to end. A genuinely static clip (nothing ticks, nothing
   moves, every frame identical) scores 0% here and still refuses.
2. **the single biggest consecutive-pair jump** — at least one step
   somewhere in the clip (the tick landing) must itself clear 1%. This
   stops (1) from passing on a single corrupted/flickered frame while
   everything else is frozen: a real tick's data changes by far more than
   1% of the canvas when it lands, so a genuine tick clears this easily; a
   one-frame JPEG glitch generally will not.

Do not lower the 1% floor to make a specific clip pass — it is already an
existing, independently-verified threshold used elsewhere in this same
pipeline; weakening it would be removing the gate's teeth under a different
name. The headline GIF's start/end frames are chosen by scanning the whole
recorded Overview dwell for the best-positioned 5-second window
(`best_headline_window()`) instead of a fixed early offset, since the one
real tick in a given dwell can land anywhere in it; the motion gate itself
is unchanged by this and still decides on its own terms.

Pillow coalesces identical frames even with optimization disabled. The old
4 fps cuts sampled identical crops across long five-second polling holds and
missed the final Overview change; Pillow merged the headline to two frames
and the drill to five. No duplicates are padded and no interpolated data or
motion is drawn into the images.

**The post-encoding frame-count gate measures distinct frames surviving
encoding, not `sampling fps × duration`** (issue #413, the frame-count twin
of the motion gate above — same root cause, different check). The old rule
demanded every frame sampled at the GIF's OWN playback rate (10fps) survive
Pillow's deduplication distinct from its neighbor, i.e. that the clip never
go idle for even 100ms — the same false "continuous change from a UI that is
idle between ticks and during network waits" assumption the motion gate
made, this time applied to frame count. A live drill-down clip spanning the
whole `drill()` interaction (~11.2s, including its genuine network-latency
waits) measured 75 distinct decoded frames against an old requirement of 112
(span × 10fps — zero tolerance for any duplicate anywhere in an 11-second
clip); 75 distinct frames over 11.2s is a perfectly watchable animation. The
new floor is `DECODED_MIN_FPS` (2, deliberately far below the 10fps sampling
rate, which governs an unrelated concern — how densely the source video is
polled for candidate frames): a floor on how fast the *output* must visibly
change to read as an animation rather than a slideshow, not on how many of
the sampled frames survive untouched. A clip that really is just two static
states (e.g. return-to-start or any clip whose content holds still for most
of its length) still correctly refuses — Pillow legitimately coalesces it to
2-3 frames, nowhere near a watchable minimum at any defensible floor. Do not
raise `DECODED_MIN_FPS` to fit a specific clip, and do not pulse the capture
harder hoping for more frames during a real idle wait — the screen genuinely
is not changing then, so more captures only produce more duplicates Pillow
would coalesce right back out; both would be buying a pass, not fixing a
gate.
Inclusive cuts now always retain the last recorded frame at the final sample.
GIFs use a 1000-pixel canvas. Their data definitions include inclusive source
frame bounds, fps, output width and crops in CSS pixels. Crops can be a single
`[x,y,width,height]` rectangle, or bands of rectangles: rectangles within a
band join horizontally; bands stack vertically. This keeps relevant table
columns at their recorded text size while the whole AAS chart fits the width.
The drill clip's crop keyframe changes framing at its recorded Waterfall
landing. No text is drawn onto frames. Inspect readability at the destination
GitHub width before owner sign-off; byte counts alone cannot establish it.
Overview's `%DB Time` column is preserved in full: the small blue rectangle
beside IO's `5.0s` was the cropped tail of its real `.pct-fill` percentage bar
(39.2% of DB time), not a selection, hover or rendering defect.

Edit the two `gifs` definitions in `walk.json`, then re-cut without a browser:

```sh
python3 scripts/demo-walk-record.py --recut tests/results/demo_walk/mock-review/walk.json
```

Exit codes: 0 complete; 2 inspected empty panel/missing usable drill data or
no recorded Overview change; 3 unable to inspect (connection/navigation/mount,
viewport, DOM or browser failure); 4 console/page/rejection error; 5 insufficient
or malformed frames, gaps or dropped video frames; 6 configuration, runbook
drift, output/codec verification or invalid/oversized GIF.
New codes: **7 GIF frame count gate** (sampled or decoded); **8 GIF motion
gate** (first/last, or the single biggest consecutive-pair jump; sampled
crop, palette or decoded — see the motion gate description above).
Existing codes retain their meanings. An empty-motion GIF is an observed
artifact refusal, with the failed check named in the diagnostic.
The distinction between 2 and 3 is observed-negative versus unobserved, rather
than turning an inspection failure into evidence of emptiness.

This tool produces local review artifacts. Publishing to GitHub and recording
the final pre-tag SHA remain subsequent steps; it does not push or open PRs.
