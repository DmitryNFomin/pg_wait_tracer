/* Node unit tests for the explicit state modules (lib/state.js) and the
 * transport's single-flight superseding (lib/transport.js). These replace the
 * old grab-bag global + _refreshGen counters; the tests pin their behavior.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    ServerInfo, TimeRange, FilterStack, CompareState, FIFTEEN_MIN_NS,
} from '../../web/static/lib/state.js';
import { Transport, CancelledError } from '../../web/static/lib/transport.js';

// ── TimeRange ──────────────────────────────────────────────────────────────

test('initDefault: last 15 min ending at server now', () => {
    const s = new ServerInfo();
    s.update({ from_ns: 0, to_ns: 100, now_ns: 1_000_000_000_000, num_cpus: 4 });
    const t = new TimeRange(s);
    t.initDefault();
    assert.equal(t.to, 1_000_000_000_000);
    assert.equal(t.from, 1_000_000_000_000 - FIFTEEN_MIN_NS);
});

test('anchorLive always ends at NOW (never stale)', () => {
    const s = new ServerInfo();
    s.update({ now_ns: 5_000_000_000_000 });
    const t = new TimeRange(s);
    t.anchorLive(300);  // last 5 min
    assert.equal(t.to, 5_000_000_000_000);
    assert.equal(t.from, 5_000_000_000_000 - 300e9);
    // server clock advances -> re-anchor moves the window forward
    s.update({ now_ns: 6_000_000_000_000 });
    t.anchorLive(300);
    assert.equal(t.to, 6_000_000_000_000);
});

test('zoomTo records history; zoomOut pops it', () => {
    const s = new ServerInfo();
    s.update({ from_ns: 0, to_ns: 1000, now_ns: 1000 });
    const t = new TimeRange(s);
    t.set(100, 200);
    t.zoomTo(120, 180);
    assert.deepEqual([t.from, t.to], [120, 180]);
    t.zoomOut();
    assert.deepEqual([t.from, t.to], [100, 200]);
});

test('zoomOut with empty history widens 2x clamped to trace bounds', () => {
    const s = new ServerInfo();
    s.update({ from_ns: 0, to_ns: 1000 });
    const t = new TimeRange(s);
    t.set(400, 600);          // span 200, mid 500
    t.zoomOut();
    assert.deepEqual([t.from, t.to], [300, 700]);  // mid +/- span
    t.set(50, 150);
    t.zoomOut();              // would go -50..250, clamp low to 0
    assert.equal(t.from, 0);
});

test('compare state is one negative offset scalar and rejects future baselines', () => {
    const c = new CompareState();
    assert.equal(c.enabled, false);
    assert.equal(c.set(true, '-86400000000000'), true);
    assert.deepEqual(c.snapshot(), { enabled: true, offsetNs: -86400000000000 });
    assert.equal(c.set(true, '1'), true);
    assert.equal(c.enabled, false);
});

// ── FilterStack ────────────────────────────────────────────────────────────

test('drill/drillUp round-trips filters and remembers the source view', () => {
    const f = new FilterStack();
    assert.ok(f.isEmpty());
    f.drill('class', 'IO', 'IO', 'overview');
    assert.deepEqual(f.snapshot(), { class: 'IO' });
    f.drill('event_id', 5, 'IO:DataFileRead', 'events');
    assert.deepEqual(f.snapshot(), { class: 'IO', event_id: 5 });
    assert.equal(f.breadcrumbs.length, 2);

    const backToView = f.drillUp(1);   // back to after the first drill
    assert.equal(backToView, 'events');
    assert.deepEqual(f.snapshot(), { class: 'IO' });
    assert.equal(f.breadcrumbs.length, 1);
});

test('clear resets everything', () => {
    const f = new FilterStack();
    f.drill('pid', 1001, 'PID 1001', 'sessions');
    f.clear();
    assert.ok(f.isEmpty());
    assert.deepEqual(f.snapshot(), {});
});

// ── Transport single-flight ─────────────────────────────────────────────────

// Minimal fake WebSocket capturing sent frames.
class FakeWS {
    constructor() { this.sent = []; this.readyState = 1; this._listeners = {}; }
    addEventListener(ev, fn) { (this._listeners[ev] ||= []).push(fn); }
    send(s) { this.sent.push(JSON.parse(s)); }
    deliver(obj) {
        const e = { data: JSON.stringify(obj) };
        (this._listeners.message || []).forEach(fn => fn(e));
    }
}

// WebSocket.OPEN constant for Transport.isOpen()
globalThis.WebSocket = { OPEN: 1 };

test('request supersedes the pending request on the same channel', async () => {
    const ws = new FakeWS();
    const tr = new Transport();
    tr.attach(ws);

    const p1 = tr.request('overview.table', 'time_model', { from: 1 });
    const p1err = p1.catch(e => e);          // first should be cancelled
    const p2 = tr.request('overview.table', 'time_model', { from: 2 });

    const err = await p1err;
    assert.ok(err instanceof CancelledError, 'first request cancelled');

    // Two frames went out (ids 1 and 2).
    assert.equal(ws.sent.length, 2);
    const id2 = ws.sent[1].id;

    // Late response for the superseded id1 is dropped; id2 resolves.
    ws.deliver({ id: ws.sent[0].id, value: 'stale' });
    ws.deliver({ id: id2, value: 'fresh' });
    const r2 = await p2;
    assert.equal(r2.value, 'fresh');
});

test('different channels do not supersede each other', async () => {
    const ws = new FakeWS();
    const tr = new Transport();
    tr.attach(ws);
    const a = tr.request('active.aas', 'aas', {});
    const b = tr.request('overview.table', 'time_model', {});
    ws.deliver({ id: ws.sent[0].id, v: 'a' });
    ws.deliver({ id: ws.sent[1].id, v: 'b' });
    assert.equal((await a).v, 'a');
    assert.equal((await b).v, 'b');
});

test('cancel rejects the pending request and drops its late response', async () => {
    const ws = new FakeWS();
    const tr = new Transport();
    tr.attach(ws);
    const p = tr.request('view.ch', 'cmd', {});
    const perr = p.catch(e => e);
    tr.cancel('view.ch');
    assert.ok((await perr) instanceof CancelledError);
    // delivering the (now unknown) id is a no-op, not a throw
    ws.deliver({ id: ws.sent[0].id, v: 'late' });
});

test('rejectAll clears everything (disconnect)', async () => {
    const ws = new FakeWS();
    const tr = new Transport();
    tr.attach(ws);
    const p = tr.send('info');
    const perr = p.catch(e => e);
    tr.rejectAll('disconnected');
    assert.equal((await perr).message, 'disconnected');
    assert.deepEqual(tr.pending, {});
});
