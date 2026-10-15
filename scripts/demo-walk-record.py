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
                                  MOTION_FLOOR, MOTION_LEVEL, sample_indices, save_walk,
                                  write_analysis, write_gif)
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


# GIF composition (issue "the deliverable GIFs are not postable"). These are
# SELECTION numbers -- which window of a recording gets published -- never
# gate thresholds: demo_walk_record_lib's MOTION_FLOOR / DECODED_MIN_FPS /
# GIF_MIN_SECONDS are untouched, and every clip chosen here is still handed
# to the same unchanged write_gif()/motion_gate() to pass or refuse on its
# own merits.
#
# LIVE_WINDOW_S is the runbook's live quick-range (the 900s button clicked in
# record()), repeated here as a number so the coverage check below can say
# what fraction of the chart would be blank.
LIVE_WINDOW_S = 900.0
# A 15-minute chart of a capture that has only been running nine minutes is
# ~40% blank on its left half -- measured on the pre-tag recording at
# 304a772, which was numerically correct and still unpostable for exactly
# this reason. CAPTURE_COVERAGE_SLACK_S tolerates only the handful of
# seconds between the server's own from_ns and the window edge that a
# just-long-enough capture legitimately shows (clock granularity, the
# quick-range click landing a moment after the window was computed), not a
# visibly empty chart.
CAPTURE_COVERAGE_SLACK_S = 5.0
# The live UI ticks every 5s (app.js startAutoRefresh). A 5s headline clip
# therefore contains exactly ONE tick: 47 of 48 frame pairs identical and a
# single step, which is what made the pre-tag headline read as a still
# image. 20s is four tick intervals, so the published clip shows the chart
# advancing repeatedly rather than once. Not 15: measured on the retained
# pre-tag footage, only ONE tick in its whole 9.97s Overview dwell moved the
# headline crop by >= MOTION_FLOOR (a 9s window scores 1 update, same as a
# 5s one), i.e. about one visible update per ~10s rather than per 5s. A 15s
# window would be sized for the cadence the code hopes for; 20s is sized for
# the one the footage actually shows.
HEADLINE_WINDOW_S = 20.0
# How many SEPARATE visible updates the chosen window must contain (see
# count_visible_updates: runs of above-floor pairs, so one tick's redraw
# straddling two sampled frames still counts once). Two is the smallest
# number that cannot be satisfied by the single-step clip this replaces.
HEADLINE_MIN_UPDATES = 2
# Room for best_headline_window to slide the window within the dwell instead
# of being pinned to its start: two further tick intervals of candidate
# positions, so "the best-positioned window" is a real choice.
HEADLINE_SLIDE_S = 10.0
DEFAULT_DWELL_S = 10.0
OVERVIEW_DWELL_S = HEADLINE_WINDOW_S + HEADLINE_SLIDE_S


def _as_number(value):
    """A JS-evaluated numeric reading, or None if it is not a finite number.
    Strings are accepted because the bridge carries ns as strings in places
    (BigInt-safety); bools are not numbers here, and None/absent stays None
    so a MISSING reading is never silently read as 0."""
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value) if math.isfinite(value) else None
    if isinstance(value, str):
        try:
            number = float(value.strip())
        except ValueError:
            return None
        return number if math.isfinite(number) else None
    return None


def window_capture_coverage(reading, window_s=LIVE_WINDOW_S):
    """Refuse a headline recording whose time window reaches back to before
    the capture existed -- the "~45% empty chart" half of the composition
    problem. The window is the runbook's rolling 15 minutes; if the daemon
    has only been capturing for nine of them, the chart's left half is blank
    and no clip selection can fix that, because the missing pixels are
    missing data. Waiting is the fix, so this refuses with the number of
    seconds left to wait.

    Both bounds are read from the page: `window_from_ns` is the client's own
    window start and `capture_from_ns` is the SERVER-declared earliest
    timestamp in the trace (lib/state.js ServerInfo.fromNs, fed by the
    backend's `info`) -- deliberately not two numbers derived from the same
    client-side clock, which would agree with each other no matter how
    little data existed. A missing, non-numeric or zero capture bound means
    this check cannot see the capture's extent at all: it refuses (code 3)
    rather than approving, because ServerInfo's own pre-handshake default is
    0 and treating that as "capture began at the epoch" would pass every
    window ever."""
    reading = reading or {}
    window_from = _as_number(reading.get("window_from_ns"))
    capture_from = _as_number(reading.get("capture_from_ns"))
    if window_from is None or capture_from is None or capture_from <= 0:
        raise Refusal(3, "could not read the window/capture bounds needed to tell "
                      "whether the headline chart is full: window_from_ns="
                      f"{reading.get('window_from_ns')!r}, capture_from_ns="
                      f"{reading.get('capture_from_ns')!r}")
    empty_lead_s = (capture_from - window_from) / 1e9
    if empty_lead_s > CAPTURE_COVERAGE_SLACK_S:
        raise Refusal(2, f"the {window_s:.0f}s headline window starts {empty_lead_s:.0f}s "
                      f"before the capture began, so {empty_lead_s/window_s:.0%} of the "
                      "chart would be blank. Let the capture run for at least "
                      f"{window_s:.0f}s (another {empty_lead_s:.0f}s) and record again.")
    return {"window_s": window_s, "window_from_ns": window_from,
            "capture_from_ns": capture_from, "empty_lead_s": max(0.0, empty_lead_s),
            "slack_s": CAPTURE_COVERAGE_SLACK_S}


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
    dwells = {tab: DEFAULT_DWELL_S for tab in tabs}
    # The Overview dwell is the material the headline GIF is cut from, so it
    # is the one dwell that cannot be the generic default: a 10s dwell can
    # only hold a 5s-ish clip, i.e. one live tick, which is exactly the
    # single-step headline this replaces.
    dwells["overview"] = OVERVIEW_DWELL_S
    if overrides:
        supplied = json.loads(Path(overrides).read_text())
        if set(supplied) != set(tabs):
            raise Refusal(6, "--dwells must specify exactly all eleven tabs")
        dwells = {k: float(v) for k, v in supplied.items()}
    if any(not math.isfinite(v) or v < 5 for v in dwells.values()):
        raise Refusal(6, "every dwell must be finite and at least five seconds")
    # Checked for the supplied AND default case alike, before a browser
    # opens: an Overview dwell too short to contain the headline window
    # would otherwise be discovered eleven dwells later, as a window scan
    # with nothing to scan.
    needed = HEADLINE_WINDOW_S + HEADLINE_SLIDE_S
    if dwells.get("overview", 0.0) < needed:
        raise Refusal(6, f"overview dwell must be at least {needed:g}s so the headline clip can "
                      f"span {HEADLINE_WINDOW_S:g}s of live ticks with room to position it; "
                      f"got {dwells.get('overview', 0.0):g}s")
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


def count_visible_updates(deltas, floor=MOTION_FLOOR):
    """How many SEPARATE visible updates a clip contains: maximal RUNS of
    consecutive-pair deltas at or above `floor`, not the number of pairs
    above it.

    The distinction is the whole point. A live tick repaints the chart
    between two sampled frames, but a repaint that lands mid-sample shows up
    as TWO adjacent above-floor pairs for ONE update; counting pairs would
    let the single-step clip this change exists to replace report "2 updates"
    and sail through a "several ticks" requirement. Counting runs reports it
    as the one update it is (test_one_tick_straddling_two_frames_counts_once).

    `floor` defaults to the gate's own MOTION_FLOOR -- read, never modified --
    so "visible" here means the same thing it means to motion_gate()."""
    updates, inside = 0, False
    for delta in deltas:
        if delta >= floor:
            if not inside:
                updates += 1
            inside = True
        else:
            inside = False
    return updates


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

    Composition fix on top of that: among windows, MORE SEPARATE VISIBLE
    UPDATES wins first (count_visible_updates), and only then the
    motion_gate score above. A 5s window can only ever contain one live
    tick, so the published headline was 47 identical frame pairs and one
    step -- correct data, unwatchable clip. Ranking updates first picks the
    stretch of the dwell where the chart moved repeatedly, which is what
    "the chart is alive" looks like; the gate's own thresholds are still
    whatever they were, and the tie-break is still min(endpoint, best_step)
    so a many-updates window with a weak end-to-end difference cannot win
    over a strictly better one.

    Returns a dict describing the chosen window. `visible_updates` and
    `limiting_motion_gate_score` are None when the pool is shorter than
    window_s -- there is nothing to slide across and nothing was measured,
    which the caller treats as a refusal rather than as a zero."""
    times = [frames[i]["timestamp_s"] for i in pool_indices]
    if len(pool_indices) < 2 or times[-1] - times[0] < window_s:
        return {"start_frame": pool_indices[0], "end_frame": pool_indices[-1],
                "limiting_motion_gate_score": None, "visible_updates": None,
                "window_s": window_s, "scanned_candidates": 0,
                "pool_span_s": times[-1] - times[0] if times else 0.0}

    cache, deltas_cache = {}, {}

    def delta_for(a, b):
        # Candidate windows overlap heavily (one new start frame at a time),
        # so without this the same adjacent pair is re-differenced once per
        # candidate: ~120 candidates x ~200 pairs for a 20s window inside a
        # 30s dwell, against only a few hundred DISTINCT pairs.
        if (a, b) not in deltas_cache:
            deltas_cache[(a, b)] = _frame_delta(canvas_for(a), canvas_for(b))
        return deltas_cache[(a, b)]

    def canvas_for(i):
        if i not in cache:
            with Image.open(staging / frames[i]["path"]) as im:
                picture = compose(im.convert("RGB"), crop, width)
            canvas = Image.new("RGB", (width, picture.height), (30, 30, 58))
            canvas.paste(picture, (0, 0))
            cache[i] = canvas
        return cache[i]

    best, scanned = None, 0
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
        endpoint = delta_for(indices[0], indices[-1])
        deltas = [delta_for(a, b) for a, b in zip(indices, indices[1:])]
        score = min(endpoint, max(deltas))
        updates = count_visible_updates(deltas)
        scanned += 1
        # Rank: a window the gate would ACCEPT beats one it would refuse,
        # then more separate visible updates, then the gate score itself.
        # The gate leg comes first deliberately -- ranking updates above it
        # could pick a window with two mid-clip steps that returns to its
        # starting state (first/last ~0), which motion_gate() then refuses
        # while a perfectly publishable window existed in the same dwell.
        # That is the #411-follow-up mistake (optimising a metric the gate
        # does not use) in a new costume, so it is ruled out here.
        key = (score >= MOTION_FLOOR, updates, score)
        if best is None or key > best["_rank"]:
            best = {"start_frame": start_i, "end_frame": end_i, "_rank": key,
                    "limiting_motion_gate_score": score, "visible_updates": updates,
                    "first_last_changed_fraction": endpoint,
                    "best_consecutive_changed_fraction": max(deltas),
                    "clears_motion_floor": key[0]}
    if best is None:
        return {"start_frame": pool_indices[0], "end_frame": pool_indices[-1],
                "limiting_motion_gate_score": None, "visible_updates": None,
                "window_s": window_s, "scanned_candidates": 0,
                "pool_span_s": times[-1] - times[0]}
    best.pop("_rank")
    best.update(window_s=window_s, scanned_candidates=scanned,
                pool_span_s=times[-1] - times[0])
    return best


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


def rank_varying_queries(candidates):
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
    return sorted(varying, key=lambda r: duration_ms(r["p95"] or "") - duration_ms(r["avg"] or ""),
                  reverse=True)


def select_varying_query(candidates):
    """The best-spread query: rank_varying_queries()' own first choice."""
    return rank_varying_queries(candidates)[0]


# drill-down composition fix, second half: the published clip was ONE flat
# bar -- a single 1.0s Lock:transactionid in a single lane, spanning the
# whole width. Honest, and a demonstration of nothing. The Queries-level
# select_varying_query() above picks a query whose DURATIONS vary; this picks
# an EXECUTION of it whose WAITS vary, by the same discipline: rank on
# numbers already visible in the DOM (the executions table's own "Leader
# events" and "Workers" columns), then VERIFY against what the chart
# actually drew, and refuse rather than fall back to "whatever was selected
# by default".
DRILL_MIN_WAIT_BARS = 2
# How many ranked executions to actually open before giving up. The table's
# event/worker counts are a promise about the drawn chart, not the chart
# itself (events can be CPU-only, and a worker lane can carry no wait at
# all), so a candidate can rank well and still draw poorly -- try the next
# one instead of refusing on the first disappointment. Bounded, because each
# attempt is a real click, a real server round trip and real recorded
# footage.
DRILL_EXECUTION_ATTEMPTS = 3


def execution_row_key(row):
    """Identity of an executions-table row as the DOM shows it: its start
    time and leader PID cells. Indices are not identity -- the table is
    re-rendered on every live tick and a new execution can appear above the
    one just ranked, so a click by index can land on a different row than
    the one scored (and did, for the by-index selection this replaces)."""
    cells = row.get("cells") or []
    return (cells[0] if len(cells) > 0 else None, cells[1] if len(cells) > 1 else None)


def rank_rich_executions(rows):
    """Executions worth drilling into, best first: more worker lanes beats
    more leader events, because a second lane is the thing a single-lane
    flat bar cannot show at all.

    The floor is deliberately low and structural -- at least two leader
    events, or at least one worker -- because this ranks the TABLE's
    promise; the real decision is check_drilled_richness() on the drawn
    chart. A row whose counts are unreadable (em dash, blank, a renamed
    column) scores as nothing rather than as zero-and-still-eligible: it is
    dropped from the ranking, never silently ranked first by a 0 that was
    actually a missing field."""
    scored = []
    for row in rows:
        events = _as_number(row.get("n_events"))
        workers = _as_number(row.get("n_workers"))
        if events is None or workers is None:
            continue
        if events < DRILL_MIN_WAIT_BARS and workers < 1:
            continue
        scored.append((workers, events, row))
    scored.sort(key=lambda t: (t[0], t[1]), reverse=True)
    return [row for _, _, row in scored]


def waterfall_richness(bars):
    """What the drilled waterfall chart actually DREW, from its own ECharts
    series data. A "wait" is a named event whose name carries a ':' -- the
    same test drill() already uses to find a clickable wait bar, so this
    counts exactly the bars a viewer can click, not payload rows."""
    tuples = [b for b in (bars or []) if isinstance(b, list) and len(b) > 3]
    waits = [b for b in tuples if isinstance(b[3], str) and ":" in b[3]]
    return {"bars": len(tuples), "wait_bars": len(waits),
            "wait_lanes": len({b[2] for b in waits}),
            "wait_names": sorted({b[3] for b in waits}),
            "lanes": len({b[2] for b in tuples})}


def check_drilled_richness(richness):
    """None when this drilled execution is worth publishing, else the reason
    it is not. "Several distinct waits across more than one lane": at least
    DRILL_MIN_WAIT_BARS drawn wait bars, and more than one DISTINCT thing
    among them -- more than one lane (a parallel execution's workers) or
    more than one wait class. One 1.0s bar in one lane fails both, which is
    the clip this exists to stop publishing."""
    if richness["wait_bars"] < DRILL_MIN_WAIT_BARS:
        return (f"{richness['wait_bars']} drawn wait bar(s), need "
                f"{DRILL_MIN_WAIT_BARS}")
    if richness["wait_lanes"] < 2 and len(richness["wait_names"]) < 2:
        return (f"{richness['wait_bars']} wait bars but all of one class "
                f"({richness['wait_names']}) in one lane")
    return None


def check_drilled_view_fits(reading):
    """#415/#413: the drilled Waterfall's readout line must be ON SCREEN at
    the pinned viewport, with the page not scrolled.

    The recorder used to reach it with an instant scroll jump at the moment
    of the wait-bar click -- a jump in the published clip, and a crop
    measured in a scroll position the earlier frames were not in. The UI
    now sizes the executions list so the pane fits
    (executionListMaxHeight, web/static/lib/builders/waterfall.js), so this
    is a CHECK, not a workaround: if the layout ever stops fitting, the
    recording refuses with the overflow in pixels instead of quietly
    scrolling and publishing a jump.

    Refuses when a reading is missing or non-numeric: an unmeasurable
    layout is not a passing one."""
    reading = reading or {}
    values = {key: _as_number(reading.get(key)) for key in
              ("readout_bottom", "viewport_height", "scroll_y", "document_height")}
    missing = sorted(k for k, v in values.items() if v is None)
    if missing:
        raise Refusal(3, "could not measure whether the drilled Waterfall readout is "
                      f"on screen; unreadable: {', '.join(missing)} (got {reading!r})")
    if values["viewport_height"] <= 0:
        raise Refusal(3, f"nonsensical viewport height {values['viewport_height']}")
    overflow = values["readout_bottom"] - values["viewport_height"]
    if overflow > 0:
        raise Refusal(2, f"#415: the drilled Waterfall readout's bottom sits {overflow:.0f}px "
                      f"below the {values['viewport_height']:.0f}px fold -- the published clip "
                      "would need a scroll jump to show what the clicked bar was")
    if values["scroll_y"] != 0:
        raise Refusal(2, f"the drilled view is scrolled to y={values['scroll_y']:.0f}; the clip "
                      "must be recorded at the top of the page, not at a scroll offset")
    return {"readout_bottom": values["readout_bottom"],
            "viewport_height": values["viewport_height"],
            "readout_px_above_fold": -overflow,
            "document_height": values["document_height"]}


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


EXECUTIONS_LISTING_JS = """() => {
    const table = document.querySelector('#executions-table table');
    if (!table) return {headers: [], rows: []};
    const headers = [...table.querySelectorAll('th')].map(e => e.innerText.trim());
    const events = headers.indexOf('Leader events'), workers = headers.indexOf('Workers');
    return {headers, rows: [...table.querySelectorAll('tbody tr.clickable')].map((r, i) => ({
        index: i,
        n_events: events < 0 ? null : (r.cells[events]?.innerText ?? '').trim(),
        n_workers: workers < 0 ? null : (r.cells[workers]?.innerText ?? '').trim(),
        cells: [...r.cells].map(e => e.innerText.trim())}))};
}"""

WATERFALL_BARS_JS = """() => {
    const el = document.querySelector('#waterfall-chart');
    const chart = el && echarts.getInstanceByDom(el);
    if (!chart) return null;
    return chart.getOption().series.flatMap(s => s.data || []);
}"""


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
    ranked_queries = rank_varying_queries(candidates)
    selected = ranked_queries[0]
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
    # drill-down composition fix: choose WHICH execution to publish instead
    # of keeping whichever one the view selected by default (which produced
    # the single flat 1.0s bar). Ranked on the table's own visible columns,
    # then verified against the chart the click actually drew -- see
    # rank_rich_executions()/check_drilled_richness().
    listing = page.evaluate(EXECUTIONS_LISTING_JS)
    for column in ("Leader events", "Workers"):
        if column not in (listing.get("headers") or []):
            raise Refusal(3, f"drill: the executions table has no '{column}' column "
                          f"(headers: {listing.get('headers')}), so this cannot tell which "
                          "execution has several waits -- refusing rather than publishing "
                          "an unexamined one")
    ranked = rank_rich_executions(listing.get("rows") or [])
    attempts, chosen, richness, selection = [], None, None, None
    for candidate in ranked[:DRILL_EXECUTION_ATTEMPTS]:
        key = execution_row_key(candidate)
        # Re-resolve the row by its (start, PID) identity immediately before
        # clicking: a live tick re-renders this table and can insert a newer
        # execution above the ranked one, which would make a click by index
        # land on a different row than the one scored.
        current = (page.evaluate(EXECUTIONS_LISTING_JS).get("rows") or [])
        index = next((row["index"] for row in current
                      if execution_row_key(row) == key), None)
        if index is None:
            attempts.append({"row": list(key), "reason": "row left the table before it "
                             "could be opened"})
            continue
        mount = smoke._view_mount(page)
        selected_at = page.evaluate("Date.now()*1.0")
        # Explicit selection is visible in the continuous recording.
        page.locator("#executions-table tbody tr.clickable").nth(index).click()
        capture.pulse()
        try:
            pulsing_wait(page, capture,
                         lambda: _fresh_mount(smoke, page, "waterfall", selected_at, mount["seq"]), timeout)
        except Exception as exc:
            capture.guard()
            raise Refusal(3, f"drill: execution selection's Waterfall refresh never became ready: {exc}") from exc
        selection = page.locator("#executions-table tr.selected-execution")
        if selection.count() != 1:
            raise Refusal(3, "drill: could not identify one selected execution")
        landed_cells = [text.strip() for text in selection.locator("td").all_inner_texts()]
        if tuple(landed_cells[:2]) != key:
            raise Refusal(3, f"drill: clicked execution {list(key)} but the view selected "
                          f"{landed_cells[:2]}")
        measured = waterfall_richness(page.evaluate(WATERFALL_BARS_JS))
        reason = check_drilled_richness(measured)
        attempts.append({"row": list(key), "reason": reason, **measured})
        if reason is None:
            chosen, richness = candidate, measured
            break
    if chosen is None:
        raise Refusal(2, "drill: no execution of the drilled query shows several distinct "
                      "waits across more than one lane, so the clip would again be one flat "
                      f"bar. Ranked {len(ranked)} candidate(s), opened "
                      f"{len([a for a in attempts if 'wait_bars' in a])}: "
                      + json.dumps(attempts))
    # issue #413/#415: the drilled Waterfall view used to be TALLER than the
    # pinned 981px viewport at a real capture's row count -- #waterfall-readout
    # sat 68px below the fold (measured at 304a772, reproduced in
    # tests/web_unit/waterfall_fit.test.mjs' geometry) -- and the recorder
    # scrolled it into view here, which is both a jump in the published clip
    # and a layout change AFTER earlier frames were captured. The UI now sizes
    # the executions list so the pane fits, and everything pixel-based below
    # (the wait-bar click point, the crosshair sweep, execution_crop's rect())
    # is measured in the one and only scroll position there is: the top.
    # #415/#413: the readout must already BE on screen -- the UI now sizes
    # the executions list so the whole pane fits this viewport
    # (executionListMaxHeight). This verifies it rather than scrolling:
    # the scroll_into_view_if_needed() that used to sit here was an instant
    # jump inside the published clip, and it moved the page AFTER earlier
    # frames had been captured at a different offset.
    fold = check_drilled_view_fits(page.evaluate("""() => {
        const el = document.querySelector('#waterfall-readout');
        const box = el ? el.getBoundingClientRect() : null;
        return {readout_bottom: box ? box.bottom : null, viewport_height: innerHeight,
                scroll_y: scrollY, document_height: document.documentElement.scrollHeight};
    }"""))
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
            "wait_readout":readout, "before":before,"after":after,
            "execution_attempts":attempts, "execution_richness":richness,
            "fold":fold, **checked}, query_crop, execution_crop


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
            if not args.archived:
                # The headline chart must be FULL: refuse now, before eleven
                # dwells of recording, if this window reaches back to before
                # the capture existed.
                window_info["capture_coverage"] = window_capture_coverage(page.evaluate(
                    """() => ({window_from_ns: window.__pgwt?.timeRange?.from,
                               capture_from_ns: window.__pgwt?.server?.fromNs})"""))
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
            # HEADLINE_WINDOW_S of native crosshair inspection spanning SEVERAL
            # live ticks -- from wherever in the WHOLE recorded Overview dwell
            # the chart updated most often (issue #409 follow-up for the
            # position, the composition fix for the length and the ranking),
            # not a fixed offset from the dwell start and not a single tick.
            scan = best_headline_window(
                staging,capture.frames,overview["frame_indices"],10,headline_crop,1000,
                HEADLINE_WINDOW_S)
            overview["headline_window_scan"] = scan
            if scan["visible_updates"] is None:
                raise Refusal(2, f"the recorded Overview dwell ({scan['pool_span_s']:.1f}s of "
                              f"frames) is shorter than the {HEADLINE_WINDOW_S:g}s headline "
                              "window, so no clip spanning several live ticks could be cut")
            if scan["visible_updates"] < HEADLINE_MIN_UPDATES:
                raise Refusal(2, f"the best {HEADLINE_WINDOW_S:g}s window in the recorded "
                              f"Overview dwell contains {scan['visible_updates']} visible "
                              f"update(s), need {HEADLINE_MIN_UPDATES}: the published clip "
                              "would be a still image with one step in it")
            first,last = scan["start_frame"],scan["end_frame"]
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
                # Width = the waterfall pane's OWN captured width, not a fixed
                # 1000: the pane is ~1670px wide, so publishing it at 1000
                # shrank every label and number in it to 60% of the size the
                # browser rendered -- the one thing compose() exists to avoid
                # ("joining DOM-derived column crops without shrinking text").
                # Clamped to the captured frame's width, which rect() already
                # guarantees the crop is inside.
                {"name":"drill-down","path":"drill-down.gif","start_frame":drill_indices[0],
                 "end_frame":drill_indices[-1],"fps":10,
                 "width":min(execution_crop[2],WIDTH),"crop":execution_crop},
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
            (RESULTS/f"{args.run_id}.refusal.json").write_text(json.dumps({"complete":False,"exit_code":exc.code,
                "reason":str(exc),"console_errors":getattr(exc,"errors",[])},indent=2)+"\n")
        return exc.code
    except (ValueError,OSError,KeyError,TypeError) as exc:
        print(f"REFUSED (6): artifact/configuration failure: {exc}",file=sys.stderr)
        return 6
    finally:
        if staging:
            shutil.rmtree(staging)


if __name__ == "__main__":
    sys.exit(main())
