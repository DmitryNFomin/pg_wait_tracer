/* pgwt — view manager: enter/leave lifecycle + the response chokepoint.
 *
 * This is the single place that decides whether a fetched response is allowed
 * to render. It replaces the scattered `_refreshGen` re-checks in the old
 * app.js, where each async branch had to remember to re-validate before
 * painting (and several didn't, which is where the chaos races were).
 *
 * Model
 * -----
 * A *view* corresponds to a tab. Each view is an object:
 *
 *   { id,                        // string, matches the tab's data-tab
 *     requests(ctx),             // async: fetch + return raw `data`. Uses the
 *                                //   transport's channels (ctx.transport).
 *     build(data, ctx),          // PURE: data -> render model (no DOM/charts)
 *     mount(el, model, ctx),     // thin: paint model into el, wire callbacks
 *     enter(ctx),                // create chart instance(s), one-time setup
 *     leave() }                  // dispose chart instance(s) — MUST finish
 *                                //   before the next enter()
 *
 * Lifecycle guarantees
 * --------------------
 *   1. switchTo(id) calls the outgoing view's leave() (synchronously, so chart
 *      dispose completes) BEFORE the incoming view's enter(). No two views ever
 *      hold a chart instance for the same DOM node at once.
 *   2. Every refresh runs under an *epoch*. switchTo bumps the epoch. When a
 *      refresh's awaited requests() resolve, the manager checks the epoch is
 *      still current AND the view is still active before calling build()/mount().
 *      A response addressed to a superseded view/epoch is dropped here — the
 *      chokepoint — even though the view's own code re-checks nothing.
 *   3. transport channels are namespaced per view, so a new refresh of the same
 *      view supersedes the prior in-flight one (single-flight), and leaving a
 *      view cancels its channels.
 */

import { CancelledError } from './transport.js';

export class ViewManager {
    /* ctx is the shared services bag handed to every view hook:
     *   { transport, server, timeRange, filters, ...app callbacks }
     * The manager augments each call's ctx with `channel(name)` and the live
     * epoch so views never invent their own generation logic. */
    constructor(ctx) {
        this.ctx = ctx;
        this.views = {};          // id -> view object
        this.active = null;       // currently-entered view object
        this.epoch = 0;           // bumped on every switch / explicit refresh
        this.containerEl = null;  // el handed to mount()
        this._channels = new Set(); // channels opened by the active view
    }

    register(view) {
        this.views[view.id] = view;
        return this;
    }

    setContainer(el) { this.containerEl = el; }

    activeId() { return this.active ? this.active.id : null; }

    /* Switch to view `id`: leave the old view (dispose charts) fully, then
     * enter the new one, then refresh it. Bumps the epoch so any in-flight
     * response for the old view/epoch is dropped at the chokepoint. */
    async switchTo(id, refreshOpts) {
        const next = this.views[id];
        if (!next) throw new Error('unknown view: ' + id);

        // Invalidate everything in flight for the outgoing view.
        this.epoch++;
        this._cancelChannels();

        if (this.active) {
            try { this.active.leave(); } catch (e) { /* dispose best-effort */ }
        }

        this.active = next;
        try { next.enter(this._viewCtx()); } catch (e) { /* enter best-effort */ }

        await this.refresh(refreshOpts);
    }

    /* Re-fetch + re-render the active view under a fresh epoch. `opts.userInitiated`
     * is informational for the app (e.g. to stop auto-refresh); the chokepoint
     * fires regardless. (Still unwired — the app pauses live itself before any
     * switch/drill/zoom, P4.) Returns when the render (or drop) is done. */
    async refresh(opts) {
        if (!this.active) return;
        const view = this.active;
        const myEpoch = ++this.epoch;
        const ctx = this._viewCtx();

        let data;
        try {
            data = await view.requests(ctx);
        } catch (e) {
            if (e instanceof CancelledError) return;   // superseded: drop
            // Request failure (error envelope / disconnect / timeout, UI-1):
            // surface it to the app so it can show the degraded-transport
            // state — but only if this refresh is still the current one (a
            // stale epoch's failure is nobody's business, same rule as data).
            if (this.active === view && myEpoch === this.epoch &&
                this.ctx.onRequestError) {
                // P1: hand over the pane too, so a command-level error
                // (transport:false envelope) paints an error card there
                // instead of being silently dropped.
                try { this.ctx.onRequestError(e, this._pane(view)); }
                catch (e2) { /* best-effort */ }
            }
            return;
        }

        // THE CHOKEPOINT: only paint if this response still belongs to the
        // active view AND its epoch was not superseded by a newer refresh/switch.
        if (this.active !== view || myEpoch !== this.epoch) return;

        let model;
        try {
            model = view.build(data, ctx);
        } catch (e) {
            // P1: a build throw must be LOUD — console + per-pane error card —
            // never a silent stale pane.
            console.error('[pgwt] view build failed:', view.id, e);
            this._viewError(view, e);
            return;
        }
        if (this.active !== view || myEpoch !== this.epoch) return;

        try {
            view.mount(this.containerEl, model, ctx);
            this._clearPaneError();
        } catch (e) {
            console.error('[pgwt] view mount failed:', view.id, e);
            this._viewError(view, e);
        }
    }

    /* P1: the pane descriptor handed to the app's error hooks — which view
     * failed, where its error card should paint, and how to retry. */
    _pane(view) {
        return { view: view.id, el: this.containerEl,
                 retry: () => this.refresh() };
    }

    /* P1: report a build/mount throw to the app (it paints the error card). */
    _viewError(view, err) {
        if (!this.ctx.onViewError) return;
        try { this.ctx.onViewError(this._pane(view), err); }
        catch (e2) { /* best-effort */ }
    }

    /* P1: a successful mount clears any leftover error card in the pane.
     * Views that rebuild el.innerHTML clear it implicitly; shell-preserving
     * views (histogram) do not. No-op under Node tests (bare-object container). */
    _clearPaneError() {
        const el = this.containerEl;
        if (!el || !el.querySelector) return;
        const card = el.querySelector(':scope > .pane-error');
        if (card) card.remove();
    }

    /* Build the per-call ctx: the shared bag plus view-scoped helpers. */
    _viewCtx() {
        const id = this.active ? this.active.id : '_';
        const self = this;
        return Object.assign(Object.create(this.ctx), {
            viewId: id,
            epoch: this.epoch,
            /* Namespaced single-flight channel for this view. */
            channel(name) {
                const ch = id + '.' + name;
                self._channels.add(ch);
                return ch;
            },
            /* True while this view is still the active one. Views with their own
             * in-view async re-fetches (e.g. the histogram's selector change)
             * use this as the chokepoint, mirroring the manager's own check. */
            isActive() { return self.active && self.active.id === id; },
        });
    }

    _cancelChannels() {
        for (const ch of this._channels) this.ctx.transport.cancel(ch);
        this._channels.clear();
    }
}
