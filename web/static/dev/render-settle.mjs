/* pgwt — deterministic render-settle bookkeeping for the dev gallery
 * (issue #155: the gallery snapshot gate is nondeterministic).
 *
 * Pure state machine, no DOM/echarts/uPlot references, so it is
 * unit-testable in plain Node (tests/web_unit/render-settle.test.mjs).
 * gallery.js wires this to real DOM attributes (cell.dataset.settled,
 * document.body.dataset.galleryReady) and real chart completion events
 * (echarts 'finished', uPlot 'draw'); this module only tracks "how many
 * renders are in flight" and "has the gallery fully settled at least once",
 * independent of what a "render" or "settle" event actually is.
 *
 * Why this exists: animation:false only removes SERIES transitions — the
 * zrender/echarts paint itself is still scheduled on requestAnimationFrame,
 * so the synchronous setOption() call that kicks off a render returns well
 * before the canvas is actually painted. A capture taken right after that
 * synchronous call races the paint; this is exactly the class of bug filed
 * as #155 (the same commit's gallery cells differed by up to 3.2% run to
 * run in CI). The fix is to wait for the chart's REAL completion signal
 * instead of "the render call returned".
 */

/** One chart/mount's pending-render flag — boolean, not a counter: repeated
 * begin() calls before a matching settle() collapse to ONE pending unit, so
 * a superseded re-render's eventual completion event (if it still fires) is
 * a harmless no-op rather than double-counting. */
export function createRenderTracker() {
    let pending = false;
    return {
        /** Marks a new render in flight. Returns true the FIRST time (the
         * caller should count this against a ready gate), false if a render
         * was already pending (nothing new to count). */
        begin() {
            if (pending) return false;
            pending = true;
            return true;
        },
        /** Marks the render complete. Returns true if it actually cleared a
         * pending flag (the caller should resolve it against a ready gate),
         * false if nothing was pending (a stray/duplicate completion event,
         * which must be ignored, not double-resolved). */
        settle() {
            if (!pending) return false;
            pending = false;
            return true;
        },
        get isPending() { return pending; },
    };
}

/** Tracks "every render kicked off during the initial synchronous pass has
 * actually completed". `markLoopDone()` corresponds to that pass returning;
 * `addPending()`/`resolvePending()` are begin()/settle() transitions that
 * ACTUALLY changed state (i.e. createRenderTracker() returned true — do not
 * call these for a no-op begin/settle). isReady goes true exactly once, the
 * first time both "loop done" and "nothing pending" hold, and never reverts:
 * later re-renders (tick replay) can push pendingCount back above 0, but
 * must not un-ready a gate the initial capture already waited on. */
export function createReadyGate() {
    let pendingCount = 0;
    let loopDone = false;
    let ready = false;
    return {
        addPending() {
            pendingCount += 1;
        },
        resolvePending() {
            pendingCount -= 1;
            if (loopDone && pendingCount === 0) ready = true;
        },
        markLoopDone() {
            loopDone = true;
            if (pendingCount === 0) ready = true;
        },
        get pendingCount() { return pendingCount; },
        get isReady() { return ready; },
    };
}
