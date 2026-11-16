import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { pilotRequest, pilotEvidenceHtml } from '../../web/static/lib/builders/pilot-evidence.js';

const max = '18446744073709551615';
const fields = {capture: 'A', pid: '4294967295', fork: max, event: 'PgSleep', from: '9223372036854775809', to: max};
test('pilot request preserves uint64 identity', () => {
    assert.deepEqual(pilotRequest(fields), {target: {capture: 'A', pid: 4294967295, fork: max},
        query: {event: 'PgSleep', from: fields.from, to: max}});
    assert.equal(pilotRequest({...fields, fork: ''}).target.fork, null);
});
test('pilot rejects non-decimal and overflowing input', () => {
    for (const key of ['fork', 'from', 'to']) for (const value of ['-1', '1.0', '1e3', ' 1', '1 ', '', '18446744073709551616'])
        if (!(key === 'fork' && value === '')) assert.throws(() => pilotRequest({...fields, [key]: value}), key + ':' + value);
    for (const pid of ['0', '4294967296', '-1']) assert.throws(() => pilotRequest({...fields, pid}));
});
test('pilot displays UINT64_MAX wire identity and unavailable explicitly', () => {
    const metric = {value: max, unit: 'ns', completeness: 'complete', reasons: [],
        detail: {closed_ns: max, open_ns: '0'}, diagnostic: {recorded_ns: max, covered_ns: max, required_ns: max}};
    const response = {wait_time: metric, completed_waits: {...metric, unit: 'count'}, storage: {}};
    const html = pilotEvidenceHtml(response);
    assert.ok(html.includes('<output data-value>' + max + '</output>'));
    assert.ok(html.includes('<span data-closed>' + max + '</span>'));
    assert.ok(html.includes('<dd data-diagnostic="recorded_ns">' + max + '</dd>'));
    const unavailable = {...metric, value: null, detail: undefined, completeness: 'unavailable', reasons: ['coverage_incomplete']};
    const absent = pilotEvidenceHtml({...response, wait_time: unavailable});
    assert.ok(absent.includes('Wait time: <output data-value data-completeness>unavailable</output></h3>'));
    assert.ok(absent.includes('coverage_incomplete'));
    assert.throws(() => pilotEvidenceHtml({...response, wait_time: {...metric, value: 1}}));
    assert.ok(pilotEvidenceHtml({error: '<bad>'}).includes('&lt;bad&gt;'));
});

const fixtures = JSON.parse(readFileSync(new URL('../fixtures/pilot-responses.json', import.meta.url)));
test('pilot unavailable headings omit units and group diagnostics as incomplete', () => {
    const html = pilotEvidenceHtml(fixtures[1].response);
    for (const label of ['Wait time', 'Completed waits'])
        assert.ok(html.includes(label + ': <output data-value data-completeness>unavailable</output></h3>'));
    assert.ok(!html.includes('data-unit'));
    assert.equal(html.match(/Recorded so far \(incomplete, not a result\)/g).length, 2);
    assert.ok(html.includes('pilot-diagnostics pilot-incomplete'));
    assert.ok(html.includes('<dd data-diagnostic="recorded_ns">2</dd>'));
    assert.ok(html.includes('<dd data-diagnostic="recorded_count">0</dd>'));
    assert.ok(!html.includes('pilot-diagnostic-hint'));
});
test('pilot complete values keep units, detail and a diagnostic explanation', () => {
    const html = pilotEvidenceHtml(fixtures[0].response);
    assert.ok(html.includes('Wait time: <output data-value>4</output> <span data-unit>ns</span></h3>'));
    assert.ok(html.includes('Completed waits: <output data-value>1</output> <span data-unit>count</span></h3>'));
    assert.ok(html.includes('recorded is matching wait time or closes; covered is certified window time; required is window time within this backend\'s lifetime.'));
    assert.ok(!html.includes('pilot-incomplete'));
});
test('pilot explains each core reason and preserves unknown tokens safely', () => {
    const response = structuredClone(fixtures[1].response);
    const reasons = [
        ['coverage_incomplete', 'Some required exact evidence is missing.'],
        ['evidence_invalid', 'The evidence is invalid, so this result cannot be computed.'],
        ['invalid_query', 'The query has a missing or invalid event or time.'],
        ['invalid_window', 'The window start must be earlier than its end.'],
    ];
    response.wait_time.reasons = reasons.map(([token]) => token).concat(['<unknown>', 'toString']);
    response.wait_time.diagnostic = {recorded_ns: null, covered_ns: null, required_ns: null};
    const html = pilotEvidenceHtml(response);
    for (const [token, sentence] of reasons)
        assert.ok(html.includes('<span data-reason-sentence>' + sentence + '</span> <code data-reason-token>' + token + '</code>'));
    assert.ok(html.includes('<p><code data-reason-token>&lt;unknown&gt;</code></p>'));
    assert.ok(html.includes('<p><code data-reason-token>toString</code></p>'));
    assert.ok(html.includes('<dd data-diagnostic="recorded_ns">unavailable</dd>'));
});
