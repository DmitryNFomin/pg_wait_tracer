#!/usr/bin/env python3
"""test_chip_label_alignment.py -- overlay chip label containment (issue #213).

drawOverlayLines (web/static/lib/uplot-aas.js) draws a colored chip sized to
its label, then writes the label in white with a small padding inside it.
It sets ctx.font and ctx.textBaseline every draw but relied on whatever
ctx.textAlign the vendored uPlot axis-label pass (drawAxes, which runs
immediately before this function's 'draw' hook) happened to leave set — the
canvas 2D context is one mutable object shared across hooks, and uPlot never
resets textAlign after its own axis pass. When that leftover value is
'right', this function's fillText anchor (chip left edge + pad) is read as
the text's RIGHT edge instead of its left: the label is drawn ending at the
chip's left edge and extending backwards over the plot background, mostly
OUTSIDE the chip.

This reproduces at ANY device pixel ratio -- the leftover-textAlign bug is a
canvas-state ordering defect, not a DPR-dependent one, even though it was
first noticed on a DPR-2 Retina panel (a 20px label offset by ~90px reads as
obviously broken; a 10px label offset by ~45px is easy to miss at a glance).
This test therefore checks BOTH DSF 1 and DSF 2 so a "looks fine at DSF 1"
reviewer skim cannot let a regression back in at the untested DPR.

Ground truth is the real Canvas 2D draw calls, not a pixel-color heuristic:
an init script wraps CanvasRenderingContext2D.fillRect/fillText for the
target cell's canvas and records (rect, text, textAlign, measured width) in
call order. For every label draw, this asserts the text's computed span
(from its OWN recorded x/textAlign/measuredWidth -- what the browser will
actually paint) lies within its immediately-preceding chip fillRect's span,
padded. That is exactly the "chip and text come apart" defect class: it
fails on a leftover textAlign of 'right' OR 'center', not only 'right', and
it cannot pass by accident (there is no shared computation between the
"expected" and "actual" sides -- one is the fillRect call, the other is the
fillText call).

gallery-uplot-aas-mixed-escalation exercises BOTH label paths at once: the
'8 CPUs' hline reference chip (bottom loop, N-CPUs) and the
'Escalated (anomaly)' vline chip (top loop, escalation edge) -- so a single
cell covers Defect 1's "affects both overlay chips" claim.

No root, no PG, no SSH -- uses mock_server.py, exactly like test_web_ui.py.
"""
import os
import signal
import socket
import subprocess
import sys
import time

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    msg = "playwright not installed (pip install playwright && playwright install chromium)"
    if os.environ.get("CI"):
        print(f"ERROR: {msg}", file=sys.stderr)
        sys.exit(1)
    print(f"SKIP: {msg}")
    sys.exit(0)

HTTP_PORT = int(os.environ.get("PGWT_CHIP_PORT", "18880"))
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")
GALLERY_URL = f"http://127.0.0.1:{HTTP_PORT}/dev/gallery.html"
CELL = "gallery-uplot-aas-mixed-escalation"

# Records fillRect/fillText calls for the target cell's canvas only, in call
# order -- so "the fillRect immediately before this fillText" is unambiguous
# (drawOverlayLines always paints a chip then its label, never interleaved
# with another chip's calls, per its source).
INIT_SCRIPT = """
window.__calls = [];
const origFillRect = CanvasRenderingContext2D.prototype.fillRect;
const origFillText = CanvasRenderingContext2D.prototype.fillText;
CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
    if (this.canvas && this.canvas.closest && this.canvas.closest('#%(cell)s')) {
        window.__calls.push({ kind: 'rect', x, y, w, h });
    }
    return origFillRect.apply(this, arguments);
};
CanvasRenderingContext2D.prototype.fillText = function(text, x, y, maxWidth) {
    if (this.canvas && this.canvas.closest && this.canvas.closest('#%(cell)s')) {
        window.__calls.push({
            kind: 'text', text, x, y,
            textAlign: this.textAlign,
            measuredWidth: this.measureText(text).width,
        });
    }
    return origFillText.apply(this, arguments);
};
""" % {"cell": CELL}


def _wait_port(proc, port, timeout=10.0):
    deadline = time.time() + timeout
    while True:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return True
        except OSError:
            if time.time() >= deadline:
                return False
            time.sleep(0.05)


def start_mock_server():
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(HTTP_PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if not _wait_port(proc, HTTP_PORT):
        if proc.poll() is None:
            proc.kill()
        out, err = proc.communicate()
        print(f"mock_server failed to start on port {HTTP_PORT}:\n"
              f"{out.decode()}\n{err.decode()}")
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def label_calls(page):
    """[(rect, text_call), ...] pairs -- each chip label's fillRect and its
    immediately-following fillText, in draw order."""
    calls = page.evaluate("() => window.__calls")
    pairs = []
    for i, c in enumerate(calls):
        if c["kind"] == "text":
            # the nearest preceding rect call is this label's chip
            rect = next((r for r in reversed(calls[:i]) if r["kind"] == "rect"), None)
            if rect is not None:
                pairs.append((rect, c))
    return pairs


def text_span(text_call):
    """The x-span [left, right] the browser will actually paint for this
    fillText call, given its OWN recorded anchor/textAlign/measuredWidth --
    independent of what the chip rect says (that's the point: this is
    ground truth about what gets painted, not an assumption)."""
    x = text_call["x"]
    w = text_call["measuredWidth"]
    align = text_call["textAlign"]
    if align == "left" or align == "start":
        return x, x + w
    if align == "right" or align == "end":
        return x - w, x
    if align == "center":
        return x - w / 2, x + w / 2
    raise AssertionError(f"unhandled textAlign {align!r}")


def check_dsf(playwright_module, dsf):
    browser = playwright_module.chromium.launch()
    ctx = browser.new_context(viewport={"width": 1400, "height": 1000},
                               device_scale_factor=dsf)
    page = ctx.new_page()
    page.add_init_script(INIT_SCRIPT)
    page.goto(GALLERY_URL)
    page.wait_for_selector(f"#{CELL} canvas", timeout=15000)
    page.wait_for_timeout(500)
    pairs = label_calls(page)
    ctx.close()
    browser.close()

    failures = []
    if len(pairs) < 2:
        failures.append(
            f"DSF {dsf}: expected >=2 chip labels in {CELL} "
            f"('8 CPUs' + 'Escalated (...)'), got {len(pairs)} -- "
            "the gallery cell or fixture changed shape; this check cannot "
            "see the chips it is supposed to be grading")
        return failures

    for rect, text_call in pairs:
        chip_left, chip_right = rect["x"], rect["x"] + rect["w"]
        text_left, text_right = text_span(text_call)
        # Must lie within the chip, allowing the 2px*dpr fillRect->fillText
        # padding the source uses on the near edge, and a 1px rounding slop.
        ok = (text_left >= chip_left - 1) and (text_right <= chip_right + 1)
        if not ok:
            failures.append(
                f"DSF {dsf}: label {text_call['text']!r} span "
                f"[{text_left:.1f}, {text_right:.1f}] not inside chip "
                f"[{chip_left:.1f}, {chip_right:.1f}] "
                f"(textAlign={text_call['textAlign']!r})")
    return failures


def main():
    with sync_playwright() as p:
        proc = start_mock_server()
        try:
            all_failures = []
            for dsf in (1, 2):
                all_failures.extend(check_dsf(p, dsf))
        finally:
            stop_mock_server(proc)

    if all_failures:
        print("FAIL: chip label alignment (issue #213)")
        for f in all_failures:
            print(f"  {f}")
        sys.exit(1)

    print("PASS: chip label alignment -- every overlay label stays inside "
          "its chip at DSF 1 and DSF 2")


if __name__ == "__main__":
    main()
