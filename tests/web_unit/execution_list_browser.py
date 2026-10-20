"""Real-shell execution-list clipping checks at snapshot and demo viewports."""
import json
from pathlib import Path


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
    source = (Path(__file__).resolve().parents[2] /
              'web/static/lib/builders/waterfall.js').read_text()
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
            page.goto(url + '#tab=waterfall&live=1&span=900')
            page.wait_for_selector('#status.connected')
            page.wait_for_selector('#waterfall-chart canvas')
            page.wait_for_function("document.querySelector('.execution-list').style.maxHeight !== ''")
            page.wait_for_timeout(300)
            results[name] = execution_list_metrics(page)
            assert results[name]['viewport'] == [width, height, dpr], results[name]
            # Once the document fits, its min-height must not turn blank
            # viewport space into a growing trailing-space reservation.
            for _ in range(3):
                page.evaluate("window.dispatchEvent(new Event('resize'))")
                page.wait_for_timeout(100)
            resized = execution_list_metrics(page)
            for key in ['list_height', 'full_rows', 'readout_bottom', 'scroll_height', 'chart_height']:
                if resized[key] != results[name][key]:
                    failures[name + '_resize'] = (
                        f'geometry changed after resize: {key}; ' + json.dumps(resized))
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
    else:
        failures['negative_control'] = '60px floor survived the visibility assertion'
    results['failures'] = failures
    results['ok'] = not failures
    return results
