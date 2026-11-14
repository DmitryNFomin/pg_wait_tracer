/* evidence-v1 consumer: preserve every wire digit; never derive a metric. */
import { esc } from '../format.js';

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

export function pilotEvidenceHtml(response) {
    if (response.error) return '<p role="alert">' + esc(response.error) + '</p>';
    let html = '';
    for (const key of ['wait_time', 'completed_waits']) {
        const metric = response[key];
        const value = wire(metric.value);
        html += '<div class="pilot-metric" data-metric="' + key + '"><strong>' +
            (key === 'wait_time' ? 'Wait time' : 'Completed waits') + '</strong> ' +
            '<output data-value>' + esc(value) + '</output> ' + esc(metric.unit) +
            ' <span data-completeness>' + esc(metric.completeness) + '</span>' +
            '<span data-reasons>' + esc(metric.reasons.join(', ')) + '</span>';
        if (metric.detail) html += '<p>Closed: <span data-closed>' + esc(wire(metric.detail.closed_ns)) +
            '</span> ns; open: <span data-open>' + esc(wire(metric.detail.open_ns)) + '</span> ns</p>';
        html += '<dl>';
        for (const [name, number] of Object.entries(metric.diagnostic))
            html += '<dt>' + esc(name) + '</dt><dd data-diagnostic="' + esc(name) + '">' + esc(wire(number)) + '</dd>';
        html += '</dl></div>';
    }
    html += '<details><summary>Storage diagnostics</summary><pre>' + esc(JSON.stringify(response.storage, null, 2)) + '</pre></details>';
    return html;
}
