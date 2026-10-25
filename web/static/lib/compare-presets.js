/* B keeps A's duration. Its entire shifted window must fit the served trace,
 * including when live A ends a little beyond the latest retained sample.
 * Fractions of available history work even on sub-second captures (#549). */
export function comparePresetOffsets(from, to, traceFrom, traceTo) {
    if (![from, to, traceFrom, traceTo].every(Number.isFinite) ||
        from >= to || traceFrom >= traceTo) return [];
    const max = Math.min(Number.MAX_SAFE_INTEGER, Math.floor(from - traceFrom));
    const min = Math.max(1, Math.ceil(to - traceTo));
    if (max < min) return [];
    // Prefer readable whole seconds/milliseconds without introducing a
    // shortest preset: a shorter capture simply uses finer precision.
    return [...new Set([0.25, 0.5, 1].map(fraction => {
        const raw = max * fraction;
        const quantum = raw >= 86400e9 ? 86400e9 : raw >= 3600e9 ? 3600e9 :
            raw >= 60e9 ? 60e9 : raw >= 1e9 ? 1e9 : raw >= 1e6 ? 1e6 : 1;
        return -Math.max(min, Math.floor(raw / quantum) * quantum);
    }))].filter(offset => Number.isSafeInteger(offset) &&
        from + offset < from && from + offset >= traceFrom &&
        to + offset <= traceTo);
}
