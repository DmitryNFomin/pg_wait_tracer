#!/usr/bin/env python3
"""The --diag-data wiring: does the flag actually REACH the probe?

issue #384 added a per-frame panel-data probe to tests/ui_live_smoke.py
(`_panel_data_probe`, gated on run_tab's `diag_data=`). The instrument was
complete and called by nothing on the rehearsal path: tests/demo_rehearsal.py
has its own run_tab call site, so `--diag-data` could exist, be documented,
and still never produce a byte during a rehearsal. A present-but-unreached
instrument is the diagnostic equivalent of a gate that cannot see.

So this asserts the whole thread, each link separately, and each with a
demonstrable red (all of these fail if the corresponding line is dropped):

  1. the CLI accepts --diag-data and defaults it OFF (a counted attempt must
     not silently pay for per-frame probes);
  2. run_tab still HAS a diag_data parameter -- if issue #384's instrument is
     renamed, the thread breaks silently and this is the only place that
     notices;
  3. _run_passes FORWARDS the flag's value to every run_tab call, both when
     it is on and when it is off (a wiring that hard-codes True is as wrong
     as one that hard-codes False);
  4. _run_passes' default is False, so an older caller cannot turn the
     diagnostic on by accident.

No browser is started: run_tab is replaced by a recorder, which is the only
way to assert what argument it was HANDED rather than what it did with it.

Run: python3 tests/test_demo_rehearsal_diag.py
"""
import inspect
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# A hard import: if playwright is missing this test FAILS (nonzero exit)
# rather than skipping. A wiring check that quietly does not run is exactly
# the failure mode it exists to catch.
import demo_rehearsal as dr
import ui_live_smoke as live_smoke

failures = []
passes = 0


def check(cond, msg):
    global passes
    if cond:
        passes += 1
        print(f"  PASS: {msg}")
    else:
        failures.append(msg)
        print(f"  FAIL: {msg}")


REQUIRED_CLI = ["--url", "http://x/", "--trace-dir", "/tmp/x",
                "--daemon-log", "/tmp/x.log"]


def test_cli_accepts_the_flag_and_defaults_it_off():
    ap = dr.build_arg_parser()
    off = ap.parse_args(REQUIRED_CLI)
    on = ap.parse_args(REQUIRED_CLI + ["--diag-data"])
    check(off.diag_data is False,
          f"--diag-data defaults OFF, so a counted attempt pays nothing "
          f"({off.diag_data!r})")
    check(on.diag_data is True,
          f"--diag-data turns it on ({on.diag_data!r})")


def test_run_tab_still_takes_diag_data():
    params = inspect.signature(live_smoke.run_tab).parameters
    check("diag_data" in params,
          "ui_live_smoke.run_tab still has the diag_data parameter this "
          f"wiring targets ({sorted(params)})")
    check(params.get("diag_data") is not None
          and params["diag_data"].default is False,
          "run_tab's own default is OFF, so the gating walk is unaffected")


def _record_run_tab(recorded):
    def fake_run_tab(browser, tab_id, url, out_dir, ticks, first_data_timeout,
                     blink_threshold, viewport=None, device_scale_factor=None,
                     diag_data=False, **kw):
        recorded.append({"tab": tab_id, "diag_data": diag_data})
        # The same keys _run_passes reads out of a real result, so this
        # exercises the real call site rather than a shortened one.
        return {"ok": True, "ticks_observed": ticks, "rendered": True,
                "checks": {}, "blink_sweep": {"ticks": []}}
    return fake_run_tab


def _passes_with(diag_data, monkeypatched):
    """Drive _run_passes over ONE pass of every tab with run_tab and the
    liveness assertions replaced. One pass at offset 0 against a t0 of now:
    `remaining` is non-positive, so nothing sleeps and nothing here depends
    on wall-clock timing."""
    recorded = []
    saved = (live_smoke.run_tab, live_smoke._assert_workload_alive,
             dr._assert_daemon_alive)
    live_smoke.run_tab = _record_run_tab(recorded)
    live_smoke._assert_workload_alive = lambda *a, **k: None
    dr._assert_daemon_alive = lambda *a, **k: None
    try:
        kwargs = {}
        if diag_data is not None:
            kwargs["diag_data"] = diag_data
        dr._run_passes(None, "http://x/", monkeypatched, 2,
                       [("early", 0.0)], 1.0, None, None, None,
                       time.monotonic(),
                       viewport={"width": 100, "height": 100},
                       device_scale_factor=1, **kwargs)
    finally:
        (live_smoke.run_tab, live_smoke._assert_workload_alive,
         dr._assert_daemon_alive) = saved
    return recorded


def test_flag_reaches_every_run_tab_call(tmp):
    on = _passes_with(True, tmp)
    off = _passes_with(False, tmp)
    default = _passes_with(None, tmp)
    check(len(on) > 0 and len(on) == len(off) == len(default),
          f"the control held: every tab was walked in all three runs "
          f"({len(on)}/{len(off)}/{len(default)} calls)")
    check(bool(on) and all(c["diag_data"] is True for c in on),
          f"--diag-data reaches EVERY run_tab call "
          f"({[c['diag_data'] for c in on]})")
    check(bool(off) and all(c["diag_data"] is False for c in off),
          f"without the flag nothing is probed -- not hard-coded on "
          f"({[c['diag_data'] for c in off]})")
    check(bool(default) and all(c["diag_data"] is False for c in default),
          f"_run_passes' own default is OFF for an older caller "
          f"({[c['diag_data'] for c in default]})")


def main():
    import tempfile
    tmp = tempfile.mkdtemp(prefix="demo_rehearsal_diag_")
    print("--- test_cli_accepts_the_flag_and_defaults_it_off ---")
    test_cli_accepts_the_flag_and_defaults_it_off()
    print("--- test_run_tab_still_takes_diag_data ---")
    test_run_tab_still_takes_diag_data()
    print("--- test_flag_reaches_every_run_tab_call ---")
    test_flag_reaches_every_run_tab_call(tmp)
    print(f"\n{passes}/{passes + len(failures)} passed, {len(failures)} failed")
    for f in failures:
        print(f"  - {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
