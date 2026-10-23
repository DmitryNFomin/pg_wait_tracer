"""Real-shell execution-list clipping checks at snapshot and demo viewports."""
import json
import re
from pathlib import Path

from demo_rehearsal_lib import capped_fixture_ok


def waterfall_url(url, hash_filter=''):
    return url + '#tab=waterfall&live=1&span=900' + ('&' + hash_filter if hash_filter else '')


def _asset_source(relative):
    path = Path(__file__).resolve().parents[2] / 'web/static' / relative
    if not path.is_file():
        raise RuntimeError(f'#514 negative-control asset is missing: {path}')
    return path.read_text()


def _wait_for_execution_list(page):
    page.wait_for_selector('#status.connected')
    page.wait_for_selector('#waterfall-chart canvas')
    page.wait_for_function("document.querySelector('.execution-list').style.maxHeight !== ''")


def install_dense_execution_fixture(page):
    """Exercise the real model/mount with a capped response, not DOM-only rows.
    Preserve the current filter and record exactly what the view received.
    """
    if page.locator('#live-btn.active').count():
        page.click('#live-btn')
    page.evaluate("""() => {
        const transport = window.__pgwt.transport;
        const original = transport.request.bind(transport);
        window.__executionFixture = null;
        window.__executionFixtureError = null;
        transport.request = async (channel, cmd, params) => {
            const response = await original(channel, cmd, params);
            if (cmd !== 'executions') return response;
            const first = response.rows[0];
            // #514: cloning an absent row would test no real execution.
            if (!first) {
                window.__executionFixtureError = 'dense fixture requires a nonempty executions response';
                return response;
            }
            const rows = Array.from({ length: 100 }, (_, i) => ({ ...first,
                start_ns: String(BigInt(first.start_ns) + BigInt(i)) }));
            const fixture = { ...response, rows, total_count: 265756, truncated: true };
            window.__executionFixture = fixture;
            return fixture;
        };
    }""")
    page.click('#executions-sort-toggle')
    page.wait_for_function("""window.__executionFixtureError || (window.__executionFixture &&
        document.querySelectorAll('#executions-table tbody tr.clickable').length === 100)""")
    error = page.evaluate('window.__executionFixtureError')
    assert not error, error
    _wait_for_execution_list(page)
    page.wait_for_timeout(300)
    response = page.evaluate('window.__executionFixture')
    rendered = page.locator('#executions-table tbody tr.clickable').count()
    assert capped_fixture_ok(rendered, len(response['rows']), response.get('total_count'),
                             response.get('truncated')), 'invalid capped executions fixture'
    assert rendered == 100 and response['total_count'] == 265756, 'dense fixture was not installed'
    return response


def assert_execution_disclosure(page, response):
    disclosure = page.locator('#executions-disclosure')
    assert disclosure.count() == 1 and disclosure.is_visible(), (
        'execution disclosure is not visible without hover/expansion')
    text = disclosure.inner_text()
    assert text.strip(), 'execution disclosure has no visible text without hover/expansion'
    rendered = page.locator('#executions-table tbody tr.clickable').count()
    numbers = [int(n.replace(',', '')) for n in re.findall(r'[\d,]+', text)]
    assert numbers == [rendered, response['total_count']], (
        f'execution disclosure counts mismatch: text={text!r}, '
        f'rendered_rows={rendered}, response_total={response["total_count"]}')
    assert rendered == len(response['rows']), 'fixture rows did not reach the table'
    geometry = disclosure.evaluate("""el => {
        const b = el.getBoundingClientRect();
        const list = el.closest('.execution-list').getBoundingClientRect();
        return { height: b.height, top: b.top, bottom: b.bottom,
            fully_visible: b.height > 0 && b.top >= Math.max(0, list.top) &&
                b.bottom <= Math.min(innerHeight, list.bottom) };
    }""")
    assert geometry['fully_visible'], 'execution disclosure clipped: ' + json.dumps(geometry)
    outside_tbody = disclosure.evaluate("el => !el.closest('tbody')")
    assert outside_tbody, 'disclosure is inside table body'
    return {'text': text, 'rendered_rows': rendered,
            'response_rows': len(response['rows']), 'truncated': response['truncated'],
            'fixture_installed': response == page.evaluate('window.__executionFixture'),
            'visible': True, 'outside_tbody': outside_tbody,
            'viewport': page.evaluate('[innerWidth, innerHeight, devicePixelRatio]'),
            'response_total': response['total_count'], **geometry}


def assert_scroll_affordance(page, expected):
    metrics = page.locator('.execution-list').evaluate("""el => ({
        scroll_height: el.scrollHeight, client_height: el.clientHeight,
        has_more: el.classList.contains('list-has-more'),
        shadow: getComputedStyle(el).boxShadow,
    })""")
    assert metrics['has_more'] == expected, 'execution scroll affordance mismatch: ' + json.dumps(metrics)
    assert (metrics['scroll_height'] > metrics['client_height']) == expected, metrics
    if expected:
        assert metrics['shadow'] != 'none', 'scroll affordance has no visual treatment'
    return metrics


def run_execution_disclosure_gates(browser, url, *, hash_filter='', include_fitting=True):
    results = {}
    for name, width, height, dpr in [('snapshot', 1280, 800, 1), ('demo', 1710, 981, 2)]:
        ctx = browser.new_context(viewport={'width': width, 'height': height}, device_scale_factor=dpr)
        try:
            page = ctx.new_page()
            page.goto(waterfall_url(url, hash_filter))
            _wait_for_execution_list(page)
            response = install_dense_execution_fixture(page)
            results[name] = assert_execution_disclosure(page, response)
            results[name]['scroll_affordance'] = assert_scroll_affordance(page, True)
            metrics = execution_list_metrics(page)
            assert metrics['viewport'] == [width, height, dpr], metrics
            for _ in range(3):
                page.evaluate("window.dispatchEvent(new Event('resize'))")
                page.wait_for_timeout(100)
            resized = execution_list_metrics(page)
            metrics['stable_after_resize'] = all(
                resized[key] == metrics[key] for key in
                ['list_height', 'full_rows', 'readout_bottom', 'scroll_height', 'chart_height'])
            results[name].update(metrics)
            assert_execution_list(metrics, 1)
            assert metrics['list_height'] >= metrics['required_list_height'] > 0, (
                'execution list below measured minimum: ' + json.dumps(metrics))
            assert metrics['stable_after_resize'], 'execution list geometry changed after resize'
            # Mutate the actual rendered element, then restore it. These
            # controls prove this same assertion rejects both regressions.
            element = page.locator('#executions-disclosure')
            original = element.inner_text()
            failures = {}
            for control in ['disclosure_title_only', 'disclosure_total_as_shown']:
                if control == 'disclosure_title_only':
                    element.evaluate('(el, text) => { el.title = text; el.textContent = ""; }', original)
                else:
                    element.evaluate('(el, total) => { el.textContent = "Showing " + total.toLocaleString() + " of " + total.toLocaleString(); }', response['total_count'])
                try:
                    assert_execution_disclosure(page, response)
                except AssertionError as exc:
                    failures[control] = str(exc)
                else:
                    raise AssertionError('disclosure negative control survived: ' + control)
                finally:
                    element.evaluate('(el, text) => { el.removeAttribute("title"); el.textContent = text; }', original)
            results[name]['negative_control_failures'] = failures
        finally:
            ctx.close()

    # The capped list has a disclosure prefix, so exercise its row assertion
    # separately from the untruncated list's existing floor control.
    source = _asset_source('lib/builders/waterfall.js')
    current = 'const minimum = Math.max(rowHeights[0] ?? EXECUTION_LIST_MIN_PX,'
    assert source.count(current) == 1, 'cannot restore the old execution-list floor'
    reverted = source.replace(current, 'const minimum = Math.max(60,')
    ctx = browser.new_context(viewport={'width': 1280, 'height': 800}, device_scale_factor=1)
    served = []
    try:
        page = ctx.new_page()
        def restore_floor(route):
            served.append(route.request.url)
            route.fulfill(body=reverted, content_type='application/javascript')
        page.route('**/lib/builders/waterfall.js', restore_floor)
        page.goto(waterfall_url(url, hash_filter))
        _wait_for_execution_list(page)
        install_dense_execution_fixture(page)
        metrics = execution_list_metrics(page)
        results['floor_60'] = dict(metrics, served_assets=served)
        assert metrics['viewport'] == [1280, 800, 1], metrics
        assert served, 'negative control did not serve the reverted module'
        try:
            assert_execution_list(metrics, 1)
        except AssertionError as exc:
            results['snapshot']['negative_control_failures']['list_floor_60'] = str(exc)
        else:
            raise AssertionError('60px floor survived the capped-list visibility assertion')
    finally:
        ctx.close()

    if not include_fitting:
        results['ok'] = True
        return results

    # Four real mock rows fit at this tall viewport; tight viewports quite
    # correctly cap even four rows. No fixture interception or row injection.
    ctx = browser.new_context(viewport={'width': 1710, 'height': 1400}, device_scale_factor=2)
    try:
        page = ctx.new_page()
        page.goto(url + '#tab=waterfall&live=1&span=900')
        page.wait_for_selector('#waterfall-chart canvas')
        page.wait_for_timeout(300)
        assert page.locator('#executions-table tbody tr.clickable').count() == 4, 'default mock must render four rows'
        disclosure = page.locator('#executions-disclosure')
        assert disclosure.count() == 1 and not disclosure.is_visible() and not disclosure.inner_text(), (
            'fitting fixture claims truncation')
        results['fitting'] = assert_scroll_affordance(page, False)
    finally:
        ctx.close()
    results['ok'] = True
    return results


def execution_list_metrics(page):
    return page.evaluate("""() => {
        const list = document.querySelector('.execution-list');
        const box = list.getBoundingClientRect();
        // clientHeight excludes the border and any horizontal scrollbar.
        // is_visible() alone would accept rows clipped by overflow:auto.
        const top = Math.max(0, box.top + list.clientTop);
        const bottom = Math.min(innerHeight, box.top + list.clientTop + list.clientHeight);
        const left = Math.max(0, box.left + list.clientLeft);
        const right = Math.min(innerWidth, box.left + list.clientLeft + list.clientWidth);
        const rect = el => {
            const b = el.getBoundingClientRect();
            return { top: b.top, bottom: b.bottom, height: b.height,
                full: b.width > 0 && b.height > 0 && b.top >= top &&
                    b.bottom <= bottom && b.left >= left && b.right <= right };
        };
        const header = rect(list.querySelector('thead'));
        const rows = [...list.querySelectorAll('tbody tr.clickable')].map(rect);
        const borderBottom = parseFloat(getComputedStyle(list).borderBottomWidth) || 0;
        const pane = document.querySelector('.waterfall-pane').getBoundingClientRect();
        return { viewport: [innerWidth, innerHeight, devicePixelRatio],
            list_top: box.top, list_height: box.height,
            list_max_height: list.style.maxHeight, clip_bottom: bottom,
            pane_height: pane.height,
            chart_height: document.querySelector('#waterfall-chart').getBoundingClientRect().height,
            trailing_below_pane: list.parentElement.getBoundingClientRect().bottom - pane.bottom,
            list_scroll_top: list.scrollTop, scroll_y: scrollY,
            scroll_height: document.documentElement.scrollHeight,
            readout_bottom: document.querySelector('#waterfall-readout').getBoundingClientRect().bottom,
            prefix_height: header.top - box.top, header, rows,
            header_plus_row_height: header.height + (rows[0]?.height || 0),
            required_list_height: rows.length ? Math.ceil(rows[0].bottom - box.top + borderBottom) : null,
            full_rows: rows.filter(row => row.full).length };
    }""")


def assert_execution_list(metrics, minimum_rows):
    assert metrics['scroll_y'] == 0 and metrics['list_scroll_top'] == 0, (
        'execution list was scrolled: ' + json.dumps(metrics))
    assert metrics['header']['full'] and metrics['full_rows'] >= minimum_rows, (
        f'execution list clipped: header_full={metrics["header"]["full"]}, '
        f'full_rows={metrics["full_rows"]}, required_rows={minimum_rows}; '
        + json.dumps(metrics))


def run_execution_list_gates(browser, url):
    """Use the snapshot's unfiltered Waterfall, retaining the PID-1000 fold
    probe separately in verify_waterfall_fold.py. Revert only the floor in
    the served module for the negative control, never in the working tree.
    """
    source = _asset_source('lib/builders/waterfall.js')
    current = 'const minimum = Math.max(rowHeights[0] ?? EXECUTION_LIST_MIN_PX,'
    assert source.count(current) == 1, 'cannot restore the old execution-list floor'
    reverted = source.replace(current, 'const minimum = Math.max(60,')
    results = {}
    failures = {}
    for name, width, height, dpr in [('snapshot', 1280, 800, 1),
                                     ('demo', 1710, 981, 2),
                                     ('floor_60', 1280, 800, 1),
                                     ('demo_floor_60', 1710, 981, 2)]:
        ctx = browser.new_context(viewport={'width': width, 'height': height},
                                  device_scale_factor=dpr)
        served = []
        try:
            page = ctx.new_page()
            if name.endswith('floor_60'):
                def restore_floor(route):
                    served.append(route.request.url)
                    route.fulfill(body=reverted, content_type='application/javascript')
                page.route('**/lib/builders/waterfall.js', restore_floor)
            page.goto(waterfall_url(url))
            _wait_for_execution_list(page)
            page.wait_for_timeout(300)
            results[name] = execution_list_metrics(page)
            assert results[name]['viewport'] == [width, height, dpr], results[name]
            # Once the document fits, its min-height must not turn blank
            # viewport space into a growing trailing-space reservation.
            for _ in range(3):
                page.evaluate("window.dispatchEvent(new Event('resize'))")
                page.wait_for_timeout(100)
            resized = execution_list_metrics(page)
            results[name]['stable_after_resize'] = True
            for key in ['list_height', 'full_rows', 'readout_bottom', 'scroll_height', 'chart_height']:
                if resized[key] != results[name][key]:
                    failures[name + '_resize'] = (
                        f'geometry changed after resize: {key}; ' + json.dumps(resized))
                    results[name]['stable_after_resize'] = False
            if name.endswith('floor_60'):
                assert served, 'negative control did not serve the reverted module'
        finally:
            ctx.close()

    for name, minimum_rows in [('snapshot', 1), ('demo', 2)]:
        try:
            assert_execution_list(results[name], minimum_rows)
            assert results[name]['list_height'] >= results[name]['required_list_height'], (
                'execution list below measured minimum: ' + json.dumps(results[name]))
        except AssertionError as exc:
            failures[name] = str(exc)
    demo = results['demo']
    if demo['scroll_height'] != 981 or demo['readout_bottom'] > 981:
        failures['demo_fold'] = f'demo fold changed: {demo}'
    for key in ['list_max_height', 'list_height', 'readout_bottom', 'scroll_height']:
        if demo[key] != results['demo_floor_60'][key]:
            failures['demo_floor_binding'] = f'demo geometry changed: {key}'
    try:
        assert_execution_list(results['floor_60'], 1)
    except AssertionError as exc:
        results['negative_control_failure'] = str(exc)
        results['negative_control_failures'] = {'list_floor_60': str(exc)}
    else:
        failures['negative_control'] = '60px floor survived the visibility assertion'
    results['failures'] = failures
    results['ok'] = not failures
    return results


def chart_metrics(page):
    return page.evaluate("""() => {
        const host = document.querySelector('#waterfall-chart');
        const chart = echarts.getInstanceByDom(host);
        const data = chart.getModel().getSeriesByIndex(0).getData();
        const bars = [];
        for (let i = 0; i < data.count(); i++) {
            const graphic = data.getItemGraphicEl(i);
            if (!graphic || graphic.ignore || graphic.invisible) continue;
            const box = graphic.getBoundingRect();
            if (box.width > 0 && box.height > 0) bars.push({i, box});
        }
        const widest = bars.sort((a, b) => b.box.width - a.box.width)[0];
        const tuple = widest && chart.getOption().series[0].data[widest.i];
        const canvas = host.querySelector('canvas').getBoundingClientRect();
        const point = tuple && chart.convertToPixel({gridIndex: 0},
            [(tuple[0] + tuple[1]) / 2, tuple[2]]);
        return {
            height: host.getBoundingClientRect().height,
            minimum_height: parseFloat(getComputedStyle(host).minHeight),
            canvas_height: canvas.height, rendered_bars: bars.length,
            minimum_bar_height: bars.length ? Math.min(...bars.map(b => b.box.height)) : 0,
            click: point ? {x: canvas.left + point[0], y: canvas.top + point[1],
                name: tuple[3]} : null,
        };
    }""")


def assert_waterfall_fold(result):
    """#462/#514: the drill-down readout must fit as well as the initial one."""
    height = result['viewport_height']
    chart = result['chart']
    for state, scroll_height, scroll_y, readout_bottom in [
            ('before click', result['scroll_height'], result['scroll_y'], result['readout_bottom']),
            ('after click', chart['scroll_height_after_click'], chart['scroll_y_after_click'],
             chart['readout_bottom_after_click'])]:
        assert scroll_height <= height and scroll_y == 0 and readout_bottom <= height, (
            f'Waterfall fold {state}: scroll_height={scroll_height}, scroll_y={scroll_y}, '
            f'readout_bottom={readout_bottom}, viewport_height={height}')


def _measure_waterfall_fold(browser, url, viewport, hash_filter, stylesheet=None):
    width, height, dpr = viewport
    ctx = browser.new_context(viewport={'width': width, 'height': height}, device_scale_factor=dpr)
    served = []
    try:
        page = ctx.new_page()
        if stylesheet is not None:
            def restore_chart_floor(route):
                served.append(route.request.url)
                route.fulfill(body=stylesheet, content_type='text/css')
            page.route('**/style.css', restore_chart_floor)
        driven_url = waterfall_url(url, hash_filter)
        page.goto(driven_url)
        _wait_for_execution_list(page)
        response = install_dense_execution_fixture(page)
        # #514: the chart-floor control isolates fold overflow; a disclosure
        # clipped by that deliberate overflow is not its failure evidence.
        disclosure = None
        if stylesheet is None:
            disclosure = assert_execution_disclosure(page, response)
            disclosure['scroll_affordance'] = assert_scroll_affordance(page, True)
        before = execution_list_metrics(page)
        chart = chart_metrics(page)
        assert before['viewport'] == list(viewport), before
        # #514: keep the resize probe; a transient fit is not fold evidence.
        for _ in range(3):
            page.evaluate("window.dispatchEvent(new Event('resize'))")
            page.wait_for_timeout(100)
        resized = chart_metrics(page)
        after_resize = execution_list_metrics(page)
        chart['stable_after_resize'] = all(
            resized[key] == chart[key] for key in
            ['height', 'canvas_height', 'rendered_bars', 'minimum_bar_height']) and all(
            before[key] == after_resize[key] for key in
            ['list_height', 'full_rows', 'readout_bottom', 'scroll_height', 'chart_height'])
        assert chart['stable_after_resize'], 'Waterfall geometry changed after resize'
        assert (resized['height'] >= resized['minimum_height'] and resized['rendered_bars'] > 0
                and resized['minimum_bar_height'] > 0 and resized['click']), (
            'no rendered Waterfall bar to inspect: ' + json.dumps(resized))
        click = resized['click']
        page.mouse.click(click['x'], click['y'])
        inspected = page.locator('#waterfall-readout .wf-inspect-name')
        assert inspected.count() == 1 and inspected.inner_text() == click['name'], (
            'Waterfall readout does not name the clicked bar: ' + json.dumps(click))
        chart['readout_clickable'] = True
        after = execution_list_metrics(page)
        chart.update(readout_bottom_after_click=after['readout_bottom'],
                     scroll_height_after_click=after['scroll_height'],
                     scroll_y_after_click=after['scroll_y'])
        if stylesheet is not None:
            assert served, 'chart_floor_240 did not serve the reverted stylesheet'
            assert resized['minimum_height'] == 240, 'chart_floor_240 did not restore the chart floor'
        return {'url': driven_url, 'viewport': list(viewport), 'viewport_height': height,
                'fixture_rows': len(response['rows']), 'disclosure': disclosure,
                'readout_bottom': before['readout_bottom'], 'scroll_height': before['scroll_height'],
                'scroll_y': before['scroll_y'], 'execution_list_max_height': before['list_max_height'],
                'above_fold': before['readout_bottom'] <= height,
                'not_scrollable': before['scroll_height'] <= height,
                'scroll_y_zero': before['scroll_y'] == 0, 'chart': chart,
                'served_assets': served}
    finally:
        ctx.close()


def run_waterfall_fold_gate(browser, url, *, viewport=(1710, 981, 2), hash_filter=''):
    """Share the real-shell fold and chart-floor control with rehearsal (#514).

    The mock caller supplies f.pid=1000; the live bridge has no such PID.
    Only a failure of the fold assertion counts as negative-control evidence.
    """
    result = _measure_waterfall_fold(browser, url, viewport, hash_filter)
    failures = {}
    try:
        assert_waterfall_fold(result)
    except AssertionError as exc:
        failures['fold'] = str(exc)
    result['chart']['ok'] = not failures
    source = _asset_source('style.css')
    pattern = (r'@media\s*\(max-height:\s*1000px\)\s*\{\s*'
               r'#waterfall-chart\s*\{\s*min-height:\s*216px;\s*\}\s*\}')
    reverted, count = re.subn(pattern, '', source)
    assert count == 1, 'cannot restore the 240px Waterfall chart floor'
    control = _measure_waterfall_fold(browser, url, viewport, hash_filter, reverted)
    result['chart_floor_240'] = control
    result['negative_control_failures'] = {}
    try:
        assert_waterfall_fold(control)
    except AssertionError as exc:
        if max(control['scroll_height'], control['chart']['scroll_height_after_click']) > viewport[1]:
            result['negative_control_failures']['chart_floor_240'] = str(exc)
        else:
            failures['chart_floor_240'] = 'chart floor control failed without overflowing the document'
    else:
        failures['chart_floor_240'] = '240px chart floor survived the fold assertion'
    result['failures'] = failures
    result['ok'] = not failures
    return result
