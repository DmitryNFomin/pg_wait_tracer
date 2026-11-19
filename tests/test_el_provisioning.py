#!/usr/bin/env python3
"""Host-only execution of EL provisioning blocks with command shims.

All absolute filesystem paths in the selected blocks are rebased into a
TemporaryDirectory. No package, database, browser or BPF tool is executed.
The blocks come from the real provisioner, including its PG loop and heredocs.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / 'tests/provision-runner.sh').read_text()


def between(source, start, end):
    assert source.count(start) == source.count(end) == 1, (start, end)
    return source.split(start, 1)[1].split(end, 1)[0]


def recipe(source):
    # Keep whole case statements so dispatch, rather than a copied EL arm,
    # determines which commands execute.
    dispatch = source.split('case "$OS" in\n', 1)[1].split('\nif [[ $(id -u) -ne 0 ]]; then', 1)[0]
    assert dispatch.rstrip().endswith('esac')
    blocks = ['case "$OS" in\n' + dispatch]
    for number, following in ((1, 2), (3, 4), (4, 5)):
        block = between(source, f'# {number}. ', f'# {following}. ')
        block = block[block.index('\ncase "$OS" in'):] if number != 4 else block[block.index('\nfor V in'):]
        # Section 4 also contains the intervening shared environment setup.
        blocks.append(block.rsplit('# ---------------------------------------------------------------------------', 1)[0])
    block = source.split('# 5. ', 1)[1]
    blocks.append(between(block, '\ncase "$OS" in\n', '\nesac\n'))
    blocks[-1] = 'case "$OS" in\n' + blocks[-1] + '\nesac\n'
    return '\n'.join(blocks)


def run_recipe(source, os_name, installed):
    with tempfile.TemporaryDirectory(prefix='pgwt-el-provision-') as name:
        tmp = Path(name)
        for directory in ('etc/yum.repos.d', 'etc/sudoers.d', 'usr/local/bin', 'bin'):
            (tmp / directory).mkdir(parents=True)
        for version in (13, 16, 17, 18):
            data = tmp / f'var/lib/pgsql/{version}/data'
            data.mkdir(parents=True)
            for filename in ('postgresql.conf', 'pg_hba.conf') + (('PG_VERSION',) if installed else ()):
                (data / filename).touch()
            binaries = tmp / f'usr/pgsql-{version}/bin'
            binaries.mkdir(parents=True)
            for binary in ('pg_isready', 'psql', 'pgbench', 'createdb', 'dropdb', f'postgresql-{version}-setup'):
                path = binaries / binary
                path.write_text('#!/bin/sh\nprintf \'%s\\t%s\\n\' \"$0\" \"$*\" >> \"$CALLS\"\n')
                path.chmod(0o755)
        block = recipe(source)
        # Only filesystem roots are rebased; repository URLs stay verbatim.
        block = re.sub(r'(?<![A-Za-z0-9/:])/(etc|usr|var)(?=/)',
                       lambda m: str(tmp) + m.group(), block)
        # Section 4's shared environment setup writes /etc files as root on
        # the box. The same writes now land in the fixture filesystem.
        (tmp / 'etc/environment').touch()
        (tmp / 'etc/profile.d').mkdir()
        (tmp / 'usr/local/bin/pgbench').touch()
        prefix = r'''
set -euo pipefail
record() { printf '%s\t' "$@" >> "$CALLS"; printf '\n' >> "$CALLS"; }
log() { record log "$@"; }
date() { printf "100\n"; }
mktemp() { printf "%s/secure-path\n" "$FIXTURE"; }
dnf() {
    record dnf "$@"
    if [[ "$1" == list && "$INSTALLED" == 0 ]]; then return 1; fi
}
rpm() { record rpm "$@"; [[ "$INSTALLED" == 1 ]]; }
systemctl() { record systemctl "$@"; [[ "$1" != is-active || "$INSTALLED" == 1 ]]; }
sudo() { record sudo "$@"; }
# GNU sed -i/-E and grep -P are Linux-only here. Record them; fixture
# configs start empty, and the real shell appends the canonical settings.
sed() { record sed "$@"; }
grep() { record grep "$@"; return 1; }
visudo() { record visudo "$@"; }
install() { record install "$@"; }
go() { record go "$@"; }
python3() { record python3 "$@"; }
python3.11() { record python3.11 "$@"; }
command() {
    if [[ "$*" == '-v go' && "$INSTALLED" == 0 ]]; then return 1; fi
    builtin command "$@"
}
'''
        env = dict(os.environ, OS=os_name, INSTALLED=str(int(installed)),
                   CALLS=str(tmp / 'calls'), FIXTURE=str(tmp), PATH=str(tmp / 'usr/local/bin') + ':' + os.environ['PATH'])
        result = subprocess.run(['bash', '-c', prefix + block], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        assert result.returncode == 0, result.stdout
        calls = (tmp / 'calls').read_text().replace(str(tmp), '<root>')
        files = {str(p.relative_to(tmp)): p.read_text() for p in (tmp / 'etc').rglob('*') if p.is_file()}
        files.update({str(p.relative_to(tmp)): p.read_text() for p in (tmp / 'var').rglob('*') if p.is_file()})
        return calls, files


def main():
    for installed in (False, True):
        el8_calls, el8_files = run_recipe(SOURCE, 'el8', installed)
        el9_calls, el9_files = run_recipe(SOURCE, 'el9', installed)
        assert 'config-manager\t--set-enabled\tpowertools\t' in el8_calls
        assert 'config-manager\t--set-enabled\tcrb\t' in el9_calls
        assert 'powertools' not in el9_calls
        for version, port in ((13, 5413), (16, 5416), (17, 5417), (18, 5418)):
            config = el9_files[f'var/lib/pgsql/{version}/data/postgresql.conf']
            assert f'port = {port}\n' in config, config
            assert "shared_preload_libraries = 'pg_stat_statements'\n" in config
            assert ('compute_query_id = on\n' in config) == (version >= 14)
            assert f'/pgsql-{version}/bin/psql\t-p\t{port}\t-d\tpostgres\t-c\tCREATE EXTENSION IF NOT EXISTS pg_stat_statements' in el9_calls
            assert f'/pgsql-{version}/bin/pgbench\t-p\t{port}\t-i\t-s\t10\t' in el9_calls
            if not installed:
                assert f'postgresql-{version}-setup\tinitdb' in el9_calls
                assert f'baseurl=https://yum-archive.postgresql.org/{version}/redhat/rhel-9-x86_64/' in el9_files[f'etc/yum.repos.d/pgdg{version}-archive.repo']
        if not installed:
            assert 'EL-9-x86_64/pgdg-redhat-repo-latest.noarch.rpm' in el9_calls
            assert 'dnf\tinstall\t-y\tgolang\t' in el9_calls
        for tool in ('python3.11\tpython3.11-pip', 'playwright==1.60.0', 'bpftool\tlibbpf-devel'):
            assert tool in el9_calls, tool
        # The two OSes differ only in their repository choices and OS label.
        assert el8_calls == el9_calls.replace('CRB for EL9', 'PowerTools for EL8').replace('crb\t', 'powertools\t').replace('EL9 repo', 'EL8 repo').replace('EL-9-x86_64', 'EL-8-x86_64').replace('(el9 has', '(el8 has')
        assert el8_files == {k: v.replace('rhel-9-x86_64', 'rhel-8-x86_64') for k, v in el9_files.items()}
        print(f'PASS: EL9 provisioning commands, PG clusters, scale 10 and tools (installed={installed})')

    # Optional migration evidence from the exact starting revision. This is
    # deliberately separate from the EL9 expected answers above.
    if len(sys.argv) == 2:
        old = Path(sys.argv[1]).read_text()
        for installed in (False, True):
            assert run_recipe(old, 'el8', installed) == run_recipe(SOURCE, 'el8', installed)
            print(f'PASS: EL8 executed commands and generated files byte-identical to baseline (installed={installed})')

    # Execute the actual cloud-init selection, stopping before SSH-key lookup
    # or a server-create call. Literal snapshot ids never query the API.
    vm = (ROOT / 'tests/hetzner-vm.sh').read_text()
    function = between(vm, '\ncmd_create() {', '\ncmd_delete() {')
    function = function[:function.rfind('\n}')]
    function = function[:function.index('\n    local labels_json=')]
    for os_name in ('el8', 'el9', 'ubuntu'):
        script = ('set -euo pipefail\ndie() { echo "$*" >&2; exit 1; }\n'
                  'cmd_create() {' + function + '\nprintf "%s" "$cloud_init"; }\n'
                  'cmd_create --image-snapshot 123 --snapshot-os "$OS"\n')
        env = dict(os.environ, SCRIPT_DIR=str(ROOT / 'tests'), DEFAULT_TYPE='cx33',
                   DEFAULT_IMAGE='rocky-9', DEFAULT_NAME='fixture', OS=os_name)
        result = subprocess.run(['bash', '-c', script], env=env, text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        selected = Path(result.stdout)
        assert selected.name == f'cloud-init-{os_name}-minimal.yaml', result.stdout
        assert selected.is_file()
        if os_name == 'el9':
            assert 'dnf install -y rsync' in selected.read_text()
            assert 'apt' not in selected.read_text()
        print(f'PASS: {os_name} snapshot selects its OS-specific minimal cloud-init')

    # Run the real snapshot builder in a scratch repo. Every transport and
    # VM call is a local shim; fail at provisioning, before any image action.
    with tempfile.TemporaryDirectory(prefix='pgwt-el-snapshot-') as name:
        tmp = Path(name)
        (tmp / 'tests').mkdir()
        (tmp / 'bin').mkdir()
        builder = tmp / 'tests/build-snapshots.sh'
        builder.write_text((ROOT / 'tests/build-snapshots.sh').read_text())
        vm = tmp / 'tests/hetzner-vm.sh'
        vm.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CALLS"\n[ "$1" != create ] || echo "123 192.0.2.1"\n')
        vm.chmod(0o755)
        for tool, body in (('ssh', 'exit 97'), ('rsync', 'exit 0'), ('curl', 'exit 98'), ('security', 'exit 98')):
            path = tmp / 'bin' / tool
            path.write_text('#!/bin/sh\n' + body + '\n')
            path.chmod(0o755)
        env = dict(os.environ, HCLOUD_TOKEN='fixture', CALLS=str(tmp / 'calls'),
                   PATH=str(tmp / 'bin') + ':' + os.environ['PATH'])
        for os_name, image in (('el8', 'rocky-8'), ('el9', 'rocky-9'), ('ubuntu', 'ubuntu-24.04')):
            (tmp / 'calls').write_text('')
            result = subprocess.run(['/bin/bash', str(builder), os_name], env=env,
                                    text=True, capture_output=True, timeout=10)
            assert result.returncode == 1 and 'provisioning failed (rc=97)' in result.stderr, result
            call = shlex.split((tmp / 'calls').read_text().splitlines()[0])
            assert call[call.index('--image') + 1] == image
            if os_name == 'el9':
                assert call[call.index('--cloud-init') + 1] == 'tests/cloud-init-el9-minimal.yaml'
            else:
                assert '--cloud-init' not in call
            print(f'PASS: {os_name} snapshot builder selects its base image and cloud-init before provisioning')
        (tmp / 'calls').write_text('')
        result = subprocess.run(['/bin/bash', str(builder), 'plan9'], env=env,
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == 2 and 'OS must be ubuntu|el8|el9' in result.stderr
        assert not (tmp / 'calls').read_text()
        print('PASS: unknown snapshot OS refuses before any VM call')


if __name__ == '__main__':
    main()
