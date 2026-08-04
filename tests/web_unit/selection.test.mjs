/* Node unit tests for the pure pixel->time math of the drag-select overlay
 * (lib/selection.js). The pointer-event wiring is browser-only (driven by
 * Playwright), but the math is isolated so an off-by-one in the band->range
 * conversion is caught here in milliseconds.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    pixelRangeToTime, pixelBoxToValues, pointerCoordinates,
} from '../../web/static/lib/selection.js';

// Fake chart: linear pixel->value map, value = px * 1e6 (so pixels read as ms).
function linearChart(scale = 1e6, offset = 0) {
    return {
        convertFromPixel(_finder, [x]) { return [x * scale + offset]; },
    };
}

test('maps a left-to-right drag to sorted {from,to}', () => {
    const r = pixelRangeToTime(100, 300, linearChart());
    assert.deepEqual(r, { from: 100e6, to: 300e6 });
});

test('drag direction does not matter (always sorted)', () => {
    const r = pixelRangeToTime(300, 100, linearChart());
    assert.deepEqual(r, { from: 100e6, to: 300e6 });
});

test('degenerate (zero-width) range returns null', () => {
    assert.equal(pixelRangeToTime(150, 150, linearChart()), null);
});

test('honors offset (non-zero axis origin)', () => {
    const r = pixelRangeToTime(0, 50, linearChart(1e6, 5_000_000));
    assert.deepEqual(r, { from: 5_000_000, to: 55_000_000 });
});

test('null/NaN conversion -> null (no garbage range)', () => {
    const bad = { convertFromPixel() { return [NaN]; } };
    assert.equal(pixelRangeToTime(10, 90, bad), null);
    const none = { convertFromPixel() { return null; } };
    assert.equal(pixelRangeToTime(10, 90, none), null);
});

test('rounds to integer nanoseconds', () => {
    const frac = { convertFromPixel(_f, [x]) { return [x + 0.4]; } };
    const r = pixelRangeToTime(10, 90, frac);
    assert.deepEqual(r, { from: 10, to: 90 });  // 10.4->10, 90.4->90
});

test('2D box maps and sorts time plus inverted pixel-y values', () => {
    const chart = { convertFromPixel: (_finder, [x, y]) => [x * 10, 1000 / y] };
    assert.deepEqual(pixelBoxToValues(30, 100, 10, 20, chart), {
        from: 100, to: 300, yFrom: 10, yTo: 50,
    });
});

test('2D box rejects degenerate or invalid conversion', () => {
    const chart = { convertFromPixel: () => [NaN, 1] };
    assert.equal(pixelBoxToValues(0, 0, 20, 20, chart), null);
    assert.equal(pixelBoxToValues(0, 0, 0, 20, chart), null);
});

test('horizontal-only box retains a valid time range with degenerate y', () => {
    const chart = { convertFromPixel: (_finder, [x, y]) => [x * 10, y] };
    assert.deepEqual(pixelBoxToValues(10, 25, 30, 25, chart), {
        from: 100, to: 300, yFrom: 25, yTo: 25,
    });
});

test('padded host keeps band host-local and conversion canvas-local', () => {
    const p = pointerCoordinates(330, 608,
        { left: 20, top: 584 }, { left: 30, top: 588 });
    assert.deepEqual(p, {
        hostX: 310, hostY: 24, chartX: 300, chartY: 20,
    });
});
