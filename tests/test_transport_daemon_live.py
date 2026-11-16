#!/usr/bin/env python3
"""#871 B integration: the DAEMON with --exact-perf completes rounds with the BPF producers.

Deadline gate (issues #931 and #932): unthrottled pgbench -S -c16 -j4,
rate 0, natural exit by duration + 15 s and SIGTERM to exit within 15 s
while load continues. VOID/unfinished evidence is acceptable. A timeout
fails; killing the process is cleanup only. Run alone with --only deadline.

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
  clean -C:    pgbench -C -c 16 -j 4 [-R rate] -f hold.sql   (see below)
  cutdiscard:  harness-only kernel discard on a non-issuer CPU; defer E/L
    until all acknowledgements and a real cut HOLD, then resume with traffic
    beyond the cut. The uncounted mutant must VOID(sequence_gap).
  three live mutants of src/percpu_transport.c, rebuilt here and required to
  go red at one named assertion each. They are the three defects that kept
  the daemon from ever completing a round on Linux.

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
  * -C arm: each connection holds for --churn-hold-ms (default 100) before
    its query. A plain `pgbench -S -C` backend lives a few milliseconds and
    can exit before the daemon has attached to it. Measured on EL8 4.18: on
    PG13, where attaching costs milliseconds of /proc/<pid>/mem reads, that
    happens in every run (the default transport loses them too and counts
    them: lifecycle_init_lost_total 15 and 20 in two 8 s runs at 200
    connections/s); on PG18 it happened in 1 of 8 runs. Under this mode each
    such backend is VOID(counter_change, lifecycle_init_drop). That is the
    attachment race, not the transport, and this test does not hide it: run
    --churn-hold-ms 0 to see it.
This test attaches nothing per pid itself: the daemon does, and counts a
backend that exits during its attach apart from a failure on a live one
(wp_attach_exit_race_total, wp_attach_failures_total). Both are printed with
each arm. Neither is hidden: a backend the daemon never attached is an INIT
loss, which is a VOID, which fails the arm.
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
import signal
import struct
import subprocess
import sys
import tempfile
import time

from test_init_hint_recovery import ctl, stop, wait_for
from test_transport_producers_live import Refused, checked, preemption_mode, readable
from testutil import find_postmaster
import transport_cutdiscard as cutdiscard

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
)
# Not a live mutant: completion without the acknowledgements. On the box it
# is stopped by the sequence check before or after its first round, so it
# goes red at one of two assertions depending on timing. The host mutant
# skip_cpu_ack kills it at one. The acknowledgement assertions below still
# hold on every clean run.


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


def run_arm(binary, pm, db, churn, seconds, rate, directory, uncertified, hold_ms=0,
            cutdiscard_stimulus=False):
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
            if cutdiscard_stimulus:
                # Activate the harness only once the independent load process
                # is alive. No sleeps/retries select a favourable schedule.
                worker = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    assert worker.poll() is None, 'cutdiscard pgbench is active before stimulus'
                    (binary.parent / 'ready').touch()
                    out, err = worker.communicate(timeout=seconds + 60)
                    load = subprocess.CompletedProcess(command, worker.returncode, out, err)
                finally:
                    if worker.poll() is None:
                        worker.kill()
                    worker.wait(timeout=5)
                    (binary.parent / 'ready').unlink(missing_ok=True)
            else:
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
            'attach_exit_races': metrics['wp_attach_exit_race_total'],
            'premise': 'TEST PREMISE (PGWT_TEST_EXACT_ATTACH_PREMISE=1)' in text}


def run_deadline_arm(pm, db, directory, uncertified, seconds, terminate):
    """Issues #931/#932: exercise the overload path, including shutdown under load."""
    label = 'sigterm' if terminate else 'duration'
    trace = directory / f'deadline-{label}'
    trace.mkdir()
    log = trace / 'daemon.stderr'
    env = {key: value for key, value in os.environ.items() if not key.startswith('PGWT_TEST_')}
    env['PGWT_TEST_EXACT_ATTACH_PREMISE'] = '1'
    if uncertified:
        env['PGWT_TEST_EXACT_UNCERTIFIED_KERNEL'] = '1'
    duration = seconds + 60 if terminate else seconds
    command = [str(TRACER), '--mode', 'full', '--exact-perf', '--pid', str(pm),
               '-T', str(trace), '-i', '1', '--duration', str(duration), '-q']
    # No -R: rate 0 is unthrottled. Keep producers active through the entire
    # exit allowance, and terminate them only in cleanup after the verdict.
    load_command = ['pgbench', '-n', '-U', 'postgres', '-S', '-c', '16', '-j', '4',
                    '-T', str(duration + 60), db]
    load = None
    with log.open('w') as error, (trace / 'pgbench.out').open('w') as load_out:
        launched = time.monotonic()
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=error, env=env)
        try:
            wait_for(proc, lambda: (trace / 'pgwt.sock').exists(), 45,
                     'deadline daemon control socket not ready')
            started = time.monotonic()
            load = subprocess.Popen(load_command, stdout=load_out, stderr=load_out)
            print(f'RUN deadline={label} duration={duration} rate=0 clients=16 ' +
                  ' '.join(load_command), flush=True)
            if terminate:
                # Waiting for the stimulus interval is not an exit timeout.
                # An early exit cannot demonstrate signal handling under load.
                try:
                    proc.wait(timeout=seconds)
                except subprocess.TimeoutExpired:
                    pass
                else:
                    raise AssertionError('SIGTERM arm stayed alive through its load stimulus: ' + log.read_text())
                assert load.poll() is None, 'unthrottled load remains active before SIGTERM'
                signal_at = time.monotonic()
                proc.send_signal(signal.SIGTERM)
                allowance = 15
            else:
                allowance = seconds + 15 - (time.monotonic() - started)
            try:
                proc.wait(timeout=allowance)
            except subprocess.TimeoutExpired:
                raise AssertionError(f'{label}: capture exit deadline exceeded under unthrottled c16 load') from None
            exited = time.monotonic()
            elapsed = exited - (signal_at if terminate else started)
            assert elapsed <= (15 if terminate else seconds + 15), f'{label}: exit within deadline'
            # The daemon's duration starts after launch, using whole monotonic
            # seconds. Its earliest legal deadline is floor(launch) + duration.
            if not terminate:
                assert exited >= int(launched) + duration, 'duration: stayed alive for the requested duration'
            assert load.poll() is None, 'unthrottled load continued until the daemon exited'
            assert proc.returncode == 0, log.read_text()
        finally:
            # Timeout remains FAIL even if cleanup succeeds. Do not use stop(),
            # whose graceful wait would extend the measured exit allowance.
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=5)
            if load is not None:
                if load.poll() is None:
                    load.terminate()
                try:
                    load.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    load.kill()
                    load.wait(timeout=5)
    stats, _ = measurements(log.read_text())
    dump = qualification(trace / 'merge-qualification.bin')
    assert stats is not None and dump['end'] is not None, 'deadline arm wrote its final capture status'
    if (stats.get('finish_deadline_reached', 0) or stats.get('finish_pass_limit_reached', 0) or
            stats.get('unfinished_records', 0)):
        assert stats['status'] != 0 and dump['voids'] and dump['end']['status'] != 0, \
            'unfinished deadline evidence is VOID, never complete'
    print(f'DEADLINE {label}: PASS elapsed={elapsed:.3f}s status={stats["status"]} '
          f'finish_deadline_reached={stats.get("finish_deadline_reached", "old-build")} '
          f'finish_pass_limit_reached={stats.get("finish_pass_limit_reached", "old-build")} '
          f'unfinished_records={stats.get("unfinished_records", "old-build")}', flush=True)


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
          f"init_lost={row['init_lost']} attach_failures={row['attach_failures']} "
          f"attach_exit_races={row['attach_exit_races']}", flush=True)
    for name in ('CPU', 'E', 'L'):
        cell = cells.get(name, {})
        print(f"ARM {label} dest={name} attempted={cell.get('attempted')} delivered={cell.get('delivered')} "
              f"failures={cell.get('failures')} discards={cell.get('discards')} ok={cell.get('ok')}", flush=True)
    # Measurements for the overhead screen. None of these is asserted.
    print(f"MEASURED {label} held_age_ms p50={ms('held_us_p50')} p99={ms('held_us_p99')} max={ms('held_us_max')} "
          f"event_to_release_ms p50={ms('event_us_p50')} p99={ms('event_us_p99')} max={ms('event_us_max')} "
          f"(oldest: rank={stats.get('event_max_rank')} source={stats.get('event_max_source')}) "
          f"round_ms mean={ms('round_us_mean')} max={ms('round_us_max')} "
          f"records_per_s={stats.get('released_data', 0) / wall:.0f} "
          f"wire_records_per_s={stats.get('wire_records', 0) / wall:.0f} "
          f"daemon_cpu_pct={100 * row['cpu'] / wall:.1f} queue_high={stats.get('queue_high')} "
          f"first_interval_uncertified={dump['first_uncertified']} forks_released={dump['forks']}", flush=True)


def run_cutdiscard(pm, db, directory, uncertified, seconds, rate, no_mutants):
    online = set()
    for group in Path('/sys/devices/system/cpu/online').read_text().strip().split(','):
        bounds = [int(item) for item in group.split('-')]
        online.update(range(bounds[0], bounds[-1] + 1))
    cpus = sorted(online)
    if len(cpus) < 2 or not online <= os.sched_getaffinity(0):
        raise Refused('cutdiscard needs at least two online CPUs, all available to daemon helpers')
    with cutdiscard.postgres_away_from_cpu(pm, cpus[1]):
        for uncounted in ((False,) if no_mutants else (False, True)):
            binary = cutdiscard.build(directory, cpus[1], uncounted)
            row = run_arm(binary, pm, db, False, seconds, rate, directory, uncertified,
                          cutdiscard_stimulus=True)
            label = 'cutdiscard-uncounted' if uncounted else 'cutdiscard'
            report(label, row)
            for line in row['stderr'].splitlines():
                if line.startswith('CUTDISCARD '):
                    print(line, flush=True)
            assert row['premise'], 'cutdiscard daemon announced attachment test premise'
            if uncounted:
                cutdiscard.assert_mutant(row)
                print('mutant cutdiscard-uncounted: KILLED (VOID(sequence_gap), no premature release)', flush=True)
            else:
                cutdiscard.assert_counted(row)
                assert_clean(row)
                print('CUTDISCARD LIVE PASS: holds, cuts, terminal_discards > 0; exact reconciliation', flush=True)


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
                        help='-C arm: hold each connection this long before its query (default 100; 0 = plain -S -C)')
    parser.add_argument('--only', choices=('c16', 'churn', 'deadline', 'cutdiscard'), help='run one load/gate only')
    parser.add_argument('--deadline-seconds', type=int, default=18, help='unthrottled deadline stimulus (at least 18 s)')
    parser.add_argument('--no-mutants', action='store_true', help='measurement runs only')
    parser.add_argument('--dry-run', action='store_true', help='portable cutdiscard source/oracle checks; no BPF')
    options = parser.parse_args()
    if options.dry_run:
        cutdiscard.dry_run()
        return 0
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise Refused('Linux root required; no BPF on the Mac')
    if options.seconds < 5:
        raise Refused('each pgbench arm must run at least 5 seconds')
    if options.deadline_seconds < 18:
        raise Refused('deadline stimulus must run at least 18 seconds')
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
    hold = 100 if options.churn_hold_ms is None else options.churn_hold_ms
    with tempfile.TemporaryDirectory(prefix='pgwt-b-live-') as tmp:
        directory = Path(tmp)
        db = f'pgwt_b_{os.getpid()}'
        checked(['createdb', '-U', 'postgres', db])
        try:
            checked(['pgbench', '-U', 'postgres', '-i', '-s', '1', db])
            if not options.only or options.only == 'deadline':
                for terminate in (False, True):
                    run_deadline_arm(pm, db, directory, uncertified, options.deadline_seconds, terminate)
            if not options.only or options.only == 'cutdiscard':
                run_cutdiscard(pm, db, directory, uncertified, options.seconds,
                               options.rate_c16, options.no_mutants)
            for churn, rate in ((False, options.rate_c16), (True, options.rate_churn)):
                if options.only in ('deadline', 'cutdiscard'):
                    continue
                if options.only and (options.only == 'churn') != churn:
                    continue
                for run in range(1, options.runs + 1):
                    label = (f"kernel={kernel} pg={major} load={'-C' if churn else '-c16'} "
                             f"hold_ms={hold if churn else 0} rate={rate or 'unthrottled'} run={run}")
                    row = run_arm(TRACER, pm, db, churn, options.seconds, rate, directory, uncertified,
                                  hold if churn else 0)
                    report(label, row)
                    assert row['premise'], 'the daemon announced the attachment test premise'
                    assert_clean(row)
                    print(f'CLEAN {label}: PASS', flush=True)
            if not options.no_mutants and options.only not in ('deadline', 'cutdiscard'):
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
    if options.only == 'deadline':
        print('TRANSPORT DEADLINE LIVE PASS: natural exit and SIGTERM under unthrottled c16 load; certification=false')
        return 0
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
