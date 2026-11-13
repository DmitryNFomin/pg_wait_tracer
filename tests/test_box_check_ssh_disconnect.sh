#!/usr/bin/env bash
# Run the real ephemeral provisioning stage and EXIT cleanup in a scratch tree.
# The launcher's VM-create process substitution cannot open /dev/fd in the Mac
# sandbox, so extract the stage and cleanup verbatim rather than creating a VM.
# A process-group deadline also works with macOS's stock bash (no timeout).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 - "$ROOT" <<'PY'
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile


def executable(path, source):
    path.write_text('#!/usr/bin/env bash\nset -euo pipefail\n' + source)
    path.chmod(0o755)


def bounded(command, env):
    child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True,
                             start_new_session=True)
    try:
        output, _ = child.communicate(timeout=10)
        return child.returncode, output
    except subprocess.TimeoutExpired:
        raise AssertionError('stage exceeded 10-second deadline') from None
    finally:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait()


root = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='pgwt-ssh-disconnect-') as temp:
    scratch = Path(temp)
    repo = scratch / 'repo'
    bin_dir = scratch / 'bin'
    for directory in (repo / 'scripts', repo / 'tests', bin_dir):
        directory.mkdir(parents=True, exist_ok=True)
    launcher = repo / 'scripts/box-check.sh'
    original = (root / 'scripts/box-check.sh').read_text()
    trace = scratch / 'calls'
    env = dict(os.environ, PATH=f'{bin_dir}:{os.environ["PATH"]}',
               CALL_LOG=str(trace), EPHEMERAL='1', KEEP='0', OS='ubuntu',
               HCLOUD_TOKEN='offline-fixture', PGWT_SNAPSHOT_IMAGE='123')
    executable(repo / 'tests/hetzner-sweep.sh', 'exit 0\n')
    executable(repo / 'tests/hetzner-vm.sh', '''
case "$1" in
    delete) [[ "$2" == 4242 ]]; echo 'delete 4242' >> "$CALL_LOG" ;;
    *) exit 99 ;;
esac
''')
    executable(bin_dir / 'curl', '''
[[ "$*" == *'https://api.hetzner.cloud/v1/servers/4242'* ]] || exit 99
echo 'confirm 404' >> "$CALL_LOG"
printf 404
''')
    # The fake refuses a missing interval even for a command it would otherwise
    # accept. A protected provision stage simulates SSH detecting a dropped link.
    executable(bin_dir / 'ssh', '''
interval= count= connect=
while [[ "${1:-}" == -o ]]; do
    case "$2" in
        ServerAliveInterval=*) interval="${2#*=}" ;;
        ServerAliveCountMax=*) count="${2#*=}" ;;
        ConnectTimeout=*) connect="${2#*=}" ;;
    esac
    shift 2
done
if [[ -z "$interval" ]]; then
    echo 'missing ServerAliveInterval' >> "$CALL_LOG"
    exit 97
fi
[[ "$interval" == 30 && "$count" == 6 && "$connect" == 20 ]] || exit 98
case "${*: -1}" in
    'mkdir -p pgwt-provision') exit 0 ;;
    'sudo bash pgwt-provision/tests-provision-runner.sh ubuntu')
        echo 'drop interval=30 count=6 connect=20 rc=255' >> "$CALL_LOG"
        exit 255 ;;
    *) exit 99 ;;
esac
''')
    # Exercise the real rsync -e argument by launching its selected transport.
    executable(bin_dir / 'rsync', '''
while [[ $# -gt 0 ]]; do
    if [[ "$1" == -e ]]; then
        read -r -a transport <<< "$2"
        exec "${transport[@]}" root@192.0.2.1 'mkdir -p pgwt-provision'
    fi
    shift
done
exit 99
''')
    # Reaching anything beyond the failed provisioning stage is a failure.
    for name in ('git', 'security', 'hcloud', 'scp'):
        executable(bin_dir / name,
                   'echo "unexpected command" >> "$CALL_LOG"\nexit 99\n')

    rc, _ = bounded(['ssh', 'fixture', 'true'], env)
    assert rc == 97 and trace.read_text() == 'missing ServerAliveInterval\n'
    print('PASS: fake ssh without ServerAliveInterval exits 97')

    def stage_harness(source):
        # Only setup is supplied here: represent a VM that was just created.
        # The stage's pipeline status handling, exit and cleanup stay verbatim.
        def extract(pattern):
            matches = re.findall(pattern, source, re.M | re.S)
            assert len(matches) == 1, f'stage harness anchor changed: {pattern}'
            return matches[0]
        options = extract(r'^(ssh_keepalive=\([^\n]*\))$')
        token = extract(r'^(hcloud_token\(\) \{\n.*?^\})$')
        cleanup = extract(r'^(ephemeral_cleanup\(\) \{\n.*?^\})$')
        stage = extract(r'(^    echo "box-check: staging repo \+ running .*?)^else\n')
        trap = extract(r'^    (trap ephemeral_cleanup EXIT)$')
        return '\n'.join(('set -uo pipefail', f'cd {shlex.quote(str(repo))}',
                          options, token, cleanup, 'server_id=4242',
                          'server_ip=192.0.2.1', 'server_name=fixture',
                          'cleanup_done=0', 'target=root@192.0.2.1',
                          f'log={shlex.quote(str(scratch / "stage.log"))}',
                          trap, stage)) + '\n'

    def check(source):
        launcher.write_text(stage_harness(source))
        trace.write_text('')
        rc, output = bounded(['bash', str(launcher)], env)
        calls = trace.read_text().splitlines()
        assert rc == 1, f'stage must exit 1, got {rc}: {output}'
        assert calls.count('delete 4242') == 1, f'cleanup not called once: {calls}\n{output}'
        assert calls.count('confirm 404') == 1, f'deletion not confirmed: {calls}'
        assert 'unexpected command' not in calls, calls
        assert 'drop interval=30 count=6 connect=20 rc=255' in calls, \
            f'disconnect stage did not receive keepalive: {calls}'
        assert 'failed (rc=255)' in output, output
        assert 'confirmed via Hetzner API: server 4242' in output, output
        return rc

    rc = check(original)
    print(f'PASS: extracted real stage detects SSH exit 255; stage exit={rc}; '
          'EXIT trap deletes VM once and confirms 404 (deadline 10s)')
    call = ('ssh -o BatchMode=yes "${ssh_keepalive[@]}" '
            '-o StrictHostKeyChecking=no "$target" '
            '"sudo bash pgwt-provision/tests-provision-runner.sh $OS"')
    assert original.count(call) == 1, 'provision stage mutation anchor changed'
    mutant = original.replace(call, call.replace(' "${ssh_keepalive[@]}"', ''), 1)
    try:
        check(mutant)
    except AssertionError as error:
        assert 'disconnect stage did not receive keepalive' in str(error), str(error)
        assert 'missing ServerAliveInterval' in trace.read_text()
        print('PASS: negative control removing stage options rejected: '
              'fake SSH exit=97, stage exit=1, cleanup ran; '
              'keepalive assertion failed')
    else:
        sys.exit('FAIL: behaviour negative control survived')
PY
