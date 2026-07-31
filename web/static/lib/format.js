/* pgwt — pure formatting + color helpers (no DOM, no globals).
 *
 * Extracted verbatim from the old app.js so builders (which run in Node for
 * unit tests) can format values without importing the whole app. Behavior is
 * byte-identical to the originals — the Playwright "exact value" tests pin it.
 */

// -- Wait class colors (Oracle ASH / RDS PI inspired) -------------------------

export const WAIT_CLASSES = [
    { key: 'cpu',       label: 'CPU',       color: 'rgb(80,250,123)' },
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
];

export const CLASS_COLOR_MAP = {};
WAIT_CLASSES.forEach(c => {
    CLASS_COLOR_MAP[c.label.toLowerCase()] = c.color;
    CLASS_COLOR_MAP[c.key] = c.color;
});

// Palette for per-event series in drill-down charts
export const EVENT_PALETTE = [
    'rgb(30,144,255)', 'rgb(255,99,71)', 'rgb(50,205,50)', 'rgb(255,215,0)',
    'rgb(138,43,226)', 'rgb(0,206,209)', 'rgb(255,140,0)', 'rgb(220,20,60)',
    'rgb(0,191,255)', 'rgb(255,105,180)', 'rgb(127,255,0)', 'rgb(255,69,0)',
    'rgb(75,0,130)', 'rgb(0,250,154)', 'rgb(255,182,193)', 'rgb(100,149,237)',
];

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

export function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
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
