#!/usr/bin/env python3
"""safari_webdriver.py -- a minimal W3C WebDriver HTTP client for real
Safari, driven via /usr/bin/safaridriver.

Deliberately NOT Selenium (avoids a new pip dependency when talking to
safaridriver's own HTTP endpoint is a handful of JSON calls) and
deliberately NOT Playwright's `webkit` channel. Playwright-WebKit is a
different build from Safari -- different JIT, different networking stack,
different timer and WebSocket behaviour -- so driving it and reporting the
result as "Safari" would be exactly the class of error this project has
been finding all night: an instrument that measures a near neighbour of the
thing and reports it as the thing. This module only ever talks to a real
Safari.app process through safaridriver's WebDriver endpoint.

safaridriver requires a human to have already run `safaridriver --enable`
once (a privileged, interactive action -- a TCC-gated preference change).
This module never attempts that itself; SafariSession's constructor surfaces
whatever safaridriver's own error says (its wording distinguishes "remote
automation is not enabled" from a TCC prompt from a driver-version mismatch
-- three different problems), rather than swallowing or reinterpreting it.

Usage:
    from safari_webdriver import SafariSession
    with SafariSession("http://localhost:9997") as s:
        s.navigate("http://localhost:8385/")
        title = s.execute_script("return document.title")
        s.screenshot("/tmp/shot.png")
"""
import base64
import json
import time
import urllib.error
import urllib.request


class WebDriverError(Exception):
    """Raised with safaridriver's own error/message verbatim -- never
    reworded or re-diagnosed, so the caller can tell "not enabled" apart
    from a TCC/auth failure apart from anything else."""

    def __init__(self, message, raw=None):
        super().__init__(message)
        self.raw = raw


def _request(base_url, method, path, body=None, timeout=30):
    url = f"{base_url}{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                  headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        try:
            raw = json.loads(e.read().decode())
        except Exception:
            raise WebDriverError(f"HTTP {e.code} (unparseable body)")
        err = raw.get("value", {}) if isinstance(raw, dict) else {}
        raise WebDriverError(
            f"{err.get('error', 'unknown')}: {err.get('message', '')}", raw)
    except urllib.error.URLError as e:
        raise WebDriverError(f"could not reach safaridriver at {base_url}: {e}")
    return raw.get("value") if isinstance(raw, dict) else raw


class SafariSession:
    """One WebDriver session against a real Safari window. `driver_url`
    points at an already-running `safaridriver --port N` (this class does
    not start/stop the safaridriver process itself -- the caller owns that
    process's lifetime, same separation as Playwright's browser vs.
    context)."""

    def __init__(self, driver_url, page_load_timeout_s=60):
        self.driver_url = driver_url.rstrip("/")
        self.session_id = None
        self.capabilities = None
        caps = {"alwaysMatch": {"browserName": "safari"}}
        value = self._raw("POST", "/session", {"capabilities": caps})
        self.session_id = value["sessionId"]
        self.capabilities = value.get("capabilities", {})
        self._raw("POST", f"/session/{self.session_id}/timeouts",
                   {"pageLoad": int(page_load_timeout_s * 1000)})

    def _raw(self, method, path, body=None, timeout=30):
        return _request(self.driver_url, method, path, body, timeout)

    def navigate(self, url):
        self._raw("POST", f"/session/{self.session_id}/url", {"url": url})

    def current_url(self):
        return self._raw("GET", f"/session/{self.session_id}/url")

    def execute_script(self, script, args=None):
        return self._raw("POST", f"/session/{self.session_id}/execute/sync",
                          {"script": script, "args": args or []})

    def screenshot_png_bytes(self):
        b64 = self._raw("GET", f"/session/{self.session_id}/screenshot")
        return base64.b64decode(b64)

    def screenshot(self, path):
        with open(path, "wb") as f:
            f.write(self.screenshot_png_bytes())

    def set_window_rect(self, width, height, x=0, y=0):
        return self._raw("POST", f"/session/{self.session_id}/window/rect",
                          {"width": width, "height": height, "x": x, "y": y})

    def reset_zoom(self):
        """Sends Cmd+0 (Safari's 'Actual Size' shortcut, resets page zoom to
        100%) via the W3C WebDriver Actions API. Found live (2026-09-28,
        coordinator): Safari persists a PER-HOSTNAME page zoom (measured
        ~115% for the hostname 'localhost' specifically on this Mac,
        confirmed by 127.0.0.1 and about:blank both being unaffected) --
        invisible, permanent-until-cleared, per-machine state that makes a
        rehearsal unreproducible if inherited rather than set. There is no
        W3C-standard "set zoom" endpoint; a keyboard shortcut via Actions is
        the only lever available here. Call AFTER navigate() (a shortcut
        needs a page with focus) and BEFORE fullscreen() (viewport
        assertions must see the reset zoom, not the persisted one)."""
        actions = {"actions": [{
            "type": "key", "id": "keyboard",
            "actions": [
                {"type": "keyDown", "value": ""},  # meta/cmd
                {"type": "keyDown", "value": "0"},
                {"type": "keyUp", "value": "0"},
                {"type": "keyUp", "value": ""},
            ],
        }]}
        self._raw("POST", f"/session/{self.session_id}/actions", actions)
        # No native "zoom changed" event to wait on; the shortcut itself is
        # synchronous UI work, a short settle covers the (rare) case Safari
        # animates the zoom transition.
        time.sleep(0.3)

    def fullscreen(self, min_settle_s=1.5, settle_timeout_s=5.0, settle_poll_s=0.3):
        """W3C WebDriver 'Fullscreen Window' -- what the demo actually
        presents at, distinct from a sized-but-windowed rect
        (set_window_rect). Returns the window rect WebDriver reports after
        the transition.

        Found live (2026-09-28): the reported CSS viewport PLATEAUS at a
        transiently-too-tall value before reaching its true final size --
        the toolbar-collapse animation isn't monotonically smooth, it
        measured 1074 and HELD there for >=0.5s before dropping to the
        correct 1069 around the 1s mark on this Mac. A naive "stop once two
        consecutive polls agree" loop stops AT that plateau and returns the
        wrong number with high confidence -- confirmed by hitting exactly
        that bug here first. So: sleep an unconditional min_settle_s floor
        first (past the known plateau), THEN poll for two consecutive equal
        reads to absorb any further variance, bounded by settle_timeout_s
        so a genuinely stuck transition still surfaces as a real mismatch
        in the caller's own viewport assertion instead of hanging here."""
        rect = self._raw("POST", f"/session/{self.session_id}/window/fullscreen")
        time.sleep(min_settle_s)
        deadline = time.monotonic() + settle_timeout_s
        prev = self.viewport_size()
        while time.monotonic() < deadline:
            time.sleep(settle_poll_s)
            cur = self.viewport_size()
            if cur == prev:
                break
            prev = cur
        return rect

    def viewport_size(self):
        """Actual CSS viewport of the content area (window.innerWidth/
        innerHeight) -- WebDriver's setWindowRect sizes the OS window's
        OUTER bounds (title bar + toolbar included), so the achieved inner
        size must be MEASURED, never assumed equal to what was requested."""
        return self.execute_script(
            "return {width: window.innerWidth, height: window.innerHeight, "
            "dpr: window.devicePixelRatio}")

    def close(self):
        if self.session_id is None:
            return
        sid = self.session_id
        self.session_id = None
        try:
            self._raw("DELETE", f"/session/{sid}")
        except WebDriverError:
            pass  # best-effort on teardown; nothing left to report to

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
