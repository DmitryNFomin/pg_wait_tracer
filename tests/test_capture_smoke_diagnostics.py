#!/usr/bin/env python3
"""Capture-smoke failure diagnostics, without PostgreSQL or BPF.

Run the real query_event phase and checks with fake workload/daemon processes.
The daemon writes to the phase's real TemporaryFile; a non-pgss query id makes
only the core phantom-id check fail (#614).
"""
import contextlib
import io
import tempfile
import unittest
from unittest.mock import Mock, patch

import test_capture_smoke as smoke


class CaptureSmokeDiagnosticsTests(unittest.TestCase):
    def run_query_phase(self, view_id, prior_failures=0):
        diagnostic = (
            "excluding PgBackendStatus validation PID 12345 "
            "from startup capture enrollment")

        def popen(argv, **kwargs):
            if argv[0] == smoke.TRACER:
                kwargs['stdout'].write(
                    f"{view_id} Lock:relation 1 100.0 0.0\n".encode())
                kwargs['stderr'].write((diagnostic + '\n').encode())
                kwargs['stdout'].flush()
                kwargs['stderr'].flush()
            return Mock()

        def observe(proc, predicate, timeout_s):
            self.assertTrue(predicate(), "fake daemon must emit parsed query ids")
            return True

        output = io.StringIO()
        with tempfile.TemporaryDirectory() as trace_dir, \
                contextlib.ExitStack() as stack:
            replacements = {
                'tests_run': 0,
                'tests_passed': 0,
                'tests_failed': prior_failures,
                'psql': Mock(return_value=''),
                'wait_for_tagged_backend_count': Mock(return_value=4),
                'wait_for_control_socket': Mock(return_value=True),
                'wait_for_observation': observe,
                'terminate_and_wait': Mock(),
                'pgss_query_ids': Mock(return_value={1}),
                'pgss_phantom_truth_ids': Mock(return_value={1}),
                'pgss_all_rows': Mock(return_value={1: (3, 'pgbench query')}),
            }
            for name, replacement in replacements.items():
                stack.enter_context(patch.object(smoke, name, replacement))
            stack.enter_context(patch.object(smoke.subprocess, 'Popen', popen))
            stack.enter_context(patch.object(
                smoke.subprocess, 'run', return_value=Mock(returncode=0)))
            stack.enter_context(patch.object(
                smoke.tempfile, 'mkdtemp', return_value=trace_dir))
            with contextlib.redirect_stdout(output):
                smoke.phase_live_query_event(
                    123, 'tiered', 14, 'pgwt_smoke_test', core=True)
            failures = smoke.tests_failed - prior_failures
        return output.getvalue(), failures, diagnostic

    def test_phantom_id_failure_surfaces_daemon_stderr(self):
        output, failures, diagnostic = self.run_query_phase(view_id=2)
        failed_lines = [line for line in output.splitlines() if 'FAIL:' in line]
        self.assertEqual(failures, 1, output)
        self.assertEqual(len(failed_lines), 1, output)
        self.assertIn('[core] query_event view ids', failed_lines[0])
        self.assertIn('no phantoms', failed_lines[0])
        self.assertIn('matched=0', failed_lines[0])
        self.assertEqual(output.count(diagnostic), 1, output)
        self.assertEqual(output.count('daemon stderr tail:'), 1, output)

    def test_passing_phase_keeps_daemon_stderr_quiet(self):
        for prior_failures in (0, 1):
            with self.subTest(prior_failures=prior_failures):
                output, failures, diagnostic = self.run_query_phase(
                    view_id=1, prior_failures=prior_failures)
                self.assertEqual(failures, 0, output)
                self.assertNotIn(diagnostic, output)


if __name__ == '__main__':
    unittest.main()
