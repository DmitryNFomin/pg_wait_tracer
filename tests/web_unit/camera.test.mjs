/* Node unit tests for lib/camera.js — the pure camera of the instrument model
 * (Track U, U2a; docs/INSTRUMENT_ARCHITECTURE.md §1).
 *
 * These pin:
 *   - cursor-anchored zoom: the timestamp under the cursor stays pinned
 *     (bit-exact at power-of-two coordinates; within a few float64 ulps at
 *     realistic 2026 ns epochs, where ulp ≈ 256 ns)
 *   - span clamps to [minSpanNs, maxSpanNs] with the anchor still pinned
 *   - the FULL follow/detached transition table — the structural enforcement
 *     of the project's live-means-NOW rule (a live tick must never re-anchor
 *     a detached investigation; re-attach must never show a stale right edge)
 *   - quantize(): power-of-2 resolution ladder, 3x skirt, bit-exact boundary
 *     alignment at 2026 epochs, phase stability (same window => same strip).
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Camera, MIN_RES_NS, MIN_SPAN_NS } from '../../web/static/lib/camera.js';

// A 2026-02-ish wall-clock epoch in ns (float64 ulp here is 256 ns).
const EPOCH_2026 = 1_770_000_000_000_000_000;
const SPAN_15M = 900e9;

// ── cursor-anchored zoom ────────────────────────────────────────────────────

test('zoomAt pins the anchor timestamp exactly (power-of-two coordinates)', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    const moved = cam.zoomAt(0.25, 2);
    assert.equal(moved, true);
    // anchor = 0.25 * 2^40 = 2^38; newSpan = 2^39; from = 2^38 - 0.25*2^39 = 2^37.
    assert.equal(cam.fromNs, 2 ** 37);
    assert.equal(cam.toNs, 2 ** 37 + 2 ** 39);
    assert.equal(cam.fromNs + 0.25 * cam.span(), 2 ** 38, 'anchor still under cursor');
});

test('zoomAt pins the anchor at realistic 2026 ns epochs (ulp tolerance)', () => {
    const cam = new Camera({ fromNs: EPOCH_2026, toNs: EPOCH_2026 + SPAN_15M });
    const frac = 0.37;
    const anchorBefore = cam.fromNs + frac * cam.span();
    cam.zoomAt(frac, 1.6);
    const anchorAfter = cam.fromNs + frac * cam.span();
    // ulp at 1.77e18 is 256 ns; allow a few ulps of arithmetic round-off —
    // orders of magnitude below any honest data floor (100 ms sampled).
    assert.ok(Math.abs(anchorAfter - anchorBefore) <= 1024,
        `anchor drifted ${anchorAfter - anchorBefore} ns`);
    assert.ok(Math.abs(cam.span() - SPAN_15M / 1.6) <= 1024, 'span shrank by factor');
});

test('zoomAt in then out at the same cursor round-trips the window', () => {
    const cam = new Camera({ fromNs: EPOCH_2026, toNs: EPOCH_2026 + SPAN_15M });
    cam.zoomAt(0.7, 2.5);
    cam.zoomAt(0.7, 1 / 2.5);
    assert.ok(Math.abs(cam.fromNs - EPOCH_2026) <= 1024, 'from restored');
    assert.ok(Math.abs(cam.toNs - (EPOCH_2026 + SPAN_15M)) <= 1024, 'to restored');
});

test('zoomAt clamps span at minSpanNs with the anchor still pinned', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40, minSpanNs: 1000 });
    cam.zoomAt(0.5, 1e12);   // absurd zoom-in
    assert.equal(cam.span(), 1000, 'span clamped to the floor');
    assert.equal(cam.fromNs + 0.5 * cam.span(), 2 ** 39, 'anchor (mid) pinned');
});

test('zoomAt clamps span at maxSpanNs', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40, maxSpanNs: 2e12 });
    cam.zoomAt(0.5, 1 / 8);  // zoom out 8x would exceed the cap
    assert.equal(cam.span(), 2e12, 'span clamped to the cap');
});

test('zoomAt rejects nonsense factors without touching state', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40, mode: 'follow' });
    assert.equal(cam.zoomAt(0.5, 0), false);
    assert.equal(cam.zoomAt(0.5, -2), false);
    assert.equal(cam.zoomAt(0.5, NaN), false);
    assert.equal(cam.mode, 'follow', 'no detach on a rejected gesture');
});

// ── pan / setWindow ─────────────────────────────────────────────────────────

test('panByFrac shifts by the exact fraction of span (and inverts)', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    cam.panByFrac(0.5);
    assert.equal(cam.fromNs, 2 ** 39);
    assert.equal(cam.toNs, 2 ** 40 + 2 ** 39);
    cam.panByFrac(-0.5);
    assert.equal(cam.fromNs, 0);
    assert.equal(cam.toNs, 2 ** 40);
});

test('setWindow sorts unordered input', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    cam.setWindow(9e9, 3e9);
    assert.equal(cam.fromNs, 3e9);
    assert.equal(cam.toNs, 9e9);
});

test('setWindow expands a degenerate range to minSpanNs around the point', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    cam.setWindow(5e6, 5e6);
    assert.equal(cam.span(), MIN_SPAN_NS);
    assert.equal((cam.fromNs + cam.toNs) / 2, 5e6, 'centered on the point');
});

// ── follow / detached state machine ─────────────────────────────────────────

test('followTick advances the window keeping span (right edge = now)', () => {
    const cam = new Camera({ fromNs: 0, toNs: 0 });
    cam.attachFollow(EPOCH_2026);
    const span = cam.span();
    assert.equal(cam.followTick(EPOCH_2026 + 5e9), true);
    assert.equal(cam.toNs, EPOCH_2026 + 5e9, 'right edge = now');
    assert.equal(cam.span(), span, 'span preserved');
    assert.equal(cam.mode, 'follow');
});

test('followTick ignores out-of-order (older/equal) now', () => {
    const cam = new Camera({ mode: 'follow', fromNs: 1e12, toNs: 2e12 });
    assert.equal(cam.followTick(2e12), false, 'equal now: no move');
    assert.equal(cam.followTick(1.5e12), false, 'older now: no move');
    assert.equal(cam.fromNs, 1e12);
    assert.equal(cam.toNs, 2e12);
});

test('followTick is a strict no-op when detached (tick never re-anchors)', () => {
    const cam = new Camera({ fromNs: 1e12, toNs: 2e12 });   // detached
    const events = [];
    cam.onChange(e => events.push(e));
    assert.equal(cam.followTick(9e12), false);
    assert.equal(cam.fromNs, 1e12, 'window untouched');
    assert.equal(cam.toNs, 2e12, 'window untouched');
    assert.equal(events.length, 0, 'no change event');
});

test('attachFollow re-anchors a stale window to NOW, preserving span', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    cam.setWindow(1e12, 1e12 + 600e9);      // old, detached investigation
    cam.attachFollow(EPOCH_2026);           // Live button pressed
    assert.equal(cam.toNs, EPOCH_2026, 'right edge is NOW, never stale');
    assert.equal(cam.span(), 600e9, 'span preserved across re-attach');
    assert.equal(cam.mode, 'follow');
});

test('FULL follow/detached transition table', () => {
    const T = 2 ** 41;
    const OPS = {
        zoomAt: (c) => c.zoomAt(0.5, 2),
        panByFrac: (c) => c.panByFrac(0.25),
        setWindow: (c) => c.setWindow(T - 2 ** 31, T - 2 ** 30),
        followTick: (c) => c.followTick(T + 2 ** 20),
        attachFollow: (c) => c.attachFollow(T + 2 ** 20),
        detach: (c) => c.detach(),
    };
    // [startMode, op, endMode, windowMoves]
    const TABLE = [
        ['follow',   'zoomAt',       'detached', true],
        ['follow',   'panByFrac',    'detached', true],
        ['follow',   'setWindow',    'detached', true],
        ['follow',   'followTick',   'follow',   true],
        ['follow',   'attachFollow', 'follow',   true],
        ['follow',   'detach',       'detached', false],
        ['detached', 'zoomAt',       'detached', true],
        ['detached', 'panByFrac',    'detached', true],
        ['detached', 'setWindow',    'detached', true],
        ['detached', 'followTick',   'detached', false],   // THE invariant
        ['detached', 'attachFollow', 'follow',   true],
        ['detached', 'detach',       'detached', false],
    ];
    for (const [start, op, end, moves] of TABLE) {
        const cam = new Camera({ fromNs: T - 2 ** 30, toNs: T, mode: start });
        OPS[op](cam);
        const tag = `${start} + ${op}`;
        assert.equal(cam.mode, end, `${tag} -> ${end}`);
        const moved = cam.fromNs !== T - 2 ** 30 || cam.toNs !== T;
        assert.equal(moved, moves, `${tag} window ${moves ? 'moves' : 'stays'}`);
    }
});

// ── quantize: the power-of-2 resolution ladder ──────────────────────────────

test('quantize picks the largest power-of-2 bucket <= span/target', () => {
    const cam = new Camera({ fromNs: 0, toNs: SPAN_15M });   // 15 min
    const q = cam.quantize(300);
    assert.equal(q.resNs, 2 ** 31, '900e9/300 = 3e9 -> 2^31 ≈ 2.15 s buckets');
    const winBuckets = cam.span() / q.resNs;
    assert.ok(winBuckets >= 300 && winBuckets < 600,
        `window holds [target, 2*target) buckets, got ${winBuckets}`);
});

test('quantize: 3x skirt, outward alignment, exact at 2026 epochs', () => {
    const cam = new Camera({ fromNs: EPOCH_2026, toNs: EPOCH_2026 + SPAN_15M });
    const q = cam.quantize(300);
    const span = cam.span();
    assert.ok(Number.isInteger(Math.log2(q.resNs)), 'resNs is a power of two');
    assert.ok(q.stripFrom <= cam.fromNs - span, 'skirt covers one span before');
    assert.ok(q.stripTo >= cam.toNs + span, 'skirt covers one span after');
    assert.ok(q.stripTo - q.stripFrom < 3 * span + 2 * q.resNs, 'skirt is 3x, not more');
    assert.ok(Number.isInteger(q.stripFrom / q.resNs), 'stripFrom on the res grid');
    assert.ok(Number.isInteger(q.stripTo / q.resNs), 'stripTo on the res grid');
    assert.equal(q.buckets, (q.stripTo - q.stripFrom) / q.resNs, 'exact bucket count');
    assert.equal(q.winFromNs, cam.fromNs);
    assert.equal(q.winToNs, cam.toNs);
});

test('quantize is phase-stable: same window => same strip key', () => {
    const a = new Camera({ fromNs: EPOCH_2026, toNs: EPOCH_2026 + SPAN_15M });
    const b = new Camera({ fromNs: EPOCH_2026, toNs: EPOCH_2026 + SPAN_15M });
    assert.deepEqual(a.quantize(300), a.quantize(300), 'recompute drift-free');
    assert.deepEqual(a.quantize(300), b.quantize(300), 'instance-independent');
});

test('quantize: a pan by exactly resNs shifts the strip by exactly resNs', () => {
    const S = 2 ** 50;
    const a = new Camera({ fromNs: S, toNs: S + 2 ** 40 });
    const qa = a.quantize(300);
    const b = new Camera({ fromNs: S + qa.resNs, toNs: S + 2 ** 40 + qa.resNs });
    const qb = b.quantize(300);
    assert.equal(qb.resNs, qa.resNs);
    assert.equal(qb.stripFrom, qa.stripFrom + qa.resNs, 'grid-snapped shift');
    assert.equal(qb.buckets, qa.buckets);
});

test('quantize ladder: zooming in 2x steps resNs down one power', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    const r0 = cam.quantize(300).resNs;
    cam.zoomAt(0.5, 2);
    assert.equal(cam.quantize(300).resNs, r0 / 2);
});

test('quantize clamps resNs at MIN_RES_NS (the float64 ulp floor)', () => {
    const cam = new Camera({ fromNs: 0, toNs: MIN_SPAN_NS });   // 1 ms window
    const q = cam.quantize(10000);   // ideal = 100 ns -> below the floor
    assert.equal(q.resNs, MIN_RES_NS);
});

// ── onChange subscription ───────────────────────────────────────────────────

test('onChange delivers snapshots with causes, in order', () => {
    const cam = new Camera({ fromNs: 0, toNs: 2 ** 40 });
    const events = [];
    cam.onChange(e => events.push(e));
    cam.setWindow(0, 2 ** 39);
    cam.zoomAt(0.5, 2);
    cam.panByFrac(0.1);
    cam.attachFollow(2 ** 41);
    cam.followTick(2 ** 41 + 2 ** 20);
    cam.detach();
    assert.deepEqual(events.map(e => e.cause),
        ['set', 'zoom', 'pan', 'attach', 'tick', 'detach']);
    const last = events[events.length - 1];
    assert.deepEqual(
        { fromNs: last.fromNs, toNs: last.toNs, mode: last.mode },
        cam.snapshot(),
        'event carries the resulting state');
});

test('onChange is silent on no-ops and after unsubscribe', () => {
    const cam = new Camera({ fromNs: 1e12, toNs: 2e12 });   // detached
    const events = [];
    const off = cam.onChange(e => events.push(e));
    cam.detach();                    // already detached: no event
    cam.followTick(9e12);            // detached tick: no event
    assert.equal(events.length, 0);
    cam.panByFrac(0.5);
    assert.equal(events.length, 1);
    off();
    cam.panByFrac(0.5);
    assert.equal(events.length, 1, 'unsubscribed: no further events');
});
