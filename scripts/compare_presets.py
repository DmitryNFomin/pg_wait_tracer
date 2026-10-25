"""Compare-picker reachability checks shared by UI tests and the recorder.

Only the standard library is imported; the caller supplies a page object.
"""
import math


def assert_compare_presets_reachable(page):
    """Inspect the actual picker against this connection's trace and range.

    Run on every tab of both real and mock walks. Counts alone cannot prove
    that any offered baseline can be used (#549).
    """
    if not page.locator('#compare-picker').is_visible():
        page.locator('#compare-btn').click()
    try:
        evidence = page.evaluate("""() => {
            const {server, timeRange} = window.__pgwt;
            const visible = el => el.getClientRects().length > 0 &&
                getComputedStyle(el).visibility !== 'hidden';
            return {
                trace: [server.fromNs, server.toNs],
                range: [timeRange.from, timeRange.to],
                offsets: [...document.querySelectorAll('#compare-picker [data-b-offset]')]
                    .filter(el => visible(el) && !el.disabled)
                    .map(el => Number(el.dataset.bOffset)),
                explanation: document.querySelector('.compare-presets').innerText.trim(),
            };
        }""")
    finally:
        page.locator('#compare-btn').click()
    start, end = evidence['trace']
    a, b = evidence['range']
    offsets = evidence['offsets']
    invalid = [off for off in offsets if not (
        math.isfinite(off) and off < 0 and off == int(off)
        and abs(off) <= 2**53 - 1 and start <= a + off < b + off <= end)]
    if invalid:
        raise AssertionError(f"compare presets unreachable: {invalid}; {evidence}")
    if not offsets:
        if a > start and 0 < b - a <= end - start:
            raise AssertionError(f"compare offers no reachable preset despite available history: {evidence}")
        if not evidence['explanation']:
            raise AssertionError(f"compare needs an explanation when no earlier baseline fits: {evidence}")
    return evidence
