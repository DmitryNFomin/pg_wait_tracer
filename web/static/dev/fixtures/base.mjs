/* pgwt — shared constants for the deterministic gallery fixtures (Phase U1).
 *
 * HARD RULE for every module in this directory: PURE DATA, fully deterministic.
 * No Date.now(), no Math.random(), no transcendentals (Math.sin/pow rounding is
 * implementation-defined) — only integer arithmetic, IEEE-exact +,-,*,/ and
 * fixed literals, so a fixture byte-compares equal on every engine and every
 * run. That determinism is what makes the gallery-cell pixel snapshots
 * (tight-threshold, review §6 item 8) trustworthy.
 *
 * The epoch matches tests/mock_server.py's canned window (_TO_NS ≈ 1.774e18)
 * so gallery timestamps format like the Playwright suites' timestamps. Note ns
 * epochs exceed 2^53 — doubles quantize them to ~256 ns steps. That is exactly
 * what production sees (review P11), and it is deterministic.
 */

/* One hour before the mock's _TO_NS — fixtures build windows forward from here. */
export const BASE_NS = 1_774_000_000_000_000_000 - 3_600_000_000_000;

export const SEC_NS = 1_000_000_000;
export const MIN_NS = 60 * SEC_NS;

/* Round to 4 decimals (what the AAS builder does to series values anyway);
 * also collapses any last-ulp arithmetic noise well below pixel visibility. */
export function r4(v) {
    return Math.round(v * 10000) / 10000;
}

/* Deterministic triangle wave in [0,1] with period 2p: 1 at i%2p==0, 0 at
 * i%2p==p. The fixtures' only "waveform" primitive (no Math.sin — see above). */
export function tri(i, p) {
    const m = ((i % (2 * p)) + 2 * p) % (2 * p);
    return Math.abs(m - p) / p;
}
