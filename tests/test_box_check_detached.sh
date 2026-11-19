#!/usr/bin/env bash
# Hermetic host transport test: execute the launcher and remote shell locally.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 - "$ROOT" "$@" <<'PY'
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

root = Path(sys.argv[1])
original = (root / 'scripts/box-check.sh').read_text()

TRANSPORT = r'''#!/usr/bin/env python3
import fcntl
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
base = Path(os.environ['FIXTURE'])
name = Path(sys.argv[0]).name
args = sys.argv[1:]
def record(value):
    with (base / 'calls').open('a') as out:
        out.write(value + '\n')
def execute(command, data=None):
    return subprocess.run(['bash', '-c', command], cwd=base / 'home', input=data).returncode
if name == 'git':
    if args == ['rev-parse', '--abbrev-ref', 'HEAD']:
        print('fixture')
    elif args == ['rev-parse', 'HEAD']:
        print('a' * 40)
    elif args[:2] == ['ls-tree', '-rz']:
        pass
    else:
        sys.exit('unexpected git: ' + repr(args))
elif name == 'date' and args == ['+%s']:
    print(1800000000 + int((base / 'clock').read_text()))
elif name == 'date':
    os.execv('/bin/date', ['date', *args])
elif name == 'sleep':
    seconds = int(args[0])
    record('sleep ' + str(seconds))
    clock = base / 'clock'
    clock.write_text(str(int(clock.read_text()) + seconds))
    time.sleep(0.04)
elif name == 'setsid':
    os.setsid()
    os.execvp(args[0], args)
elif name == 'flock':
    fcntl.flock(9, fcntl.LOCK_EX | (fcntl.LOCK_NB if '-n' in args else 0))
elif name == 'sudo':
    os.execvp(args[0], args)
elif name == 'bpftool':
    pass
elif name == 'curl':
    assert args[-3:] == ['https://api.hetzner.cloud/v1/servers/4242', '-H',
                         'Authorization: Bearer fixture-token'], 'unexpected cloud call'
    record('confirm 404')
    print('404', end='')
elif name == 'rsync':
    record('rsync ' + args[-2])
    src, dst = args[-2:]
    src = src.replace('root@192.0.2.1:', 'fixture:', 1)
    dst = dst.replace('root@192.0.2.1:', 'fixture:', 1)
    if src.startswith('fixture:'):
        directory = Path(src.removeprefix('fixture:')).parents[2]
        if not (directory / '.box-check.exit').exists():
            (base / 'early-results').touch()
            sys.exit(1)
        target = Path(dst)
        target.mkdir(parents=True, exist_ok=True)
        for path in Path(src.removeprefix('fixture:')).iterdir():
            shutil.copyfile(path, target / path.name)
    elif dst.endswith('/.pgwt-box-source-manifest'):
        shutil.copyfile(src, dst.removeprefix('fixture:'))
    elif ':pgwt-provision/' in dst:
        pass
    elif dst.startswith('fixture:'):
        directory = Path(dst.removeprefix('fixture:'))
        shutil.copytree(base / 'payload', directory, dirs_exist_ok=True)
    else:
        sys.exit('unexpected rsync: ' + repr(args))
elif name == 'ssh':
    command = args[-1]
    for option in ('ServerAliveInterval=30', 'ServerAliveCountMax=6', 'ConnectTimeout='):
        assert any(option in arg for arg in args), 'missing SSH bound: ' + option
    assert '/tmp/pgwt-box-check.lock' not in command, 'real lock referenced'
    if 'tests-provision-runner.sh' in command:
        sys.exit(0)
    if 'mktemp -d' in command:
        template = shlex.split(command)[-1]
        Path(template).parent.mkdir(parents=True, exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix='fixture.', dir=Path(template).parent))
        (base / 'directory').write_text(str(directory))
        print(directory)
        sys.exit(0)
    if "python3 - '" in command:
        directory = Path((base / 'directory').read_text())
        count_file = base / 'poll-count'
        count = int(count_file.read_text()) + 1 if count_file.exists() else 1
        count_file.write_text(str(count))
        record('poll ' + str(count))
        # Fail only after a successful mid-run snapshot. The first failed
        # response deliberately leaks some bytes before returning SSH 255.
        active = directory / 'ready'
        seen = base / 'seen'
        if active.exists() and not seen.exists():
            seen.touch()
        elif seen.exists() and int((base / 'failures').read_text()) < int(os.environ['FAIL_POLLS']):
            failed = int((base / 'failures').read_text()) + 1
            (base / 'failures').write_text(str(failed))
            print('999 running 999\nDISCARDED-PARTIAL-RESPONSE', end='')
            sys.exit(255)
        elif seen.exists():
            (base / 'release').touch()
        # Execute the actual remote poll program, including its status read.
        data = sys.stdin.buffer.read()
        sys.exit(execute(command, data))
    if 'sudo rm -rf' in command:
        directory = Path((base / 'directory').read_text())
        alive = (directory / 'ready').exists() and not (directory / '.box-check.exit').exists()
        record('cleanup alive=' + str(alive))
        if (directory / '.box-check.log').exists():
            shutil.copyfile(directory / '.box-check.log', base / 'remote.log')
        rc = execute(command)
        if alive and not directory.exists():
            (base / 'premature-cleanup').touch()
        sys.exit(rc)
    if 'nohup setsid' in command:
        record('launch')
    sys.exit(execute(command))
else:
    sys.exit('unexpected tool: ' + name)
'''

PRODUCER = r'''#!/usr/bin/env python3
import fcntl
import os
from pathlib import Path
import sys
import time
base = Path(os.environ['FIXTURE'])
with (base / 'lock').open('w') as other:
    try:
        fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        pass
    else:
        raise AssertionError('run_all outside box lock')
print('REMOTE-FIRST: café', flush=True)
# Include a chunk larger than the poll reader's buffer and a partial line.
sys.stdout.write('PAYLOAD:' + 'x' * 70000 + '\nfragment-')
sys.stdout.flush()
Path('ready').touch()
deadline = time.monotonic() + 8
while not (base / 'release').exists():
    if time.monotonic() > deadline:
        sys.exit(98)
    time.sleep(0.005)
print('completed', flush=True)
for number in range(20):
    print('REMOTE-ROW:%02d' % number, flush=True)
artifacts = Path('tests/results/ui_live')
artifacts.mkdir(parents=True)
(artifacts / 'run.id').write_text('1800000999\n')
(artifacts / 'evidence').write_text('this run\n')
print('Executed: 20; failed: 0', flush=True)
print('Live UI smoke: PASS', flush=True)
print('REMOTE-LAST', flush=True)
# Keep a real window between the last bytes and the EXIT trap's publication.
time.sleep(0.12)
sys.exit(int(os.environ.get('RUN_RC', '0')))
'''


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(source, *, give_up=False, run_rc=0, build_rc=0, ephemeral=False):
    with tempfile.TemporaryDirectory(prefix='pgwt-detached-') as temp:
        base = Path(temp)
        local = base / 'local'
        for directory in ('scripts', 'tests'):
            (local / directory).mkdir(parents=True)
        for directory in ('bin', 'home', 'payload/scripts', 'payload/tests'):
            (base / directory).mkdir(parents=True)
        (base / 'clock').write_text('0')
        (base / 'failures').write_text('0')
        (base / 'btf').touch()
        # Only host paths differ; all launch, poll, completion and cleanup
        # code executes unchanged, with the actual flock and publication order.
        source = source.replace('/tmp/pgwt-box-check.lock', str(base / 'lock'))
        source = source.replace('/sys/kernel/btf/vmlinux', str(base / 'btf'))
        # The Mac sandbox denies process substitution's /dev/fd reopen in
        # VM creation. Redirect that fixture-only stderr stream directly;
        # the create/delete stubs and detached lifecycle still run normally.
        source = source.replace('2> >(tee -a "$log" >&2)', '2>>"$log"')
        (local / 'scripts/box-check.sh').write_text(source)
        (local / 'scripts/tree-hash.sh').write_text('#!/bin/sh\nprintf "%040d\\n" 1\n')
        (local / 'tests/hetzner-sweep.sh').write_text('#!/bin/sh\nexit 0\n')
        (local / 'tests/hetzner-vm.sh').write_text(
            '#!/bin/sh\ncase "$1" in\n'
            'create) echo "4242 192.0.2.1";;\n'
            'delete) echo "delete $2" >> "$FIXTURE/calls";;\n'
            '*) exit 99;;\nesac\n')
        (base / 'bin/transport').write_text(TRANSPORT)
        (base / 'bin/transport').chmod(0o755)
        for name in ('ssh', 'rsync', 'git', 'date', 'sleep', 'setsid', 'flock', 'sudo', 'bpftool', 'curl'):
            (base / 'bin' / name).symlink_to('transport')
        builder = '''#!/usr/bin/env python3
import fcntl
import os
from pathlib import Path
with open(Path(os.environ['FIXTURE']) / 'lock', 'w') as other:
    try:
        fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        pass
    else:
        raise AssertionError('build outside box lock')
print('REMOTE-BUILD', flush=True)
raise SystemExit(int(os.environ['BUILD_RC']))
'''
        # box-build is invoked through bash, so let its shim execute Python.
        (base / 'payload/scripts/builder.py').write_text(builder)
        (base / 'payload/scripts/box-build.sh').write_text('exec python3 scripts/builder.py\n')
        tier = (root / 'scripts/box-tier.sh').read_text()
        (base / 'payload/scripts/box-tier.sh').write_text(
            tier.replace('/sys/kernel/btf/vmlinux', str(base / 'btf')))
        (base / 'payload/tests/run_all.sh').write_text(PRODUCER)
        for path in (local / 'scripts/tree-hash.sh', local / 'tests/hetzner-sweep.sh',
                     local / 'tests/hetzner-vm.sh',
                     base / 'payload/tests/run_all.sh'):
            path.chmod(0o755)
        env = dict(os.environ, FIXTURE=temp, TMPDIR=temp,
                   PATH=str(base / 'bin') + os.pathsep + os.environ['PATH'],
                   PGWT_BOX='fixture', PGWT_BOX_DIR=str(base / 'remote'),
                   EPHEMERAL='1' if ephemeral else '0', KEEP='0', OS='ubuntu', PG='',
                   HCLOUD_TOKEN='fixture-token', PGWT_SNAPSHOT_IMAGE='123', PGWT_BOX_POLL_SECONDS='10',
                   PGWT_BOX_OUTAGE_SECONDS='3' if give_up else '30',
                   FAIL_POLLS='100' if give_up else '2', RUN_RC=str(run_rc), BUILD_RC=str(build_rc))
        env.pop('PGWT_BOX_UBUNTU', None)
        proc = None
        directory = None
        try:
            proc = subprocess.Popen(['bash', str(local / 'scripts/box-check.sh')], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                output = proc.communicate(timeout=60)[0].decode()
            except subprocess.TimeoutExpired as error:
                detail = re.sub(r'x{100,}', '<payload>', (error.output or b'').decode(errors='replace'))
                detail += '\nCALLS:\n' + (base / 'calls').read_text()
                remote_dir = Path((base / 'directory').read_text())
                if (remote_dir / '.box-check.log').exists():
                    detail += '\nREMOTE:\n' + re.sub(r'x{100,}', '<payload>', (remote_dir / '.box-check.log').read_text())
                raise AssertionError('launcher timeout: ' + detail) from None
            require((base / 'directory').exists(), 'private directory not created: ' + output)
            directory = Path((base / 'directory').read_text())
            require(not (base / 'premature-cleanup').exists(), 'cleanup before exit file')
            require(not (base / 'early-results').exists(), 'exit code read before publication')
            calls = (base / 'calls').read_text()
            log_files = list((local / 'tests/results').glob('box-check-*.log'))
            require(len(log_files) == 1, 'missing local log')
            log = log_files[0].read_bytes()
            require(b'DISCARDED-PARTIAL-RESPONSE' not in log, 'partial failed poll appended')
            if give_up:
                require(proc.returncode == 76, 'give-up exit mapping: ' + output)
                fatals = [line for line in output.splitlines() if 'FATAL' in line]
                pid = (directory / '.box-check.pid').read_text().strip()
                identity = 'VM id=4242' if ephemeral else 'fixture'
                require(len(fatals) == 1 and identity in fatals[0] and
                        str(directory / '.box-check.log') in fatals[0] and 'pid=' + pid in fatals[0],
                        'give-up diagnostic: ' + output)
                require(directory.exists() and not (directory / '.box-check.exit').exists(),
                        'active remote directory removed')
                os.kill(int(pid), 0)
                require(['sleep 2', 'sleep 1'] == [line for line in calls.splitlines() if line.startswith('sleep ')][-2:], 'outage bound/backoff not applied: ' + calls)
                if ephemeral:
                    require(calls.splitlines().count('delete 4242') == 1 and
                            calls.splitlines().count('confirm 404') == 1 and
                            'confirmed via Hetzner API: server 4242' in output,
                            'ephemeral deletion/confirmation changed')
                    return 'ephemeral give-up=76; VM id/PID/log named; delete and 404 confirmation'
                require('cleanup alive=True' in calls, 'active cleanup path not reached')
                return 'give-up=76; bounded outage; live PID/directory retained'
            expected_rc = build_rc or run_rc
            require(proc.returncode == expected_rc, 'completion exit mapping: ' + output)
            remote = (base / 'remote.log').read_bytes()
            start = log.find(b'box-check: lock acquired ')
            require(remote and start >= 0 and log[start:] == remote,
                    'remote log has gaps or duplicates')
            require(not directory.exists(), 'completed directory not pruned')
            require(f'exit={expected_rc} — summary:' in output, 'summary exit changed')
            if not build_rc:
                require(int((base / 'failures').read_text()) == 2, 'mid-run failures not exercised')
                require(b'REMOTE-LAST\n' in remote, 'completion before exit file')
                require(remote.endswith(b'REMOTE-LAST\n'), 'remote tail missing')
                require(b'Executed: 20; failed: 0\nLive UI smoke: PASS\n' in log, 'summary lines changed')
                require((local / 'tests/results/ui_live/evidence').read_text() == 'this run\n',
                        'results not synced')
                require('sleep 2' in calls.splitlines() and 'sleep 4' in calls.splitlines(), 'retry backoff not exercised')
            return f'exit={expected_rc}; complete byte stream; artifacts; safe cleanup'
        finally:
            (base / 'release').touch()
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.communicate()
            if directory is None and (base / 'directory').exists():
                directory = Path((base / 'directory').read_text())
            if directory and directory.exists():
                deadline = time.monotonic() + 3
                while not (directory / '.box-check.exit').exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not (directory / '.box-check.exit').exists() and (directory / '.box-check.pid').exists():
                    try:
                        os.killpg(int((directory / '.box-check.pid').read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass


def mutation(before, after):
    require(original.count(before) == 1, 'mutation anchor changed: ' + before)
    return original.replace(before, after, 1)


if len(sys.argv) == 4 and sys.argv[2] == '--check-only':
    print('PASS: ' + run_case(Path(sys.argv[3]).read_text()))
    sys.exit(0)
require(len(sys.argv) == 2, 'usage: test_box_check_detached.sh [--check-only SCRIPT]')
print('PASS: recovery: ' + run_case(original))
print('PASS: run_all status: ' + run_case(original, run_rc=7))
print('PASS: build status: ' + run_case(original, build_rc=2))
print('PASS: ' + run_case(original, give_up=True))
print('PASS: ' + run_case(original, give_up=True, ephemeral=True))
mutants = [
    ('no retry', mutation('    delay=$backoff', '    exit 255\n    delay=$backoff'), False,
     'completion exit mapping'),
    ('offset not advanced', mutation('            offset=$size', '            offset=0'), False,
     'remote log has gaps or duplicates'),
    # Reset an acknowledged offset on a transport failure. Round 1's +17
    # injection could overshoot the final log and hit the outage assertion
    # instead of the byte-stream assertion; this exercises the named reset.
    ('offset reset', mutation('    [[ "$outage_start" -ne 0 ]] || outage_start=$poll_start',
                              '    offset=0\n    [[ "$outage_start" -ne 0 ]] || outage_start=$poll_start'), False,
     'remote log has gaps or duplicates'),
    ('cleanup before exit file', mutation("[ '$remote_started' = 0 ]", 'true'), True,
     'cleanup before exit file'),
    ('exit read before publication', mutation("rc = status.read_text().strip() if status.exists() else 'running'",
                                            "rc = status.read_text().strip() if status.exists() else '0'"), False,
     'exit code read before publication'),
]
for name, source, give_up, intended in mutants:
    try:
        run_case(source, give_up=give_up)
    except AssertionError as error:
        require(intended in str(error), f'{name}: wrong failure: {error}')
        print(f'PASS: mutant {name} rejected: {intended}')
    else:
        raise AssertionError('mutant survived: ' + name)
print('OK: detached box-check host tests (5 mutants rejected)')
PY
