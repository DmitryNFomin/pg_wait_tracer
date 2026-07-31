/* pgwt — WebSocket transport with request ids and single-flight per channel.
 *
 * Replaces app.js's ad-hoc `_refreshGen` generation counters. The old code
 * guarded against stale responses by stamping a global generation number on
 * every user action and re-checking it before each renderer ran — fragile,
 * because every async branch had to remember to re-check, and several did not
 * (the histogram/timeline/transitions branches), which is exactly where the
 * chaos races lived.
 *
 * Here, superseding is structural:
 *
 *   - Every request carries a unique id and belongs to a named *channel*
 *     (e.g. "overview.table", "overview.chart"). A channel models "the one
 *     in-flight request whose answer this slot cares about".
 *   - request(channel, cmd, params) cancels any pending request on the same
 *     channel before issuing the new one. The cancelled request's promise
 *     rejects with a CancelledError and its eventual response is dropped when
 *     it arrives (its id is no longer the channel's current id).
 *   - send(cmd, params) is the channel-less escape hatch (e.g. one-off `info`
 *     polls) — no superseding, just a plain request id.
 *
 * The view-manager layers a second, coarser guard on top (drop responses for a
 * view that is no longer active); transport handles the fine-grained
 * "newest-request-on-this-channel-wins".
 */

export class CancelledError extends Error {
    constructor(msg) { super(msg || 'cancelled'); this.name = 'CancelledError'; }
}

/* A request that failed instead of returning data (Trust Milestone UI-1).
 * Raised for error envelopes ({"id":N,"error":"..."}), local timeouts, and
 * disconnects. `transport` is true when the failure is connection-level (the
 * bridge lost the SSH/server pipe, the socket closed, we are not connected)
 * as opposed to a command-level error from a healthy server (e.g. the control
 * proxy's "daemon not running"). Consumers use this flag to distinguish
 * "transport degraded" (show the connection-lost state) from "this particular
 * command failed" (degrade just that feature). Command-level envelopes may
 * additionally carry `cmd` (the request's command) and the server's structured
 * fields `code` / `hint` / `maxEvents` (DUR-9 window_too_large) so the client
 * can present the refusal (P1). */
export class TransportError extends Error {
    constructor(msg, opts) {
        super(msg || 'request failed');
        this.name = 'TransportError';
        this.transport = !!(opts && opts.transport);
    }
}

export class Transport {
    constructor() {
        this.ws = null;
        this.nextId = 1;
        this.pending = {};        // id -> { resolve, reject, timer, channel, cmd }
        this.channelId = {};      // channel -> current (latest) request id
        this.timeoutMs = 30000;
    }

    /* Wire up an open WebSocket. Caller owns open/close/reconnect lifecycle and
     * calls attach() on each fresh connection. */
    attach(ws) {
        this.ws = ws;
        ws.addEventListener('message', (e) => this._onMessage(e));
    }

    _onMessage(e) {
        let data;
        try { data = JSON.parse(e.data); } catch { return; }
        const p = this.pending[data.id];
        if (!p) return;             // unknown / already-superseded id: drop
        clearTimeout(p.timer);
        delete this.pending[data.id];
        if (p.channel && this.channelId[p.channel] === data.id) {
            delete this.channelId[p.channel];
        }
        /* UI-1: an error envelope is a FAILED request, never data. The bridge
         * tags its own (connection-level) envelopes with "transport":true;
         * everything else is a command-level error from a live server. */
        if (typeof data.error === 'string' && data.error !== '') {
            const err = new TransportError(data.error, { transport: !!data.transport });
            // P1: keep the structured error fields the server emitted exactly
            // so the client can present them (server.c reject_overload).
            err.cmd = p.cmd;
            if (typeof data.code === 'string') err.code = data.code;
            if (typeof data.hint === 'string') err.hint = data.hint;
            if (typeof data.max_events === 'number') err.maxEvents = data.max_events;
            p.reject(err);
            return;
        }
        p.resolve(data);
    }

    isOpen() {
        return this.ws && this.ws.readyState === WebSocket.OPEN;
    }

    /* Low-level request without a channel — no superseding. */
    send(cmd, params) {
        return this._issue(cmd, params, null);
    }

    /* Single-flight request on `channel`: supersedes any pending request on the
     * same channel (its promise rejects with CancelledError; its late response
     * is dropped on arrival). */
    request(channel, cmd, params) {
        this.cancel(channel);
        return this._issue(cmd, params, channel);
    }

    /* Cancel the pending request (if any) on `channel`. */
    cancel(channel) {
        const id = this.channelId[channel];
        if (id == null) return;
        const p = this.pending[id];
        delete this.channelId[channel];
        if (p) {
            clearTimeout(p.timer);
            delete this.pending[id];
            p.reject(new CancelledError('superseded: ' + channel));
        }
    }

    _issue(cmd, params, channel) {
        if (!this.isOpen()) {
            return Promise.reject(new TransportError('not connected', { transport: true }));
        }
        const id = this.nextId++;
        const msg = JSON.stringify({ id, cmd, ...params });
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                delete this.pending[id];
                if (channel && this.channelId[channel] === id) {
                    delete this.channelId[channel];
                }
                reject(new TransportError('timeout', { transport: true }));
            }, this.timeoutMs);
            this.pending[id] = { resolve, reject, timer, channel, cmd };
            if (channel) this.channelId[channel] = id;
            this.ws.send(msg);
        });
    }

    /* Reject everything in flight (called on disconnect). */
    rejectAll(reason) {
        for (const id of Object.keys(this.pending)) {
            const p = this.pending[id];
            clearTimeout(p.timer);
            p.reject(new TransportError(reason, { transport: true }));
        }
        this.pending = {};
        this.channelId = {};
    }
}
