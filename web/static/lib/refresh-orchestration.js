/* pgwt — live-view refresh orchestration (issue #192).
 *
 * One refresh tick paints two independent things: the persistent AAS pane
 * and the active tab view. They share no data dependency — each derives its
 * window from the same `timeRange` object already, not from the other's
 * response (issue #192's own review point) — so there is no reason to make
 * one wait on the other's round trip. Running them concurrently removes the
 * 0.5-1.7s lag where a viewer sees the AAS chart update and then watches the
 * tab below it catch up a tick later.
 *
 * Both `refreshAas` and `refreshTab` are expected to handle their own
 * failures internally (app.js's refreshActive() and ViewManager.refresh()
 * both catch and surface errors via onRequestError/onViewError rather than
 * rejecting) so neither call starves or is starved by the other. This
 * function still uses Promise.all rather than allSettled: if that
 * assumption is ever violated and one call starts rejecting, the caller
 * should see it rather than have it silently swallowed.
 */
export async function runConcurrentRefresh(refreshAas, refreshTab) {
    await Promise.all([refreshAas(), refreshTab()]);
}
