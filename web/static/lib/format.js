/* pgwt — pure formatting + color helpers (no DOM, no globals).
 *
 * Extracted verbatim from the old app.js so builders (which run in Node for
 * unit tests) can format values without importing the whole app. Behavior is
 * byte-identical to the originals — the Playwright "exact value" tests pin it.
 */

// -- Wait class colors (Oracle ASH / RDS PI inspired) -------------------------

// The CPU *group* keeps its historical meaning — running plus waiting for a
// processor (the Oracle ASH / RDS Performance Insights convention, and what a
// database engineer assumes "CPU" means) — see docs/AAS_SEMANTICS_DECISION.md.
// Where we can measure the split (the exact tier), it renders as two bands:
// 'cpu' (running) and 'offcpu' (ready but waiting for a core). 'offcpu' is
// appended at the END of this array, NOT spliced in next to 'cpu': array
// POSITION here doubles as the wire-format class index (PGWT_NUM_CLASSES=11,
// src/compute.h) that web/static/lib/builders/timeline.js and lib/table.js's
// stackedBar() index into directly (WAIT_CLASSES[classIdx]) — 'offcpu' is not
// one of those 11 real wait_event classes (it is a residual computed
// server-side, never a per-span wait_event_info value), so inserting it
// anywhere but the end would silently relabel every class after it. The AAS
// stacked-area builders (aas.js, uplot-aas.js) that DO want 'offcpu' drawn
// adjacent to 'cpu' use AAS_CLASS_ORDER below instead of this array's order.
// Owner's colour decision (#187 review): dark green for the ordinary state
// ("CPU (running)") and an acid/electric green for the state worth noticing
// ("CPU (waiting for a core)" — queued for a processor). Both sit in the
// same ~113-142° green hue family (29° apart — a clearly related pair, not
// a hue-only distinction) but 0.31 apart in HSL lightness (0.29 vs 0.60) so
// the boundary reads even to someone with reduced green discrimination who
// relies on lightness alone, not just hue — the WCAG relative-luminance
// contrast between the two swatches themselves is ~4.0:1. Checked against
// every other WAIT_CLASSES hue: the nearest neighbor is 'bufferpin' (171°),
// 29° from the dark green and 59° from the electric one — no collision.
// Against the app's dark theme background (#1a1a2e, style.css): the dark
// green's own contrast ratio (~3.1:1) matches 'io' (3.5:1, the previous
// lowest in the palette) so it stays visible rather than vanishing into the
// background; the electric green's ratio (~12.2:1) is among the highest in
// the palette, by design — it is the one meant to jump out.
export const WAIT_CLASSES = [
    { key: 'cpu',       label: 'CPU (running)', color: 'rgb(28,120,62)' },
    { key: 'io',        label: 'IO',        color: 'rgb(30,100,255)' },
    { key: 'lock',      label: 'Lock',      color: 'rgb(255,85,85)' },
    { key: 'lwlock',    label: 'LWLock',    color: 'rgb(255,121,198)' },
    { key: 'ipc',       label: 'IPC',       color: 'rgb(0,200,255)' },
    { key: 'client',    label: 'Client',    color: 'rgb(255,220,100)' },
    { key: 'timeout',   label: 'Timeout',   color: 'rgb(255,165,0)' },
    { key: 'bufferpin', label: 'BufferPin', color: 'rgb(0,210,180)' },
    { key: 'activity',  label: 'Activity',  color: 'rgb(150,100,255)' },
    { key: 'extension', label: 'Extension', color: 'rgb(190,150,255)' },
    { key: 'unknown',   label: 'Unknown',   color: 'rgb(180,180,180)' },
    { key: 'offcpu',    label: 'CPU (waiting for a core)', color: 'rgb(79,250,56)' },
];

export const CLASS_COLOR_MAP = {};
WAIT_CLASSES.forEach(c => {
    CLASS_COLOR_MAP[c.label.toLowerCase()] = c.color;
    CLASS_COLOR_MAP[c.key] = c.color;
});

// Stacking/legend order for the AAS stacked-area charts ONLY (aas.js,
// uplot-aas.js): 'offcpu' immediately follows 'cpu' so the two bands paint as
// one adjacent group, unlike WAIT_CLASSES' wire-format-pinned order above
// (where 'offcpu' must stay last). Every other consumer (timeline.js,
// table.js) keeps using WAIT_CLASSES directly.
export const AAS_CLASS_ORDER = (() => {
    const byKey = {};
    WAIT_CLASSES.forEach(c => { byKey[c.key] = c; });
    return ['cpu', 'offcpu', 'io', 'lock', 'lwlock', 'ipc', 'client',
            'timeout', 'bufferpin', 'activity', 'extension', 'unknown']
        .map(k => byKey[k]);
})();

export function classColor(name) {
    if (!name) return null;
    const lower = name.toLowerCase();
    if (CLASS_COLOR_MAP[lower]) return CLASS_COLOR_MAP[lower];
    const colon = name.indexOf(':');
    if (colon > 0) {
        const cls = name.substring(0, colon).toLowerCase();
        if (CLASS_COLOR_MAP[cls]) return CLASS_COLOR_MAP[cls];
    }
    const stripped = lower.replace('*', '');
    if (CLASS_COLOR_MAP[stripped]) return CLASS_COLOR_MAP[stripped];
    return null;
}

/* Index of a class — or of a "Class:Event" name's class prefix — in
 * WAIT_CLASSES, or -1 when unrecognized. This is the canonical class order;
 * per-event series stack in it (U1 stable stack order). */
export function classIndex(name) {
    if (!name) return -1;
    const lower = String(name).toLowerCase();
    const colon = lower.indexOf(':');
    const cls = (colon > 0 ? lower.substring(0, colon) : lower).replace('*', '');
    for (let i = 0; i < WAIT_CLASSES.length; i++) {
        if (WAIT_CLASSES[i].key === cls || WAIT_CLASSES[i].label.toLowerCase() === cls)
            return i;
    }
    return -1;
}

/* #187: the server-side `class_name` filter identity for a class DISPLAY
 * LABEL — decoupled from the label on purpose. pgwt-server matches a click-
 * drill's class_name via strcasecmp against pgwt_class_names ("cpu", "io",
 * ...); it has never heard of a parenthetical qualifier. Before this
 * existed, both the AAS band click (views/active.js) and the Time Model
 * table row click (table-configs.js) sent the raw display label straight
 * through as the filter value, which was harmless while every label matched
 * its class 1:1 — until 'cpu' grew the "(running)" suffix here, at which
 * point a class-mode drill on CPU silently matched zero rows (found live in
 * this feature's own `make check` run, not by inspection).
 *
 * Returns null for 'offcpu' ("CPU (waiting for a core)"): it is a computed
 * residual, not a wait_event class the server can filter by, and there is no
 * Events-table row that could ever back it — callers must treat null as
 * "not drillable", never fall back to sending the raw label. */
export function classFilterName(label) {
    if (label == null) return null;
    if (label === 'CPU (waiting for a core)') return null;
    return String(label).replace(/\s*\(running\)$/, '').replace('*', '');
}

// -- Event color service (U1, review P2) --------------------------------------
//
// One deterministic color per wait EVENT: a tint of its class's semantic hue.
// The retired EVENT_PALETTE keyed colors by ARRAY POSITION while the server
// re-ranks the top-N event series by total AAS every window — so the same
// event changed color on every live tick, in hues (tomato/limegreen/gold)
// that mean Lock/CPU/Client everywhere else. Here the HUE always comes from
// the event's class (semantics preserved; the WAIT_CLASSES colors are
// untouched) and a stable hash of the event NAME picks one of EVENT_TINTS
// lightness/saturation steps within that hue. Same event = same color in
// every view, forever, regardless of rank, order, or refresh.
//
// EVENT_TINTS and the FNV-1a hash are part of the visual contract: changing
// either recolors every event everywhere (a pixel-baseline change).

/* [lightness delta, saturation delta] steps within the class hue. Step 0 is
 * the class color itself; the rest are visibly distinct tints of it. */
export const EVENT_TINTS = [
    [0.00, 0.00],
    [0.14, -0.10],
    [-0.12, 0.00],
    [0.24, -0.20],
    [-0.20, 0.05],
    [0.07, -0.30],
];

/* FNV-1a over the event name -> tint step. The stable identity hash. */
export function eventTintStep(name) {
    const s = String(name == null ? '' : name);
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h % EVENT_TINTS.length;
}

function rgbToHsl(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    const l = (max + min) / 2;
    let h = 0, s = 0;
    const d = max - min;
    if (d !== 0) {
        s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
        if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (max === g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
    return [h, s, l];
}

function hslToRgb(h, s, l) {
    if (s === 0) {
        const v = Math.round(l * 255);
        return [v, v, v];
    }
    const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const p = 2 * l - q;
    const conv = (t) => {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1 / 6) return p + (q - p) * 6 * t;
        if (t < 1 / 2) return q;
        if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
        return p;
    };
    return [Math.round(conv(h + 1 / 3) * 255), Math.round(conv(h) * 255),
            Math.round(conv(h - 1 / 3) * 255)];
}

/* eventColor(classIdx_or_name, eventName) -> 'rgb(r,g,b)'.
 *
 * `cls` may be a WAIT_CLASSES index, any class spelling classColor accepts
 * ('io', 'IO', 'LWLock'), or null — then the class is derived from the
 * eventName's "Class:Event" prefix (falling back to the Unknown gray). The
 * tint step depends ONLY on eventName, so every caller that names the same
 * event gets the same color no matter how it identifies the class. */
export function eventColor(cls, eventName) {
    let base = null;
    if (typeof cls === 'number') base = WAIT_CLASSES[cls] ? WAIT_CLASSES[cls].color : null;
    else if (cls != null) base = classColor(String(cls));
    if (!base && eventName != null) base = classColor(String(eventName));
    if (!base) base = CLASS_COLOR_MAP.unknown;
    const m = /^rgb\((\d+),\s*(\d+),\s*(\d+)\)$/.exec(base);
    if (!m) return base;
    const [dl, ds] = EVENT_TINTS[eventTintStep(eventName)];
    const [h, s, l] = rgbToHsl(+m[1], +m[2], +m[3]);
    // Achromatic bases (the Unknown gray) must stay achromatic: adding
    // saturation would smuggle in a hue (red) that means Lock elsewhere.
    const s2 = s === 0 ? 0 : Math.min(1, Math.max(0.15, s + ds));
    const l2 = Math.min(0.88, Math.max(0.18, l + dl));
    const [r, g, b] = hslToRgb(h, s2, l2);
    return 'rgb(' + r + ',' + g + ',' + b + ')';
}

// -- Time / number formatting -------------------------------------------------

export function fmtTime(ns, bucketNs) {
    if (!ns) return '--';
    const d = new Date(ns / 1e6);
    const hms = d.toUTCString().slice(17, 25);  // "HH:MM:SS" in UTC
    if (!bucketNs || bucketNs >= 1000000000) return hms;
    const frac = (ns % 1000000000) / 1e9;
    if (bucketNs >= 1000000) return hms + '.' + frac.toFixed(3).slice(2);       // ms
    return hms + '.' + frac.toFixed(6).slice(2);                                // us
}

/* Exact decimal-nanosecond formatter for marker/waterfall payloads. Epoch-ns
 * values exceed Number's integer range; split with BigInt before constructing
 * the millisecond Date so sub-microsecond digits are never quantized away. */
export function fmtTimeNs(ns, bucketNs) {
    if (ns == null || ns === '' || ns === 0 || ns === '0') return '--';
    let value;
    try {
        value = typeof ns === 'bigint' ? ns :
            (typeof ns === 'number' ? BigInt(Math.round(ns)) : BigInt(ns));
    } catch (e) { return '--'; }
    const d = new Date(Number(value / 1000000n));
    if (!Number.isFinite(d.getTime())) return '--';
    const hms = d.toUTCString().slice(17, 25);
    const bucket = bucketNs == null ? 0n : BigInt(Math.round(Number(bucketNs)));
    if (!bucket || bucket >= 1000000000n) return hms;
    const frac = (value % 1000000000n + 1000000000n) % 1000000000n;
    const digits = frac.toString().padStart(9, '0');
    if (bucket >= 1000000n) return hms + '.' + digits.slice(0, 3);
    return hms + '.' + digits.slice(0, 6);
}

export function fmtTimeMs(ms) {
    if (!ms) return '--';
    const d = new Date(ms);
    const hms = d.toUTCString().slice(17, 25);
    const frac = ms % 1000;
    return hms + '.' + String(frac).padStart(3, '0');
}

export function fmtDuration(ns) {
    const s = ns / 1e9;
    if (s < 60) return s.toFixed(0) + 's';
    if (s < 3600) return (s / 60).toFixed(1) + 'm';
    return (s / 3600).toFixed(1) + 'h';
}

export function fmtMs(ms) {
    if (ms == null || isNaN(ms)) return '—';
    if (ms >= 1000) return (ms / 1000).toFixed(1) + 's';
    if (ms >= 1) return ms.toFixed(1) + 'ms';
    if (ms >= 0.001) return (ms * 1000).toFixed(0) + 'µs';
    return '<1µs';
}

// P1 null-guards: a missing/NaN value renders as an em-dash (like fmtMs/fmtUs
// always did), never a TypeError that blanks the whole pane.
export function fmtCount(n) {
    if (n == null || isNaN(n)) return '—';
    if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
    if (n >= 1e3) return (n / 1e3).toFixed(1) + 'K';
    return n.toString();
}

export function fmtPct(p) { if (p == null || isNaN(p)) return '—'; return p.toFixed(1) + '%'; }
export function fmtAas(a) { if (a == null || isNaN(a)) return '—'; return a.toFixed(2); }
export function fmtUs(us) {
    // null = gated latency column (sampled-only row, no real durations —
    // T1/FID-3); render as em-dash like fmtMs does.
    if (us == null || isNaN(us)) return '—';
    if (us >= 1e6) return (us / 1e6).toFixed(1) + 's';
    if (us >= 1000) return (us / 1000).toFixed(1) + 'ms';
    return us.toFixed(0) + 'µs';
}

/* HTML-escape. Quotes are escaped too: esc() output lands in ATTRIBUTES as
 * often as in text (title="..." on the percentile bounds and the drill
 * affordances, and every hostile-name tooltip), where a bare " would close
 * the attribute early and let the rest of a wait-event / SQL string be
 * parsed as markup. In text position &quot; / &#39; still render as " and ',
 * so visible output is unchanged. */
export function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;').replace(/"/g, '&quot;')
                    .replace(/'/g, '&#39;');
}

// -- datetime-local <-> ns, pinned to UTC (UI-11) ------------------------------
//
// Every time pgwt displays (chart axes, tooltips, the window readout) is UTC,
// and the window readout is labeled "UTC". The custom-range picker's
// <input type="datetime-local"> fields are therefore ALSO defined as UTC and
// labeled "(UTC)" in the markup: the user types the times they read off the
// screen. Browsers give datetime-local values no timezone, so both directions
// must be explicit — a naive `new Date(str)` would parse the string in the
// browser's LOCAL zone and hand a UTC+3 user a window three hours off.

/* ns since epoch -> "YYYY-MM-DDTHH:MM:SS" rendered in UTC. */
export function nsToDatetimeLocalUTC(ns) {
    const d = new Date(ns / 1e6);
    const pad = (n) => String(n).padStart(2, '0');
    return d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()) +
        'T' + pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()) + ':' + pad(d.getUTCSeconds());
}

/* "YYYY-MM-DDTHH:MM[:SS]" (a datetime-local value, defined as UTC) -> ns since
 * epoch, or null if unparsable. */
export function datetimeLocalUTCToNs(str) {
    if (!str) return null;
    const ms = Date.parse(str + 'Z');   // the Z suffix forces UTC parsing
    if (isNaN(ms)) return null;
    return ms * 1e6;
}

// -- Version-skew banner text (T7 / TST-11) -----------------------------------
//
// Pure mirror of web/bridge.go:versionSkewWarning — the Go bridge warns on
// stderr, this drives the UI banner. Returns null when the pair matches (no
// banner). Warn, never refuse: a skewed Mac-client/Linux-server pair is the
// normal deployment state, so the point is only visibility. `null` server
// fields mean the server did not report them (a build predating the v0.13
// handshake).
export function versionSkew(clientVersion, clientProto, serverVersion, serverProto) {
    if (serverVersion == null && serverProto == null) {
        return {
            level: 'warn',
            short: 'server version unknown',
            detail: 'The pgwt-server build predates the version handshake ' +
                '(< v0.13). Client is ' + clientVersion + ' (protocol ' +
                clientProto + '). Redeploy pgwt-server from the same checkout.',
        };
    }
    if (serverProto !== clientProto) {
        return {
            level: 'error',
            short: 'protocol mismatch',
            detail: 'pgwt-server ' + (serverVersion || '?') + ' speaks protocol ' +
                serverProto + ', this client (' + clientVersion + ') expects ' +
                clientProto + '. Responses may be misinterpreted — update the ' +
                'older side before trusting what you see.',
        };
    }
    if (serverVersion !== clientVersion) {
        return {
            level: 'warn',
            short: 'version skew',
            detail: 'pgwt-server is ' + serverVersion + ', this client is ' +
                clientVersion + ' (same protocol ' + clientProto + ' — safe, ' +
                'but behavior/fixes may differ). Rebuild/redeploy to align.',
        };
    }
    return null;
}
