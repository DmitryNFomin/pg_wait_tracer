/* #351: the degraded-transport overlay must show the upstream pgwt-server's
 * own startup-refusal reason, not just the generic "server disconnected" /
 * "server unavailable (reconnecting)" message it showed before. The bridge
 * forwards that reason as TransportError.reason (web/refusal.go +
 * lib/transport.js) only when one was established; buildDegradedDetail /
 * degradedDetailHtml (lib/panels.js) decide the rendered shape, pure and
 * Node-testable -- app.js only writes the resulting HTML into the DOM.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildDegradedDetail, degradedDetailHtml } from '../../web/static/lib/panels.js';

test('#351 reason only (no upstream cause): one line, unchanged from before this field existed', () => {
    const model = buildDegradedDetail('server disconnected', null);
    assert.deepEqual(model, { reasonText: 'server disconnected', causeText: '' });
    const html = degradedDetailHtml(model);
    assert.equal(html, 'server disconnected');
    assert.ok(!html.includes('Why:'), html);
});

test('#351 reason + upstream cause: BOTH render, the cause on its own line', () => {
    const model = buildDegradedDetail('server disconnected',
        'pgwt-server: no trace files found in /data/traces');
    assert.deepEqual(model, {
        reasonText: 'server disconnected',
        causeText: 'pgwt-server: no trace files found in /data/traces',
    });
    const html = degradedDetailHtml(model);
    assert.ok(html.includes('server disconnected'), html);
    assert.ok(html.includes('Why: pgwt-server: no trace files found in /data/traces'), html);
    assert.ok(html.includes('<br>'), html);   // genuinely a second line, not concatenated
});

test('#351 a whitespace-only cause is treated as no cause (never a blank "Why:" line)', () => {
    const model = buildDegradedDetail('server disconnected', '   ');
    assert.equal(model.causeText, '');
    assert.ok(!degradedDetailHtml(model).includes('Why:'));
});

test('#351 null/undefined reason does not crash and renders no leading text', () => {
    assert.deepEqual(buildDegradedDetail(null, null), { reasonText: '', causeText: '' });
    assert.deepEqual(buildDegradedDetail(undefined, undefined), { reasonText: '', causeText: '' });
    assert.equal(degradedDetailHtml(buildDegradedDetail(null, null)), '');
});

test('#351 a cause is HTML-escaped -- a hostile upstream line cannot inject markup', () => {
    const model = buildDegradedDetail('server disconnected',
        'pgwt-server: <img src=x onerror=alert(1)> refused');
    const html = degradedDetailHtml(model);
    assert.ok(!html.includes('<img'), html);
    assert.ok(html.includes('&lt;img'), html);
});

test('#351 degradedDetailHtml(null) renders nothing (never throws)', () => {
    assert.equal(degradedDetailHtml(null), '');
});
