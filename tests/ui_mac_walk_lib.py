#!/usr/bin/env python3
"""ui_mac_walk_lib.py -- pure helpers for tests/ui_mac_walk.py (the Mac-side
demo-path walk: real Chrome + Go bridge on the Mac, ssh to a VM running the
daemon + PG). Every function here is a plain parser or verdict builder --
no subprocess, no network, no Playwright -- so it is testable anywhere with
plain python3 (tests/test_ui_mac_walk_lib.py), independent of macOS itself.

docs/DEMO_REHEARSAL_CRITERIA.md pre-registers that a tick measured under
non-normal Mac memory pressure is VOID (infrastructure noise), never a
product failure. That only means anything if every sample is attributable
to the wall-clock window it was taken in -- see window_void_reason.
"""
import re

# app.js's AAS auto-refresh cadence (5s) -- the same constant every other
# live-mode script in this repo (SWEEP_OFFSETS_MS's own comments, the
# "5s tick cadence" note in ui_live_smoke.sh) is written against.
TICK_S = 5.0

# Criteria doc §5: "every time-axis tab's newest bucket is within 2 ticks of
# wall clock". This walk cannot read a per-chart-type bucket timestamp for
# all 11 panel types in one pass (see ui_mac_walk.py's module docstring for
# what it measures instead, and why) -- FRESHNESS_MAX_AGE_S is still the
# criteria's own bound, applied to the proxy signal this walk actually has.
FRESHNESS_MAX_TICKS = 2
FRESHNESS_MAX_AGE_S = TICK_S * FRESHNESS_MAX_TICKS

# sysctl kern.memorystatus_vm_pressure_level values (xnu, see
# <mach/vm_statistics.h> semantics as memory_pressure(1)'s -l warn/critical
# flags name them): 1 = normal, 2 = warn, 4 = critical.
PRESSURE_LEVEL_NAMES = {1: "normal", 2: "warn", 4: "critical"}


def parse_pressure_level(sysctl_output):
    """Parses `sysctl kern.memorystatus_vm_pressure_level` stdout
    ("kern.memorystatus_vm_pressure_level: N") -> int, or None if the line
    is missing/unparseable -- e.g. sysctl exited non-zero, or a future macOS
    changes the output format. The caller must treat None as "could not
    measure", never silently as "normal" (see sample_is_normal)."""
    m = re.search(r':\s*(\d+)\s*$', sysctl_output or "")
    return int(m.group(1)) if m else None


def parse_free_percent(memory_pressure_output):
    """Parses `memory_pressure -Q` stdout ("System-wide memory free
    percentage: NN%") -> int, or None if absent/unparseable."""
    m = re.search(r'free percentage:\s*(\d+)\s*%', memory_pressure_output or "")
    return int(m.group(1)) if m else None


def pressure_level_name(level):
    if level is None:
        return "unmeasured"
    return PRESSURE_LEVEL_NAMES.get(level, f"unknown({level})")


def sample_is_normal(sample):
    """A sample counts as NORMAL only if the pressure level was actually
    measured AND equals 1 (xnu "normal"). An unmeasured sample
    (pressure_level is None -- sysctl missing/failed) is NEVER treated as
    normal: the criteria doc's whole point is that a non-normal tick must be
    attributable and excluded, and an unattributable sample must fail
    closed the same way a measured-bad one does, not be silently counted as
    clean by defaulting missing data to 'fine'."""
    return sample.get("pressure_level") == 1


def bracketing_samples(samples, start_ts, end_ts):
    """Returns (before, after): the most recent sample at or before
    start_ts, and the earliest sample at or after end_ts. Either is None if
    no such sample exists. Does not assume `samples` is sorted (the caller
    may be handing over a live sampler's accumulated list as-is)."""
    before = None
    after = None
    for s in samples:
        ts = s.get("ts")
        if ts is None:
            continue
        if ts <= start_ts and (before is None or ts > before["ts"]):
            before = s
        if ts >= end_ts and (after is None or ts < after["ts"]):
            after = s
    return before, after


def sample_cadence_stats(samples):
    """Returns (mean_gap, max_gap) between consecutive samples (sorted by
    ts), or (None, None) if fewer than 2 timestamped samples exist. Used to
    derive a bracketing distance bound from the sampler's OWN MEASURED
    behavior, never its configured interval alone (2026-09-28 review: a
    thread that cannot keep its configured cadence -- subprocess cost, GC,
    system contention -- makes a bound picked from the config a fiction)."""
    ts = sorted(s["ts"] for s in samples if s.get("ts") is not None)
    if len(ts) < 2:
        return None, None
    gaps = [ts[i + 1] - ts[i] for i in range(len(ts) - 1)]
    return sum(gaps) / len(gaps), max(gaps)


def window_void_reason(samples, start_ts, end_ts, max_gap_s):
    """Returns a human-readable reason if this [start_ts, end_ts] window
    cannot be certified as measured-under-normal-pressure, else None.

    BRACKETING, not "a sample strictly inside the window" (changed
    2026-09-28): found live that a tab window can be sub-second (0.16-0.88s
    measured) while the environment sampler ticks on a much coarser cadence
    by design -- requiring a sample INSIDE such a window discarded a
    perfectly good, verifiably-quiet run (loadavg 1.37, 56% free) as
    unattributable, which is exactly the false "cannot tell" this function
    exists to avoid. A window is judged by the environment ENCLOSING it
    instead: the most recent sample at/before the window start and the
    earliest sample at/after the window end.

    max_gap_s is REQUIRED, no default (2026-09-28 review, blocker): without
    a distance bound, two normal samples an hour apart certify everything
    between them as quiet, which defeats the whole check -- two calm
    readings either side of two minutes of real load would score clean.
    Callers must derive it from sample_cadence_stats() on their OWN
    measured samples, not from the sampler's configured interval (which the
    review also found this Mac's subprocess-based sampler does not
    reliably keep).

    Both bracketing samples must exist, each within max_gap_s of its own
    window edge, AND both must read normal, for the window to pass -- a
    missing bracket, or one farther than max_gap_s away, is the real
    "cannot attribute" case and the window still voids. Never speeds up
    sampling or widens the window to manufacture an in-window sample --
    the fix is in how existing samples are read, not in gathering more of
    them to paper over the same question.
    """
    before, after = bracketing_samples(samples, start_ts, end_ts)
    if before is None or after is None:
        side = "before" if before is None else "after"
        return (f"no mac sample {side} this window's "
                f"{'start' if side == 'before' else 'end'} -- cannot attribute")
    before_gap = start_ts - before["ts"]
    after_gap = after["ts"] - end_ts
    if before_gap > max_gap_s or after_gap > max_gap_s:
        return (f"bracketing sample too far from window edge "
                f"(before={before_gap:.2f}s, after={after_gap:.2f}s, "
                f"bound={max_gap_s:.2f}s) -- cannot attribute")
    bad = [s for s in (before, after) if not sample_is_normal(s)]
    if bad:
        names = sorted({pressure_level_name(s.get("pressure_level")) for s in bad})
        return (f"{len(bad)}/2 bracketing mac samples non-normal "
                f"({', '.join(names)})")
    return None


def freshness_ok(age_s, max_age_s=FRESHNESS_MAX_AGE_S):
    if age_s is None:
        return False, "freshness not measured"
    if age_s < 0:
        return False, f"negative age ({age_s:.1f}s) -- clock skew or bad sample"
    if age_s > max_age_s:
        return False, f"newest data is {age_s:.1f}s old (> {max_age_s:.1f}s bound)"
    return True, f"{age_s:.1f}s old (<= {max_age_s:.1f}s bound)"


def freshness_status(age_s, max_age_s=FRESHNESS_MAX_AGE_S):
    """TRI-STATE, not the boolean freshness_ok() returns: 'unmeasured' |
    'failed' | 'ok'. Found live (2026-09-28 review): a real run's verdict
    aggregated to `ok: true` while every one of its 11 tabs carried
    freshness age_s=None -- because freshness_ok(None) collapses
    "could not measure" into the same False as a genuine staleness
    failure, and nothing upstream then distinguished the two before
    deciding the tab (and the whole run) passed. An unmeasured criterion
    is not a satisfied one, and it is not the same claim as a measured
    failure either -- a reader needs to tell "the product is stale" apart
    from "we could not tell"."""
    if age_s is None:
        return "unmeasured"
    if age_s < 0 or age_s > max_age_s:
        return "failed"
    return "ok"


def verdict_status(counted, ok):
    """Tri-state overall status, alongside the raw counted/ok booleans
    (kept for backward compatibility): 'unmeasured' (majority-void --
    the run could not be attributed to the Mac's environment at all),
    'failed' (attributable, but at least one criterion did not pass --
    includes an unmeasured PER-TAB criterion like freshness, since that
    is folded into each tab's own `ok` before this is computed), or
    'satisfied' (attributable and every criterion passed). Never
    conflates "could not tell" with either a real pass or a real
    failure."""
    if not counted:
        return "unmeasured"
    return "satisfied" if ok else "failed"


def build_tab_entry(tab_id, rendered, render_detail, ttfp_s, console_errors,
                     pgwt_console_errors, freshness_age_s, mac_void_reason,
                     wall_start=None, wall_end=None, bracket_before=None,
                     bracket_after=None, render_state=None):
    """One tab's row in the verdict. `ok` is false if the tab didn't render,
    threw a real (non-'[pgwt]') console error, OR its window is void for
    Mac-side infrastructure reasons -- void_reason is reported SEPARATELY so
    a reader can tell "the product broke" from "the Mac was busy building
    something else at the time" (criteria doc: void is infrastructure, not
    a product failure) rather than collapsing both into one boolean.

    wall_start/wall_end (epoch seconds) and bracket_before/bracket_after
    (the actual mac_samples entries window_void_reason bracketed against,
    or None) are persisted here -- found live 2026-09-28: their absence is
    exactly why an earlier run's void verdict could not be re-derived from
    its own saved summary.json afterward. All four default to None so
    existing callers that predate this (tests/ui_mac_walk.py's Chrome walk)
    keep working unchanged; a future run can be re-judged from stored data
    without re-running only if these are populated.

    `ok` now REQUIRES freshness_status == "ok" too (fixed 2026-09-28,
    review blocker): found live that a real run's verdict aggregated to
    ok: true while freshness was unmeasured (age_s: None) on all 11
    tabs -- freshness was computed and stored, but never gated this
    field, so an unmeasured criterion silently passed through. An
    unmeasured criterion is never treated as satisfied.

    render_state (2026-09-28, review correction): 'data' | 'empty' |
    'failed' | None (default, for callers that don't distinguish).
    waterfall/scatter legitimately render a documented EMPTY state
    (".loading" div, e.g. "No executions for selected range") when the
    trace window has no completed executions -- no canvas is ever created
    for that state BY DESIGN (waterfall.js:215, exec-scatter.js:70), so it
    is not the same result as a tab that actually failed to render, and
    `rendered` alone collapsed the two. render_state is reported
    separately so a reader can tell them apart; it does not itself gate
    `ok` (that stays driven by `rendered`, which the caller sets True for
    BOTH 'data' and 'empty' -- an empty panel showing its own correct
    empty-state message is a legitimate, working panel)."""
    fresh_ok, fresh_detail = freshness_ok(freshness_age_s)
    fresh_status = freshness_status(freshness_age_s)
    ok = (bool(rendered) and not console_errors and mac_void_reason is None
          and fresh_status == "ok")
    return {
        "tab": tab_id,
        "rendered": bool(rendered),
        "render_detail": render_detail,
        "render_state": render_state,
        "ttfp_s": ttfp_s,
        "console_errors": list(console_errors),
        "pgwt_console_errors": list(pgwt_console_errors),
        "freshness": {"age_s": freshness_age_s, "ok": fresh_ok,
                       "status": fresh_status, "detail": fresh_detail},
        "void_reason": mac_void_reason,
        "wall_start": wall_start,
        "wall_end": wall_end,
        "bracket_before": bracket_before,
        "bracket_after": bracket_after,
        "ok": ok,
    }


def run_id_matches(verdict, expected_run_id):
    """Criteria §1: "the verdict carries this invocation's run.id and it is
    asserted, not assumed." Pure equality check factored out so the
    assertion itself has a test independent of file I/O -- a stale verdict
    dict from a previous invocation must not satisfy a caller who checks
    this."""
    return isinstance(verdict, dict) and verdict.get("run", {}).get("id") == expected_run_id


# ── connection-drop / recovery classification ────────────────────────────
# web/static/app.js's setStatus(text, cls) sets #status.className to
# "status " + cls, cls in {"connecting", "connected", "error", "degraded"}
# (index.html's initial markup, app.js lines ~372-546). Used by
# ui_mac_walk_reconnect.py's screenshots-and-timings drop/recovery
# measurement -- kept here, not there, so the parsing has a test
# independent of Playwright/a live browser.
CONNECTION_CLASSES = ("connecting", "connected", "error", "degraded")


def classify_status(class_attr):
    """Parses #status's className ("status " + cls) into the connection
    state word app.js assigned. Returns None for a class string carrying
    none of the known words -- an unrecognised/empty class must never be
    silently read as 'connected' (that would report a drop as a recovery)."""
    if not class_attr:
        return None
    words = class_attr.split()
    for w in CONNECTION_CLASSES:
        if w in words:
            return w
    return None


def is_connected_state(class_attr):
    return classify_status(class_attr) == "connected"


def version_skew_from_snapshot(snapshot):
    """snapshot: {"present": bool, "visible": bool, "text": str, "cls": str}
    read from #version-skew (web/static/app.js renderVersionSkew /
    web/static/lib/format.js versionSkew). Returns None when no skew is
    showing -- the element is absent, or present but hidden
    (el.style.display = 'none', app.js's no-skew path) -- else a dict with
    the level ('warn'/'error', parsed from the element's own className:
    app.js sets className = 'version-skew ' + skew.level) and the raw badge
    text.

    A walk that never reads this element can only tell a version-skewed run
    from a clean one by a human looking at a screenshot; this is what makes
    it machine-checkable instead."""
    if not snapshot or not snapshot.get("present") or not snapshot.get("visible"):
        return None
    cls = snapshot.get("cls") or ""
    level = None
    for w in ("warn", "error"):
        if w in cls.split():
            level = w
    return {"level": level, "text": snapshot.get("text")}


def screen_locked_from_ioreg(ioreg_dict):
    """Extracts CGSSessionScreenIsLocked from the parsed
    `ioreg -n Root -d1 -a` plist (IOConsoleUsers[0].CGSSessionScreenIsLocked
    -- confirmed root cause, 2026-09-28: macOS refuses a fullscreen Space
    transition -- session.fullscreen()'s POST /session/{id}/window/fullscreen
    -- when the console session is locked, which is why it failed with an
    opaque "unknown error" independent of any code here). Returns True/
    False, or None if the shape is missing/unexpected -- fail closed, the
    caller must never treat None as "unlocked"."""
    try:
        users = ioreg_dict.get("IOConsoleUsers")
        if not users:
            return None
        return bool(users[0].get("CGSSessionScreenIsLocked", False))
    except (AttributeError, IndexError, TypeError):
        return None


def display_asleep_from_system_profiler(sp_dict):
    """Extracts whether ANY display is asleep from the parsed
    `system_profiler SPDisplaysDataType -json` output (a display's
    'spdisplays_asleep' == 'spdisplays_yes'). Returns True if any display is
    asleep, False if at least one display was found and none are asleep,
    None if the shape is missing/unexpected (no GPUs/displays listed at
    all) -- fail closed, same contract as screen_locked_from_ioreg."""
    try:
        gpus = sp_dict.get("SPDisplaysDataType")
        if not gpus:
            return None
        any_display = False
        for gpu in gpus:
            for disp in gpu.get("spdisplays_ndrvs", []):
                any_display = True
                if disp.get("spdisplays_asleep") == "spdisplays_yes":
                    return True
        return False if any_display else None
    except (AttributeError, TypeError):
        return None


def effective_zoom(target_width, achieved_width):
    """Recovers Safari's effective page zoom factor from a viewport
    assertion mismatch: target_width / achieved_width. Safari's persisted
    per-hostname zoom SHRINKS the achieved CSS viewport by dividing it by
    the zoom factor (measured 2026-09-28: ~115% zoom on the hostname
    "localhost" turned a 1710px target into ~1486px achieved, 1710/1486 ~=
    1.15), so this recovers the zoom to report even when the harness has
    no way to set it directly (no W3C WebDriver "set zoom" endpoint).
    Returns None when achieved_width is falsy (0/None/missing) -- "could
    not compute" is not the same claim as "zoom is 1.0", and this also
    avoids a ZeroDivisionError on a genuinely broken viewport read."""
    if not achieved_width:
        return None
    return target_width / achieved_width


def walk_precondition_reason(screen_locked, display_asleep):
    """Given the two booleans (or None if unmeasured -- see the two
    functions above), returns a reason string if the timed walk must
    REFUSE to start (never attempted, never retried in a loop -- coordinator
    2026-09-28: "an error naming the real cause is worth more than five
    minutes of opaque retries"), else None. Fail closed: an unmeasured
    (None) precondition refuses too -- "could not tell" is not "assume it's
    fine". Lock is checked before sleep because it is the actual macOS
    restriction; display-asleep is downstream of it (the display sleeps,
    then the session locks) but still checked/reported on its own, since a
    display can be asleep on an otherwise-unlocked session too."""
    if screen_locked is None or display_asleep is None:
        return ("could not determine screen lock / display sleep state "
                "-- refusing to guess")
    if screen_locked:
        return ("screen session is locked (CGSSessionScreenIsLocked) -- "
                "macOS refuses fullscreen automation on a locked session")
    if display_asleep:
        return "display is asleep (system_profiler spdisplays_asleep)"
    return None


def majority_void(tabs):
    """True if MORE THAN HALF of `tabs` (build_tab_entry dicts) are void
    (Mac-side infrastructure noise, not a product signal) -- coordinator's
    explicit rule (2026-09-28): "if more than half the ticks void, the run
    does not count and say so rather than reporting the survivors." An
    empty list counts as majority-void too (nothing measured is the same
    as "does not count", not vacuously fine) -- fail closed, same as
    build_verdict's own empty/all-void handling. A TIE (exactly half) does
    NOT count as majority -- the rule is "more than half"."""
    if not tabs:
        return True
    void_n = sum(1 for t in tabs if t.get("void_reason"))
    return void_n * 2 > len(tabs)


def build_verdict(run_id, tabs, mac_samples, notes=None):
    """Top-level verdict -- same shape discipline as
    tests/results/demo_rehearsal/summary.json: run.id present (checked by
    run_id_matches, not assumed), a per-tab reason, machine-readable, never
    prose-only.

    NOT a demo-rehearsal-criteria gate: this walk is a first honest
    measurement of a path never exercised before, not part of the
    pre-registered clean-rehearsal sequence. `ok` here means "every
    attributable tab rendered cleanly", nothing more -- a reader must look
    at void_tabs before treating a False as a product regression, and an
    empty (all-void) run is explicitly NOT ok (a run that measured nothing
    must not report "clean" by vacuous truth over an empty list)."""
    non_void = [t for t in tabs if t.get("void_reason") is None]
    ok = bool(tabs) and len(non_void) > 0 and all(t["ok"] for t in non_void)
    void_tabs = [t["tab"] for t in tabs if t.get("void_reason")]
    counted = not majority_void(tabs)
    return {
        "run": {"id": run_id},
        "ok": ok,
        "note": "measurement only -- not a demo-rehearsal-criteria gate (docs/DEMO_REHEARSAL_CRITERIA.md)",
        "tabs": {t["tab"]: t for t in tabs},
        "tab_order": [t["tab"] for t in tabs],
        "void_tabs": void_tabs,
        "void_count": len(void_tabs),
        "tab_count": len(tabs),
        # coordinator's explicit rule (2026-09-28): more than half void ->
        # the run does not count as a measurement, regardless of what the
        # surviving non-void tabs show.
        "counted": counted,
        "mac_samples": mac_samples,
        "notes": notes or [],
        # Tri-state summary of the two booleans above (review blocker,
        # 2026-09-28) -- "unmeasured" (majority-void), "failed" (counted
        # but a criterion did not pass, INCLUDING an unmeasured per-tab
        # criterion like freshness, which is folded into `ok` above via
        # each tab's own `ok`), or "satisfied".
        "status": verdict_status(counted, ok),
    }
