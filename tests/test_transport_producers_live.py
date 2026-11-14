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
import socket
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
    # A backend that exits before or during instrumentation is an exit race:
    # counted (attach_exit_races) and reported, never a failure. An attach
    # error on a backend that was still live is one, and the probe names it.
    assert row['attach_failures'] == 0, ('attach failed on a live backend', row['attach_failures'])
    # A discard is an attempt withdrawn by design with nothing lost; it is
    # counted apart from failures (transport_discards) and reported.
    assert all(cell['attempted'] == cell['delivered'] + cell['discarded'] and cell['failed'] == 0
               for cell in row['cells']), row


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


# The layout the daemon itself uses, so this test needs no PostgreSQL server
# headers (the gate image has none, and no other live test compiles against
# them). PG17+ publishes a pointer straight to wait_event_info, so there is no
# offset; PG13 uses the validated x86_64 table in src/discovery.c:
#   pgwt_detect_pgproc_wait_offset()   -> offsetof(PGPROC, wait_event_info)
#   pgwt_detect_pg13_query_offsets()   -> QueryDesc / PlannedStmt offsets
# transport_producers_host.py checks on every `make check` that these numbers
# are the ones in that file, so the two cannot drift apart silently.
PG13_LAYOUT = {'pgproc_wait_event_info': 684, 'querydesc_plannedstmt': 8,
               'plannedstmt_queryid': 8, 'querydesc_sourcetext': 16}


def layout(major, machine):
    """(wait offset, QueryDesc.plannedstmt, PlannedStmt.queryId, QueryDesc.sourceText)."""
    if major >= 17:
        return 0, 0, 0, 0
    if major == 13 and machine == 'x86_64':
        return (PG13_LAYOUT['pgproc_wait_event_info'], PG13_LAYOUT['querydesc_plannedstmt'],
                PG13_LAYOUT['plannedstmt_queryid'], PG13_LAYOUT['querydesc_sourcetext'])
    raise Refused(f'PG{major} on {machine} has no validated layout in src/discovery.c')


def settings(pm):
    exe = str(Path(f'/proc/{pm}/exe').resolve())
    version = checked([exe, '--version'])
    major = int(version.split()[-1].split('.')[0])
    if major not in (13, 17, 18):
        raise Refused(f'PG{major} has no authorised layout here')
    wait, qd, ps, source = layout(major, os.uname().machine)
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


def verifier_matrix(binary, label):
    """Every program of the object, loaded alone, with the mode off and on."""
    result = subprocess.run([str(binary), '--probe', '--verifier-matrix'],
                            capture_output=True, text=True, timeout=300)
    lines = [line for line in result.stdout.splitlines() if line.startswith('VERIFIER')]
    for line in lines:
        print(f'{label} {line}', flush=True)
    verdicts = [line.split() for line in lines if line.startswith('VERIFIER mode=')]
    rejected = [fields for fields in verdicts if fields[3] not in ('ACCEPTED', 'SKIPPED-NO-BTF')]
    modes = {fields[1] for fields in verdicts}
    return result, verdicts, rejected, modes


def receive(sock, size):
    data = b''
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError('PostgreSQL closed a held connection')
        data += chunk
    return data


def held_backend(db, delay=0.3):
    """One backend whose INIT the probe is certain to be watching.

    The postmaster forks a backend when the client connects, and that backend
    reads the startup packet before InitProcess() publishes its wait pointer.
    Withholding the packet for `delay` gives the probe time to consume the
    FORK and arm the bootstrap watchpoint, so on_bootstrap fires for this
    backend by construction instead of by winning a race. Without this, the
    probe saw INIT for 1 of 18 backends in one 16-client run and for none in
    the next. Returns the open socket and the backend PID.
    """
    directory, port = checked(['psql', '-U', 'postgres', '-d', db, '-AtF', '|', '-c',
                               "select split_part(current_setting('unix_socket_directories'), ',', 1), "
                               "current_setting('port')"]).split('|')
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(15)
    sock.connect(f'{directory.strip()}/.s.PGSQL.{port.strip()}')
    time.sleep(delay)
    body = struct.pack('!I', 196608) + b'user\0postgres\0database\0' + db.encode() + b'\0\0'
    sock.sendall(struct.pack('!I', len(body) + 4) + body)
    pid = None
    while True:
        kind = receive(sock, 1)
        payload = receive(sock, struct.unpack('!I', receive(sock, 4))[0] - 4)
        if kind == b'R' and struct.unpack('!I', payload[:4])[0] != 0:
            raise Refused('the held-startup stimulus needs local trust authentication for postgres')
        if kind == b'E':
            raise RuntimeError(f'held connection refused: {payload!r}')
        if kind == b'K':
            pid = struct.unpack('!I', payload[:4])[0]
        if kind == b'Z':
            break
    if not pid:
        raise RuntimeError('held connection reported no backend PID')
    return sock, pid


def hold_backends(db, both_parities):
    """Four held backends; for the INIT mutant, until an odd and an even PID."""
    held = []
    try:
        while len(held) < 4 or (both_parities and len({pid & 1 for _, pid in held}) < 2):
            if len(held) >= 40:
                raise Refused('no odd and even backend PID among 40 held connections')
            held.append(held_backend(db))
    except BaseException:
        release_backends(held)
        raise
    print('HELD-STARTUP backends (INIT watched by construction): ' +
          ' '.join(str(pid) for _, pid in held), flush=True)
    return held


def release_backends(held):
    for sock, _ in held:
        try:
            sock.sendall(b'X\x00\x00\x00\x04')
        except OSError:
            pass
        sock.close()


def run_pg(args, directory, db, seconds, churn, spin=0, binary=BINARY, extra=()):
    log = directory / f'probe-{binary.parent.name}-{churn}-{spin}.stderr'
    with log.open('w') as error:
        proc = subprocess.Popen([str(binary), '--probe', *args, '--cohort-db', db, '--spin-ns', str(spin), *extra],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error, text=True)
        held = []
        try:
            await_ready(proc, log)
            # Not in the falsifier arm: it turns the first fork on each CPU
            # into a forced failure, so that backend has no FORK record.
            held = [] if spin else hold_backends(db, '--init-mutant' in extra)
            command = ['pgbench', '-U', 'postgres', '-S', '-c', '16', '-j', '4', '-T', str(seconds), db]
            if churn:
                command.insert(1, '-C')
            print('RUN ' + ' '.join(command), flush=True)
            workload = checked(command)
            print(workload, flush=True)
            if proc.poll() is not None:
                # The probe ends its loop on its first PROBE-FAIL. Show why:
                # its own lines (not libbpf's per-attach warnings) and the
                # per-destination summary it printed on the way out.
                own = [line for line in log.read_text().splitlines() if not line.startswith('libbpf: ')]
                raise AssertionError('the probe exited during the workload, exit='
                                     f'{proc.returncode}\n' + '\n'.join(own[-80:]) + '\n' +
                                     (proc.stdout.read() or '')[-6000:])
            # Held backends stay connected until the probe has stopped, so
            # each one's stored INIT and title are there to be observed.
            proc.stdin.write('STOP\n')
            proc.stdin.flush()
            row = summary(proc, log)
            row['held'] = len(held)
            # Non-vacuity, by construction rather than by luck: the probe
            # must have observed the stored INIT of every held backend.
            stored = {int(pid) for pid in re.findall(r'^INIT-STORED pid=(\d+)', log.read_text(), re.M)}
            missing = sorted({pid for _, pid in held} - stored)
            assert not missing, ('held backends without an observed stored INIT', missing, sorted(stored))
            print(f"ARM binary={binary.parent.name} churn={int(churn)} spin_ns={spin} ok={row['ok']} "
                  f"forks={row['forks']} attached={row['attached']} "
                  f"exited_before_attach={row['exited_before_attach']} "
                  f"attach_exit_races={row['attach_exit_races']} attach_failures={row['attach_failures']} "
                  f"held={row['held']} "
                  f"init_stored={row['init_stored']} init_records_on_L={row['init_records']} "
                  f"init_reserve_failures={row['init_reserve_failures']} trace={row['trace']} text={row['text']} "
                  f"attempted={row['attempted']} delivered={sum(c['delivered'] for c in row['cells'])} "
                  f"failures={row['failures']} discards={row['discards']} lost={row['lost']} spin_cpus={row['spin_cpus']} "
                  f"spin_min_ns={row['spin_min_ns']} requests={row['requests']}", flush=True)
            return row
        finally:
            release_backends(held)
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
    parser.add_argument('--churn-only', type=int, metavar='N',
                        help='run only the clean pgbench -C arm, N times in a row; every one must pass')
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
        major, args = settings(pm)
        print(f'PG{major} PROBE ' + ' '.join([str(BINARY), '--probe', *args]), flush=True)
        # The production object: every program must pass the verifier on its
        # own, with exact_perf_mode off and on. A whole-object load reports
        # only the first rejection.
        result, verdicts, rejected, modes = verifier_matrix(BINARY, 'PRODUCTION')
        assert result.returncode == 0 and verdicts and not rejected and modes == {'mode=0', 'mode=1'}, (
            'every production program loads with the mode off and on',
            result.returncode, rejected, result.stderr[-6000:])
        db = f'pgwt_a1_{os.getpid()}'
        checked(['createdb', '-U', 'postgres', db])
        try:
            checked(['pgbench', '-U', 'postgres', '-i', '-s', '1', db])
            if options.churn_only:
                # The same arm and the same assertions as the full test,
                # repeated; the first failure ends the run.
                for attempt in range(1, options.churn_only + 1):
                    assert_clean_pg(run_pg(args, directory, db, options.seconds, True))
                    print(f'CLEAN -C ARM {attempt} of {options.churn_only}: PASS', flush=True)
                print(f'A1 CLEAN -C ARM: {options.churn_only} of {options.churn_only} passed in a row; '
                      'the other arms were not run')
                return 0
            for churn in (False, True):
                row = run_pg(args, directory, db, options.seconds, churn)
                assert_clean_pg(row)
            run_reuse(directory)
            mutant = build_variant(directory, 'init-publication-mutant')
            # The mutant reserves on L from NMI context, where 4.18 refuses a
            # contended reservation, and it gives up every odd PID's
            # reservation the same way so that each run meets refusals.
            # --init-mutant accepts exactly those failures (counted at the
            # INIT site, per CPU) and nothing else, so the arm reaches its
            # kill assertion instead of aborting on them.
            row = run_pg(args, directory, db, 5, True, binary=mutant, extra=('--init-mutant',))
            # The second number cannot separate a reservation the kernel
            # refused from one the mutant gave up on purpose (odd PIDs).
            print(f"mutant INIT-publication-restored: INIT records on L={row['init_records']}, "
                  f"INIT reservations refused or given up on L={row['init_reserve_failures']}", flush=True)
            # Other clean-arm assertions must remain green. Neither a load
            # failure nor another assertion counts as killing this mutant.
            assert row['init_stored'] > 0 and row['init_records'] > 0, row
            assert row['init_reserve_failures'] > 0, ('the mutant arm met no refused INIT reservation', row)
            assert row['attached'] > 0 and row['trace'] > 0 and row['failures'] == 0 and row['lost'] == 0, row
            assert all(cell['attempted'] == cell['delivered'] + cell['failed'] + cell['discarded'] and
                       (cell['dest'] == 2 or cell['failed'] == 0) for cell in row['cells']), row
            assert sum(cell['failed'] for cell in row['cells']) == row['init_reserve_failures'], row
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
            result, verdicts, rejected, modes = verifier_matrix(falsifier, 'FALSIFIER')
            if result.returncode or rejected or not verdicts:
                raise Refused('the verifier rejected the falsifier object; its log:\n' + result.stderr[-6000:])
            row = run_pg(args, directory, db, 5, True, spin=20000000, binary=falsifier)
            assert row['spun_at_least_1ms'] and row['spin_min_ns'] >= 1000000 and row['failures'] > 0, row
            # Control: the same spins with no stamped challenge. The helper
            # still runs after each spin, so only the missing evidence
            # differs, and the result must be a refusal, never a pass.
            try:
                run_pg(args, directory, db, 5, True, spin=20000000, binary=falsifier,
                       extra=('--no-challenge',))
            except Refused as refusal:
                assert 'no matched request during spin' in str(refusal), refusal
                print('control falsifier-without-challenge: REFUSED as required '
                      '(no matched request during spin)', flush=True)
            else:
                raise AssertionError('the falsifier passed without challenge evidence')
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
