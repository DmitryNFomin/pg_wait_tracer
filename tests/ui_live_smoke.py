#!/usr/bin/env python3
"""ui_live_smoke.py -- issue #93: live UI smoke walk of all 11 tabs.

Proves that every panel works against a REAL daemon with live data (not
tests/mock_server.py's canned replies): rendered, no console errors, no
blinking, stable identity colours, no resource leak. Designed to run on the
gate box inside tests/run_all.sh's live section (via tests/ui_live_smoke.sh),
but has two front-ends so the walk logic itself is testable off the box:

    --mock             starts tests/mock_server.py itself and walks against
                        it -- validates the walk/artifact/verdict machinery
                        on a plain Mac, no daemon, no BPF, no box.
    --url <page URL>    walks against an already-running UI. Phase 2 points
                        this at the real Go bridge (tests/ui_live_smoke.sh).

Per tab (Overview, Events, Sessions, Queries, Histogram, Timeline,
Transitions, Concurrency, Waterfall, Scatter, Matrix), live mode on
(the app's default), at least MIN_TICKS ticks:
  1. rendered    -- panel-specific non-empty check (chart has data / table
                     has rows / graph has nodes; see PANEL_CHECKS).
  2. clean       -- zero console errors / page errors / unhandled rejections
                     for the whole tab visit ('[pgwt]'-prefixed console
                     errors, the app's OWN failure reporting, never fail
                     this by themselves but are recorded and printed, never
                     silently dropped).
  3. no_blink    -- a frame ~100ms after each tick is asserted non-blank
                     (a CONTINUITY teardown-to-blank flash must not pass);
                     the GATING check is the issue #119 offset sweep itself
                     (SWEEP_OFFSETS_MS, 200-2000ms), now anchored on the
                     ViewManager mount chokepoint's own event for this tab
                     (issue #193 round 2) instead of a guess from the tick's
                     AAS-request send -- graded on the WORST of its
                     consecutive-pair diff ratios against
                     ui_live_smoke_lib.BLINK_THRESHOLD (0.1%). A single
                     anchored pair (issue #193's first round) was found by
                     review to be blind to a transient that fell wholly
                     inside or wholly outside its own two-frame window (an
                     injected 400ms overlay measured ratio 0.0 on the pair,
                     1.0 on every sweep offset) -- the sweep's multiple
                     samples across the tick are what actually catch that.
                     If the mount sequence itself advances while the sweep
                     is being captured, the tick is reported NOT MEASURED
                     instead of a fabricated ratio
                     (ui_live_smoke_lib.blink_sweep_gate_verdict), and a tab
                     needs a minimum FRACTION of its ticks actually measured
                     (ui_live_smoke_lib.MIN_MEASURED_FRACTION) to pass at
                     all -- discarding most ticks as unmeasured must not
                     read as "ok, ratio 0.0". The AAS legend keeps each
                     event name's colour stable across ticks.
  4. no_leak     -- chart/uplot/pending-request counts (the chaos suite's
                     probe, test_web_ui_chaos.py's _LEAK_PROBE) are stable
                     across the visit; how long the probe took to settle is
                     recorded too (a slow settle is a latency regression
                     worth a trace even when it does not fail the check).

Five of these tabs (Transitions/Concurrency/Waterfall/Scatter/Matrix)
pause live mode on entry or on any drill (app.js), and the driver resumes
it via #live-btn (see _navigate_to_tab) -- deliberately: the walk grades
the state a real user reaches by pressing Live on one of these "heavy"
views, which the app permits and therefore must render as well as any
other live state.

Fail-safe: if the UI shows no data within FIRST_DATA_TIMEOUT_S (60s), the tab
FAILS loudly -- never a skip (mirrors run_all.sh --require-live). Same for
the controlled workload (pgbench + the lock/sleep sessions,
tests/ui_live_smoke.sh): if --pgbench-pid/--workload-pid are given and either
process is gone at a tab boundary, the whole run FAILS loudly rather than
grading the remaining tabs against an idle system.

Artifacts: tests/results/ui_live/<tab>/tick-N.png, one .webm per tab,
tests/results/ui_live/summary.json.

Usage:
    python3 tests/ui_live_smoke.py --mock
    python3 tests/ui_live_smoke.py --url http://localhost:8384/
"""
import argparse
import asyncio
import json
import os
import socket
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke_lib as lib

PLAYWRIGHT_HINT = "ERROR: pip install playwright && playwright install chromium"
try:
    from playwright.sync_api import (sync_playwright, TimeoutError as PWTimeoutError,
                                     Error as PWError)
except ImportError:
    # issue #221 (tests/test_unit_import_guard.py): a module a LISTED unit test
    # imports must be importable with Playwright unavailable. This used to
    # sys.exit(1) at import time, which made any importer un-unit-testable --
    # tests/demo_rehearsal.py, and through it tests/test_demo_rehearsal_diag.py.
    # Nothing is silently degraded: the single place that opens a browser calls
    # _require_playwright() below and refuses with the same message, and the
    # placeholder exception classes are never raised by anything, so an
    # `except PWTimeoutError` simply cannot match in a build without the
    # package -- where no browser code runs in the first place.
    sync_playwright = None

    class PWTimeoutError(Exception):
        pass

    class PWError(Exception):
        pass


def _require_playwright():
    """Refuse, loudly and at the point of use, when a browser is needed and
    Playwright is not installed. Never a skip: a live walk that cannot start
    is a failure, not a pass."""
    if sync_playwright is None:
        print(PLAYWRIGHT_HINT, file=sys.stderr)
        sys.exit(1)

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "results", "ui_live")
# --mock's own default out-dir, deliberately NOT RESULTS_DIR: a developer
# running `--mock` on their own Mac after pulling real box evidence via
# `make box-check` must never have reset_output_dir() wipe that evidence.
MOCK_RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "results", "ui_live_mock")
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "mock_server.py")
MOCK_PORT = int(os.environ.get("PGWT_UI_LIVE_MOCK_PORT", "18830"))

# issue #252: the CI-tier gating walk (main(), below) now defaults to the
# SAME viewport/DPR demo_rehearsal.py pins (docs/DEMO_REHEARSAL_CRITERIA.md,
# docs/chrome-demo-viewport-2026-09-28.md) -- see run_tab()'s own docstring
# for why the narrower 1280x900 DSF1 default this replaced was the reason
# #252 (a DPR-2-only capture-cost defect) escaped every PR gate and was
# only found by a 40-minute rehearsal. Duplicated here (not imported from
# demo_rehearsal.py) because the dependency runs the other way --
# demo_rehearsal.py imports THIS module as `live_smoke`, so this module
# importing demo_rehearsal.py back would be circular.
DEMO_VIEWPORT_WIDTH = 1710
DEMO_VIEWPORT_HEIGHT = 981
DEMO_DEVICE_SCALE_FACTOR = 2

# Turns unhandled promise rejections into console errors, same idiom as
# test_web_ui.py / test_web_ui_chaos.py, so the console guard catches them.
UNHANDLED_REJECTION_HOOK = """
window.addEventListener('unhandledrejection', e => {
    const r = e.reason;
    console.error('Unhandled rejection: ' +
        ((r && (r.stack || r.message)) || String(r)));
});
"""

# Reused verbatim from tests/test_web_ui_chaos.py's _LEAK_PROBE (the B3
# soak-test acceptance probe): live ECharts instances + uPlot roots + pending
# transport requests.
LEAK_PROBE_JS = """() => {
    let charts = 0;
    if (typeof echarts !== 'undefined') {
        for (const el of document.querySelectorAll('*')) {
            try { if (echarts.getInstanceByDom(el)) charts++; } catch (e) {}
        }
    }
    const uplots = document.querySelectorAll('.uplot').length;
    const t = window.__pgwt && window.__pgwt.transport;
    const pending = t ? Object.keys(t.pending).length : -1;
    return { charts, uplots, pending };
}"""

# Installed once per page, right after the WS connects: counts periodic
# 'aas' requests (the persistent AAS pane refreshes on every live tick
# regardless of which tab is active -- app.js startAutoRefresh, 5s cadence)
# so the driver can wait for real ticks instead of a blind sleep multiple.
TICK_HOOK_JS = """() => {
    window.__uiLiveTicks = [];
    const ws = window.__pgwt.transport.ws;
    const send = ws.send.bind(ws);
    ws.send = data => {
        try {
            const msg = JSON.parse(data);
            if (msg && msg.cmd === 'aas') window.__uiLiveTicks.push(Date.now());
        } catch (e) {}
        return send(data);
    };
}"""

# issue #193: reads window.__pgwt.viewMount() -- the ViewManager mount
# chokepoint's own record of the last successful view.mount() ({id, seq,
# at}, `at` = Date.now() in this SAME browser context at mount time),
# exposed by web/static/app.js from web/static/lib/view-manager.js's
# lastMount. Returns {missing: true} rather than throwing when the hook
# itself is absent (an old/broken app.js), so the Python side can tell
# "nothing mounted yet" (a legitimate wait) apart from "this build has no
# such hook" (a loud failure, never a hang -- see
# _view_mount/_wait_for_mount_at_or_after).
VIEW_MOUNT_JS = """() => {
    const fn = window.__pgwt && window.__pgwt.viewMount;
    if (typeof fn !== 'function') return {missing: true};
    return {missing: false, mount: fn()};
}"""

# Failure-only, read-only state, not a paint measurement. Each field says
# whether it was actually read; an absent hook must not look like empty data.
# Limit cards/requests to 20 and card text to 2000 characters.
FAILURE_SNAPSHOT_TIMEOUT_S = 1.0
FAILURE_SNAPSHOT_JS = """() => {
    const record = read => {
        try {
            const value = read();
            if (value === undefined) throw new Error('instrumentation unavailable');
            return {status: 'recorded', value};
        } catch (e) {
            return {status: 'unavailable', error: String(e)};
        }
    };
    const p = window.__pgwt;
    return {
        status: 'recorded',
        active_tab: record(() => {
            if (!p || !('activeTab' in p)) throw new Error('activeTab hook unavailable');
            return p.activeTab;
        }),
        last_mount: record(() => {
            if (!p || typeof p.viewMount !== 'function')
                throw new Error('viewMount hook unavailable');
            const m = p.viewMount();
            return m == null ? m : {id: m.id, seq: m.seq, at: m.at};
        }),
        pane_errors: record(() => Array.from(document.querySelectorAll('.pane-error'))
            .slice(0, 20).map(el => el.textContent.slice(0, 2000))),
        pending_requests: record(() => {
            if (!p || !p.transport || !p.transport.pending)
                throw new Error('transport.pending unavailable');
            return Object.entries(p.transport.pending).slice(0, 20)
                .map(([id, r]) => ({id, cmd: r.cmd, channel: r.channel}));
        }),
    };
}"""

# AAS legend chip colours keyed by event/class name (active.js renderLegend:
# `.aleg` chips, each with a coloured swatch <span> child).
LEGEND_COLORS_JS = """() => {
    const out = {};
    document.querySelectorAll('#aas-legend .aleg').forEach(el => {
        const name = el.textContent.trim();
        const swatch = el.querySelector('span');
        if (swatch) out[name] = getComputedStyle(swatch).backgroundColor;
    });
    return out;
}"""

# issue #252 self-test: a synthetic overlay (a top horizontal slice of the
# clip rect -- see DEFAULT_INJECT_BLINK_FRAC below, review round 1: a FULL-
# panel overlay proves only that a GROSS blink is caught, 1000x over
# BLINK_THRESHOLD).
# position:fixed + the clip rect's own viewport-relative x/y (the SAME
# coordinate space page.screenshot(clip=...) uses) guarantees the overlay
# is positioned within the region the sweep captures, independent of the
# target element's own positioning context.
#
# DEFAULT_INJECT_BLINK_FRAC: the overlay covers this fraction of the clip
# rect's HEIGHT (full width, top-aligned) -- a slice comfortably above
# BLINK_THRESHOLD (0.001) without stepping over it by three orders of
# magnitude the way a full-panel overlay (frac=1.0) does.
#
# issue #266: the show/hide used to be two setTimeout calls at absolute
# browser-clock times ([mount+700, mount+1100]), chosen so that "the 1000ms
# sample's real capture window" would land inside them. That placement was
# an ASSUMPTION about a quantity nothing measures: a page.screenshot()
# call's raster instant is known only to lie somewhere inside the call, and
# on the gate box the 1000ms frame's call starts at mount+1008..1014 and
# runs 210-344ms -- straddling the 1100ms close with ~90ms of margin. So
# whether the stimulus was on screen when the frame was taken came down to
# capture latency, the self-test went red ~1 run in 3 on identical code
# (three box-check runs on commit 278023b: ratio 0.1998 / 0.0 / 0.1998),
# and a red could not be told apart from a detector that had stopped
# detecting.
#
# Now the stimulus is sequenced against the sweep's own frame loop
# (_BlinkInjector): shown after the preceding frame's capture has RETURNED,
# removed after the target frame's capture has RETURNED. Program order, not
# clock arithmetic, is what keeps the overlay out of the neighbouring
# frames, and each transition reports the two timestamps that bound it
# (before the DOM mutation, and after two requestAnimationFrame hops
# confirm the mutation has been presented) -- so
# ui_live_smoke_lib.inject_stimulus_verdict can PROVE, from measured times
# only, that the overlay covered the target frame's whole capture interval
# and no other frame's. Nothing here assumes where in a capture call the
# raster lands; an unprovable geometry refuses instead of passing.
DEFAULT_INJECT_BLINK_FRAC = 0.2

# Minimum on-screen dwell, measured from the overlay's own first presented
# frame (NOT from whenever the evaluate() call happened to land): --inject-
# blink-ms. The overlay is removed at the later of that deadline and the
# target frame's capture returning, so the configured blink duration is a
# FLOOR on a real, measured on-screen window rather than a wall-clock
# window the frames may or may not have intersected. On the gate box the
# bracket is the binding term (210-344ms of capture inside a ~400-840ms
# bracket); the floor only matters if a capture ever returns faster than
# --inject-blink-ms.
_INJECT_BLINK_SHOW_JS = """(p) => {
    const requestedAtMs = Date.now();
    const el = document.createElement('div');
    el.id = '__pgwt_inject_blink_overlay';
    el.style.cssText =
        `position:fixed;left:${p.x}px;top:${p.y}px;width:${p.width}px;` +
        `height:${p.height}px;background:#ff00ff;z-index:2147483647;`;
    document.body.appendChild(el);
    // Safety net ONLY (never the timing mechanism): if the Python side dies
    // between show and hide, the overlay must not survive into later ticks.
    // Generous on purpose -- if this ever fires first it would shorten the
    // stimulus, and the verdict would then refuse rather than approve.
    setTimeout(() => {
        const e = document.getElementById('__pgwt_inject_blink_overlay');
        if (e) e.remove();
    }, p.safetyMs);
    // Two rAF hops: the first callback runs BEFORE the frame containing
    // this mutation is presented, the second after it -- so the resolved
    // time is one by which the overlay IS on screen.
    return new Promise(resolve => {
        requestAnimationFrame(() => requestAnimationFrame(() => resolve({
            requestedAtMs: requestedAtMs,
            paintedAtMs: Date.now(),
        })));
    });
}"""

_INJECT_BLINK_HIDE_JS = """(p) => {
    const fire = (resolve) => {
        // Read the clock BEFORE the removal: the overlay is still on screen
        // at this instant, because a removal can only be presented later.
        const requestedAtMs = Date.now();
        const el = document.getElementById('__pgwt_inject_blink_overlay');
        const wasPresent = !!el;
        if (el) el.remove();
        requestAnimationFrame(() => requestAnimationFrame(() => resolve({
            requestedAtMs: requestedAtMs,
            clearedAtMs: Date.now(),
            wasPresent: wasPresent,
        })));
    };
    return new Promise(resolve => {
        const remaining = p.minUntilMs - Date.now();
        if (remaining > 0) setTimeout(() => fire(resolve), remaining);
        else fire(resolve);
    });
}"""

# Panel-specific "rendered" checks -- same idiom test_web_ui.py already uses
# (read the ECharts option, not just canvas-exists, so an empty series can't
# pass). Table tabs are checked with plain DOM row counts.
_TABLE_CHECK = """() => {
    const rows = document.querySelectorAll('#table-container table tbody tr');
    if (!rows || rows.length === 0) return {ok:false, detail:'no rows'};
    return {ok:true, detail: rows.length + ' rows'};
}"""

PANEL_CHECKS = {
    "overview": _TABLE_CHECK,
    "events": _TABLE_CHECK,
    "sessions": _TABLE_CHECK,
    "queries": _TABLE_CHECK,
    "histogram": """() => {
        const el = document.getElementById('heatmap-container');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'heatmap') return {ok:false, detail:'not a heatmap: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no data'};
        return {ok:true, detail: s0.data.length + ' cells'};
    }""",
    # The series length alone cannot say WHICH render path drew the tab: above
    # ~2 spans per painted pixel the timeline draws class-share SEGMENTS (one
    # or more per pixel column) instead of one rect per wait (#106), and
    # "N spans" reads identically either way. The banner the user sees carries
    # the density sentence, so report it — box evidence then states which path
    # the live data took, and the item count is labelled for what it is.
    "timeline": """() => {
        const el = document.getElementById('timeline-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'custom') return {ok:false, detail:'not custom: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no spans'};
        const b = document.getElementById('timeline-banner');
        const note = b ? b.textContent.trim() : '';
        // "aggregated at " is the load-bearing substring of the density
        // banner (lib/builders/timeline.js timelineBannerNote says so).
        const aggregated = note.indexOf('aggregated at ') >= 0;
        return {ok:true, detail: s0.data.length +
            (aggregated ? ' segments' : ' spans') +
            (note ? ' | ' + note : '')};
    }""",
    "transitions": """() => {
        const el = document.getElementById('dfg-container');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'graph') return {ok:false, detail:'not a graph: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no nodes'};
        return {ok:true, detail: s0.data.length + ' nodes'};
    }""",
    "concurrency": """() => {
        const el = document.getElementById('concurrency-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'line') return {ok:false, detail:'not line: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no peak data'};
        return {ok:true, detail: s0.data.length + ' buckets'};
    }""",
    "waterfall": """() => {
        const rows = document.querySelectorAll('#executions-table tbody tr');
        if (!rows || rows.length === 0) return {ok:false, detail:'no executions'};
        const el = document.getElementById('waterfall-chart');
        if (!el) return {ok:false, detail:'no chart container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const y = c.getOption().yAxis && c.getOption().yAxis[0];
        if (!y || !y.data || y.data.length === 0) return {ok:false, detail:'no lanes'};
        return {ok:true, detail: rows.length + ' executions, ' + y.data.length + ' lanes'};
    }""",
    "scatter": """() => {
        const el = document.getElementById('scatter-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no points'};
        return {ok:true, detail: s0.data.length + ' points'};
    }""",
    "matrix": """() => {
        const el = document.getElementById('matrix-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'heatmap') return {ok:false, detail:'not a heatmap: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no cells'};
        return {ok:true, detail: s0.data.length + ' cells'};
    }""",
}

# Ready selector waited on right after navigating to a tab, before the
# per-tick render check is trusted (avoids racing the first setOption).
_READY_SELECTOR = {
    "overview": "#table-container table tbody tr",
    "events": "#table-container table tbody tr",
    "sessions": "#table-container table tbody tr",
    "queries": "#table-container table tbody tr",
    "histogram": "#heatmap-container canvas",
    "timeline": "#timeline-chart canvas",
    "transitions": "#dfg-container canvas, #dfg-container svg",
    "concurrency": "#concurrency-chart canvas",
    "waterfall": "#waterfall-chart canvas",
    "scatter": "#scatter-chart canvas",
    "matrix": "#matrix-chart canvas",
}

# The element the no_blink two-frame diff is scoped to. Deliberately just the
# tab's OWN panel, not the full viewport: the persistent AAS pane (shown on
# every tab, see test_web_ui_chaos.py's leak-probe comment) draws its own
# live cursor/axis-pointer machinery that is expected to shimmer a little
# between two frames of the SAME data (docs/VISUAL_CHECKLIST.md STABILITY
# note) -- that is a known, accepted source, not something every OTHER
# panel's blink check should also have to absorb.
_PANEL_ELEMENT_SELECTOR = {
    "overview": "#table-container",
    "events": "#table-container",
    "sessions": "#table-container",
    "queries": "#table-container",
    "histogram": "#heatmap-container",
    "timeline": "#timeline-chart",
    "transitions": "#dfg-container",
    "concurrency": "#concurrency-chart",
    "waterfall": "#waterfall-chart",
    "scatter": "#scatter-chart",
    "matrix": "#matrix-chart",
}

TICK_TIMEOUT_S = 30  # generous multiple of the 5s tick cadence

# issue #193 (round 1): the gating check used to be a single pair of frames
# anchored on a fixed delay after the tick's own AAS-request timestamp
# (BLINK_PAIR_ANCHOR_MS, 500ms -- see git history for the issue #119
# gate-box evidence that picked that value). That anchor was provably wrong
# under real load: app.js's refresh() awaits the summary pane's own round
# trip BEFORE calling ViewManager.refresh() (web/static/app.js), so the
# ACTIVE TAB's data lands later than any guess made from the AAS send time.
#
# issue #193 (round 2, review): round 1's fix kept a SINGLE anchored pair,
# just re-anchored on the ViewManager mount chokepoint's own event instead
# of a fixed delay. Review then PROVED that a single pair -- anchored on
# ANYTHING -- is blind to a transient narrower than its own capture window:
# injecting a 400ms blank overlay into a view (the PR #188 shape) and
# running the live check both ways measured ratio 0.0 on the old fixed pair
# AND 0.0 on the mount-anchored pair, because both compare two frames that
# land wholly inside or wholly outside the transient. The ONLY instrument
# that caught it was the issue #119 offset sweep (SWEEP_OFFSETS_MS) at
# ratio 1.0 on every consecutive pair -- multiple samples spread across the
# tick, not two samples chosen up front, are what actually detect a
# transient of unknown width and unknown position.
#
# The gate is now BUILT ON the sweep instead of a separate pair: the sweep
# itself is anchored on the mount event (round 1's genuine insight -- the
# AAS-send-time anchor undershoots under load, proven by the rehearsal's own
# pair_offsets_ms drifting to 1-10s) via _wait_for_mount_at_or_after, then
# graded on its WORST (max) consecutive-pair ratio
# (ui_live_smoke_lib.blink_sweep_gate_verdict) instead of on one pair.
# SWEEP_OFFSETS_MS (ui_live_smoke_lib.py) is unchanged -- the same 5 values
# issue #119 picked from real gate-box evidence, just measured from the
# mount event now instead of the tick's AAS send.
#
# The sequence is re-checked immediately before the sweep's first capture
# and immediately after its last; if it advanced anywhere in that bracket
# (a second tick's mount landing mid-sweep), the WHOLE sweep straddled a
# real content boundary and the tick is reported NOT MEASURED (never a
# fabricated ratio) -- see ui_live_smoke_lib.blink_sweep_gate_verdict. A tab
# also needs at least ui_live_smoke_lib.MIN_MEASURED_FRACTION of its ticks
# actually measured to pass at all (review SHOULD-FIX: a tab that discarded
# most of its ticks as not-measured must not report ok on the remainder
# alone).


class SmokeFailure(Exception):
    """A tab could not even be checked (fail loudly, never skip)."""


class ConsoleErrorGuard:
    """Same idiom as test_web_ui.py / test_web_ui_chaos.py: collects
    console errors and page errors. '[pgwt]'-prefixed console.error calls
    are the app's OWN failure reporting, not a crash -- they are recorded
    verbatim but do not fail the guard by themselves."""

    def __init__(self, page):
        self.errors = []
        page.on("console", self._on_console)
        page.on("pageerror", self._on_pageerror)

    def _on_console(self, msg):
        if msg.type == "error":
            self.errors.append(f"console: {msg.text}")

    def _on_pageerror(self, err):
        self.errors.append(f"pageerror: {err}")

    def drain(self):
        errors = self.errors
        self.errors = []
        return errors


def _is_pgwt_error(e):
    return e.startswith("console: [pgwt]")


def _failure_snapshot(page):
    """Bound the entire browser read on Python's event loop, even if the
    renderer or driver never replies. sync Page.evaluate has no timeout;
    isolate its private async bridge here and report unavailable if that
    bridge changes. No threads, polling, or calls on the successful path.
    """
    try:
        evaluate = page._impl_obj.evaluate
        sync = page._sync

        async def read():
            return await asyncio.wait_for(
                evaluate(FAILURE_SNAPSHOT_JS), FAILURE_SNAPSHOT_TIMEOUT_S)

        return sync(read())
    except asyncio.TimeoutError:
        return {"status": "unavailable",
                "error": f"snapshot timed out after {FAILURE_SNAPSHOT_TIMEOUT_S}s"}
    except Exception as e:
        return {"status": "unavailable", "error": f"{type(e).__name__}: {e}"}


def _failed_tab_diagnostics(page, guard, console_errors, pgwt_errors,
                            pre_mount_diagnostics):
    snapshot = _failure_snapshot(page)
    # Reading pumps browser events too; drain after it so those errors also
    # survive. Lists include anything already drained during earlier ticks.
    for error in guard.drain():
        (pgwt_errors if _is_pgwt_error(error) else console_errors).append(error)
    return {"console_errors": console_errors,
            "pre_mount_diagnostics": pre_mount_diagnostics or None,
            "failure_snapshot": snapshot}


# ── mock front-end ───────────────────────────────────────────────────────────

def _wait_ports(proc, ports, timeout=10.0):
    deadline = time.time() + timeout
    for p in ports:
        while True:
            if proc.poll() is not None:
                return False
            try:
                with socket.create_connection(("127.0.0.1", p), timeout=0.25):
                    break
            except OSError:
                if time.time() >= deadline:
                    return False
                time.sleep(0.05)
    return True


def start_mock_server(port):
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if not _wait_ports(proc, (port, port + 1)):
        if proc.poll() is None:
            proc.kill()
        out, err = proc.communicate()
        print(f"mock_server failed to start on port {port} "
              f"(WS {port + 1}) — port busy, or the mock crashed:\n"
              f"{out.decode()}\n{err.decode()}",
              file=sys.stderr)
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def mock_app_url(port):
    return f"http://127.0.0.1:{port}/?ws=ws://127.0.0.1:{port + 1}/ws"


# Two independent app.js mechanisms pause live mode as a SIDE EFFECT of
# navigation, and both apply here:
#   - switchTab(): entering transitions/concurrency/waterfall/scatter/matrix
#     ("too heavy for the 5s live loop", views/*.js pausesLive) calls
#     stopAutoRefresh().
#   - drill(): EVERY drill gesture (P4) calls stopAutoRefresh(), including the
#     Sessions-row-click that is the only way to reach Timeline with data.
# Rather than special-case which tabs happen to trigger which mechanism (and
# silently go stale the next time either list changes), the driver checks the
# #live-btn's own state after landing on the tab and resumes live itself when
# it finds it paused -- exactly what a user parked on that tab and wanting
# live ticks would do. This deliberately exercises the "heavy" views under
# the same continuous 5s refresh the other tabs get, which is the regression
# this suite exists to catch, not a workaround for a skip.
_LIVE_ACTIVE_JS = "() => { const b = document.getElementById('live-btn'); " \
                  "return !!b && b.classList.contains('active'); }"


# ── tab navigation ───────────────────────────────────────────────────────────

def _navigate_to_tab(page, tab_id, timeout_s, resume_live=True):
    """Switches to tab_id, driving the same clicks a user would. Timeline has
    no standalone entry point with data (a bare tab click shows the "select a
    session" prompt) -- it is reached by drilling into a session row, exactly
    like tests/test_web_ui.py's test_timeline_tab / test_sessions_table.

    Returns ttfp_ms (issue #245, time-to-first-paint): elapsed milliseconds
    from THIS navigation's own landing click -- the drill-down row click for
    Timeline, the bare tab click for every other tab -- to the ViewManager
    mount chokepoint's first FRESH mount of tab_id at/after that click (the
    same chokepoint _wait_for_mount_at_or_after anchors the blink sweep to).
    For Timeline, nav_start_ms is captured AFTER the Sessions-tab bare click
    has already rendered its own table (page.wait_for_selector above) and
    the clickable row has been located, immediately before row.click() --
    so the timer spans only the drill-down that actually reaches Timeline's
    own data, never the earlier Sessions-tab bare click (issue #242's
    earlier 30s Timeline finding measured exactly that "select a session"
    prompt; issue #245 retracted it and requires this measurement instead).

    Measured and consumed BEFORE the live-resume click below: a paused-live
    tab's #live-btn click triggers a SECOND, LATER mount (a plain refresh),
    which must never be counted as this navigation's own first paint."""
    if tab_id == "timeline":
        page.click(".tab[data-tab='sessions']")
        page.wait_for_selector("#table-container table tbody tr", timeout=timeout_s * 1000)
        row = page.query_selector("#table-container table tbody tr.clickable")
        if row is None:
            raise SmokeFailure("no clickable session row to drill into for Timeline")
        nav_start_ms = page.evaluate("Date.now()")
        row.click()
    else:
        nav_start_ms = page.evaluate("Date.now()")
        page.click(f".tab[data-tab='{tab_id}']")

    try:
        page.wait_for_selector(_READY_SELECTOR[tab_id], timeout=timeout_s * 1000)
    except PWTimeoutError:
        raise SmokeFailure(
            f"panel did not render ({_READY_SELECTOR[tab_id]!r}) within {timeout_s}s")

    mount = _wait_for_mount_at_or_after(page, tab_id, nav_start_ms, timeout_s, min_seq=None)
    ttfp_ms = mount["at"] - nav_start_ms

    if resume_live and not page.evaluate(_LIVE_ACTIVE_JS):
        live_btn = page.query_selector("#live-btn")
        if live_btn is None:
            raise SmokeFailure("no #live-btn to resume live mode")
        live_btn.click()
        page.wait_for_timeout(500)  # let the resumed refresh settle before ticks count
        if not page.evaluate(_LIVE_ACTIVE_JS):
            raise SmokeFailure("clicking #live-btn did not resume live mode")

    return ttfp_ms


def _wait_for_tick(page, target_count, timeout_s):
    try:
        page.wait_for_function(
            "window.__uiLiveTicks && window.__uiLiveTicks.length >= " + str(target_count),
            timeout=timeout_s * 1000)
        return True
    except PWTimeoutError:
        return False


_NO_PANEL_DIMS = {"box_width": None, "box_height": None,
                  "clip_width": None, "clip_height": None}


def _panel_clip_and_dims(page, tab_id):
    """Determines the screenshot clip for THIS panel, right now, plus the
    dims to report alongside it -- (clip_or_None, dims), never raising.
    dims (issue #252 evidence) is {"box_width", "box_height", "clip_width",
    "clip_height"}, CSS px, all None when no box/clip was determined (panel
    missing, detached, or scrolled fully out of view) -- same "cannot
    capture" contract as the frame side (see panel_capture_clip's own
    docstring): a None clip here must never fall back to an unbounded
    capture.

    issue #252 (performance): the offset sweep used to call the equivalent
    of this function once PER FRAME (5x/tick) -- two extra CDP round trips
    (query_selector, bounding_box) that bought nothing, because a panel's
    position/size is stable for the ~2s a single sweep spans; only its ROW
    COUNT changes, and only across ticks, never mid-sweep. #table-container
    (the ViewManager's own stable containerEl -- see
    _ATOMIC_PANEL_SNAPSHOT_JS's own comment) is never replaced mid-tick, so
    computing this ONCE per tick and reusing it for all 5 frames
    (_capture_with_clip below) is exact, not an approximation, and removes
    2 of the 3 round trips from every frame after the first.

    scroll_into_view_if_needed() first, same as Playwright's own element
    screenshot semantics, so the visible slice is deterministic (the
    panel's top edge at/near the viewport top) instead of whatever the page
    happened to be scrolled to already."""
    panel = page.query_selector(_PANEL_ELEMENT_SELECTOR[tab_id])
    if panel is None:
        return None, dict(_NO_PANEL_DIMS)
    try:
        panel.scroll_into_view_if_needed()
        box = panel.bounding_box()
    except PWError:
        return None, dict(_NO_PANEL_DIMS)
    viewport = page.viewport_size
    box_dims = ({"box_width": box["width"], "box_height": box["height"]}
                if box is not None else
                {"box_width": None, "box_height": None})
    # lib.panel_capture_clip returns None for "cannot safely determine a
    # clip" (missing box OR missing viewport) -- treated the same as every
    # other "cannot capture" case here, NEVER a fallback to an unclipped/
    # full-element capture (issue #197 review: that fallback silently
    # reintroduces the exact cost regression this issue's fix removed, with
    # no signal in summary.json -- see panel_capture_clip's own docstring).
    clip = lib.panel_capture_clip(box, viewport)
    if clip is None:
        return None, {**box_dims, "clip_width": None, "clip_height": None}
    dims = {**box_dims, "clip_width": clip["width"], "clip_height": clip["height"]}
    return clip, dims


def _capture_with_clip(page, clip):
    """page.screenshot(clip=clip, scale="css") for an already-computed clip
    rect (a plain {"x","y","width","height"} dict, not an element handle --
    see _panel_clip_and_dims), or None on any Playwright error/missing
    clip. Never raises.

    issue #252 (performance): scale="css" (Playwright >=1.9, present since
    1.60.0 here) rasterises the screenshot at 1x CSS pixels regardless of
    the page's device_scale_factor, instead of the default "device" scale.
    demo_rehearsal.py pins device_scale_factor=2 (DEMO_DEVICE_SCALE_FACTOR),
    so the default was encoding/decoding ~4x more pixels than the CSS-px
    region being diffed actually needed -- measured as the dominant cost on
    a text-dense table panel (Sessions/Events/Queries), not panel area
    itself (clip_rect_to_viewport already bounds that). Same CSS-px region
    captured, same content compared; only the raster resolution changes."""
    if clip is None:
        return None
    try:
        return page.screenshot(clip=clip, scale="css")
    except PWError:
        return None


def _safe_panel_screenshot(page, tab_id):
    """Convenience wrapper: _panel_clip_and_dims + _capture_with_clip in one
    call, for a caller that wants a single fresh frame (not part of a
    multi-frame sweep -- see _capture_at_offset for that path, which
    computes the clip ONCE per tick instead of once per call). Returns
    (frame_bytes_or_None, dims), never raising -- see _panel_clip_and_dims'
    own docstring for what dims contains and when it is all-None.

    issue #197: this used to be a plain elementHandle.screenshot() call,
    which -- when the element is taller than the viewport -- Playwright
    captures IN FULL by temporarily expanding the capture region beyond the
    viewport, not just the visible slice. The Sessions panel (#table-container
    has no vertical overflow/height cap -- web/static/style.css -- so it grows
    to its full row count) measured ~5793px tall at 205 rows on the gate box,
    and that single capture cost ~1.4s: the 5-frame blink sweep (SWEEP_OFFSETS_MS
    spans 200-2000ms) then took longer than the 5s live tick it was meant to
    sample inside of, so every sweep straddled a mount and the gate reported a
    red the UI never earned. Clipping the capture to the panel/viewport
    intersection (lib.panel_capture_clip / lib.clip_rect_to_viewport) makes
    one frame's cost roughly constant regardless of row count, and keeps the
    capture anchored to the panel (never a whole-page screenshot, which
    would let unrelated chrome dominate the diff -- see
    _PANEL_ELEMENT_SELECTOR's own comment)."""
    clip, dims = _panel_clip_and_dims(page, tab_id)
    return _capture_with_clip(page, clip), dims


# issue #142: single atomic page.evaluate() -- query the selector AND read its
# content in the SAME synchronous JS turn, so there is no gap for a page-side
# view's own (synchronous, atomic) el.innerHTML rebuild to straddle, unlike
# _safe_panel_screenshot's two separate CDP round trips (query_selector, then
# a LATER .screenshot() call). Reads the chart's own <canvas> pixels via
# toDataURL() when present (same JS thread, no separate screenshot API call);
# falls back to a DOM-content check for table tabs, whose panel selector
# (#table-container) is the ViewManager's own stable containerEl and is never
# replaced by a view's mount() (only its children are -- see app.js
# `vm.setContainer(tableEl)`, called once, never re-targeted per view).
_ATOMIC_PANEL_SNAPSHOT_JS = """(sel) => {
    const el = document.querySelector(sel);
    if (!el) return {present: false};
    const canvas = el.querySelector('canvas');
    if (canvas) {
        return {present: true, canvas: true, dataURL: canvas.toDataURL('image/png')};
    }
    return {present: true, canvas: false,
            hasContent: el.children.length > 0 || el.textContent.trim().length > 0};
}"""


def _atomic_panel_snapshot(page, tab_id):
    """Single-round-trip, race-free read of the panel's current state -- see
    _ATOMIC_PANEL_SNAPSHOT_JS. Returns the JS snapshot dict; ui_live_smoke_lib.
    blind_window_ok() turns it into a pass/fail verdict (kept pure/testable
    there, same idiom as every other verdict function in this module)."""
    return page.evaluate(_ATOMIC_PANEL_SNAPSHOT_JS, _PANEL_ELEMENT_SELECTOR[tab_id])


# ── per-frame DATA probe (issue #384, --diag-data only) ──────────────────────
#
# The blink sweep measures PIXELS. When a pair diverges it cannot say
# whether the chart's own data moved between the two frames or the UI
# flashed with identical data, because nothing recorded the data. This reads
# it straight off the rendered chart -- window.echarts' instance for the
# panel, its CURRENT option's series values -- so it is what the frame was
# painting, not a re-fetch (a re-fetch answers a different question: what
# the server would say now).
#
# OFF the gating path, by the #266 precedent (read_end_clock): called only
# when --diag-data was passed, so a gating tick pays exactly the round trips
# it paid before.
#
# Refuses rather than guesses: a missing panel, a missing window.echarts, no
# instance (a non-chart tab, or a disposed chart), a throw, or a series
# larger than DATA_PROBE_CELL_CAP all come back as {"error": ...} /
# {"truncated": true} and ui_live_smoke_lib.data_snapshot_ok turns them into
# a REFUSAL. Nothing here may be read as "no data, therefore unchanged".
DATA_PROBE_CELL_CAP = 60000

_DATA_PROBE_JS = """([sel, cap]) => {
    const atMs = Date.now();
    const out = {at_ms: atMs, selector: sel};
    try {
        const host = document.querySelector(sel);
        if (!host) { out.error = 'panel element ' + sel + ' not in the DOM'; return out; }
        const ec = window.echarts;
        if (!ec || typeof ec.getInstanceByDom !== 'function') {
            out.error = 'window.echarts.getInstanceByDom is missing'; return out;
        }
        let inst = ec.getInstanceByDom(host);
        if (!inst) {
            for (const k of host.querySelectorAll('div')) {
                const i2 = ec.getInstanceByDom(k);
                if (i2) { inst = i2; break; }
            }
        }
        if (!inst) {
            out.error = 'no ECharts instance on ' + sel + ' or its descendants '
                      + '(non-chart panel, or the chart is disposed)';
            return out;
        }
        const opt = inst.getOption();
        const series = (opt && opt.series) || [];
        const cells = [];
        const names = [];
        let truncated = false;
        const push = (v) => { cells.push(typeof v === 'number' ? v : NaN); };
        for (const s of series) {
            names.push((s.type || '?') + ':' + (s.name == null ? '' : s.name));
            for (const d of (s.data || [])) {
                if (cells.length >= cap) { truncated = true; break; }
                if (Array.isArray(d)) { for (const v of d) push(v); }
                else if (typeof d === 'number') { push(d); }
                else if (d && typeof d === 'object' && Array.isArray(d.value)) {
                    for (const v of d.value) push(v);
                } else if (d && typeof d === 'object') { push(d.value); }
                else { push(NaN); }
            }
            if (truncated) break;
        }
        out.series_count = series.length;
        out.series_names = names;
        out.cell_count = cells.length;
        out.truncated = truncated;
        out.cells = cells;
        out.read_ms = Date.now() - atMs;
        return out;
    } catch (e) {
        out.error = 'probe threw: ' + (e && e.message ? e.message : String(e));
        return out;
    }
}"""


def _panel_data_probe(page, tab_id):
    """One read of what the panel's chart is CURRENTLY rendering, plus the
    page's own Date.now() at the read (the wall-clock anchor every frame
    needs to be ordered against workload events in a single clock).

    Never raises: a Playwright-level failure is returned as an {"error":
    ...} snapshot so the diagnostic keeps a record of the refusal instead of
    aborting the tick it was only observing. ui_live_smoke_lib.
    data_snapshot_ok is what decides whether the record is usable."""
    try:
        return page.evaluate(_DATA_PROBE_JS,
                             [_PANEL_ELEMENT_SELECTOR[tab_id], DATA_PROBE_CELL_CAP])
    except Exception as e:
        return {"at_ms": None, "error": f"probe call failed: {e}"}


class _BlinkInjector:
    """issue #266: sequences the self-test's synthetic overlay against the
    sweep's own frame loop instead of against the wall clock.

    `before_frame(i)` / `after_frame(i)` are called by the sweep loop around
    every frame; only the target index does anything. The overlay is
    appended after frame i-1's capture call has RETURNED and removed after
    frame i's capture call has RETURNED, so the neighbouring frames are kept
    clean by PROGRAM ORDER -- no assumption about where in a capture call
    the browser's raster instant falls, which is the assumption #266's
    ~1-in-3 flake came from (see _INJECT_BLINK_SHOW_JS's own comment).

    Records `stimulus`, the four page-clock timestamps that bound the
    overlay's on-screen window, for ui_live_smoke_lib.inject_stimulus_verdict
    to grade against the frames' own capture windows. Every field starts as
    None and only a completed handshake fills it in: a show or hide that
    never ran leaves Nones behind, and the verdict REFUSES on a None rather
    than assuming the stimulus was there (an un-run injection must never
    read as a pass, and must never be blamed on the detector either).

    min_dwell_ms (--inject-blink-ms) is a FLOOR on the overlay's measured
    on-screen time, not a window placed on the clock -- see
    _INJECT_BLINK_SHOW_JS's comment."""

    # Safety-net margin added to min_dwell_ms for the page-side auto-remove
    # (see _INJECT_BLINK_SHOW_JS): must comfortably exceed one frame's
    # capture cost so it never truncates the stimulus during a healthy run.
    SAFETY_MARGIN_MS = 5000

    def __init__(self, page, tab_id, clip, frac, min_dwell_ms, target_index):
        self.page = page
        self.tab_id = tab_id
        self.clip = clip
        self.frac = frac
        self.min_dwell_ms = min_dwell_ms
        self.target_index = target_index
        self.stimulus = {
            "target_frame_index": target_index,
            "min_dwell_ms": min_dwell_ms,
            "shown_requested_ms": None,
            "shown_painted_ms": None,
            "hidden_requested_ms": None,
            "hidden_cleared_ms": None,
            "was_present_at_removal": None,
        }

    def before_frame(self, index):
        if index != self.target_index:
            return
        res = self.page.evaluate(_INJECT_BLINK_SHOW_JS, {
            "x": self.clip["x"], "y": self.clip["y"],
            "width": self.clip["width"],
            "height": self.clip["height"] * self.frac,
            "safetyMs": self.min_dwell_ms + self.SAFETY_MARGIN_MS,
        }) or {}
        self.stimulus["shown_requested_ms"] = res.get("requestedAtMs")
        self.stimulus["shown_painted_ms"] = res.get("paintedAtMs")

    def after_frame(self, index):
        if index != self.target_index:
            return
        painted = self.stimulus["shown_painted_ms"]
        # No painted time means the show handshake never completed; remove
        # whatever may be on screen immediately (min_until in the past) and
        # let the verdict refuse on the missing field.
        min_until = (painted + self.min_dwell_ms) if painted is not None else 0
        res = self.page.evaluate(_INJECT_BLINK_HIDE_JS,
                                 {"minUntilMs": min_until}) or {}
        self.stimulus["hidden_requested_ms"] = res.get("requestedAtMs")
        self.stimulus["hidden_cleared_ms"] = res.get("clearedAtMs")
        self.stimulus["was_present_at_removal"] = res.get("wasPresent")


def _capture_at_offset(page, tab_id, base_ts_ms, offset_ms, clip,
                       read_end_clock=False):
    """Sleeps only the remainder to base_ts_ms+offset_ms (never a flat sleep
    from wherever this is called) and screenshots the panel at the given,
    already-computed clip (issue #252: one clip per TICK, shared by every
    frame of that tick's sweep -- see _panel_clip_and_dims). Returns
    (raw_png_or_None, achieved_offset_ms, capture_ms, end_clock_ms) -- the
    achieved offset can exceed offset_ms if prior work already ran past the
    target.

    read_end_clock (issue #266): when True, reads the PAGE's clock once more
    after the screenshot returns and returns it as end_clock_ms, so the
    frame's whole capture interval is expressed in a SINGLE clock -- the
    same Date.now() the injected-stimulus timestamps use. Deriving the
    interval's end as start + capture_ms instead mixes the page clock with
    Python's time.monotonic(); measured on the Mac, that left the
    stimulus-vs-frame boundary margins at 1.7ms and 2.7ms, i.e. a clock
    comparison that could go either way, which is precisely the class of
    unmeasured assumption #266 exists to remove. Costs one extra CDP round
    trip per frame, so it is requested ONLY on a self-test tick (the
    injected tick of a --inject-blink-ms run); every gating tick calls this
    exactly as before and gets end_clock_ms=None.
    base_ts_ms is the mount event's own timestamp (issue #193 round 2), not
    the tick's AAS-request send -- see _wait_for_mount_at_or_after.
    capture_ms (issue #197 evidence) is the wall-clock time the screenshot
    call itself took -- the direct, measured answer to "is a frame's capture
    cost bounded", not inferred from achieved-offset drift."""
    target_ms = base_ts_ms + offset_ms
    now_ms = page.evaluate("Date.now()")
    if target_ms > now_ms:
        page.wait_for_timeout(target_ms - now_ms)
        now_ms = page.evaluate("Date.now()")
    t0 = time.monotonic()
    frame = _capture_with_clip(page, clip)
    capture_ms = (time.monotonic() - t0) * 1000.0
    end_clock_ms = page.evaluate("Date.now()") if read_end_clock else None
    return frame, now_ms - base_ts_ms, capture_ms, end_clock_ms


def _view_mount(page):
    """Reads window.__pgwt.viewMount() (issue #193's mount chokepoint hook:
    web/static/lib/view-manager.js's ViewManager.lastMount, exposed by
    web/static/app.js). Returns the raw {"id", "seq", "at"} dict, or None
    before the first mount of the page's lifetime.

    Raises SmokeFailure if the hook ITSELF is missing -- an old/broken
    app.js build must fail loudly here, never be silently read as "nothing
    has mounted yet" (which would make _wait_for_mount_at_or_after poll all
    the way to its own timeout looking exactly like a slow-but-real wait,
    hiding a missing-instrumentation bug behind a generic timeout
    message)."""
    result = page.evaluate(VIEW_MOUNT_JS)
    if result.get("missing"):
        raise SmokeFailure(
            "window.__pgwt.viewMount() is missing -- the mount-sequence "
            "instrumentation (issue #193) is not present in this build")
    return result.get("mount")


def _wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, timeout_s=TICK_TIMEOUT_S,
                                 interval_ms=50, min_seq=None):
    """Polls until the ViewManager mount chokepoint (_view_mount) reports a
    mount of `tab_id` whose OWN wall-clock timestamp (`at`, Date.now() in
    the SAME browser context as tick_ts_ms) is >= tick_ts_ms -- issue #193's
    replacement for a fixed delay from the tick's AAS-request send: the
    summary pane's own round trip runs before ViewManager.refresh()
    (web/static/app.js's refresh()), so a fixed guess from the send time
    undershoots under load; this waits for the actual paint decision
    instead of guessing when it happens.

    Compares TIMESTAMPS, not a FIXED seq baseline read once at navigation
    time (review round 2 found that off by one): navigating to a tab already
    triggers its own refresh()/mount() (switchTab's refreshActive() +
    vm.switchTo(), which is also __uiLiveTicks' very first entry), so a seq
    value read right after navigation already reflects tick 1's own mount
    -- waiting for "seq > that ONE baseline, forever" there would silently
    wait for tick 2's mount instead, and every later tick would inherit the
    one-tick shift. A timestamp comparison alone has no such fixed baseline
    to get wrong: `mount.at >= tick_ts_ms` is correct for tick 1 the exact
    same way it is for every later tick.

    min_seq (issue #197, per-call, NOT a fixed baseline -- the caller passes
    the PREVIOUS TICK's own consumed mount seq each time, None for tick 1):
    timestamp alone is not sufficient. A mount whose own refresh cycle takes
    longer than the 5s live tick interval can land with `at` already past
    the NEXT tick's tick_ts, so a purely timestamp-based wait can be handed
    back the SAME mount the previous tick's sweep already anchored to and
    consumed -- that tick's whole 5-frame sweep then fires back-to-back
    against an already-stale mount_at_ms, with none of the spacing across
    the tick that lets it see a sub-second transient at all (evidence: the
    rehearsal's achieved first offsets drifted to 2.5-4.5s instead of the
    ~200ms target -- one live mount sampled multiple times, same seq, same
    pixels). min_seq requires the accepted mount's OWN seq to be strictly
    newer than the one already consumed (lib.mount_is_fresh) -- this is
    per-tick state threaded through by the caller, not the single
    navigation-time baseline round 2 rejected, so it does not reintroduce
    that bug: tick 1 passes min_seq=None (no previous tick to be newer
    than), exactly like the timestamp check already does.

    Raises SmokeFailure -- NEVER hangs, never silently returns a stale
    record -- if the hook is missing (see _view_mount) or no matching FRESH
    mount lands within timeout_s. Both are real product-visible failures:
    the first means the instrumentation itself regressed, the second means
    the tab's view never repainted a NEW mount for a whole tick's worth of
    budget (a frozen tab, or -- pre-fix -- the harness quietly re-sweeping a
    stale one forever)."""
    deadline = time.monotonic() + timeout_s
    while True:
        mount = _view_mount(page)
        if lib.mount_is_fresh(mount, tab_id, tick_ts_ms, min_seq):
            return mount
        if time.monotonic() >= deadline:
            raise SmokeFailure(
                f"no FRESH view.mount() of tab {tab_id!r} at/after this "
                f"tick's AAS send ({tick_ts_ms}) with seq > {min_seq!r} "
                f"landed within {timeout_s}s (issue #193/#197 mount-anchor "
                "wait)")
        page.wait_for_timeout(interval_ms)


def _poll_render_check(page, tab_id, timeout_s=2.0, interval_ms=150):
    """Evaluates PANEL_CHECKS[tab_id], retrying briefly on failure. A tick's
    re-render (ECharts dispose+init when the underlying data identity
    changes -- e.g. Waterfall's "latest execution" rotating under continuous
    real load, observed against a real daemon) can take a moment after the
    tick's WS response lands before the chart instance exists again. Bounded
    exactly like Playwright's own wait_for_selector: a check STILL failing
    after this budget is a real failure, reported as such, never swallowed."""
    deadline = time.monotonic() + timeout_s
    while True:
        raw = page.evaluate(PANEL_CHECKS[tab_id])
        ok, detail = lib.render_check_ok(raw)
        if ok or time.monotonic() >= deadline:
            return ok, detail
        page.wait_for_timeout(interval_ms)


def _settled_leak_probe(page, timeout_s=TICK_TIMEOUT_S, interval_ms=200):
    """Poll LEAK_PROBE_JS until `pending` settles to 0 or the budget runs
    out. A single-instant read can catch a request that is normally
    in-flight for a moment (a real server under real load, vs. the
    zero-latency mock) and wrongly call it a leak -- test_web_ui_chaos.py's
    test_soak_random_navigation re-probes the SAME probe after a settle
    window for exactly this reason; this is the same idiom, not a widened
    tolerance (a request stuck pending for the whole budget still fails).
    Budget matches TICK_TIMEOUT_S: measured on the gate box, a `transitions`/
    `exec_scatter`/`top_queries` query over several minutes of --mode full
    capture (every event, not sampled) can legitimately take a few seconds
    to answer -- the same real-server-latency reasoning that sized
    TICK_TIMEOUT_S already, not a separate ad hoc allowance.

    Returns (probe, elapsed_s): the elapsed time is recorded in summary.json
    (issue #93 review item 6) even when it does not, by itself, fail
    no_leak -- a probe that took most of the budget to settle is a latency
    regression worth a trace."""
    start = time.monotonic()
    deadline = start + timeout_s
    probe = page.evaluate(LEAK_PROBE_JS)
    while probe.get("pending", 0) != 0 and time.monotonic() < deadline:
        page.wait_for_timeout(interval_ms)
        probe = page.evaluate(LEAK_PROBE_JS)
    return probe, time.monotonic() - start


def _pid_is_live(pid):
    """True if pid exists and is not a zombie. os.kill(pid, 0) alone is not
    enough (issue #93 review nit): it succeeds for a zombie too -- the PID
    stays reserved until the parent reaps it, so a dead-but-unreaped pgbench/
    workload process would pass as "alive" and the walk would keep grading
    an idle system. /proc/<pid>/stat's state field (immediately after the
    LAST ')' -- the command name itself can contain parens) is 'Z' for a
    zombie; anything else, or an unreadable/missing /proc entry meaning the
    process is fully gone, is handled by the caller."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            content = f.read()
    except OSError:
        return False
    fields_after_comm = content.rsplit(")", 1)[-1].split()
    state = fields_after_comm[0] if fields_after_comm else ""
    return state != "Z"


def _assert_workload_alive(pgbench_pid, workload_pid, where):
    """FAILS LOUDLY (raises SystemExit) if either the pgbench or the lock/
    sleep workload process is gone (or a zombie -- see _pid_is_live). Called
    at each tab boundary (issue #93 review item 2): a controlled-load
    process that died partway through the walk would leave the REMAINING
    tabs grading an idle system -- rendered/no_blink/no_leak could all still
    trivially "pass" against stale, frozen data, which is exactly the false
    confidence this whole test exists to prevent. None for either pid means
    the caller did not wire this check up (e.g. --mock, or a manual --url
    run) -- skipped, not failed."""
    for label, pid in (("pgbench", pgbench_pid), ("lock/sleep workload", workload_pid)):
        if pid is None:
            continue
        if not _pid_is_live(pid):
            print(f"FATAL: {label} (pid {pid}) is no longer running ({where}) -- "
                  f"refusing to grade the remaining tabs against an idle system",
                  file=sys.stderr)
            sys.exit(1)


FOOTAGE_NOT_EVALUATED = "not evaluated in footage mode"


def _wait_footage_offset(page, base_ts_ms, offset_ms):
    """Keep the sweep's absolute deadlines without calling a capture API.

    The gated _capture_at_offset stays untouched, including its cost.
    No synthetic capture latency: only the removed captures cease to take time.
    """
    target_ms = base_ts_ms + offset_ms
    now_ms = page.evaluate("Date.now()")
    if target_ms > now_ms:
        page.wait_for_timeout(target_ms - now_ms)
        page.evaluate("Date.now()")


def _mark_footage_result(result, ticks_observed, ticks_requested, video_path):
    """Unevaluated image checks can never yield a green gate, even on errors."""
    result["footage_only"] = True
    result["footage_complete"] = (
        video_path is not None and "error" not in result
        and ticks_observed == ticks_requested
        and all(result[key]["ok"] is True for key in
                ("rendered", "clean", "no_leak", "color_stability", "first_paint")))
    result["ok"] = False
    result["known_failing"] = False
    result["xpass"] = False
    result["ticks_observed"] = ticks_observed
    for key in ("no_blink", "capture_budget", "frame_dims", "frame_spacing"):
        result[key]["ok"] = None
        result[key]["detail"] = FOOTAGE_NOT_EVALUATED
    result["no_blink"]["measured"].update(
        ok=None, measured_count=0, detail=FOOTAGE_NOT_EVALUATED)
    result["blink_sweep"]["detail"] = FOOTAGE_NOT_EVALUATED
    for diagnostic in result.get("pre_mount_diagnostic", []):
        diagnostic["note"] = FOOTAGE_NOT_EVALUATED


# ── one tab's walk ───────────────────────────────────────────────────────────

def run_tab(browser, tab_id, url, out_dir, ticks, first_data_timeout,
            blink_threshold, viewport=None, device_scale_factor=None,
            inject_blink_ms=None, inject_tick=None,
            inject_blink_frac=DEFAULT_INJECT_BLINK_FRAC,
            inject_frame_index=lib.INJECT_FRAME_INDEX, diag_data=False,
            footage_only=False, replay_probe=None):
    """replay_probe: optional replay observer, attached before navigation and
    verified after the existing navigation/render checks. Requires ticks=0;
    fixed-window URL hydration keeps Live off. The replay result reports only
    rendering, transport/DOM agreement and leak/console checks, never live
    blink or tick measurements. Existing callers retain the live path.

    inject_blink_ms/inject_tick (issue #252 self-test, off by default --
    None/None, the value every real gating caller -- tests/ui_live_smoke.sh,
    demo_rehearsal.py -- passes, so this is a strict addition with no effect
    on a normal run): when both are set, a synthetic overlay covering the
    top `inject_blink_frac` fraction of the panel's own clip rect (full
    width) is made visible for exactly ONE frame of this tab's
    `inject_tick`'th sweep -- frame `inject_frame_index` (issue #266: shown
    after frame index-1's capture returns, removed after frame index's
    capture returns, so program order, not clock arithmetic, keeps it out of
    the neighbouring frames; see _BlinkInjector) -- and the resulting sweep
    ratio/verdict is recorded on the returned result as "self_test". This is
    the proof that a REAL blink of known size still fails the gate after a
    capture-path change; the pure-function tests
    (test_ui_live_smoke_lib.py) exercise the verdict math in isolation but
    cannot see a capture resolution/region change, which is exactly the kind
    of change issue #252 makes.

    The self_test record carries the stimulus's own measured on-screen
    bounds and every frame's capture window, and
    lib.inject_stimulus_verdict grades the GEOMETRY separately from the
    detection: `stimulus_ok` says the overlay provably covered the target
    frame's whole capture interval and no other frame's, and `caught` is
    then read from the consecutive PAIRS the stimulus itself created
    (index-1,index) and (index,index+1) -- not from the sweep's max ratio,
    which any unrelated diff or failed capture elsewhere in the sweep could
    satisfy on its own. Before #266 the self-test asserted only "the max
    ratio cleared the threshold" against a wall-clock overlay window
    nothing verified, so a red meant either a broken detector OR a stimulus
    that was never on screen when a frame was taken -- and on the gate box
    it meant the second ~1 run in 3.

    inject_blink_frac (review round 1): a FULL-panel overlay produces
    ratio~1.0, three orders of magnitude above BLINK_THRESHOLD (0.001) --
    it proves a gross blink is caught, not that the actual configured
    threshold still works. DEFAULT_INJECT_BLINK_FRAC (a top horizontal
    slice, not the whole panel) still comfortably clears the threshold
    (robust against anti-aliasing/cell-boundary noise a threshold-sized
    slice would risk) while no longer stepping over it by 1000x.

    viewport / device_scale_factor (issue #223, default changed by #252):
    the criteria doc's pinned demo client viewport, 1710x981 at DPR 2 (see
    docs/DEMO_REHEARSAL_CRITERIA.md and
    docs/chrome-demo-viewport-2026-09-28.md). Originally ONLY
    tests/demo_rehearsal.py passed these, and this docstring used to say
    ui_live_smoke.py's own CI-tier walk (main(), below) keeping the
    narrower 1280x900 DSF1 default instead "would slow every PR for
    nothing". #252 falsifies that: before its fix, DPR2 quadrupled the
    pixels a screenshot had to encode/decode, that cost was exactly what
    made the Sessions sweep drift past a whole tick at demo length, and NO
    PR gate ran at DPR2 at all -- so the defect passed four PR gates
    before a 40-minute rehearsal finally caught it, at real cost (the
    rehearsal's own attempt counter, and a demo-blocking bug found far
    later than a PR gate could have found it).

    #252's fix (page.screenshot(scale="css"); one clip computed per tick,
    not per frame) makes per-frame capture cost DPR-INDEPENDENT --
    reviewed and measured: 64.5ms at DPR2 vs 62.0ms at DPR1, a 4% gap, not
    the 2x/4x the pixel count would suggest. So the DPR2 default does NOT
    exist to "exercise DPR2's real capture cost" (review round 1 finding
    4 -- an earlier version of this comment claimed exactly that, and it
    was wrong: with scale="css" there is no separate DPR2 cost left to
    exercise). It exists for two different, narrower reasons: (1) it
    renders the demo's actual layout and raster geometry, the same
    viewport a real viewer/presenter sees, which 1280x900 DSF1 does not;
    (2) it is the TRIPWIRE for a FUTURE regression -- if scale="css" is
    ever removed, decoded frame dimensions double relative to the CSS-px
    clip only at DPR2 (DPR1's scale is 1x either way, so nothing there
    would change), so running the gate at DPR2 is what would make that
    specific regression visible (see build_tab_result's frame_dims_ok
    check -- lib.FRAME_DIMS_TOLERANCE_PX's own comment -- which catches
    exactly this deterministically, from the decoded array's own shape,
    not from a wall-clock inference: lib.capture_budget is reporting-only
    now, see lib.CAPTURE_MS_BOUND_MS's own comment for why). A later agent
    reading only the OLD wording could conclude the DPR2 walk itself is
    the cost guard and remove something else (e.g. this viewport default)
    as "redundant" with that check -- it is not; the check depends on
    running at DPR2 to have anything to catch. An explicit override
    (--viewport-width/--viewport-height/--device-scale-factor) is still
    available for anyone who needs the old, narrower walk.

    Whichever viewport is requested, it is never trusted on faith: right
    after the page loads, the ACTUAL innerWidth/innerHeight/devicePixelRatio
    are read back and compared against what was requested
    (lib.viewport_mismatch_reason) -- docs/DEMO_REHEARSAL_CRITERIA.md names
    exactly this trap ("assert the viewport actually obtained, not the one
    requested" / "assert the effective zoom too"): a request that never
    actually reached browser.new_context() must fail loudly here, not read
    as a silent, plausible-looking pass."""
    # Footage is an artifact; the gate is separate. The footage walk's own
    # checks are NOT the evidence -- the counted rehearsals are. Clipped
    # screenshots disturb the attached video recorder; replacing them with
    # full-viewport captures increases gated capture cost. Footage therefore
    # performs zero CDP captures, without changing the normal capture path.
    if replay_probe is not None and (ticks != 0 or footage_only):
        raise ValueError("replay requires ticks=0 and no footage mode")
    if footage_only and (diag_data or inject_blink_ms is not None):
        raise ValueError("footage mode cannot evaluate screenshot diagnostics or blink injection")
    req_width = viewport["width"] if viewport else 1280
    req_height = viewport["height"] if viewport else 900
    req_dsf = device_scale_factor if device_scale_factor is not None else 1
    tab_dir = os.path.join(out_dir, tab_id)
    os.makedirs(tab_dir, exist_ok=True)
    # issue #384: per-frame diagnostic artifacts (one PNG per COMPARED frame
    # + one JSON per tick). Created only on a --diag-data run, so a gating
    # run's artifact tree is byte-for-byte what it was.
    diag_dir = os.path.join(tab_dir, "diag")
    if diag_data:
        os.makedirs(diag_dir, exist_ok=True)
    context = browser.new_context(
        viewport={"width": req_width, "height": req_height},
        device_scale_factor=req_dsf,
        record_video_dir=tab_dir,
        record_video_size={"width": req_width, "height": req_height})
    context.add_init_script(UNHANDLED_REJECTION_HOOK)
    page = context.new_page()
    guard = ConsoleErrorGuard(page)
    console_errors = []
    pgwt_errors = []
    tick_paths = []
    video_path = None
    footage_ticks = 0
    # None until _navigate_to_tab's own return sets it below -- if a
    # SmokeFailure fires before then (or navigation itself fails), the
    # except branch below passes this unmeasured None through rather than
    # raising NameError. If navigation SUCCEEDS but a LATER tick-loop step
    # fails, this already holds the real measured value, and
    # build_failed_tab_result carries it through instead of discarding it.
    ttfp_ms = None
    # None means the sweep diagnostic was never reached; do not report a
    # fabricated empty measurement when navigation itself failed.
    pre_mount_diagnostics = None
    try:
        if replay_probe is not None:
            replay_probe.attach(page)
        page.goto(url)
        actual = page.evaluate(
            "() => ({w: window.innerWidth, h: window.innerHeight, "
            "dpr: window.devicePixelRatio})")
        viewport_reason = lib.viewport_mismatch_reason(
            req_width, req_height, req_dsf,
            actual["w"], actual["h"], actual["dpr"])
        if viewport_reason is not None:
            raise SmokeFailure(
                f"viewport not achieved as requested: {viewport_reason}")
        try:
            page.wait_for_selector("#status.connected", timeout=first_data_timeout * 1000)
        except PWTimeoutError:
            raise SmokeFailure(
                f"WebSocket never reached #status.connected within {first_data_timeout}s")

        if replay_probe is not None:
            replay_probe.assert_owner(page)

        # No-data fail-safe (issue #93 acceptance item 6): real rows, not a
        # perpetual '.loading' placeholder, within the fixed budget.
        deadline = time.monotonic() + first_data_timeout
        while page.query_selector("#table-container table tbody tr") is None:
            if time.monotonic() > deadline:
                raise SmokeFailure(f"UI showed no data within {first_data_timeout}s")
            page.wait_for_timeout(200)

        page.evaluate(TICK_HOOK_JS)
        ttfp_ms = _navigate_to_tab(page, tab_id, first_data_timeout,
                                   resume_live=replay_probe is None)
        replay_result = None
        if replay_probe is not None:
            render = _poll_render_check(page, tab_id, first_data_timeout)
            if not render[0]:
                raise SmokeFailure(render[1])
            replay_result = replay_probe.verify(page)
            path = os.path.join(tab_dir, "replay.png")
            page.screenshot(path=path, scale="css")
            tick_paths.append(path)

        leak_before, leak_before_settle_s = _settled_leak_probe(page)

        # Tick BASELINE (this issue): the number of AAS sends TICK_HOOK_JS has
        # already recorded by the time navigation and the leak probe are both
        # finished. The loop below waits for `tick_baseline + i`, not `i`.
        #
        # Why: the hook is installed BEFORE _navigate_to_tab, and navigation
        # itself sends AAS -- switchTab's refreshActive(), plus a SECOND send
        # from the #live-btn resume click on every tab whose view pauses live
        # (app.js pausesLive: transitions/concurrency/waterfall/scatter/matrix,
        # and timeline via drill()). So __uiLiveTicks was already non-empty
        # here and `_wait_for_tick(page, 1)` returned IMMEDIATELY, handing
        # tick 1 a tick_ts that predates _navigate_to_tab's own fixed
        # `wait_for_timeout(500)` resume settle. The sweep then anchored on
        # that navigation mount, so 500ms of settle + the leak probe + the
        # 100ms blind-window sleep + the render check all sat BETWEEN the
        # anchor and the sweep's own first frame.
        #
        # Measured (achieved_offsets_ms[0], i.e. mount -> just before frame 1):
        # tick 1 ran 426-853ms against 203-226ms on ticks 2-6, in every one of
        # 31 retained runs over 10 days, and ONLY on the tabs that pause live
        # (excess 111-641ms vs -5..+195ms for the rest). That is what forced
        # frame_spacing_ok's tick-1 exemption, and it is why frame 3 of tick 1
        # drifted 57-183ms on a HEALTHY tree: frame 3's target of 1000ms was
        # already in the past once A+capture(f1)+capture(f2) exceeded it.
        # See tests/results/ui_live_anchor_evidence/ for the full derivation.
        #
        # Worse, the drift it produced moved the WRONG WAY: because the 500ms
        # settle is a fixed sleep, a tab that painted FASTER left more of that
        # sleep on the far side of its own mount, so its anchor was STALER.
        # r(ttfp_ms, tick-1 A) = -0.77 over 30 gate-1-class tab-runs
        # (timeline -0.96); transitions' ttfp_ms fell 1211 -> 581ms while its
        # tick-1 A rose 479 -> 662ms. The gate was firing because first paint
        # IMPROVED.
        #
        # NOT a navigation-time SEQ baseline: _wait_for_mount_at_or_after's own
        # docstring records review round 2 rejecting exactly that ("seq > that
        # ONE baseline, forever" silently waits for tick 2's mount and shifts
        # every later tick). This is a TICK-COUNT baseline consumed once to
        # offset the loop's target; the per-tick min_seq threading below is
        # untouched, so that bug is not reintroduced.
        #
        # Cost: tick 1 now waits for a genuinely new live tick, so a tab's walk
        # starts up to one TICK_INTERVAL_S (5s) later -- ~55s across 11 tabs.
        # TICK_TIMEOUT_S is 30s, a 6x margin over that one 5s interval, so the
        # baseline cannot turn a healthy tab into a timeout.
        tick_baseline = page.evaluate("window.__uiLiveTicks.length")

        legend_ticks = []
        blink_ratios = []
        blink_pair_offsets_ms = []
        blink_sweep_ticks = []
        blink_not_measured = []
        # issue #252 self-test: filled in only on inject_tick, only when
        # inject_blink_ms was given (both None for every real gating
        # caller). None otherwise, all the way to the returned result.
        self_test_result = None
        # issue #197: the seq of the mount the PREVIOUS tick's sweep
        # consumed, so _wait_for_mount_at_or_after refuses to hand back the
        # same (stale) mount to this tick -- see its own docstring and
        # lib.mount_is_fresh. None for tick 1 (no previous tick to be newer
        # than).
        last_consumed_mount_seq = None
        # issue #100 (review round 4): timeline-only diagnostic, one entry
        # per tick -- see the capture site below for what it measures and
        # why. Empty for every other tab; never read by build_tab_result's
        # `ok` computation.
        pre_mount_diagnostics = []
        # issue #384 (--diag-data only): one entry per tick, each with every
        # frame's bracketing data probes, its page-clock capture window and
        # its own PNG, plus lib.sweep_data_pair_reports' per-pair verdict.
        # Empty on every gating run; written to <tab_dir>/diag/ and never
        # read by build_tab_result.
        diag_ticks = []
        render_ok, render_detail = False, "never checked"
        for i in range(1, ticks + 1):
            if not _wait_for_tick(page, tick_baseline + i, TICK_TIMEOUT_S):
                raise SmokeFailure(
                    f"tick {i}/{ticks} (hook index {tick_baseline + i - 1}, "
                    f"baseline {tick_baseline}) did not arrive within "
                    f"{TICK_TIMEOUT_S}s")
            # Read the tick this iteration actually waited for, by INDEX --
            # not "whatever is last". The two differ whenever a further tick
            # lands while this iteration is still working (the sweep alone
            # spans ~2s of a 5s cadence), and reading `last` there would
            # silently re-anchor the iteration on a NEWER send than the one it
            # represents. lib.tick_hook_index() is the pure index arithmetic
            # (unit-tested); it refuses a baseline/i that cannot address a
            # real entry rather than returning something plausible.
            tick_ts_ms = page.evaluate(
                "idx => window.__uiLiveTicks[idx]",
                lib.tick_hook_index(tick_baseline, i))
            if tick_ts_ms is None:
                raise SmokeFailure(
                    f"tick {i}/{ticks}: __uiLiveTicks["
                    f"{lib.tick_hook_index(tick_baseline, i)}] is absent after "
                    f"_wait_for_tick reported {tick_baseline + i} ticks -- the "
                    "tick hook was replaced or reset mid-walk")

            # issue #100 (review round 4, corrected round 5), TIMELINE
            # ONLY: try to capture the panel as the PREVIOUS tick's mount
            # left it, before THIS tick's own mount can touch it, then diff
            # it below against the sweep's own first frame (mount+200ms) to
            # test #100's premise (the reported ~4.5% "unchanged-data"
            # redraw was actually the tick's own legitimate window-advance
            # repaint -- axis labels/gridlines, timeline.js's xAxis sits at
            # the bottom of the grid -- caught mid-flight by the OLD
            # 1200ms-anchor instrument). Never gates: reporting-only, read
            # by nothing in build_tab_result.
            #
            # Round 4's mistake: it assumed "nothing repaints containerEl
            # between mounts" (true, ViewManager's own contract) meant this
            # capture was safe to trust unconditionally. It does not -- that
            # invariant says nothing about whether OUR OWN capture (a real
            # round trip: page.evaluate + a screenshot + PNG decode, real
            # wall-clock time) finishes BEFORE the next mount, which is
            # timeline's own live race (its mount can land ~150ms after the
            # tick, sometimes faster than this capture completes). Asserted,
            # never proven -- and on a real run it was proven WRONG twice
            # (ticks 5-6, review round 5): a genuine window-advance repaint
            # happened on both, confirmed by diffing the run's own saved
            # per-tick frames, while this probe read 0.0 on the exact same
            # ticks, because its "before" frame was actually taken AFTER.
            #
            # Fixed the same way the gating sweep already brackets its own
            # capture: read the mount seq immediately AFTER this screenshot;
            # only trust the frame as "before" if that seq is STILL behind
            # THIS tick's eventual mount once it lands (checked further
            # down, once `mount` is known, via
            # lib.pre_mount_diagnostic_verdict). None (no mount observed at
            # all yet) trivially precedes any mount that will ever land.
            pre_mount_frame = None
            pre_mount_seq_after = None
            if tab_id == "timeline":
                if not footage_only:
                    pre_mount_frame, _pre_mount_dims = _safe_panel_screenshot(page, tab_id)
                else:
                    _panel_clip_and_dims(page, tab_id)  # retain the same scroll/geometry prep
                m_after = _view_mount(page)
                pre_mount_seq_after = m_after["seq"] if m_after is not None else None

            # Blind-window check (issue #93 review item 5): between the tick
            # landing and the render-check retry + gating settle below,
            # NOTHING is sampled -- a teardown-to-blank flash right after the
            # tick (a CONTINUITY violation) would go completely unnoticed by
            # the later, already-resettled blink pair. Grab one atomic
            # snapshot ~100ms after the tick and assert the panel is not
            # blank. Widens nothing: this is an ADDITIONAL check, not a
            # relaxed one.
            #
            # issue #142: this used to be _safe_panel_screenshot() (two
            # separate CDP round trips: query_selector, then a LATER
            # .screenshot() call), which intermittently reported scatter/
            # matrix as "gone" -- not because the panel was ever actually
            # absent or blank, but because chart-tab views rebuild their host
            # div via el.innerHTML on EVERY refresh (synchronously, so no
            # externally-observable in-between state exists -- see
            # _safe_panel_screenshot's own updated docstring), and that
            # rebuild occasionally landed in the gap between the two round
            # trips, staling the handle this function already held.
            # _atomic_panel_snapshot reads the panel in a SINGLE
            # page.evaluate() call, closing that gap by construction: the
            # panel is observed at a moment the product actually holds a
            # single, atomic DOM/paint state, never a torn one.
            page.wait_for_timeout(100)
            ok, reason = lib.blind_window_ok(_atomic_panel_snapshot(page, tab_id))
            if not ok:
                raise SmokeFailure(
                    f"tick {i}: {reason} ~100ms after the tick "
                    "(CONTINUITY teardown-to-blank)")

            render_ok, render_detail = _poll_render_check(page, tab_id)
            if not render_ok:
                raise SmokeFailure(f"tick {i}: {render_detail}")

            # issue #193 (round 2, review): the GATING check IS the offset
            # sweep (issue #119, SWEEP_OFFSETS_MS), anchored on the mount
            # chokepoint's own event for this tab instead of a fixed delay
            # from the tick's AAS-request send. Review proved a single
            # anchored pair -- however it is anchored -- is blind to a
            # transient narrower than its own capture window (an injected
            # 400ms overlay measured ratio 0.0 on the pair, 1.0 on every
            # sweep offset); multiple samples spread across the tick are
            # what actually catch a transient of unknown width/position.
            #
            # 1. Wait for a mount of THIS TAB whose own timestamp is at/
            #    after this tick's AAS send AND whose seq is strictly newer
            #    than the previous tick's own consumed mount
            #    (_wait_for_mount_at_or_after) -- the chokepoint's own signal
            #    that a fresh response was actually painted, never a guess
            #    at when that happens, and never the same mount a previous
            #    tick already swept (issue #197).
            mount = _wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, TICK_TIMEOUT_S,
                                                 min_seq=last_consumed_mount_seq)
            mount_at_ms = mount["at"]
            last_consumed_mount_seq = mount["seq"]
            # issue #93 review item 3, kept: record how long after the AAS
            # send the mount landed, so a future drift under load stays
            # visible in summary.json.
            achieved_a_ms = mount_at_ms - tick_ts_ms
            blink_pair_offsets_ms.append(achieved_a_ms)

            # 2. Re-check the sequence immediately before the sweep's first
            #    capture and immediately after its last, bracketing the
            #    WHOLE sweep -- if it moved anywhere in between (a second
            #    tick's mount landing mid-sweep), the sweep straddled a real
            #    content boundary and must be reported NOT MEASURED, never
            #    a fabricated ratio.
            seq_before_sweep = mount["seq"]
            sweep_offsets = list(lib.SWEEP_OFFSETS_MS)
            # issue #252 (performance): one clip for the WHOLE tick's sweep,
            # not one per frame -- see _panel_clip_and_dims' own docstring
            # for why this is exact, not an approximation, for the ~2s a
            # sweep spans.
            sweep_clip, sweep_dims = _panel_clip_and_dims(page, tab_id)
            # issue #252 self-test, re-sequenced for issue #266: the
            # overlay is shown and hidden by _BlinkInjector from INSIDE the
            # frame loop below (after frame target-1 returns, and after
            # frame target returns), not scheduled on the wall clock here.
            # See _BlinkInjector / _INJECT_BLINK_SHOW_JS for why: the old
            # [mount+700, mount+1100] window relied on an unmeasured
            # assumption about where inside a page.screenshot() call the
            # raster instant falls, and went red ~1 run in 3 on identical
            # code when it fell the other way.
            injector = None
            if (inject_blink_ms is not None and inject_tick is not None
                    and i == inject_tick and sweep_clip is not None):
                # review round 1: top SLICE of the clip rect, not the whole
                # thing -- see DEFAULT_INJECT_BLINK_FRAC's own comment.
                injector = _BlinkInjector(page, tab_id, sweep_clip,
                                          inject_blink_frac, inject_blink_ms,
                                          inject_frame_index)
            sweep_raw_frames = []
            sweep_achieved_ms = []
            sweep_capture_ms = []
            # (call_start_ms, call_end_ms) per frame in the PAGE's own clock
            # -- the interval the frame's unknown raster instant must lie
            # in. BOTH ends are page-clock reads taken by
            # _capture_at_offset (read_end_clock=True), never a page start
            # plus a Python-measured duration: the stimulus timestamps are
            # page-clock too, so every comparison in
            # inject_stimulus_verdict is within one clock and ordered by
            # causality (each read happens-after the previous CDP call),
            # which is what makes the geometry proof hold by construction
            # instead of by margin. Only recorded on a self-test tick;
            # [] on every gating tick, which pays no extra round trip.
            sweep_frame_windows_ms = []
            # issue #384 (--diag-data only): one record per frame.
            diag_frames = []
            for frame_index, offset in enumerate(sweep_offsets):
                if injector is not None:
                    injector.before_frame(frame_index)
                # issue #384: the data probe BRACKETS the capture call -- one
                # read before it, one after -- because a screenshot's raster
                # instant lies somewhere inside the call. Program order plus
                # CDP's own ordering is what makes that a bracket, the same
                # argument #266 makes for read_end_clock; only when both
                # reads agree is the frame's data certified (see
                # lib.frame_data_certified). Both reads are skipped entirely
                # unless --diag-data asked for them.
                probe_before = _panel_data_probe(page, tab_id) if diag_data else None
                if footage_only:
                    _wait_footage_offset(page, mount_at_ms, offset)
                    frame, achieved, capture_ms, end_clock_ms = None, None, None, None
                else:
                    frame, achieved, capture_ms, end_clock_ms = _capture_at_offset(
                        page, tab_id, mount_at_ms, offset, sweep_clip,
                        read_end_clock=injector is not None or diag_data)
                probe_after = _panel_data_probe(page, tab_id) if diag_data else None
                if injector is not None:
                    injector.after_frame(frame_index)
                sweep_raw_frames.append(frame)
                sweep_achieved_ms.append(achieved)
                sweep_capture_ms.append(capture_ms)
                window = (None if frame is None or end_clock_ms is None else
                          (mount_at_ms + achieved, end_clock_ms))
                sweep_frame_windows_ms.append(window)
                if diag_data:
                    # Every COMPARED frame keeps its own PNG, not just the
                    # tick's first one: a pair verdict is unusable without
                    # both images of the pair it is about.
                    frame_png = None
                    if frame is not None:
                        frame_png = os.path.join(
                            diag_dir, f"tick-{i}-frame-{frame_index}.png")
                        with open(frame_png, "wb") as fh:
                            fh.write(frame)
                    diag_frames.append({
                        "frame_index": frame_index,
                        "target_offset_ms": offset,
                        "achieved_ms": achieved,
                        "capture_ms": capture_ms,
                        "window_ms": window,
                        "png": frame_png,
                        "probe_before": probe_before,
                        "probe_after": probe_after,
                    })
            after_sweep = _view_mount(page)
            seq_after_sweep = after_sweep["seq"] if after_sweep is not None else None

            sweep_arrays = [lib.png_bytes_to_array(f) if f is not None else None
                            for f in sweep_raw_frames]

            # issue #100 (review round 4, corrected round 5): pre-mount vs
            # sweep's own first frame, timeline only -- see the capture site
            # above for what precedes and why, and
            # lib.pre_mount_diagnostic_verdict's own docstring for the
            # precedence check itself.
            if tab_id == "timeline":
                pre_mount_arr = (lib.png_bytes_to_array(pre_mount_frame)
                                 if pre_mount_frame is not None else None)
                sweep_first_arr = sweep_arrays[0] if sweep_arrays else None
                sig = lib.pre_mount_diagnostic_verdict(
                    pre_mount_arr, sweep_first_arr, pre_mount_seq_after, mount["seq"])
                pre_mount_diagnostics.append({"tick": i, **sig})

            if footage_only:
                ratio, blink_note = None, FOOTAGE_NOT_EVALUATED
            else:
                ratio, blink_note = lib.blink_sweep_gate_verdict(
                    sweep_arrays, seq_before_sweep, seq_after_sweep)
            if blink_note:
                print(f"  note [{tab_id}] tick {i}: {blink_note}")
            if ratio is None:
                blink_not_measured.append({"tick": i, "reason": blink_note})
            else:
                blink_ratios.append(ratio)

            if (inject_blink_ms is not None and inject_tick is not None
                    and i == inject_tick):
                # issue #266: grade the geometry and the detection
                # separately, and read detection off the PAIRS the stimulus
                # itself created rather than off the sweep's max ratio --
                # max() is satisfied by any diff anywhere in the sweep,
                # including a failed capture (scored 1.0 by
                # sweep_consecutive_diff_ratios), so a self-test built on it
                # can report "caught" without the stimulus having been seen.
                stimulus = injector.stimulus if injector is not None else None
                stimulus_ok, stimulus_reason = lib.inject_stimulus_verdict(
                    stimulus, sweep_frame_windows_ms, inject_frame_index)
                pair_ratios = [r for r, _n in
                               lib.sweep_consecutive_diff_ratios(sweep_arrays)]
                # The stimulus appears between frames index-1 and index and
                # disappears between index and index+1, so BOTH boundary
                # pairs must see it (the second only exists when the target
                # is not the sweep's last frame). Review round 1: the
                # arithmetic lives in lib.inject_detection_verdict so it has
                # its own unit tests, like every other verdict here.
                caught, boundary, boundary_ratios = lib.inject_detection_verdict(
                    ratio, pair_ratios, inject_frame_index, blink_threshold)
                self_test_result = {
                    "tick": i, "ratio": ratio, "measured": ratio is not None,
                    "note": blink_note, "inject_blink_ms": inject_blink_ms,
                    "inject_frame_index": inject_frame_index,
                    "stimulus": stimulus,
                    "stimulus_ok": stimulus_ok,
                    "stimulus_reason": stimulus_reason,
                    "frame_windows_ms": sweep_frame_windows_ms,
                    "pair_ratios": pair_ratios,
                    "boundary_pair_indices": boundary,
                    "boundary_pair_ratios": boundary_ratios,
                    # #252 review round 1: this used to hardcode >= 0.5, so
                    # raising BLINK_THRESHOLD toward 0.5 (e.g. to 0.4) would
                    # leave the self-test reporting PASS while the gate it
                    # is supposed to defend had gone slack. The threshold is
                    # the tab's own blink_threshold -- see
                    # lib.inject_detection_verdict.
                    "caught": caught,
                }

            if diag_data:
                pair_ratios_diag = [r for r, _n in
                                    lib.sweep_consecutive_diff_ratios(sweep_arrays)]
                diag_rec = {
                    "tick": i,
                    "tab": tab_id,
                    "mount_seq": mount["seq"],
                    "mount_at_ms": mount_at_ms,
                    "tick_ts_ms": tick_ts_ms,
                    "seq_before_sweep": seq_before_sweep,
                    "seq_after_sweep": seq_after_sweep,
                    "sweep_ratio": ratio,
                    "sweep_note": blink_note,
                    "frames": diag_frames,
                    "pairs": lib.sweep_data_pair_reports(
                        diag_frames, pair_ratios_diag,
                        threshold=blink_threshold),
                }
                diag_ticks.append({k: v for k, v in diag_rec.items()
                                   if k != "frames"})
                with open(os.path.join(diag_dir, f"tick-{i}.json"), "w") as fh:
                    json.dump(diag_rec, fh, indent=1, sort_keys=True)
                    fh.write("\n")
                for pr in diag_rec["pairs"]:
                    print(f"  diag [{tab_id}] tick {i} pair {pr['pair_index']} "
                          f"({pr['frame_a_achieved_ms']:.0f}->"
                          f"{pr['frame_b_achieved_ms']:.0f}ms): "
                          f"ratio={pr['image_ratio']} "
                          f"verdict={pr['verdict']} "
                          f"data={pr['data'].get('reason') or ''}"
                          f"cells_differing={pr['data'].get('cells_differing')} "
                          f"sum {pr['data'].get('sum_a')}->{pr['data'].get('sum_b')}")

            if not footage_only:
                blink_sweep_ticks.append(
                    lib.build_sweep_tick_record(sweep_achieved_ms, sweep_arrays,
                                                target_offsets_ms=sweep_offsets,
                                                capture_ms=sweep_capture_ms,
                                                mount_seq=mount["seq"],
                                                panel_dims=sweep_dims))

            # The sweep's smallest-offset frame doubles as this tick's
            # artifact PNG (closest analogue to the old gating pair's
            # frame_a).
            if sweep_raw_frames and sweep_raw_frames[0] is not None:
                tick_path = os.path.join(tab_dir, f"tick-{i}.png")
                with open(tick_path, "wb") as f:
                    f.write(sweep_raw_frames[0])
                tick_paths.append(tick_path)

            if footage_only:
                footage_ticks += 1
            legend = page.evaluate(LEGEND_COLORS_JS)
            if legend:
                legend_ticks.append(legend)

            for e in guard.drain():
                if _is_pgwt_error(e):
                    # The app's own failure reporting -- never fails the tab
                    # by itself, but must be visible, not dropped silently
                    # (issue #93 review item 3).
                    print(f"  note [{tab_id}] tick {i}: {e}")
                    pgwt_errors.append(e)
                else:
                    console_errors.append(e)

        leak_after, leak_after_settle_s = _settled_leak_probe(page)
        color_violations = (lib.color_stability_violations(legend_ticks)
                            if legend_ticks else [])
        # blink_ratios holds only MEASURED ticks (blink_sweep_gate_verdict's
        # ratio=None/"not measured" ticks are excluded, tracked separately
        # in blink_not_measured) -- so an all-not-measured tab lands here as
        # an empty list, worst_blink=None, and no_blink_ok(None, ...) is
        # False by construction: an unmeasured signal fails the gate rather
        # than silently passing it (issue #193). build_tab_result ALSO
        # requires a minimum fraction of ticks to be measured at all
        # (review SHOULD-FIX) -- a tab that discarded most of its ticks as
        # not-measured must not report ok on the remainder alone.
        worst_blink = max(blink_ratios) if blink_ratios else None

        result = lib.build_tab_result(
            tab_id, render_ok, render_detail, len(tick_paths), console_errors,
            worst_blink, color_violations, leak_before, leak_after,
            artifacts={}, blink_threshold=blink_threshold,
            pgwt_console_errors=pgwt_errors,
            leak_before_settle_s=leak_before_settle_s,
            leak_after_settle_s=leak_after_settle_s,
            blink_pair_offsets_ms=blink_pair_offsets_ms,
            blink_sweep_ticks=blink_sweep_ticks,
            blink_not_measured=blink_not_measured,
            pre_mount_diagnostics=pre_mount_diagnostics,
            ttfp_ms=ttfp_ms)
        if replay_probe is not None:
            result = {"tab": tab_id, "ok": not console_errors and
                      lib.leak_probe_ok(leak_before) and lib.leak_probe_ok(leak_after),
                      "mode": "replay", "rendered": True,
                      "console_errors": console_errors,
                      "pgwt_console_errors": pgwt_errors,
                      "comparison": replay_result}
        if diag_data:
            result["diag_data"] = diag_ticks
        if self_test_result is not None:
            result["self_test"] = self_test_result
    except (SmokeFailure, AssertionError) as e:
        print(f"  FAIL [{tab_id}]: {e}", file=sys.stderr)
        result = lib.build_failed_tab_result(tab_id, str(e),
                                             ticks_observed=len(tick_paths),
                                             pgwt_console_errors=pgwt_errors,
                                             ttfp_ms=ttfp_ms,
                                             **_failed_tab_diagnostics(
                                                 page, guard, console_errors,
                                                 pgwt_errors, pre_mount_diagnostics))
    except Exception:
        traceback.print_exc()
        result = lib.build_failed_tab_result(
            tab_id, "unhandled exception (see stderr traceback)",
            ticks_observed=len(tick_paths), pgwt_console_errors=pgwt_errors,
            ttfp_ms=ttfp_ms,
            **_failed_tab_diagnostics(page, guard, console_errors, pgwt_errors,
                                      pre_mount_diagnostics))
    finally:
        video = page.video
        context.close()
        if video is not None:
            try:
                src = video.path()
                dst = os.path.join(tab_dir, f"{tab_id}.webm")
                os.replace(src, dst)
                video_path = dst
            except Exception as e:
                print(f"  note: could not finalize video for {tab_id}: {e}",
                      file=sys.stderr)

    if footage_only:
        _mark_footage_result(result, footage_ticks, ticks, video_path)
    result["artifacts"] = {"frames": tick_paths, "video": video_path}
    return result


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", help="page URL of an already-running UI "
                    "(phase 2: the real Go bridge)")
    ap.add_argument("--mock", action="store_true",
                    help="start tests/mock_server.py and walk against it")
    ap.add_argument("--footage-only", action="store_true",
                    help="record video with zero screenshots; image checks are "
                         "not evaluated in footage mode (requires a non-gating --out-dir)")
    ap.add_argument("--out-dir", default=None,
                    help=f"artifact directory (default {RESULTS_DIR} for "
                    f"--url, {MOCK_RESULTS_DIR} for --mock -- kept separate "
                    "so a local --mock run can never wipe real box evidence)")
    ap.add_argument("--ticks", type=int, default=lib.MIN_TICKS,
                    help=f"live ticks to observe per tab (default {lib.MIN_TICKS}; "
                    "the issue requires >= 6 -- never lower this in a gating run)")
    ap.add_argument("--first-data-timeout", type=float,
                    default=lib.FIRST_DATA_TIMEOUT_S,
                    help="no-data fail-safe in seconds (default "
                    f"{lib.FIRST_DATA_TIMEOUT_S}; issue #93 item 6)")
    ap.add_argument("--blink-threshold", type=float, default=lib.BLINK_THRESHOLD,
                    help="TEST-ONLY override for the deliberate-failure demo "
                    "(issue #93 phase-1 validation step). Gating runs "
                    "(tests/ui_live_smoke.sh) never pass this -- the default "
                    f"is the issue's stated 0.1%% ({lib.BLINK_THRESHOLD}).")
    ap.add_argument("--pgbench-pid", type=int, default=None,
                    help="PID of the background pgbench process (from "
                    "tests/ui_live_smoke.sh); checked alive at every tab "
                    "boundary, FAILS LOUDLY if it is gone (review item 2)")
    ap.add_argument("--workload-pid", type=int, default=None,
                    help="PID of the looping lock/sleep workload process "
                    "(from tests/ui_live_smoke.sh); same liveness check as "
                    "--pgbench-pid")
    ap.add_argument("--inject-blink-ms", type=int, default=None,
                    help="issue #252 self-test (OFF BY DEFAULT -- never "
                    "passed by a real gating run: tests/ui_live_smoke.sh, "
                    "demo_rehearsal.py). Overlays --inject-tab's panel for "
                    "this many ms during its --inject-tick'th sweep and "
                    "asserts the gate still catches it -- proof a capture-"
                    "path change did not blind the gate to a real blink. "
                    "issue #266: this is the MINIMUM measured on-screen "
                    "dwell, counted from the overlay's own first presented "
                    "frame; the overlay is removed at the later of that "
                    "deadline and --inject-frame's capture call returning. "
                    "Keep it below the gap to the NEXT sweep offset (500ms "
                    "for the default offsets): a dwell that runs into the "
                    "next frame's capture makes the geometry unprovable, "
                    "and inject_stimulus_verdict then refuses -- loudly, "
                    "with the overlapping frame named.")
    ap.add_argument("--inject-tab", default="sessions",
                    help="which tab's sweep gets the synthetic blink "
                    "(default sessions -- issue #252's own subject); "
                    "ignored unless --inject-blink-ms is set")
    ap.add_argument("--inject-tick", type=int, default=2,
                    help="which 1-based tick of --inject-tab gets the "
                    "synthetic blink (default 2); ignored unless "
                    "--inject-blink-ms is set")
    ap.add_argument("--inject-frame", type=int,
                    default=lib.INJECT_FRAME_INDEX,
                    help="issue #266: which 0-based frame of the injected "
                    "tick's offset sweep the synthetic overlay is made "
                    "visible for (default "
                    f"{lib.INJECT_FRAME_INDEX}, the "
                    f"{lib.SWEEP_OFFSETS_MS[lib.INJECT_FRAME_INDEX]}ms "
                    "offset). Must be >= 1: the self-test grades the "
                    "consecutive pair the stimulus's own appearance "
                    "creates, so a preceding clean frame has to exist; "
                    "ignored unless --inject-blink-ms is set")
    ap.add_argument("--inject-blink-frac", type=float,
                    default=DEFAULT_INJECT_BLINK_FRAC,
                    help="review round 1: fraction of --inject-tab's panel "
                    "height the synthetic overlay covers (top slice, full "
                    f"width; default {DEFAULT_INJECT_BLINK_FRAC} -- a full "
                    "panel, frac=1.0, only proves a GROSS blink is caught, "
                    "1000x over BLINK_THRESHOLD); ignored unless "
                    "--inject-blink-ms is set")
    # issue #384 -- the blink-sweep DATA diagnostic. Two flags, both
    # strictly additive and both OFF for every gating caller
    # (tests/ui_live_smoke.sh, demo_rehearsal.py pass neither):
    #
    #   --diag-data: bracket every sweep frame with a read of what the
    #     panel's chart is actually rendering (_panel_data_probe), keep EVERY
    #     compared frame's PNG, and write <tab>/diag/tick-N.json with
    #     lib.sweep_data_pair_reports' per-pair verdict. This is what makes
    #     "did the data change between these two frames?" answerable at all;
    #     without it the harness kept one PNG per tick and no data.
    #   --tabs: walk only the named tabs. The histogram divergence is in the
    #     "late" phase, so it needs ~12 ticks -- on ONE tab, not on eleven.
    #     Guarded by lib.diag_out_dir_ok: a tab subset may not write the
    #     GATING results dir, because write_summary's `ok` is computed over
    #     whatever tabs are present and tests/run_all.sh reads that file.
    ap.add_argument("--diag-data", action="store_true",
                    help="diagnostic: record each sweep frame's underlying "
                         "chart data (bracketing probes), its page-clock "
                         "window and its own PNG; off by default, costs two "
                         "extra round trips per frame")
    ap.add_argument("--tabs", default=None,
                    help="diagnostic: comma-separated subset of tabs to walk "
                         "(requires a non-gating --out-dir)")
    ap.add_argument("--viewport-width", type=int, default=DEMO_VIEWPORT_WIDTH,
                    help="issue #252: this CI-tier walk now defaults to the "
                    "SAME viewport demo_rehearsal.py pins, so a PR gate "
                    "renders the demo's actual layout/raster geometry and "
                    "acts as the tripwire that makes a future scale=\"css\" "
                    "removal visible in capture_ms (default "
                    f"{DEMO_VIEWPORT_WIDTH}; see run_tab()'s own docstring)")
    ap.add_argument("--viewport-height", type=int, default=DEMO_VIEWPORT_HEIGHT,
                    help=f"issue #252 (default {DEMO_VIEWPORT_HEIGHT})")
    ap.add_argument("--device-scale-factor", type=float,
                    default=DEMO_DEVICE_SCALE_FACTOR,
                    help=f"issue #252 (default {DEMO_DEVICE_SCALE_FACTOR})")
    args = ap.parse_args()

    if bool(args.url) == bool(args.mock):
        ap.error("exactly one of --url or --mock is required")

    if args.footage_only and (args.diag_data or args.inject_blink_ms is not None):
        ap.error("--footage-only cannot be combined with --diag-data or --inject-blink-ms")
    if args.footage_only:
        if not args.out_dir:
            ap.error("--footage-only requires a non-gating --out-dir")
        gate_dir = os.path.realpath(RESULTS_DIR)
        footage_dir = os.path.realpath(args.out_dir)
        if os.path.commonpath([gate_dir, footage_dir]) in (gate_dir, footage_dir):
            ap.error("--footage-only requires an --out-dir separate from the gating results tree")

    mock_proc = None
    if args.mock:
        mock_proc = start_mock_server(MOCK_PORT)
        url = mock_app_url(MOCK_PORT)
    else:
        url = args.url

    out_dir = args.out_dir or (MOCK_RESULTS_DIR if args.mock else RESULTS_DIR)

    # issue #384: validated BEFORE reset_output_dir() deletes anything -- a
    # refused invocation must not have already wiped the gating directory it
    # was being refused permission to write.
    only_tabs = [t.strip() for t in args.tabs.split(",") if t.strip()] if args.tabs else []
    unknown = [t for t in only_tabs if t not in lib.TABS]
    if unknown:
        print(f"--tabs names unknown tab(s) {unknown}; known: {list(lib.TABS)}",
              file=sys.stderr)
        return 2
    ok_dir, why_dir = lib.diag_out_dir_ok(only_tabs, out_dir, RESULTS_DIR)
    if not ok_dir:
        print(why_dir, file=sys.stderr)
        return 2

    # Every artifact under out_dir must belong to THIS run -- delete and
    # recreate it fresh before any tab runs, so a stale tick-N.png from an
    # earlier invocation (mock, real, or a previous box-check) can never
    # survive into this run's summary.json (issue #93 ui-reviewer finding).
    lib.reset_output_dir(out_dir)

    # This run's marker (issue #93 review item 3): tests/run_all.sh exports
    # PGWT_RUN_MARKER (its own start timestamp) and re-emits this run's
    # verdict only if run.id matches it -- tests/results is excluded from
    # box-check.sh's up-rsync, so the box otherwise keeps the LAST run's
    # ui_live/ between invocations, and a smoke that died before the walk
    # would have run_all.sh print a stale, unrelated PASS/FAIL as if it were
    # this run's. Written IMMEDIATELY after reset_output_dir so it survives
    # even a failure on the very first tab. Falls back to the wall clock for
    # --mock/manual runs, which have no run_all.sh marker to match.
    run_marker = os.environ.get("PGWT_RUN_MARKER", str(int(time.time())))
    with open(os.path.join(out_dir, "run.id"), "w") as f:
        f.write(run_marker)

    _assert_workload_alive(args.pgbench_pid, args.workload_pid, "before the walk")
    results = []
    # review round 1 finding 3: self-test mode has no periodic caller
    # because walking all 11 tabs just to exercise one injected tab is too
    # expensive to wire into box-check on every run. Self-test mode walks
    # ONLY --inject-tab -- the rest of the tabs contribute nothing to
    # self_test_ok (main()'s own exit-code logic below only ever looks at
    # args.inject_tab's result), so skipping them is a pure cost cut, not a
    # narrower proof. A real gating invocation never sets --inject-blink-ms
    # (tests/ui_live_smoke.sh, demo_rehearsal.py), so lib.TABS is still
    # walked in full there.
    tabs_to_walk = ([args.inject_tab] if args.inject_blink_ms is not None
                    else lib.TABS)
    if only_tabs:
        tabs_to_walk = only_tabs
    _require_playwright()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            try:
                for tab_id in tabs_to_walk:
                    print(f"=== {tab_id} ===")
                    inject_here = (args.inject_blink_ms is not None
                                   and tab_id == args.inject_tab)
                    result = run_tab(browser, tab_id, url, out_dir,
                                     args.ticks, args.first_data_timeout,
                                     args.blink_threshold,
                                     viewport={"width": args.viewport_width,
                                              "height": args.viewport_height},
                                     device_scale_factor=args.device_scale_factor,
                                     inject_blink_ms=(args.inject_blink_ms
                                                      if inject_here else None),
                                     inject_tick=(args.inject_tick
                                                  if inject_here else None),
                                     inject_blink_frac=args.inject_blink_frac,
                                     inject_frame_index=args.inject_frame,
                                     diag_data=args.diag_data,
                                     footage_only=args.footage_only)
                    results.append(result)
                    _assert_workload_alive(args.pgbench_pid, args.workload_pid,
                                           f"after tab {tab_id!r}")
                    kf_line = lib.known_failing_report_line(tab_id, result["ok"])
                    status = kf_line if kf_line else ("PASS" if result["ok"] else "FAIL")
                    if args.footage_only:
                        status = ("FOOTAGE RECORDED" if result["footage_complete"] else "FOOTAGE FAILED")
                        print(f"  image checks: {FOOTAGE_NOT_EVALUATED}")
                    print(f"  {status} [{tab_id}] "
                          f"ticks={result['ticks_observed']} "
                          f"rendered={result['rendered']}")
                    # frame_spacing is REPORTING-ONLY (see
                    # ui_live_smoke_lib.FRAME_SPACING_DRIFT_BOUND_MS): it no
                    # longer fails the tab, so without this line a drift
                    # regression would be invisible to anyone reading the log
                    # and would only exist inside summary.json. Printed as a
                    # note, never as a FAIL, and never consulted for `status`.
                    spacing = result.get("frame_spacing") or {}
                    if spacing.get("ok") is False:
                        print(f"  note [{tab_id}] frame_spacing "
                              f"(REPORTING-ONLY, does not gate): "
                              f"{spacing.get('detail')}")
            finally:
                browser.close()
    finally:
        if mock_proc is not None:
            stop_mock_server(mock_proc)

    summary_path = os.path.join(out_dir, "summary.json")
    summary = lib.write_summary(summary_path, results)
    if args.footage_only:
        summary.update(footage_only=True, ok=False,
                       detail=FOOTAGE_NOT_EVALUATED,
                       footage_complete=bool(results) and all(
                           r["footage_complete"] for r in results))
        with open(summary_path, "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"  footage: {'RECORDED' if summary['footage_complete'] else 'FAILED'}")
        print(f"  image checks: {FOOTAGE_NOT_EVALUATED}")
        print(f"  summary: {summary_path}")
        return 0 if summary["footage_complete"] else 1

    # issue #252 self-test (off unless --inject-blink-ms was given): pull
    # the injected tab's self_test verdict up to a top-level summary.json
    # key, so it is found the same way every other run-level fact is
    # (summary["self_test"]), not buried one level down under a specific
    # tab a reader has to already know to look at.
    self_test = None
    if args.inject_blink_ms is not None:
        tab_result = summary["tabs"].get(args.inject_tab)
        self_test = (tab_result or {}).get("self_test")
        summary["self_test"] = self_test
        with open(summary_path, "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")

    # issue #384: roll every tick's per-pair reports into the one sentence
    # the investigation needs. Reporting only -- summary["ok"] is untouched,
    # and main()'s exit code below never reads it.
    if args.diag_data:
        all_pairs = []
        for tab_id in tabs_to_walk:
            for tick in ((summary["tabs"].get(tab_id) or {}).get("diag_data") or []):
                all_pairs.extend(tick.get("pairs") or [])
        diag_v, diag_detail = lib.diag_verdict(all_pairs)
        summary["diag_verdict"] = {"verdict": diag_v, "detail": diag_detail,
                                   "pairs_examined": len(all_pairs)}
        with open(summary_path, "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"  DIAG VERDICT: {diag_v} -- {diag_detail} "
              f"({len(all_pairs)} pairs examined)")

    print()
    print("════════════════════════════════════════")
    print("  UI LIVE SMOKE SUMMARY")
    print("════════════════════════════════════════")
    # self-test mode only ever walks tabs_to_walk (== [args.inject_tab]),
    # not lib.TABS -- printing every OTHER tab as FAIL here would claim a
    # result for tabs that were never even visited.
    for tab_id in tabs_to_walk:
        r = summary["tabs"].get(tab_id)
        kf_line = lib.known_failing_report_line(tab_id, r["ok"]) if r else None
        label = kf_line if kf_line else ("PASS" if r and r["ok"] else "FAIL")
        print(f"  {label} {tab_id}")
    print(f"  summary: {summary_path}")
    print(f"  overall: {'PASS' if summary['ok'] else 'FAIL'}"
          + (f" (failed: {', '.join(summary['failed_tabs'])})"
             if summary["failed_tabs"] else "")
          + (f" (known-failing: {', '.join(summary['known_failing_tabs'])})"
             if summary["known_failing_tabs"] else "")
          + (f" (xpass: {', '.join(summary['xpass_tabs'])})"
             if summary["xpass_tabs"] else ""))

    if args.inject_blink_ms is not None:
        # The self-test's own exit code supersedes the normal `summary["ok"]`
        # gate here: --inject-tab is DELIBERATELY broken by the overlay, so
        # summary["ok"] being False is expected and uninformative in this
        # mode -- what matters is whether the gate actually caught the
        # injected blink AND failed that tab for it, asserting the FULL tab
        # verdict (not just the image-diff helper in isolation).
        tab_ok = (summary["tabs"].get(args.inject_tab) or {}).get("ok")
        caught = self_test is not None and self_test.get("caught")
        # issue #266: the stimulus geometry is its own, separately reported
        # gate. A self-test can only speak about the DETECTOR when the
        # overlay provably covered exactly one sweep frame; when it did not,
        # this REFUSES (exit 1 with the reason) instead of either passing on
        # an unverified premise or blaming the detector for a blink that was
        # never on screen. Both conditions must hold -- stimulus_ok alone is
        # a harness check, caught alone is the old unverified assertion.
        stimulus_ok = self_test is not None and self_test.get("stimulus_ok")
        stimulus_reason = (self_test or {}).get("stimulus_reason")
        self_test_ok = bool(caught) and bool(stimulus_ok) and tab_ok is False
        detail = (f"self_test={self_test}" if self_test is not None else
                  f"self_test never recorded -- --inject-tick={args.inject_tick} "
                  f"was never reached on tab {args.inject_tab!r} "
                  f"(ticks={args.ticks})")
        print(f"  SELF-TEST {'PASS' if self_test_ok else 'FAIL'}: "
              f"the stimulus geometry was "
              f"{'PROVEN' if stimulus_ok else 'NOT proven'}"
              + (f" ({stimulus_reason})" if not stimulus_ok and stimulus_reason
                 else "")
              + f", the injected blink was "
              f"{'caught' if caught else 'NOT caught'} "
              f"on pairs {(self_test or {}).get('boundary_pair_indices')} "
              f"(ratios {(self_test or {}).get('boundary_pair_ratios')}) "
              f"and the tab verdict was {'FAIL' if tab_ok is False else tab_ok!r} "
              f"({detail})")
        return 0 if self_test_ok else 1

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
