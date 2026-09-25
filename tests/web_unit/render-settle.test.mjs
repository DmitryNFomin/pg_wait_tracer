/* Node unit tests for web/static/dev/render-settle.mjs — the deterministic
 * render-settle bookkeeping behind the gallery snapshot gate (issue #155:
 * the gate rendered the same commit twice and 13/14 dense cells differed by
 * up to 3.2%, because the capture raced the chart's async paint instead of
 * waiting for its real completion event).
 *
 * Pure state machine, no DOM/echarts/uPlot — gallery.js wires this to real
 * dataset attributes and chart events; these tests pin the bookkeeping only:
 *   - createRenderTracker: begin()/settle() collapse repeats to one pending
 *     unit; a stray settle() (nothing pending) is a no-op, not a double
 *     resolve.
 *   - createReadyGate: isReady goes true only once BOTH the initial
 *     synchronous render loop is done AND nothing is pending, and never
 *     reverts once true (a later re-render pushing pendingCount above 0
 *     again must not un-ready the gate the initial screenshot already
 *     waited on).
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    createRenderTracker, createReadyGate,
} from '../../web/static/dev/render-settle.mjs';

// ── createRenderTracker ──────────────────────────────────────────────────────

test('createRenderTracker: begin then settle is the normal one-shot cycle', () => {
    const t = createRenderTracker();
    assert.equal(t.isPending, false);
    assert.equal(t.begin(), true, 'first begin is a new pending unit');
    assert.equal(t.isPending, true);
    assert.equal(t.settle(), true, 'settle resolves the pending unit');
    assert.equal(t.isPending, false);
});

test('createRenderTracker: repeated begin() before settle collapses to one unit', () => {
    const t = createRenderTracker();
    assert.equal(t.begin(), true);
    assert.equal(t.begin(), false, 'already pending -- not a NEW pending unit');
    assert.equal(t.begin(), false);
    assert.equal(t.settle(), true, 'one settle resolves the collapsed unit');
    assert.equal(t.settle(), false, 'nothing left pending -- stray settle is a no-op');
});

test('createRenderTracker: a stray settle with nothing pending is a no-op', () => {
    const t = createRenderTracker();
    assert.equal(t.settle(), false);
    assert.equal(t.isPending, false);
});

test('createRenderTracker: settle -> begin -> settle is two independent cycles', () => {
    const t = createRenderTracker();
    t.begin();
    t.settle();
    assert.equal(t.begin(), true, 'a fresh render after a full cycle counts again');
    assert.equal(t.settle(), true);
});

// ── createReadyGate ──────────────────────────────────────────────────────────

test('createReadyGate: not ready until the loop is done AND nothing pending', () => {
    const g = createReadyGate();
    g.addPending();
    assert.equal(g.isReady, false);
    g.markLoopDone();
    assert.equal(g.isReady, false, 'still one render outstanding');
    g.resolvePending();
    assert.equal(g.isReady, true);
    assert.equal(g.pendingCount, 0);
});

test('createReadyGate: markLoopDone with nothing pending is ready immediately', () => {
    const g = createReadyGate();
    assert.equal(g.isReady, false);
    g.markLoopDone();
    assert.equal(g.isReady, true, 'an all-panel page with no charts settles at once');
});

test('createReadyGate: order does not matter -- resolvePending before markLoopDone', () => {
    const g = createReadyGate();
    g.addPending();
    g.resolvePending();
    assert.equal(g.isReady, false, 'loop has not returned yet');
    g.markLoopDone();
    assert.equal(g.isReady, true);
});

test('createReadyGate: ready never reverts once true, even if pending rises again', () => {
    const g = createReadyGate();
    g.markLoopDone();
    assert.equal(g.isReady, true);
    g.addPending();                       // a tick-replay re-render begins
    assert.equal(g.isReady, true, 'the ONE-TIME gallery-ready flag must not un-set');
    assert.equal(g.pendingCount, 1);
    g.resolvePending();
    assert.equal(g.isReady, true);
    assert.equal(g.pendingCount, 0);
});

test('createReadyGate: multiple charts must ALL settle before ready', () => {
    const g = createReadyGate();
    g.addPending();
    g.addPending();
    g.addPending();
    g.markLoopDone();
    g.resolvePending();
    assert.equal(g.isReady, false, '1 of 3 settled');
    g.resolvePending();
    assert.equal(g.isReady, false, '2 of 3 settled');
    g.resolvePending();
    assert.equal(g.isReady, true, '3 of 3 settled');
});
