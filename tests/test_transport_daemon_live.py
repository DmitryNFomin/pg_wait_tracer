#!/usr/bin/env python3
"""#871 B integration: the DAEMON with --exact-perf completes rounds with the BPF producers.

Usage (Linux root, project built):
  python3 tests/test_transport_daemon_live.py --pid POSTMASTER [--seconds 10] [--runs 1]

What the Mac cannot show, and this test does: the daemon registers its pinned
helper threads with the producers, names the thread that publishes the E and
L boundaries, issues a new request token per round, consumes both boundaries
and every CPU's higher-round acknowledgement, completes the round and
releases records to the qualification sink. The host tests fabricate every
boundary record; here the kernel produces them.

Arms. Each clean arm starts the daemon BEFORE the workload (backends that
exist earlier have no lifetime cookie and are counted as uncovered), runs one
pgbench load, lets the last rounds finish, stops the daemon and checks the
named assertions below against the daemon's final measurements AND against
the qualification file it wrote.
  clean -c16:  pgbench -S -c 16 -j 4 [-R rate]
  clean -C:    pgbench -S -C -c 16 -j 4 [-R rate]            (PG17/18)
               pgbench -C -c 16 -j 4 [-R rate] -f hold.sql   (PG13, see below)
  four live mutants of src/percpu_transport.c, rebuilt here and required to
  go red at one named assertion each. The first three are the defects that
  kept the daemon from ever completing a round on Linux.

TEST PREMISES, stated so nobody reads this as certification:
  * PGWT_TEST_EXACT_ATTACH_PREMISE=1: unit A2 (the attachment proof) does not
    exist, so without this every fork-seeded generation VOIDs at its first
    TRANS (contract row 33). The daemon ASSUMES the proof and says so.
  * Not EL8 4.18: PGWT_TEST_EXACT_UNCERTIFIED_KERNEL=1, mechanism only.
  * The default loads are rate-limited (--rate-c16, --rate-churn). The merge
    coordinator (src/core/merge.c, not this unit) spends time quadratic in
    the records of a round; unthrottled select-only pgbench -c16 on a 4-vCPU
    box outruns it and the capture ends in VOID(allocation_fail). Pass 0 to
    run unthrottled and see it.
  * PG13, -C arm: each connection holds for --churn-hold-ms (default 100 on
    PG13, 0 elsewhere) before its query. Measured on EL8 4.18: on PG13 the
    daemon needs milliseconds of /proc/<pid>/mem reads to attach a backend,
    and a plain `pgbench -S -C` backend lives a few milliseconds, so some
    exit unattached. The default transport loses them too and counts them
    (lifecycle_init_lost_total, 15 and 20 in two 8 s runs at 200 conn/s);
    under this mode each one is VOID(counter_change, lifecycle_init_drop).
    That is not a transport defect and this test does not hide it: run
    --churn-hold-ms 0 on PG13 to see it.
The 100 ms budget is NOT asserted: held age and event-to-release age are
printed. A round completed on a non-empty pass is required; none is REFUSED
(exit 78), never PASS. All scratch lives in a TemporaryDirectory.
"""
import argparse
import collections
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time

from test_init_hint_recovery import ctl, stop, wait_for
from test_transport_producers_live import Refused, checked, preemption_mode, readable
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / 'pg_wait_tracer'
REASONS = ('counter_change', 'counter_unknown', 'perf_lost', 'clock_regression', 'seal_violation',
           'ambiguous_tie', 'sequence_gap', 'unknown_evidence', 'dependency_lost', 'writer_push_fail',
           'allocation_fail', 'duplicate_identity', 'sort_order_breach', 'sequence_reuse',
           'malformed_envelope', 'queue_ownership')
COUNTERS = ('lifecycle_fork_drop', 'lifecycle_init_drop', 'lifecycle_exit_drop', 'wait_read_fail',
            'query_text_drop', 'pg13_query_text_drop', 'exact_query_update_fail', 'exact_cmd_update_fail',
            'exact_phase_update_fail', 'accum_update_fail', 'fork_state_update_fail', 'event_ring_drop',
            'state_map_update_fail', 'seen_query_ids_update_fail', 'writer_push_fail')

# name, edits of src/percpu_transport.c (each anchor exactly once), the ONE
# assertion that must go red. A build failure or another assertion is not a kill.
MUTANTS = (
    ('helpers-not-registered',
     [('if (!tid || bpf_map_update_elem(helpers_fd, &tid, &cpu, BPF_ANY)) return tid ? -errno : -EINVAL;',
       'if (!tid) return -EINVAL;')],
     'rounds completed > 0'),
    ('leader-issues-el',
     [('d->skel->rodata->helper_el_tid = l->helpers[0].tid;', 'd->skel->rodata->helper_el_tid = 0;')],
     'rounds completed > 0'),
    ('constant-el-token',
     [('.el_request = fence ? 0 : t->el_token};', '.el_request = fence ? 0 : 1};')],
     'rounds complete repeatedly'),
    ('completion-without-acknowledgements',
     [('    if (!t->fencing) return 0;\n    for (size_t i = 0; i < t->ncpu; ++i) if (!t->acks[i]) return 0;\n',
       '    if (!t->fencing) return 0;\n')],
     "every completed round had every CPU's acknowledgement"),
)


def qualification(path):
    """Independent read of the daemon's qualification file (PGWTQB02)."""
    data = Path(path).read_bytes()
    assert data[:8] == b'PGWTQB02', data[:8]
    cpus = struct.unpack_from('<I', data, 12)[0]
    offset = 24 + 4 * cpus
    kinds = collections.Counter()
    voids, frontiers, end = [], [], None
    forks = uncertified = 0
    while offset < len(data):
        size, kind, flags, source = struct.unpack_from('<HBBI', data, offset)
        assert size >= 8 and offset + size <= len(data), ('truncated qualification record', offset, size)
        kinds[kind] += 1
        if kind == 1:
            rank = (flags >> 1) & 3
            forks += rank == 0
            uncertified += rank == 2 and not flags & 1
        elif kind == 2:
            frontiers.append(struct.unpack_from('<Q', data, offset + 16)[0])
        elif kind == 3:
            counters = struct.unpack_from('<15Q', data, offset + 32)
            changed = {name: value for name, value in zip(COUNTERS, counters) if value}
            voids.append((REASONS[source] if source < len(REASONS) else str(source), changed))
        elif kind == 4:
            age, lost, status = struct.unpack_from('<QQI', data, offset + 8)
            end = {'max_held_ns': age, 'perf_lost': lost, 'status': status}
        offset += size
    return {'cpus': cpus, 'text': kinds[0], 'data': kinds[1], 'frontiers': len(frontiers), 'voids': voids,
            'end': end, 'forks': forks, 'first_uncertified': uncertified,
            'frontiers_increase': all(a < b for a, b in zip(frontiers, frontiers[1:]))}


def measurements(text):
    """The daemon's final 'exact-perf: stats' and three 'reconcile' lines."""
    stats = None
    cells = {}
    for line in text.splitlines():
        if line.startswith('exact-perf: stats '):
            stats = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
        elif line.startswith('exact-perf: reconcile '):
            name = re.search(r'dest=(\w+)', line).group(1)
            cells[name] = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
    return stats, cells


def assert_clean(row):
    """Named assertions. The live mutants are matched on these exact messages."""
    stats, cells, dump = row['stats'], row['cells'], row['dump']
    assert stats is not None and set(cells) == {'CPU', 'E', 'L'}, 'the daemon printed its final measurements'
    assert stats['rounds_completed'] > 0, 'rounds completed > 0'
    # Counted from the consumed boundary records at each completion.
    assert stats['acks_at_completion'] == stats['rounds_completed'] * stats['cpus'], \
        "every completed round had every CPU's acknowledgement"
    assert stats['acks_before_fence'] == 0, 'every acknowledgement was produced after its fence request'
    assert stats['rounds_completed'] >= 10, 'rounds complete repeatedly'
    if stats['rounds_completed_nonempty'] == 0:
        raise Refused('no round completed on a pass that consumed something: this run cannot '
                      'show that completion does not need an empty pass')
    assert stats['voids'] == 0 and not dump['voids'] and stats['status'] == 0 and \
        dump['end'] and dump['end']['status'] == 0, 'no VOID'
    assert stats['released_data'] > 0, 'records released > 0'
    assert dump['data'] == stats['released_data'] and dump['text'] == stats['released_text'] and \
        dump['frontiers'] == stats['frontiers'], \
        'the qualification file holds exactly the records the daemon says it released'
    assert dump['frontiers'] > 0 and dump['frontiers_increase'], 'frontiers advance'
    assert stats['seal_violation'] == 0, 'seal_violation = 0'
    assert stats['sequence_gap'] == 0 and all(cell['failures'] == 0 for cell in cells.values()), \
        'sequence_gap = 0 with all failure counters zero'
    assert stats['perf_lost'] == 0 and dump['end']['perf_lost'] == 0, 'perf LOST = 0'
    assert all(cell['read'] == 1 and cell['ok'] == 1 and
               cell['attempted'] == cell['delivered'] + cell['failures'] + cell['discards']
               for cell in cells.values()), 'attempted = delivered + failures + discards per destination'


def daemon_cpu_ticks(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return int(fields[11]) + int(fields[12])


def run_arm(binary, pm, db, churn, seconds, rate, directory, uncertified, hold_ms=0):
    trace = directory / f'trace-{binary.parent.name}-{int(churn)}-{time.monotonic_ns()}'
    trace.mkdir()
    log = trace / 'daemon.stderr'
    env = {key: value for key, value in os.environ.items() if not key.startswith('PGWT_TEST_')}
    env['PGWT_TEST_EXACT_ATTACH_PREMISE'] = '1'
    if uncertified:
        env['PGWT_TEST_EXACT_UNCERTIFIED_KERNEL'] = '1'
    command = ['pgbench', '-n', '-U', 'postgres', '-S', '-c', '16', '-j', '4', '-T', str(seconds), db]
    if churn:
        command.insert(1, '-C')
    if churn and hold_ms:
        script = trace / 'hold.sql'
        script.write_text(f'SELECT pg_sleep({hold_ms / 1000});\n'
                          'SELECT abalance FROM pgbench_accounts WHERE aid = 1;\n')
        command[command.index('-S'):command.index('-S') + 1] = ['-f', str(script)]
    if rate:
        command[1:1] = ['-R', str(rate)]
    with log.open('w') as error:
        proc = subprocess.Popen([str(binary), '--mode', 'full', '--exact-perf', '--pid', str(pm),
                                 '-T', str(trace), '-i', '1', '--duration', str(seconds + 60), '-q'],
                                stdout=subprocess.DEVNULL, stderr=error, env=env)
        try:
            wait_for(proc, lambda: (trace / 'pgwt.sock').exists(), 45,
                     'exact-perf daemon control socket not ready')
            ticks, started = daemon_cpu_ticks(proc.pid), time.monotonic()
            print('RUN ' + ' '.join(command), flush=True)
            load = subprocess.run(command, capture_output=True, text=True, timeout=seconds + 60)
            wall = time.monotonic() - started
            cpu = (daemon_cpu_ticks(proc.pid) - ticks) / os.sysconf('SC_CLK_TCK')
            assert load.returncode == 0, (load.stdout, load.stderr)
            assert proc.poll() is None, log.read_text()
            # The load is over: let the rounds that are open or needed for the
            # last records finish before the daemon is told to stop.
            time.sleep(1.5)
            metrics = ctl(trace)
        finally:
            stop(proc)
    text = log.read_text()
    stats, cells = measurements(text)
    tps = re.search(r'^tps = ([0-9.]+)', load.stdout, re.M)
    return {'stats': stats, 'cells': cells, 'dump': qualification(trace / 'merge-qualification.bin'),
            'stderr': text, 'wall': wall, 'cpu': cpu, 'tps': float(tps.group(1)) if tps else 0.0,
            'init_lost': metrics['lifecycle_init_lost_total'],
            'attach_failures': metrics['wp_attach_failures_total'],
            'premise': 'TEST PREMISE (PGWT_TEST_EXACT_ATTACH_PREMISE=1)' in text}


def report(label, row):
    stats, cells, dump = row['stats'] or {}, row['cells'], row['dump']
    wall = row['wall'] or 1.0

    def ms(key):
        return f"{stats.get(key, 0) / 1000:.1f}"
    print(f"ARM {label} tps={row['tps']:.0f} rounds_requested={stats.get('rounds_requested')} "
          f"rounds_completed={stats.get('rounds_completed')} "
          f"rounds_completed_nonempty={stats.get('rounds_completed_nonempty')} "
          f"rounds_per_s={stats.get('rounds_completed', 0) / wall:.1f} "
          f"acks_at_completion={stats.get('acks_at_completion')} cpus={stats.get('cpus')} "
          f"acks_before_fence={stats.get('acks_before_fence')} holds={stats.get('holds')} cuts={stats.get('cuts')} "
          f"released_data={stats.get('released_data')} released_text={stats.get('released_text')} "
          f"frontiers={stats.get('frontiers')} file_data={dump['data']} file_text={dump['text']} "
          f"file_frontiers={dump['frontiers']} voids={stats.get('voids')} file_voids={dump['voids']} "
          f"seal_violation={stats.get('seal_violation')} sequence_gap={stats.get('sequence_gap')} "
          f"perf_lost={stats.get('perf_lost')} uncovered={stats.get('uncovered')} "
          f"skipped={stats.get('skipped')} terminal_discards={stats.get('terminal_discards')} "
          f"init_lost={row['init_lost']} attach_failures={row['attach_failures']}", flush=True)
    for name in ('CPU', 'E', 'L'):
        cell = cells.get(name, {})
        print(f"ARM {label} dest={name} attempted={cell.get('attempted')} delivered={cell.get('delivered')} "
              f"failures={cell.get('failures')} discards={cell.get('discards')} ok={cell.get('ok')}", flush=True)
    # Measurements for the overhead screen. None of these is asserted.
    print(f"MEASURED {label} held_age_ms p50={ms('held_us_p50')} p99={ms('held_us_p99')} max={ms('held_us_max')} "
          f"event_to_release_ms p50={ms('event_us_p50')} p99={ms('event_us_p99')} max={ms('event_us_max')} "
          f"round_ms mean={ms('round_us_mean')} max={ms('round_us_max')} "
          f"records_per_s={stats.get('released_data', 0) / wall:.0f} "
          f"wire_records_per_s={stats.get('wire_records', 0) / wall:.0f} "
          f"daemon_cpu_pct={100 * row['cpu'] / wall:.1f} queue_high={stats.get('queue_high')} "
          f"first_interval_uncertified={dump['first_uncertified']} forks_released={dump['forks']}", flush=True)


def build_mutant(directory, name, edits):
    """Recompile only src/percpu_transport.c with the project's own flags and
    link it with the objects the project build already produced."""
    target = directory / name
    target.mkdir()
    variables = ('CC', 'CFLAGS', 'LDFLAGS', 'USER_OBJS')
    recipe = '.PHONY: pgwt-b-print-config\npgwt-b-print-config:\n\t@:\n'
    recipe += ''.join(f'$(info PGWT_CONFIG_{key}=$({key}))\n' for key in variables)
    output = checked(['make', '-s', '--no-print-directory', '-f', 'Makefile', '-f', '-',
                      'pgwt-b-print-config'], input=recipe, cwd=ROOT)
    config = {}
    for line in output.splitlines():
        if line.startswith('PGWT_CONFIG_'):
            key, value = line[len('PGWT_CONFIG_'):].split('=', 1)
            config[key] = shlex.split(value)
    assert set(config) == set(variables), output
    source = (ROOT / 'src/percpu_transport.c').read_text()
    for before, after in edits:
        assert source.count(before) == 1, (name, 'mutation anchor is not unique', before)
        source = source.replace(before, after)
    mutated = target / 'percpu_transport.c'
    mutated.write_text(source)
    replaced = target / 'percpu_transport.o'
    checked([*config['CC'], *config['CFLAGS'], '-c', str(mutated), '-o', str(replaced)], cwd=ROOT)
    objects = [str(replaced) if Path(item).name == 'percpu_transport.o' else item
               for item in config['USER_OBJS']]
    assert objects != config['USER_OBJS'] and all((ROOT / item).exists() or item == str(replaced)
                                                  for item in objects), 'project objects must be built'
    binary = target / 'pg_wait_tracer'
    checked([*config['CC'], *config['CFLAGS'], *objects, '-o', str(binary), *config['LDFLAGS']], cwd=ROOT)
    print(f'BUILT mutant {name}: {binary}', flush=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--pg-version', type=int)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--runs', type=int, default=1, help='clean runs per load; every one must pass')
    parser.add_argument('--rate-c16', type=int, default=2000, help='pgbench -R for the -c16 arm; 0 = unthrottled')
    parser.add_argument('--rate-churn', type=int, default=200, help='pgbench -R for the -C arm; 0 = unthrottled')
    parser.add_argument('--churn-hold-ms', type=int, default=None,
                        help='-C arm: hold each connection this long before its query (default: 100 on PG13, else 0)')
    parser.add_argument('--only', choices=('c16', 'churn'), help='measurement runs: one load only')
    parser.add_argument('--no-mutants', action='store_true', help='measurement runs only')
    options = parser.parse_args()
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise Refused('Linux root required; no BPF on the Mac')
    if options.seconds < 5:
        raise Refused('each pgbench arm must run at least 5 seconds')
    pm = options.pid or (find_postmaster(pg_version=options.pg_version) if options.pg_version else
                         find_postmaster(pg_version=13) or find_postmaster())
    if not pm:
        raise Refused('running PostgreSQL required')
    release = os.uname().release
    uncertified = not (release.startswith('4.18.') and '.el8' in release)
    print('KERNEL ' + checked(['uname', '-a']), flush=True)
    dynamic = readable(Path('/sys/kernel/debug/sched/preempt'))
    mode = preemption_mode(dynamic, readable(Path('/boot') / ('config-' + release)))
    print(f'PREEMPT active={mode} dynamic={dynamic!r}', flush=True)
    version = checked([str(Path(f'/proc/{pm}/exe').resolve()), '--version'])
    major = int(version.split()[-1].split('.')[0])
    if major not in (13, 17, 18):
        raise Refused(f'PG{major} has no authorised layout here')
    print(f'PG{major} pm={pm} TEST PREMISES: attachment proof ASSUMED (A2 absent); '
          f"{'MECHANISM ONLY on this kernel; ' if uncertified else ''}"
          f'rates c16={options.rate_c16 or "unthrottled"} churn={options.rate_churn or "unthrottled"}',
          flush=True)
    kernel = 'el8-4.18' if not uncertified else release
    hold = options.churn_hold_ms if options.churn_hold_ms is not None else (100 if major == 13 else 0)
    with tempfile.TemporaryDirectory(prefix='pgwt-b-live-') as tmp:
        directory = Path(tmp)
        db = f'pgwt_b_{os.getpid()}'
        checked(['createdb', '-U', 'postgres', db])
        try:
            checked(['pgbench', '-U', 'postgres', '-i', '-s', '1', db])
            for churn, rate in ((False, options.rate_c16), (True, options.rate_churn)):
                if options.only and (options.only == 'churn') != churn:
                    continue
                for run in range(1, options.runs + 1):
                    label = (f"kernel={kernel} pg={major} load={'-C' if churn else '-c16'}"
                             f"{f'(hold {hold} ms)' if churn and hold else ''} "
                             f"rate={rate or 'unthrottled'} run={run}")
                    row = run_arm(TRACER, pm, db, churn, options.seconds, rate, directory, uncertified,
                                  hold if churn else 0)
                    report(label, row)
                    assert row['premise'], 'the daemon announced the attachment test premise'
                    assert_clean(row)
                    print(f'CLEAN {label}: PASS', flush=True)
            if not options.no_mutants:
                for name, edits, assertion in MUTANTS:
                    binary = build_mutant(directory, name, edits)
                    row = run_arm(binary, pm, db, False, 5, options.rate_c16 or 2000, directory, uncertified)
                    report(f'mutant={name}', row)
                    try:
                        assert_clean(row)
                    except AssertionError as error:
                        # Only its own assertion counts. A daemon that failed
                        # to start, or another red assertion, is not a kill.
                        assert str(error) == assertion, (name, 'went red elsewhere', str(error))
                        print(f'mutant {name}: KILLED ({error})', flush=True)
                    else:
                        raise AssertionError(f'live mutant {name} survived')
        finally:
            checked(['dropdb', '-U', 'postgres', '--if-exists', db])
    if mode in ('full', 'unknown'):
        raise Refused(f'preemption mode {mode}: the completion fence is not argued for it; mechanism results above only')
    print('B INTEGRATION LIVE PASS: daemon and producers complete rounds together; helper registration, '
          'issuer thread, request tokens, acknowledgements, release and accounting; certification=false')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Refused as error:
        print(f'LIVE-REFUSED: {error}', file=sys.stderr)
        raise SystemExit(78)
