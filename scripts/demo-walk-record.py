#!/usr/bin/env python3
"""Record the pinned Chrome demo; see docs/DEMO_WALK_RECORDER.md."""
import argparse
import base64
from contextlib import ExitStack
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from demo_walk_record_lib import (Refusal, chart_change, compose, coverage, frame_indices,
                                  MOTION_LEVEL, sample_indices, save_walk, write_analysis, write_gif,
                                  young_capture_refusal)
from PIL import Image, ImageChops

WIDTH, HEIGHT, DPR = 1710, 981, 2
RESULTS = ROOT / "tests/results/demo_walk"

# issue found live (same family as #411's MOTION_FLOOR and #413's
# DECODED_MIN_FPS): encode_video()'s own post-encode decode/seek
# verification used to budget a flat 10 seconds, sized for whatever walk
# happened to be running when it was written -- but it verifies an
# artifact (walk.webm) whose size VARIES with the walk's own length.
# Measured today: three live runs encoding 133-141s of video (3240-3380
# frames) passed; three later runs, each ~100-150 frames longer (from
# measurement round trips since removed -- see _tooltip_bboxes_at()),
# failed this SAME check 3/3 times with "WebM seek timed out" -- not a
# broken video, a bigger one taking longer to seek. The passing runs
# themselves only cleared the old flat budget by ~2s out of 10 -- a near
# miss, not a comfortable margin. Scale the budget with the video's own
# duration instead. VERIFY_MS_PER_VIDEO_SECOND is deliberately generous:
# at 100ms of verification budget per 1s of video, the longest passing
# walk measured today (140.9s) gets 14.1s, real margin instead of a near
# miss, and a hypothetical 300s walk gets 30s. VERIFY_MIN_MS is the
# original flat floor, unchanged, so a short clip's budget can only grow
# from this change, never shrink below what has always worked. This still
# has teeth: whatever it computes is a concrete, finite number of
# milliseconds -- a genuinely stuck/undecodable video still times out,
# it is never given an unbounded budget (test_video_verify_timeout_*).
VERIFY_MS_PER_VIDEO_SECOND = 100
VERIFY_MIN_MS = 10_000


def video_verify_timeout_ms(duration_ms):
    return max(VERIFY_MIN_MS, VERIFY_MS_PER_VIDEO_SECOND * duration_ms / 1000)


AMBIGUITIES = [
    "Section 4 specifies no per-tab dwell times; explicit default is 10 seconds per tab, overridable by --dwells JSON.",
    "Section 4 says 'in the order the UI lists them' but Timeline requires a return to Sessions after Histogram; the numbered order is retained and that return is a navigation step.",
    "Section 4 mentions Queries → Waterfall's query_id pivot; ordinary row click goes to Events. The visible Executions → cell opens Waterfall.",
]

OBSERVE_JS = """() => {
    const visible = e => {
        const r = e.getBoundingClientRect(), s = getComputedStyle(e);
        return s.visibility !== 'hidden' && s.display !== 'none' &&
            r.width > 0 && r.height > 0 && r.top < innerHeight && r.bottom > 0 &&
            r.left < innerWidth && r.right > 0;
    };
    const text = e => e.innerText.trim().replace(/\\s+/g, ' ');
    const headlines = [...document.querySelectorAll('#summary-bar .metric')]
        .filter(visible).map(e => ({label:text(e.querySelector('.metric-label')),
                                  value:text(e.querySelector('.metric-value'))}));
    const table = document.querySelector('#table-container table');
    const table_rows = table ? [...table.querySelectorAll('tbody tr')].filter(visible)
        .map(r => [...r.querySelectorAll('td')].filter(visible).map(text)) : [];
    const headers = table ? [...table.querySelectorAll('th')].filter(visible).map(text) : [];
    const readouts = [...document.querySelectorAll('#table-container .view-title, '
        + '#table-container .chart-readout, #table-container [id$="banner"], '
        + '#executions-counts-label, #executions-sort-label')].filter(visible).map(text);
    const issues = [...document.querySelectorAll('.pane-error, #version-skew, #degraded-overlay')]
        .filter(visible).map(text);
    return {timestamp_s:Date.now()/1000, headlines, headers, table_rows, readouts, issues,
            status:document.querySelector('#status')?.innerText,
            tab:window.__pgwt?.activeTab, range:document.querySelector('#time-range')?.innerText,
            live:document.querySelector('#live-btn')?.classList.contains('active')};
}"""


def runbook_plan(tabs, overrides):
    body = (ROOT / "docs/DEMO_RUNBOOK.md").read_text()
    section = body.split("## 4. The walk itself", 1)[1].split("## 5.", 1)[0]
    listed = re.findall(r"^\d+\. (?:\*\*)?([A-Z][a-z]+)", section, re.M)
    if [s.lower() for s in listed] != tabs:
        raise Refusal(6, f"runbook order differs from readiness helper: {listed}")
    # Do not silently substitute our defaults if the runbook later gains timings.
    if re.search(r"\bdwell\b|\b\d+\s*(?:seconds?|secs?)\s+(?:on|per|each)\b", section, re.I):
        raise Refusal(6, "runbook has new dwell instructions: update timing data explicitly")
    dwells = {tab: 10.0 for tab in tabs}
    if overrides:
        supplied = json.loads(Path(overrides).read_text())
        if set(supplied) != set(tabs):
            raise Refusal(6, "--dwells must specify exactly all eleven tabs")
        dwells = {k: float(v) for k, v in supplied.items()}
    if any(not math.isfinite(v) or v < 5 for v in dwells.values()):
        raise Refusal(6, "every dwell must be finite and at least five seconds")
    return dwells, {"path": "docs/DEMO_RUNBOOK.md", "sha256": hashlib.sha256(body.encode()).hexdigest(),
                   "tab_order": listed, "dwell_seconds": dwells, "ambiguities": AMBIGUITIES}


class Capture:
    def __init__(self, page, session, root, fps):
        self.page, self.session, self.root, self.fps = page, session, root, fps
        self.frames, self.errors, self.capture_errors = [], [], []
        self.current = "startup"
        self.stopped = False
        page.on("console", lambda m: self.error("console", m.text) if m.type == "error" else None)
        page.on("pageerror", lambda e: self.error("pageerror", str(e)))
        session.on("Page.screencastFrame", self.frame)

    def error(self, kind, message):
        # CDP timestamps and Python wall time are both UTC epoch seconds.
        self.errors.append({"timestamp_s": time.time(), "tab": self.current,
                            "kind": kind, "message": message})

    def frame(self, event):
        from PIL import Image
        try:
            metadata = event["metadata"]
            timestamp = metadata["timestamp"]
            data = base64.b64decode(event["data"], validate=True)
            with Image.open(io.BytesIO(data)) as image:
                dims = image.size
            if dims != (WIDTH, HEIGHT):
                raise ValueError(f"screencast size {dims}, expected {(WIDTH, HEIGHT)}")
            digest = hashlib.sha256(data).hexdigest()
            # A cadence restart and a compositor update can deliver the same
            # native frame twice. Acknowledge that exact duplicate, but never
            # count it toward coverage or invent a later capture timestamp.
            if (self.frames and timestamp == self.frames[-1]["timestamp_s"]
                    and digest == self.frames[-1]["sha256"]):
                return
            if self.frames and timestamp <= self.frames[-1]["timestamp_s"]:
                raise ValueError("screencast timestamp missing, duplicated or regressed")
            index = len(self.frames)
            path = f"frames/{index:06d}.jpg"
            (self.root / path).write_bytes(data)
            self.frames.append({"index": index, "timestamp_s": timestamp, "path": path,
                                "size": list(dims), "metadata": metadata,
                                "sha256": digest})
        except Exception as exc:
            self.capture_errors.append(str(exc))
        finally:
            self.session.send("Page.screencastFrameAck", {"sessionId": event["sessionId"]})

    def pulse(self):
        self.session.send("Page.stopScreencast")
        self.session.send("Page.startScreencast", {"format": "jpeg", "quality": 95,
                                                  "maxWidth": WIDTH, "maxHeight": HEIGHT,
                                                  "everyNthFrame": 1})

    def stop(self):
        if self.stopped:
            return
        self.stopped = True
        self.session.remove_listener("Page.screencastFrame",self.frame)
        try:
            self.session.send("Page.stopScreencast")
        except Exception as exc:
            self.capture_errors.append(f"could not stop screencast: {exc}")

    def guard(self):
        if self.errors:
            raise Refusal(4, f"{self.current}: {self.errors[-1]['kind']}: {self.errors[-1]['message']}")
        if self.capture_errors:
            raise Refusal(5, f"{self.current}: {self.capture_errors[-1]}")


def observe(page, tab, smoke, capture):
    capture.guard()
    ok, detail = smoke._poll_render_check(page, tab)
    result = page.evaluate(OBSERVE_JS)
    result["panel"] = {"ok": ok, "detail": detail}
    capture.guard()
    if result["issues"]:
        raise Refusal(3, f"{tab}: cannot inspect healthy panel: {result['issues']}")
    if not ok:
        raise Refusal(2, f"{tab}: inspected panel was empty: {detail}")
    if result["tab"] != tab:
        raise Refusal(3, f"{tab}: active tab is {result['tab']}")
    return result


def reassert_archived_window(page):
    """--archived: smoke._navigate_to_tab resumes live mode on every tab
    switch that landed paused (the SAME helper the live-demo walk relies on
    to keep the rolling window moving). Live resume re-anchors to the
    server's real wall-clock NOW (app.js initLiveMode: timeRange.anchorLive),
    which for an archived capture is far outside the trace's own data --
    exactly the empty-panel symptom this flag exists to avoid. Re-click the
    "All" quick-range after every navigation to restore the trace's own
    [server.fromNs, server.toNs) span: that path (app.js initTimePicker,
    secs===0) uses the ALREADY-CACHED server.fromNs/toNs from the initial
    connection, never a fresh NOW, so it is stable across repeated clicks."""
    page.locator("#time-range").click()
    page.locator('#time-picker button[data-range="0"]').click()
    page.wait_for_timeout(200)


def navigate(page, tab, smoke, capture, timeout, archived=False):
    capture.current = tab
    try:
        smoke._navigate_to_tab(page, tab, timeout)
    except Exception as exc:
        capture.guard()
        # A mounted empty panel was LOOKED AT; missing mount/browser is unobserved.
        mount = smoke._view_mount(page)
        if mount and mount.get("id") == tab:
            ok, detail = smoke.lib.render_check_ok(page.evaluate(smoke.PANEL_CHECKS[tab]))
            if not ok:
                raise Refusal(2, f"{tab}: inspected mounted panel was empty: {detail}") from exc
        raise Refusal(3, f"{tab}: never became ready: {exc}") from exc
    if archived:
        reassert_archived_window(page)
    capture.guard()


WAIT_MIX_SKIPPED_ARCHIVED = "wait-mix assertion skipped: archived window (fixed span, nothing advances)"
WAIT_MIX_CHECKED_LIVE = "wait-mix assertion: changed across the dwell"


def check_overview_wait_mix(observations, archived):
    """Overview's visible wait mix changing across the dwell is proof the
    recording is watching a LIVE, moving window, not a frozen screenshot --
    that only makes sense for the live path. --archived pins the window to
    the trace's own fixed [from_ns,to_ns) span (reassert_archived_window),
    so nothing in that span advances; an UNCHANGING mix is then the correct,
    expected state, not evidence of a stale connection. Demanding a live
    walk's behavior from an archived replay would refuse every archived walk
    unconditionally, so skip the assertion rather than fake movement. This
    does NOT make an archived walk succeed overall: the SAME stillness still
    trips chart_change()'s AAS-chart motion gate a few steps later (no data
    means no visible pixel change either), and that gate is deliberately
    left refusing -- --archived is for inspecting a retained capture, never
    for producing a GIF of one.

    Returns a short string recording what happened (skipped vs checked),
    stored on the Overview tab's result so a SKIPPED assertion never reads
    like a passed one in walk.json/analysis.md -- a skipped check that looks
    like a checked one is a defect class this project has hit repeatedly,
    including inside its own verification code."""
    def mix(observation):
        return [(r[0], r[2]) for r in observation["table_rows"] if len(r) > 2
                and r[0] in ("CPU (running)", "CPU (waiting for a core)", "CPU*", "IO", "Lock", "LWLock")]
    if archived:
        return WAIT_MIX_SKIPPED_ARCHIVED
    if not any(mix(o) != mix(observations[0]) for o in observations[1:]):
        raise Refusal(2, "Overview: inspected wait mix did not change during the recorded stretch")
    return WAIT_MIX_CHECKED_LIVE


def dwell(page, tab, seconds, smoke, capture, inspect_path=None):
    observations = [observe(page, tab, smoke, capture)]
    start = page.evaluate("Date.now()/1000")
    deadline = time.monotonic()+seconds
    began_dwell = time.monotonic()
    next_sample = time.monotonic()+1
    while time.monotonic() < deadline:
        began = time.monotonic()
        if inspect_path:
            x0, x1, y = inspect_path
            progress = (began-began_dwell)/seconds
            page.mouse.move(x0+(x1-x0)*progress, y)
        capture.pulse()
        # Capture above the required minimum to cover DOM inspection overhead;
        # the frame count and gap gates still use the requested fps unchanged.
        page.wait_for_timeout(max(1, min(deadline-time.monotonic(), 1/(capture.fps*2)-(time.monotonic()-began))*1000))
        capture.guard()
        if time.monotonic() >= next_sample:
            observations.append(observe(page, tab, smoke, capture))
            next_sample = time.monotonic()+1
    end = page.evaluate("Date.now()/1000")
    observations.append(observe(page, tab, smoke, capture))
    checked = coverage(capture.frames, start, end, capture.fps)
    return {"tab": tab, "dwell": {"start_s": start, "end_s": end, "requested_s": seconds},
            "inspection_path": inspect_path,
            "observations": observations, "console_errors": [], **checked}


def _frame_delta(a, b):
    """The SAME pixel-change metric motion_gate() uses internally (not
    reimplemented differently): fraction of pixels whose luminance delta is
    >= MOTION_LEVEL between two already-composed/cropped RGB canvases."""
    hist = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L").histogram()
    return sum(hist[MOTION_LEVEL:]) / (a.width * a.height)


def best_headline_window(staging, frames, pool_indices, fps, crop, width, window_s):
    """Scan every real-frame-anchored [start, start+window_s] span inside
    `pool_indices` (the Overview tab's full recorded dwell, not just a
    fixed early slice of it) for the one that best satisfies motion_gate()
    -- at the exact crop/fps/width the real headline GIF will use --
    instead of a single fixed offset from the dwell start.

    issue #409 follow-up, #411 follow-up: three live runs refused the
    headline GIF's motion gate at a fixed 0.3-5.3s offset, while
    chart_change() -- a much looser check (best single later frame vs.
    frame 0 >= 1%, not the clip-level checks below) -- had already passed
    on the SAME dwell. That gap means the Overview's AAS crop does move
    enough somewhere in the dwell; a fixed early window just wasn't looking
    at it. This picks the best-positioned window instead of a hardcoded
    one -- the gate itself (motion_gate(), called unchanged by write_gif()
    on whatever window this returns) is not touched or loosened: if even
    the best window in the whole recording still can't clear the floor,
    write_gif() still refuses, honestly, with the best number this function
    could find.

    The score matches motion_gate()'s OWN two required checks exactly
    (first/last AND the best single consecutive step, both required,
    neither an average) -- not just one of them: a window is only as good
    as its WORSE check, so this maximizes min(endpoint, best_step), never
    either alone. An earlier version of this function optimized the
    MINIMUM consecutive-pair delta instead, which matched the OLD
    per-consecutive-pair gate this file no longer uses; left unchanged
    after #411 re-expressed the gate, it could pick a window with a strong
    middle transition but a small end-to-end difference, correctly refused
    by motion_gate() on "first/last" -- observed live, issue #411 follow-up.

    Returns (start_frame, end_frame, score) for the best candidate found,
    or (pool_indices[0], pool_indices[-1], None) if the pool is shorter
    than window_s -- there is nothing to slide across."""
    times = [frames[i]["timestamp_s"] for i in pool_indices]
    if len(pool_indices) < 2 or times[-1] - times[0] < window_s:
        return pool_indices[0], pool_indices[-1], None

    cache = {}

    def canvas_for(i):
        if i not in cache:
            with Image.open(staging / frames[i]["path"]) as im:
                picture = compose(im.convert("RGB"), crop, width)
            canvas = Image.new("RGB", (width, picture.height), (30, 30, 58))
            canvas.paste(picture, (0, 0))
            cache[i] = canvas
        return cache[i]

    best_score, best_start, best_end = None, pool_indices[0], pool_indices[-1]
    for start_i in pool_indices:
        start_t = frames[start_i]["timestamp_s"]
        if start_t + window_s > times[-1] + 1e-9:
            break
        end_candidates = [i for i in pool_indices if frames[i]["timestamp_s"] <= start_t + window_s]
        if not end_candidates or end_candidates[-1] <= start_i:
            continue
        end_i = end_candidates[-1]
        indices = sample_indices(frames, {"start_frame": start_i, "end_frame": end_i, "fps": fps})
        if len(indices) < 2:
            continue
        canvases = [canvas_for(i) for i in indices]
        endpoint = _frame_delta(canvases[0], canvases[-1])
        best_step = max(_frame_delta(a, b) for a, b in zip(canvases, canvases[1:]))
        score = min(endpoint, best_step)
        if best_score is None or score > best_score:
            best_score, best_start, best_end = score, start_i, end_i
    return best_start, best_end, best_score


def verify_compositor(page, session, timeout):
    """Chrome's outer window can clip CDP while innerHeight remains correct.

    Probe real CDP frames, then resize only the outer window by the measured
    difference. Browser viewport and DPR never change.
    """
    dimensions = []
    def probe(event):
        dimensions.append((event["metadata"]["deviceWidth"], event["metadata"]["deviceHeight"]))
        session.send("Page.screencastFrameAck", {"sessionId": event["sessionId"]})
    session.on("Page.screencastFrame", probe)
    try:
        for attempt in range(3):
            dimensions.clear()
            session.send("Page.startScreencast", {"format": "jpeg"})
            deadline = time.monotonic()+timeout
            while not dimensions and time.monotonic() < deadline:
                page.wait_for_timeout(50)
            session.send("Page.stopScreencast")
            if not dimensions:
                raise Refusal(5, "Chrome compositor probe received no screencast frames")
            w, h = dimensions[-1]
            if (w, h) == (WIDTH, HEIGHT):
                return
            window = session.send("Browser.getWindowForTarget")
            bounds = window["bounds"]
            session.send("Browser.setWindowBounds", {"windowId": window["windowId"],
                "bounds": {"width": bounds["width"]+WIDTH-w, "height": bounds["height"]+HEIGHT-h}})
        raise Refusal(5, f"Chrome compositor did not reach pinned geometry: {dimensions[-1]}")
    finally:
        session.remove_listener("Page.screencastFrame", probe)


def table_crop(page, keys, title_selector=None):
    """A horizontal band of visible table columns, with their original pixels."""
    return page.evaluate("""({keys,title}) => {
        const table = document.querySelector('#table-container table');
        const box = table.getBoundingClientRect();
        let top = title ? document.querySelector(title).getBoundingClientRect().top : box.top;
        top = Math.max(0, Math.floor(top));
        const bottom = Math.min(innerHeight, Math.ceil(box.bottom));
        return keys.map(key => {
            const th = table.querySelector('th[data-sort="'+key+'"]') ||
                (key === '_executions' ? [...table.querySelectorAll('th')].find(e=>e.innerText==='Waterfall') : null);
            if (!th) throw new Error('missing table column: '+key);
            const r = th.getBoundingClientRect();
            const widths = {name:340,text:450,ms:130,aas:90,
                p95_exec_ms:100,avg_exec_ms:100,_executions:120,
                pid:110,query_id:260,duration_ms:140};
            const width = Math.min(Math.ceil(r.width), widths[key] || Math.ceil(r.width));
            const left = th.classList.contains('num') ? Math.ceil(r.right)-width : Math.floor(r.left);
            return [Math.max(0,left),top,Math.min(innerWidth-Math.max(0,left),width),bottom-top];
        });
    }""", {"keys": keys, "title": title_selector})


def rect(page, selector):
    """issue #413: a negative width/height used to be CONSTRUCTIBLE here
    (min(WIDTH-x, ...) or min(HEIGHT-y, ...) goes negative once x or y is
    itself past the viewport edge) and only surfaced three steps downstream,
    in compose(), as an opaque "crop outside captured frame" with no element
    name. Refuse HERE instead, naming the element and its position relative
    to the viewport, so a caller that forgot to scroll an off-screen element
    into view first (as drill() now does for #waterfall-readout) fails at
    the point the bad geometry is actually produced."""
    box = page.locator(selector).bounding_box()
    if not box:
        raise Refusal(3, f"could not inspect bounds of {selector}")
    x, y = max(0, math.floor(box["x"])), max(0, math.floor(box["y"]))
    width, height = min(WIDTH-x, math.ceil(box["width"])), min(HEIGHT-y, math.ceil(box["height"]))
    if width <= 0 or height <= 0:
        raise Refusal(3, f"{selector}: bounding box {box} leaves a {width}x{height} crop within "
                      f"the {WIDTH}x{HEIGHT} captured viewport -- element is off-screen "
                      f"(top={box['y']:.0f}, left={box['x']:.0f}); scroll it into view before "
                      "measuring")
    return [x, y, width, height]


def _tooltip_bbox(page):
    """The native ECharts item-hover tooltip's own bounding box, or None if
    not currently shown. Matched by its distinctive 'Lane:' text
    (waterfallTooltipFormatter's own second line, web/static/lib/builders/
    waterfall.js) since the tooltip is not confined to -- or even a
    descendant of -- its chart's own container, so it cannot be found via a
    CSS selector scoped to the chart."""
    return page.evaluate("""() => {
        let found = null;
        document.querySelectorAll('div').forEach(el => {
            const s = getComputedStyle(el);
            if (s.position === 'absolute' && el.innerText && el.innerText.includes('Lane:')) {
                const r = el.getBoundingClientRect();
                found = {top:r.top, left:r.left, right:r.right, bottom:r.bottom};
            }
        });
        return found;
    }""")


def _tooltip_bboxes_at(page, points):
    """Batched sibling of _tooltip_bbox(): measures the tooltip's bounding
    box at EACH of several candidate (x, y) positions in ONE round trip,
    instead of one Playwright page.mouse.move() + capture pulse per
    candidate. Each position is a synthetic 'mousemove' dispatched to the
    chart's own canvas -- ECharts/zrender's handler reads clientX/clientY
    off the event, the same for a synthetic one as a real one, so this
    reproduces the same tooltip the real sweep in dwell() would show
    without moving the real OS-level cursor at all. requestAnimationFrame
    yields one frame between dispatch and measurement so the tooltip DOM
    has settled. Returns a list of bboxes/None, same order as `points`.

    This exists because the per-candidate round trips it replaces are
    exactly what grew a live walk by ~100-150 frames and, measured 3/3,
    tipped encode_video()'s own video-verification timeout over its old
    flat budget (see VERIFY_MS_PER_VIDEO_SECOND's comment) -- removing the
    cause, not raising a different budget to tolerate it."""
    return page.evaluate("""async ([points]) => {
        const el = document.querySelector('#waterfall-chart');
        const target = el.querySelector('canvas') || el;
        const results = [];
        for (const {x, y} of points) {
            target.dispatchEvent(new MouseEvent('mousemove',
                {clientX: x, clientY: y, bubbles: true, cancelable: true}));
            await new Promise(r => requestAnimationFrame(r));
            let box = null;
            document.querySelectorAll('div').forEach(d => {
                const s = getComputedStyle(d);
                if (s.position === 'absolute' && d.innerText && d.innerText.includes('Lane:')) {
                    const r2 = d.getBoundingClientRect();
                    box = {top: r2.top, left: r2.left, right: r2.right, bottom: r2.bottom};
                }
            });
            results.push(box);
        }
        return results;
    }""", [points])


def _rect_contains(outer, inner):
    """outer=[x,y,w,h] (a rect() result); inner={top,left,right,bottom} (a
    _tooltip_bbox() result). True iff inner is fully within the region
    outer's own crop will actually capture."""
    x, y, w, h = outer
    return (inner["left"] >= x and inner["top"] >= y
            and inner["right"] <= x+w and inner["bottom"] <= y+h)


def duration_ms(text):
    match = re.fullmatch(r"\s*([\d.]+)\s*(ms|µs|us|s)\s*", text)
    return float(match[1])*{"ms":1,"µs":0.001,"us":0.001,"s":1000}[match[2]] if match else -1


# drill-down composition fix: pick a query whose EXECUTIONS actually vary, not just the
# single slowest one. The old rule (max p95, tie-break avg) deterministically
# picked this workload's fixed-duration pg_advisory_lock(id) holds (each an
# exact 0.1/0.3/1.0/3.0s shape by design -- tests/live_loop_workload.py), so
# the drilled executions table showed ten identical rows: technically
# honest, visually "nothing varies here" -- the opposite of the point.
VARYING_SPREAD_RATIO = 0.2  # p95 must exceed avg by at least this fraction of avg.


def select_varying_query(candidates):
    """Rank by the ABSOLUTE spread between p95 and average (ms) -- the same
    two numbers already visible in the DOM, never a server-side aggregate --
    among candidates whose spread is at least VARYING_SPREAD_RATIO of their
    own average: a conservative floor that rejects noise-level jitter but
    clears a genuinely variable query easily (measured live: this workload's
    row-lock-wait UPDATE clears it at ~2x). If nothing clears that floor,
    every visible candidate really is fixed-duration right now -- raises
    distinctly rather than silently falling back to "pick the biggest
    number", which is exactly the complaint this replaces. The caller can
    report that refusal as a workload-variety question, not a recorder bug."""
    varying = [r for r in candidates
               if duration_ms(r["avg"] or "") > 0
               and duration_ms(r["p95"] or "") - duration_ms(r["avg"] or "")
                   >= VARYING_SPREAD_RATIO * duration_ms(r["avg"] or "")]
    if not varying:
        raise Refusal(2, "drill: no query with visibly varying execution durations (every "
                      "candidate's p95 is within 20% of its average) -- the workload has "
                      "nothing worth drilling into right now")
    return max(varying, key=lambda r: duration_ms(r["p95"] or "") - duration_ms(r["avg"] or ""))


def aas_inspection_path(page):
    box = page.locator('#aas-chart-container').bounding_box()
    return [box['x']+80, box['x']+box['width']*0.80, box['y']+box['height']*0.70]


def pulsing_wait(page, capture, predicate, timeout_s):
    """Poll `predicate()` while pulsing the capture at dwell()'s own cadence
    (>= 2x the minimum frame-gap rate), instead of a single pulse before a
    blocking wait and then nothing until it resolves.

    issue #409: drill()'s span (unlike per-tab navigate()+dwell(), whose
    waits are never wrapped in a coverage() check -- only dwell()'s OWN
    start/end is) is checked by ONE coverage() call across its entire
    continuous recording, start to end. A live run against a REAL remote
    bridge (not the mock, not localhost) measured a consecutive-pair gap of
    ~0.48s during the Waterfall mount wait -- twice in a row, same
    magnitude -- because that wait (like Playwright's own
    `wait_for_selector`) polls the condition but never asks the capture for
    a fresh compositor frame while it does, so a genuinely network-bound
    wait (real SSH/WS round trip to the bridge, not CDP-local) can sit
    there with no new screencast frame for longer than the 2/fps gap limit.
    The mock cannot reproduce this: it has no such latency to begin with.

    predicate() returning a truthy value ends the wait and that value is
    returned. predicate() is expected to raise on an actually-unrecoverable
    condition (e.g. _view_mount's missing-hook case) rather than return
    falsy for it -- that still propagates immediately, exactly like the
    direct calls this replaces."""
    interval_s = 1.0 / (capture.fps * 2)
    deadline = time.monotonic() + timeout_s
    while True:
        capture.pulse()
        capture.guard()
        result = predicate()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise TimeoutError(f"condition not met within {timeout_s}s")
        page.wait_for_timeout(interval_s * 1000)


def _fresh_mount(smoke, page, tab_id, tick_ts_ms, min_seq):
    """predicate for pulsing_wait: the SAME chokepoint/comparison
    _wait_for_mount_at_or_after uses (reused, not reimplemented -- only the
    polling shell is local here so it can be pulsed), returning the mount
    dict once fresh, else None/falsy so pulsing_wait keeps polling. A
    missing viewMount hook still raises immediately from _view_mount, same
    as before."""
    mount = smoke._view_mount(page)
    return mount if smoke.lib.mount_is_fresh(mount, tab_id, tick_ts_ms, min_seq) else None


def drill(page, smoke, capture, timeout, archived=False):
    capture.current = "drill-setup"
    # Start the supplementary clip without the walk's session filter.
    if page.locator("#breadcrumb .crumb-clear").count():
        page.locator("#breadcrumb .crumb-clear").click()
        navigate(page,"overview",smoke,capture,timeout,archived)
    navigate(page, "queries", smoke, capture, timeout, archived)
    start = page.evaluate("Date.now()/1000")
    before = dwell(page, "queries", 2, smoke, capture, aas_inspection_path(page))
    # Rank actual DOM p95 execution duration, then average, never aggregate DB Time.
    candidates = page.evaluate("""() => {
        const table=document.querySelector('#table-container table');
        const heads=[...table.querySelectorAll('th')];
        const p95=heads.findIndex(e=>e.dataset.sort==='p95_exec_ms');
        const avg=heads.findIndex(e=>e.dataset.sort==='avg_exec_ms');
        return [...table.querySelectorAll('tbody tr.clickable')].map((r,i)=>({
            index:i,p95:r.cells[p95]?.innerText,avg:r.cells[avg]?.innerText,
            cells:[...r.cells].map(e=>e.innerText.trim())}));
    }""")
    if not candidates:
        raise Refusal(2, "drill: inspected Queries had no clickable rows")
    selected = select_varying_query(candidates)
    query_crop = [[rect(page, "#aas-chart-container")],
                  table_crop(page, ["query_id", "text", "_executions", "p95_exec_ms", "avg_exec_ms"])]
    row = page.locator("#table-container tbody tr.clickable").nth(selected["index"])
    row.locator(".query-executions-action").hover()
    capture.pulse()
    at = page.evaluate("Date.now()*1.0")
    row.locator(".query-executions-action").click()
    capture.current = "drill-waterfall"
    selector = smoke._READY_SELECTOR["waterfall"]
    try:
        pulsing_wait(page, capture, lambda: page.locator(selector).count() > 0, timeout)
        pulsing_wait(page, capture, lambda: _fresh_mount(smoke, page, "waterfall", at, None), timeout)
    except Exception as exc:
        capture.guard()
        raise Refusal(3, f"drill: per-execution Waterfall never became ready: {exc}") from exc
    capture.pulse()
    observe(page, "waterfall", smoke, capture)
    landed = page.evaluate("Date.now()/1000")
    selection = page.locator("#executions-table tr.selected-execution")
    if selection.count() != 1:
        raise Refusal(3, "drill: could not identify one selected execution")
    mount = smoke._view_mount(page)
    selected_at = page.evaluate("Date.now()*1.0")
    selection.click()  # Explicit selection is visible in the continuous recording.
    capture.pulse()
    try:
        pulsing_wait(page, capture,
                     lambda: _fresh_mount(smoke, page, "waterfall", selected_at, mount["seq"]), timeout)
    except Exception as exc:
        capture.guard()
        raise Refusal(3, f"drill: execution selection's Waterfall refresh never became ready: {exc}") from exc
    # issue #413: the drilled Waterfall view's own content (executions table +
    # chart + readout) is taller than the pinned 981px viewport at this
    # execution's row count -- #waterfall-readout alone sits ~67px below the
    # fold. Scroll it into view BEFORE computing anything pixel-based below
    # (the wait-bar click point, the crosshair sweep, and execution_crop's
    # rect()s all read CURRENT on-screen position) so every one of them is
    # computed in the SAME final scroll position, rather than some before a
    # scroll and some after -- a mix that would point later video frames at
    # the wrong part of the page. Measured live: this one scroll already
    # brings BOTH #waterfall-chart and #waterfall-readout fully into the
    # viewport simultaneously (scrollY 0 -> 121, the page's one and only
    # overflow amount here), so no second scroll target is needed.
    page.locator("#waterfall-readout").scroll_into_view_if_needed()
    capture.pulse()
    execution_row = selection.locator("td").all_inner_texts()
    # Click an actual drawn non-CPU wait bar, rather than asserting from payload.
    point = page.evaluate("""() => {
        const el=document.querySelector('#waterfall-chart');
        const c=echarts.getInstanceByDom(el), opt=c.getOption();
        const data=opt.series.flatMap(s=>s.data||[]);
        const wait=data.find(d=>Array.isArray(d) && typeof d[3]==='string' && d[3].includes(':'));
        if(!wait) return null;
        const pixel = t => c.convertToPixel({gridIndex:0},[t,wait[2]]);
        const xy=pixel((Number(wait[0])+Number(wait[1]))/2);
        const b=el.getBoundingClientRect();return {x:b.left+xy[0],y:b.top+xy[1],wait:wait[3],
            x0:b.left+pixel(Number(wait[0]))[0],x1:b.left+pixel(Number(wait[1]))[0]};
    }""")
    if not point:
        raise Refusal(2, "drill: inspected execution has no specific drawable wait")
    wait_at = page.evaluate("Date.now()/1000")
    page.mouse.click(point["x"], point["y"])
    readout = page.locator("#waterfall-readout").inner_text().strip()
    if point["wait"] not in readout or "PID" not in readout or "Duration" not in readout:
        raise Refusal(3, f"drill: could not read the selected wait from DOM: {readout}")
    # drill-down composition fix: the GIF's job is "drill into one execution
    # and see what it waited on" -- the lane, its tooltip, and the readout
    # line, as ONE coherent region. The previous crop additionally carried
    # the executions table (needed only to SELECT the row, not to keep
    # watching afterward) and a fixed top margin that, measured live, pulled
    # in that table's own trailing rows and the ".waterfall-banner" text
    # above the chart -- three unrelated things sliced into one frame.
    # web/static/views/waterfall.js's ensureShell() renders banner,
    # truncation notice, chart and readout as ONE contiguous DOM section
    # (<section class="waterfall-pane">); crop THAT, not the individual
    # pieces, so there is one natural boundary instead of hand-picked
    # margins, and the executions table is not in this band at all.
    # rect() already clamps to the captured viewport's own bounds (never
    # wider than the 1710px frame compose() crops from); no separate
    # min(1000, ...) clamp here -- that copied a safeguard from an OLD,
    # narrower readout-only band and, applied to the whole (~1670px-wide)
    # pane, was truncating real content before compose() ever got the
    # chance to resize it down to the 1000px output width. Measured live:
    # that self-inflicted truncation is what made a tooltip "not fit" even
    # though it was fully within the pane's own true extent.
    execution_crop = rect(page, ".waterfall-pane")
    # drill-down composition fix: the published clip is just "here is what
    # this execution waited on" -- the query/execution SELECTION that got us
    # here (the table click) is not part of that story, and keeping it in
    # the clip forced a shared canvas height with the much taller query/
    # executions tables, leaving most of the frame as unused background
    # below the short waterfall pane. Mark the clip's start HERE, right
    # after measuring execution_crop, not earlier (right after the scroll
    # call): a frame captured BEFORE the page's post-scroll layout had
    # genuinely settled -- measured live, this crop applied to a frame that
    # early still showed table rows bleeding in at the top -- would be
    # cropped with geometry that did not describe it yet. Anchoring the
    # clip's own start to the SAME moment the crop was measured means every
    # captured frame in [clip_start_s, end_s) is guaranteed to postdate the
    # layout execution_crop actually describes.
    clip_start_s = page.evaluate("Date.now()/1000")
    # ECharts' item-hover tooltip is not confined to its chart's own bounding
    # box, and measured live it can clip at EITHER edge of the crop
    # depending on where the cursor lands -- left when hovering near the
    # bar's own start, right when hovering near its end (an earlier "sweep
    # the center 40%" fix narrowed this but did not eliminate it: frame 55
    # of 63 in a live run still clipped on the right). Rather than guess a
    # "safe enough" sweep range, measure the tooltip that is ALREADY showing
    # (from the click above, anchored at the bar's own center) and verify it
    # fits the crop -- refuse rather than publish a sliced tooltip if even
    # the center doesn't fit. A center that doesn't fit is a finding of the
    # same family as #415 (the drilled Waterfall's layout not fitting this
    # viewport), not something a crop or sweep choice can paper over.
    tooltip = _tooltip_bbox(page)
    if tooltip and not _rect_contains(execution_crop, tooltip):
        raise Refusal(2, f"drill: the clicked wait's native tooltip ({tooltip}) does not "
                      f"fit within the waterfall pane's own crop ({execution_crop}) even "
                      "centered on the bar")
    # Prefer a sweep across the center 40% of the bar -- real motion, not
    # just the one-time click/tooltip transition, matters for the frame-
    # count and motion gates -- but only where the tooltip is PROVEN to fit
    # at both ends; otherwise hold the cursor at the bar's center (already
    # verified safe above) rather than risk the edge-clipping this replaces.
    # Measured in ONE round trip via _tooltip_bboxes_at() (synthetic events,
    # not real mouse moves) -- the real recorded dwell below is the only
    # place the actual cursor moves. The center is re-dispatched last so the
    # tooltip's own DOM state matches the already-verified real position
    # regardless of which path is chosen next.
    sweep_margin = (point["x1"]-point["x0"]) * 0.3
    lo, hi = point["x0"]+sweep_margin, point["x1"]-sweep_margin
    lo_box, hi_box, _ = _tooltip_bboxes_at(page, [
        {"x": lo, "y": point["y"]}, {"x": hi, "y": point["y"]}, {"x": point["x"], "y": point["y"]}])
    capture.pulse()
    both_fit = all(box is None or _rect_contains(execution_crop, box) for box in (lo_box, hi_box))
    inspect_path = [lo, hi, point["y"]] if both_fit else None
    after = dwell(page, "waterfall", 5, smoke, capture, inspect_path)
    end = page.evaluate("Date.now()/1000")
    checked = coverage(capture.frames, start, end, capture.fps)
    return {"start_s":start,"end_s":end,"landed_s":landed,"clip_start_s":clip_start_s,
            "query_row":selected["cells"], "execution_row":execution_row,
            "actions":[{"action":"query-executions click","timestamp_s":at/1000},
                       {"action":"execution selection","timestamp_s":selected_at/1000},
                       {"action":"wait bar click","timestamp_s":wait_at,"wait":point["wait"]}],
            "wait_readout":readout, "before":before,"after":after, **checked}, query_crop, execution_crop


def encode_video(context, root, frames):
    # A separate page encodes the saved frames; the demo page is never replaced.
    page = context.new_page()
    page.route("**/__demo_encoder__", lambda route: route.fulfill(status=200, content_type="text/html", body="<!doctype html>"))
    page.goto("http://127.0.0.1/__demo_encoder__")
    page.evaluate((ROOT / "scripts/demo-walk-video.js").read_text())
    page.evaluate("([w,h])=>demoVideoStart(w,h)", [WIDTH, HEIGHT])
    origin = frames[0]["timestamp_s"]
    timestamps = []
    for f in frames:
        at = round((f["timestamp_s"]-origin)*1_000_000)
        timestamps.append(at)
        data = base64.b64encode((root/f["path"]).read_bytes()).decode()
        page.evaluate("([b,t,k])=>demoVideoFrame(b,t,k)", [data, at, f["index"] % 20 == 0])
    duration_ms = (frames[-1]["timestamp_s"]-origin)*1000+250
    info = page.evaluate("([w,h,d])=>demoVideoFinish(w,h,d)", [WIDTH, HEIGHT, duration_ms])
    if info["frames"] != len(frames):
        raise Refusal(5, "WebCodecs dropped recorded frames")
    with (root / "walk.webm").open("wb") as output:
        for start in range(0, info["bytes"], 256_000):
            output.write(base64.b64decode(page.evaluate("([s,e])=>demoVideoBytes(s,e)", [start,start+256_000])))
    # Verify Chrome can actually load and decode the encoded file.
    def serve_video(route):
        path = root/"walk.webm"
        total = path.stat().st_size
        match = re.fullmatch(r"bytes=(\d+)-(\d*)",route.request.headers.get("range",""))
        if match:
            start,end = int(match[1]),min(total-1,int(match[2]) if match[2] else total-1)
            with path.open("rb") as source:
                source.seek(start); data=source.read(end-start+1)
            route.fulfill(status=206,content_type="video/webm",body=data,
                          headers={"Accept-Ranges":"bytes","Content-Range":f"bytes {start}-{end}/{total}"})
        else:
            route.fulfill(path=str(path),content_type="video/webm",headers={"Accept-Ranges":"bytes"})
    page.route("**/__demo_video__",serve_video)
    verify_timeout_ms = video_verify_timeout_ms(duration_ms)
    actual = page.evaluate("""async ([timeoutMs]) => {
        const v=document.createElement('video');v.src='/__demo_video__';
        document.body.append(v);
        await new Promise((resolve,reject)=>{
            const timer=setTimeout(()=>reject(new Error('WebM load timed out')),timeoutMs);
            v.onloadeddata=()=>{clearTimeout(timer);resolve();};
            v.onerror=()=>{clearTimeout(timer);reject(new Error('WebM decode failed'));};
        });
        const decoded=[];
        for(const at of [v.duration*0.5,Math.max(0,v.duration-0.5)]) {
            await new Promise((resolve,reject)=>{
                const timer=setTimeout(()=>reject(new Error('WebM seek timed out')),timeoutMs);
                v.onseeked=()=>{clearTimeout(timer);resolve();};v.currentTime=at;
            });
            decoded.push(v.currentTime);
        }
        return {width:v.videoWidth,height:v.videoHeight,duration:v.duration,decoded_seek_times_s:decoded};
    }""", [verify_timeout_ms])
    desired=[duration_ms/2000,max(0,duration_ms/1000-0.5)]
    if ((actual["width"],actual["height"]) != (WIDTH,HEIGHT)
            or abs(actual["duration"]-duration_ms/1000)>0.1
            or any(abs(a-b)>0.1 for a,b in zip(actual["decoded_seek_times_s"],desired))):
        raise Refusal(6, f"video decode verification failed: {actual}")
    page.close()
    return {"path":"walk.webm","bytes":info["bytes"],"codec":"VP8", "frame_timestamps_us":timestamps,
            "origin_timestamp_s":origin,"verified":actual}


def record(args, staging):
    import ui_live_smoke as smoke
    from free_ports import find_free_base
    from playwright.sync_api import sync_playwright
    dwells, runbook = runbook_plan(smoke.lib.TABS, args.dwells)
    mock = None
    capture = None
    try:
        if args.mock:
            port = find_free_base(2)
            log = (staging/"mock.log").open("w")
            mock = subprocess.Popen([sys.executable, str(ROOT/"tests/demo_walk_mock.py"), "--port",str(port)], stdout=log,stderr=log)
            if not smoke._wait_ports(mock,(port,port+1)):
                raise Refusal(3, f"mock server never became ready on {port}/{port+1}")
            url = smoke.mock_app_url(port)
        else:
            url = args.url
        with sync_playwright() as p, ExitStack() as cleanup:
            browser = p.chromium.launch(channel="chrome", headless=not args.headed)
            context = browser.new_context(viewport={"width":WIDTH,"height":HEIGHT}, device_scale_factor=DPR)
            context.add_init_script("window.addEventListener('unhandledrejection', e=>console.error('Unhandled rejection:',String(e.reason)))")
            page = context.new_page()
            page.set_default_timeout(args.timeout*1000)
            session = context.new_cdp_session(page)
            session.send("Page.enable")
            verify_compositor(page, session, args.timeout)
            (staging/"frames").mkdir()
            capture = Capture(page, session, staging, args.fps)
            cleanup.callback(capture.stop)
            page.goto(url)
            page.wait_for_selector("#status.connected",timeout=args.timeout*1000)  # Same connected gate as run_tab.
            geometry = []
            for _ in range(2):
                actual = page.evaluate("({w:innerWidth,h:innerHeight,dpr:devicePixelRatio})")
                reason = smoke.lib.viewport_mismatch_reason(WIDTH,HEIGHT,DPR,actual["w"],actual["h"],actual["dpr"])
                if reason:
                    raise Refusal(3, f"pinned viewport not achieved: {reason}")
                geometry.append(actual); page.wait_for_timeout(200)
            # Default: keep the runbook's 15-minute LIVE range, compare off.
            # --archived (default off): for a capture that is already older
            # than 15 minutes relative to Mac wall-clock, the stock live
            # quick-range shows every panel empty -- the trace's own data
            # ends before that window starts. The "All" quick-range
            # (data-range="0") instead sets the window to the trace's own
            # [server.fromNs, server.toNs) span. This is NOT the live-demo
            # gesture; walk["window"] records which one ran and
            # write_analysis() states the distinction explicitly so a reader
            # never mistakes a full-capture-span recording for a live one.
            page.locator("#time-range").click()
            page.locator(f'#time-picker button[data-range="{0 if args.archived else 900}"]').click()
            if page.evaluate("Boolean(window.__pgwt?.compareSnapshot()?.enabled)"):
                raise Refusal(3,"compare mode must be off")
            window_info = {"mode": "archived-full-span" if args.archived else "live-15min"}
            if args.archived:
                span_ok = page.evaluate(
                    "Math.abs((window.__pgwt.timeRange.to-window.__pgwt.timeRange.from)"
                    " - (window.__pgwt.server.toNs-window.__pgwt.server.fromNs)) < 1024")
                if not span_ok:
                    raise Refusal(3,"full trace-span range was not achieved")
                window_info["from_ns"] = page.evaluate("window.__pgwt.server.fromNs")
                window_info["to_ns"] = page.evaluate("window.__pgwt.server.toNs")
                window_info["span_s"] = (window_info["to_ns"]-window_info["from_ns"])/1e9
            elif page.evaluate("window.__pgwt.timeRange.to-window.__pgwt.timeRange.from") != 900_000_000_000:
                raise Refusal(3,"15-minute range was not achieved")
            window_from_ns = page.evaluate("window.__pgwt.timeRange.from")
            server_from_ns = page.evaluate("window.__pgwt.server.fromNs")
            if not args.allow_young:
                refusal = young_capture_refusal(server_from_ns,window_from_ns)
                if refusal:
                    raise refusal
            capture.pulse()
            walk = {"schema_version":1,"complete":False,"run_id":args.run_id,
                "source":"mock_server.py with changing Overview fixture" if args.mock else "real bridge",
                "url":url,"sha":subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
                "browser_version":browser.version,"geometry_readings":geometry,"window":window_info,
                "capture":{"method":"CDP Page.startScreencast","clock":"UTC epoch seconds; CDP metadata.timestamp and DOM Date.now()/1000",
                           "fps_min":args.fps,"still_frame_policy":"restart screencast on cadence; retain actual timestamps; never pad missing frames",
                           "resolution":[WIDTH,HEIGHT],"device_scale_factor":DPR},
                "runbook":runbook,"tabs":[],"console_errors":capture.errors,"frames":capture.frames}
            headline_crop = None
            for tab in smoke.lib.TABS:
                print(f"Recording {tab}: {dwells[tab]:g}s",flush=True)
                navigate(page,tab,smoke,capture,args.timeout,args.archived)
                if tab == "overview":
                    headline_crop = [[rect(page,"#aas-chart-container")],
                                     table_crop(page,["name","ms","pct","aas"])]
                result = dwell(page,tab,dwells[tab],smoke,capture,
                               aas_inspection_path(page) if tab == 'overview' else None)
                walk["tabs"].append(result)
            print("Recording continuous Queries → execution → wait interaction",flush=True)
            interaction, query_crop, execution_crop = drill(page,smoke,capture,args.timeout,args.archived)
            walk["drill"] = interaction
            capture.stop(); capture.guard()
            overview = walk["tabs"][0]
            overview["wait_mix_check"] = check_overview_wait_mix(overview["observations"], args.archived)
            overview["chart_change"] = chart_change(staging,capture.frames,overview["frame_indices"],headline_crop[0][0])
            # Five seconds of native crosshair inspection, including the live tick --
            # from wherever in the WHOLE recorded Overview dwell actually moves the
            # most (issue #409 follow-up), not a fixed offset from the dwell start.
            first,last,headline_motion = best_headline_window(
                staging,capture.frames,overview["frame_indices"],10,headline_crop,1000,5.0)
            overview["headline_window_scan"] = {"start_frame":first,"end_frame":last,
                "limiting_motion_gate_score":headline_motion}
            # drill-down composition fix: ONE crop for the clip's ENTIRE span
            # (from clip_start_s, where the page is already scrolled to its
            # final position -- see drill()'s own comment) instead of the
            # pre-landing query_crop plus a crop_keyframes switch to
            # execution_crop. One crop means one shared canvas height that
            # actually matches this clip's own content, not the much taller
            # query/executions tables the clip no longer shows.
            drill_indices=frame_indices(capture.frames,interaction["clip_start_s"],interaction["end_s"])
            walk["gifs"]=[
                {"name":"headline","path":"headline.gif","start_frame":first,"end_frame":last,
                 "fps":10,"width":1000,"crop":headline_crop},
                {"name":"drill-down","path":"drill-down.gif","start_frame":drill_indices[0],
                 "end_frame":drill_indices[-1],"fps":10,"width":1000,"crop":execution_crop},
            ]
            print(f"Encoding {len(capture.frames)} timestamped frames in Chrome",flush=True)
            try:
                walk["video"]=encode_video(context,staging,capture.frames)
            except Refusal:
                raise
            except Exception as exc:
                raise Refusal(6,f"Chrome video encoding/decoding failed: {exc}") from exc
            for definition in walk["gifs"]:
                write_gif(staging,capture.frames,definition)
            capture.guard()
            walk["complete"]=True
            save_walk(staging,walk); write_analysis(staging,walk)
            context.close(); browser.close()
            return walk
    except Refusal as exc:
        exc.errors = capture.errors if capture else []
        raise
    except Exception as exc:
        if capture and capture.errors:
            failure = Refusal(4,f"{capture.current}: {capture.errors[-1]['message']}")
        elif isinstance(exc,(OSError,ValueError,KeyError,TypeError)):
            failure = Refusal(6,f"artifact/configuration failure: {exc}")
        else:
            failure = Refusal(3,f"could not inspect/record: {type(exc).__name__}: {exc}")
        failure.errors = capture.errors if capture else []
        raise failure from exc
    finally:
        if mock:
            smoke.stop_mock_server(mock)
            log.close()


def recut(path):
    root = path.resolve().parent
    walk=json.loads(path.read_text())
    if not walk.get("complete") or len(walk.get("gifs",[])) != 2:
        raise Refusal(6,"re-cut needs a complete walk with exactly two GIF definitions")
    with tempfile.TemporaryDirectory(prefix=".recut-",dir=root) as temp:
        staging=Path(temp)
        (staging/"frames").symlink_to(root/"frames",target_is_directory=True)
        for definition in walk["gifs"]:
            write_gif(staging,walk["frames"],definition)
        for definition in walk["gifs"]:
            os.replace(staging/definition["path"],root/definition["path"])
        save_walk(root,walk)
    return walk,root


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    mode=parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mock",action="store_true")
    mode.add_argument("--url",help="real bridge UI URL, including optional ws query parameter")
    mode.add_argument("--recut",type=Path,help="edit walk.json GIF data, then re-cut without Chrome")
    parser.add_argument("--run-id",default=time.strftime("%Y%m%dT%H%M%SZ",time.gmtime())+f"-{os.getpid()}")
    parser.add_argument("--dwells",help="JSON object of all eleven per-tab durations, >=5 seconds each")
    parser.add_argument("--fps",type=int,default=12,help="minimum capture cadence (1–16), default 12; GIFs use 10")
    parser.add_argument("--timeout",type=float,default=60,help="readiness timeout in seconds")
    parser.add_argument("--headed",action="store_true",help="show the dedicated Chrome window")
    parser.add_argument("--archived",action="store_true",
                         help="window = the served trace's own [from_ns,to_ns) span (data-range=\"0\", "
                              "\"All\"), not the live 15-minute quick-range. For a capture that is "
                              "already older than 15 minutes relative to wall-clock, where the stock "
                              "live gesture would show every panel empty. Default off: unchanged live "
                              "path. LIMITATION: this is for INSPECTING a retained capture, not for "
                              "producing GIFs -- the window is the trace's own fixed span, so nothing "
                              "in it advances, so the Overview AAS chart cannot visibly change, so the "
                              "walk will always refuse at that motion gate before ever cutting a GIF. "
                              "Record real demo GIFs against a LIVE capture (no --archived).")
    parser.add_argument("--allow-young",action="store_true",
                         help="override the young-capture refusal: proceed even when the "
                              "capture's own data starts after the rendered window's start "
                              "(the window's early range will then visibly have no data)")
    args=parser.parse_args()
    staging=None
    try:
        if args.recut:
            walk,output=recut(args.recut)
        else:
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*",args.run_id) or not (1<=args.fps<=16) or not (0<args.timeout<=300):
                raise Refusal(6,"invalid run-id, fps or timeout")
            if args.archived and not args.url:
                raise Refusal(6,"--archived only makes sense against a real bridge (--url); "
                                "--mock already serves data anchored to wall-clock \"now\"")
            RESULTS.mkdir(parents=True,exist_ok=True)
            output=RESULTS/args.run_id
            if output.exists():
                raise Refusal(6,f"refusing to overwrite existing run: {output}")
            staging=Path(tempfile.mkdtemp(prefix=f".{args.run_id}-",dir=RESULTS))
            walk=record(args,staging)
            os.rename(staging,output); staging=None
        print(f"Complete: {output}")
        for definition in walk["gifs"]:
            print(f"{definition['path']}: {definition['bytes']} bytes, frames {definition['start_frame']}–{definition['end_frame']}")
        return 0
    except Refusal as exc:
        print(f"REFUSED ({exc.code}): {exc}",file=sys.stderr)
        if staging:
            refused=RESULTS/f"{args.run_id}.refused"
            (staging/"refusal.json").write_text(json.dumps({"complete":False,"exit_code":exc.code,
                "reason":str(exc),"console_errors":getattr(exc,"errors",[])},indent=2)+"\n")
            os.rename(staging,refused); staging=None
        return exc.code
    except (ValueError,OSError,KeyError,TypeError) as exc:
        print(f"REFUSED (6): artifact/configuration failure: {exc}",file=sys.stderr)
        return 6
    finally:
        if staging:
            shutil.rmtree(staging)


if __name__ == "__main__":
    sys.exit(main())
