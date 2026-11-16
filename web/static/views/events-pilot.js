import { pilotRequest, pilotEvidenceHtml } from '../lib/builders/pilot-evidence.js';

let unmount = null;
export function unmountEventsPilot() {
    if (unmount) unmount();
    unmount = null;
}

/* A sibling of the legacy pane: an unavailable legacy request cannot erase
 * this opt-in panel. The wall-clock picker never enters this request. */
export function mountEventsPilot(ctx) {
    unmountEventsPilot();
    const params = new URLSearchParams(window.location.search);
    if (params.get('pilot') !== '1') return;
    const panel = document.createElement('section');
    panel.id = 'events-pilot';
    panel.className = 'events-pilot';
    panel.innerHTML = '<h2>Exact evidence pilot · evidence-v1</h2>' +
        '<p>One backend generation and event. Enter monotonic nanoseconds from the capture; the time picker does not apply.</p>' +
        '<form><label>Capture <input name="capture" required></label>' +
        '<label>PID <input name="pid" inputmode="numeric" required></label>' +
        '<label>Fork (ns; empty = unknown) <input name="fork" inputmode="numeric"></label>' +
        '<label>Event <input name="event" required></label>' +
        '<label>From (monotonic ns) <input name="from" inputmode="numeric" required></label>' +
        '<label>To (monotonic ns) <input name="to" inputmode="numeric" required></label>' +
        '<button type="submit">Query exact evidence</button></form><div class="pilot-result" aria-live="polite">Enter a target and window.</div>';
    const form = panel.querySelector('form');
    const result = panel.querySelector('.pilot-result');
    const defaults = { capture: '', pid: '', fork: '', event: 'PgSleep', from: '', to: '' };
    for (const key of Object.keys(defaults)) form.elements[key].value = params.get('pilot_' + key) ?? defaults[key];
    ctx.summaryEl.before(panel);
    const channel = ctx.channel('pilot');
    let current = null;
    async function submit(e) {
        if (e) e.preventDefault();
        const token = {};
        current = token;
        try {
            const fields = Object.fromEntries(new FormData(form));
            const request = pilotRequest(fields);
            result.textContent = 'Reading exact evidence…';
            const response = await ctx.transport.request(channel, 'pilot_wait_metrics', request);
            if (current === token && panel.isConnected) result.innerHTML = pilotEvidenceHtml(response);
        } catch (error) {
            if (current === token && panel.isConnected) result.textContent = error.message;
        }
    }
    form.addEventListener('submit', submit);
    unmount = () => { current = null; ctx.transport.cancel(channel); panel.remove(); };
    if (params.has('pilot_from') && params.has('pilot_to')) void submit();
}
