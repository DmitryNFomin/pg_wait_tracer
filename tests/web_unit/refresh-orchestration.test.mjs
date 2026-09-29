/* Node unit tests for lib/refresh-orchestration.js (issue #192).
 *
 * app.js's refresh() used to `await refreshActive()` (the AAS pane fetch)
 * before starting `vm.refresh()` (the active tab), so every tab's data
 * landed 0.5-1.7s after the AAS chart above it, every tick. These pin:
 *   - both calls start before EITHER settles (the actual defect: a
 *     sequential `await a(); await b();` would never observe b starting
 *     before a resolves)
 *   - the combinator still waits for both to finish before resolving
 *   - a rejection from either side is not swallowed
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runConcurrentRefresh } from '../../web/static/lib/refresh-orchestration.js';

function deferred() {
    let resolve, reject;
    const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
    return { promise, resolve, reject };
}

test('runConcurrentRefresh starts the tab fetch before the AAS fetch settles', async () => {
    const log = [];
    const aas = deferred();
    const tab = deferred();
    const refreshAas = () => {
        log.push('aas:start');
        return aas.promise.then(() => log.push('aas:done'));
    };
    const refreshTab = () => {
        log.push('tab:start');
        return tab.promise.then(() => log.push('tab:done'));
    };

    const p = runConcurrentRefresh(refreshAas, refreshTab);

    // Both must have been invoked already, before either has resolved. Under
    // the old sequential code (`await refreshAas(); await refreshTab();`)
    // 'tab:start' would not appear here at all yet -- it would only be
    // logged after 'aas:done'.
    assert.deepEqual(log, ['aas:start', 'tab:start']);

    // Resolve the tab fetch FIRST -- it must not be blocked on the AAS one.
    tab.resolve();
    await Promise.resolve();
    await Promise.resolve();
    assert.deepEqual(log, ['aas:start', 'tab:start', 'tab:done']);

    aas.resolve();
    await p;
    assert.deepEqual(log, ['aas:start', 'tab:start', 'tab:done', 'aas:done']);
});

test('runConcurrentRefresh waits for both sides before resolving', async () => {
    const aas = deferred();
    const tab = deferred();
    let settled = false;
    const p = runConcurrentRefresh(() => aas.promise, () => tab.promise)
        .then(() => { settled = true; });

    aas.resolve();
    await Promise.resolve();
    await Promise.resolve();
    assert.equal(settled, false, 'must not resolve until the tab side is done too');

    tab.resolve();
    await p;
    assert.equal(settled, true);
});

test('runConcurrentRefresh does not swallow a rejection', async () => {
    const boom = new Error('boom');
    await assert.rejects(
        () => runConcurrentRefresh(() => Promise.resolve(), () => Promise.reject(boom)),
        /boom/,
    );
});
