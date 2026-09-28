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


def samples_in_window(samples, start_ts, end_ts):
    return [s for s in samples if start_ts <= s.get("ts", float("-inf")) <= end_ts]


def window_void_reason(samples, start_ts, end_ts):
    """Returns a human-readable reason if this [start_ts, end_ts] window
    cannot be certified as measured-under-normal-pressure, else None.

    Two distinct void cases, both reported (never merged into a generic
    "not ok"):
      - the window has NO mac samples at all (sampler wasn't running, or the
        window was too short to catch a sample) -- unattributable, not
        "assumed fine";
      - at least one sample inside the window was non-normal or unmeasured.
    """
    in_window = samples_in_window(samples, start_ts, end_ts)
    if not in_window:
        return "no mac samples recorded during this window -- cannot attribute"
    bad = [s for s in in_window if not sample_is_normal(s)]
    if bad:
        names = sorted({pressure_level_name(s.get("pressure_level")) for s in bad})
        return (f"{len(bad)}/{len(in_window)} mac samples non-normal during "
                f"this window ({', '.join(names)})")
    return None


def freshness_ok(age_s, max_age_s=FRESHNESS_MAX_AGE_S):
    if age_s is None:
        return False, "freshness not measured"
    if age_s < 0:
        return False, f"negative age ({age_s:.1f}s) -- clock skew or bad sample"
    if age_s > max_age_s:
        return False, f"newest data is {age_s:.1f}s old (> {max_age_s:.1f}s bound)"
    return True, f"{age_s:.1f}s old (<= {max_age_s:.1f}s bound)"


def build_tab_entry(tab_id, rendered, render_detail, ttfp_s, console_errors,
                     pgwt_console_errors, freshness_age_s, mac_void_reason):
    """One tab's row in the verdict. `ok` is false if the tab didn't render,
    threw a real (non-'[pgwt]') console error, OR its window is void for
    Mac-side infrastructure reasons -- void_reason is reported SEPARATELY so
    a reader can tell "the product broke" from "the Mac was busy building
    something else at the time" (criteria doc: void is infrastructure, not
    a product failure) rather than collapsing both into one boolean."""
    fresh_ok, fresh_detail = freshness_ok(freshness_age_s)
    ok = bool(rendered) and not console_errors and mac_void_reason is None
    return {
        "tab": tab_id,
        "rendered": bool(rendered),
        "render_detail": render_detail,
        "ttfp_s": ttfp_s,
        "console_errors": list(console_errors),
        "pgwt_console_errors": list(pgwt_console_errors),
        "freshness": {"age_s": freshness_age_s, "ok": fresh_ok, "detail": fresh_detail},
        "void_reason": mac_void_reason,
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
    }
