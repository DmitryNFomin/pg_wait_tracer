#!/usr/bin/env python3
"""Real sockets + real driver/launcher shell, with capture/build/SSH replaced.

--tree DIR runs the same assertions against an older exported tree for REDs.
Never contacts a box or starts a real workload. Each fake process is tracked.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import signal
import time
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--tree', type=Path, default=HERE.parent)
args, remaining = parser.parse_known_args()
TREE = args.tree.resolve()
sys.argv[1:] = remaining
PYTHON = sys.executable


def executable(path, source):
    path.write_text(source)
    path.chmod(0o755)


def await_file(path, proc=None):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if path.exists():
            return
        if proc and proc.poll() is not None:
            raise AssertionError(f'fixture exited {proc.returncode} before {path}')
        time.sleep(.01)
    raise AssertionError(f'timed out waiting for {path}')


# The same asset bytes as the tree: even the production served-asset guard
# cannot distinguish this foreign listener. /session returns a real HTTP 200.
HTTP = r"""
import http.server, json, os, pathlib, sys, time
port, root, ready, trigger = sys.argv[1:]
if trigger:
    while not pathlib.Path(trigger).exists(): time.sleep(.01)
root = pathlib.Path(root)
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        if self.path == '/session': body = b'{}'
        elif self.path == '/embedded-assets':
            paths = sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file())
            body = json.dumps(dict(paths=paths, count=len(paths))).encode()
        else:
            path = root / self.path.lstrip('/')
            if not path.is_file(): self.send_error(404); return
            body = path.read_bytes()
        self.send_response(200); self.end_headers(); self.wfile.write(body)
server = http.server.HTTPServer(('127.0.0.1', int(port)), Handler)
pathlib.Path(ready).write_text(str(os.getpid()))
server.timeout = .1
end = time.monotonic() + 30
while time.monotonic() < end: server.handle_request()
"""


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='harness-ownership-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'repo'
        for directory in ('tests', 'scripts', 'web/static', 'bin'):
            (self.repo / directory).mkdir(parents=True, exist_ok=True)
        for name in ('demo_rehearsal.sh', 'ui_live_smoke.sh', 'live_daemon_lib.sh',
                     'bridge_owner.py', 'demo_rehearsal_lib.py', 'ui_live_smoke_lib.py'):
            if (TREE / 'tests' / name).exists():
                shutil.copy(TREE / 'tests' / name, self.repo / 'tests' / name)
        for name in ('demo-rehearsal.sh', 'demo-rehearsal-remote-run.sh'):
            shutil.copy(TREE / 'scripts' / name, self.repo / 'scripts' / name)
        # Only PG discovery/provisioning are replaced; readiness, asset
        # verification, driver flow and cleanup remain production code.
        (self.repo / 'tests/testutil.sh').write_text('find_postmaster() { echo 999999; }\n')
        with (self.repo / 'tests/live_daemon_lib.sh').open('a') as f:
            f.write('\nderive_pgport() { :; }\ncheck_pgbench_provisioned() { :; }\n')
        (self.repo / 'web/static/asset.js').write_text('same build bytes\n')
        self.http = self.base / 'fixture_http.py'
        self.http.write_text(HTTP)
        # Nothing runs as root; these are process-lifetime fixtures only.
        hold = f'#!{PYTHON}\nimport time\ntime.sleep(120)\n'
        executable(self.repo / 'pgwt-server', hold)
        executable(self.repo / 'bin/pgbench', hold)
        (self.repo / 'tests/live_loop_workload.py').write_text(hold)
        executable(self.repo / 'pg_wait_tracer', f'#!{PYTHON}\n' + r"""
import socket, sys, time
from pathlib import Path
trace = Path(sys.argv[sys.argv.index('-T') + 1])
sock = socket.socket(socket.AF_UNIX); sock.bind(str(trace / 'pgwt.sock')); sock.listen()
time.sleep(120)
""")
        # Faulting bridge stays alive AFTER the bind error, reproducing the
        # exact liveness/foreign-HTTP race; healthy mode really owns its port.
        executable(self.repo / 'web/pgwt', f'#!{PYTHON}\n' + r"""
import errno, os, pathlib, socket, subprocess, sys, time
port = sys.argv[sys.argv.index('--port') + 1]
pathlib.Path(os.environ['BRIDGE_STARTED']).write_text(str(os.getpid()))
if os.environ.get('FOREIGN') == '1':
    ready = pathlib.Path(os.environ['HTTP_READY'])
    while not ready.exists(): time.sleep(.01)
    try:
        sock = socket.socket(); sock.bind(('127.0.0.1', int(port)))
    except OSError as exc:
        if exc.errno != errno.EADDRINUSE: raise
        print('listen tcp 127.0.0.1:' + port + ': bind: address already in use (' + str(exc) + ')', flush=True)
    else: raise AssertionError('foreign fixture did not occupy the port')
    time.sleep(3)  # alive throughout the OLD readiness check
    sys.exit(1)
os.execv(sys.executable, [sys.executable, os.environ['HTTP_SCRIPT'], port,
    os.environ['STATIC_ROOT'], os.environ['HTTP_READY'], ''])
""")
        # Actual walker exec records argv, then returns a failure to exercise
        # teardown on unsuccessful runs as well as successful readiness.
        walker = r"""
import json, os, pathlib, sys
pathlib.Path(os.environ['WALK_ARGV']).write_text(json.dumps(sys.argv))
bridge_pid = int(pathlib.Path(os.environ['BRIDGE_STARTED']).read_text())
try: os.kill(bridge_pid, 0); alive = True
except ProcessLookupError: alive = False
print('WALK_REACHED bridge_live=' + str(alive) + ' argv=' + json.dumps(sys.argv), flush=True)
if os.environ.get('WALK_BLOCK') == '1':
    import time
    time.sleep(120)
sys.exit(int(os.environ.get('WALK_RC', '1')))
"""
        for name in ('demo_rehearsal.py', 'ui_live_smoke.py'):
            (self.repo / 'tests' / name).write_text(walker)
        for name in ('ssh', 'make', 'bpftool', 'flock'):
            executable(self.repo / 'bin' / name, '#!/bin/sh\nexit 0\n')
        executable(self.repo / 'bin/nproc', '#!/bin/sh\necho 1\n')
        executable(self.repo / 'bin/sysctl', '#!/bin/sh\nexit 1\n')
        executable(self.repo / 'bin/setsid', '#!/usr/bin/env bash\n[[ "$1" == -w ]] && shift\nexec "$@"\n')
        executable(self.repo / 'bin/sudo', '#!/usr/bin/env bash\nwhile [[ "$1" == *=* ]]; do export "$1"; shift; done\nexec bash "$@"\n')
        # BSD mktemp treats XXXXXX.log literally; keep all fixture logs
        # unique and inside this test's directory on either platform.
        executable(self.repo / 'bin/mktemp', f'#!{PYTHON}\n' + r"""
import os, sys, tempfile
if '-d' in sys.argv:
    print(tempfile.mkdtemp(dir=os.environ['FIXTURE_TMP']))
else:
    fd, path = tempfile.mkstemp(dir=os.environ['FIXTURE_TMP']); os.close(fd); print(path)
""")
        # Small waits keep the integration suite cheap; the bridge's Python
        # delay is deliberately unaffected and remains alive at the walk.
        executable(self.repo / 'bin/sleep', f'#!{PYTHON}\nimport sys,time\ntime.sleep(min(float(sys.argv[1]), .05))\n')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.env = dict(os.environ, PATH=f"{self.repo / 'bin'}:{os.environ['PATH']}",
            PGWT_DEMO_PORT=str(self.port), PGWT_UI_LIVE_PORT=str(self.port),
            BRIDGE_STARTED=str(self.base / 'bridge.started'), HTTP_READY=str(self.base / 'http.ready'),
            HTTP_SCRIPT=str(self.http), STATIC_ROOT=str(self.repo / 'web/static'),
            WALK_ARGV=str(self.base / 'walk.argv'), DURATION_MIN='35', FIXTURE_TMP=str(self.base))
        self.env.pop('DIAG_DATA', None)

    def foreign(self, late=False):
        proc = subprocess.Popen([PYTHON, str(self.http), str(self.port),
            str(self.repo / 'web/static'), self.env['HTTP_READY'],
            self.env['BRIDGE_STARTED'] if late else ''],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def stop():
            if proc.poll() is None: proc.terminate()
            proc.wait(timeout=5)
        self.addCleanup(stop)
        if not late: await_file(Path(self.env['HTTP_READY']), proc)
        self.env['FOREIGN'] = '1'
        return proc

    def driver(self, name):
        run = subprocess.run(['bash', f'tests/{name}.sh', '--pid', '999999'],
            cwd=self.repo, env=self.env, text=True, capture_output=True, timeout=15)
        return run, run.stdout + run.stderr

    def exercise_foreign(self, driver, late):
        foreign = self.foreign(late)
        start = time.monotonic()
        run, output = self.driver(driver)
        elapsed = time.monotonic() - start
        walked = Path(self.env['WALK_ARGV']).exists()
        print(f'{driver} {"foreign-after-preflight" if late else "occupied HTTP-200"}: '
              f'rc={run.returncode} walk={walked} elapsed={elapsed:.2f}s', flush=True)
        print('\n'.join(line for line in output.splitlines()
                        if any(s in line for s in ('REFUSE', 'WALK_REACHED', 'bind:', 'served-asset check: OK'))), flush=True)
        self.assertFalse(walked, output)
        self.assertNotEqual(run.returncode, 0, output)
        self.assertIn('REFUSE', output)
        self.assertIsNone(foreign.poll(), 'must not kill the foreign owner')
        if not late:
            self.assertLess(elapsed, 2, output)
            self.assertFalse(Path(self.env['BRIDGE_STARTED']).exists())
        else:
            log = self.repo / f'tests/results/{"demo_rehearsal" if driver == "demo_rehearsal" else "ui_live"}/bridge.log'
            self.assertIn('bind: address already in use', log.read_text())

    def test_rehearsal_occupied_port(self): self.exercise_foreign('demo_rehearsal', False)
    def test_smoke_occupied_port(self): self.exercise_foreign('ui_live_smoke', False)
    def test_rehearsal_foreign_200_while_new_pid_alive(self): self.exercise_foreign('demo_rehearsal', True)
    def test_smoke_foreign_200_while_new_pid_alive(self): self.exercise_foreign('ui_live_smoke', True)

    def test_owned_bridge_reaches_walk_and_cleanup_keeps_log(self):
        for driver, rc in (('demo_rehearsal', '0'), ('ui_live_smoke', '1')):
            self.env['WALK_RC'] = rc
            with self.subTest(driver=driver):
                run, output = self.driver(driver)
                self.assertTrue(Path(self.env['WALK_ARGV']).exists(), output)
                pid = int(Path(self.env['BRIDGE_STARTED']).read_text())
                with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
                result = 'demo_rehearsal' if driver == 'demo_rehearsal' else 'ui_live'
                self.assertTrue((self.repo / f'tests/results/{result}/bridge.log').exists(), output)
                self.assertNotIn('survived teardown', output)
                for name in ('WALK_ARGV', 'BRIDGE_STARTED', 'HTTP_READY'):
                    Path(self.env[name]).unlink()

    def test_term_cleans_up_owned_bridge(self):
        self.env['WALK_BLOCK'] = '1'
        for driver in ('demo_rehearsal', 'ui_live_smoke'):
            with self.subTest(driver=driver):
                log = self.base / 'term.log'
                with log.open('w') as out:
                    proc = subprocess.Popen(['bash', f'tests/{driver}.sh', '--pid', '999999'],
                        cwd=self.repo, env=self.env, stdout=out, stderr=subprocess.STDOUT)
                    try:
                        try:
                            await_file(Path(self.env['WALK_ARGV']), proc)
                        except AssertionError:
                            self.fail(log.read_text())
                        pid = int(Path(self.env['BRIDGE_STARTED']).read_text())
                        proc.terminate()
                        self.assertEqual(proc.wait(timeout=15), 143, log.read_text())
                        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
                        result = 'demo_rehearsal' if driver == 'demo_rehearsal' else 'ui_live'
                        self.assertTrue((self.repo / f'tests/results/{result}/bridge.log').exists())
                    finally:
                        if proc.poll() is None: proc.kill(); proc.wait(timeout=5)
                for name in ('WALK_ARGV', 'BRIDGE_STARTED', 'HTTP_READY'):
                    Path(self.env[name]).unlink()

    def test_dead_bridge_leader_does_not_leave_its_child(self):
        child_ready = self.base / 'child.ready'
        parent = self.base / 'parent.py'
        parent.write_text("""
import os, pathlib, signal, sys, time
pid = os.fork()
if pid == 0:
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))
    time.sleep(120)
else:
    while not pathlib.Path(sys.argv[1]).exists(): time.sleep(.01)
    os._exit(0)
""")
        command = f"""
source {shlex.quote(str(self.repo / 'tests/live_daemon_lib.sh'))}
run_bridge {shlex.quote(PYTHON)} {shlex.quote(str(parent))} {shlex.quote(str(child_ready))} &
pid=$!
wait "$pid"
stop_bridge "$pid"
"""
        try:
            run = subprocess.run(['bash', '-c', command], env=self.env,
                                 capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            child = int(child_ready.read_text())
            with self.assertRaises(ProcessLookupError): os.kill(child, 0)
        finally:
            if child_ready.exists():
                try: os.kill(int(child_ready.read_text()), signal.SIGKILL)
                except ProcessLookupError: pass

    def test_diag_data_reaches_executed_box_argv(self):
        source = (TREE / 'scripts/demo-rehearsal.sh').read_text()
        # Execute the launcher's actual SSH statement. Only the transport is
        # replaced: its remote shell command really runs the remote runner,
        # sudo forwarding, driver, and finally the Python argv recorder.
        start = source.index('    launch_out=$(ssh ')
        end = source.index('\n    if [[ "$launch_out"', start)
        statement = source[start:end]
        remote = self.repo / 'scripts/demo-rehearsal-remote-run.sh'
        # Platform-only probes use local fixture files/tools; the command
        # arguments and every forwarding hop remain production code.
        remote.write_text(remote.read_text().replace('/sys/kernel/btf/vmlinux', str(self.repo / 'pgwt-server'))
                          .replace('/tmp/pgwt-box-check.lock', str(self.base / 'fixture.lock')))
        executable(self.repo / 'bin/ssh', '#!/usr/bin/env bash\nexec bash -c "${*: -1}"\n')
        for mode in ('1', '0'):
            command = f"""
set -uo pipefail
DURATION_MIN=35; demo_rehearsal_start_epoch=fixture; PG=18
RETAIN_TRACE=1; TAG=fixture-tag; DIAG_DATA={mode}
remote_dir={shlex.quote(str(self.repo))}; target=local-fixture; ssh_strict=''; log={shlex.quote(str(self.base / 'ssh.log'))}
{statement}
"""
            run = subprocess.run(['bash', '-c', command], cwd=self.repo, env=self.env,
                                 capture_output=True, text=True, timeout=10)
            await_file(self.repo / 'rehearsal.done')
            self.assertTrue(Path(self.env['WALK_ARGV']).exists(), (self.repo / 'rehearsal.out').read_text() + run.stderr)
            argv = json.loads(Path(self.env['WALK_ARGV']).read_text())
            print(f'DIAG_DATA={mode} executed box argv: {json.dumps(argv)}', flush=True)
            self.assertEqual('--diag-data' in argv, mode == '1')
            self.assertIn('fixture-tag', argv)
            for path in ('rehearsal.done',): (self.repo / path).unlink()
            for name in ('WALK_ARGV', 'BRIDGE_STARTED', 'HTTP_READY'):
                Path(self.env[name]).unlink()

    def test_overview_zero_response_with_one_row_refuses(self):
        # Use the real builder/formatter to render the server's one-row zero
        # response, then evaluate the driver's actual render predicate in
        # Node. No replica of duration formatting or render predicate.
        spec = importlib.util.spec_from_file_location('candidate_smoke', TREE / 'tests/ui_live_smoke.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        result = subprocess.check_output(['node', '--input-type=module', '-e', """
import {buildTableModel} from './web/static/lib/table.js';
import {overviewConfig} from './web/static/lib/builders/table-configs.js';
const check = eval('(' + JSON.parse(process.argv[1]) + ')');
const responses = [0, 0.001, 10, null].map(ms => ({rows:[{name:'DB Time', ms, indent:0}]}));
const results = responses.map(response => {
    const model = buildTableModel(overviewConfig, response.rows, null);
    // DOM adapter contains the production builder's cell text; duration
    // formatting and the gate itself are both real, not reimplemented.
    const rows = model.rows.map(row => ({cells: row.cells.map(c => ({textContent:c.html}))}));
    globalThis.document = {querySelectorAll: () => rows};
    return {response, result:check()};
});
console.log(JSON.stringify(results));
""", json.dumps(module.PANEL_CHECKS['overview'])], cwd=TREE, text=True)
        for record, expected in zip(json.loads(result), (False, True, True, False)):
            print(f'Overview one-row response {record}', flush=True)
            self.assertEqual(record['result']['ok'], expected)



class ListenerInspectionTests(unittest.TestCase):
    def test_linux_ss_ownership_and_fail_closed_paths(self):
        spec = importlib.util.spec_from_file_location('bridge_owner', TREE / 'tests/bridge_owner.py')
        owner = importlib.util.module_from_spec(spec); spec.loader.exec_module(owner)
        cases = [
            ('', 1),
            ('LISTEN 0 4096 127.0.0.1:8385 0.0.0.0:* users:(("pgwt",pid=42,fd=7))', 0),
            ('LISTEN 0 4096 [::1]:8385 [::]:* users:(("pgwt",pid=41,fd=7))', 2),
            ('LISTEN 0 4096 127.0.0.1:8385 0.0.0.0:*', 'unavailable'),
            ('LISTEN 0 4096 *:8385 *:* users:(("pgwt",pid=42,fd=7),("other",pid=41,fd=7))', 2),
        ]
        with patch.object(owner.sys, 'platform', 'linux'), \
                patch.object(owner.sys, 'argv', ['bridge_owner.py', '8385', '42']):
            for output, expected in cases:
                with self.subTest(output=output), patch.object(owner.subprocess, 'run',
                        return_value=subprocess.CompletedProcess([], 0, output, '')) as command:
                    if expected == 'unavailable':
                        with self.assertRaises(RuntimeError): owner.main()
                    else: self.assertEqual(owner.main(), expected)
                    self.assertEqual(command.call_args.args[0], ['ss', '-H', '-ltnp', 'sport = :8385'])
            with patch.object(owner.subprocess, 'run', side_effect=FileNotFoundError('ss')):
                with self.assertRaises(OSError): owner.main()
            with patch.object(owner.subprocess, 'run',
                              return_value=subprocess.CompletedProcess([], 1, '', 'permission denied')):
                with self.assertRaises(RuntimeError): owner.main()


if __name__ == '__main__': unittest.main(verbosity=2)
