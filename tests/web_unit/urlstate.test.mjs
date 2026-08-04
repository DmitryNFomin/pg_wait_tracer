/* Node unit tests for the URL hash-state codec (lib/state.js — Track U
 * Phase U2, review P9) and the FilterStack extensions that back the filter
 * bar (review P3 wire 7).
 *
 * Runs under `node --test` — no framework, no browser, no network. The codec
 * is the pure half of the P9 feature; app.js only decides WHEN to read/write
 * the hash. The two review-mandated hard rules are pinned here:
 *   - ns timestamps ride as STRINGS (they exceed 2^53);
 *   - live=1 never carries a window (a live link can never pin stale time —
 *     the app re-anchors to NOW on restore).
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    FilterStack, serializeHashState, parseHashState,
} from '../../web/static/lib/state.js';

// ── serializeHashState ───────────────────────────────────────────────────────

test('serialize: fixed window with filters and sort, ns as exact strings', () => {
    const h = serializeHashState({
        tab: 'events',
        live: false,
        spanSecs: 900,
        fromNs: 1_700_000_000_000_000_000,
        toNs: 1_700_000_900_000_000_000,
        filters: { class: 'IO', pid: 1234 },
        sort: { key: 'total_ms', asc: false },
    });
    // ns appear as full decimal strings — never exponent-formatted, never
    // JSON-number-rounded (review P9: they exceed 2^53).
    assert.equal(h,
        'tab=events&from=1700000000000000000&to=1700000900000000000' +
        '&f.class=IO&f.pid=1234&sort=total_ms.desc');
});

test('serialize: live mode carries span, NEVER from/to (live means NOW)', () => {
    const h = serializeHashState({
        tab: 'overview', live: true, spanSecs: 300,
        fromNs: 1.7e18, toNs: 1.7e18 + 300e9,   // present but must be ignored
        filters: {}, sort: null,
    });
    assert.equal(h, 'tab=overview&live=1&span=300');
    assert.ok(!h.includes('from='), 'a live hash pins no window');
});

test('serialize: canonical — filter key order is sorted, same state same string', () => {
    const a = serializeHashState({ tab: 't', live: true, spanSecs: 60,
        filters: { pid: 1, class: 'IO' }, sort: null });
    const b = serializeHashState({ tab: 't', live: true, spanSecs: 60,
        filters: { class: 'IO', pid: 1 }, sort: null });
    assert.equal(a, b);
});

test('serialize: values are URI-encoded (hostile SQL in a query label-less value)', () => {
    const h = serializeHashState({ tab: 'queries', live: true, spanSecs: 60,
        filters: { query_id: '123&f.pid=9#x' }, sort: null });
    assert.ok(h.includes('f.query_id=123%26f.pid%3D9%23x'));
    // ... and round-trips intact.
    const s = parseHashState('#' + h);
    assert.equal(s.filters.query_id, '123&f.pid=9#x');
});

test('serialize: hostile keys are URI-encoded symmetrically', () => {
    const hostile = 'x&f.pid';
    const h = serializeHashState({ tab: 'queries', live: true, spanSecs: 60,
        filters: { [hostile]: '9' }, sort: null });
    const pair = h.split('&').find(p => p.startsWith('f.x%26f.pid='));
    assert.ok(pair, h);
    assert.equal(decodeURIComponent(pair.slice(0, pair.indexOf('='))), 'f.' + hostile);
    // The component codec round-trips the key, but the parser's filter
    // allowlist rejects it before it can reach the server.
    assert.deepEqual(parseHashState('#' + h).filters, {});
});

// ── parseHashState ───────────────────────────────────────────────────────────

test('parse: full round-trip preserves every field and every ns digit', () => {
    const state = {
        tab: 'sessions', live: false, spanSecs: 900,
        fromNs: 1_770_123_456_789_012_224,   // representable float64 ns
        toNs: 1_770_123_756_789_012_224,
        filters: { class: 'LWLock', event_id: 137, pid: 4242,
                   query_id: '12345678901234567890' },
        sort: { key: 'db_time_ms', asc: true },
    };
    const s = parseHashState('#' + serializeHashState(state));
    assert.equal(s.tab, 'sessions');
    assert.equal(s.live, false);
    assert.equal(s.fromNs, state.fromNs);
    assert.equal(s.toNs, state.toNs);
    // Wire types (server.c parse_filters): event_id/pid numbers; class and
    // query_id strings (query_id explicitly — uint64 precision).
    assert.deepEqual(s.filters, { class: 'LWLock', event_id: 137, pid: 4242,
                                  query_id: '12345678901234567890' });
    assert.deepEqual(s.sort, { key: 'db_time_ms', asc: true });
});

test('parse: live=1 strips any window a crafted hash smuggles in', () => {
    const s = parseHashState('#tab=overview&live=1&span=300' +
        '&from=1700000000000000000&to=1700000900000000000');
    assert.equal(s.live, true);
    assert.equal(s.fromNs, null);
    assert.equal(s.toNs, null);
    assert.equal(s.spanSecs, 300);
});

test('parse: incomplete or inverted window is dropped, not half-applied', () => {
    assert.equal(parseHashState('#tab=x&from=1700000000000000000').fromNs, null);
    const inv = parseHashState('#tab=x&from=200&to=100');
    assert.equal(inv.fromNs, null);
    assert.equal(inv.toNs, null);
});

test('parse: hostile input never throws — empty, foreign, malformed escapes', () => {
    assert.equal(parseHashState(''), null);
    assert.equal(parseHashState('#'), null);
    assert.equal(parseHashState(null), null);
    // A foreign anchor (in-page link) has no recognized pairs -> null.
    assert.equal(parseHashState('#section-3'), null);
    // Malformed %-escapes are dropped pair-by-pair, the rest still parses.
    const s = parseHashState('#tab=events&f.class=%E0%A4%A');
    assert.equal(s.tab, 'events');
    assert.deepEqual(s.filters, {});
    // Garbage numbers are rejected, not NaN-propagated.
    assert.equal(parseHashState('#span=banana'), null);
    assert.equal(parseHashState('#f.pid=banana'), null);
});

test('parse: filter keys and numeric wire domains are strict allowlists', () => {
    const s = parseHashState('#tab=events&f.class=IO&f.query_id=18446744073709551615' +
        '&f.event_id=4294967295&f.pid=2147483647&f.unknown=sent');
    assert.deepEqual(s.filters, {
        class: 'IO', query_id: '18446744073709551615',
        event_id: 4294967295, pid: 2147483647,
    });
    for (const pair of [
        'f.event_id=-1', 'f.event_id=1.5', 'f.event_id=4294967296',
        'f.pid=-1', 'f.pid=1.5', 'f.pid=2147483648', 'f.pid=Infinity',
    ]) {
        const parsed = parseHashState('#tab=events&' + pair);
        assert.deepEqual(parsed.filters, {}, pair);
    }
});

test('parse: sort direction decodes from the trailing .asc/.desc', () => {
    assert.deepEqual(parseHashState('#sort=p99_us.asc').sort,
        { key: 'p99_us', asc: true });
    assert.deepEqual(parseHashState('#sort=p99_us.desc').sort,
        { key: 'p99_us', asc: false });
    // A key containing dots keeps everything before the LAST one.
    assert.deepEqual(parseHashState('#sort=a.b.asc').sort,
        { key: 'a.b', asc: true });
});

// ── FilterStack: removeFilter / restore / labels (wire 7) ────────────────────

test('removeFilter: drops one dimension as a NEW step (history append-only)', () => {
    const f = new FilterStack();
    f.drill('class', 'IO', 'IO', 'overview');
    f.drill('pid', 1001, 'PID 1001', 'sessions');
    assert.equal(f.removeFilter('class', 'timeline'), true);
    // The other dimension survives — this is what distinguishes the chip ✕
    // from a breadcrumb rewind.
    assert.deepEqual(f.snapshot(), { pid: 1001 });
    assert.deepEqual(f.labels, { pid: 'PID 1001' });
    // The pre-removal state was PUSHED, so back restores the removed filter.
    assert.equal(f.breadcrumbs.length, 3);
    f.drillUp(2);
    assert.deepEqual(f.snapshot(), { class: 'IO', pid: 1001 });
    assert.deepEqual(f.labels, { class: 'IO', pid: 'PID 1001' });
});

test('removeFilter: absent key is a no-op', () => {
    const f = new FilterStack();
    f.drill('class', 'IO', 'IO', 'overview');
    assert.equal(f.removeFilter('pid', 'events'), false);
    assert.deepEqual(f.snapshot(), { class: 'IO' });
    assert.equal(f.breadcrumbs.length, 1);
});

test('restore: adopts a filter set with a fresh history (deep-link semantics)', () => {
    const f = new FilterStack();
    f.drill('class', 'IO', 'IO', 'overview');
    f.restore({ event_id: 137, class: 'LWLock' });
    assert.deepEqual(f.snapshot(), { event_id: 137, class: 'LWLock' });
    assert.equal(f.breadcrumbs.length, 0);
    assert.deepEqual(f.labels, {});      // labels fall back to key=value
    assert.equal(f.currentLabel, null);
    f.restore(null);
    assert.ok(f.isEmpty());
});

test('drillUp restores the label map alongside the filters', () => {
    const f = new FilterStack();
    f.drill('class', 'IO', 'IO', 'overview');
    f.drill('event_id', 137, 'IO:DataFileRead', 'events');
    assert.deepEqual(f.labels,
        { class: 'IO', event_id: 'IO:DataFileRead' });
    f.drillUp(1);
    assert.deepEqual(f.labels, { class: 'IO' });
    f.clear();
    assert.deepEqual(f.labels, {});
});
