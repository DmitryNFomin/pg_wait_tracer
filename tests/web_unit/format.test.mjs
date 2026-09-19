/* Node unit tests for lib/format.js — the UTC time conversions (UI-11).
 *
 * pgwt displays every time in UTC and defines the custom-range
 * datetime-local inputs as UTC too. These conversions must be independent of
 * the host timezone: the functions use only getUTC accessors and Z-suffixed
 * parsing, so the expected strings hold no matter what TZ the test runs under.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    nsToDatetimeLocalUTC, datetimeLocalUTCToNs, fmtTime, versionSkew, esc,
} from '../../web/static/lib/format.js';

// 2026-03-15 12:34:56 UTC
const NS = Date.UTC(2026, 2, 15, 12, 34, 56) * 1e6;

test('nsToDatetimeLocalUTC renders the UTC wall time regardless of host TZ', () => {
    assert.equal(nsToDatetimeLocalUTC(NS), '2026-03-15T12:34:56');
});

test('datetimeLocalUTCToNs parses the input as UTC (a UTC+3 browser gets no 3h shift)', () => {
    assert.equal(datetimeLocalUTCToNs('2026-03-15T12:34:56'), NS);
    // Without seconds (what browsers emit unless step=1 forces them).
    assert.equal(datetimeLocalUTCToNs('2026-03-15T12:34'),
        Date.UTC(2026, 2, 15, 12, 34, 0) * 1e6);
});

test('round-trip ns -> input string -> ns is exact at second granularity', () => {
    assert.equal(datetimeLocalUTCToNs(nsToDatetimeLocalUTC(NS)), NS);
});

test('datetimeLocalUTCToNs rejects garbage', () => {
    assert.equal(datetimeLocalUTCToNs(''), null);
    assert.equal(datetimeLocalUTCToNs(null), null);
    assert.equal(datetimeLocalUTCToNs('not-a-date'), null);
});

test('fmtTime renders HH:MM:SS in UTC', () => {
    assert.equal(fmtTime(NS), '12:34:56');
});

// -- Version-skew banner (T7 / TST-11) ---------------------------------------
// Mirror of web/bridge.go:versionSkewWarning. Warn, never refuse.

test('versionSkew returns null when client and server match exactly', () => {
    assert.equal(versionSkew('v0.13', 1, 'v0.13', 1), null);
});

test('versionSkew warns (safe) when versions differ but protocol matches', () => {
    const s = versionSkew('v0.13', 1, 'v0.12', 1);
    assert.equal(s.level, 'warn');
    assert.equal(s.short, 'version skew');
    assert.match(s.detail, /v0\.12/);
    assert.match(s.detail, /v0\.13/);
});

test('versionSkew errors on a protocol mismatch', () => {
    const s = versionSkew('v0.14', 2, 'v0.13', 1);
    assert.equal(s.level, 'error');
    assert.equal(s.short, 'protocol mismatch');
});

test('versionSkew warns when the server predates the handshake (null fields)', () => {
    const s = versionSkew('v0.13', 1, null, null);
    assert.equal(s.level, 'warn');
    assert.match(s.detail, /predates/);
});

// ── esc() also escapes quotes: its output lands in title="..." attributes ──

test('esc escapes the five HTML-significant characters, quotes included', () => {
    assert.equal(esc('a & b < c > d'), 'a &amp; b &lt; c &gt; d');
    assert.equal(esc('say "hi"'), 'say &quot;hi&quot;');
    assert.equal(esc("it's"), 'it&#39;s');
});

test('esc output cannot break out of a title attribute', () => {
    // A hostile wait-event / SQL string reaching a tooltip: with a bare " the
    // attribute would close early and the rest would parse as markup.
    const hostile = '" onmouseover="alert(1)" x="';
    const html = '<span title="' + esc(hostile) + '">v</span>';
    assert.ok(!html.includes('onmouseover="'), html);
    // Exactly one attribute value: the opening quote, the escaped payload,
    // the closing quote.
    assert.equal(html.match(/"/g).length, 2, html);
});
