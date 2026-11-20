#!/usr/bin/env python3
"""Opt-in #933 measurement: plain pgbench -C at 200 connections/s.

Linux/root only. This is an expected-red diagnostic, not a qualification gate.
Exit 1 means counter_change ended capture before the stop request; 0 means
that symptom was not reproduced. Errors/refusal use exit 2/78. Each run's
logs, metrics and exact stop-request time are kept in --output outside the repo.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

from test_init_hint_recovery import ctl, stop, wait_for
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]


def summary(text):
    lines = text.splitlines()
    return {
        'init_losses': sum(line.startswith('INIT-LOST:') for line in lines),
        'evidence_lines': sum(line.startswith('INIT-LOST-EVIDENCE:') for line in lines),
        'capture_ended_counter_change': any(
            'exact-perf: suspended: counter_change ' in line and
            'automatic REARM disabled' in line for line in lines),
    }


def attach_timeline_records(text):
    """Parse the diagnostic JSON without interpreting it as integrity evidence."""
    result = {'timelines': [], 'summaries': [], 'stats': []}
    prefixes = {'ATTACH-TIMELINE: ': 'timelines',
                'ATTACH-TIMELINE-SUMMARY: ': 'summaries',
                'ATTACH-TIMELINE-STATS: ': 'stats'}
    for line in text.splitlines():
        for prefix, key in prefixes.items():
            if line.startswith(prefix):
                result[key].append(json.loads(line[len(prefix):]))
    return result


def wakeup_runs(runs, paired):
    """Reverse each pair's order to balance drift on the same VM/build."""
    return [(run, arm) for run in range(1, runs + 1)
            for arm in ((('force', 'adaptive') if run % 2 else ('adaptive', 'force'))
                        if paired else ('default',))]


def timeline_report(records):
    """Nearest-rank latency and loss populations; missing stays missing."""
    rows = records['timelines']
    def population(selected):
        latencies = sorted(r['rb_receive_ns'] - r['fork_ns'] for r in selected
                           if r.get('rb_receive_ns') is not None and r.get('fork_ns') is not None
                           and r['rb_receive_ns'] >= r['fork_ns'])
        outcomes = {name: {'backends': 0, 'lost': 0} for name in
                    ('attached', 'never_fired', 'fired+stored', 'fired+not_stored', 'unknown')}
        for row in selected:
            item = outcomes.setdefault(row['outcome'], {'backends': 0, 'lost': 0})
            item['backends'] += 1
            item['lost'] += bool(row['lost'])
        return {'backends': len(selected), 'lost': sum(bool(r['lost']) for r in selected),
                'by_outcome': outcomes,
                'fork_to_receive_ns': {'n': len(latencies), **{
                    f'p{p}': latencies[(len(latencies) * p + 99) // 100 - 1] if latencies else None
                    for p in (50, 90, 99)}}}
    stats = records['stats'][-1] if records['stats'] else {}
    elapsed = stats.get('outer_loop_elapsed_ns', 0) / 1e9
    return {'all': population(rows),
            'batch_records_before=0': population([r for r in rows if r.get('batch_records_before') == 0]),
            'batch_records_before>0': population([r for r in rows if r.get('batch_records_before') is not None
                                                 and r['batch_records_before'] > 0]),
            'batch_records_before_unknown': sum(r.get('batch_records_before') is None for r in rows),
            'diagnostic_stats': stats,
            'wakes_per_second': stats.get('outer_loop_wakes', 0) / elapsed if elapsed else None,
            'lifecycle_wakes_per_second': stats.get('outer_loop_lifecycle_wakes', 0) / elapsed if elapsed else None,
            'observation_seconds': elapsed}


def checked(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, check=True, **kwargs)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--pg-version', type=int, choices=(13, 17, 18))
    parser.add_argument('--binary', type=Path, default=ROOT / 'pg_wait_tracer')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--rate', type=int, default=200)
    parser.add_argument('--attach-timeline', action='store_true',
                        help='enable attachment lines and shutdown distributions')
    parser.add_argument('--paired-wakeup', action='store_true',
                        help='interleave force/adaptive FORK wakes on this build; enables timeline')
    args = parser.parse_args()
    if args.paired_wakeup:
        args.attach_timeline = True
    if sys.platform != 'linux' or os.geteuid() != 0:
        print('LIVE-REFUSED: Linux root required; no BPF on macOS', file=sys.stderr)
        return 78
    if args.runs < 1 or args.seconds < 5 or args.rate < 1:
        parser.error('positive runs/rate and at least 5 seconds required')
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error('--output must be outside the repository')
    output.mkdir(parents=True, exist_ok=False)
    pm = args.pid or find_postmaster(pg_version=args.pg_version)
    if not pm:
        parser.error('running PostgreSQL required (--pid or --pg-version)')
    binary = args.binary.resolve()
    db = f'pgwt_init_loss_{os.getpid()}'
    checked(['createdb', '-U', 'postgres', db])
    rows = []
    try:
        checked(['pgbench', '-U', 'postgres', '-i', '-s', '1', db])
        for run, arm in wakeup_runs(args.runs, args.paired_wakeup):
            directory = output / (f'{arm}-r{run}' if args.paired_wakeup else f'r{run}')
            directory.mkdir()
            log = directory / 'daemon.stderr'
            env = {key: value for key, value in os.environ.items() if not key.startswith('PGWT_TEST_')}
            env['PGWT_TEST_EXACT_ATTACH_PREMISE'] = '1'
            # The historical Ubuntu arm is a mechanism measurement; A2 is
            # absent, and this override never asserts kernel qualification.
            release = os.uname().release
            if not (release.startswith('4.18.') and '.el8' in release):
                env['PGWT_TEST_EXACT_UNCERTIFIED_KERNEL'] = '1'
            command = [str(binary), '--mode', 'full', '--exact-perf', '--pid', str(pm),
                       '-T', str(directory), '-i', '1', '--duration', str(args.seconds + 60), '-q']
            if args.attach_timeline:
                command.append('--attach-timeline')
            if args.paired_wakeup:
                command.append(f'--test-fork-wakeup={arm}')
            load = ['pgbench', '-n', '-U', 'postgres', '-S', '-C', '-c', '16', '-j', '4',
                    '-R', str(args.rate), '-T', str(args.seconds), db]
            print(f'RUN {run} arm={arm}: {shlex.join(command)}; {shlex.join(load)}; certification=false', flush=True)
            (directory / 'commands.json').write_text(json.dumps({'daemon': command, 'load': load,
                'test_premises': {key: value for key, value in env.items() if key.startswith('PGWT_TEST_')}}, indent=2) + '\n')
            with log.open('w') as error:
                proc = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=error)
                try:
                    wait_for(proc, lambda: (directory / 'pgwt.sock').exists(), 45, 'daemon control socket not ready')
                    with (directory / 'pgbench.out').open('w') as load_out:
                        subprocess.run(load, stdout=load_out, stderr=subprocess.STDOUT,
                                       check=True, timeout=args.seconds + 60)
                    assert proc.poll() is None, f'daemon exited early: {proc.returncode}'
                    # Same 1.5s post-load drain as test_transport_daemon_live;
                    # not a retry, acceptance delay, or extra connection hold.
                    time.sleep(1.5)
                    (directory / 'metrics.json').write_text(json.dumps(ctl(directory), indent=2) + '\n')
                finally:
                    stop_request = time.monotonic_ns()
                    before_stop = log.read_text()
                    (directory / 'before-stop.stderr').write_text(before_stop)
                    (directory / 'stop-request-ns.txt').write_text(str(stop_request) + '\n')
                    stop(proc)
            row = summary(before_stop)
            row.update(run=run, arm=arm, stop_request_ns=stop_request, daemon_returncode=proc.returncode)
            assert proc.returncode == 0, f'daemon failed: {proc.returncode}; see {log}'
            if args.attach_timeline:
                timeline = attach_timeline_records(log.read_text())
                row['attachment'] = timeline_report(timeline)
                (directory / 'attach-timeline.jsonl').write_text(''.join(
                    json.dumps(record, sort_keys=True) + '\n' for record in timeline['timelines']))
                (directory / 'attach-timeline-summary.json').write_text(json.dumps(
                    {'summaries': timeline['summaries'], 'stats': timeline['stats']}, indent=2) + '\n')
            # Show all evidence, while clearly separating shutdown observations.
            for line in log.read_text().splitlines():
                if line.startswith(('INIT-LOST:', 'INIT-LOST-EVIDENCE:')) or 'suspended:' in line:
                    print(line, flush=True)
            print('RESULT ' + json.dumps(row, sort_keys=True), flush=True)
            rows.append(row)
    finally:
        checked(['dropdb', '-U', 'postgres', '--if-exists', db])
    (output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
    if args.paired_wakeup:
        by_arm = {}
        for arm in ('force', 'adaptive'):
            records = {'timelines': [], 'stats': []}
            for run, candidate in wakeup_runs(args.runs, True):
                if candidate != arm:
                    continue
                parsed = attach_timeline_records((output / f'{arm}-r{run}' / 'daemon.stderr').read_text())
                records['timelines'].extend(parsed['timelines'])
                records['stats'].extend(parsed['stats'])
            # Sum per-run observation periods; never bridge time between runs.
            combined = {key: sum(stat.get(key, 0) for stat in records['stats'])
                        for key in ('outer_loop_wakes', 'outer_loop_lifecycle_wakes', 'outer_loop_elapsed_ns',
                                    'completed', 'pending', 'table_overflows', 'summary_dropped', 'replaced_pending')}
            records['stats'] = [combined]
            by_arm[arm] = timeline_report(records)
        (output / 'paired-summary.json').write_text(json.dumps(by_arm, indent=2) + '\n')
        print('PAIRED ' + json.dumps(by_arm, sort_keys=True), flush=True)
    red = sum(row['capture_ended_counter_change'] for row in rows)
    print(f'REPRO #933: {red}/{len(rows)} red before stop; evidence={output}; certification=false', flush=True)
    return 1 if red else 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f'REPRO ERROR: {error}', file=sys.stderr)
        raise SystemExit(2)
