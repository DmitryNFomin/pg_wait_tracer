"""Harness-only cut/terminal-discard stimulus; no installed binary uses it."""
from contextlib import contextmanager
import os
import json
import re
import subprocess
import tempfile
from pathlib import Path
import shlex

from test_transport_producers_live import Refused, checked

ROOT = Path(__file__).resolve().parents[1]


def replace_once(source, before, after):
    assert source.count(before) == 1, ('cutdiscard anchor is not unique', before)
    return source.replace(before, after)


def bpf_source(source, victim, uncounted=False):
    # Same reservation/give-up shape as the existing odd-PID INIT stimulus,
    # but a withdrawn attempt is counted as a discard, never as a failure.
    # No maps, rodata fields, wire layouts or skeleton member layouts change.
    stimulus = '''
    /* CUTDISCARD TEST BUILD ONLY: main thread admitted for one syscall. */
    if (tid == (u32)(id >> 32)) {
        if (cpu == VICTIM_CPU) {
            transport_attempt(PGWT_WIRE_DEST_E);
            struct pgwt_wire_cut *withdrawn =
                bpf_ringbuf_reserve(&event_ringbuf, sizeof(*withdrawn), 0);
            if (!withdrawn) {
                transport_failure(PGWT_WIRE_DEST_E);
                return 0;
            }
            COUNT_DISCARD
            bpf_ringbuf_discard(withdrawn, 0);
        } else {
            transport_boundary(ctx, PGWT_WIRE_DEST_E, round);
            transport_boundary(ctx, PGWT_WIRE_DEST_L, round);
        }
        return 0;
    }
'''.replace('VICTIM_CPU', str(victim)).replace(
        'COUNT_DISCARD', '/* mutant: accounting removed */' if uncounted else
        'transport_discard(PGWT_WIRE_DEST_E);')
    anchor = '    if (!round) return 0;\n'
    return replace_once(source, anchor, anchor + stimulus)


def reader_source(source):
    anchor = 'static struct linux_transport *linux_state(const struct pgwt_daemon *d)'
    declarations = '''static void cd_before_targets(struct linux_transport *);
static bool cd_allow_ring(struct linux_transport *);
static void cd_positions(struct linux_transport *, const uint64_t [2], const uint64_t [2]);
'''
    source = replace_once(source, anchor, declarations + anchor)
    source = replace_once(source,
        '{ struct linux_transport *l = ctx; return percpu_triples(l, l->attempts_fd, out, count); }',
        '{ struct linux_transport *l = ctx; cd_before_targets(l); '
        'return percpu_triples(l, l->attempts_fd, out, count); }')
    anchor = '        produced[i] = ring__producer_pos(r);\n    }\n'
    source = replace_once(source, anchor, anchor + '    cd_positions(l, produced, consumed);\n')
    source = replace_once(source,
        '            r->consumed = 0; r->limit = 4096;',
        '            if (!cd_allow_ring(l)) { *empty = false; continue; }\n'
        '            r->consumed = 0; r->limit = 4096;')
    anchor = '    r->consumed = 0; r->limit = bounded ? 4096 : 65536;'
    source = replace_once(source, anchor,
        '    if (!cd_allow_ring(l)) return dest == PGWT_WIRE_DEST_E ? -EAGAIN : 0;\n' + anchor)
    anchor = 'static int linux_helpers_start(void *);'
    return replace_once(source, anchor, '#include "transport_cutdiscard.h"\n' + anchor)


def build(directory, victim, uncounted=False):
    """Build only on Linux, in scratch. Reuse the project objects except the
    instrumented reader and daemon.c (which embeds the test BPF skeleton).
    The test BPF has exactly the product's map/prog/rodata member layouts,
    so the remaining objects use the same skeleton ABI.
    """
    name = 'cutdiscard-uncounted' if uncounted else 'cutdiscard'
    target = directory / name
    target.mkdir()
    variables = ('CC', 'CFLAGS', 'LDFLAGS', 'USER_OBJS', 'CLANG', 'BPF_CFLAGS', 'BPFTOOL')
    recipe = '.PHONY: pgwt-cd-config\npgwt-cd-config:\n\t@:\n'
    recipe += ''.join(f'$(info PGWT_CONFIG_{key}=$({key}))\n' for key in variables)
    output = checked(['make', '-s', '--no-print-directory', '-f', 'Makefile', '-f', '-',
                      'pgwt-cd-config'], input=recipe, cwd=ROOT)
    config = {}
    for line in output.splitlines():
        if line.startswith('PGWT_CONFIG_'):
            key, value = line[len('PGWT_CONFIG_'):].split('=', 1)
            config[key] = shlex.split(value)
    assert set(config) == set(variables), output
    source = target / 'cutdiscard.bpf.c'
    source.write_text(bpf_source((ROOT / 'src/bpf/pg_wait_tracer.bpf.c').read_text(), victim, uncounted))
    obj = target / 'pg_wait_tracer.bpf.o'
    checked([*config['CLANG'], *config['BPF_CFLAGS'], '-I', str(ROOT / 'src/bpf'),
             '-c', str(source), '-o', str(obj)], cwd=ROOT)
    skeleton = checked([*config['BPFTOOL'], 'gen', 'skeleton', str(obj)], cwd=ROOT)
    (target / 'pg_wait_tracer.skel.h').write_text(skeleton + '\n')
    replacements = {}
    for unit in ('daemon', 'percpu_transport'):
        source = (ROOT / f'src/{unit}.c').read_text()
        if unit == 'percpu_transport':
            source = reader_source(source)
            source = ('#define PGWT_CUTDISCARD_READY ' + json.dumps(str(target / 'ready')) + '\n' +
                      f'#define PGWT_CUTDISCARD_UNCOUNTED {int(uncounted)}\n' + source)
        path = target / f'{unit}.c'
        path.write_text(source)
        compiled = target / f'{unit}.o'
        checked([*config['CC'], '-I', str(target), '-I', str(ROOT / 'tests'),
                 *config['CFLAGS'], '-c', str(path), '-o', str(compiled)], cwd=ROOT)
        replacements[f'{unit}.o'] = str(compiled)
    objects = [replacements.get(Path(item).name, item) for item in config['USER_OBJS']]
    assert sum(Path(item).name in replacements for item in config['USER_OBJS']) == 2
    binary = target / 'pg_wait_tracer'
    checked([*config['CC'], *config['CFLAGS'], *objects, '-o', str(binary), *config['LDFLAGS']], cwd=ROOT)
    print(f'BUILT {name}: {binary}', flush=True)
    return binary


def process_identity(pid):
    # Start time protects cleanup against PID reuse.
    return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]


@contextmanager
def postgres_away_from_cpu(pm, victim):
    """Reserve the non-issuer E key without suppressing product events.
    Pin the postmaster first: newly forked backends inherit its restriction.
    Restore living original tasks and newly created descendants on exit.
    This is a mechanism stimulus, not an overhead/throughput measurement.
    """
    saved = {}
    original = os.sched_getaffinity(pm)
    if not original - {victim}:
        raise Refused('cutdiscard needs another CPU for PostgreSQL traffic')
    def tasks():
        # Restrict this postmaster's process tree, never another cluster.
        parents = {}
        for path in Path('/proc').iterdir():
            if path.name.isdecimal():
                try:
                    fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
                    parents[int(path.name)] = int(fields[1])
                except (FileNotFoundError, ProcessLookupError):
                    continue
        descendants = {pm}
        while True:
            children = {pid for pid, parent in parents.items() if parent in descendants}
            if children <= descendants:
                return descendants
            descendants.update(children)

    def pin(pid):
        try:
            identity = process_identity(pid)
            mask = os.sched_getaffinity(pid)
            if not mask - {victim}:
                raise Refused(f'PostgreSQL pid {pid} cannot run away from cutdiscard CPU {victim}')
            saved[pid] = (identity, mask)
            os.sched_setaffinity(pid, mask - {victim})
        except (FileNotFoundError, ProcessLookupError):
            pass

    try:
        pin(pm)
        for pid in tasks():
            if pid != pm:
                pin(pid)
        print(f'CUTDISCARD affinity: PostgreSQL excludes CPU {victim}; restored in cleanup', flush=True)
        yield
    finally:
        for pid in tasks():
            try:
                identity = process_identity(pid)
                old = saved.get(pid)
                if old and old[0] == identity:
                    os.sched_setaffinity(pid, old[1])
                elif not old:
                    os.sched_setaffinity(pid, original)
            except (FileNotFoundError, ProcessLookupError):
                pass


def assert_schedule(row):
    text, stats = row['stderr'], row['stats']
    assert 'CUTDISCARD injected ' in text, 'kernel final-discard stimulus reached'
    assert 'consumed_seq=0 waiting_data=1 ' in text, 'real watchpoint DATA was waiting before target freeze'
    assert 'CUTDISCARD cut ' in text, 'real producer-position cut reached after every acknowledgement'
    assert 'CUTDISCARD held no_premature_release=1 traffic_after_cut=1 draining=1' in text, \
        'held without release while other kernel traffic continued'
    assert stats['holds'] > 0 and stats['cuts'] > 0, 'holds > 0 and cuts > 0'
    assert stats['acks_before_fence'] == 0, 'acknowledgements follow fence request'
    assert stats['perf_lost'] == 0, 'no perf LOST in cutdiscard schedule'
    assert all(cell['read'] == 1 and cell['failures'] == 0 for cell in row['cells'].values()), \
        'no transport failure in cutdiscard schedule'
    baseline = re.search(r'^CUTDISCARD injected .* completed=(\d+) data=(\d+) text=(\d+) frontiers=(\d+)$',
                         text, re.M)
    assert baseline, 'release baseline recorded at target freeze'
    return tuple(map(int, baseline.groups()))


def assert_counted(row):
    completed, _, _, _ = assert_schedule(row)
    assert not row['dump']['voids'], 'counted final discard completes without VOID'
    assert row['stats']['rounds_completed'] > completed, 'stimulated round eventually completes'
    assert row['stats']['terminal_discards'] > 0, 'terminal_discards > 0'
    assert row['cells']['E']['discards'] >= 1, 'kernel E discard reconciled'


def assert_mutant(row):
    completed, data, text, frontiers = assert_schedule(row)
    assert row['stats']['sequence_gap'] > 0 and row['stats']['status'] != 0 and \
        row['dump']['voids'] == [('sequence_gap', {})] and row['dump']['end']['status'] != 0, \
        'uncounted final discard is VOID(sequence_gap)'
    assert row['stats']['rounds_completed'] == completed and row['dump']['data'] == data and \
        row['dump']['text'] == text and row['dump']['frontiers'] == frontiers, \
        'mutant releases nothing after stimulus'
    assert row['cells']['E']['attempted'] == row['cells']['E']['delivered'] + row['cells']['E']['discards'] + 1, \
        'mutant has exactly the deliberately uncounted E attempt'
    assert all(row['cells'][dest]['attempted'] == row['cells'][dest]['delivered'] +
               row['cells'][dest]['discards'] for dest in ('CPU', 'L')), \
        'mutant reconciles every other destination'


def dry_run():
    """Portable source-anchor and verdict checks; no BPF compilation/loading.
    Kernel scheduling/positions are deliberately not claimed by this check.
    """
    from copy import deepcopy
    source = (ROOT / 'src/bpf/pg_wait_tracer.bpf.c').read_text()
    counted = bpf_source(source, 1)
    mutant = bpf_source(source, 1, True)
    assert counted.replace('            transport_discard(PGWT_WIRE_DEST_E);',
                           '            /* mutant: accounting removed */', 1) == mutant
    reader_source((ROOT / 'src/percpu_transport.c').read_text())
    row = dict(stderr='CUTDISCARD injected cpu=1 issuer=0 dest=E attempted=1 consumed_seq=0 waiting_data=1 '
                      'completed=0 data=0 text=0 frontiers=0\n'
                      'CUTDISCARD cut E=56 L=56 consumer_E=0 acks=2\n'
                      'CUTDISCARD held no_premature_release=1 traffic_after_cut=1 draining=1\n',
               stats=dict(holds=1, cuts=1, acks_before_fence=0, perf_lost=0, terminal_discards=1,
                          sequence_gap=0, status=0, rounds_completed=1),
               cells={dest: dict(read=1, failures=0, discards=int(dest == 'E'),
                                 attempted=3, delivered=3 - int(dest == 'E'))
                      for dest in ('CPU', 'E', 'L')},
               dump=dict(voids=[], end=dict(status=0), data=1, text=0, frontiers=1))
    assert_counted(row)
    broken = deepcopy(row)
    broken['cells']['E']['discards'] = 0
    broken['stats'].update(terminal_discards=0, sequence_gap=1, status=1, rounds_completed=0)
    broken['dump'].update(voids=[('sequence_gap', {})], end=dict(status=1), data=0, frontiers=0)
    assert_mutant(broken)
    for case in (broken, {**row, 'stderr': ''}):
        try:
            assert_counted(case)
        except AssertionError:
            pass
        else:
            raise AssertionError('cutdiscard oracle accepted missing accounting or stimulus')
    for change in ({'voids': [('allocation_fail', {})]}, {'data': 1}, {'text': 1}, {'frontiers': 1}):
        case = deepcopy(broken)
        case['dump'].update(change)
        try:
            assert_mutant(case)
        except AssertionError:
            pass
        else:
            raise AssertionError('mutant oracle accepted the wrong VOID or premature release')
    with tempfile.TemporaryDirectory(prefix='pgwt-cutdiscard-host-') as tmp:
        directory = Path(tmp)
        ready = directory / 'ready'
        ready.touch()
        binary = directory / 'scheduler'
        checked([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-DPGWT_CUTDISCARD_READY=' + json.dumps(str(ready)),
                 '-DPGWT_CUTDISCARD_UNCOUNTED=0',
                 str(ROOT / 'tests/transport_cutdiscard_host.c'), '-o', str(binary)])
        print(checked([str(binary)]), flush=True)
        for name, assertion in (
                ('missing-ack', 'every CPU acknowledgement consumed before cut'),
                ('premature-data', 'no completion, DATA/TEXT release or FRONTIER before passing cut'),
                ('premature-text', 'no completion, DATA/TEXT release or FRONTIER before passing cut'),
                ('premature-frontier', 'no completion, DATA/TEXT release or FRONTIER before passing cut'),
                ('premature-completion', 'no completion, DATA/TEXT release or FRONTIER before passing cut')):
            result = subprocess.run([str(binary), name], text=True, capture_output=True, timeout=5)
            assert result.returncode < 0 and f'CUTDISCARD ASSERT: {assertion}\n' in result.stderr, \
                (name, 'scheduler control survived or failed elsewhere', result.stdout, result.stderr)
        print('CUTDISCARD HOST controls: missing ack and premature DATA/TEXT/FRONTIER/completion rejected')
    print('CUTDISCARD DRY RUN PASS: source anchors, scheduler and verdict controls; live scheduling UNVERIFIED')
