/* The backend-activity UI, rendered against the shape the SERVER actually
 * sends (integration finding, release-integration-2).
 *
 * #455 was built as two halves briefed from the issue text, not from each
 * other. src/backend_span.c emits a Queries row's span as
 * `backend_activity: {source, span, ...}`, ns boundaries as JSON STRINGS,
 * contained executions as `exec_starts`, multi-statement as
 * `multi_exec`/`multi_query`. The UI half read `backend_activity_span`,
 * `start_observed`, `executions_count`, `multi_statement` -- four misses and
 * one envelope too shallow. Every one of its own unit tests stayed green,
 * because they fed it its own placeholder shape; the real Queries cell drew
 * the literal string "Not measured" over a measured, closed, 420 ms span.
 *
 * So these cases feed the REAL shape, taken from tests/mock_server.py (which
 * tests/test_protocol_drift.py pins against the real pgwt-server) and kept
 * equal to it by tests/test_mock_backend_activity_fixture.py.
 *
 * The fixture is checked for CONTENT first: a fixture with no measured span
 * would make every assertion below vacuously satisfiable, which is the way
 * this gate would go blind a second time.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
    backendActivityHtml, backendSpanEnvelope, normalizeBackendSpan,
    backendActivityDrillModel,
} from '../../web/static/lib/builders/backend-span.js';
import { buildQueriesBackendActivityHtml } from '../../web/static/views/queries.js';
import { makeQueriesConfig } from '../../web/static/lib/builders/table-configs.js';

const WIRE = JSON.parse(readFileSync(new URL(
    './fixtures/mock_backend_activity.json', import.meta.url), 'utf8'));

test('the fixture carries a MEASURED row span -- without it every case '
    + 'below would pass on an empty file', () => {
    const span = WIRE.measured_row.backend_activity.span;
    assert.equal(span.measurement, 'measured');
    assert.equal(span.completion, 'closed');
    assert.equal(span.completed_duration_ms, 420);
    // The four field names that actually differ from the UI half's shape.
    assert.equal(typeof span.start_ns, 'string');
    assert.equal(typeof span.exec_starts, 'number');
    assert.equal(typeof span.multi_exec, 'boolean');
    assert.ok(!Object.hasOwn(WIRE.measured_row, 'backend_activity_span'));
    assert.ok(!Object.hasOwn(span, 'start_observed'));
    assert.ok(!Object.hasOwn(span, 'executions_count'));
});

test('Queries cell: a measured, closed span renders its duration -- NOT the '
    + 'bare string "Not measured"', () => {
    const html = backendActivityHtml(WIRE.measured_row);
    assert.match(html, /Closed · 420\.0ms/);
    assert.match(html, /Measurement: Measured · Backend activity boundary: Closed/);
    assert.notEqual(html, 'Not measured');
});

test('Queries cell: an UNMEASURED row still reads Not measured with the '
    + "server's reason -- the adapter never invents a measurement", () => {
    const html = backendActivityHtml(WIRE.unmeasured_row);
    assert.match(html, /Not measured/);
    assert.ok(!/Closed · /.test(html));
    const span = (WIRE.unmeasured_row.backend_activity || {}).span;
    if (span && typeof span.reason === 'string') {
        // The reason lives on the span for this shape; the drill shows it.
        assert.equal(span.measurement, 'unmeasured');
    }
});

test('Queries cell: a row with NO backend-activity information at all is '
    + 'absent, not measured -- present === false', () => {
    const env = backendSpanEnvelope({ query_id: '1', total_ms: 10 });
    assert.equal(env.present, false);
    assert.equal(env.span, null);
    assert.equal(backendActivityHtml({ query_id: '1' }), 'Not measured');
});

test('drill model: the real boundaries and exec count reach the reader', () => {
    const model = backendActivityDrillModel(WIRE.measured_row.backend_activity.span);
    assert.equal(model.pid, 1001);
    assert.equal(model.startText, 'start observed');
    assert.equal(model.endText, 'end observed');
    assert.equal(model.executionsCount, 1);
    assert.match(model.executionsText, /1 observed execution/);
});

test('normalizeBackendSpan: a null boundary stays unobserved, and nothing '
    + 'is invented for a field the server did not send', () => {
    const unmeasured = normalizeBackendSpan(
        { start_ns: null, end_ns: null, measurement: 'unmeasured' });
    assert.equal(unmeasured.start_observed, false);
    assert.equal(unmeasured.end_observed, false);
    assert.equal(unmeasured.executions_count, undefined);
    assert.equal(unmeasured.multi_statement, undefined);
    assert.equal(normalizeBackendSpan(null), null);
    // An explicit boolean from the placeholder shape is NOT overwritten.
    assert.equal(normalizeBackendSpan(
        { start_observed: true, start_ns: null }).start_observed, true);
});

test('Queries panel: the measured row produces a span entry, not an empty '
    + 'list under a populated header', () => {
    const html = buildQueriesBackendActivityHtml({
        ...WIRE.top_queries_root,
        rows: [WIRE.measured_row, WIRE.unmeasured_row],
    });
    assert.match(html, /backend-activity-panel/);
    assert.match(html, /Closed · 420\.0ms/);
    assert.match(html, /1 observed execution/);
    // "Not measured" as the WHOLE panel body is the dead-panel symptom.
    assert.ok(!/<p>Not measured<\/p>/.test(html), html.slice(0, 200));
    const entries = html.match(/<details open>/g) || [];
    assert.equal(entries.length, 2, 'one entry per row carrying an envelope');
});

test('Queries panel: span_detail\'s spans[] array renders its entries', () => {
    const html = buildQueriesBackendActivityHtml(WIRE.span_detail);
    assert.ok(WIRE.span_detail.spans.length > 0);
    assert.equal((html.match(/<details open>/g) || []).length,
        WIRE.span_detail.spans.length);
    assert.match(html, /Closed · 420\.0ms/);
});

test('Waterfall: execution_detail\'s own envelope renders with its scope', () => {
    const html = backendActivityHtml(WIRE.execution_detail, true);
    assert.match(html, /Closed · 420\.0ms/);
    assert.match(html, /1 observed execution/);
    assert.match(html, /backend-activity-scope/);
});

test('envelope: a stated absence is rendered as such, with its reason '
    + 'escaped -- absence is never silence', () => {
    const html = backendActivityHtml({
        backend_activity: null,
        backend_activity_unavailable_reason: 'no span <observed>',
    });
    assert.match(html, /Not measured/);
    assert.match(html, /no span &lt;observed&gt;/);
    assert.equal(backendSpanEnvelope({ backend_activity: null }).present, true);
});

/* ── Reviewer findings on this branch ───────────────────────────────────────
 * The same PR introduced the SAME concept on two surfaces with two different
 * default states: Waterfall as one collapsed <details> (+17 px) and Queries
 * as one force-expanded <details open> PER ENTRY (+493 px on the
 * table_queries gallery cell), with no way to collapse short of clicking
 * every summary. And the cell's `Closed · 0.4 s` sat beside `DB Time 4.2 s`
 * with nothing visible saying one is an OBSERVED WALL-CLOCK SPAN and the
 * other a SUMMED wait/CPU total -- the #455 gap traded for a new ambiguity.
 */
test('reviewer fix 1: the Queries panel is ONE collapsed section, the same '
    + 'default state Waterfall uses for the identical concept', () => {
    const html = buildQueriesBackendActivityHtml({
        ...WIRE.top_queries_root,
        rows: [WIRE.measured_row, WIRE.unmeasured_row],
    });
    // One container, collapsed: no `open` on the OUTER element.
    assert.match(html, /^<details class="backend-activity-panel">/);
    assert.doesNotMatch(html, /^<details[^>]*\sopen/);
    assert.match(html, /<summary>Backend activity/);
    // The information is one click away, not two: the per-entry details stay
    // expanded INSIDE the collapsed section.
    assert.equal((html.match(/<details open>/g) || []).length, 2);
    assert.match(html, /Closed · 420\.0ms/);
});

test('Queries selected span is visible in its identity cell, without a standalone column', () => {
    const cols = makeQueriesConfig('exact').columns;
    assert.ok(!cols.some(c => c.key === 'backend_activity_span'));
    const html = cols.find(c => c.key === 'text').format(WIRE.measured_row);
    assert.match(html, /Longest measured closed backend span: 420\.0ms/);
    assert.match(html, /PID 1001/);
    assert.doesNotMatch(html, /Measurement:|Completion:/);
});
