#!/usr/bin/env bash
# Static transport guard and deliberately unprotected call sites. Mac-runnable.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 - "$ROOT" "$@" <<'PY'
from pathlib import Path
import re
import shlex
import shutil
import sys
import tempfile


def guard(root):
    texts = {name: (root / 'scripts' / name).read_text() for name in
             ('box-check.sh', 'box-build.sh')}
    definition = re.findall(r'^ssh_keepalive=\(([^\n]*)\)$',
                            texts['box-check.sh'], re.M)
    required = ['-o', 'ServerAliveInterval=30', '-o', 'ServerAliveCountMax=6',
                '-o', 'ConnectTimeout=20']
    assert len(definition) == 1 and shlex.split(definition[0]) == required, \
        'shared keepalive definition missing or changed'
    calls = 0
    for name, text in texts.items():
        # Join shell continuations before tokenizing; comments and quoted
        # remote command strings are not local SSH invocations.
        logical = text.replace('\\\n', '')
        for number, line in enumerate(logical.splitlines(), 1):
            lexer = shlex.shlex(line, posix=True, punctuation_chars='();|&<>')
            lexer.whitespace_split = True
            tokens = list(lexer)
            for token in tokens:
                if token.startswith('rsync_e='):
                    assert token.startswith('rsync_e=ssh ${ssh_keepalive[*]}'), \
                        f'{name}:{number}: rsync_e lacks shared keepalive'
            for index, token in enumerate(tokens):
                if token not in ('ssh', 'rsync'):
                    continue
                args = []
                for arg in tokens[index + 1:]:
                    if arg in (';', '||', '&&', '|', ')'):
                        break
                    args.append(arg)
                calls += 1
                if token == 'ssh':
                    assert '${ssh_keepalive[@]}' in args, \
                        f'{name}:{number}: ssh lacks shared keepalive'
                else:
                    assert '-e' in args, f'{name}:{number}: rsync lacks SSH transport'
                    transport = args[args.index('-e') + 1]
                    assert (transport.startswith('ssh ${ssh_keepalive[*]}') or
                            transport in ('$rsync_e', '${rsync_e}') and
                            'rsync_e=ssh ${ssh_keepalive[*]}' in
                            shlex.split(logical, comments=True)), \
                        f'{name}:{number}: rsync transport lacks shared keepalive'
    assert calls > 0, 'no transport invocations inspected'
    return calls


root = Path(sys.argv[1])
if len(sys.argv) == 4 and sys.argv[2] == '--check-only':
    root = Path(sys.argv[3])
    try:
        print(f'PASS: {guard(root)} SSH/rsync invocations use shared keepalive')
    except (AssertionError, ValueError, OSError, IndexError) as error:
        sys.exit(f'FAIL: {error}')
    sys.exit(0)
assert len(sys.argv) == 2, 'usage: test_box_check_ssh_keepalive.sh [--check-only ROOT]'
print(f'PASS: {guard(root)} SSH/rsync invocations use shared keepalive')
with tempfile.TemporaryDirectory(prefix='pgwt-ssh-static-') as temp:
    mutant = Path(temp)
    (mutant / 'scripts').mkdir()
    for name in ('box-check.sh', 'box-build.sh'):
        shutil.copyfile(root / 'scripts' / name, mutant / 'scripts' / name)
    original = (mutant / 'scripts/box-check.sh').read_text()
    # Remove the shared options from each direct SSH and early rsync call
    # separately, and from each assignment used by the later rsync calls.
    sites = list(re.finditer(r' "\$\{ssh_keepalive\[@\]\}"| \$\{ssh_keepalive\[\*\]\}|"\$rsync_e"',
                             original))
    assert sites, 'no call sites available for negative control'
    for index, site in enumerate(sites, 1):
        replacement = '"ssh"' if site.group() == '"$rsync_e"' else ''
        (mutant / 'scripts/box-check.sh').write_text(
            original[:site.start()] + replacement + original[site.end():])
        try:
            guard(mutant)
        except AssertionError as error:
            assert 'lacks shared keepalive' in str(error), str(error)
        else:
            sys.exit(f'FAIL: call-site negative control {index} survived')
    print(f'PASS: negative controls rejected all {len(sites)} unprotected call-site/transport mutants')
    (mutant / 'scripts/box-check.sh').write_text(original)
    for name in ('box-check.sh', 'box-build.sh'):
        path = mutant / 'scripts' / name
        saved = path.read_text()
        for call in ('ssh fixture true', 'rsync -az -e ssh ./ fixture:/tmp/'):
            path.write_text(saved + '\n' + call + '\n')
            try:
                guard(mutant)
            except AssertionError as error:
                assert 'lacks shared keepalive' in str(error), str(error)
            else:
                sys.exit(f'FAIL: unprotected {call} in {name} survived')
        path.write_text(saved)
    print('PASS: negative controls rejected new unprotected SSH/rsync in both scripts')
PY
