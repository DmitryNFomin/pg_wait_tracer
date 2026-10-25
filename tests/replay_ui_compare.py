"""Compare painted DOM/chart data with the exact consumed WebSocket replies.

Expected numbers/units are calculated here, independently of UI format/build
functions. This is presentation verification, not an accounting oracle.
"""
from collections import Counter
from datetime import datetime, timezone
import json
import math
from pathlib import Path

import demo_workload_coverage as coverage

# Observe which promises actually resolve; cancelled/unknown replies never
# enter this list. Only wrap resolve, leaving payload, channel and timing intact.
CONSUMED_HOOK = """(() => {
    window.__replayConsumed = [];
    const send = WebSocket.prototype.send;
    WebSocket.prototype.send = function(raw) {
        const msg = JSON.parse(raw);
        const t = window.__pgwt && window.__pgwt.transport;
        const pending = t && t.pending[msg.id];
        if (pending) {
            const resolve = pending.resolve;
            pending.resolve = data => {
                window.__replayConsumed.push({id:data.id, cmd:msg.cmd, channel:pending.channel});
                resolve(data);
            };
        }
        return send.call(this, raw);
    };
})();"""

SNAPSHOT = """tab => {
    const p = window.__pgwt;
    const tables = {};
    for (const host of ['table-container', 'executions-table']) {
        const el = document.getElementById(host);
        if (!el) continue;
        tables[host] = {
            headers: Array.from(el.querySelectorAll('thead th')).map(x => x.textContent.trim().replace(/[▲▼]/g, '').trim()),
            rows: Array.from(el.querySelectorAll('tbody tr')).map(tr =>
                Array.from(tr.querySelectorAll('td')).map(td => td.textContent.trim()))
        };
    }
    const selectors = {histogram:'heatmap-container', timeline:'timeline-chart',
        transitions:'dfg-container', concurrency:'concurrency-chart',
        waterfall:'waterfall-chart', scatter:'scatter-chart', matrix:'matrix-chart'};
    const el = document.getElementById(selectors[tab]);
    const chart = el && echarts.getInstanceByDom(el);
    const option = chart ? chart.getOption() : null;
    const metrics = Array.from(document.querySelectorAll('#summary-bar .metric')).map(el =>
        [el.querySelector('.metric-label').textContent, el.querySelector('.metric-value').textContent]);
    return {tables, option, metrics, consumed:window.__replayConsumed,
        mount:p.viewMount(), live:document.querySelector('#live-btn').classList.contains('active'),
        from:p.timeRange.from, to:p.timeRange.to,
        threshold:document.querySelector('#dfg-slider')?.value,
        hideIdle:document.querySelector('#dfg-hide-idle')?.checked};
}"""


def fixed(value, digits):
    # JS Number.toFixed rounds the binary value, including half ties, upward.
    # Python formatting uses bankers rounding, so use the exact float ratio.
    n, d = float(value).as_integer_ratio()
    sign = '-' if n < 0 else ''
    q, r = divmod(abs(n) * 10**digits, d)
    q += 2 * r >= d
    text = str(q).zfill(digits + 1)
    return sign + (text[:-digits] + '.' + text[-digits:] if digits else text)


def ms(value):
    if value is None:
        return '—'
    if value >= 1000:
        return fixed(value / 1000, 1) + 's'
    if value >= 1:
        return fixed(value, 1) + 'ms'
    if value >= .001:
        return fixed(value * 1000, 0) + 'µs'
    return '<1µs'


def us(value):
    if value is None:
        return '—'
    if value >= 1e6:
        return fixed(value / 1e6, 1) + 's'
    if value >= 1000:
        return fixed(value / 1000, 1) + 'ms'
    return fixed(value, 0) + 'µs'


def count(value):
    if value is None:
        return '—'
    if value >= 1e6:
        return fixed(value / 1e6, 1) + 'M'
    if value >= 1000:
        return fixed(value / 1000, 1) + 'K'
    return str(value)


def pct(value):
    return '—' if value is None else fixed(value, 1) + '%'


def aas(row):
    value = row.get('aas')
    v = '—' if value is None else fixed(value, 2)
    if row.get('pct') is None or (row.get('name') == 'Idle' and row.get('indent') == 0):
        return '—' if value is None else f'— (idle occ. {v})'
    return v


def equal(actual, expected, label):
    def browser_numbers(value):
        if isinstance(value, (int, float)) and abs(value) > 2**53:
            return float(value)
        if isinstance(value, list):
            return [browser_numbers(v) for v in value]
        if isinstance(value, dict):
            return {k: browser_numbers(v) for k, v in value.items()}
        return value
    if browser_numbers(actual) != browser_numbers(expected):
        raise AssertionError(f'{label}: displayed={str(actual)[:220]!r} WS={str(expected)[:220]!r}')


def compare_table(tab, table, response, to_ns):
    """Every returned row and its numeric cells; counts cannot pass vacuously."""
    rows = response.get('rows') or []
    if not rows:
        raise AssertionError(f'{tab}: empty WS rows')
    if tab == 'overview':
        source = rows
        rows = list(rows)
        for i, parent in enumerate(source):
            if parent.get('indent') not in (0, 1):
                continue
            children = []
            for child in source[i + 1:]:
                if child.get('indent') in (0, 1):
                    break
                children.append(child)
            remainder = parent['ms'] - sum(c['ms'] for c in children)
            if children and remainder > .05:
                rows.append(dict(name='… other (not shown)', ms=remainder, pct=None, aas=None))
    actual = table['rows']
    equal(len(actual), len(rows), f'{tab} row count')
    headers = table['headers']
    # Header text is part of the units contract, not inferred by cell position.
    columns = {
        'overview': {'Time': ('ms', ms), '%DB Time': ('pct', pct), 'AAS': (None, aas)},
        'events': {'Count': ('count', count), 'Total': ('total_ms', ms),
            'Avg': ('avg_us', us), 'Max': ('max_us', us), '%DB': ('pct', pct), 'AAS': (None, aas)},
        'sessions': {'Backend': ('type', lambda v: v or 'unknown'),
            'User': ('user', lambda v: v or ''), 'Database': ('db', lambda v: v or ''),
            'Top Wait': ('top_wait', str), 'DB Time': ('db_time_ms', ms), 'CPU%': ('cpu_pct', pct), 'Wait%': ('wait_pct', pct)},
        'queries': {'DB Time': ('total_ms', ms), 'Execs': ('exec_count', count),
            'Plans': ('plan_count', count), 'Avg Exec': ('avg_exec_ms', ms),
            'p95 Exec': ('p95_exec_ms', ms), 'p99 Exec': ('p99_exec_ms', ms),
            'Avg Plan': ('avg_plan_ms', ms), 'p95 Plan': ('p95_plan_ms', ms),
            'p99 Plan': ('p99_plan_ms', ms), '% attributed DB Time': ('pct', pct),
            'Wait/CPU intervals': ('count', count), 'Avg interval': ('avg_us', us)},
        'waterfall': {'Plan': ('plan_ms', ms), 'Leader events': ('n_events', str),
            'Workers': ('n_workers', str)},
    }[tab]
    key = {'overview':'name', 'events':'name', 'sessions':'pid', 'queries':'query_id',
           'waterfall':'start_ns'}[tab]
    # Overview can sort the class groups; bind by identity, never by rank.
    by_identity = {}
    for row in rows:
        identity = str(row[key])
        if identity == 'Other (background)':
            identity = 'Background idle (parked)'
        if tab == 'waterfall':
            value = int(row[key])
            identity = datetime.fromtimestamp(value // 10**9, timezone.utc).strftime('%H:%M:%S')
            identity += '.' + str((value // 10**6) % 1000).zfill(3)
            if row.get('started_before_window'):
                identity = '↤ ' + identity
        by_identity.setdefault(identity, []).append(row)
    checked = 0
    for cells in actual:
        identity = cells[0]
        if not by_identity.get(identity):
            raise AssertionError(f'{tab}: unknown/duplicate displayed row {identity!r}')
        # Waterfall can have identical millisecond starts: disambiguate pid/query.
        candidates = by_identity[identity]
        row = next((r for r in candidates if tab != 'waterfall' or
                    (str(r['pid']) == cells[1] and str(r.get('query_id') or '0') == cells[2])), None)
        if row is None:
            raise AssertionError(f'{tab}: row identity does not match WS: {cells[:3]}')
        candidates.remove(row)
        for label, (field, fmt) in columns.items():
            if label not in headers:
                raise AssertionError(f'{tab}: missing unit/header {label!r}')
            expected = fmt(row.get(field)) if field else fmt(row)
            if tab == 'overview' and label == '%DB Time' and row.get('name') == 'Idle':
                expected = '—'
            equal(cells[headers.index(label)], expected, f'{tab}/{identity}/{label}')
            checked += 1
        if tab == 'queries':
            ratio = ('—' if not row.get('exec_count') or row.get('plan_count') is None else
                     fixed(min(row['plan_count'], row['exec_count']) / row['exec_count'] * 100, 0) + '%')
            equal(cells[headers.index('Plan%')], ratio, f'queries/{identity}/Plan%')
            checked += 1
        if tab == 'events':
            for label, field in [('P50','p50_us'), ('P95','p95_us'), ('P99','p99_us')]:
                value = row.get(field)
                expected = ('—' if value is None else 'Unresolved'
                            if row.get(field.replace('_us','_overflow')) else '≤' + us(value))
                equal(cells[headers.index(label)], expected, f'{tab}/{identity}/{label}')
                checked += 1
        if tab == 'waterfall':
            if row.get('in_progress'):
                elapsed = (int(to_ns) - int(row['start_ns'])) / 1e6
                expected = '≥ ' + ms(elapsed) + ' (running)' if elapsed > 0 else 'In progress'
            else:
                expected = ms(row.get('duration_ms')) + (' *' if row.get('end_inferred') else '')
            equal(cells[headers.index('Duration')], expected, f'waterfall/{identity}/Duration')
            checked += 1
    return checked


def compare_chart(tab, option, data, snapshot, request, capture_from):
    series = option['series'][0]
    points = series.get('data') or []
    if not points:
        raise AssertionError(f'{tab}: no painted points')
    if tab == 'histogram':
        equal([p[:2] + [p[3]] for p in points], data['cells'], 'histogram cells/counts')
        equal(option['yAxis'][0]['data'], data['labels'], 'histogram latency units')
        for p in points:
            if not math.isclose(p[2], math.log1p(p[3]), rel_tol=1e-14):
                raise AssertionError('histogram log count/color mismatch')
    elif tab == 'concurrency':
        equal(points, [None if p['t'] < capture_from else p.get('max', 0)
                       for p in data['peaks']], 'concurrency peak sessions/bucket')
    elif tab == 'scatter':
        expected = [[float(p['t']) / 1e6, p['duration_ms'], p['pid'], p['query_id'], str(p['t'])]
                    for p in data['points'] if p.get('duration_ms') is not None and p['duration_ms'] > 0]
        equal(points, expected, 'scatter execution coordinates/ms/count')
        equal(option['yAxis'][0]['name'], 'Execution duration (ms)', 'scatter units')
    elif tab == 'matrix':
        xs = option['xAxis'][0]['data']
        ys = option['yAxis'][0]['data']
        # Rank/truncation is independently checked, then sum only visible pairs.
        scores = Counter()
        sums = {}
        for link in data['links']:
            scores[link['source']] += link['value']
            scores[link['target']] += link['value']
            key = (link['source'], link['target'])
            n, duration = sums.get(key, (0, 0))
            sums[key] = (n + link['value'], duration + link.get('duration_ms', 0))
        equal(set(xs), set(sorted(scores, key=lambda n: (-scores[n], n))[:20]), 'matrix visible nodes')
        expected = {(a,b):(n,d) for (a,b),(n,d) in sums.items() if a in xs and b in ys}
        actual = {(xs[p[0]],ys[p[1]]):(p[3],p[4]) for p in points}
        equal(actual, expected, 'matrix transition cells/counts/ms')
        for p in points:
            if not math.isclose(p[2], math.log1p(p[3]), rel_tol=1e-14):
                raise AssertionError('matrix log count/color mismatch')
    elif tab == 'transitions':
        links = [l for l in data['links'] if not snapshot['hideIdle'] or
                 (l['source'] != 'Client:ClientRead' and l['target'] != 'Client:ClientRead')]
        threshold = float(snapshot['threshold'] or 0)
        cutoff = max((l['value'] for l in links), default=0) * threshold / 100
        expected = [(l['source'],l['target'],l['value']) for l in links if l['value'] >= cutoff]
        actual = [(l['source'],l['target'],l['value']) for l in series['links']]
        equal(sorted(actual), sorted(expected), 'transitions edge counts')
        visible = {n for a,b,_ in expected for n in (a,b)}
        actual_nodes = {n['name'] for n in points} - {'pgwt-layout-anchor-tl', 'pgwt-layout-anchor-br'}
        equal(actual_nodes, visible, 'transition node identities/count')
        nodes = {n['name']:n for n in data['nodes']}
        for node in points:
            if node['name'] not in nodes:  # two invisible layout anchors
                if node['name'] not in ('pgwt-layout-anchor-tl', 'pgwt-layout-anchor-br'):
                    raise AssertionError('unknown transition node')
                continue
            value = nodes[node['name']]['total_ms']
            equal(node['value'], value, 'transition node ms')
            text = fixed(value / 1000, 1) + 's' if value >= 1000 else fixed(value, 0) + 'ms'
            equal(node['label']['formatter'].split('\n')[-1], text, 'transition node units')
    elif tab == 'waterfall':
        lanes = [data['leader']] + data.get('workers', [])
        raw = []
        for index, lane in enumerate(lanes):
            for e in lane.get('events', []):
                start, duration = int(e['start_ns']), int(e['dur_ns'])
                raw.append([start, start + duration, index, e['name'], e['we'],
                            e['start_ns'],e['dur_ns'],e.get('cpu_ns'),lane['pid'],'event'])
        plan = data.get('plan')
        if plan:
            start, end = int(plan['start_ns']), int(plan['end_ns'])
            raw.append([start,end,0,'Plan phase',None,str(start),str(end-start),None,lanes[0]['pid'],'plan'])
        origin = min([int(request['start_ns'])] + [r[0] for r in raw])
        expected = [[r[0]-origin,r[1]-origin] + r[2:] for r in raw]
        equal([p[:10] for p in points], expected, 'waterfall event lanes/ns/count')
        equal(option['yAxis'][0]['data'], ['PID ' + str(lane['pid']) +
              (' (leader)' if i == 0 else '') for i,lane in enumerate(lanes)], 'waterfall lane PIDs')
    elif tab == 'timeline':
        equal(option['yAxis'][0]['data'], ['PID ' + str(p) for p in data['pids']], 'timeline PIDs')
        # Keep replay narrow enough to use literal spans; refuse an unchecked
        # density projection rather than blessing it on nonempty series alone.
        if len(points[0]) != 8:
            raise AssertionError('timeline aggregation needs a narrower --window-s')
        indexes = {p:i for i,p in enumerate(data['pids'])}
        expected = []
        for e in data['events']:
            start, duration = float(e['s']), float(e['d'])
            end = start + duration
            if end <= request['from'] or e['s'] >= request['to']:
                continue
            expected.append([max(start,request['from']), min(end,request['to']),
                             indexes[e['p']], e['n'],e['c'],e['q'],e['d'],e['s']])
        equal(points, expected, 'timeline spans/ns/count')
    return len(points)


class ReplayProbe:
    def __init__(self, tab, expected_headers, from_ns, to_ns, out, perturb=False):
        self.tab, self.headers = tab, expected_headers
        self.start, self.end, self.out = from_ns, to_ns, Path(out)
        self.perturb = perturb
        self.sent, self.received = {}, {}
        self.connections = 0

    def attach(self, page):
        page.add_init_script(CONSUMED_HOOK)
        page.on('websocket', self._socket)

    def _socket(self, ws):
        self.connections += 1
        # A reconnect invalidates the id namespace and must refuse.
        ws.on('framesent', lambda raw: self._frame('sent', raw))
        ws.on('framereceived', lambda raw: self._frame('received', raw))

    def _frame(self, direction, raw):
        data = json.loads(raw)
        target = self.sent if direction == 'sent' else self.received
        target[data['id']] = data
        with (self.out / self.tab / 'websocket.jsonl').open('a') as stream:
            stream.write(json.dumps(dict(direction=direction, frame=data)) + '\n')

    def _response(self, consumed, cmd):
        matched = [c for c in consumed if c['cmd'] == cmd]
        if not matched:
            raise AssertionError(f'{self.tab}: no consumed WS response for {cmd}')
        ident = matched[-1]['id']
        if ident not in self.received or ident not in self.sent:
            raise AssertionError(f'{self.tab}: consumed response {ident} was not captured')
        data, req = self.received[ident], self.sent[ident]
        if data.get('error') or data.get('unavailable'):
            raise AssertionError(f'{self.tab}: server refused: {data}')
        return data, req

    def assert_owner(self, page):
        consumed = page.evaluate('window.__replayConsumed')
        info, _ = self._response(consumed, 'info')
        equal(info.get('_replay_trace_headers'), self.headers,
              'ownership trace header start_wall_ns')

    def verify(self, page):
        page.wait_for_function("Object.keys(window.__pgwt.transport.pending).length === 0")
        # Two animation frames let ECharts paint after the mount, not a second query.
        page.evaluate('() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))')
        if self.perturb:
            page.locator('#table-container tbody tr').first.locator('td').nth(1).evaluate(
                "el => el.textContent = 'PERTURBED 999s'")
        snapshot = page.evaluate(SNAPSHOT, self.tab)
        (self.out / self.tab / 'display.json').write_text(json.dumps(snapshot, indent=2) + '\n')
        equal(self.connections, 1, 'WebSocket connection count')
        equal(snapshot['live'], False, 'replay Live state')
        equal(snapshot['from'], float(self.start), 'fixed window start')
        equal(snapshot['to'], float(self.end), 'fixed window end')
        equal(snapshot['mount']['id'], self.tab, 'mounted tab')
        consumed = snapshot['consumed']
        info, _ = self._response(consumed, 'info')
        equal(info.get('_replay_trace_headers'), self.headers, 'ownership trace header start_wall_ns')
        cmd, _, checker = coverage.TAB_QUERIES[self.tab]
        data, req = self._response(consumed, cmd)
        equal(req['from'], snapshot['from'], 'consumed request start')
        equal(req['to'], snapshot['to'], 'consumed request end')
        ok, detail = checker(data, info.get('num_cpus')) if self.tab == 'concurrency' else checker(data)
        if not ok:
            raise AssertionError(f'{self.tab}: tab coverage failed: {detail}')
        if self.tab in ('overview','events','sessions','queries','waterfall'):
            host = 'executions-table' if self.tab == 'waterfall' else 'table-container'
            checked = compare_table(self.tab, snapshot['tables'][host], data, self.end)
            if self.tab == 'overview':
                expected_metrics = [['DB Time', ms(data['rows'][0]['ms'])]]
                if data.get('wall_ms'):
                    expected_metrics.append(['Wall', ms(data['wall_ms'])])
                expected_metrics.append(['AAS', fixed(data['rows'][0]['aas'], 2)])
                idle = next((r for r in data['rows'] if r['name'] == 'Idle' and r['indent'] == 0), None)
                if idle:
                    expected_metrics.append(['Idle', ms(idle['ms'])])
                expected_metrics.append(['CPUs', str(info['num_cpus'])])
                equal(snapshot['metrics'], expected_metrics, 'overview summary units/values')
                checked += len(expected_metrics)
            if self.tab == 'waterfall':
                detail_data, detail_req = self._response(consumed, 'execution_detail')
                checked += compare_chart('waterfall', snapshot['option'], detail_data, snapshot, detail_req, 0)
        else:
            checked = compare_chart(self.tab, snapshot['option'], data, snapshot, req, int(info['from_ns']))
        return dict(response_id=data['id'], command=cmd, compared=checked,
                    coverage=detail, ownership='start_wall_ns matches fixture')
