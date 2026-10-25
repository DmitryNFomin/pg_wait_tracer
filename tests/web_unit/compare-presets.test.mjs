import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { comparePresetOffsets } from '../../web/static/lib/compare-presets.js';

const end = 1_774_000_000_000_000_000;

test('initial HTML offers no unreachable presets on a sub-second trace', () => {
    const html = readFileSync(new URL('../../web/static/index.html', import.meta.url), 'utf8');
    const offsets = [...html.matchAll(/<button\b([^>]*\bdata-b-offset="(-?\d+)"[^>]*)>/g)]
        .filter(match => !/\bdisabled\b/.test(match[1]))
        .map(match => Number(match[2]));
    for (const offset of offsets) {
        assert.ok(end - 125_000_000 + offset >= end - 500_000_000 &&
            end + offset <= end && offset < 0,
        `initially offered offset ${offset} cannot fit a 500 ms trace`);
    }
});

function assertReachable(from, to, start, end) {
    const offsets = comparePresetOffsets(from, to, start, end);
    assert.ok(offsets.length > 0, 'available history must yield a usable preset');
    assert.equal(new Set(offsets).size, offsets.length, 'no duplicate choices');
    for (const offset of offsets) {
        assert.ok(Number.isSafeInteger(offset) && offset < 0);
        assert.ok(from + offset >= start && to + offset <= end,
            `unreachable offset ${offset}: B=[${from + offset}, ${to + offset}], trace=[${start}, ${end}]`);
        assert.ok(from + offset < from, 'offset must actually move the window');
    }
    return offsets;
}

for (const span of [35 * 60e9, 20e9, 500_000_000]) {
    test(`all offered presets fit a ${span} ns served trace`, () => {
        assertReachable(end - span / 4, end, end - span, end);
    });
}

test('range changes and retention changes recompute the available history', () => {
    const start = end - 35 * 60e9;
    const later = assertReachable(end - 5 * 60e9, end, start, end);
    const earlier = assertReachable(start + 20e9, start + 40e9, start, end);
    assert.ok(Math.max(...earlier.map(Math.abs)) < Math.min(...later.map(Math.abs)));
    assertReachable(end - 5 * 60e9, end, end - 6 * 60e9, end);
});

test('no earlier baseline fits a full-span or oversized range', () => {
    const start = end - 20e9;
    assert.deepEqual(comparePresetOffsets(start, end, start, end), []);
    assert.deepEqual(comparePresetOffsets(start - 1e9, end, start, end), []);
    assert.deepEqual(comparePresetOffsets(start + 1e9, end + 2e9, start, end), []);
});

test('a live range beyond the latest sample still offers only fully retained baselines', () => {
    assertReachable(end - 5 * 60e9, end + 30e9, end - 35 * 60e9, end);
});

test('tiny history deduplicates choices and invalid bounds offer none', () => {
    assertReachable(2, 3, 1, 3);
    for (const bounds of [[0, 0, 0, 1], [1, 2, 0, 0], [NaN, 2, 0, 3],
        [1, Infinity, 0, 3], [2, 1, 0, 3]]) {
        assert.deepEqual(comparePresetOffsets(...bounds), []);
    }
});
