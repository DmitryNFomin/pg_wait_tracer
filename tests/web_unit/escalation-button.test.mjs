/* Full-mode button honesty, without a daemon or browser. Runs with:
 *   node --test tests/web_unit/escalation-button.test.mjs
 * Executes the real mount, click handler, control client and Transport. Only
 * the DOM surface and daemon WebSocket peer are doubles; this does not claim
 * live tiered coverage (see test_escalation.sh / test_escalation_budget.c).
 * No defect found: known full-mode status hides the button; before status is
 * known, a click displays the daemon's truthful not-tiered refusal.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mountUnavailablePanel, mountEscalateControl }
    from '../../web/static/lib/panels.js';
import { Transport } from '../../web/static/lib/transport.js';

// src/control.c:66-67,133,157-160: exact provider, escalation disabled.
const FULL_STATUS = Object.freeze({
    mode: 'full', tier: 'escalated', escalation_supported: false,
    escalation_reason: 'none', escalation_seconds_remaining: 0,
});
// src/escalation.c:473-476 -> src/control.c:701-708. A command denial is
// nested in `response`, not a transport error. It must never look like a grant.
const FULL_DENIAL = Object.freeze({
    ok: false, error: 'escalation requires --mode tiered', budget_remaining_s: 0,
});
const EMPTY = { unavailable: 'requires full-fidelity data', fidelity: 'none' };

// Minimal DOM adapter for this mount's id lookup + listener API. Lookups only
// succeed for ids actually emitted in innerHTML, so missing buttons/status
// elements cannot be hidden by unconditional querySelector stubs.
function host() {
    return {
        style: {}, nodes: new Map(),
        set innerHTML(html) {
            this.html = html;
            this.nodes.clear();
            for (const [, id] of html.matchAll(/\bid="([^"]+)"/g)) {
                this.nodes.set('#' + id, {
                    disabled: false, textContent: '', className: '', listeners: {},
                    addEventListener(event, fn) { this.listeners[event] = fn; },
                });
            }
        },
        get innerHTML() { return this.html; },
        querySelector(selector) { return this.nodes.get(selector) || null; },
    };
}

test('full-mode status hides both panel and header escalate buttons', () => {
    const ctx = { getEscalationStatus: () => FULL_STATUS };
    const panel = host();
    mountUnavailablePanel(panel, EMPTY, ctx);
    assert.equal(panel.querySelector('#unavail-escalate'), null);
    assert.doesNotMatch(panel.innerHTML, /<button\b/);
    assert.match(panel.innerHTML, /Escalation is not available/);
    const header = host();
    mountEscalateControl(header, ctx);
    assert.equal(header.innerHTML, '');
    assert.equal(header.style.display, 'none');
});

test('visible panel button settles with the full-mode refusal, never a grant',
    { timeout: 2000 }, async (t) => {
        const originalWS = globalThis.WebSocket;
        globalThis.WebSocket = { OPEN: 1 };
        t.after(() => { globalThis.WebSocket = originalWS; });
        const frames = [];
        const ws = {
            readyState: 1,
            addEventListener(event, fn) { this[event] = fn; },
            send(frame) { frames.push(JSON.parse(frame)); },
        };
        const transport = new Transport();
        transport.attach(ws);
        t.after(() => transport.rejectAll('test finished'));
        let status = null, polls = 0;
        const ctx = {
            transport,
            getEscalationStatus: () => status,
            refreshEscalationStatus: async () => { polls++; status = FULL_STATUS; },
        };
        const panel = host();
        mountUnavailablePanel(panel, EMPTY, ctx);
        const btn = panel.querySelector('#unavail-escalate');
        const message = panel.querySelector('#unavail-status');
        assert.ok(btn, 'exercise the visible button before status is known');
        assert.ok(message, 'the result has a rendered status element');
        assert.equal(typeof btn.listeners.click, 'function', 'real click listener wired');
        const click = btn.listeners.click();
        assert.equal(btn.disabled, true);
        assert.equal(btn.textContent, 'Escalating…');
        assert.equal(frames.length, 1, 'click sends exactly one request');
        assert.deepEqual(frames[0], {
            id: 1, cmd: 'control',
            request: { cmd: 'escalate', duration_s: 60, reason: 'manual' },
        });
        ws.message({ data: JSON.stringify({ id: frames[0].id, response: FULL_DENIAL }) });
        await click;  // The test timeout refuses a handler that never settles.
        assert.match(message.textContent, /^escalation requires --mode tiered\b/);
        assert.doesNotMatch(message.textContent, /Escalated for|De-escalated/);
        assert.equal(message.className, 'unavailable-status err');
        assert.equal(btn.disabled, false, 'button does not remain stuck');
        assert.equal(btn.textContent, 'Escalate 60s');
        assert.deepEqual(transport.pending, {}, 'response completed the request');
        assert.equal(polls, 1);
        assert.equal(status, FULL_STATUS, 'full fidelity remains unchanged');
        mountUnavailablePanel(panel, EMPTY, ctx);
        assert.equal(panel.querySelector('#unavail-escalate'), null,
            'the next render removes the unsupported action');
    });
