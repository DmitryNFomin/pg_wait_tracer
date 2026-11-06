#!/usr/bin/env python3
"""Refs #804: pacing quantity, background positive controls, and inert refusal.

Linux/root/live PG13..18, matching server development headers, cc, built daemon
and pgwt-server. No version pin. Both full and DEFAULT tiered capture are tested.
The timed-checkpoint workload follows scratchpad/i804/exp.sh: 30s timeout, .9
completion target, 3m dirty rows, one generate_series client, an 8-second window.

The supplied measurements were 8.7s controls vs 17.4--19.2s broken. We require
<=12s total and <=2s checkpointer DB Time (2s is the explicit allowance chosen
from the control's ~.7s beyond its 8s client; it is NOT a new measured result).
A >=3s pacing row and >=5s client prevent empty/quiet captures passing.

Positive controls use a tiny dynamic background worker instead of depending on
hardware-specific checkpoint IO speed: CPU mode really burns CPU; lock mode
really blocks on a PostgreSQL advisory lock. The harness verifies those sources
via /proc CPU and pg_stat_activity before requiring their measured DB Time.
Excluding all background we==0 makes the CPU floor fail; excluding all background
waits makes the >=5s Lock:advisory floor fail. These are real resource requests,
not fabricated wei writes. Live mutation/red-before evidence requires Linux.
"""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

from testutil import find_postmaster

ROOT = Path(__file__).resolve().parent.parent
TRACER = Path(os.environ.get('PGWT_TEST_TRACER', str(ROOT / 'pg_wait_tracer')))
SERVER = ROOT / 'pgwt-server'
TABLE = 'pgwt804_dirty'


def require(ok, message):
    if not ok:
        raise AssertionError(message)
    print('PASS:', message, flush=True)


def sql(statement, timeout=120):
    return subprocess.run(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres',
                           '-d', 'postgres', '-Atc', statement], check=True,
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


def start_sql(statement, label):
    env = dict(os.environ, PGAPPNAME=label)
    return subprocess.Popen(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres',
                             '-d', 'postgres', '-Atc', statement], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)


def stop_sql(proc, label):
    # Close only this test's connection, not unrelated suite clients. Stop the
    # server-side query too; killing psql alone can leave its backend computing.
    try:
        sql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE "
            f"application_name='{label}' AND pid<>pg_backend_pid()")
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()


def wait_for(predicate, seconds, message):
    # Bounded readiness polling, never a retry of a failed assertion/capture.
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.1)
    raise AssertionError('readiness deadline: ' + message)


def proc_stat(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return fields[0], int(fields[11]) + int(fields[12])  # state, utime+stime


def capture(pm, directory, mode, seconds=8, never_match=False):
    env = dict(os.environ)
    if never_match:
        env['PGWT_TEST_PACING_FILTER_NEVER_MATCH'] = '1'
    args = [str(TRACER), '--pid', str(pm), '--interval', '8', '--duration',
            str(seconds), '--quiet', '-T', str(directory)]
    if mode == 'full':
        args += ['--mode', 'full']
    else:
        # Keep the DEFAULT mode but suppress anomaly promotion: this leg must
        # exercise the sampled tier for the whole quantity/control window.
        args += ['--anomaly-aas-factor', '1000000', '--anomaly-lock-fraction', '1']
    # Omit --mode for the default tiered leg, as required by this regression.
    result = subprocess.run(args, env=env, capture_output=True, text=True,
                            timeout=seconds + 40)
    (directory.parent / (directory.name + '.log')).write_text(result.stderr)
    return result


def report(directory, command, **extra):
    proc = subprocess.Popen([str(SERVER), str(directory)], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        def request(req):
            proc.stdin.write(json.dumps(req) + '\n')
            proc.stdin.flush()
            if not select.select([proc.stdout], [], [], 15)[0]:
                raise AssertionError('pgwt-server response deadline')
            response = json.loads(proc.stdout.readline())
            require('error' not in response, f'{command} response is readable')
            return response
        info = request({'id': 1, 'cmd': 'info'})
        require(int(info['to_ns']) > int(info['from_ns']), 'nonempty capture window')
        return request({'id': 2, 'cmd': command,
                        'from': max(0, int(info['from_ns']) - 1_000_000_000),
                        'to': int(info['to_ns']) + 1_000_000_000,
                        'limit': 1024, **extra})
    finally:
        proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stdout.close()
        proc.stderr.close()


def dirty():
    sql(f'UPDATE {TABLE} SET m=md5(m)')


def wait_pacing(ckpt, native):
    def asleep():
        event = sql(f"SELECT coalesce(wait_event,'NULL') FROM pg_stat_activity WHERE pid={ckpt}")
        state, _ = proc_stat(ckpt)
        return state == 'S' and event == ('CheckpointWriteDelay' if native else 'NULL')
    wait_for(asleep, 45, 'timed checkpoint write spreading')


def quantity(pm, base, mode, ckpt, native):
    # Refuse (never absorb) load this test did not start: a stray active
    # client, e.g. one leaked by an earlier test, doubles total DB Time and
    # would read as a probe miss.
    strays = sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='client backend' "
                 "AND state='active' AND pid<>pg_backend_pid()")
    require(strays == '0', f'{mode}: no stray active client backend before the capture '
            f'(found {strays})')
    sql('CHECKPOINT')
    dirty()
    wait_pacing(ckpt, native)
    label = f'pgwt804_cpu_{mode}'
    client = start_sql('SELECT count(*) FROM generate_series(1,500000000)', label)
    try:
        wait_for(lambda: sql(f"SELECT count(*) FROM pg_stat_activity WHERE application_name='{label}' "
                             "AND state='active'") == '1', 5, 'CPU client running')
        client_pid = int(sql(f"SELECT pid FROM pg_stat_activity WHERE application_name='{label}'"))
        directory = base / ('quantity_' + mode)
        result = capture(pm, directory, mode)
        require(result.returncode == 0, f'{mode} capture succeeds: {result.stderr[-1200:]}')
        tm = report(directory, 'time_model')
        total = float(tm['db_time_ms'])
        require(5000 <= total <= 12000, f'{mode} total DB Time {total:.1f}ms in [5000,12000]')
        sessions = report(directory, 'top_sessions')['rows']
        ckpt_ms = sum(float(r['db_time_ms']) for r in sessions if int(r['pid']) == ckpt)
        cpu_ms = sum(float(r['db_time_ms']) for r in sessions if int(r['pid']) == client_pid)
        require(cpu_ms >= 5000, f'{mode} real CPU client counted: {cpu_ms:.1f}ms')
        require(ckpt_ms <= 2000, f'{mode} checkpointer DB Time {ckpt_ms:.1f}ms <=2000')
        events = report(directory, 'top_events', pid=ckpt)['rows']
        pacing = sum(float(r['total_ms']) for r in events
                     if r['name'] == 'Timeout:CheckpointWriteDelay')
        require(pacing >= 3000, f'{mode} pacing workload present: {pacing:.1f}ms')
    finally:
        stop_sql(client, label)


def compile_worker(base, pm, major):
    binary = Path(os.readlink(f'/proc/{pm}/exe'))
    candidates = [Path(f'/usr/include/postgresql/{major}/server'),
                  Path(f'/usr/pgsql-{major}/include/server')]
    pg_config = binary.parent / 'pg_config'
    if pg_config.exists():
        candidates.insert(0, Path(subprocess.check_output(
            [str(pg_config), '--includedir-server'], text=True).strip()))
    headers = next((p for p in candidates if (p / 'postgres.h').is_file()), None)
    require(headers is not None, f'matching PG{major} server headers for deterministic controls')
    library = base / 'control.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I' + str(headers),
                    '-I' + str(headers.parent), str(ROOT / 'tests/ckpt_pacing_worker.c'),
                    '-o', str(library)], check=True)
    os.chmod(base, 0o755)
    os.chmod(library, 0o755)
    path = str(library).replace("'", "''")
    sql(f"CREATE FUNCTION pgwt804_launch(text,integer) RETURNS integer "
        f"AS '{path}', 'pgwt804_launch' LANGUAGE C STRICT")
    return path


def positive_controls(pm, base, mode, library):
    label = 'pgwt804_lock_holder'
    blocker = start_sql('SELECT pg_advisory_lock(804804); SELECT pg_sleep(90)', label)
    workers = []
    try:
        wait_for(lambda: sql("SELECT count(*) FROM pg_locks WHERE locktype='advisory' "
                             "AND objid=804804 AND granted") == '1', 5, 'advisory holder ready')
        waiter = int(sql(f"SELECT pgwt804_launch('{library}',2)"))
        workers.append(waiter)
        wait_for(lambda: sql(f"SELECT wait_event FROM pg_stat_activity WHERE pid={waiter}")
                 == 'advisory', 5, 'background worker in a real Lock:advisory wait')
        cpu = int(sql(f"SELECT pgwt804_launch('{library}',1)"))
        workers.append(cpu)
        sleeper = int(sql(f"SELECT pgwt804_launch('{library}',3)"))
        workers.append(sleeper)
        _, ticks_before = proc_stat(cpu)
        directory = base / ('controls_' + mode)
        result = capture(pm, directory, mode)
        require(result.returncode == 0, f'{mode} positive-control capture succeeds')
        _, ticks_after = proc_stat(cpu)
        os_cpu_ms = (ticks_after - ticks_before) * 1000 / os.sysconf('SC_CLK_TCK')
        require(os_cpu_ms > 2000, f'{mode} background CPU source actually ran {os_cpu_ms:.1f}ms')
        require(sql(f"SELECT wait_event FROM pg_stat_activity WHERE pid={waiter}") == 'advisory',
                f'{mode} resource request remained blocked for the entire capture')
        cpu_tm = report(directory, 'time_model', pid=cpu)
        require(float(cpu_tm['cpu_ms']) > 2000, f'{mode} real background CPU stays counted')
        waits = report(directory, 'top_events', pid=waiter)['rows']
        lock_ms = sum(float(r['total_ms']) for r in waits if r['name'] == 'Lock:advisory')
        waiter_tm = report(directory, 'time_model', pid=waiter)
        require(lock_ms >= 5000 and float(waiter_tm['db_time_ms']) >= 5000,
                f'{mode} non-pacing background resource wait stays DB Time ({lock_ms:.1f}ms)')
        # Discrimination control: a NON-checkpointer issuing the very same
        # 100 ms select/pselect6 sleep must NOT become CheckpointWriteDelay.
        # (The other two controls never call the hooked syscall, so they
        # cannot see an over-wide filter.)
        sl_events = report(directory, 'top_events', pid=sleeper)['rows']
        mis = sum(float(r['total_ms']) for r in sl_events
                  if r['name'] == 'Timeout:CheckpointWriteDelay')
        require(mis <= 200, f'{mode} non-checkpointer 100ms sleep is NOT '
                f'Timeout:CheckpointWriteDelay ({mis:.1f}ms)')
        if mode == 'full':
            sl_tm = report(directory, 'time_model', pid=sleeper)
            require(float(sl_tm['db_time_ms']) >= 5000,
                    f'full non-checkpointer waitless sleep stays DB Time '
                    f'({float(sl_tm["db_time_ms"]):.1f}ms)')
    finally:
        for pid in workers:
            sql(f'SELECT pg_terminate_backend({pid})')
        stop_sql(blocker, label)


def refusal(pm, base, ckpt, native):
    if native:
        print('SKIP refusal control: PG>=14.3 supplies native CheckpointWriteDelay', flush=True)
        return
    sql('CHECKPOINT')
    dirty()
    wait_pacing(ckpt, False)
    # Both modes must refuse independently of whether their sampler is active.
    for mode in ('full', 'tiered'):
        result = capture(pm, base / ('refusal_' + mode), mode, seconds=20, never_match=True)
        require(result.returncode != 0, f'{mode} inert filter refuses with nonzero exit')
        require('PGWT_TEST_PACING_FILTER_NEVER_MATCH=1' in result.stderr,
                f'{mode} test hook is loud')
        require('ERROR: INERT' in result.stderr and 'select/pselect6' in result.stderr
                and 'ckpt_pacing_probe_state=inert' in result.stderr,
                f'{mode} refusal reports both syscalls and inert state before shutdown')
        if mode == 'full':
            # A fresh dirty workload for the second daemon, never retry a capture.
            sql('CHECKPOINT')
            dirty()
            wait_pacing(ckpt, False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--modes', default='full,tiered',
                        help='capture modes to run (default both; narrowed only '
                             'to attribute a mutant to one mode)')
    args = parser.parse_args()
    modes = [m for m in args.modes.split(',') if m in ('full', 'tiered')]
    require(modes, 'at least one capture mode selected')
    pm = args.pid or find_postmaster()
    require(os.geteuid() == 0 and sys.platform.startswith('linux'), 'Linux root live harness')
    version = int(sql('SHOW server_version_num'))
    major = version // 10000
    native = version >= 140003
    require(major >= 13, f'supported PostgreSQL {version}')
    ckpt = int(sql("SELECT pid FROM pg_stat_activity WHERE backend_type='checkpointer'"))
    # ALTER SYSTEM is restored exactly: preserve an existing auto.conf override
    # instead of converting an inherited setting into a permanent override.
    settings = {}
    for name in ('checkpoint_timeout', 'checkpoint_completion_target'):
        value = sql(f"SELECT setting FROM pg_file_settings WHERE name='{name}' "
                    "AND sourcefile LIKE '%/postgresql.auto.conf' ORDER BY seqno DESC LIMIT 1")
        settings[name] = value or None
    with tempfile.TemporaryDirectory(prefix='pgwt804_', dir='/tmp') as tmp:
        base = Path(tmp)
        try:
            library = compile_worker(base, pm, major)
            sql("ALTER SYSTEM SET checkpoint_timeout='30s'")
            sql('ALTER SYSTEM SET checkpoint_completion_target=0.9')
            sql('SELECT pg_reload_conf()')
            sql(f'CREATE TABLE {TABLE} WITH (autovacuum_enabled=false) AS '
                'SELECT g, md5(g::text) m FROM generate_series(1,3000000) g')
            for mode in modes:
                quantity(pm, base, mode, ckpt, native)
                positive_controls(pm, base, mode, library)
            refusal(pm, base, ckpt, native)
        finally:
            # Python finally is the trap: SIGTERM/SIGINT are converted to an
            # exception below so every exit restores GUCs and removes fixtures.
            for name, value in settings.items():
                sql(f'ALTER SYSTEM RESET {name}' if value is None else
                    f"ALTER SYSTEM SET {name}='{value.replace(chr(39), chr(39)*2)}'")
            sql('SELECT pg_reload_conf()')
            sql(f'DROP TABLE IF EXISTS {TABLE}')
            sql('DROP FUNCTION IF EXISTS pgwt804_launch(text,integer)')
    print('PASS checkpointer pacing live suite', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise RuntimeError(f'interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    main()
