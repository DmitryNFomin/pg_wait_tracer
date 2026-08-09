/* Node unit tests for lib/stripcache.js — the strip cache of the instrument
 * model (Track U, U2a; docs/INSTRUMENT_ARCHITECTURE.md §1/§3 row 2/row 8).
 *
 * These pin the cache policy the dashboard transport deliberately lacks:
 *   - stale-but-useful retention: a coarser/older overlapping strip keeps
 *     serving (stretched) while the exact strip is still in flight — data is
 *     NEVER discarded on supersede
 *   - in-flight dedup by strip key; latest-wins flag on onData arrivals
 *   - LRU eviction at maxStrips
 *   - generation invalidation: strips AND in-flight results of the old
 *     generation are dropped (filter changes must never serve old data)
 *   - errors resolve as { ok:false, error } (ensure never rejects) and free
 *     the in-flight slot for retry.
 *
 * fetchStrip is injected, so the cache is driven with a controllable fake —
 * no timers, no network, fully deterministic.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    StripCache, stripKey, fullyCoversStrip,
} from '../../web/static/lib/stripcache.js';

/* Controllable fetchStrip: records calls, exposes per-call resolve/reject. */
function makeFetch() {
    const calls = [];
    const fetch = (args) => {
        const c = { args };
        c.promise = new Promise((resolve, reject) => {
            c.resolve = resolve;
            c.reject = reject;
        });
        calls.push(c);
        return c.promise;
    };
    return { fetch, calls };
}

/* Build a quantized descriptor the way lib/camera.js quantize() does:
 * 3x skirt around the window, aligned outward to resNs boundaries. */
function mkQ(resNs, winFrom, winTo) {
    const span = winTo - winFrom;
    const stripFrom = Math.floor((winFrom - span) / resNs) * resNs;
    const stripTo = Math.ceil((winTo + span) / resNs) * resNs;
    return {
        resNs, stripFrom, stripTo,
        buckets: Math.round((stripTo - stripFrom) / resNs),
        winFromNs: winFrom, winToNs: winTo,
    };
}

function wired(opts) {
    const { fetch, calls } = makeFetch();
    const cache = new StripCache({ fetchStrip: fetch, ...(opts || {}) });
    return { cache, calls };
}

// ── dedup + basic fill ──────────────────────────────────────────────────────

test('ensure dedups in-flight fetches by strip key', async () => {
    const { cache, calls } = wired();
    const q = mkQ(1024, 100_000, 200_000);
    const p1 = cache.ensure(q);
    const p2 = cache.ensure(q);
    assert.equal(calls.length, 1, 'one fetch for two ensures');
    assert.deepEqual(calls[0].args,
        { fromNs: q.stripFrom, toNs: q.stripTo, buckets: q.buckets },
        'fetchStrip receives the strip geometry');
    calls[0].resolve({ tag: 'strip' });
    const [r1, r2] = await Promise.all([p1, p2]);
    assert.deepEqual(r1, { ok: true, payload: { tag: 'strip' } });
    assert.deepEqual(r2, { ok: true, payload: { tag: 'strip' } });
    const got = cache.get(q);
    assert.equal(got.exact, true);
    assert.deepEqual(got.payload, { tag: 'strip' });
});

test('ensure on an exact-cached strip is a no-fetch short-circuit', async () => {
    const { cache, calls } = wired();
    const q = mkQ(1024, 100_000, 200_000);
    const p = cache.ensure(q);
    calls[0].resolve({ tag: 'strip' });
    await p;
    const r = await cache.ensure(q);
    assert.equal(calls.length, 1, 'no second fetch');
    assert.deepEqual(r, { ok: true, payload: { tag: 'strip' }, cached: true });
});

test('constructor requires fetchStrip', () => {
    assert.throws(() => new StripCache({}), /fetchStrip/);
});

// ── stale-but-useful serving ────────────────────────────────────────────────

test('get serves the coarser overlapping strip while the refine is in flight', async () => {
    const { cache, calls } = wired();
    const coarse = mkQ(4096, 100_000, 200_000);
    const fine = mkQ(1024, 120_000, 150_000);   // window inside the coarse strip
    const pc = cache.ensure(coarse);
    calls[0].resolve({ tag: 'coarse' });
    await pc;

    // Camera zoomed in: exact strip not cached yet -> the coarse one serves,
    // reported with ITS OWN geometry so the renderer can stretch it honestly.
    const stale = cache.get(fine);
    assert.equal(stale.exact, false);
    assert.deepEqual(stale.payload, { tag: 'coarse' });
    assert.equal(stale.resNs, coarse.resNs);
    assert.equal(stale.stripFrom, coarse.stripFrom);
    assert.equal(stale.stripTo, coarse.stripTo);

    const pf = cache.ensure(fine);              // refine in background
    assert.equal(calls.length, 2);
    assert.deepEqual(cache.get(fine).payload, { tag: 'coarse' },
        'still serving stale while in flight — never blank');
    calls[1].resolve({ tag: 'fine' });
    await pf;
    const exact = cache.get(fine);
    assert.equal(exact.exact, true);
    assert.deepEqual(exact.payload, { tag: 'fine' });
});

test('small compare offset cannot reuse overlapping A skirt as baseline B', async () => {
    const { cache, calls } = wired();
    const a = mkQ(1024, 100_000, 200_000);
    const b = mkQ(1024, 50_000, 150_000); // |offset| <= A window span
    const pa = cache.ensure(a);
    calls[0].resolve({ tag: 'A-skirt' });
    await pa;

    const overlap = cache.get(b);
    assert.ok(overlap, 'A 3x skirt overlaps the shifted B request');
    assert.equal(overlap.exact, false);
    assert.equal(fullyCoversStrip(overlap, b), false,
        'provisional compare must wait for B instead of fabricating A-vs-A');

    const pb = cache.ensure(b);
    calls[1].resolve({ tag: 'B-strip' });
    await pb;
    assert.equal(fullyCoversStrip(cache.get(b), b), true);
    assert.deepEqual(cache.get(b).payload, { tag: 'B-strip' });
});

test('get returns null when nothing overlaps the window', async () => {
    const { cache, calls } = wired();
    const p = cache.ensure(mkQ(1024, 100_000, 200_000));
    calls[0].resolve({ tag: 'a' });
    await p;
    assert.equal(cache.get(mkQ(1024, 900_000_000, 900_100_000)), null);
});

test('best-pick prefers window coverage, then closest resolution (finer on tie)', async () => {
    const { cache, calls } = wired();
    // Full coverage, far resolution vs partial coverage, exact resolution.
    const win = [1_000_000, 2_000_000];
    const full = mkQ(16384, win[0], win[1]);                 // covers all of win
    const partial = mkQ(2048, win[0] - 700_000, win[0] + 100_000); // clips win's left edge
    let n = 0;
    for (const q of [full, partial]) {
        const p = cache.ensure(q);
        calls[n++].resolve({ tag: stripKey(q) });
        await p;
    }
    const want = mkQ(2048, win[0], win[1]);   // not cached
    const got = cache.get(want);
    assert.equal(got.exact, false);
    assert.deepEqual(got.payload, { tag: stripKey(full) },
        'full coverage beats matching resolution');

    // Equal coverage: resolution distance decides, finer wins a distance tie.
    const fine = mkQ(1024, win[0], win[1]);
    const coarse = mkQ(4096, win[0], win[1]);
    for (const q of [coarse, fine]) {
        const p = cache.ensure(q);
        calls[n++].resolve({ tag: stripKey(q) });
        await p;
    }
    const got2 = cache.get(want);
    assert.equal(got2.exact, false);
    assert.deepEqual(got2.payload, { tag: stripKey(fine) },
        '1024 vs 4096 for a 2048 request: equal log2 distance, finer wins');
});

// ── LRU eviction ────────────────────────────────────────────────────────────

test('LRU evicts the oldest strip beyond maxStrips', async () => {
    const { cache, calls } = wired({ maxStrips: 2 });
    // Three disjoint strips (windows far apart -> no overlap at all).
    const A = mkQ(1024, 10_000_000, 10_100_000);
    const B = mkQ(1024, 20_000_000, 20_100_000);
    const C = mkQ(1024, 30_000_000, 30_100_000);
    let n = 0;
    for (const q of [A, B, C]) {
        const p = cache.ensure(q);
        calls[n++].resolve({ tag: stripKey(q) });
        await p;
    }
    assert.equal(cache.size(), 2);
    assert.equal(cache.get(A), null, 'oldest evicted');
    assert.equal(cache.get(B).exact, true);
    assert.equal(cache.get(C).exact, true);
});

test('LRU: a get() touch protects a strip from eviction', async () => {
    const { cache, calls } = wired({ maxStrips: 2 });
    const A = mkQ(1024, 10_000_000, 10_100_000);
    const B = mkQ(1024, 20_000_000, 20_100_000);
    const C = mkQ(1024, 30_000_000, 30_100_000);
    let n = 0;
    for (const q of [A, B]) {
        const p = cache.ensure(q);
        calls[n++].resolve({ tag: stripKey(q) });
        await p;
    }
    cache.get(A);                                // touch A -> B is now oldest
    const p = cache.ensure(C);
    calls[n++].resolve({ tag: stripKey(C) });
    await p;
    assert.equal(cache.get(A).exact, true, 'touched strip survives');
    assert.equal(cache.get(B), null, 'untouched strip evicted');
});

// ── generation invalidation ─────────────────────────────────────────────────

test('invalidate discards all strips and drops in-flight results', async () => {
    const { cache, calls } = wired();
    const A = mkQ(1024, 10_000_000, 10_100_000);
    const B = mkQ(1024, 20_000_000, 20_100_000);
    const events = [];
    cache.onData(e => events.push(e));

    const pa = cache.ensure(A);
    calls[0].resolve({ tag: 'a' });
    await pa;
    assert.equal(events.length, 1);

    const pb = cache.ensure(B);                  // in flight across the bump
    cache.invalidate();                          // filters changed
    assert.equal(cache.get(A), null, 'cached strips gone');
    calls[1].resolve({ tag: 'b' });              // lands into the old generation
    const rb = await pb;
    assert.deepEqual(rb, { ok: false, stale: true });
    assert.equal(cache.get(B), null, 'old-generation arrival not stored');
    assert.equal(events.length, 1, 'no onData for a dropped arrival');

    // A re-ensure after the bump refetches under the new generation.
    const pb2 = cache.ensure(B);
    assert.equal(calls.length, 3);
    calls[2].resolve({ tag: 'b2' });
    await pb2;
    assert.deepEqual(cache.get(B).payload, { tag: 'b2' });
});

// ── latest-wins notification ────────────────────────────────────────────────

test('onData flags latest-wins; superseded arrivals are kept, not latest', async () => {
    const { cache, calls } = wired();
    const A = mkQ(1024, 10_000_000, 10_100_000);
    const B = mkQ(1024, 20_000_000, 20_100_000);
    const events = [];
    cache.onData(e => events.push(e));

    const pa = cache.ensure(A);
    const pb = cache.ensure(B);                  // B is now the latest interest
    calls[1].resolve({ tag: 'b' });              // newest lands first
    await pb;
    calls[0].resolve({ tag: 'a' });              // old one lands late
    await pa;

    assert.equal(events.length, 2);
    assert.equal(events[0].key, stripKey(B));
    assert.equal(events[0].latest, true, 'arrival for the newest ensure');
    assert.equal(events[1].key, stripKey(A));
    assert.equal(events[1].latest, false, 'superseded arrival flagged');
    assert.equal(cache.get(A).exact, true, 'stale-but-useful: late strip RETAINED');
    assert.equal(cache.get(B).exact, true);
});

test('onData unsubscribe stops delivery', async () => {
    const { cache, calls } = wired();
    const A = mkQ(1024, 10_000_000, 10_100_000);
    const B = mkQ(1024, 20_000_000, 20_100_000);
    const events = [];
    const off = cache.onData(e => events.push(e));
    const pa = cache.ensure(A);
    calls[0].resolve({ tag: 'a' });
    await pa;
    off();
    const pb = cache.ensure(B);
    calls[1].resolve({ tag: 'b' });
    await pb;
    assert.equal(events.length, 1);
});

// ── failure behavior ────────────────────────────────────────────────────────

test('a failed fetch resolves { ok:false, error }, caches nothing, and can retry', async () => {
    const { cache, calls } = wired();
    const A = mkQ(1024, 10_000_000, 10_100_000);
    const p1 = cache.ensure(A);
    calls[0].reject(new Error('window_too_large'));
    const r1 = await p1;                         // ensure NEVER rejects
    assert.equal(r1.ok, false);
    assert.match(r1.error.message, /window_too_large/);
    assert.equal(cache.get(A), null, 'failure cached nothing');

    const p2 = cache.ensure(A);                  // in-flight slot was freed
    assert.equal(calls.length, 2, 'retry issues a fresh fetch');
    calls[1].resolve({ tag: 'a' });
    await p2;
    assert.equal(cache.get(A).exact, true);
});

test('a synchronously-throwing fetchStrip resolves { ok:false, error }', async () => {
    const cache = new StripCache({ fetchStrip: () => { throw new Error('boom'); } });
    const r = await cache.ensure(mkQ(1024, 10_000_000, 10_100_000));
    assert.equal(r.ok, false);
    assert.match(r.error.message, /boom/);
});
