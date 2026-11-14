#!/usr/bin/env python3
"""A1 standalone probe: load real production BPF and drain CPU/E/L directly.

Usage (Linux root, project + tests already built):
  python3 tests/test_transport_producers_live.py --pid POSTMASTER --seconds 20

Runs 16-client pgbench, a 16-client -C arm, initial-namespace PID reuse with
retained records, and the >=1ms delayed-failure falsifier. The reuse arm writes
/proc/sys/kernel/ns_last_pid on the disposable gate box; it does not change
pid_max or signal another task. Missing permissions/stimulus and rejected spin
bytecode are REFUSED (exit 78), never PASS. No daemon or merge certification.
All logs/build scratch live in TemporaryDirectory, never tests/results/.
"""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import struct
import subprocess
import sys
import tempfile
import time

from testutil import find_postmaster
from transport_producers_host import restore_init_publication

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'tests/test_transport_producers'


class Refused(RuntimeError):
    pass


def preemption_mode(dynamic, config):
    """v6.8 kernel/sched/debug.c:232-250 brackets the active mode in ().

    debugfs is authoritative on a dynamic kernel: a boot argument can have
    been changed at runtime. Boot config is sufficient only for static mode.
    Missing/unreadable dynamic evidence is unknown, never silently 'none'.
    """
    active = re.findall(r'\((none|voluntary|full)\)', dynamic)
    if len(active) == 1:
        return active[0]
    flags = dict(line.split('=', 1) for line in config.splitlines()
                 if line.startswith('CONFIG_') and '=' in line)
    if flags.get('CONFIG_PREEMPT_DYNAMIC') == 'y':
        return 'unknown'
    if flags.get('CONFIG_PREEMPT') == 'y' or flags.get('CONFIG_PREEMPT_RT') == 'y':
        return 'full'
    if flags.get('CONFIG_PREEMPT_VOLUNTARY') == 'y':
        return 'voluntary'
    if flags.get('CONFIG_PREEMPT_NONE') == 'y':
        return 'none'
    return 'unknown'


def readable(path):
    try:
        return path.read_text().strip()
    except OSError as error:
        print(f'UNREADABLE {path}: {error}', flush=True)
        return ''


def build_variant(directory, variant):
    """Linux only. Read existing recipe variables without editing build files.

    Build artifacts, skeleton and loader stay together outside the repository.
    Production clean arms always use the normal project-built binary.
    """
    if sys.platform != 'linux':
        raise Refused('variant BPF builds require Linux')
    directory = directory / variant
    directory.mkdir()
    variables = ('CLANG', 'BPF_CFLAGS', 'BPFTOOL', 'CC', 'CFLAGS', 'LIBBPF_LIB')
    recipe = '.PHONY: pgwt-a1-print-config\npgwt-a1-print-config:\n\t@:\n'
    recipe += ''.join(f'$(info PGWT_CONFIG_{key}=$({key}))\n' for key in variables)
    output = checked(['make', '-s', '--no-print-directory', '-f', 'Makefile', '-f', '-',
                      'pgwt-a1-print-config'], input=recipe, cwd=ROOT)
    config = {}
    for line in output.splitlines():
        if line.startswith('PGWT_CONFIG_'):
            key, value = line[len('PGWT_CONFIG_'):].split('=', 1)
            config[key] = shlex.split(value)
    assert set(config) == set(variables), output
    if variant == 'falsifier':
        source = ROOT / 'tests/transport_falsifier.bpf.c'
        defines = ['-DPGWT_TRANSPORT_FALSIFIER=1']
    elif variant == 'init-publication-mutant':
        source = directory / 'mutant.bpf.c'
        source.write_text(restore_init_publication((ROOT / 'src/bpf/pg_wait_tracer.bpf.c').read_text()))
        defines = []
    else:
        raise ValueError(variant)
    obj = directory / 'pg_wait_tracer.bpf.o'
    binary = directory / 'probe'
    checked([*config['CLANG'], *config['BPF_CFLAGS'], '-I', str(ROOT / 'src/bpf'),
             '-c', str(source), '-o', str(obj)], cwd=ROOT)
    # The basename pins the skeleton API to pg_wait_tracer_bpf, just as in
    # the project recipe. The temporary include directory must come first.
    skeleton = checked([*config['BPFTOOL'], 'gen', 'skeleton', str(obj)], cwd=ROOT)
    (directory / 'pg_wait_tracer.skel.h').write_text(skeleton + '\n')
    checked([*config['CC'], '-I', str(directory), *config['CFLAGS'], *defines,
             '-std=c11', '-Werror', '-o', str(binary), str(ROOT / 'tests/test_transport_producers.c'),
             *config['LIBBPF_LIB'], '-lelf', '-lz', '-pthread'], cwd=ROOT)
    print(f'BUILT {variant}: {binary}', flush=True)
    return binary


def assert_clean_pg(row):
    # Check this exact assertion first; the built restoration mutant must
    # reach it with an otherwise healthy clean transport/stored-INIT cohort.
    assert row['init_records'] == 0, 'zero INIT records on L under exact_perf_mode'
    assert row['init_stored'] > 0, 'at least one cohort pid has INIT stored'
    assert row['attached'] > 0 and row['trace'] > 0 and row['failures'] == 0 and row['lost'] == 0, row
    assert all(cell['attempted'] == cell['delivered'] and cell['failed'] == 0 for cell in row['cells']), row


def checked(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, timeout=90, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{command!r}: exit {result.returncode}\n{result.stdout}{result.stderr}')
    return result.stdout.strip()


def symbols(exe):
    found = {}
    for command in (['nm', '-an', '--defined-only', exe], ['nm', '-Dn', '--defined-only', exe]):
        result = subprocess.run(command, capture_output=True, text=True)
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) == 3:
                try:
                    found[fields[2]] = int(fields[0], 16)
                except ValueError:
                    pass
    return found


def elf(exe):
    with open(exe, 'rb') as stream:
        header = stream.read(64)
        if header[:6] != b'\x7fELF\x02\x01':
            raise Refused('probe requires a little-endian ELF64 postgres')
        kind = struct.unpack_from('<H', header, 16)[0]
        offset = struct.unpack_from('<Q', header, 32)[0]
        stride, count = struct.unpack_from('<HH', header, 54)
        segments = []
        for index in range(count):
            stream.seek(offset + index * stride)
            segment = struct.unpack('<IIQQQQQQ', stream.read(56))
            if segment[0] == 1:
                segments.append((segment[3], segment[2], segment[5]))
    return kind, segments


def settings(pm, scratch):
    exe = str(Path(f'/proc/{pm}/exe').resolve())
    version = checked([exe, '--version'])
    major = int(version.split()[-1].split('.')[0])
    if major not in (13, 17, 18):
        raise Refused(f'PG{major} has no authorised layout here')
    pg_config = str(Path(exe).with_name('pg_config'))
    include = checked([pg_config, '--includedir-server'])
    code = '#include <stdio.h>\n#include <stddef.h>\n#include "postgres.h"\n#include "storage/proc.h"\n#include "executor/execdesc.h"\n#include "nodes/plannodes.h"\n#undef printf\nint main(void) { printf("%zu %zu %zu %zu\\n", offsetof(PGPROC, wait_event_info), offsetof(QueryDesc, plannedstmt), offsetof(PlannedStmt, queryId), offsetof(QueryDesc, sourceText)); return 0; }\n'
    unit, layout = scratch / 'layout.c', scratch / 'layout'
    unit.write_text(code)
    checked([os.environ.get('CC', 'cc'), '-std=gnu11', '-I', include, str(unit), '-o', str(layout)])
    wait, qd, ps, source = map(int, checked([str(layout)]).split())
    names = symbols(exe)
    pointer = 'MyProc' if major == 13 else 'my_wait_event_info'
    required = [pointer, 'pgstat_report_activity']
    required += ['standard_ExecutorStart'] if major == 13 else ['pgstat_report_query_id', 'debug_query_string']
    missing = [name for name in required if name not in names]
    if missing:
        raise Refused(f'ELF symbols unavailable: {missing}')
    kind, segments = elf(exe)
    bias = 0
    if kind == 3:
        for line in Path(f'/proc/{pm}/maps').read_text().splitlines():
            fields = line.split()
            if len(fields) >= 6 and fields[2] == '00000000' and fields[-1] == exe:
                bias = int(fields[0].split('-')[0], 16)
                break
        if not bias:
            raise Refused('PIE runtime bias not resolved')

    def file_offset(name):
        value = names[name]
        for virtual, offset, length in segments:
            if virtual <= value < virtual + length:
                return value - virtual + offset
        raise Refused(f'{name} is outside ELF file-backed LOAD segments')

    args = ['--pm', str(pm), '--ptr', str(names[pointer] + bias), '--offset', str(wait if major == 13 else 0),
            '--exe', exe, '--activity', str(file_offset('pgstat_report_activity')),
            '--query', str(file_offset('standard_ExecutorStart' if major == 13 else 'pgstat_report_query_id')),
            '--pg13', str(int(major == 13)), '--running', str(2 if major <= 17 else 3),
            '--qd', str(qd), '--ps', str(ps), '--source', str(source)]
    if major >= 17:
        args += ['--debug-text', str(names['debug_query_string'] + bias)]
    return major, args


def await_ready(proc, log):
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    try:
        if not selector.select(60):
            raise RuntimeError('probe did not become ready in 60s')
        # Read exactly the ready line; TextIO.readline may prefetch the final
        # JSON of the short self-reuse arm, hiding it from communicate().
        ready = bytearray()
        deadline = time.monotonic() + 60
        while not ready.endswith(b'\n'):
            if time.monotonic() >= deadline or not selector.select(max(0, deadline-time.monotonic())):
                raise RuntimeError('partial probe readiness line timed out')
            chunk = os.read(proc.stdout.fileno(), 1)
            if not chunk:
                break
            ready += chunk
        line = ready.decode().strip()
        if not line.startswith('PROBE-READY '):
            rc = proc.wait(timeout=10)
            detail = log.read_text()
            if rc == 78:
                raise Refused(detail)
            raise RuntimeError(f'probe startup exit={rc}, stdout={line!r}\n{detail}')
        print(line, flush=True)
    finally:
        selector.close()


def summary(proc, log):
    out, _ = proc.communicate(timeout=30)
    if proc.returncode == 78:
        raise Refused(log.read_text())
    rows = [json.loads(line) for line in out.splitlines() if line.startswith('{')]
    assert proc.returncode == 0 and len(rows) == 1 and rows[0]['ok'], (proc.returncode, out, log.read_text())
    for line in log.read_text().splitlines():
        if line.startswith('SPIN-EVIDENCE '):
            print(line, flush=True)
    print(json.dumps(rows[0], sort_keys=True), flush=True)
    return rows[0]


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate(timeout=5)


def run_pg(args, directory, db, seconds, churn, spin=0, binary=BINARY):
    log = directory / f'probe-{binary.parent.name}-{churn}-{spin}.stderr'
    with log.open('w') as error:
        proc = subprocess.Popen([str(binary), '--probe', *args, '--cohort-db', db, '--spin-ns', str(spin)],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error, text=True)
        try:
            await_ready(proc, log)
            command = ['pgbench', '-U', 'postgres', '-S', '-c', '16', '-j', '4', '-T', str(seconds), db]
            if churn:
                command.insert(1, '-C')
            print('RUN ' + ' '.join(command), flush=True)
            workload = checked(command)
            print(workload, flush=True)
            assert proc.poll() is None, log.read_text()
            proc.stdin.write('STOP\n')
            proc.stdin.flush()
            return summary(proc, log)
        finally:
            stop(proc)


def run_reuse(directory):
    log = directory / 'probe-reuse.stderr'
    with log.open('w') as error:
        proc = subprocess.Popen([str(BINARY), '--probe', '--self-reuse'], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=error, text=True)
        try:
            await_ready(proc, log)
            row = summary(proc, log)
            assert row['pid_reuses'] >= 1, row
            assert row['forks'] == 2, row
            return row
        finally:
            stop(proc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--seconds', type=int, default=20)
    parser.add_argument('--pg-version', type=int)
    options = parser.parse_args()
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise Refused('Linux root required; no BPF on the Mac')
    if options.seconds < 5:
        raise Refused('each pgbench arm must run at least 5 seconds')
    for line in Path('/proc/self/status').read_text().splitlines():
        if line.startswith('NSpid:') and len(line.split()) != 2:
            raise Refused('PID reuse must exercise initial-namespace wire PIDs')
    pm = options.pid or (find_postmaster(pg_version=options.pg_version) if options.pg_version else
                         find_postmaster(pg_version=13) or find_postmaster())
    if not pm:
        raise Refused('running PostgreSQL required')
    print('KERNEL ' + checked(['uname', '-a']), flush=True)
    print('CMDLINE ' + Path('/proc/cmdline').read_text().strip(), flush=True)
    dynamic = readable(Path('/sys/kernel/debug/sched/preempt'))
    config = readable(Path('/boot') / ('config-' + os.uname().release))
    mode = preemption_mode(dynamic, config)
    print(f'PREEMPT active={mode} dynamic={dynamic!r}', flush=True)
    print('KERNEL_CONFIG ' + ' '.join(line for line in config.splitlines()
          if line.startswith(('CONFIG_PREEMPT', 'CONFIG_HZ='))), flush=True)
    with tempfile.TemporaryDirectory(prefix='pgwt-a1-probe-') as tmp:
        directory = Path(tmp)
        major, args = settings(pm, directory)
        print(f'PG{major} PROBE ' + ' '.join([str(BINARY), '--probe', *args]), flush=True)
        db = f'pgwt_a1_{os.getpid()}'
        checked(['createdb', '-U', 'postgres', db])
        try:
            checked(['pgbench', '-U', 'postgres', '-i', '-s', '1', db])
            for churn in (False, True):
                row = run_pg(args, directory, db, options.seconds, churn)
                assert_clean_pg(row)
            run_reuse(directory)
            mutant = build_variant(directory, 'init-publication-mutant')
            row = run_pg(args, directory, db, 5, True, binary=mutant)
            # Other clean-arm assertions must remain green. Neither a load
            # failure nor another assertion counts as killing this mutant.
            assert row['init_stored'] > 0 and row['init_records'] > 0, row
            assert row['attached'] > 0 and row['trace'] > 0 and row['failures'] == 0 and row['lost'] == 0, row
            assert all(cell['attempted'] == cell['delivered'] and cell['failed'] == 0 for cell in row['cells']), row
            try:
                assert_clean_pg(row)
            except AssertionError as error:
                assert str(error) == 'zero INIT records on L under exact_perf_mode', error
                print(f'mutant INIT-publication-restored: KILLED ({error})', flush=True)
            else:
                raise AssertionError('INIT publication mutant survived')
            # Production clean arms above load the normal object. Only this
            # standalone test build contains the back-edge, even at knob zero.
            falsifier = build_variant(directory, 'falsifier')
            row = run_pg(args, directory, db, 5, True, spin=20000000, binary=falsifier)
            assert row['spun_at_least_1ms'] and row['spin_min_ns'] >= 1000000 and row['failures'] > 0, row
        finally:
            checked(['dropdb', '-U', 'postgres', '--if-exists', db])
    if mode in ('full', 'unknown'):
        raise Refused(f'preemption mode {mode}: completion fence is not qualified; mechanism results above only')
    print('A1 PROBE PASS: loads, lifetime cookies including retained PID reuse, all attempts delivered, E/L ordering, same-CPU helper, delayed failure; certification=false')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Refused as error:
        print(f'PROBE-REFUSED: {error}', file=sys.stderr)
        raise SystemExit(78)
