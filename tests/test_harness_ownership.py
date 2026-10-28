#!/usr/bin/env python3
"""Real sockets + real driver/launcher shell, with capture/build/SSH replaced.

--tree DIR runs the same assertions against an older exported tree for REDs.
Never contacts a box or starts a real workload. Each fake process is tracked.
"""
import argparse
import contextlib
import importlib.util
import io
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
                     'bridge_owner.py', 'demo_rehearsal_lib.py', 'ui_live_smoke_lib.py',
                     'testutil.py'):
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
        # #622: the harness stands in for the provisioner and declares probes.
        self.env['PGWT_EXPECT_PHASE_PROBES'] = '1'

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


PROC_HEADER = ('  sl  local_address rem_address   st tx_queue rx_queue tr '
               'tm->when retrnsmt   uid  timeout inode\n')
LISTEN_ST = '0A'


def tcp_row(port, inode, state=LISTEN_ST, addr='0100007F', sl=0):
    """One /proc/net/tcp data row, in the kernel's real column layout."""
    return (f'{sl:>4}: {addr}:{port:04X} 00000000:0000 {state} '
            f'00000000:00000000 00:00000000 00000000     0        0 {inode} '
            f'1 0000000000000000 100 0 0 10 0')


def load_owner():
    spec = importlib.util.spec_from_file_location('bridge_owner', TREE / 'tests/bridge_owner.py')
    owner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owner)
    return owner


class ListenerInspectionTests(unittest.TestCase):
    def test_linux_ss_ownership_and_fail_closed_paths(self):
        owner = load_owner()
        cases = [
            ('', 1),
            ('LISTEN 0 4096 127.0.0.1:8385 0.0.0.0:* users:(("pgwt",pid=42,fd=7))', 0),
            ('LISTEN 0 4096 [::1]:8385 [::]:* users:(("pgwt",pid=41,fd=7))', 2),
            ('LISTEN 0 4096 *:8385 *:* users:(("pgwt",pid=42,fd=7),("other",pid=41,fd=7))', 2),
        ]
        with patch.object(owner.sys, 'platform', 'linux'), \
                patch.object(owner.sys, 'argv', ['bridge_owner.py', '8385', '42']):
            for output, expected in cases:
                with self.subTest(output=output), patch.object(owner.subprocess, 'run',
                        return_value=subprocess.CompletedProcess([], 0, output, '')) as command:
                    self.assertEqual(owner.main(), expected)
                    self.assertEqual(command.call_args.args[0], ['ss', '-H', '-ltnp', 'sport = :8385'])
            # A pid-less line with no /proc to fall back to is an inspection
            # failure, never "the port is free".
            with patch.object(owner, 'PROC', '/nonexistent-proc-root'), \
                    patch.object(owner.subprocess, 'run', return_value=subprocess.CompletedProcess(
                        [], 0, 'LISTEN 0 4096 127.0.0.1:8385 0.0.0.0:*', '')):
                with self.assertRaises(owner.Unattributable): owner.main()
            with patch.object(owner.subprocess, 'run', side_effect=FileNotFoundError('ss')):
                with self.assertRaises(OSError): owner.main()
            with patch.object(owner.subprocess, 'run',
                              return_value=subprocess.CompletedProcess([], 1, '', 'permission denied')):
                with self.assertRaises(RuntimeError): owner.main()


class ProcFallbackTests(unittest.TestCase):
    """The /proc route ss uses internally, used only on a line ss left bare.

    A bare line is a race inside ss, not a broken host: ss scans /proc/*/fd
    before it asks the kernel for sockets, so a socket that starts listening
    in between is dumped with no owner. Reading the inode table first and the
    fds second cannot lose that race (see tests/bridge_owner.py).

    Every case here feeds bridge_owner.py an `ss` line WITHOUT `pid=` -- the
    only trigger for the fallback -- and then varies what /proc can answer.
    The bar is: attribute when /proc can, refuse with exit 3 when it cannot,
    and NEVER turn "cannot attribute" into "unused port" or "ours".
    """
    PORT = 8385
    OURS = 42

    def setUp(self):
        self.owner = load_owner()
        self.temp = tempfile.TemporaryDirectory(prefix='proc-fallback-')
        self.addCleanup(self.temp.cleanup)
        self.roots = 0

    def fake_proc(self, tcp=(), tcp6=None, holders=(), tcp_unreadable=False,
                  tcp_absent=False, blocked_holders=()):
        self.roots += 1
        root = Path(self.temp.name) / f'proc{self.roots}'
        (root / 'net').mkdir(parents=True)
        if tcp_unreadable:
            (root / 'net/tcp').mkdir()  # exists; open() raises for any uid
        elif not tcp_absent:
            (root / 'net/tcp').write_text(PROC_HEADER + ''.join(f'{r}\n' for r in tcp))
        if tcp6 is not None:
            (root / 'net/tcp6').write_text(PROC_HEADER + ''.join(f'{r}\n' for r in tcp6))
        for pid, inode in holders:
            fds = root / str(pid) / 'fd'
            fds.mkdir(parents=True, exist_ok=True)
            os.symlink(f'socket:[{inode}]', fds / str(len(list(fds.iterdir())) + 3))
        for pid in blocked_holders:
            (root / str(pid)).mkdir(parents=True, exist_ok=True)
            # A pid whose fd dir cannot be listed. A plain file reproduces it
            # for root and non-root alike, unlike chmod 000 which root ignores.
            (root / str(pid) / 'fd').write_text('not a directory\n')
        return root

    def run_cli(self, proc_root, ss_lines, expected=OURS):
        """Exercise the real CLI exit-code mapping, not just main()."""
        argv = ['bridge_owner.py', str(self.PORT)] + ([str(expected)] if expected else [])
        out, err = io.StringIO(), io.StringIO()
        with patch.object(self.owner.sys, 'platform', 'linux'), \
                patch.object(self.owner.sys, 'argv', argv), \
                patch.object(self.owner, 'PROC', str(proc_root)), \
                patch.object(self.owner.subprocess, 'run', return_value=subprocess.CompletedProcess(
                    [], 0, ''.join(f'{line}\n' for line in ss_lines), '')), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.owner.cli()
        return code, out.getvalue() + err.getvalue()

    BARE_V4 = 'LISTEN 0      4096   127.0.0.1:8385 0.0.0.0:*'
    BARE_V6 = 'LISTEN 0      4096   [::1]:8385 [::]:*'

    def test_fallback_attributes_the_listener_ss_could_not(self):
        """The ephemeral-image case: ss gives no pid=, /proc says it is ours."""
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(self.OURS, 777)])
        code, output = self.run_cli(root, [self.BARE_V4])
        print(f'ss-bare + /proc attributes to us: exit={code}', flush=True)
        self.assertEqual(code, 0, output)
        self.assertIn('NOTE bridge ownership', output)
        self.assertIn(f'PID(s) [{self.OURS}]', output)
        self.assertNotIn('REFUSE', output)

    def test_fallback_still_refuses_a_genuinely_foreign_listener(self):
        """#544 MUST survive the fallback: /proc names somebody else -> exit 2."""
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(41, 777)])
        code, output = self.run_cli(root, [self.BARE_V4])
        print(f'ss-bare + /proc attributes to a FOREIGN pid: exit={code}', flush=True)
        self.assertEqual(code, 2, output)
        self.assertIn(f'REFUSE bridge port {self.PORT}: occupied by listener '
                      f'PID(s) [41]; expected {self.OURS}', output)
        self.assertNotIn('inspection impossible', output)

    def test_fallback_refuses_a_foreign_listener_even_with_no_expected_pid(self):
        """Preflight: a bare line that /proc can place is still an occupied port."""
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(41, 777)])
        code, output = self.run_cli(root, [self.BARE_V4], expected=None)
        self.assertEqual(code, 2, output)
        self.assertIn('expected an unused port', output)

    def test_mixed_lines_one_attributed_by_ss_one_by_proc(self):
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)],
                              tcp6=[tcp_row(self.PORT, 778, addr='0' * 32)],
                              holders=[(self.OURS, 777), (self.OURS, 778)])
        ours = f'LISTEN 0 4096 [::1]:8385 [::]:* users:(("pgwt",pid={self.OURS},fd=7))'
        code, output = self.run_cli(root, [ours, self.BARE_V4])
        self.assertEqual(code, 0, output)

    def test_one_inode_ours_one_unowned_refuses_instead_of_approving(self):
        """The widening case from the design notes: a second LISTEN inode on the
        same port that nobody owns must refuse, NOT be averaged away."""
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777), tcp_row(self.PORT, 778, sl=1)],
                              holders=[(self.OURS, 777)])
        code, output = self.run_cli(root, [self.BARE_V4])
        print(f'one inode ours + one unowned: exit={code}', flush=True)
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
        self.assertIn('[778]', output)

    # --- bypass suite: every way the fallback could fail to SEE -------------
    def test_no_listen_row_for_the_port_refuses_not_free(self):
        """Q1: an empty inode set must never read as "nothing is listening"."""
        root = self.fake_proc(tcp=[tcp_row(9999, 777)])
        for expected in (self.OURS, None):
            with self.subTest(expected=expected):
                code, output = self.run_cli(root, [self.BARE_V4], expected=expected)
                self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
                self.assertIn('no LISTEN row for that port', output)

    def test_established_row_on_the_port_is_not_a_listener(self):
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777, state='01')],
                              holders=[(self.OURS, 777)])
        code, output = self.run_cli(root, [self.BARE_V4])
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)

    def test_inode_with_no_fd_holder_refuses(self):
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(self.OURS, 999)])
        code, output = self.run_cli(root, [self.BARE_V4])
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
        self.assertIn('no process on this host owns them', output)

    def test_unreadable_fd_dir_is_reported_not_silently_skipped(self):
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], blocked_holders=[4242])
        code, output = self.run_cli(root, [self.BARE_V4])
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
        self.assertIn('1 unreadable', output)

    def test_unreadable_proc_net_tcp_refuses(self):
        root = self.fake_proc(tcp_unreadable=True, holders=[(self.OURS, 777)])
        code, output = self.run_cli(root, [self.BARE_V4])
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
        self.assertIn('cannot be read', output)

    def test_absent_proc_net_tcp_and_tcp6_refuses(self):
        root = self.fake_proc(tcp_absent=True)
        code, output = self.run_cli(root, [self.BARE_V4])
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
        self.assertIn('net/tcp6 to read', output)

    def test_absent_tcp6_alone_is_tolerated_ipv6_may_be_off(self):
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(self.OURS, 777)])
        self.assertFalse((root / 'net/tcp6').exists())
        self.assertEqual(self.run_cli(root, [self.BARE_V4])[0], 0)

    def test_unparseable_rows_refuse(self):
        for label, row in (('short row', '   0: 0100007F:20C1 00000000:0000 0A'),
                           ('non-hex port', '   0: 0100007F:ZZZZ 00000000:0000 0A '
                            '00000000:00000000 00:00000000 00000000 0 0 777 1'),
                           ('non-numeric inode', tcp_row(self.PORT, 'abc'))):
            with self.subTest(label):
                root = self.fake_proc(tcp=[row], holders=[(self.OURS, 777)])
                code, output = self.run_cli(root, [self.BARE_V4])
                self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
                self.assertIn('unparseable', output)

    def test_ss_that_cannot_run_at_all_stays_exit_2_and_never_reaches_proc(self):
        """A dependency exiting 126/127 is a tool failure, not a host-inspection
        verdict, and must not be able to reach the attributing path."""
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(self.OURS, 777)])
        argv = ['bridge_owner.py', str(self.PORT), str(self.OURS)]
        for label, outcome in (
                ('127 not found', dict(side_effect=FileNotFoundError('ss'))),
                ('126 not executable', dict(return_value=subprocess.CompletedProcess(
                    [], 126, '', 'ss: Permission denied'))),
                ('timeout', dict(side_effect=subprocess.TimeoutExpired('ss', 2)))):
            with self.subTest(label), patch.object(self.owner.sys, 'platform', 'linux'), \
                    patch.object(self.owner.sys, 'argv', argv), \
                    patch.object(self.owner, 'PROC', str(root)), \
                    patch.object(self.owner, 'proc_attribution',
                                 side_effect=AssertionError('fallback must not run')), \
                    patch.object(self.owner.subprocess, 'run', **outcome):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    code = self.owner.cli()
                self.assertEqual(code, 2, err.getvalue())
                self.assertIn('cannot verify', err.getvalue())

    def test_empty_ss_output_never_reaches_the_fallback(self):
        root = self.fake_proc(tcp=[tcp_row(self.PORT, 777)], holders=[(41, 777)])
        for expected, code in ((self.OURS, 1), (None, 0)):
            with self.subTest(expected=expected):
                self.assertEqual(self.run_cli(root, [], expected=expected)[0], code)

    def test_inspection_impossible_code_cannot_read_as_success_or_retry(self):
        self.assertEqual(self.owner.INSPECTION_IMPOSSIBLE, 3)
        self.assertNotIn(self.owner.INSPECTION_IMPOSSIBLE, (0, 1, 2))

    # Tree-agnostic: runs bridge_owner.py as the real CLI with a stub `ss`
    # that reproduces the gate-snapshot image's bare line, against a real
    # socket in a real other process. Patches nothing, so `--tree <parent>`
    # exercises it identically and goes RED there (exit 2, "cannot verify").
    @unittest.skipUnless(sys.platform == 'linux', 'needs a real /proc; the box runs it')
    def test_end_to_end_bare_ss_line_over_a_real_foreign_socket(self):
        base = Path(self.temp.name)
        ready = base / 'holder.ready'
        (base / 'holdsock.py').write_text(
            'import socket, sys, time\n'
            's = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1)\n'
            'open(sys.argv[1], "w").write(str(s.getsockname()[1]))\n'
            'time.sleep(60)\n')
        holder = subprocess.Popen([sys.executable, str(base / 'holdsock.py'), str(ready)])
        self.addCleanup(lambda: (holder.kill(), holder.wait(timeout=5)))
        await_file(ready, holder)
        port = int(ready.read_text())
        stub = base / 'stubbin'
        stub.mkdir()
        executable(stub / 'ss', '#!/bin/sh\n'
                   f'echo "LISTEN 0      4096   127.0.0.1:{port} 0.0.0.0:*"\n')
        env = dict(os.environ, PATH=f'{stub}:{os.environ["PATH"]}')

        def cli(expected):
            return subprocess.run([sys.executable, str(TREE / 'tests/bridge_owner.py'),
                                   str(port), str(expected)], env=env,
                                  capture_output=True, text=True, timeout=20)

        owned = cli(holder.pid)
        print(f'end-to-end bare ss line, owner={holder.pid}: exit={owned.returncode} '
              f'{owned.stderr.strip()}', flush=True)
        self.assertEqual(owned.returncode, 0, owned.stderr)
        # #544: the SAME bare line, a PID that does not own it -> still refuse.
        foreign = cli(holder.pid + 100000)
        print(f'end-to-end bare ss line, foreign expectation: '
              f'exit={foreign.returncode} {foreign.stderr.strip()}', flush=True)
        self.assertEqual(foreign.returncode, 2, foreign.stderr)
        self.assertIn(f'occupied by listener PID(s) [{holder.pid}]', foreign.stderr)

    @unittest.skipUnless(sys.platform == 'linux', 'needs a real /proc; the box runs it')
    def test_real_proc_attributes_a_real_listening_socket(self):
        """No fixtures: a real socket, the real kernel tables, real /proc fds."""
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            sock.listen(1)
            port = sock.getsockname()[1]
            argv = ['bridge_owner.py', str(port), str(os.getpid())]
            bare = f'LISTEN 0 4096 127.0.0.1:{port} 0.0.0.0:*'
            with patch.object(self.owner.sys, 'argv', argv), \
                    patch.object(self.owner.subprocess, 'run',
                                 return_value=subprocess.CompletedProcess([], 0, bare + '\n', '')):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    code = self.owner.cli()
            print(f'real /proc attribution on port {port}: exit={code} '
                  f'{err.getvalue().strip()}', flush=True)
            self.assertEqual(code, 0, err.getvalue())
            self.assertIn(f'PID(s) [{os.getpid()}]', err.getvalue())
            # Same socket, a PID that is not ours: still a refusal.
            with patch.object(self.owner.sys, 'argv',
                              ['bridge_owner.py', str(port), str(os.getpid() + 1)]), \
                    patch.object(self.owner.subprocess, 'run',
                                 return_value=subprocess.CompletedProcess([], 0, bare + '\n', '')):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    code = self.owner.cli()
            self.assertEqual(code, 2, err.getvalue())


TM_WARNING = (
    "lsof: WARNING: can't stat() smbfs file system "
    "/Volumes/.timemachine/BACKUPMACHINE._smb._tcp.local./"
    "75D3DCD7-2C05-4FC3-9820-5CADA12775AD/TimeMachine1\n"
    "      Output information may be incomplete.\n"
    '      assuming "dev=360000a8" from mount table\n')


class LsofStderrTests(unittest.TestCase):
    """macOS: a mount warning on stderr must not read as an inspection failure.

    lsof exits 0 and still enumerates TCP sockets, but writes a WARNING about
    a filesystem it could not stat. Treating any stderr as fatal refused every
    ownership check on a Mac with a Time Machine SMB volume mounted, so no
    branch on that machine could produce a `make check` stamp.

    The bar: tolerate that exact shape, refuse anything else BY NAME, and keep
    #544 -- a foreign owner still exits 2 -- on this platform too.
    """
    PORT = 8385
    OURS = 42

    def setUp(self):
        self.owner = load_owner()

    def run_cli(self, stdout, stderr, expected=OURS, returncode=0):
        argv = ['bridge_owner.py', str(self.PORT)] + ([str(expected)] if expected else [])
        err = io.StringIO()
        with patch.object(self.owner.sys, 'platform', 'darwin'), \
                patch.object(self.owner.sys, 'argv', argv), \
                patch.object(self.owner.subprocess, 'run',
                             return_value=subprocess.CompletedProcess(
                                 [], returncode, stdout, stderr)) as command:
                with contextlib.redirect_stderr(err):
                    code = self.owner.cli()
        self.assertEqual(command.call_args.args[0][0], 'lsof')
        return code, err.getvalue()

    def test_the_mount_warning_is_tolerated_and_ownership_still_verified(self):
        code, output = self.run_cli(f'p{self.OURS}\nf7\n', TM_WARNING)
        print(f'lsof mount warning + our listener: exit={code}', flush=True)
        self.assertEqual(code, 0, output)
        self.assertIn('tolerating a known-harmless lsof mount warning', output)
        self.assertNotIn('REFUSE', output)

    def test_foreign_listener_still_refuses_despite_the_warning(self):
        """#544 on macOS: the warning must not become a free pass."""
        code, output = self.run_cli('p41\nf7\n', TM_WARNING)
        print(f'lsof mount warning + FOREIGN listener: exit={code}', flush=True)
        self.assertEqual(code, 2, output)
        self.assertIn(f'REFUSE bridge port {self.PORT}: occupied by listener '
                      f'PID(s) [41]; expected {self.OURS}', output)

    def test_foreign_listener_refuses_in_preflight_despite_the_warning(self):
        code, output = self.run_cli('p41\nf7\n', TM_WARNING, expected=None)
        self.assertEqual(code, 2, output)
        self.assertIn('expected an unused port', output)

    def test_warning_with_no_listener_is_still_an_honest_empty_answer(self):
        for expected, code in ((self.OURS, 1), (None, 0)):
            with self.subTest(expected=expected):
                self.assertEqual(self.run_cli('', TM_WARNING, expected=expected)[0], code)

    def test_two_unstattable_mounts_are_tolerated(self):
        self.assertEqual(self.run_cli(f'p{self.OURS}\nf7\n', TM_WARNING * 2)[0], 0)

    # --- bypass suite: every way tolerance could become a free pass ---------
    def test_an_unrecognised_stderr_line_still_refuses_and_is_named(self):
        for label, stderr in (
                ('real failure alone', 'lsof: status error on /x: Permission denied\n'),
                ('real failure after the warning',
                 TM_WARNING + 'lsof: status error on /x: Permission denied\n'),
                ('continuation lines with nothing to continue',
                 '      Output information may be incomplete.\n'),
                ('a different warning shape',
                 "lsof: WARNING: can't stat() smbfs filesystem /Volumes/x\n"),
                ('unrelated noise', 'lsof: no pwd entry for UID 501\n')):
            with self.subTest(label):
                code, output = self.run_cli(f'p{self.OURS}\nf7\n', stderr)
                self.assertEqual(code, 2, output)
                self.assertIn('cannot verify', output)
                self.assertNotIn('tolerating', output)

    def test_a_tolerated_warning_does_not_excuse_a_bad_exit_code(self):
        for rc in (2, 126, 127):
            with self.subTest(rc=rc):
                code, output = self.run_cli(f'p{self.OURS}\nf7\n', TM_WARNING, returncode=rc)
                self.assertEqual(code, 2, output)
                self.assertIn(f'lsof exited {rc}', output)

    def test_a_tolerated_warning_does_not_excuse_unparseable_output(self):
        code, output = self.run_cli('COMMAND  PID USER\n', TM_WARNING)
        self.assertEqual(code, 2, output)
        self.assertIn('unparseable lsof output', output)

    def test_listeners_without_pids_are_inspection_impossible_not_success(self):
        code, output = self.run_cli('f7\nf8\n', TM_WARNING)
        self.assertEqual(code, self.owner.INSPECTION_IMPOSSIBLE, output)
        self.assertIn('without owning PIDs', output)

    # Real lsof, real socket, real mount state: no mocking at all. Runs the
    # CLI from TREE, so `--tree <parent>` goes RED here on this Mac while the
    # volume is mounted. Skipped on Linux, where the lsof route is unused.
    @unittest.skipUnless(sys.platform == 'darwin', 'lsof route is macOS-only')
    def test_real_lsof_over_a_real_socket_with_the_live_mount_state(self):
        temp = tempfile.TemporaryDirectory(prefix='lsof-real-')
        self.addCleanup(temp.cleanup)
        base = Path(temp.name)
        ready = base / 'holder.ready'
        (base / 'hold.py').write_text(
            'import socket, sys, time\n'
            's = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1)\n'
            'open(sys.argv[1], "w").write(str(s.getsockname()[1]))\n'
            'time.sleep(60)\n')
        holder = subprocess.Popen([sys.executable, str(base / 'hold.py'), str(ready)])
        self.addCleanup(lambda: (holder.kill(), holder.wait(timeout=5)))
        await_file(ready, holder)
        port = int(ready.read_text())
        bare = subprocess.run(['lsof', '-nP', '-iTCP', '-sTCP:LISTEN'],
                              capture_output=True, text=True, timeout=20)
        print(f'live lsof stderr: {bare.stderr.strip().splitlines()[:1]}', flush=True)

        def cli(expected):
            return subprocess.run([sys.executable, str(TREE / 'tests/bridge_owner.py'),
                                   str(port), str(expected)],
                                  capture_output=True, text=True, timeout=20)

        owned = cli(holder.pid)
        print(f'real lsof, owner={holder.pid}: exit={owned.returncode} '
              f'{owned.stderr.strip()}', flush=True)
        self.assertEqual(owned.returncode, 0, owned.stderr)
        foreign = cli(holder.pid + 100000)
        print(f'real lsof, foreign expectation: exit={foreign.returncode} '
              f'{foreign.stderr.strip()}', flush=True)
        self.assertEqual(foreign.returncode, 2, foreign.stderr)
        self.assertIn(f'occupied by listener PID(s) [{holder.pid}]', foreign.stderr)


class CallerExitCodeTests(unittest.TestCase):
    """The shell wrappers must treat exit 3 as a refusal, not readiness."""

    def shell(self, exit_code, script):
        temp = tempfile.TemporaryDirectory(prefix='caller-rc-')
        self.addCleanup(temp.cleanup)
        base = Path(temp.name)
        shutil.copy(TREE / 'tests/live_daemon_lib.sh', base / 'live_daemon_lib.sh')
        (base / 'bridge_owner.py').write_text(
            f'import sys\nprint("stub exit {exit_code}", file=sys.stderr)\n'
            f'sys.exit({exit_code})\n')
        log = base / 'bridge.log'
        log.write_text('stub bridge log\n')
        command = (f'set -uo pipefail\nsource {shlex.quote(str(base / "live_daemon_lib.sh"))}\n'
                   f'log={shlex.quote(str(log))}\n{script}')
        return subprocess.run(['bash', '-c', command], capture_output=True,
                              text=True, timeout=30)

    def test_bridge_port_state_propagates_each_code_unchanged(self):
        for code in (0, 1, 2, 3):
            with self.subTest(code=code):
                run = self.shell(code, 'bridge_port_state 8385 42; echo "rc=$?"')
                self.assertIn(f'rc={code}', run.stdout, run.stdout + run.stderr)

    def test_wait_bridge_ready_refuses_on_inspection_impossible_without_looping(self):
        start = time.monotonic()
        run = self.shell(3, 'wait_bridge_ready http://127.0.0.1:1/ 8385 "$$" "$log"; '
                            'echo "rc=$?"')
        elapsed = time.monotonic() - start
        print(f'wait_bridge_ready with exit 3: rc line={run.stdout.strip()!r} '
              f'elapsed={elapsed:.2f}s', flush=True)
        self.assertIn('rc=1', run.stdout, run.stdout + run.stderr)
        # The immediate-refuse branch, not the loop-exhausted one: a caller
        # that treated 3 as retryable would reach the other message after 60
        # polls. Asserted by message, not only by the clock.
        self.assertIn('ownership unavailable, foreign listener, or bridge exited',
                      run.stderr)
        # 60 polls at 0.5s would be ~30s; refusing is immediate.
        self.assertLess(elapsed, 10, run.stdout + run.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
