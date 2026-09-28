#!/usr/bin/env python3
"""ui_mac_walk_safari_reconnect.py -- the same bridge-drop/recovery
measurement as tests/ui_mac_walk_reconnect.py, but against REAL Safari via
safaridriver/WebDriver (tests/safari_webdriver.py) instead of Playwright
Chrome. See that module's docstring for why this is not Playwright-webkit.

Assumes:
  - `safaridriver --enable` has already been run, once, interactively, by a
    human (this script never attempts it -- see safari_webdriver.py).
  - The remote VM's trace directory + pgwt-server binary already exist
    (tests/ui_mac_walk.sh left them in place; pgwt-server reads trace files
    from disk and does not need a live daemon process to keep serving them).
  - Nothing else is already listening on --safaridriver-port.

What it measures, matching the owner's priority ask verbatim:
  1. Initial connect (banner + viewport identity: Safari/macOS version,
     achieved CSS viewport, devicePixelRatio -- recorded in the verdict).
  2. Kill the LOCAL bridge process outright (simulates the ssh hop itself
     dying, the harshest real failure -- not just a network blip).
  3. HOLD it down long enough (default 20s) for the browser's own
     reconnectDelay backoff (web/static/app.js: 2s doubling, capped at
     16000ms) to climb past its cap and make at least one retry AT the cap
     -- a single fast kill/restart (as in the Chrome run) only ever
     exercises the FIRST 2s attempt and cannot confirm the cap is real.
  4. Restart the bridge, and measure wall-clock time to the #status
     element reporting 'connected' again, plus whether the panel's data
     (row count / DB Time figure) matches pre-drop -- i.e. no state lost.

Usage:
    python3 tests/ui_mac_walk_safari_reconnect.py \\
        --bridge-bin web/pgwt --port 8387 --safaridriver-port 9995 \\
        --trace-dir /root/.../trace --server-path /root/.../pgwt-server \\
        --target root@1.2.3.4 --hold-down-s 20
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_mac_walk_lib as lib
from safari_webdriver import SafariSession, WebDriverError

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "results", "ui_mac_walk_safari_reconnect")

# Measured empirically (2026-09-28, this Mac, Safari 18.6): an outer window
# rect of 1440x984 yields an exact 1440x932 CSS inner viewport at DPR 2 --
# WebDriver's setWindowRect sizes the OS window's OUTER bounds (title bar +
# toolbar included), there is no direct "set CSS viewport" verb, so this is
# a measured constant, not a formula -- verified again at runtime via
# viewport_size() and recorded, never assumed.
WINDOW_OUTER_SIZE = (1440, 984)
TARGET_VIEWPORT = {"width": 1440, "height": 932, "dpr": 2}


def url_ready(url, timeout=2.0):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


def spawn_bridge(bridge_bin, port, trace_dir, server_path, target, log_path):
    log = open(log_path, "ab")
    proc = subprocess.Popen([bridge_bin, "--port", str(port),
                              "--trace-dir", trace_dir,
                              "--server-path", server_path, target],
                             stdout=log, stderr=subprocess.STDOUT)
    base_url = f"http://localhost:{port}/"
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"bridge exited during startup (rc={proc.returncode}); see {log_path}")
        if url_ready(base_url + "session"):
            return proc, base_url
        time.sleep(0.5)
    proc.kill()
    raise RuntimeError(f"bridge never answered {base_url}session; see {log_path}")


def spawn_safaridriver(port, log_path):
    log = open(log_path, "ab")
    proc = subprocess.Popen(["/usr/bin/safaridriver", "--port", str(port)],
                             stdout=log, stderr=subprocess.STDOUT)
    driver_url = f"http://localhost:{port}"
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"safaridriver exited during startup; see {log_path}")
        try:
            with urllib.request.urlopen(f"{driver_url}/status", timeout=2) as r:
                if r.status == 200:
                    return proc, driver_url
        except Exception:
            pass
        time.sleep(0.5)
    proc.kill()
    raise RuntimeError(f"safaridriver never became ready on port {port}")


def status_snapshot(session):
    return session.execute_script(
        "const el = document.getElementById('status'); "
        "return el ? {text: el.textContent, cls: el.className} : null;")


def version_skew_snapshot(session):
    """Reads #version-skew (web/static/app.js renderVersionSkew) so a
    walk can report client/server version skew from the verdict instead of
    relying on a human noticing the header badge in a screenshot."""
    return session.execute_script(
        "const el = document.getElementById('version-skew'); "
        "if (!el) return {present: false}; "
        "const cs = getComputedStyle(el); "
        "return {present: true, visible: cs.display !== 'none', "
        "text: el.textContent, cls: el.className};")


def overview_signature(session):
    """A cheap proxy for 'no state lost': the Overview table's row count
    (same selector tests/ui_live_smoke.py's _TABLE_CHECK uses) plus the
    page's own live-tick counter, if the app exposes one. Static here by
    construction (no live daemon/workload running during this experiment,
    see module docstring) -- a row-count mismatch after recovery would mean
    the reconnect resync corrupted or lost what was already loaded, not
    that new data legitimately arrived."""
    return session.execute_script(
        "const rows = document.querySelectorAll("
        "'#table-container table tbody tr').length;"
        "return {rows};")


def wait_for_state(session, predicate, timeout_s, poll_s=0.5):
    start = time.monotonic()
    deadline = start + timeout_s
    snap = None
    while True:
        snap = status_snapshot(session)
        if predicate(snap):
            return snap, time.monotonic() - start
        if time.monotonic() >= deadline:
            return snap, None
        time.sleep(poll_s)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bridge-bin", default="web/pgwt")
    ap.add_argument("--port", type=int, default=8387)
    ap.add_argument("--safaridriver-port", type=int, default=9995)
    ap.add_argument("--trace-dir", required=True)
    ap.add_argument("--server-path", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--out-dir", default=RESULTS_DIR)
    ap.add_argument("--hold-down-s", type=float, default=20.0,
                     help="how long to keep the bridge dead before restarting it "
                          "-- long enough for reconnectDelay to reach its 16s cap "
                          "and retry at least once there")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    bridge_log = os.path.join(args.out_dir, "bridge.log")
    driver_log = os.path.join(args.out_dir, "safaridriver.log")
    open(bridge_log, "w").close()
    open(driver_log, "w").close()

    result = {"browser": "safari", "port": args.port, "hold_down_s": args.hold_down_s,
              "steps": []}

    print(f"[1/6] starting safaridriver on :{args.safaridriver_port}")
    driver_proc, driver_url = spawn_safaridriver(args.safaridriver_port, driver_log)

    print(f"[2/6] starting bridge on :{args.port} against {args.target}")
    bridge_proc, base_url = spawn_bridge(args.bridge_bin, args.port, args.trace_dir,
                                          args.server_path, args.target, bridge_log)

    session = None
    try:
        try:
            session = SafariSession(driver_url)
        except WebDriverError as e:
            # Exact error text, not an inferred cause -- see module docstring.
            result["blocked"] = {"error": str(e), "raw": e.raw}
            print(f"BLOCKED: SafariSession could not open a session: {e}")
            with open(os.path.join(args.out_dir, "reconnect.json"), "w") as f:
                json.dump(result, f, indent=2, sort_keys=True)
            return 1

        result["display_identity"] = {
            "safari_browser_version": session.capabilities.get("browserVersion"),
            "safari_platform_build": session.capabilities.get("safari:platformBuildVersion"),
            "safari_platform_version": session.capabilities.get("safari:platformVersion"),
            "macos_product_version": subprocess.run(
                ["sw_vers", "-productVersion"], capture_output=True, text=True).stdout.strip(),
            "macos_build_version": subprocess.run(
                ["sw_vers", "-buildVersion"], capture_output=True, text=True).stdout.strip(),
        }

        session.navigate(base_url)
        session.set_window_rect(*WINDOW_OUTER_SIZE)
        achieved_viewport = session.viewport_size()
        result["display_identity"]["viewport"] = achieved_viewport
        result["display_identity"]["viewport_matches_target"] = (
            achieved_viewport == TARGET_VIEWPORT)
        print(f"  display identity: {result['display_identity']}")

        print("[3/6] waiting for initial connect")
        snap, elapsed = wait_for_state(
            session, lambda s: s is not None and lib.is_connected_state(s["cls"]), 30)
        if elapsed is None:
            raise RuntimeError(f"never reached connected state initially: {snap}")
        time.sleep(0.5)
        session.screenshot(os.path.join(args.out_dir, "01-connected.png"))
        sig_before = overview_signature(session)
        skew_snap = version_skew_snapshot(session)
        skew = lib.version_skew_from_snapshot(skew_snap)
        result["version_skew"] = skew
        result["steps"].append({"step": "connected", "snapshot": snap,
                                 "ttfc_s": elapsed, "overview_signature": sig_before})
        print(f"  connected after {elapsed}s: {snap}, overview={sig_before}")
        print(f"  version_skew: {skew} (raw={skew_snap})")

        print("[4/6] killing the bridge and holding it down "
              f"{args.hold_down_s}s (simulates the ssh hop dying)")
        kill_ts = time.monotonic()
        bridge_proc.kill()
        bridge_proc.wait(timeout=10)

        drop_snap, drop_elapsed_s = wait_for_state(
            session, lambda s: s is not None and not lib.is_connected_state(s["cls"]), 30)
        session.screenshot(os.path.join(args.out_dir, "02-dropped.png"))
        panel_during_drop = session.execute_script(
            "const el = document.querySelector('#table-container'); "
            "return el ? {hasContent: el.children.length > 0 || "
            "el.textContent.trim().length > 0} : {hasContent: null};")
        result["steps"].append({"step": "dropped", "snapshot": drop_snap,
                                 "drop_detected_s": drop_elapsed_s,
                                 "panel_content_during_drop": panel_during_drop})
        print(f"  drop detected after {drop_elapsed_s}s: {drop_snap}, "
              f"panel={panel_during_drop}")

        remaining = args.hold_down_s - (time.monotonic() - kill_ts)
        if remaining > 0:
            print(f"  holding down for {remaining:.1f}s more "
                  "(letting the client-side backoff climb toward its 16s cap)")
            time.sleep(remaining)
        # One more snapshot at the end of the hold, to see the backoff
        # message's own reported delay near the cap.
        held_snap = status_snapshot(session)
        result["steps"].append({"step": "held", "snapshot": held_snap,
                                 "held_for_s": time.monotonic() - kill_ts})
        print(f"  after {args.hold_down_s}s held down: {held_snap}")

        print("[5/6] restarting the bridge")
        bridge_proc, base_url2 = spawn_bridge(args.bridge_bin, args.port, args.trace_dir,
                                               args.server_path, args.target, bridge_log)
        assert base_url2 == base_url

        recovered_snap, recovery_elapsed_s = wait_for_state(
            session, lambda s: s is not None and lib.is_connected_state(s["cls"]), 60)
        session.screenshot(os.path.join(args.out_dir, "03-recovered.png"))
        total_outage_s = (None if recovery_elapsed_s is None
                           else (time.monotonic() - kill_ts))
        sig_after = overview_signature(session)
        result["steps"].append({
            "step": "recovered", "snapshot": recovered_snap,
            "recovery_s_since_bridge_restart": recovery_elapsed_s,
            "total_outage_s_since_kill": total_outage_s,
            "overview_signature": sig_after,
            "state_preserved": sig_after == sig_before,
        })
        print(f"  recovered after {recovery_elapsed_s}s (restart) / "
              f"{total_outage_s}s (total since kill): {recovered_snap}")
        print(f"  overview before={sig_before} after={sig_after} "
              f"preserved={sig_after == sig_before}")

        print("[6/6] done")
    finally:
        if session is not None:
            session.close()
        if bridge_proc.poll() is None:
            bridge_proc.kill()
        if driver_proc.poll() is None:
            driver_proc.kill()

    with open(os.path.join(args.out_dir, "reconnect.json"), "w") as f:
        json.dump(result, f, indent=2, sort_keys=True)
    print(f"\nresult: {os.path.join(args.out_dir, 'reconnect.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
