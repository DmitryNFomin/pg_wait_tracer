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
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

import demo_rehearsal as dr
import live_loop_workload as loop

ROOT = Path(__file__).resolve().parents[1]


class WorkloadLifetime(unittest.TestCase):
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
                             capture_output=True, text=True, timeout=10)
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
            ticks, schedule = dr.drlib.plan_passes(2100)
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
            with patch.object(dr.time, "monotonic", side_effect=lambda: now[0]), \
                    patch.object(dr.time, "sleep", side_effect=lambda s: now.__setitem__(0, now[0] + s)), \
                    patch.object(dr.live_smoke, "run_tab", side_effect=tab), \
                    patch.object(dr.live_smoke, "_pid_is_live",
                                 side_effect=lambda pid: now[0] < {101: pg_end, 102: loop_end}[pid]), \
                    contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                try:
                    results, _ = dr._run_passes(
                        None, "http://fixture/", tmp, ticks, schedule, 60,
                        101, 102, None, 0, viewport={}, device_scale_factor=1)
                except SystemExit as exc:
                    self.fail(f"delayed walk voided (exit={exc.code}):\n{output.getvalue()}")
            self.assertGreater(now[0], 2100, "regression must actually cross the old deadline")
            self.assertEqual(len(walked), 3 * len(dr.lib.TABS))
            summary = dr.drlib.build_demo_summary(results, {"fixture": {"ok": True}})
            self.assertFalse(summary["ok"], "selector failures must remain failures")
            self.assertEqual(summary["failed"], [f"late/{tab}" for tab in dr.lib.TABS[:3]])
            print(f"delayed walk: {now[0]:.0f}s > old 2100s; "
                  f"{len(walked)} tabs; RED verdict, no liveness void")

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

    def test_impossible_plan_refuses_before_postgres_discovery(self):
        for duration in ("0", "nan", "inf", "nonsense", "0.01", "10"):
            with self.subTest(duration=duration):
                run = subprocess.run(["bash", "tests/demo_rehearsal.sh"], cwd=ROOT,
                                     env=dict(os.environ, DURATION_MIN=duration),
                                     capture_output=True, text=True, timeout=10)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn("REFUSING rehearsal plan", run.stdout + run.stderr)
                self.assertNotIn("postmaster", run.stdout + run.stderr)

    def test_real_plans_and_explicit_mechanics_self_test_are_accepted(self):
        for duration in (30, 35, 45):
            ticks, passes = dr.drlib.rehearsal_plan(duration * 60)
            self.assertEqual(ticks, dr.lib.MIN_TICKS)
            self.assertEqual([name for name, _ in passes], ["early", "middle", "late"])
        self.assertEqual(dr.drlib.rehearsal_plan(180), dr.drlib.plan_passes(180))

    def test_direct_walker_refuses_before_changing_results(self):
        argv = ["demo_rehearsal.py", "--duration-min", "10", "--url", "http://fixture/",
                "--trace-dir", "/unused", "--daemon-log", "/unused"]
        with patch.object(sys, "argv", argv), patch.object(dr.lib, "reset_output_dir") as reset, \
                contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(dr.main(), 1)
            self.assertIn("REFUSING rehearsal plan", output.getvalue())
            reset.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
