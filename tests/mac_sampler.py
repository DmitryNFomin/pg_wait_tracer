#!/usr/bin/env python3
"""mac_sampler.py -- the Mac-side pressure/load sampler shared by
tests/ui_mac_walk.py and tests/ui_mac_walk_safari.py (issue: mac-side
walk). Factored out so the two walks (Chrome via Playwright, Safari via
safaridriver) attribute every tick against the SAME sampling code rather
than two copies drifting apart.

Impure by nature (real subprocess calls, real clock) -- tests/ui_mac_walk_lib
.py stays pure and owns the parsing/attribution logic this module's samples
feed into (parse_pressure_level, parse_free_percent, sample_is_normal,
window_void_reason).
"""
import os
import subprocess
import threading
import time

import ui_mac_walk_lib as lib

SAMPLE_INTERVAL_S = 10.0


class MacSampler:
    """Background sampler, one thread, stops with the process that started
    it -- never a detached/orphaned loop. Bounded by an Event, joined with a
    timeout by the caller before exit."""

    def __init__(self, interval_s=SAMPLE_INTERVAL_S):
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thread = None
        self.samples = []

    def _read_once(self):
        ts = time.time()
        pressure_level = None
        free_pct = None
        loadavg = None
        note = None
        try:
            r = subprocess.run(
                ["sysctl", "kern.memorystatus_vm_pressure_level"],
                capture_output=True, text=True, timeout=5)
            if r.returncode == 0:
                pressure_level = lib.parse_pressure_level(r.stdout)
            else:
                note = f"sysctl exit {r.returncode}: {r.stderr.strip()[:200]}"
        except Exception as e:
            note = f"sysctl failed: {e}"
        try:
            r = subprocess.run(["memory_pressure", "-Q"],
                                capture_output=True, text=True, timeout=5)
            if r.returncode == 0:
                free_pct = lib.parse_free_percent(r.stdout)
        except Exception:
            pass
        try:
            loadavg = list(os.getloadavg())
        except (AttributeError, OSError):
            loadavg = None
        return {"ts": ts, "pressure_level": pressure_level,
                "pressure_level_name": lib.pressure_level_name(pressure_level),
                "free_pct": free_pct, "loadavg": loadavg, "note": note}

    def read_once(self):
        """Public single-shot read, e.g. for a pre-flight quiet-Mac check
        before the timed portion of a walk starts."""
        return self._read_once()

    def _loop(self):
        while not self._stop.is_set():
            self.samples.append(self._read_once())
            self._stop.wait(self.interval_s)

    def start(self):
        # One immediate sample so a short tab visit still has attribution
        # even before the first interval elapses.
        self.samples.append(self._read_once())
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)


def wait_for_quiet(max_wait_s=300.0, poll_s=15.0, log=print):
    """Bounded pre-flight wait for pressure_level == 1 ("normal") before a
    timed walk starts -- a walk begun while the Mac is loaded just produces
    another all-void run (every tick's window fails window_void_reason).
    Never infinite: gives up after max_wait_s and returns False, letting the
    caller proceed anyway (the run's own per-tick void accounting then
    reports honestly how much of it was unusable, per the same fail-closed
    rule as everywhere else in this walk) rather than hanging.

    Returns (quiet: bool, last_sample: dict)."""
    sampler = MacSampler()
    deadline = time.monotonic() + max_wait_s
    sample = sampler.read_once()
    while not lib.sample_is_normal(sample):
        if time.monotonic() >= deadline:
            log(f"mac_sampler.wait_for_quiet: gave up after {max_wait_s}s -- "
                f"last sample: {sample}")
            return False, sample
        log(f"mac_sampler.wait_for_quiet: pressure={sample.get('pressure_level_name')} "
            f"loadavg={sample.get('loadavg')} -- waiting {poll_s}s")
        time.sleep(poll_s)
        sample = sampler.read_once()
    log(f"mac_sampler.wait_for_quiet: quiet -- {sample}")
    return True, sample
