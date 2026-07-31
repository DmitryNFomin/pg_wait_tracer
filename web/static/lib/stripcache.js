/* pgwt — client-side strip cache: the tile store of the instrument model
 * (Track U, U2a; docs/INSTRUMENT_ARCHITECTURE.md §1/§3 row 2).
 *
 * Strips of AAS buckets keyed by their power-of-2 quantization (the object
 * lib/camera.js quantize() returns), retained STALE-BUT-USEFUL: a superseded
 * strip keeps drawing — stretched/coarse — under the camera until the finer
 * one lands (the Perfetto trick the dashboard transport deliberately
 * prevents with single-flight cancel + epoch discard). Cache fills therefore
 * must arrive via the transport's non-superseding send() path
 * (transport.js:88-90); that choice lives in the injected fetchStrip — this
 * module is pure policy: no timers, no network, no DOM, and its only side
 * effect is calling fetchStrip.
 *
 * Contract summary (the U2a wiring consumes these verbatim):
 *   new StripCache({ fetchStrip, maxStrips })
 *       fetchStrip({fromNs,toNs,buckets}) -> Promise of the aas payload.
 *   get(q)     sync lookup: exact strip, else best overlapping stale strip.
 *   ensure(q)  deduped background fetch; never rejects; onData on arrival.
 *   onData(cb) cb({ key, quantized, payload, latest }) — latest-wins flag.
 *   invalidate()  generation bump: discards all strips AND all in-flight
 *                 results (used when filters change — old-filter data must
 *                 never be served under new filters).
 */

/* Cache key of a quantized strip. Generation is NOT part of the key — the
 * cache is emptied on invalidate(), so keys never cross generations. */
export function stripKey(q) {
    return q.resNs + ':' + q.stripFrom + ':' + q.stripTo;
}

export class StripCache {
    constructor(opts) {
        opts = opts || {};
        if (typeof opts.fetchStrip !== 'function') {
            throw new Error('StripCache requires fetchStrip');
        }
        this.fetchStrip = opts.fetchStrip;
        this.maxStrips = opts.maxStrips || 16;
        this.generation = 0;
        this.strips = new Map();      // key -> { q, payload }; Map order = LRU
        this._inflight = new Map();   // key -> envelope promise (dedup)
        this._latestKey = null;       // key of the most recent ensure()
        this._subs = [];
    }

    size() { return this.strips.size; }

    /* Subscribe to strip arrivals. cb({ key, quantized, payload, latest });
     * `latest` is true iff this arrival is for the most recent ensure() —
     * consumers should re-get() their CURRENT quantization on any event (an
     * older overlapping strip can still refine an empty view), and may use
     * `latest` to skip work for superseded arrivals. Returns unsubscribe(). */
    onData(cb) {
        this._subs.push(cb);
        return () => { this._subs = this._subs.filter(f => f !== cb); };
    }

    /* Synchronous, never fetches (pair with ensure() to refine). Returns
     *   { payload, exact:true,  resNs, stripFrom, stripTo, buckets }  exact hit
     *   { payload, exact:false, resNs, stripFrom, stripTo, buckets }  best
     *     stale/coarser strip overlapping the camera window — resNs/stripFrom/
     *     stripTo/buckets describe the SERVING strip (the renderer needs its
     *     real geometry to draw it stretched in the right place)
     *   null  nothing cached overlaps the window.
     * Best-pick order: window coverage desc, then |log2 resNs ratio| asc,
     * then finer first — deterministic. */
    get(quantized) {
        const key = stripKey(quantized);
        const hit = this.strips.get(key);
        if (hit) {
            this._touch(key, hit);
            return this._serve(hit, true);
        }
        const winFrom = quantized.winFromNs != null ? quantized.winFromNs : quantized.stripFrom;
        const winTo = quantized.winToNs != null ? quantized.winToNs : quantized.stripTo;
        let best = null;
        let bestKey = null;
        for (const [k, s] of this.strips) {
            const cover = Math.min(s.q.stripTo, winTo) - Math.max(s.q.stripFrom, winFrom);
            if (cover <= 0) continue;
            if (best !== null) {
                const bCover = Math.min(best.q.stripTo, winTo) - Math.max(best.q.stripFrom, winFrom);
                if (cover < bCover) continue;
                if (cover === bCover) {
                    const dNew = Math.abs(Math.log2(s.q.resNs / quantized.resNs));
                    const dBest = Math.abs(Math.log2(best.q.resNs / quantized.resNs));
                    if (dNew > dBest) continue;
                    if (dNew === dBest && s.q.resNs >= best.q.resNs) continue;
                }
            }
            best = s;
            bestKey = k;
        }
        if (!best) return null;
        this._touch(bestKey, best);
        return this._serve(best, false);
    }

    /* Ensure the exact strip for `quantized` is (or will be) cached. Deduped
     * by strip key while in flight. Returns a promise that NEVER rejects
     * (safe to fire-and-forget):
     *   { ok:true,  payload, cached:true }  already cached — no fetch
     *   { ok:true,  payload }               fetched, stored, onData fired
     *   { ok:false, stale:true }            landed after invalidate(); dropped
     *   { ok:false, error }                 fetchStrip failed; nothing cached,
     *                                       in-flight slot freed (retryable). */
    ensure(quantized) {
        const key = stripKey(quantized);
        this._latestKey = key;
        const hit = this.strips.get(key);
        if (hit) {
            this._touch(key, hit);
            return Promise.resolve({ ok: true, payload: hit.payload, cached: true });
        }
        const pending = this._inflight.get(key);
        if (pending) return pending;

        const gen = this.generation;
        const buckets = quantized.buckets != null
            ? quantized.buckets
            : Math.round((quantized.stripTo - quantized.stripFrom) / quantized.resNs);
        let raw;
        try {
            raw = this.fetchStrip({
                fromNs: quantized.stripFrom,
                toNs: quantized.stripTo,
                buckets,
            });
        } catch (error) {
            return Promise.resolve({ ok: false, error });
        }
        const p = Promise.resolve(raw).then(
            (payload) => {
                if (this._inflight.get(key) === p) this._inflight.delete(key);
                if (gen !== this.generation) return { ok: false, stale: true };
                this._store(key, quantized, payload);
                const ev = { key, quantized, payload, latest: key === this._latestKey };
                for (const cb of this._subs) cb(ev);
                return { ok: true, payload };
            },
            (error) => {
                if (this._inflight.get(key) === p) this._inflight.delete(key);
                return { ok: false, error };
            },
        );
        this._inflight.set(key, p);
        return p;
    }

    /* Generation bump: every cached strip is discarded and every in-flight
     * result is dropped on arrival (its stamped generation no longer
     * matches). Call when the data under the strips changes meaning —
     * filter/drill changes, breakdown-mode flips. */
    invalidate() {
        this.generation++;
        this.strips.clear();
        this._inflight.clear();
        this._latestKey = null;
    }

    _serve(s, exact) {
        return {
            payload: s.payload,
            exact,
            resNs: s.q.resNs,
            stripFrom: s.q.stripFrom,
            stripTo: s.q.stripTo,
            buckets: s.q.buckets,
        };
    }

    /* Move to the recently-used end (Map iteration order = insertion order). */
    _touch(key, s) {
        this.strips.delete(key);
        this.strips.set(key, s);
    }

    _store(key, q, payload) {
        this.strips.delete(key);
        this.strips.set(key, { q, payload });
        while (this.strips.size > this.maxStrips) {
            const oldest = this.strips.keys().next().value;
            this.strips.delete(oldest);
        }
    }
}
