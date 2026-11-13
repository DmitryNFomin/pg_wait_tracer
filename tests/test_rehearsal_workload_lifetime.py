#!/usr/bin/env python3
"""#543: exercise production launch argv and walker with a virtual clock.

Only external load/SQL/browser effects are faked. Three late-pass selector
timeouts advance the clock by 180s and return failed tab measurements. The
real liveness guard must allow a red verdict rather than exit before it.
"""
import contextlib
import io
import math
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]


class HangGuardTestCase(unittest.TestCase):
    # Fixture completion is a state, not a performance promise. One outer
    # watchdog covers the whole test (including setup, loops and cleanup).
    hang_timeout = 300

    def run(self, result=None):
        previous = signal.getsignal(signal.SIGALRM)
        def expired(signum, frame):
            # subTest normally catches assertions and continues the loop. A
            # hang must abort that loop too, rather than consume the watchdog.
            if self._outcome is not None:
                self._outcome.result.failfast = True
            raise AssertionError("outer hang guard expired")
        signal.signal(signal.SIGALRM, expired)
        started = time.monotonic()
        timer = signal.setitimer(signal.ITIMER_REAL, self.hang_timeout)
        try:
            return super().run(result)
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous)
            if timer[0]:
                signal.setitimer(signal.ITIMER_REAL,
                                 max(.000001, timer[0] - (time.monotonic() - started)), timer[1])


def run_wait_control(action, timeout):
    """Run a deliberately broken fixture through the real unittest watchdog."""
    class Control(HangGuardTestCase):
        hang_timeout = timeout

        def runTest(self):
            action(self)

    result = unittest.TestResult()
    Control().run(result)
    return result


def socket_inspector(path, ready=None):
    """Replace only OS listener discovery in a disposable fixture copy.

    The drivers still execute the real ownership decision and fail-closed
    CLI. Fixture HTTP servers publish their PID after listen(); a connect
    verifies the socket, and that signal identifies its actual owner.
    The preflight-only fixture needs only occupied versus free.
    """
    source = path.read_text()
    start = source.index('def listeners(port):')
    end = source.index('\ndef main():', start)
    probe = r'''
def listeners(port):
    import socket
    from pathlib import Path
    import time
    try:
        with socket.create_connection(('127.0.0.1', port)):
            pass
    except ConnectionRefusedError:
        return []
    ready = READY
    if ready is None:
        return [{os.getpid()}]  # preflight refuses every occupied port
    ready = Path(ready)
    while not ready.exists():
        time.sleep(.01)
    return [{int(ready.read_text())}]

'''.replace('READY', repr(str(ready)) if ready is not None else 'None')
    path.write_text(source[:start] + probe + source[end:])


class WorkloadLifetime(HangGuardTestCase):
    def setUp(self):
        global dr, loop
        import demo_rehearsal as dr
        import live_loop_workload as loop

    def launch_argv(self, directory):
        # Execute the actual controlled-load block. Commands record their argv
        # instead of connecting to PostgreSQL; shell startup probes are no-ops.
        source = (ROOT / "tests/demo_rehearsal.sh").read_text()
        block = source.split("# ── 1. Controlled load", 1)[1]
        block = block[block.index("\n"):].split("# ── 2. Daemon", 1)[0]
        prelude = r'''
set -uo pipefail
check_pgbench_provisioned() { :; }
sleep() { :; }
kill() { :; }
pgbench() { printf '%s\n' "$@" > "$CAPTURE/pgbench.args"; }
python3() { printf '%s\n' "$@" > "$CAPTURE/loop.args"; }
SCRIPT_DIR=tests
DURATION_S=2100
PGBENCH_LOG="$CAPTURE/pgbench.log"
WORKLOAD_LOG="$CAPTURE/loop.log"
'''
        run = subprocess.run(["bash", "-c", prelude + block + "\nwait\n"],
                             env=dict(os.environ, CAPTURE=directory),
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return [(Path(directory) / name).read_text().splitlines()
                for name in ("pgbench.args", "loop.args")]

    def test_three_selector_timeouts_past_old_deadline_produce_red_not_void(self):
        with tempfile.TemporaryDirectory() as tmp:
            pg_args, loop_args = self.launch_argv(tmp)
            # Model the lifetimes selected by the production argv. pgbench's
            # finite transaction count at --rate=25 is a completion limit too.
            pg_end = (float(pg_args[pg_args.index("-T") + 1]) if "-T" in pg_args
                      else 4 * int(pg_args[pg_args.index("-t") + 1]) / 25)
            loop_end = (math.inf if loop_args[1] == "--until-stopped"
                        else float(loop_args[1]))
            now = [0.0]
            walked = []
            ticks, schedule = dr.drlib.rehearsal_plan(2100)
            self.assertEqual(ticks, dr.lib.MIN_TICKS)
            self.assertEqual(len(schedule), 3)

            def tab(browser, tab_id, url, out_dir, *args, **kwargs):
                delayed = Path(out_dir).name == "late" and tab_id in dr.lib.TABS[:3]
                now[0] += 47 + (60 if delayed else 0)
                walked.append((Path(out_dir).name, tab_id))
                return {"ok": not delayed, "ticks_observed": ticks,
                        "rendered": {"ok": not delayed,
                                     "detail": "selector timeout" if delayed else "rows"},
                        "blink_sweep": {"ticks": [{
                            "target_offsets_ms": list(dr.lib.SWEEP_OFFSETS_MS),
                            "achieved_offsets_ms": [200, 600, 1100, 1600, 2100]}]}}

            output = io.StringIO()
            def walk_until(pg_end):
                now[0] = 0.0
                walked.clear()
                with patch.object(dr.time, "monotonic", side_effect=lambda: now[0]), \
                        patch.object(dr.time, "sleep", side_effect=lambda s: now.__setitem__(0, now[0] + s)), \
                        patch.object(dr.live_smoke, "run_tab", side_effect=tab), \
                        patch.object(dr.live_smoke, "_pid_is_live",
                                     side_effect=lambda pid: now[0] < {101: pg_end, 102: loop_end}[pid]), \
                        contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                    return dr._run_passes(
                        None, "http://fixture/", tmp, ticks, schedule, 60,
                        101, 102, None, 0, viewport={}, device_scale_factor=1)

            try:
                results, _ = walk_until(pg_end)
            except SystemExit as exc:
                self.fail(f"delayed walk voided (exit={exc.code}):\n{output.getvalue()}")
            self.assertGreater(now[0], 2100, "regression must actually cross the old deadline")
            self.assertEqual(len(walked), 3 * len(dr.lib.TABS))
            summary = dr.drlib.build_demo_summary(results, {"fixture": {"ok": True}})
            self.assertFalse(summary["ok"], "selector failures must remain failures")
            self.assertEqual(summary["failed"], [f"late/{tab}" for tab in dr.lib.TABS[:3]])
            print(f"delayed walk: {now[0]:.0f}s > old 2100s; "
                  f"{len(walked)} tabs; RED verdict, no liveness void")
            with self.subTest(pg_end=1000), self.assertRaises(SystemExit):
                walk_until(1000)

    def test_dead_workload_still_refuses(self):
        for dead in (101, 102):
            with self.subTest(dead=dead), \
                    patch.object(dr.live_smoke, "_pid_is_live", side_effect=lambda pid: pid != dead), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                dr.live_smoke._assert_workload_alive(101, 102, "fixture")

    def test_until_stopped_runs_past_duration_and_sigterm_cleans_up(self):
        for argv, expected in ((["35"], 1), (["--until-stopped"], 3)):
            with self.subTest(argv=argv), contextlib.ExitStack() as stack:
                now = [0]
                handlers = {}
                workload = Mock()
                workload.extra_sessions = []
                workload.fire.return_value = True

                def release():
                    now[0] += 60
                    if workload.release.call_count == 3:
                        handlers[signal.SIGTERM](signal.SIGTERM, None)

                workload.release.side_effect = release
                stack.enter_context(patch.object(sys, "argv", ["live_loop_workload.py", *argv]))
                stack.enter_context(patch.object(loop, "Workload", return_value=workload))
                stack.enter_context(patch.object(loop, "psql"))
                stack.enter_context(patch.object(loop.signal, "signal", side_effect=handlers.__setitem__))
                stack.enter_context(patch.object(loop.time, "monotonic", side_effect=lambda: now[0]))
                stack.enter_context(patch.object(loop.time, "sleep"))
                for name in ("_reporter_tick", "_row_lock_tick", "_io_reader_tick",
                             "_io_load_read_tick", "_io_load_write_tick", "_lockmgr_tick"):
                    stack.enter_context(patch.object(loop, name))
                self.assertEqual(loop.main(), 0)
                self.assertEqual(workload.fire.call_count, expected)
                workload.stop.assert_called_once_with(keep_table=False)

    def test_plan_preflight_before_postgres_discovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tests = root / "tests"
            tests.mkdir()
            for name in ("demo_rehearsal.sh", "testutil.sh", "live_daemon_lib.sh",
                         "bridge_owner.py", "demo_rehearsal_lib.py", "ui_live_smoke_lib.py"):
                shutil.copy(ROOT / "tests" / name, tests / name)
            socket_inspector(tests / "bridge_owner.py")
            self.check_plan_preflight(root)

    def check_plan_preflight(self, root):
        for duration in ("0", "nan", "inf", "nonsense", "0.01", "10", "35"):
            with self.subTest(duration=duration):
                # An unavailable version bounds even a misplaced preflight to
                # discovery; this test must never start load on a gate box.
                run = subprocess.run(["bash", "tests/demo_rehearsal.sh", "--pg-version", "999"], cwd=root,
                                     env=dict(os.environ, DURATION_MIN=duration),
                                     capture_output=True, text=True)
                self.assertNotEqual(run.returncode, 0)
                output = run.stdout + run.stderr
                if duration == "35":
                    self.assertIn("launch plan target=2100s", output)
                    self.assertNotIn("REFUSING", output)
                    self.assertIn("cannot find postmaster PID", output)
                else:
                    self.assertIn("REFUSING rehearsal plan", output)
                    self.assertNotIn("postmaster", output)

    def test_real_plans_and_explicit_mechanics_self_test_are_accepted(self):
        for duration in (30, 35, 45):
            ticks, passes = dr.drlib.rehearsal_plan(duration * 60)
            self.assertEqual(ticks, dr.lib.MIN_TICKS)
            self.assertEqual([name for name, _ in passes], ["early", "middle", "late"])
        self.assertEqual(dr.drlib.rehearsal_plan(180), dr.drlib.plan_passes(180))
        for duration_s in (180 - 1e-7, 180 + 1e-7):
            with self.subTest(duration_s=duration_s):
                self.assertEqual(dr.drlib.rehearsal_plan(duration_s),
                                 dr.drlib.plan_passes(duration_s))
        for duration_s in (180 - 1e-5, 180 + 1e-5):
            with self.subTest(duration_s=duration_s), self.assertRaises(ValueError):
                dr.drlib.rehearsal_plan(duration_s)

    def test_direct_walker_refuses_before_changing_results(self):
        argv = ["demo_rehearsal.py", "--duration-min", "10", "--url", "http://fixture/",
                "--trace-dir", "/unused", "--daemon-log", "/unused"]
        with patch.object(sys, "argv", argv), patch.object(dr.lib, "reset_output_dir") as reset, \
                contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(dr.main(), 1)
            self.assertIn("REFUSING rehearsal plan", output.getvalue())
            reset.assert_not_called()


class CompletionWaitControls(HangGuardTestCase):
    def test_shell_completion_waits_reject_hangs_and_fast_wrong_results(self):
        real_run = subprocess.run
        for name in ("launch_argv", "check_plan_preflight"):
            for hanging in (True, False):
                with self.subTest(wait=name, hanging=hanging), tempfile.TemporaryDirectory() as tmp:
                    reached = Path(tmp) / "reached"
                    stub = ("from pathlib import Path; import os, signal; "
                            f"Path({str(reached)!r}).touch(); "
                            "os.kill(os.getppid(), signal.SIGUSR1); " +
                            ("signal.pause()" if hanging else "raise SystemExit(7)"))

                    def command(*args, **kwargs):
                        return real_run([sys.executable, "-c", stub], **kwargs)

                    def exercise(case):
                        getattr(WorkloadLifetime, name)(case, tmp)

                    # Arm the shortened control deadline on the child's ready
                    # event, so a loaded Mac cannot spend it on Python startup.
                    previous = signal.signal(signal.SIGUSR1, lambda *_:
                        signal.setitimer(signal.ITIMER_REAL, 1 if hanging else 30))
                    try:
                        with patch.object(subprocess, "run", side_effect=command):
                            result = run_wait_control(exercise, 300)
                    finally:
                        signal.signal(signal.SIGUSR1, previous)
                    self.assertTrue(reached.exists(), "stub must reach the intended wait")
                    self.assertFalse(result.wasSuccessful())
                    self.assertFalse(result.errors, result.errors)
                    failures = "\n".join(text for _, text in result.failures)
                    if hanging:
                        self.assertIn("outer hang guard expired", failures)
                    else:
                        self.assertNotIn("outer hang guard expired", failures)
                        self.assertIn("AssertionError", failures)
                    print(f"negative {name}: {'outer guard caught hang' if hanging else 'wrong exit rejected immediately'}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
