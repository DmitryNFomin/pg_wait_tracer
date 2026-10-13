#!/usr/bin/env python3
"""One-off #369 evidence collector; no production source changes.

Linux: python3 tests/experiment_369.py BASELINE CANDIDATE OUTPUT [RESTORED]
Both input directories must be clean exports of the named revisions.
GNU ld wrappers log which compute function the real server calls. A separate,
explicit fault-injection pass redirects raw Sessions compute to summaries to
observe the otherwise guarded contract through the real JSON serializer.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


WRAPPER = r'''
#include "compute.h"
#include <stdio.h>
#include <stdlib.h>
void __real_pgwt_compute_top_sessions_from_summaries(const char *, uint64_t,
    uint64_t, const struct pgwt_filter *, double, struct pgwt_sessions_result *);
void __real_pgwt_compute_top_sessions(const struct pgwt_trace_event *, int,
    const struct pgwt_filter *, uint64_t, uint64_t, double,
    struct pgwt_sessions_result *);
void __wrap_pgwt_compute_top_sessions_from_summaries(const char *dir,
    uint64_t from, uint64_t to, const struct pgwt_filter *f, double wall,
    struct pgwt_sessions_result *out) {
    fprintf(stderr, "CONTRACT_ROUTE summary\n");
    __real_pgwt_compute_top_sessions_from_summaries(dir, from, to, f, wall, out);
    fprintf(stderr, "CONTRACT_RESULT failed=%d num_rows=%d rows_null=%d\n",
        out->failed, out->num_rows, out->rows == NULL);
}
void __wrap_pgwt_compute_top_sessions(const struct pgwt_trace_event *events,
    int count, const struct pgwt_filter *f, uint64_t from, uint64_t to,
    double wall, struct pgwt_sessions_result *out) {
    const char *forced = getenv("PGWT_CONTRACT_FORCE_SUMMARY");
    fprintf(stderr, "CONTRACT_ROUTE %s\n", forced ? "forced-summary" : "raw");
    if (forced)
        __real_pgwt_compute_top_sessions_from_summaries(forced, from, to, f, wall, out);
    else
        __real_pgwt_compute_top_sessions(events, count, f, from, to, wall, out);
    fprintf(stderr, "CONTRACT_RESULT failed=%d num_rows=%d rows_null=%d\n",
        out->failed, out->num_rows, out->rows == NULL);
}
'''

PROBE = r'''
#define main existing_probe_main
#include "test_probe_bounds.c"
#undef main
int main(void) {
    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    a->sessions[0].pid = BASE_PID;
    a->sessions[0].db_time_ns = 5 * MS;
    a->sessions[0].cpu_ns = MS;
    a->num_sessions = 1;
    struct pgwt_summary_accum *recs[] = {a};
    for (int present = 1; present >= 0; present--) {
        stub_set(recs, present);
        for (int kind = 0; kind < 4; kind++) {
            struct pgwt_filter f = {0};
            if (kind == 0) snprintf(f.class_name, sizeof(f.class_name), "Lock");
            if (kind == 1) f.event_id = WAIT_A;
            if (kind == 2) f.event_id = 0;
#ifdef CONTRACT_HAS_EVENT_FLAG
            if (kind == 1 || kind == 2) f.has_event_filter = 1;
#endif
            if (kind == 3) f.query_id = 100;
            struct pgwt_sessions_result out;
            pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &out);
            printf("{\"records\":%d,\"filter\":\"%s\",\"failed\":%d,"
                   "\"num_rows\":%d,\"rows_null\":%s}\n", present,
                   (const char *[]){"class", "event", "cpu", "query"}[kind],
                   out.failed, out.num_rows, out.rows == NULL ? "true" : "false");
            free(out.rows);
        }
    }
    free(a);
    return 0;
}
'''


def run(cmd, cwd=None, env=None):
    p = subprocess.run([str(x) for x in cmd], cwd=cwd, env=env,
                       text=True, capture_output=True, timeout=180)
    if p.returncode:
        raise RuntimeError(f"{cmd}: {p.returncode}\n{p.stdout}\n{p.stderr}")
    return p.stdout


def collect(binary, trace, requests, output, force=False):
    env = dict(os.environ)
    if force:
        env["PGWT_CONTRACT_FORCE_SUMMARY"] = str(trace)
    wire = ''.join(json.dumps(req) + '\n' for req in requests)
    p = subprocess.run([str(binary), str(trace)], input=wire, text=True,
                       capture_output=True, env=env, timeout=30)
    Path(str(output) + '.requests.jsonl').write_text(wire)
    Path(str(output) + '.responses.jsonl').write_text(p.stdout)
    Path(str(output) + '.stderr.log').write_text(p.stderr)
    assert p.returncode == 0, p.stderr
    responses = [json.loads(line) for line in p.stdout.splitlines()]
    assert len(responses) == len(requests), (requests, responses)
    return responses, [line for line in p.stderr.splitlines()
                       if line.startswith('CONTRACT_')]


def main():
    baseline, candidate, output = map(lambda s: Path(s).resolve(), sys.argv[1:4])
    output.mkdir(parents=True, exist_ok=True)
    wrapper = output / 'route_wrapper.c'
    wrapper.write_text(WRAPPER)
    probe = output / 'direct_probe.c'
    probe.write_text(PROBE)
    builds = {}
    revisions = [('master', baseline), ('d231d18', candidate)]
    if len(sys.argv) == 5:
        revisions.append(('restored', Path(sys.argv[4]).resolve()))
    for label, root in revisions:
        print('Building', label, flush=True)
        revision = (root / 'EXPERIMENT_REVISION').read_text().strip()
        buildlog = run(['make', '-j2', 'pgwt-server', f'PGWT_BUILD_VERSION={revision}'], root)
        buildlog += run(['make', '-C', 'tests', '-j2', 'gen_test_traces'], root)
        (output / f'{label}.build.log').write_text(buildlog)
        wrapped = output / f'{label}.instrumented'
        run(['gcc', '-g', '-O2', '-DPGWT_SERVER', '-I', root / 'src',
             wrapper, *sorted((root / 'build').glob('server_*.o')),
             '-Wl,--wrap=pgwt_compute_top_sessions',
             '-Wl,--wrap=pgwt_compute_top_sessions_from_summaries',
             '-llz4', '-lz', '-lm', '-o', wrapped])
        direct = output / f'{label}.direct'
        flags = ['-DCONTRACT_HAS_EVENT_FLAG'] if label != 'master' else []
        compute_deps = [root / f'build/server_{name}.o' for name in
                        ['wait_event', 'idle_rule', 'spawn', 'cJSON']]
        run(['gcc', '-g', '-O2', '-DPGWT_SERVER', *flags, '-I', root / 'src',
             '-I', root / 'tests', probe, root / 'src/compute.c',
             *compute_deps, '-lm', '-o', direct])
        direct_json = run([direct])
        (output / f'{label}.direct.jsonl').write_text(direct_json)
        for line in direct_json.splitlines():
            row = json.loads(line)
            if label == 'd231d18':
                assert (row['failed'], row['num_rows'], row['rows_null']) == (1, -1, True)
            elif label == 'master' and row['filter'] == 'cpu' and row['records']:
                assert (row['failed'], row['num_rows'], row['rows_null']) == (0, 1, False)
            else:
                assert (row['failed'], row['num_rows'], row['rows_null']) == (0, 0, True)
        pinned_binary = output / f'{label}.pinned'
        run(['gcc', '-g', '-O2', '-DPGWT_SERVER', '-I', root / 'src',
             root / 'tests/test_probe_bounds.c', root / 'src/compute.c',
             *compute_deps, '-lm', '-o', pinned_binary])
        pinned = subprocess.run([str(pinned_binary)],
                                text=True, capture_output=True, timeout=30)
        (output / f'{label}.pinned.log').write_text(pinned.stdout + pinned.stderr)
        assert pinned.returncode == (1 if label == 'd231d18' else 0)
        if label == 'd231d18':
            # Reproduce analysis B's green test by changing ONLY its expected
            # contract in a temporary copy. The repository test stays intact.
            source = (root / 'tests/test_probe_bounds.c').read_text()
            old = 'CHECK(!rsf.failed && rsf.num_rows == 0,'
            assert source.count(old) == 1
            changed = output / 'd231d18.changed-expectation.c'
            changed.write_text(source.replace(old,
                'CHECK(rsf.failed && rsf.num_rows == -1 && rsf.rows == NULL,'))
            changed_binary = output / 'd231d18.changed-expectation'
            run(['gcc', '-g', '-O2', '-DPGWT_SERVER', '-I', root / 'src',
                 changed, root / 'src/compute.c', *compute_deps,
                 '-lm', '-o', changed_binary])
            (output / 'd231d18.changed-expectation.log').write_text(run([changed_binary]))
        builds[label] = {'root': root, 'wrapped': wrapped, 'pinned_exit': pinned.returncode,
                         'revision': revision}

    sys.path.insert(0, str(baseline / 'tests'))
    from test_data_summary_honesty import build_full_300s, S
    from test_data_filters import build_scenario
    from test_data_executions import lifecycle_scenario
    from server_harness import IO_DATA_FILE_READ
    scenarios = {'long': build_full_300s()[0], 'work': build_scenario(),
                 'executions': lifecycle_scenario()}
    traces = {}
    for name, scenario in scenarios.items():
        scenario_path = output / f'{name}.scenario.json'
        scenario_path.write_text(json.dumps(scenario, indent=2) + '\n')
        trace = output / f'{name}.trace-dir'
        # The generator archives current.trace on a second invocation. Reuse
        # of an evidence directory must not accumulate duplicate input data.
        if trace.exists():
            shutil.rmtree(trace)
        run([baseline / 'tests/gen_test_traces', '-o', trace, '-s', scenario_path])
        traces[name] = trace

    _, lo, hi = build_full_300s()
    requests = [{'id': 1, 'cmd': 'info'}]
    filters = [{}, {'class': 'IO'}, {'event_id': IO_DATA_FILE_READ},
               {'query_id': '100'}, {'event_id': 0}, {'pid': 1000},
               {'class': 'NotAClass'}]
    names = ['control', 'class', 'event', 'query', 'cpu', 'pid', 'invalid-class']
    for i, f in enumerate(filters, 2):
        requests.append({'id': i, 'cmd': 'top_sessions', 'from': lo - 10*S,
                         'to': hi + 10*S, 'filters': f})
    for i, f in enumerate(filters, len(requests)+1):
        requests.append({'id': i, 'cmd': 'top_sessions', 'from': hi + 1000*S,
                         'to': hi + 1200*S, 'filters': f})
    result = {}
    for label, build in builds.items():
        clean, _ = collect(build['root'] / 'pgwt-server', traces['long'], requests,
                           output / f'{label}.production')
        measured, routes = collect(build['wrapped'], traces['long'], requests,
                                   output / f'{label}.routes')
        assert measured[1:] == clean[1:], 'instrumentation changed Sessions JSON'
        # info.now_ns is the server's real clock, so two processes differ.
        assert {k: v for k, v in measured[0].items() if k != 'now_ns'} == {
            k: v for k, v in clean[0].items() if k != 'now_ns'}
        route_names = [line.split()[1] for line in routes if line.startswith('CONTRACT_ROUTE')]
        expected = ['summary', 'raw', 'raw', 'raw',
                    'summary' if label == 'master' else 'raw', 'raw', 'raw']
        assert route_names == expected * 2, route_names
        assert sum(r['db_time_ms'] for r in clean[2]['rows']) == 150000
        assert sum(r['db_time_ms'] for r in clean[5]['rows']) == (
            300000 if label == 'master' else 150000)
        assert clean[7]['rows'] == []
        forced_requests = [requests[2], requests[3], requests[4]]
        forced, _ = collect(build['wrapped'], traces['long'], forced_requests,
                            output / f'{label}.forced-summary', force=True)
        if label != 'd231d18':
            assert all(r.get('rows') == [] and 'error' not in r for r in forced)
        else:
            assert all(r.get('code') == 'compute_failed' and 'rows' not in r for r in forced)
        workreq = [{'id': 1, 'cmd': 'time_model', 'filters': {}},
                   {'id': 2, 'cmd': 'time_model', 'filters': {'event_id': 0}}]
        work, _ = collect(build['root'] / 'pgwt-server', traces['work'], workreq,
                          output / f'{label}.cpu-work')
        times = [next(row['ms'] for row in r['rows'] if row['name'] == 'DB Time') for r in work]
        assert times == ([21, 21] if label == 'master' else [21, 3]), times
        execreq = [{'id': i+1, 'cmd': cmd, 'filters': f}
                   for i, (cmd, f) in enumerate([
                       ('executions', {}), ('executions', {'event_id': 0}),
                       ('exec_scatter', {'event_id': 0})])]
        executions, _ = collect(build['root'] / 'pgwt-server', traces['executions'],
                                execreq, output / f'{label}.cpu-executions')
        counts = [r['total_count'] for r in executions]
        assert counts == ([3, 3, 3] if label == 'master' else [3, 1, 1]), counts
        result[label] = {'revision': build['revision'],
                         'routes': dict(zip(names, route_names[:7])),
                         'empty_window_routes': dict(zip(names, route_names[7:])),
                         'cpu_work_ms': times, 'execution_counts': counts,
                         'pinned_exit': build['pinned_exit'],
                         'instrumented_json_matches_unmodified': True}
    (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
