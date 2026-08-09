/* pgwt — pure compare-table math (Track U Phase U4, D3).
 *
 * Joins the same aggregate command's A/B payloads by stable entity identity,
 * computes DB-time deltas, and applies the honest change floor before the
 * shared table renderer sees any rows. No DOM/network/state lives here.
 */

const VALUE_KEY = {
    overview: 'ms',
    events: 'total_ms',
    queries: 'total_ms',
};

function finiteMs(value) {
    const n = Number(value);
    return Number.isFinite(n) && n >= 0 ? n : 0;
}

function entityKey(kind, row) {
    if (kind === 'events') return 'event:' + String(row.event_id);
    if (kind === 'queries') return 'query:' + String(row.query_id);
    // Overview can contain the same event/class spelling at different levels.
    return 'overview:' + String(row.indent || 0) + ':' + String(row.name || '');
}

function entityLabel(kind, row) {
    if (kind === 'queries') return String(row.query_id);
    return String(row.name || '');
}

function dbTimeMs(data) {
    if (data && Number.isFinite(Number(data.db_time_ms))) {
        return finiteMs(data.db_time_ms);
    }
    const db = ((data && data.rows) || []).find(r =>
        r && r.indent === 0 && r.name === 'DB Time');
    return db ? finiteMs(db.ms) : 0;
}

/* Returns a render-neutral comparison model:
 *   rows: joined entities with a_ms/b_ms/delta_ms/abs_delta_ms/ratio
 *   floor: max(1% of the larger A/B window DB-time, 50 ms)
 *   belowFloor: exact count of joined entities omitted by that rule
 *
 * One-sided entities carry status='new'|'gone' and ratio=null. A zero divisor
 * also yields null: no path manufactures Infinity for display or sorting. */
export function buildDeltaComparison(kind, dataA, dataB, opts) {
    const valueKey = VALUE_KEY[kind];
    if (!valueKey) throw new Error('unknown compare table kind: ' + kind);
    opts = opts || {};

    // Retention absence is not a zero-valued baseline. Returning no joined
    // rows prevents the table from falsely labeling every A entity as new.
    if (opts.baselineUnavailable) {
        return {
            rows: [], floorMs: 0, belowFloor: 0, truncation: null,
            dbTimeA: dbTimeMs(dataA), dbTimeB: null,
            baselineUnavailable: true,
        };
    }

    const aRows = (dataA && dataA.rows) || [];
    const bRows = (dataB && dataB.rows) || [];
    const aMap = new Map(aRows.map(r => [entityKey(kind, r), r]));
    const bMap = new Map(bRows.map(r => [entityKey(kind, r), r]));
    const keys = new Set([...aMap.keys(), ...bMap.keys()]);
    const floorMs = Math.max(0.01 * Math.max(dbTimeMs(dataA), dbTimeMs(dataB)), 50);

    const rows = [];
    let belowFloor = 0;
    for (const key of keys) {
        const a = aMap.get(key) || null;
        const b = bMap.get(key) || null;
        const source = a || b;
        const aMs = a ? finiteMs(a[valueKey]) : 0;
        const bMs = b ? finiteMs(b[valueKey]) : 0;
        const deltaMs = aMs - bMs;
        const absDeltaMs = Math.abs(deltaMs);
        if (absDeltaMs < floorMs) {
            belowFloor++;
            continue;
        }
        const ratio = aMs > 0 && bMs > 0 ? aMs / bMs : null;
        const status = a && !b ? 'new' : (!a && b ? 'gone' : null);
        rows.push(Object.assign({}, source, {
            _entityKey: key,
            _entityLabel: entityLabel(kind, source),
            _a: a,
            _b: b,
            a_ms: aMs,
            b_ms: bMs,
            delta_ms: deltaMs,
            abs_delta_ms: absDeltaMs,
            ratio,
            status,
        }));
    }

    return {
        rows,
        floorMs,
        belowFloor,
        dbTimeA: dbTimeMs(dataA),
        dbTimeB: dbTimeMs(dataB),
        baselineUnavailable: false,
        truncation: belowFloor > 0 ? {
            omitted: belowFloor,
            text: '… ' + belowFloor.toLocaleString() +
                ' entities below the change floor',
        } : null,
    };
}

export function defaultDeltaSort(sort) {
    const allowed = { abs_delta_ms: true, ratio: true, a_ms: true };
    return sort && allowed[sort.key]
        ? sort : { key: 'abs_delta_ms', asc: false };
}
