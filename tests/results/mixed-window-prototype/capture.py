#!/usr/bin/env python3
"""capture.py -- screenshot generator for the mixed-window design decision.

PROTOTYPE, NOT FOR MERGE. Branch agent/mixed-window-prototype (issue #322's
design question). This drives the REAL web UI against tests/mock_server.py in
new PROTOTYPE-ONLY env-var modes (PGWT_MOCK_FIDELITY_RANGES, PGWT_MOCK_CPU_
NODE_MODE -- see the comments at their definitions in mock_server.py) that
make the mock emit server facts no real pgwt-server or production mock path
ever emits. That is the mock LEADING the server, which is forbidden
everywhere else in this repo (tests/test_protocol_drift.py enforces the mock
follows the server) -- it is allowed HERE, and only here, because the point
of this branch is to let the owner SEE a design option before anyone commits
to shipping it. None of these renders are a measurement of anything real;
every number is illustrative, picked to make a trade-off legible in a
screenshot.

Usage: python3 tests/results/mixed-window-prototype/capture.py
  (No root, no PG, no SSH -- same mock_server.py every other Playwright suite
  in this repo uses. Writes PNGs into this directory.)
"""
import os
import socket
import subprocess
import sys
import time

from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MOCK_SCRIPT = os.path.join(REPO_ROOT, "tests", "mock_server.py")
BASE_PORT = 18970
VIEWPORT = {"width": 1280, "height": 900}


def _wait_ports(proc, ports, timeout=10.0):
    deadline = time.time() + timeout
    for port in ports:
        while True:
            if proc.poll() is not None:
                return False
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                    break
            except OSError:
                if time.time() > deadline:
                    return False
                time.sleep(0.1)
    time.sleep(0.2)
    return proc.poll() is None


def start_mock(port, extra_env):
    env = dict(os.environ)
    env.update(extra_env)
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    )
    if not _wait_ports(proc, (port, port + 1)):
        out, err = proc.communicate()
        print(f"mock_server failed to start on port {port}:\n"
              f"{out.decode()}\n{err.decode()}", file=sys.stderr)
        sys.exit(1)
    return proc


def stop_mock(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def app_url(port):
    return f"http://127.0.0.1:{port}/?ws=ws://127.0.0.1:{port + 1}/ws"


def goto_tab(page, tab):
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(600)
    if tab != "overview":
        page.click(f".tab[data-tab='{tab}']")
    page.wait_for_timeout(1200)


def shot(page, name, selector=None):
    path = os.path.join(HERE, name)
    if selector:
        el = page.query_selector(selector)
        if el is None:
            print(f"  !! selector {selector!r} not found for {name}", file=sys.stderr)
            page.screenshot(path=path, full_page=True)
        else:
            el.screenshot(path=path)
    else:
        page.screenshot(path=path, full_page=False)
    print(f"  wrote {path}")


def run_scenario(port, env, steps):
    """env: extra env vars for the mock. steps: list of (tab, shots)."""
    proc = start_mock(port, env)
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            ctx = browser.new_context(viewport=VIEWPORT)
            page = ctx.new_page()
            page.goto(app_url(port))
            for tab, shots in steps:
                goto_tab(page, tab)
                for name, selector in shots:
                    shot(page, name, selector)
            browser.close()
    finally:
        stop_mock(proc)


def main():
    os.makedirs(HERE, exist_ok=True)

    # ── 1/2: mixed window WITH fidelity_ranges vs WITHOUT (today) ───────────
    print("Scenario: mixed window WITH fidelity_ranges (owner's proposal)")
    run_scenario(BASE_PORT, {
        "PGWT_MOCK_FIDELITY": "mixed",
        "PGWT_MOCK_FIDELITY_RANGES": "1",
    }, [
        ("overview", [
            ("01_overview_mixed_with_ranges.png", None),
            ("05_disclaimer_chip_with_ranges.png", "#aas-fidelity-chip"),
        ]),
        ("transitions", [
            ("02_transitions_mixed_with_ranges.png", None),
        ]),
    ])

    print("Scenario: mixed window WITHOUT fidelity_ranges (today's behaviour)")
    run_scenario(BASE_PORT + 1, {
        "PGWT_MOCK_FIDELITY": "mixed",
    }, [
        ("overview", [
            ("03_overview_mixed_no_ranges.png", None),
            ("06_disclaimer_chip_no_ranges.png", "#aas-fidelity-chip"),
        ]),
        ("transitions", [
            ("04_transitions_mixed_no_ranges.png", None),
        ]),
    ])

    # ── 3: Transitions CPU* node, option A vs option B ──────────────────────
    print("Scenario: Transitions CPU* node — option A (nodes exclude samples)")
    run_scenario(BASE_PORT + 2, {
        "PGWT_MOCK_FIDELITY": "mixed",
        "PGWT_MOCK_CPU_NODE_MODE": "a",
    }, [
        ("transitions", [
            ("07_transitions_option_a.png", "#dfg-container"),
        ]),
    ])

    print("Scenario: Transitions CPU* node — option B (nodes include samples)")
    run_scenario(BASE_PORT + 3, {
        "PGWT_MOCK_FIDELITY": "mixed",
        "PGWT_MOCK_CPU_NODE_MODE": "b",
    }, [
        ("transitions", [
            ("08_transitions_option_b.png", "#dfg-container"),
        ]),
    ])

    print("Done.")


if __name__ == "__main__":
    main()
