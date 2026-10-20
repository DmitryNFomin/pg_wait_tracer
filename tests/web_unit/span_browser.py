"""Real-shell gates and live DOM negative controls; no snapshot baselines."""
import json

FIXTURE = r"""() => {
    const transport = window.__pgwt.transport;
    const original = transport.request.bind(transport);
    const start = BigInt(Math.trunc(window.__pgwt.timeRange?.from || Date.now()*1e6)) + 1000000000n;
    const ns = n => String(start + BigInt(n));
    const span = { pid: 661380, measurement: 'measured', completion: 'closed',
        start_ns: ns(0), end_ns: ns(9900000000), completed_duration_ms: 9900,
        observed_elapsed_ms: 9900, exec_starts: 1, exec_ends: 1 };
    const rows = Array.from({ length: 17 }, (_, i) => ({
        query_id: String(3886912043147135675n + BigInt(i)),
        text: 'SELECT account_balance FROM accounts WHERE account_id = $1 /* dense query fixture */',
        total_ms: 719400, pct: 12.3, count: 12000, avg_us: 59950,
        top_wait: 'Lock:relation', events: [{ name: 'Lock:relation', ms: 719400 }],
        exec_count: 18000, plan_count: 18000, avg_exec_ms: .04, p95_exec_ms: .06,
        p99_exec_ms: .08, avg_plan_ms: .02, p95_plan_ms: .03, p99_plan_ms: .05,
        backend_activity: { span: i === 3 ? null : { ...span,
            ...(i === 1 ? { completion: 'in_flight', completed_duration_ms: 555000 } : {}),
            ...(i === 2 ? { measurement: 'partial', completion: 'unknown' } : {}) } },
    }));
    const qid = rows[0].query_id;
    const intended = { pid: 661380, query_id: qid, start_ns: ns(9800000000),
        end_ns: ns(9900000000), duration_ms: 100, plan_ms: .02, n_events: 1, n_workers: 0 };
    const competitor = { ...intended, pid: 777777, query_id: '999', start_ns: ns(100000000), duration_ms: 9800 };
    window.__continuity = { requests: [], intended, competitor, qid };
    transport.request = async (ch, cmd, params) => {
        window.__continuity.requests.push({ cmd, ...params });
        if (cmd === 'top_queries') return { fidelity: 'exact', rows, truncated: false };
        if (cmd === 'executions') return { fidelity: 'exact', rows: [competitor, intended], truncated: false };
        if (cmd === 'execution_detail') {
            const pid = params.filters.pid;
            return { fidelity: 'exact', query_id: pid === 661380 ? qid : '999',
                backend_activity_span: { ...span, pid },
                leader: { pid, events: [{ name: pid === 661380 ? 'CPU*' : 'Lock:advisory',
                    we: pid === 661380 ? 0 : 50331649, start_ns: params.start_ns,
                    dur_ns: '100000000', cpu_ns: null }] }, workers: [], plan: null };
        }
        if (cmd === 'session_timeline') return { fidelity: 'exact', truncated: false,
            events: [{ p: 661380, n: 'Lock:relation', s: Number(ns(1000000)), d: 9699000000, q: '0' },
                     { p: 777777, n: 'Lock:advisory', s: ns(0), d: '9800000000', q: '999' }] };
        return original(ch, cmd, params);
    };
}"""


def visible_queries(page):
    texts = page.locator('#table-container tbody tr.clickable').all_inner_texts()
    expected = ['Longest measured closed backend span: 9.9s',
                'Longest observed in-flight backend span: 9.9s so far',
                'Partial backend span', 'Backend span unavailable: Not measured']
    for i, text in enumerate(expected):
        assert text in texts[i], f'row {i}: missing visible {text!r}: {texts[i]}'
    assert '555.0' not in texts[1] and 'measured closed' not in texts[1], 'in-flight rendered as completed'
    for text in texts[:3]:
        assert 'PID 661380' in text, 'PID missing from visible row'
        assert 'Completion:' not in text and 'Measurement:' not in text, 'repeated detailed axes'
    explanation = page.locator('.queries-scope').inner_text()
    assert 'backend activity, not statement latency' in explanation
    assert 'Completed executor phase only' in explanation
    assert 'planner phase' in explanation


def horizontal_fit(page):
    metrics = page.evaluate(r"""() => {
        const host = document.querySelector('#table-container');
        const table = host.querySelector('table');
        const keys = ['total_ms', 'avg_exec_ms', 'p95_exec_ms', 'p99_exec_ms',
            'avg_plan_ms', 'p95_plan_ms', 'p99_plan_ms', 'count', 'avg_us'];
        const heads = [...table.querySelectorAll('thead th')];
        const clipped = [];
        for (const key of keys) {
            const idx = heads.findIndex(h => h.dataset.sort === key);
            if (idx < 0) { clipped.push(key + ': missing'); continue; }
            for (const el of [heads[idx], ...[...table.tBodies[0].rows].map(r => r.cells[idx])]) {
                const b = el.getBoundingClientRect();
                if (b.x < 0 || b.right > innerWidth || el.scrollWidth > el.clientWidth + 1 || !el.innerText.trim())
                    clipped.push(key + ': ' + el.innerText);
            }
        }
        for (const el of table.querySelectorAll('.query-backend-span')) {
            if (el.scrollWidth > el.clientWidth + 1) clipped.push('span qualifier clipped');
        }
        return { width: innerWidth, height: innerHeight, dpr: devicePixelRatio,
            zoom: visualViewport.scale, rows: table.tBodies[0].rows.length,
            scrollWidth: host.scrollWidth, clientWidth: host.clientWidth, clipped };
    }""")
    assert metrics['rows'] == 17, metrics
    assert (metrics['width'], metrics['height'], metrics['dpr'], metrics['zoom']) == (1710, 981, 2, 1), metrics
    assert metrics['scrollWidth'] <= metrics['clientWidth'] + 1 and not metrics['clipped'], f'horizontal overflow/clipping: {metrics}'
    return metrics


def waterfall_context(page, crop=None):
    selected = page.locator('#executions-table tr.selected-execution').inner_text()
    qid = page.evaluate('window.__continuity.qid')
    assert '661380' in selected and qid in selected, f'wrong selected backend/query: {selected}'
    identity = page.evaluate('window.__pgwt.filters.snapshot()')
    assert str(identity.get('pid')) == '661380' and identity.get('query_id') == qid, identity
    pane = page.locator('.waterfall-pane')
    crop = crop or pane.bounding_box()
    essential = page.locator('#waterfall-context')
    text = essential.inner_text() if essential.count() else ''
    for phrase in ['PID 661380', 'Query ' + qid, 'Containing backend activity:', '9.9s',
                   'Lock:relation', 'Relation wait occurred before execution', 'bars show executor phase only']:
        assert phrase in text, f'missing visible Waterfall context: {phrase}'
    assert 'Lock:advisory' not in text and 'Longest' not in text, text
    assert essential.is_visible(), 'essential summary collapsed'
    boxes = essential.locator('[data-context]').evaluate_all('els => els.map(e => { const b=e.getBoundingClientRect(); return {x:b.x,y:b.y,width:b.width,height:b.height}; })')
    assert boxes, 'no essential context lines'
    for box in boxes:
        assert (box['width'] > 0 and box['height'] > 0 and box['x'] >= crop['x'] and box['y'] >= crop['y']
                and box['x'] + box['width'] <= crop['x'] + crop['width'] + .5
                and box['y'] + box['height'] <= crop['y'] + crop['height'] + .5), f'essential context outside actual Waterfall crop: {box}, crop={crop}'
    reqs = page.evaluate('window.__continuity.requests')
    evidence = [r for r in reqs if r['cmd'] == 'session_timeline'][-1]
    detail = [r for r in reqs if r['cmd'] == 'execution_detail'][-1]
    assert evidence['filters'] == {'pid': 661380}, evidence
    assert int(evidence['from']) < int(detail['start_ns']) < int(evidence['to']), evidence
    return {'text': text, 'crop': crop, 'evidence': evidence}


def rejected(name, assertion, failures):
    try:
        assertion()
    except AssertionError as exc:
        failures[name] = str(exc)
    else:
        raise AssertionError('negative control survived: ' + name)


def run_span_gates(page, url, expect_red=False):
    page.goto(url + '#tab=overview&live=1&span=900&f.pid=661380&f.event_id=50331648')
    page.wait_for_selector('#status.connected')
    page.wait_for_selector('#table-container tbody tr')
    if page.locator('#live-btn.active').count():
        page.click('#live-btn')
    page.evaluate(FIXTURE)
    page.click('.tab[data-tab="queries"]')
    page.wait_for_function("document.querySelectorAll('#table-container tbody tr.clickable').length === 17")
    failures = {}
    # Collect all three REDs independently on the unmodified application.
    baseline_failures = {}
    for name, check in [('wording', visible_queries), ('horizontal_fit', horizontal_fit)]:
        try:
            check(page)
        except AssertionError as exc:
            baseline_failures[name] = str(exc)
    if not baseline_failures:
        row = page.locator('#table-container tbody tr.clickable').first
        original = row.inner_html()
        page.evaluate("document.querySelector('.query-backend-span').innerHTML = 'Closed · 9.9 s'")
        rejected('restore Closed · 9.9 s', lambda: visible_queries(page), failures)
        row.evaluate('(e, html) => e.innerHTML = html', original)
        page.evaluate("const e=document.querySelector('.query-backend-span'); e.title=e.innerText; e.innerHTML=e.innerHTML.replace('Longest', '')")
        rejected('hide longest in title', lambda: visible_queries(page), failures)
        row.evaluate('(e, html) => e.innerHTML = html', original)
        second = page.locator('#table-container tbody tr.clickable').nth(1)
        saved = second.inner_html()
        second.evaluate("e => e.querySelector('.query-backend-span').innerHTML='Longest measured closed backend span: 555.0 s'")
        rejected('in-flight rendered as completed', lambda: visible_queries(page), failures)
        second.evaluate('(e, html) => e.innerHTML=html', saved)
        table = page.locator('#table-container table')
        saved = table.inner_html()
        table.evaluate("""t => { for (const r of t.rows) { const c=r.insertCell(4); c.style.minWidth='340px'; c.innerHTML='Closed · 9.9 s<br>Measurement: Measured<br>Completion: Closed'; } }""")
        rejected('restore wide standalone span and repeated axes', lambda: horizontal_fit(page), failures)
        table.evaluate('(e, html) => e.innerHTML=html', saved)
        fit = horizontal_fit(page)
    else:
        fit = None
    # Restoring innerHTML above removes row listeners, so refresh via tab navigation.
    page.click('.tab[data-tab="overview"]')
    page.click('.tab[data-tab="queries"]')
    page.wait_for_function("document.querySelectorAll('#table-container tbody tr.clickable').length === 17")
    page.locator('.query-executions-action').first.click()
    page.wait_for_selector('#waterfall-chart canvas')
    page.wait_for_timeout(200)
    try:
        context = waterfall_context(page)
    except AssertionError as exc:
        baseline_failures['continuity'] = str(exc)
    if expect_red:
        assert set(baseline_failures) == {'wording', 'horizontal_fit', 'continuity'}, baseline_failures
        return {'expected_red': baseline_failures}
    if baseline_failures:
        raise AssertionError(json.dumps(baseline_failures, indent=2))
    original_hash = page.evaluate('location.hash')
    page.evaluate("""() => {
        const hash = new URLSearchParams(location.hash.slice(1));
        hash.set('f.pid', '777777'); hash.set('f.query_id', '999');
        location.hash = hash.toString();
    }""")
    page.wait_for_function("document.querySelector('#executions-table tr.selected-execution')?.innerText.includes('777777')")
    rejected('select advisory backend', lambda: waterfall_context(page), failures)
    page.evaluate('(hash) => { location.hash = hash; }', original_hash)
    page.wait_for_function("document.querySelector('#executions-table tr.selected-execution')?.innerText.includes('661380')")
    essential = page.locator('#waterfall-context')
    saved = essential.inner_html()
    essential.evaluate("e => e.innerHTML=e.innerHTML.replaceAll('Lock:relation', '')")
    rejected('remove wait name', lambda: waterfall_context(page), failures)
    essential.evaluate('(e, html) => e.innerHTML=html', saved)
    essential.evaluate("e => e.innerHTML='<details><summary>Activity</summary>'+e.innerHTML+'</details>'")
    rejected('collapse essential summary', lambda: waterfall_context(page), failures)
    essential.evaluate('(e, html) => e.innerHTML=html', saved)
    crop = page.locator('#waterfall-chart').bounding_box()
    rejected('crop context out', lambda: waterfall_context(page, crop), failures)
    return {'fit': fit, 'continuity': context, 'negative_controls': failures}
