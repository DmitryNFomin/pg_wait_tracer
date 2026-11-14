import { test } from 'node:test';
import assert from 'node:assert/strict';
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
    assert.ok(absent.includes('<output data-value>unavailable</output>'));
    assert.ok(absent.includes('coverage_incomplete'));
    assert.throws(() => pilotEvidenceHtml({...response, wait_time: {...metric, value: 1}}));
    assert.ok(pilotEvidenceHtml({error: '<bad>'}).includes('&lt;bad&gt;'));
});
