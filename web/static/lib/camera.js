/* pgwt — the camera: the AAS time window as pure client state (Track U, U2a).
 *
 * docs/INSTRUMENT_ARCHITECTURE.md §1: in the instrument model, "where am I
 * looking" is two numbers mutated at input speed — the server is never inside
 * the pan/zoom gesture loop. This module is that state plus the arithmetic:
 * cursor-anchored zoom, pan, the follow/detached live state machine, and the
 * power-of-2 resolution ladder whose quantized output keys the strip cache
 * (lib/stripcache.js). Pure: no DOM, no timers, no network — Node-tested
 * exactly like lib/selection.js.
 *
 * Time representation — float64 ABSOLUTE nanoseconds, deliberately not BigInt
 * and not split hi/lo. At 2026 epochs (~1.77e18 ns) the float64 ulp is 256 ns,
 * three orders of magnitude below the finest honest data floor (~100 ms sample
 * period in sampled tiers; MIN_SPAN_NS = 1 ms camera floor), so gesture math
 * is tolerance-safe. Where exactness DOES matter — strip cache keys must not
 * drift — it holds by construction: resNs is a power of two >= 256, and
 * scaling a float64 by a power of two only shifts the exponent, so the
 * floor/ceil alignment in quantize() is bit-exact. BigInt would force a
 * conversion at every fractional multiply (frac, factor) and buy nothing
 * below the 256 ns ulp. (This is §1's numeric footnote resolved: accept the
 * ulp, pin exactness where keys need it.)
 *
 * Live semantics (the project's live-means-NOW rule, structurally enforced):
 *   follow    right edge glued to NOW; followTick(nowNs) slides the window.
 *   detached  any pan/zoom/setWindow gesture detaches — a live tick can then
 *             NEVER re-anchor the view out from under an investigation.
 *   attachFollow(nowNs) is the only way back: it re-anchors the right edge to
 *             the caller's NOW (never a stale edge) and preserves the span.
 */

export const MIN_SPAN_NS = 1e6;                  // 1 ms window floor (default)
export const MAX_SPAN_NS = 30 * 86400 * 1e9;     // 30 days (default)
export const MIN_RES_NS = 256;                   // float64 ulp at 2026 epochs
export const DEFAULT_TARGET_BUCKETS = 300;
const SKIRT_SPANS = 1;                           // 1 span each side => 3x strip

export class Camera {
    /* opts: { fromNs, toNs, mode:'follow'|'detached', minSpanNs, maxSpanNs }.
     * The initial window is normalized through the same span clamp as
     * setWindow, so span ∈ [minSpanNs, maxSpanNs] is a class invariant. */
    constructor(opts) {
        opts = opts || {};
        this.minSpanNs = opts.minSpanNs || MIN_SPAN_NS;
        this.maxSpanNs = opts.maxSpanNs || MAX_SPAN_NS;
        this.mode = opts.mode === 'follow' ? 'follow' : 'detached';
        this._subs = [];
        const w = this._clampWindow(opts.fromNs || 0, opts.toNs || 0);
        this.fromNs = w.from;
        this.toNs = w.to;
    }

    span() { return this.toNs - this.fromNs; }

    snapshot() { return { fromNs: this.fromNs, toNs: this.toNs, mode: this.mode }; }

    /* Subscribe to state changes. cb receives { fromNs, toNs, mode, cause }
     * with cause ∈ 'zoom'|'pan'|'set'|'tick'|'attach'|'detach'. Fires only on
     * actual change (a no-op gesture is silent). Returns unsubscribe(). */
    onChange(cb) {
        this._subs.push(cb);
        return () => { this._subs = this._subs.filter(f => f !== cb); };
    }

    /* Cursor-anchored zoom (the "TradingView 10 lines", §1). frac is the
     * cursor x as 0..1 of the current window; factor > 1 zooms IN (span
     * shrinks), factor < 1 zooms out. The timestamp under the cursor stays
     * pinned under the cursor; span clamps to [minSpanNs, maxSpanNs] with the
     * anchor still pinned. DETACHES (a gesture is an investigation). */
    zoomAt(frac, factor) {
        if (!isFinite(factor) || !(factor > 0)) return false;
        frac = Math.min(1, Math.max(0, +frac || 0));
        const span = this.span();
        const anchor = this.fromNs + frac * span;
        let newSpan = span / factor;
        newSpan = Math.min(Math.max(newSpan, this.minSpanNs), this.maxSpanNs);
        const from = anchor - frac * newSpan;
        return this._apply(from, from + newSpan, 'detached', 'zoom');
    }

    /* Pan by a fraction of the current span (positive = forward in time).
     * DETACHES. */
    panByFrac(dFrac) {
        if (!isFinite(dFrac)) return false;
        const shift = dFrac * this.span();
        return this._apply(this.fromNs + shift, this.toNs + shift, 'detached', 'pan');
    }

    /* Jump to an explicit window (e.g. brush-select). Unordered input is
     * sorted; span is clamped around the midpoint. DETACHES. */
    setWindow(from, to) {
        const w = this._clampWindow(+from, +to);
        return this._apply(w.from, w.to, 'detached', 'set');
    }

    /* Live tick in follow mode: right edge = nowNs, span preserved.
     * STRICT NO-OP when detached — the 5 s tick must never re-anchor the
     * user (the has_closed_data / "last 15 min means NOW" bug class).
     * Out-of-order ticks (nowNs <= current right edge) are ignored. */
    followTick(nowNs) {
        if (this.mode !== 'follow') return false;
        if (!isFinite(nowNs) || !(nowNs > this.toNs)) return false;
        const span = this.span();
        return this._apply(nowNs - span, nowNs, 'follow', 'tick');
    }

    /* Re-attach live: anchor the right edge to the caller's NOW, preserving
     * span. Live means NOW — re-attach must never show a stale right edge,
     * so this always re-anchors regardless of where the window was. */
    attachFollow(nowNs) {
        if (!isFinite(nowNs)) return false;
        const span = this.span();
        return this._apply(nowNs - span, nowNs, 'follow', 'attach');
    }

    detach() {
        return this._apply(this.fromNs, this.toNs, 'detached', 'detach');
    }

    /* Power-of-2 resolution ladder (Perfetto-style cache keys). Returns
     *   {
     *     resNs,       // power-of-2 ns bucket width: the largest power of
     *                  // two <= span/targetBuckets (>= MIN_RES_NS), so the
     *                  // window holds [targetBuckets, 2*targetBuckets)
     *                  // buckets at this resolution
     *     stripFrom,   // window expanded by one span on each side (3x
     *     stripTo,     // skirt), aligned OUTWARD to resNs boundaries —
     *                  // bit-exact in float64 (see header)
     *     buckets,     // strip bucket count, exact integer
     *     winFromNs,   // the raw camera window this strip was derived from
     *     winToNs,     //   (stripcache scores overlap against it)
     *   }
     * Pure and phase-stable: the same window always maps to the same strip;
     * a pan by exactly k*resNs shifts the strip by exactly k*resNs. */
    quantize(targetBuckets) {
        const target = Math.max(1, Math.floor(targetBuckets || DEFAULT_TARGET_BUCKETS));
        const span = this.span();
        const ideal = span / target;
        let res = 2 ** Math.floor(Math.log2(ideal));
        if (res * 2 <= ideal) res *= 2;          // guard Math.log2 misrounding
        else if (res > ideal) res /= 2;          // (exact result either way)
        if (!(res >= MIN_RES_NS)) res = MIN_RES_NS;
        const stripFrom = Math.floor((this.fromNs - SKIRT_SPANS * span) / res) * res;
        const stripTo = Math.ceil((this.toNs + SKIRT_SPANS * span) / res) * res;
        return {
            resNs: res,
            stripFrom,
            stripTo,
            buckets: Math.round((stripTo - stripFrom) / res),
            winFromNs: this.fromNs,
            winToNs: this.toNs,
        };
    }

    /* Normalize [from,to]: sort, then clamp span into [minSpanNs, maxSpanNs]
     * growing/shrinking around the midpoint. */
    _clampWindow(from, to) {
        if (to < from) { const t = from; from = to; to = t; }
        const span = to - from;
        const clamped = Math.min(Math.max(span, this.minSpanNs), this.maxSpanNs);
        if (clamped !== span) {
            const mid = from + span / 2;
            from = mid - clamped / 2;
            to = from + clamped;
        }
        return { from, to };
    }

    /* Single mutation point: apply state, notify subscribers iff something
     * actually changed. Returns whether it did. */
    _apply(from, to, mode, cause) {
        if (from === this.fromNs && to === this.toNs && mode === this.mode) {
            return false;
        }
        this.fromNs = from;
        this.toNs = to;
        this.mode = mode;
        const snap = this.snapshot();
        snap.cause = cause;
        for (const cb of this._subs) cb(snap);
        return true;
    }
}
