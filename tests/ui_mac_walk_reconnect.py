#!/usr/bin/env python3
"""ui_mac_walk_reconnect.py -- a focused, one-off measurement of the product
question the owner raised after seeing "Error: server unavailable
(reconnecting)" with every panel blank: what does the UI show when the
bridge<->daemon connection drops mid-session, and how long does it take to
recover? This is a MEASUREMENT, not a fix -- it does not change app.js.

Same Mac-side topology as tests/ui_mac_walk.py (real browser on the Mac,
bridge on the Mac, ssh to a VM), reusing its already-populated trace
directory and pgwt-server binary -- no live daemon/pgbench needed, since
pgwt-server reads trace files it does not need the daemon process alive to
keep serving them. This script owns the bridge process directly (spawns it,
kills it to simulate the ssh hop dying, restarts it) so it does not need any
new VM-side orchestration.

Usage:
    python3 tests/ui_mac_walk_reconnect.py --bridge-bin web/pgwt \\
        --port 8386 --trace-dir /root/.../trace \\
        --server-path /root/.../pgwt-server --target root@1.2.3.4 \\
        [--browser chrome|safari-blocked-placeholder]
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_mac_walk_lib as lib

try:
    from playwright.sync_api import sync_playwright, TimeoutError as PWTimeoutError
except ImportError:
    print("ERROR: pip install playwright", file=sys.stderr)
    sys.exit(1)

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "results", "ui_mac_walk_reconnect")
VIEWPORT = {"width": 1440, "height": 932}
DEVICE_SCALE_FACTOR = 2
POLL_INTERVAL_S = 0.25
DROP_TIMEOUT_S = 30
RECOVERY_TIMEOUT_S = 90


def url_ready(url, timeout=2.0):
    import urllib.request
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


def status_snapshot(page):
    return page.evaluate(
        "() => { const el = document.getElementById('status'); "
        "return el ? {text: el.textContent, cls: el.className} : null; }")


def version_skew_snapshot(page):
    """Reads #version-skew (web/static/app.js renderVersionSkew) so a
    walk can report client/server version skew from the verdict instead of
    relying on a human noticing the header badge in a screenshot."""
    return page.evaluate(
        "() => { const el = document.getElementById('version-skew'); "
        "if (!el) return {present: false}; "
        "const cs = getComputedStyle(el); "
        "return {present: true, visible: cs.display !== 'none', "
        "text: el.textContent, cls: el.className}; }")


def wait_for_state(page, predicate, timeout_s, poll_s=POLL_INTERVAL_S):
    """Polls status_snapshot(page) until predicate(snapshot) is true.
    Returns (snapshot, elapsed_s) -- elapsed_s is None on timeout (never a
    fabricated value), snapshot is whatever was last observed."""
    start = time.monotonic()
    deadline = start + timeout_s
    snap = status_snapshot(page)
    while True:
        snap = status_snapshot(page)
        if predicate(snap):
            return snap, time.monotonic() - start
        if time.monotonic() >= deadline:
            return snap, None
        page.wait_for_timeout(int(poll_s * 1000))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bridge-bin", default="web/pgwt")
    ap.add_argument("--port", type=int, default=8386)
    ap.add_argument("--trace-dir", required=True)
    ap.add_argument("--server-path", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--out-dir", default=RESULTS_DIR)
    ap.add_argument("--browser-label", default="chrome",
                     help="recorded in the verdict so a reader can tell which "
                          "real browser produced this measurement")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    bridge_log = os.path.join(args.out_dir, "bridge.log")
    open(bridge_log, "w").close()

    result = {"browser": args.browser_label, "port": args.port, "steps": []}

    print(f"[1/5] starting bridge on :{args.port} against {args.target}")
    proc, base_url = spawn_bridge(args.bridge_bin, args.port, args.trace_dir,
                                   args.server_path, args.target, bridge_log)
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(channel="chrome")
            try:
                context = browser.new_context(viewport=VIEWPORT,
                                               device_scale_factor=DEVICE_SCALE_FACTOR)
                page = context.new_page()
                page.goto(base_url)

                print("[2/5] waiting for initial connect")
                try:
                    page.wait_for_selector("#status.connected", timeout=30000)
                except PWTimeoutError:
                    raise RuntimeError("never reached #status.connected initially")
                page.wait_for_timeout(500)
                page.screenshot(path=os.path.join(args.out_dir, "01-connected.png"))
                snap = status_snapshot(page)
                skew_snap = version_skew_snapshot(page)
                skew = lib.version_skew_from_snapshot(skew_snap)
                result["version_skew"] = skew
                result["steps"].append({"step": "connected", "snapshot": snap,
                                         "is_connected": lib.is_connected_state(snap["cls"])})
                print(f"  connected: {snap}")
                print(f"  version_skew: {skew} (raw={skew_snap})")

                print("[3/5] killing the bridge (simulates the ssh hop / server dying)")
                kill_ts = time.monotonic()
                proc.kill()
                proc.wait(timeout=10)

                drop_snap, drop_elapsed_s = wait_for_state(
                    page, lambda s: s is not None and not lib.is_connected_state(s["cls"]),
                    DROP_TIMEOUT_S)
                page.screenshot(path=os.path.join(args.out_dir, "02-dropped.png"))
                # Panel-blankness: reuses the same atomic snapshot idiom
                # tests/ui_live_smoke.py's blind-window check uses, applied
                # to whichever container the default landing tab shows.
                panel = page.evaluate(
                    "() => { const el = document.querySelector('#table-container'); "
                    "return el ? {hasContent: el.children.length > 0 || "
                    "el.textContent.trim().length > 0} : {hasContent: null}; }")
                result["steps"].append({
                    "step": "dropped", "snapshot": drop_snap,
                    "drop_detected_s": drop_elapsed_s,
                    "panel_content_after_drop": panel,
                })
                print(f"  drop detected after {drop_elapsed_s}s: {drop_snap}, panel={panel}")

                print("[4/5] restarting the bridge")
                proc, base_url2 = spawn_bridge(args.bridge_bin, args.port, args.trace_dir,
                                                args.server_path, args.target, bridge_log)
                assert base_url2 == base_url

                recovered_snap, recovery_elapsed_s = wait_for_state(
                    page, lambda s: s is not None and lib.is_connected_state(s["cls"]),
                    RECOVERY_TIMEOUT_S)
                page.screenshot(path=os.path.join(args.out_dir, "03-recovered.png"))
                total_outage_s = (None if recovery_elapsed_s is None
                                   else (time.monotonic() - kill_ts))
                result["steps"].append({
                    "step": "recovered", "snapshot": recovered_snap,
                    "recovery_s_since_bridge_restart": recovery_elapsed_s,
                    "total_outage_s_since_kill": total_outage_s,
                })
                print(f"  recovered after {recovery_elapsed_s}s (restart) / "
                      f"{total_outage_s}s (total since kill): {recovered_snap}")

                print("[5/5] checking panel repopulated after recovery")
                page.wait_for_timeout(1000)
                panel_after = page.evaluate(
                    "() => { const rows = document.querySelectorAll("
                    "'#table-container table tbody tr'); return rows.length; }")
                result["panel_rows_after_recovery"] = panel_after
                print(f"  rows after recovery: {panel_after}")

                context.close()
            finally:
                browser.close()
    finally:
        if proc.poll() is None:
            proc.kill()

    with open(os.path.join(args.out_dir, "reconnect.json"), "w") as f:
        json.dump(result, f, indent=2, sort_keys=True)
    print(f"\nresult: {os.path.join(args.out_dir, 'reconnect.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
