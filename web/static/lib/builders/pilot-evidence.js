/* evidence-v1 consumer: preserve every wire digit; never derive a metric. */
import { esc } from '../format.js';
import { buildCoverageNotice, coverageNoticeHtml } from '../panels.js';

const uint64Max = 18446744073709551615n;
function decimal(value, name) {
    if (typeof value !== 'string' || !/^[0-9]+$/.test(value) || BigInt(value) > uint64Max)
        throw new Error(name + ' must be an unsigned 64-bit decimal integer');
    return value;
}

export function pilotRequest(fields) {
    const pid = decimal(fields.pid, 'PID');
    if (BigInt(pid) < 1n || BigInt(pid) > 4294967295n)
        throw new Error('PID must be between 1 and 4294967295');
    if (!fields.capture || !fields.event) throw new Error('Capture and event are required');
    return {
        target: { capture: fields.capture, pid: JSON.parse(BigInt(pid).toString()),
            fork: fields.fork === '' ? null : decimal(fields.fork, 'Fork') },
        query: { event: fields.event, from: decimal(fields.from, 'From'), to: decimal(fields.to, 'To') },
    };
}

function wire(value) {
    if (value === null) return 'unavailable';
    if (typeof value !== 'string') throw new Error('Pilot response lost decimal string precision');
    return decimal(value, 'Response');
}

const reasonSentences = new Map([
    ['coverage_incomplete', 'Some required exact evidence is missing.'],
    ['evidence_invalid', 'The evidence is invalid, so this result cannot be computed.'],
    ['invalid_query', 'The query has a missing or invalid event or time.'],
    ['invalid_window', 'The window start must be earlier than its end.'],
]);

export function pilotEvidenceHtml(response) {
    if (response.error) return '<p role="alert">' + esc(response.error) + '</p>';
    // U3: the server ANDs its coverage verdict into the evidence; an
    // incomplete one is shown as the coverage label, never as a value.
    const notice = buildCoverageNotice(response);
    if (notice) return coverageNoticeHtml(notice);
    let html = '<div class="pilot-metrics">';
    let hasValue = false;
    for (const key of ['wait_time', 'completed_waits']) {
        const metric = response[key];
        const value = wire(metric.value);
        const unavailable = metric.value === null;
        if (!unavailable) hasValue = true;
        html += '<div class="pilot-metric" data-metric="' + key + '"><h3>' +
            (key === 'wait_time' ? 'Wait time' : 'Completed waits') + ': ' +
            '<output data-value' + (unavailable ? ' data-completeness' : '') + '>' + esc(value) + '</output>' +
            (unavailable ? '' : ' <span data-unit>' + esc(metric.unit) + '</span>') + '</h3>';
        if (!unavailable) html += '<span class="pilot-status" data-completeness>' + esc(metric.completeness) + '</span>';
        html += '<div class="pilot-reasons" data-reasons>';
        for (const reason of metric.reasons) {
            const sentence = reasonSentences.get(reason);
            html += '<p>' + (sentence ? '<span data-reason-sentence>' + esc(sentence) + '</span> ' : '') +
                '<code data-reason-token>' + esc(reason) + '</code></p>';
        }
        html += '</div>';
        if (metric.detail) html += '<p>Closed: <span data-closed>' + esc(wire(metric.detail.closed_ns)) +
            '</span> ns; open: <span data-open>' + esc(wire(metric.detail.open_ns)) + '</span> ns</p>';
        html += '<section class="pilot-diagnostics' + (unavailable ? ' pilot-incomplete' : '') + '"><h4>' +
            (unavailable ? 'Recorded so far (incomplete, not a result)' : 'Diagnostics') + '</h4><dl>';
        for (const [name, number] of Object.entries(metric.diagnostic))
            html += '<dt>' + esc(name) + '</dt><dd data-diagnostic="' + esc(name) + '">' + esc(wire(number)) + '</dd>';
        html += '</dl></section></div>';
    }
    html += '</div>';
    if (hasValue) html += '<p class="pilot-diagnostic-hint">Diagnostics: recorded is matching wait time or closes; ' +
        'covered is certified window time; required is window time within this backend\'s lifetime.</p>';
    html += '<details><summary>Storage diagnostics</summary><pre>' + esc(JSON.stringify(response.storage, null, 2)) + '</pre></details>';
    return html;
}
