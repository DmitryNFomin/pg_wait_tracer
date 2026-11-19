#!/usr/bin/env python3
"""Capture-smoke failure diagnostics, without PostgreSQL or BPF.

Run the real query_event phase and checks with fake workload/daemon processes.
The daemon writes to the phase's real TemporaryFile; a non-pgss query id makes
only the core phantom-id check fail (#614).

The #966 tests run the real Phase 1b (Mode A + the Mode C drill) with a fake
workload, daemon, control socket and pgwt-server, and force the drill's FAIL
path to show its diagnostics block prints and its trace dir is kept.
"""
import contextlib
import io
import os
import socket
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


TRUTH = 7777
FIRE_NS = 1_800_000_000_000_000_000


class FakeWorkload:
    LOCK_TABLE = smoke.Workload.LOCK_TABLE
    instances = []

    def __init__(self):
        self.backend_pids = {}
        self.discover_args = []
        FakeWorkload.instances.append(self)

    def open_sessions(self, discover_pids=False):
        self.discover_args.append(discover_pids)
        if discover_pids:
            self.backend_pids = {"sleeper": 31, "holder": 32, "waiter": 33}

    def fire(self, sleep_s=3, verify=True):
        return True

    def release(self):
        pass

    def stop(self, keep_table=False):
        pass


class QattrModeCDiagnosticsTests(unittest.TestCase):
    """#966: diagnostics on the qattr Mode C drill's FAIL path only."""

    def run_qattr_phase(self, mode_c_lock_ms, mode='full', observed=None,
                        timeline=None, metrics=None, keep_root=None,
                        rename_fails=False):
        FakeWorkload.instances = []
        root = tempfile.mkdtemp(prefix='pgwt_966_test_')
        self.addCleanup(lambda: smoke.shutil.rmtree(root, ignore_errors=True))
        dir_a = os.path.join(root, 'pgwt_smoke_qattr_a')
        dir_c = os.path.join(root, 'pgwt_smoke_qattr_c_x')
        for d in (dir_a, dir_c):
            os.mkdir(d)
        with open(os.path.join(dir_c, 'trace_0001.pgwt'), 'wb') as f:
            f.write(b'fake trace bytes')
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(os.path.join(dir_c, 'pgwt.sock'))
        sock.close()
        keep_root = keep_root or os.path.join(root, 'kept')
        ctl_calls = []
        server_calls = []
        rm_calls = []

        def popen(argv, **kwargs):
            self.assertEqual(argv[0], smoke.TRACER)
            if '--query-id' in argv:
                line = (f"Lock:relation 1 {mode_c_lock_ms} {mode_c_lock_ms} "
                        f"{mode_c_lock_ms} 99.9\n")
            else:
                line = f"{TRUTH} Lock:relation 1 8008.0 0.0\n"
            kwargs['stdout'].write(line.encode())
            kwargs['stderr'].write(b"daemon stderr line\n")
            kwargs['stdout'].flush()
            kwargs['stderr'].flush()
            return Mock()

        def observe(proc, predicate, timeout_s, interval_s=0.05):
            result = predicate()
            return result if observed is None else observed

        def ctl_query(trace_dir, cmd, timeout=5):
            ctl_calls.append((trace_dir, cmd))
            if isinstance(metrics, Exception):
                raise metrics
            return metrics

        def server_query(trace_dir, cmd, extra=None, timeout_s=None):
            server_calls.append((trace_dir, cmd, extra))
            if isinstance(timeline, Exception):
                raise timeline
            return timeline

        def run(argv, **kwargs):
            rm_calls.append(argv)
            return Mock(returncode=0)

        real_rename = os.rename

        def rename(src, dst):
            if rename_fails:
                raise OSError(18, 'Invalid cross-device link')
            return real_rename(src, dst)

        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            replacements = {
                'tests_run': 0, 'tests_passed': 0, 'tests_failed': 0,
                'psql': Mock(return_value=''),
                'Workload': FakeWorkload,
                'pgss_query_id_for': Mock(return_value=TRUTH),
                'wait_for_control_socket': Mock(return_value=True),
                'wait_for_observation': observe,
                'terminate_and_wait': Mock(),
                'ctl_query': ctl_query,
                'server_query': server_query,
                'qattr_diag_keep_root': Mock(return_value=keep_root),
            }
            for name, replacement in replacements.items():
                stack.enter_context(patch.object(smoke, name, replacement))
            stack.enter_context(patch.object(smoke.subprocess, 'Popen', popen))
            stack.enter_context(patch.object(smoke.subprocess, 'run', run))
            stack.enter_context(patch.object(
                smoke.tempfile, 'mkdtemp', side_effect=[dir_a, dir_c]))
            stack.enter_context(patch.object(smoke.time, 'sleep'))
            stack.enter_context(patch.object(
                smoke.time, 'time_ns',
                side_effect=(FIRE_NS + i * 1_000_000_000 for i in range(100))))
            stack.enter_context(patch.object(smoke.os, 'rename', rename))
            with contextlib.redirect_stdout(output):
                smoke.phase_live_query_attribution(
                    123, mode, 17, core=False)
            result = {
                'output': output.getvalue(),
                'failures': smoke.tests_failed,
                'passed': smoke.tests_passed,
            }
        result.update(dir_c=dir_c, keep_root=keep_root, ctl_calls=ctl_calls,
                      server_calls=server_calls, rm_calls=rm_calls)
        return result

    @staticmethod
    def timeline_response():
        lock_start = FIRE_NS + 5_000_000
        return {
            'id': 2, 'fidelity': 'exact', 'pids': [33], 'truncated': False,
            'total_count': 3,
            'events': [
                {'s': FIRE_NS - 50_000_000, 'd': 55_000_000,
                 'n': 'Client:ClientRead', 'p': 33, 'q': '0'},
                {'s': lock_start, 'd': 1_367_700_000, 'n': 'Lock:relation',
                 'p': 33, 'q': '0'},
                {'s': lock_start + 1_367_700_000, 'd': 1_000_000, 'n': 'CPU',
                 'p': 33, 'q': str(TRUTH)},
            ],
        }

    @staticmethod
    def metrics_response():
        return {'ok': True, 'uptime_s': 40.0, 'exact_probe_attached_mask': 3,
                'ringbuf_drops_total': 0, 'wp_attach_failures_total': 0,
                'live_query_unattributed_ns_total': 6_600_000_000,
                'summary_query_unattributed_ns_total': 0,
                'events_total': 1234}

    def mode_c_fail_lines(self, output):
        return [line for line in output.splitlines()
                if 'FAIL:' in line and 'Mode C' in line]

    def test_forced_fail_prints_block_and_keeps_trace_dir(self):
        r = self.run_qattr_phase(1367.7, timeline=self.timeline_response(),
                                 metrics=self.metrics_response())
        out = r['output']
        self.assertEqual(r['failures'], 1, out)
        self.assertEqual(len(self.mode_c_fail_lines(out)), 1, out)
        self.assertEqual(out.count('#966 qattr Mode C diagnostics'), 1, out)
        self.assertNotIn('DIAG UNAVAILABLE', out)
        # pids discovered for Mode C only; harness marks on the trace clock
        self.assertEqual([w.discover_args for w in FakeWorkload.instances],
                         [[False], [True]])
        self.assertIn('diag: pids holder=32 waiter=33 sleeper=31', out)
        self.assertIn(f'fire_ns={FIRE_NS}', out)
        # the waiter's Lock:relation interval through pgwt-server
        self.assertEqual(r['server_calls'],
                         [(r['dir_c'], 'session_timeline', {'pid': 33})])
        self.assertIn('reader=pgwt-server session_timeline pid=33', out)
        self.assertIn(f'LOCK Lock:relation start={FIRE_NS + 5_000_000} '
                      f'end={FIRE_NS + 5_000_000 + 1_367_700_000} '
                      f'dur=1367.7ms query_id=0 start-fire=+5.0ms', out)
        # one metrics call, before terminate, filtered to the counters
        self.assertEqual(r['ctl_calls'], [(r['dir_c'], 'metrics')])
        for key in ('exact_probe_attached_mask=3', 'ringbuf_drops_total=0',
                    'wp_attach_failures_total=0',
                    'live_query_unattributed_ns_total=6600000000'):
            self.assertIn(key, out)
        self.assertNotIn('events_total=1234', out)
        # the trace dir is kept (moved), not removed
        kept = os.path.join(r['keep_root'], os.path.basename(r['dir_c']))
        self.assertIn(f'diag: trace dir kept at {kept}', out)
        self.assertTrue(os.path.isfile(os.path.join(kept, 'trace_0001.pgwt')))
        self.assertFalse(os.path.exists(r['dir_c']))
        self.assertNotIn(['rm', '-rf', r['dir_c']], r['rm_calls'])

    def test_cross_device_keep_copies_without_the_socket(self):
        r = self.run_qattr_phase(1367.7, timeline=self.timeline_response(),
                                 metrics=self.metrics_response(),
                                 rename_fails=True)
        kept = os.path.join(r['keep_root'], os.path.basename(r['dir_c']))
        self.assertIn(f'diag: trace dir kept at {kept}', r['output'])
        self.assertTrue(os.path.isfile(os.path.join(kept, 'trace_0001.pgwt')))
        self.assertFalse(os.path.exists(os.path.join(kept, 'pgwt.sock')))
        self.assertFalse(os.path.exists(r['dir_c']))

    def test_pass_keeps_todays_cleanup_and_takes_no_metrics(self):
        r = self.run_qattr_phase(8008.0, timeline=self.timeline_response(),
                                 metrics=self.metrics_response())
        out = r['output']
        self.assertEqual(r['failures'], 0, out)
        self.assertNotIn('#966', out)
        self.assertNotIn('DIAG', out)
        self.assertEqual(r['ctl_calls'], [])
        self.assertEqual(r['server_calls'], [])
        self.assertIn(['rm', '-rf', r['dir_c']], r['rm_calls'])

    def test_tiered_fail_prints_no_block(self):
        r = self.run_qattr_phase(1367.7, mode='tiered',
                                 timeline=self.timeline_response(),
                                 metrics=self.metrics_response())
        self.assertEqual(r['failures'], 1, r['output'])
        self.assertNotIn('#966', r['output'])
        self.assertEqual(r['ctl_calls'], [])
        self.assertIn(['rm', '-rf', r['dir_c']], r['rm_calls'])

    def test_broken_diagnostics_leave_a_fail_a_fail(self):
        # reader, control socket and keep dir all fail: the block reports
        # each as DIAG UNAVAILABLE, the verdict stays one FAIL, and the
        # trace dir is left in place rather than deleted.
        root_file = tempfile.NamedTemporaryFile(delete=False)
        root_file.close()
        self.addCleanup(os.unlink, root_file.name)
        r = self.run_qattr_phase(
            1367.7, timeline=OSError('pgwt-server missing'),
            metrics=ConnectionRefusedError('no socket'),
            keep_root=os.path.join(root_file.name, 'under_a_file'))
        out = r['output']
        self.assertEqual(r['failures'], 1, out)
        self.assertEqual(len(self.mode_c_fail_lines(out)), 1, out)
        self.assertIn('DIAG UNAVAILABLE: waiter intervals: OSError: '
                      'pgwt-server missing', out)
        self.assertIn('DIAG UNAVAILABLE: metrics: RuntimeError: metrics call '
                      'failed: ConnectionRefusedError: no socket', out)
        self.assertIn('DIAG UNAVAILABLE: keep trace dir:', out)
        self.assertIn(f'left in place at {r["dir_c"]}', out)
        self.assertTrue(os.path.isdir(r['dir_c']))
        self.assertNotIn(['rm', '-rf', r['dir_c']], r['rm_calls'])

    def test_broken_diagnostics_leave_a_pass_a_pass(self):
        # the live poll never confirms the floor (metrics are taken and the
        # call raises) but the final output passes: no block, still a PASS.
        r = self.run_qattr_phase(
            8008.0, observed=False, timeline=OSError('unused'),
            metrics=ConnectionRefusedError('no socket'))
        self.assertEqual(r['failures'], 0, r['output'])
        self.assertEqual(r['ctl_calls'], [(r['dir_c'], 'metrics')])
        self.assertNotIn('DIAG', r['output'])
        self.assertIn(['rm', '-rf', r['dir_c']], r['rm_calls'])


if __name__ == '__main__':
    unittest.main()
