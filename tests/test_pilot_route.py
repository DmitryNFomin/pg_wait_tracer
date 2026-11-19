#!/usr/bin/env python3
"""Fixture bytes -> real server -> frozen independent metrics.

Default: portable build and all 64 scenarios, both block sizes, strict parsing,
shifted clocks, UINT64_MAX serializer, v4 and, when installed, real Chromium.
The unit tier reports missing browser modules; --require-browser makes their
absence fatal in the dedicated browser gates. --mutants
builds each broken variant separately and requires its intended assertion red.
Generated sources, binaries, trace bytes and browser artifacts stay in /tmp.
"""
import argparse
import copy
import importlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

from server_build import server_sources

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / 'contracts/evidence-v1'
COMPARED = tuple(list(range(1, 23)) + list(range(24, 28)) + list(range(31, 34)) +
                 list(range(38, 50)) + [55] + list(range(57, 65)))
AMBIGUOUS = (50, 51)
REFUSED = (23, 28, 29, 30, 34, 35, 36, 37, 52, 53, 54, 56)
METRICS = ('wait_time', 'completed_waits')


def command(args, **kwargs):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True,
                          capture_output=True, text=True, **kwargs)


class Build:
    def __init__(self, tmp):
        self.tmp = Path(tmp)
        self.cc = shlex.split(os.environ.get('CC', 'cc'))
        self.flags = shlex.split(os.environ.get('CFLAGS', '-O1 -g'))
        self.flags += ['-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                       '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'include'),
                       '-I' + str(ROOT / 'tests')]
        self.core = [ROOT / ('src/core/' + s + '.c') for s in
                     ('evidence_query', 'evidence_codec', 'evidence_replay', 'wait_metrics')]
        self.writer = self.tmp / 'writer'
        command(self.cc + self.flags + [ROOT / 'tests/pilot_fixture_writer.c',
                ROOT / 'tests/evidence_json.c', ROOT / 'src/core/contract_json.c'] + self.core[1:] +
                ['-o', self.writer])
        self.sources = server_sources()
        self.server_flags = self.flags + ['-DPGWT_SERVER', '-include', str(ROOT / 'demos/replay-compat.h')]
        self.libs = ['-lz', '-llz4', '-lm']
        if os.uname().sysname == 'Darwin':
            brew = command(['brew', '--prefix']).stdout.strip()
            self.server_flags += ['-I' + brew + '/include']
            self.libs += ['-L' + brew + '/lib']
        self.objects = []
        for i, source in enumerate(self.sources):
            obj = self.tmp / ('source-' + str(i) + '.o')
            command(self.cc + self.server_flags + ['-c', source, '-o', obj])
            self.objects.append(obj)
        self.server = self.link('server')

    def link(self, name, replacements=None):
        objects = list(self.objects)
        for relative, source in (replacements or {}).items():
            index = self.sources.index(ROOT / relative)
            path = self.tmp / (name + '-' + Path(relative).name)
            path.write_text(source)
            obj = path.with_suffix('.o')
            flags = self.server_flags + ['-I' + str((ROOT / relative).parent)]
            command(self.cc + flags + ['-c', path, '-o', obj])
            objects[index] = obj
        binary = self.tmp / name
        command(self.cc + self.flags + objects + self.libs + ['-o', binary])
        return binary


def cases():
    paths = sorted((CONTRACT / 'scenarios').glob('*.json')) + sorted((CONTRACT / 'r3/scenarios').glob('*.json'))
    data = {int(p.name[:2]): (p, json.loads(p.read_text())) for p in paths}
    assert set(data) == set(range(1, 65)), 'scenario inventory changed'
    assert len(COMPARED) == 50 and len(AMBIGUOUS) == 2 and len(REFUSED) == 12
    assert set(COMPARED + AMBIGUOUS + REFUSED) == set(data), 'pinned groups must cover exactly'
    expected = {}
    for path in (CONTRACT / 'expected.json', CONTRACT / 'r3/expected.json'):
        expected.update({int(c['id'][:2]): {k: c[k] for k in METRICS} for c in json.loads(path.read_text())})
    return data, expected


def wire_request(case):
    target = copy.deepcopy(case['target'])
    if target['fork'] is not None:
        target['fork'] = str(target['fork'])
    query = copy.deepcopy(case['query'])
    for name in ('from', 'to'):
        query[name] = str(query[name])
    return dict(id=7, cmd='pilot_wait_metrics', target=target, query=query)


def fixture(build, path, directory, block):
    directory.mkdir()
    return subprocess.run([str(build.writer), str(path), str(directory), str(block)],
                          capture_output=True, text=True, timeout=20)


def route(server, directory, request):
    global LAST_STDERR
    result = command([server, directory], input=json.dumps(request) + '\n', timeout=20)
    raw = result.stdout
    LAST_STDERR = result.stderr
    assert raw.startswith('{"id":7,'), 'response must be id-first'
    lines = raw.splitlines()
    assert len(lines) == 1, raw
    return raw, json.loads(raw)


def metric_integer_tree(value):
    if isinstance(value, dict):
        return {k: metric_integer_tree(v) for k, v in value.items()}
    if isinstance(value, list):
        return [metric_integer_tree(v) for v in value]
    if isinstance(value, str) and re.fullmatch('[0-9]+', value):
        assert int(value) <= 2**64 - 1
        return int(value)
    return value


def compare(response, expected, request):
    assert set(response) == {'id', 'pilot', 'contract', 'target', 'query', *METRICS, 'storage'}, response
    assert response['pilot'] is True and response['contract'] == 'evidence-v1'
    assert response['target'] == request['target'] and response['query'] == request['query']
    for key in METRICS:
        assert metric_integer_tree(response[key]) == expected[key], (key, response[key], expected[key])
        # JSON numbers are allowed only for the schema version, never a uint64.
        def strings(node, name=''):
            if isinstance(node, dict):
                for k, v in node.items(): strings(v, k)
            elif isinstance(node, list):
                for v in node: strings(v)
            elif isinstance(node, int):
                assert name == 'version', (name, node)
        strings(response[key])


def shift(case):
    case = copy.deepcopy(case)
    temporal = {'t', 'fork', 'start', 'exit', 'invalid_from'}
    for file in case['files']:
        for record in file['records']:
            for key in temporal & record.keys():
                if record[key] is not None: record[key] += 2**63
    if case['target']['fork'] is not None: case['target']['fork'] += 2**63
    for key in ('from', 'to'): case['query'][key] += 2**63
    return case


def legacy_v4(directory):
    # A real legacy writer file, committed fixture bytes, never v5 bytes relabelled.
    paths = sorted((ROOT / 'tests').glob('**/*.trace*'))
    for path in paths:
        if 'results' in path.parts: continue
        if path.is_file():
            data = path.read_bytes()
            if data[:6] == b'PGWT\x04\x00':
                # Use the legacy scanner's current-file name so this case
                # exercises a readable v4 trace, including startup admission.
                (directory / 'current.trace').write_bytes(data)
                return
    raise AssertionError('real v4 fixture required')


def baseline(build, tmp, data, expected):
    raw_by_id = {}
    for block in (1, 4096):
        for group, ids in (('compared', COMPARED), ('ambiguous', AMBIGUOUS), ('refused', REFUSED)):
            print(f'block_records={block} {group}: ' + ', '.join(data[i][1]['id'] for i in ids), flush=True)
            for i in ids:
                path, case = data[i]
                directory = tmp / f'{block}-{i}'
                written = fixture(build, path, directory, block)
                if group == 'refused':
                    assert written.returncode == 3 and f'REFUSED {case["id"]}:' in written.stderr, written.stderr
                    assert not list(directory.iterdir()), 'encoder refusal published bytes'
                    continue
                assert written.returncode == 0, written.stderr
                request = wire_request(case)
                raw, response = route(build.server, directory, request)
                storage = response['storage']
                assert not storage['codec_failed'] or storage['codec_stops'], storage
                if group == 'ambiguous':
                    assert response['code'] == 'pilot_unsupported' and response['storage']['capture_ambiguous'] is True
                    assert 'ambiguous' in response['error']
                    assert not any(key in response for key in METRICS)
                else:
                    compare(response, expected[i], request)
                if block == 1: raw_by_id[i] = raw
                else: assert raw == raw_by_id[i], f'block split changed response {case["id"]}'
        print(f'block_records={block}: 50 compared, 2 ambiguous, 12 refused, 0 skipped', flush=True)
    for i in (1, 3, 5, 25):
        shifted = shift(data[i][1])
        path = tmp / f'shift-{i}.json'
        path.write_text(json.dumps(shifted))
        directory = tmp / f'shift-{i}'
        assert fixture(build, path, directory, 1).returncode == 0
        request = wire_request(shifted)
        _, response = route(build.server, directory, request)
        compare(response, expected[i], request)
        print(f'shifted 2^63 scenario {i:02}: PASS', flush=True)
    strict_parsing(build, tmp / '1-1', data[1][1])
    directory = tmp / 'v4'
    directory.mkdir()
    legacy_v4(directory)
    request = wire_request(data[30][1])
    _, response = route(build.server, directory, request)
    compare(response, expected[30], request)
    print('real v4 -> frozen 30: PASS', flush=True)


def strict_parsing(build, directory, case):
    for field in ('from', 'to'):
        for bad in (None, 1, -1, '', ' 1', '1 ', '-1', '+1', '1.0', '1e3', '18446744073709551616', '1\x00x'):
            req = wire_request(case)
            req['query'][field] = bad
            _, response = route(build.server, directory, req)
            for metric in METRICS:
                assert response[metric]['value'] is None and 'invalid_query' in response[metric]['reasons'], (bad, response)
    for field, value in (('event', ''), ('event', None)):
        req = wire_request(case); req['query'][field] = value
        _, response = route(build.server, directory, req)
        assert 'invalid_query' in response['wait_time']['reasons']
    print('strict uint64 parser: PASS', flush=True)


def startup_refusals(build, tmp, data, expected):
    for name, filename in (('empty current.trace', 'current.trace'),
                           ('sidecar only', 'current.trace.meta')):
        directory = tmp / filename
        directory.mkdir()
        (directory / filename).write_bytes(b'')
        result = subprocess.run([str(build.server), str(directory)],
                                input='{"id":7,"cmd":"info"}\n',
                                capture_output=True, text=True, timeout=20)
        assert result.returncode != 0, (name, result.stdout, result.stderr)
        assert 'no trace files found' in result.stderr, result.stderr
        assert not result.stdout, result.stdout
        print(f'startup {name}: no trace files found PASS', flush=True)

    directory = tmp / 'v5-only'
    assert fixture(build, data[1][0], directory, 1).returncode == 0
    files = list(directory.iterdir())
    assert len(files) == 1 and files[0].read_bytes()[:6] == b'PGWT\x05\x00'
    request = wire_request(data[1][1])
    result = command([build.server, directory],
                     input='{"id":6,"cmd":"info"}\n' + json.dumps(request) + '\n',
                     timeout=20)
    info, response = map(json.loads, result.stdout.splitlines())
    assert info['id'] == 6 and 'error' not in info, info
    compare(response, expected[1], request)
    print('startup v5 only: info and pilot_wait_metrics PASS', flush=True)


def routing_probe(build, name, replacements=None):
    # Call the same loader/query/serializer without legacy-reader startup:
    # master's trace reader does not yet admit v6-only directories.
    source = r'''
#define main pilot_original_server_main
#include "SERVER_SOURCE"
#undef main
#include <assert.h>
int main(int argc, char **argv)
{
    assert(argc == 2);
    pgwt_evidence_buffer buffers[PGWT_PILOT_MAX_FILES];
    size_t count=0;
    assert(!pilot_buffers(argv[1],buffers,&count));
    pgwt_evidence_target target={.capture={"A",1},.pid=101,.has_fork=true,.fork=0};
    pgwt_core_query query={.event={"PgSleep",7},.from=1,.to=15};
    pgwt_core_result wait_time,completed;
    pgwt_query_storage storage;
    const char *reason=NULL;
    int status=pgwt_evidence_query(buffers,count,&target,&query,&wait_time,&completed,&storage,&reason);
    cJSON *root=cJSON_CreateObject();
    cJSON_AddNumberToObject(root,"status",status);
    if (reason) cJSON_AddStringToObject(root,"reason",reason);
    pilot_storage(root,&storage);
    if (!status) {
        cJSON_AddItemToObject(root,"wait_time",pilot_metric(&wait_time));
        cJSON_AddItemToObject(root,"completed_waits",pilot_metric(&completed));
    }
    emit_json(root);
    for (size_t i=0;i<count;++i) free((void *)buffers[i].data);
    return 0;
}
'''.replace('SERVER_SOURCE', str(ROOT / 'src/server.c'))
    return build.link(name, {'src/server.c': source, **(replacements or {})})


def trace_bytes(version, coverage=False):
    # v2/v3's 28-byte header; v4/v6 add the nominal sample period.
    header = struct.pack('<4sHHIQQ', b'PGWT', version, 0, 17, 0, 0)
    if version >= 4:
        header += struct.pack('<Q', 100_000_000)
    if not coverage:
        return header
    assert version == 6
    # U1 wire format: raw type 3, BOOT payload schema 1, CRC-32C + pad.
    # Hand-built here so the test needs neither U1's writer nor its headers.
    payload = struct.pack('<4sHBBQQQQ', b'PGWC', 1, 1, 0, 1, 0, 0, 0)
    payload += bytes(range(16)) + bytes(range(16, 32))
    crc = 0xffffffff
    for byte in payload:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    payload += struct.pack('<II', crc ^ 0xffffffff, 0)
    assert len(payload) == 80
    block = struct.pack('<QQIIIHHQQ', 1, 1, 1, len(payload), len(payload), 3, 0, 0, 0)
    return header + block + payload


def routing_cases(build, tmp, data, expected, run_mutants=False):
    binary = routing_probe(build, 'routing-probe')
    directory = tmp / 'routing-v5-source'
    assert fixture(build, data[1][0], directory, 1).returncode == 0
    v5, = directory.iterdir()
    v5 = v5.read_bytes()

    def invoke(executable, label, files):
        directory = tmp / label
        directory.mkdir()
        for name, payload in files.items():
            (directory / name).write_bytes(payload)
        return json.loads(command([executable, directory], timeout=20).stdout)

    def metrics(response):
        assert response['status'] == 0, response
        for key in METRICS:
            assert metric_integer_tree(response[key]) == expected[1][key], (key, response)

    def clean(response, count):
        storage = response['storage']
        assert storage['files'] == count, storage
        assert storage['codec_failed'] is False and storage['codec_stops'] == [], (
            'storage codec_failed/stops', storage)

    def failure(response, file):
        storage = response['storage']
        assert storage['codec_failed'] is True, ('codec failure required', response)
        assert any(s['file'] == file and s['reason'] == 'IH' for s in storage['codec_stops']), (
            'failure must name offending file', file, response)

    alone = invoke(binary, 'routing-v5', {'evidence.trace': v5})
    metrics(alone)
    clean(alone, 1)
    print('routing v5 alone: PASS', flush=True)
    for version, coverage in ((2, False), (3, False), (4, False), (6, False), (6, True)):
        label = f'v{version}' + ('-coverage' if coverage else '')
        trace = trace_bytes(version, coverage)
        response = invoke(binary, 'routing-' + label, {'current.trace': trace})
        clean(response, 1)
        assert response['status'] == 0, response
        for key in METRICS:
            assert response[key]['value'] is None and 'evidence_invalid' in response[key]['reasons'], response
        # Put the ignored trace on either side of the evidence file.
        for position in ('00', '99'):
            response = invoke(binary, f'routing-mixed-{label}-{position}', {
                position + '-trace.trace': trace, '50-evidence.trace': v5})
            metrics(response)
            clean(response, 2)
        print(f'routing {label}: alone unavailable; mixed v5 unchanged; files counted PASS', flush=True)
    for label, payload in (('v7', trace_bytes(7)), ('corrupt-v5', v5[:20])):
        response = invoke(binary, 'routing-' + label, {'bad.trace': payload})
        failure(response, 0)
        assert response['storage']['files'] == 1
        for position, file in (('00', 0), ('99', 2)):
            response = invoke(binary, f'routing-mixed-{label}-{position}', {
                position + '-bad.trace': payload, '50-evidence.trace': v5,
                '51-trace.trace': trace_bytes(6)})
            failure(response, file)
            assert response['storage']['files'] == 3
        print(f'routing {label}: IH names original file, alone and mixed PASS', flush=True)

    corrupt = bytearray(v5)
    first_block, = struct.unpack_from('<I', corrupt, 40)
    corrupt[first_block + 64] ^= 1  # Keep a valid header; break the first block CRC.
    response = invoke(binary, 'routing-corrupt-block', {
        '00-trace.trace': trace_bytes(6), '99-bad.trace': corrupt})
    storage = response['storage']
    assert storage['codec_failed'] is True and storage['codec_stops'], response
    assert storage['codec_stops'][0]['file'] == 1, response
    assert storage['codec_stops'][0]['reason'] == 'CRC', response
    print('routing corrupt v5 block: CRC names original file PASS', flush=True)

    if not run_mutants:
        return
    source = (ROOT / 'src/core/evidence_query.c').read_text()
    mutants = (
        ('drop_v6', 'case 2: case 3: case 4: case 6: return true;',
         'case 2: case 3: case 4: return true;',
         {'00-trace.trace': trace_bytes(6), '50-evidence.trace': v5}, clean, 2,
         'storage codec_failed/stops'),
        ('not_v5', 'switch (b.data[4]) {',
         'if (b.data[4] != 5) return true;\n    switch (b.data[4]) {',
         {'bad.trace': trace_bytes(7)}, failure, 0, 'codec failure required'),
        ('drop_file', '(pgwt_query_stop){.file=file,', '(pgwt_query_stop){.file=0,',
         {'00-trace.trace': trace_bytes(6), '50-evidence.trace': v5, '99-bad.trace': v5[:20]},
         failure, 2, 'failure must name offending file'),
    )
    for name, old, new, files, assertion, argument, wanted in mutants:
        assert source.count(old) == 1, (name, 'mutation site count')
        marker = 'MUTANT_REACHED:' + name
        changed = '#include <stdio.h>\n' + source.replace(old, new)
        if name == 'drop_file':
            changed = changed.replace('.file=0,', f'.file=(fprintf(stderr,"{marker}\\n"),0),')
        else:
            entry = '    switch (b.data[4]) {' if name == 'drop_v6' else '    if (b.data[4] != 5)'
            assert changed.count(entry) == 1
            changed = changed.replace(entry, f'    fprintf(stderr,"{marker}\\n");\n' + entry)
        mutant = routing_probe(build, 'routing-' + name, {'src/core/evidence_query.c': changed})
        directory = tmp / ('routing-mutant-' + name)
        directory.mkdir()
        for filename, payload in files.items():
            (directory / filename).write_bytes(payload)
        control = json.loads(command([binary, directory], timeout=20).stdout)
        assertion(control, argument)
        result = command([mutant, directory], timeout=20)
        assert marker in result.stderr, (name, 'mutation not reached')
        try:
            assertion(json.loads(result.stdout), argument)
        except AssertionError as error:
            assert wanted in str(error), (name, 'wrong assertion', error)
        else:
            raise AssertionError(name + ' SURVIVED')
        print(f'routing mutant {name}: BUILT; REACHED; RED {wanted}', flush=True)


def gallery_fixtures(build, tmp, data):
    import mock_server

    assert [f['scenario'] for f in mock_server.PILOT_FIXTURES] == [data[i][1]['id'] for i in (1, 5)]
    for fixture_data in mock_server.PILOT_FIXTURES:
        i = int(fixture_data['scenario'][:2])
        request = wire_request(data[i][1])
        assert fixture_data['request'] == {key: request[key] for key in ('target', 'query')}
        _, real = route(build.server, tmp / f'1-{i}', request)
        assert mock_server.handle_request(request) == real, fixture_data['name']
        print(f'gallery {fixture_data["name"]}: mock fixture equals real route PASS', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutants', action='store_true')
    browser_mode = parser.add_mutually_exclusive_group()
    browser_mode.add_argument('--routing-only', action='store_true', help='host-only container routing and failure provenance')
    browser_mode.add_argument('--no-browser', action='store_true', help='route-only diagnostic run; not acceptance')
    browser_mode.add_argument('--require-browser', action='store_true', help='browser gate: missing dependencies fail')
    args = parser.parse_args()
    data, expected = cases()
    with tempfile.TemporaryDirectory(prefix='pgwt-pilot-route-') as work:
        tmp = Path(work)
        build = Build(tmp)
        routing_cases(build, tmp, data, expected, args.mutants)
        if args.routing_only:
            return
        if args.mutants:
            mutants(build, tmp, data, expected)
            browser_split(tmp)
        else:
            startup_refusals(build, tmp, data, expected)
            baseline(build, tmp, data, expected)
            gallery_fixtures(build, tmp, data)
            serializer_identity(build, tmp)
            if args.no_browser:
                print('browser arm NOT RUN: --no-browser diagnostic', flush=True)
            elif browser_available(args.require_browser):
                browser(build, tmp, data, expected)


def browser_available(required):
    # Probe only the two optional imports. Browser/runtime/assertion failures
    # happen outside this handler and always fail, as in the other UI suites.
    for module in ('websockets', 'playwright.sync_api'):
        try:
            importlib.import_module(module)
        except ImportError as exc:
            # An installed dependency with a broken transitive import is a
            # failure, not evidence that the optional dependency is absent.
            if exc.name not in (module, module.split('.')[0]):
                raise
            if required:
                raise
            print(f'browser arm NOT RUN: missing {module.split(".")[0]}', flush=True)
            return False
    return True


def browser_split(tmp):
    """Exercise the real unit/browser invocations, including a skip mutant."""
    script = ROOT / 'tests/test_pilot_route.py'
    route_markers = ('startup v5 only: info and pilot_wait_metrics PASS',
                     'block_records=1: 50 compared, 2 ambiguous, 12 refused, 0 skipped',
                     'block_records=4096: 50 compared, 2 ambiguous, 12 refused, 0 skipped',
                     'gallery pilot_complete: mock fixture equals real route PASS',
                     'C serializer UINT64_MAX identity: PASS')
    browser_markers = tuple(f'browser {i:02}: wire digits, null/reasons, legacy refusal and leave PASS'
                            for i in (1, 25, 5, 30))

    def invoke(args, env=None):
        return subprocess.run([sys.executable, '-B', *map(str, args)], cwd=ROOT / 'tests',
                              env=env, capture_output=True, text=True, timeout=300)

    def require_browser_output(result):
        assert result.returncode == 0, result.stdout + result.stderr
        assert all(marker in result.stdout for marker in browser_markers), 'browser arm did not run'
        assert 'browser arm NOT RUN' not in result.stdout, result.stdout

    for module in ('websockets', 'playwright'):
        stub = tmp / ('missing-' + module)
        stub.mkdir()
        (stub / (module + '.py')).write_text(
            f'raise ImportError("deliberately unavailable", name="{module}")\n')
        env = {**os.environ, 'PYTHONPATH': str(stub) + os.pathsep + os.environ.get('PYTHONPATH', '')}
        result = invoke([script], env)
        assert result.returncode == 0, result.stdout + result.stderr
        assert all(marker in result.stdout for marker in route_markers), result.stdout
        assert result.stdout.count(f'browser arm NOT RUN: missing {module}') == 1, result.stdout
        assert not any(marker in result.stdout for marker in browser_markers), result.stdout
        required = invoke([script, '--require-browser'], env)
        assert required.returncode != 0 and 'deliberately unavailable' in required.stderr, required
        print(f'browser split missing {module}: unit route PASS; NOT RUN reported; required gate RED', flush=True)

    original = script.read_text()
    old = '                browser(build, tmp, data, expected)\n'
    assert original.count(old) == 1
    changed = original.replace(old, '                print("MUTANT_REACHED:unconditional_browser_skip", flush=True)\n')
    compile(changed, str(script), 'exec')
    # Execute the modified script outside the tree with the original __file__
    # so the test still reads the real, unmodified fixtures and source files.
    mutant = tmp / 'browser-skip.py'
    mutant.write_text('import sys\n' +
                      f'sys.path.insert(0, {str(script.parent)!r})\n' +
                      f'exec(compile({changed!r}, {str(script)!r}, "exec"), '
                      f'{{"__file__": {str(script)!r}, "__name__": "__main__"}})\n')
    result = invoke([mutant, '--require-browser'])
    assert result.returncode == 0 and 'MUTANT_REACHED:unconditional_browser_skip' in result.stdout, result
    try:
        require_browser_output(result)
    except AssertionError as exc:
        assert str(exc) == 'browser arm did not run', exc
        print('mutant unconditional_browser_skip: BUILT; REACHED; RED browser arm did not run', flush=True)
    else:
        raise AssertionError('unconditional_browser_skip SURVIVED')

    require_browser_output(invoke([script, '--require-browser']))
    print('browser split dependencies present: required browser 01,25,05,30 PASS', flush=True)




def serializer_identity(build, tmp):
    obj = tmp / 'serializer.o'
    command(build.cc + build.server_flags + ['-c', ROOT / 'tests/pilot_serializer_probe.c', '-o', obj])
    binary = tmp / 'serializer'
    objects = [v for s, v in zip(build.sources, build.objects) if s != ROOT / 'src/server.c']
    command(build.cc + build.flags + [obj] + objects + build.libs + ['-o', binary])
    print(command([binary]).stdout.strip(), flush=True)


def browser(build, tmp, data, expected):
    """Real Events view, Transport and Chromium; pilot messages hit pgwt-server.
    Legacy UI responses use the established mock. top_events deliberately refuses
    so the panel's actual lifecycle mount is exercised beside an unavailable pane.
    """
    import asyncio
    import functools
    import http.server
    import threading
    import urllib.parse
    import websockets
    from playwright.sync_api import sync_playwright
    import mock_server

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *_): pass
    http = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        functools.partial(Quiet, directory=str(ROOT / 'web/static')))
    http_thread = threading.Thread(target=http.serve_forever, daemon=True)
    http_thread.start()
    active = {'directory': tmp / '1-1', 'requests': []}
    loop = asyncio.new_event_loop()
    ready = threading.Event()
    ws_state = {}

    async def handler(socket):
        async for raw in socket:
            request = json.loads(raw)
            if request['cmd'] == 'pilot_wait_metrics':
                active['requests'].append(copy.deepcopy(request))
                # Test request id is normalized only for the route helper assertion.
                original_id = request['id']; request['id'] = 7
                _, response = route(build.server, active['directory'], request)
                response['id'] = original_id
            elif request['cmd'] == 'top_events':
                response = {'id': request['id'], 'error': 'Legacy Events unavailable for v5'}
            else:
                response = mock_server.handle_request(request)
            await socket.send(json.dumps(response))

    async def start():
        ws_state['server'] = await websockets.serve(handler, '127.0.0.1', 0)
        ws_state['port'] = ws_state['server'].sockets[0].getsockname()[1]
        ready.set()
    def serve():
        asyncio.set_event_loop(loop)
        loop.run_until_complete(start())
        loop.run_forever()
    ws_thread = threading.Thread(target=serve, daemon=True)
    ws_thread.start()
    assert ready.wait(10), 'browser websocket did not start'
    try:
        with sync_playwright() as pw:
            chromium = pw.chromium.launch(headless=True)
            page = chromium.new_page(viewport={'width': 1440, 'height': 1100})
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            for i in (1, 25, 5, 30):
                active['directory'] = tmp / ('v4' if i == 30 else f'1-{i}')
                req = wire_request(data[i][1])
                params = {'ws': f'ws://127.0.0.1:{ws_state["port"]}/ws', 'pilot': '1'}
                params.update({'pilot_' + k: '' if v is None else str(v) for k, v in req['target'].items()})
                params.update({'pilot_' + k: str(v) for k, v in req['query'].items()})
                url = f'http://127.0.0.1:{http.server_port}/?' + urllib.parse.urlencode(params)
                page.goto(url)
                page.locator('[data-tab="events"]').click()
                panel = page.locator('#events-pilot')
                panel.locator('[data-metric="wait_time"] [data-value]').wait_for()
                for metric in METRICS:
                    cell = panel.locator(f'[data-metric="{metric}"]')
                    frozen = expected[i][metric]
                    assert cell.locator('[data-value]').inner_text() == ('unavailable' if frozen['value'] is None else str(frozen['value']))
                    assert cell.locator('[data-completeness]').inner_text() == frozen['completeness']
                    assert cell.locator('[data-reason-token]').all_inner_texts() == frozen['reasons']
                    label = 'Wait time' if metric == 'wait_time' else 'Completed waits'
                    assert cell.locator('h3').inner_text() == (label + ': ' + (
                        'unavailable' if frozen['value'] is None else str(frozen['value']) + ' ' + frozen['unit']))
                    if frozen['value'] is None:
                        assert cell.locator('.pilot-diagnostics h4').inner_text() == 'Recorded so far (incomplete, not a result)'
                        assert cell.locator('[data-unit]').count() == 0
                    for key, value in frozen['diagnostic'].items():
                        assert cell.locator(f'[data-diagnostic="{key}"]').inner_text() == ('unavailable' if value is None else str(value))
                if i in (1, 5):
                    fixture_data = next(cell for cell in mock_server.PILOT_FIXTURES if cell['scenario'].startswith(f'{i:02}_'))
                    rendered = panel.locator('.pilot-result').inner_text()
                    assert [line for line in rendered.splitlines() if line] == fixture_data['rendered_result_lines'], repr(rendered)
                    print(f'DOM {fixture_data["name"]}: {json.dumps(rendered)}', flush=True)
                    # Each diagnostic column is aligned by the browser's grid layout.
                    for metric in METRICS:
                        values = panel.locator(f'[data-metric="{metric}"] dd').evaluate_all(
                            'cells => cells.map(cell => cell.getBoundingClientRect().right)')
                        assert len(set(values)) == 1, values
                if i in (5, 30):
                    sentence = ('Some required exact evidence is missing.' if i == 5 else
                                'The evidence is invalid, so this result cannot be computed.')
                    assert panel.locator('[data-reason-sentence]').all_inner_texts() == [sentence, sentence]
                sent = active['requests'][-1]
                assert sent['target'] == req['target'] and sent['query'] == req['query']
                assert not errors, errors
                page.locator('[data-tab="sessions"]').click()
                panel.wait_for(state='detached')
                print(f'browser {i:02}: wire digits, null/reasons, legacy refusal and leave PASS', flush=True)
            # Feature gate leaves the existing page free of a pilot panel.
            page.goto(f'http://127.0.0.1:{http.server_port}/?ws=ws://127.0.0.1:{ws_state["port"]}/ws')
            page.locator('[data-tab="events"]').click()
            assert page.locator('#events-pilot').count() == 0
            chromium.close()
    finally:
        async def stop():
            ws_state['server'].close()
            await ws_state['server'].wait_closed()
        asyncio.run_coroutine_threadsafe(stop(), loop).result(timeout=10)
        loop.call_soon_threadsafe(loop.stop)
        ws_thread.join(timeout=10)
        loop.close()
        http.shutdown(); http.server_close(); http_thread.join(timeout=10)




def mutants(build, tmp, data, expected):
    server_source = (ROOT / 'src/server.c').read_text()
    query_source = (ROOT / 'src/core/evidence_query.c').read_text()
    before_buffers = '    pgwt_evidence_buffer buffers[PGWT_PILOT_MAX_FILES];'
    mutations = [
        ('from_plus_one', 'src/server.c', server_source, before_buffers,
         '    ++q.from;\n' + before_buffers, (25, 3), False),
        ('ignore_window', 'src/server.c', server_source, before_buffers,
         '    q.from=0; q.to=UINT64_MAX;\n' + before_buffers, (3, 4, 17), False),
        ('null_to_zero', 'src/server.c', server_source,
         'else cJSON_AddNullToObject(obj,key);', 'else cjson_add_uint64_string(obj,key,0);', (5,), False),
        ('ignore_pid_fork', 'src/server.c', server_source, before_buffers,
         '    t.pid=101; t.has_fork=true; t.fork=0;\n' + before_buffers, (2, 14, 24), False),
        ('filter_before_replay', 'src/core/evidence_query.c', query_source,
         '    pgwt_evidence_input input =',
         '''    for (size_t i=0;i<decoded.file_count;++i) {
        pgwt_evidence_file *f=&decoded.files[i];
        pgwt_evidence_record *records=(pgwt_evidence_record *)f->records;
        size_t kept=0;
        for (size_t j=0;j<f->record_count;++j)
            if (records[j].t>=query->from && records[j].t<query->to) records[kept++]=records[j];
        f->record_count=kept;
    }
    pgwt_evidence_input input =''', (31,), False),
        ('parse_via_double', 'src/server.c', server_source,
         '    *out=v;', '    cJSON *numeric=cJSON_Parse(item->valuestring);\n    v=(uint64_t)numeric->valuedouble;\n    cJSON_Delete(numeric);\n    *out=v;', (1, 3), True),
    ]
    # One object cache; every variant must compile/link and reach its injection.
    for name, source, original, old, new, ids, shifted in mutations:
        assert original.count(old) == 1, (name, 'mutation not applied exactly once')
        marker = f'MUTANT_REACHED:{name}'
        # Reach marker belongs to the same function as the injected line.
        if old.startswith('else '):
            marked = 'else { fprintf(stderr,"' + marker + '\\n"); ' + new[5:] + ' }'
        else:
            marked = '    fprintf(stderr,"' + marker + '\\n");\n' + new
        changed = original.replace(old, marked)
        if source.endswith('evidence_query.c'): changed = '#include <stdio.h>\n' + changed
        binary = build.link(name, {source: changed})
        reached = []
        killed = []
        for i in ids:
            case = shift(data[i][1]) if shifted else data[i][1]
            path = tmp / f'{name}-{i}.json'
            path.write_text(json.dumps(case))
            directory = tmp / f'{name}-{i}'
            assert fixture(build, path, directory, 1).returncode == 0
            request = wire_request(case)
            _, control = route(build.server, directory, request)
            compare(control, expected[i], request)
            _, response = route(binary, directory, request)
            assert marker in LAST_STDERR, (name, 'did not reach intended function')
            reached.append(f'{i:02}')
            try:
                compare(response, expected[i], request)
            except AssertionError:
                killed.append(f'{i:02}')
        assert killed, (name, 'SURVIVED', reached)
        print(f'mutant {name}: BUILT; REACHED {",".join(reached)}; RED {",".join(killed)}', flush=True)
    # Browser-side lossy parsing is independently killed by both the input and
    # output UINT64_MAX identity assertions from the committed node test.
    root = tmp / 'node-mutant'
    # The whole module tree, not a list of files: the builder's import graph
    # grows (panels.js, fidelity.js, control.js) and a missing module would
    # fail the run before the mutant line is reached.
    shutil.copytree(ROOT / 'web/static/lib', root / 'web/static/lib')
    for relative in ('tests/web_unit/pilot-evidence.test.mjs', 'tests/fixtures/pilot-responses.json'):
        dest = root / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text((ROOT / relative).read_text())
    (root / 'package.json').write_text('{"type":"module"}')
    builder = root / 'web/static/lib/builders/pilot-evidence.js'
    original = builder.read_text()
    assert original.count('    return value;') == 1
    builder.write_text(original.replace('    return value;', '    console.error("MUTANT_REACHED:parse_via_Number");\n    return Number(value).toString();'))
    compiled = subprocess.run(['node', '--check', str(builder)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run(['node', '--test', str(root / 'tests/web_unit/pilot-evidence.test.mjs')],
                            capture_output=True, text=True)
    assert result.returncode == 1 and 'MUTANT_REACHED:parse_via_Number' in result.stdout + result.stderr
    assert 'pilot request preserves uint64 identity' in result.stdout and 'pilot displays UINT64_MAX' in result.stdout
    print('mutant parse_via_Number: BUILT; REACHED node input/output; RED uint64 identity', flush=True)


if __name__ == '__main__':
    main()
