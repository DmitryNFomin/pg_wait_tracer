# Chrome demo-viewport measurement (2026-09-28)

Measured Chrome's content viewport on the real demo hardware, for the
Safari→Chrome demo-client switch. This is a measurement-only record; no
product code changed. `docs/DEMO_REHEARSAL_CRITERIA.md` is intentionally
not edited here — another branch owns it and cites this file alongside
its pin.

Location note: this record originally landed under
`tests/results/chrome_demo_viewport/`, which turned out to be treated as
ignored on the machine that merges branches (so it needed a force-add and
left the tree incoherent — tracked files under a path every future run
writes to untracked). It was moved here, next to the doc that cites it,
following the existing precedent of `docs/gate-box-noise-2026-09-17.json`
(a dated raw-measurement file living directly in `docs/`, cited from
`docs/DEV_LOOP_PLAN.md`).

## Environment

- Mac: display `Color LCD` / `Built-in Liquid Retina Display`, native
  2880x1864, **Main Display**, set to a HiDPI *scaled* logical resolution
  of 1710x1107 (`screen.width`/`screen.height`), `devicePixelRatio` 2.
  Only this built-in panel was attached (confirmed via
  `system_profiler SPDisplaysDataType`); the external LG UltraFine was
  detached.
- macOS 15.6.1 (24G90).
- Chrome: `/Applications/Google Chrome.app`, version **153.0.8010.53**
  (confirmed both by `--version` on the CLI and by the CDP
  `/json/version` endpoint of the launched instance).
- Launched via the dedicated demo profile: same binary, same
  `--user-data-dir=$HOME/.pgwt-demo-chrome-profile`, same flags
  (`--no-first-run --no-default-browser-check
  --disable-session-crashed-bubble`) as `~/pgwt-demo-chrome.command`. One
  extra flag, `--remote-debugging-port=9333`, was added for automation
  only — it does not affect layout/rendering, only exposes the Chrome
  DevTools Protocol on localhost. `~/.pgwt-demo-chrome-profile` did not
  exist before this run (fresh profile, no stale zoom/session state).
  `pmset -g` confirmed `sleep 0` / `displaysleep 0` before starting.
- Served page: a trivial local HTML page (`window.innerWidth` etc.,
  reported via `fetch()` to a local Python `http.server` on
  `127.0.0.1:8899`) — not the real pgwt app, per the brief ("any trivial
  local page ... is fine").
- Full screen entered/exited via the Chrome DevTools Protocol
  `Browser.setWindowBounds({windowState: "fullscreen"|"normal"})`, which
  drives the same native macOS fullscreen transition as the green-button /
  `Cmd+Ctrl+F` shortcut (confirmed below: menu bar auto-hides, animated
  transition, and CDP reports back `windowState: "fullscreen"`).
  `osascript`/System Events keystroke injection was tried first and
  refused ("osascript is not allowed to send keystrokes") — no
  Accessibility permission was granted to work around this, since the
  brief says not to change any system setting; CDP's window-state API was
  used instead and needs no such permission.

## Measurement: full screen, 100% zoom, both hostnames

| # | Method | hostname | innerWidth x innerHeight | outerW x outerH | DPR | windowState |
|---|---|---|---|---|---|---|
| 1 | CDP read (first fullscreen entry) | 127.0.0.1 | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 2 | CDP read (same entry, 2nd read) | 127.0.0.1 | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 3 | CDP read (after toggle out+in, 2nd entry, read 1) | 127.0.0.1 | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 4 | CDP read (2nd entry, read 2) | 127.0.0.1 | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 5 | CDP read, after `page.goto` to localhost | localhost | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 6 | CDP read, localhost, repeat | localhost | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 7 | CDP read, navigated back to 127.0.0.1 | 127.0.0.1 | 1710 x 981 | 1710 x 981 | 2 | fullscreen |
| 8 | CDP read (3rd fullscreen entry, after a further toggle out+in) x3 | 127.0.0.1 | 1710 x 981 (all 3) | 1710 x 981 | 2 | fullscreen |
| — | page's own `fetch()` auto-report on `resize`, several transitions | both | settles at 1710 x 981 every time | — | 2 | — |

Raw data backing this table is in the appendix below (four capture
channels: the CDP automation script's reads across the first/second
fullscreen entry and toggle, the localhost/127.0.0.1 cross-check, the
third independent fullscreen entry, and the page's own auto-report log —
an independent channel driven only by the page's `load`/`resize`
listeners, not by the automation script).

**Stable across 3 independent full-screen entries (toggled out to
windowed and back each time), and identical at `localhost` and
`127.0.0.1`: 1710 x 981 CSS px, every single reading.**

### The transient-1069 finding

One transient value was captured and is the most reusable finding here:
the page's own `resize`-event auto-report caught one intermediate frame
mid-animation reading **1710 x 1069** (`page_auto_report_log`, second
`resize` entry below, `ts` `20:31:09.051Z`) — not the settled state. The
very next auto-report, 637ms later (`ts` `20:31:09.688Z`), read 1710 x
981, and every explicit CDP read (each taken after an extra 400-1200ms
settle delay before reading) read 1710 x 981 too. **1710 x 1069 is
coincidentally the exact Safari figure already pinned in the criteria
doc** — a reminder of precisely the failure mode the brief warns about: a
single early reading taken during a fullscreen transition animation is
not the settled viewport, and would have produced a wrong-but-plausible
number if it had been the only reading taken. This is why
`docs/DEMO_REHEARSAL_CRITERIA.md`'s rule (settle, then two identical
readings, and assert the viewport actually achieved rather than the one
requested) now exists — this run demonstrates the trap it closes, not
just asserts it.

## Effective zoom check

Chrome exposes no direct JS "page zoom %" API. The proof used instead:
this display's native backing scale factor is 2 (confirmed via
`system_profiler`: 2880 physical / 1710 logical HiDPI-scaled ≈ retina 2x
mode), so `window.devicePixelRatio` on this machine is 2.0 **only** at
100% Chrome page zoom — any other zoom level multiplies DPR away from
2.0 (e.g. 110% zoom -> DPR 2.2). Every reading above, at both `localhost`
and `127.0.0.1`, on a **freshly created** profile with no prior
per-origin zoom preference, read `devicePixelRatio: 2` exactly. This
directly targets the bug called out in the brief: Chrome (like Safari)
persists zoom per hostname, so `localhost` and `127.0.0.1` were measured
and reported separately (rows 5-7 above), and both matched.

## Recommended pin

**1710 x 981 CSS at DPR 2** (full screen, built-in display, 100% zoom, in
the dedicated demo Chrome profile). This differs from the retracted
Safari pin (1710 x 1069) only in height — Chrome's own native-fullscreen
chrome (toolbar/omnibox) reserves ~88px more vertical space than
Safari's did; the logical screen (1710 x 1107) and DPR (2) are identical
across both browsers on this hardware, as expected since those are
OS/display properties, not browser ones.

## What is NOT measured

**The screen-shared variant (Zoom/Meet/Teams) is unmeasured.** A screen
share can change effective resolution, scaling, or force a different
display mode depending on the conferencing app and its settings; this
was not tested and must not be assumed equal to the direct-display
numbers above. This gap needs the owner to check with the actual
conferencing tool that will be used for the demo.

## What was NOT done

- `make check` / `make box-check` were not run — this task changed no
  code either covers (no `src/`, no `web/`, no test-assertion changes).
  The same is true of the follow-up that moved this file from
  `tests/results/` to `docs/`: no code, no gate.
- `docs/DEMO_REHEARSAL_CRITERIA.md` was not touched.
- The owner's Chrome (the pre-existing, non-demo-profile instance,
  already running under version 152.0.7977.76) was left untouched; only
  the newly-launched demo-profile instance was driven and then quit at
  the end. No system setting (displays, power, accessibility) was
  changed — the accessibility permission prompt was declined by not
  proceeding down that path at all.

## Appendix: raw readings

### `cdp_fullscreen_toggle_127.jsonl` — first and second fullscreen entry, 127.0.0.1

```
{"innerWidth": 1200, "innerHeight": 937, "outerWidth": 1200, "outerHeight": 1024, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1068, "fullscreenElement": false, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "windowed-initial"}
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "fullscreenElement": false, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "fullscreen-1"}
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "fullscreenElement": false, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "fullscreen-2"}
{"innerWidth": 1200, "innerHeight": 937, "outerWidth": 1200, "outerHeight": 1024, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1068, "fullscreenElement": false, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "windowed-after-toggle"}
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "fullscreenElement": false, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "fullscreen-reentry-1"}
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "fullscreenElement": false, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "fullscreen-reentry-2"}
```

### `cdp_localhost_crosscheck.jsonl` — localhost vs 127.0.0.1, same fullscreen session

```
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "hostname": "localhost", "href": "http://localhost:8899/", "label": "localhost-1"}
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "hostname": "localhost", "href": "http://localhost:8899/", "label": "localhost-2"}
{"innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "hostname": "127.0.0.1", "href": "http://127.0.0.1:8899/", "label": "back-to-127"}
```

### `cdp_third_fullscreen_entry.jsonl` — a third independent toggle-out/toggle-in

```
{"label": "third-entry-1", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "hostname": "127.0.0.1", "windowState": "fullscreen"}
{"label": "third-entry-2", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "hostname": "127.0.0.1", "windowState": "fullscreen"}
{"label": "third-entry-3", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "hostname": "127.0.0.1", "windowState": "fullscreen"}
```

### `page_auto_report_log.jsonl` — the page's own independent `fetch()` reports (`load`/`resize`, not driven by the automation script's explicit reads)

```
{"reason": "load", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:29:31.385Z", "innerWidth": 1200, "innerHeight": 937, "outerWidth": 1200, "outerHeight": 1024, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1068, "visualViewportWidth": 1200, "visualViewportHeight": 937, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "load", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:30:32.522Z", "innerWidth": 1200, "innerHeight": 937, "outerWidth": 1200, "outerHeight": 1024, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1068, "visualViewportWidth": 1200, "visualViewportHeight": 937, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "resize", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:31:09.051Z", "innerWidth": 1710, "innerHeight": 1069, "outerWidth": 1710, "outerHeight": 1069, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "visualViewportWidth": 1710, "visualViewportHeight": 1069, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "resize", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:31:09.688Z", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "visualViewportWidth": 1710, "visualViewportHeight": 981, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "resize", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:31:11.066Z", "innerWidth": 1200, "innerHeight": 937, "outerWidth": 1200, "outerHeight": 1024, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1068, "visualViewportWidth": 1200, "visualViewportHeight": 922, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "resize", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:31:13.086Z", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "visualViewportWidth": 1710, "visualViewportHeight": 981, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "load", "href": "http://localhost:8899/", "hostname": "localhost", "ts": "2026-09-28T20:31:47.847Z", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "visualViewportWidth": 1710, "visualViewportHeight": 981, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "load", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:31:48.674Z", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "visualViewportWidth": 1710, "visualViewportHeight": 981, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "resize", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:32:02.891Z", "innerWidth": 1200, "innerHeight": 937, "outerWidth": 1200, "outerHeight": 1024, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1068, "visualViewportWidth": 1200, "visualViewportHeight": 922, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
{"reason": "resize", "href": "http://127.0.0.1:8899/", "hostname": "127.0.0.1", "ts": "2026-09-28T20:32:04.518Z", "innerWidth": 1710, "innerHeight": 981, "outerWidth": 1710, "outerHeight": 981, "devicePixelRatio": 2, "screenWidth": 1710, "screenHeight": 1107, "screenAvailWidth": 1710, "screenAvailHeight": 1074, "visualViewportWidth": 1710, "visualViewportHeight": 981, "fullscreenElement": false, "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"}
```
