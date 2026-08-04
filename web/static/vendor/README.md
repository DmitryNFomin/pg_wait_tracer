# Vendored third-party assets

## echarts.min.js

| | |
|---|---|
| Library | [Apache ECharts](https://echarts.apache.org/) |
| Version | **5.5.1** (bundles zrender 5.6.0; embedded `version="5.5.1"` string) |
| Source | https://cdn.jsdelivr.net/npm/echarts@5.5.1/dist/echarts.min.js (npm package `echarts@5.5.1`, file `dist/echarts.min.js`) — the committed file is **byte-identical** to that URL (verified 2026-07-07) |
| License | Apache-2.0 |
| SHA-256 | `e84270bd0cd5bdf60fefc26d00c2a391cb2e81f4d26a7a9ee16185a54773a3cf` |

Vendored (rather than loaded from a CDN) deliberately: deterministic builds,
airgapped-friendly `go:embed` packaging, stable visual snapshots, and no
supply-chain exposure from a floating CDN tag. See the comment in
`web/static/index.html`.

Verify the committed file at any time:

```sh
sha256sum web/static/vendor/echarts.min.js
# must print e84270bd0cd5bdf60fefc26d00c2a391cb2e81f4d26a7a9ee16185a54773a3cf
```

## uPlot.iife.min.js + uPlot.min.css

| | |
|---|---|
| Library | [uPlot](https://github.com/leeoniya/uPlot) (Track U Phase U2b: the AAS instrument pane's renderer — the ECharts gesture pipeline tripped the written 60 fps gate RED, `tests/results/gesture_gate.json`) |
| Version | **1.6.32** (release tag `1.6.32`; embedded `version = "1.6.32"` string) |
| Source | https://raw.githubusercontent.com/leeoniya/uPlot/1.6.32/dist/uPlot.iife.min.js and `.../dist/uPlot.min.css` — the committed files are **byte-identical** to the same paths served by the GitHub contents API (`gh api repos/leeoniya/uPlot/contents/dist/<file>?ref=1.6.32`), cross-checked 2026-08-01 |
| License | MIT |
| SHA-256 (js) | `19c8d4c6ad88929a79f4ae49d6f7161566dfd0ba3d15cc495e974f787eb78f1f` |
| SHA-256 (css) | `df630c6a8d6f8eeaff264b50f73ce5b114f646ffd9a0bb74f049b0a00135fa04` |

Same vendoring rationale as ECharts (deterministic builds, `go:embed`,
airgapped-friendly, no floating CDN tag). No build step — uPlot ships a ~50 KB
IIFE bundle. The pure AAS adapter lives in `web/static/lib/uplot-aas.js`;
behavioral notes in that module's header cite line numbers **in this exact
pinned bundle's readable twin** (`dist/uPlot.iife.js` at tag 1.6.32), so bump
the version here and re-verify those notes together.

Verify the committed files at any time:

```sh
sha256sum web/static/vendor/uPlot.iife.min.js web/static/vendor/uPlot.min.css
# must print
# 19c8d4c6ad88929a79f4ae49d6f7161566dfd0ba3d15cc495e974f787eb78f1f  web/static/vendor/uPlot.iife.min.js
# df630c6a8d6f8eeaff264b50f73ce5b114f646ffd9a0bb74f049b0a00135fa04  web/static/vendor/uPlot.min.css
```

## Update procedure

1. Download the new pinned version (never "latest"):
   - ECharts: `curl -LO https://cdn.jsdelivr.net/npm/echarts@<X.Y.Z>/dist/echarts.min.js`
   - uPlot: `curl -LO https://raw.githubusercontent.com/leeoniya/uPlot/<X.Y.Z>/dist/uPlot.iife.min.js`
     (and `dist/uPlot.min.css`)
2. Cross-check the bytes against a second source before committing (e.g.
   `npm pack echarts@<X.Y.Z>` / `npm pack uplot@<X.Y.Z>`, or the GitHub
   contents API for uPlot: `gh api repos/leeoniya/uPlot/contents/dist/<file>?ref=<X.Y.Z> --jq .content | base64 -d`).
3. Replace the file(s) under `web/static/vendor/`, update the version and
   SHA-256 in **this file** in the same commit. For uPlot, re-verify the
   source-behavior notes in `web/static/lib/uplot-aas.js` against the new
   tag's `dist/uPlot.iife.js` (they pin band/scale/setData semantics).
4. Run the local suites: `node --test 'tests/web_unit/*.test.mjs'`,
   `python3 tests/test_web_ui.py`, `python3 tests/test_web_ui_chaos.py`.
5. **Rebaseline the visual snapshots** — a chart-library upgrade almost always
   moves pixels. Baselines are generated only in CI: trigger the `CI`
   workflow via `workflow_dispatch` with `update_snapshots: true`, download
   the `snapshot-baselines` artifact, commit the PNGs under
   `tests/web_snapshots/`, and push them with the upgrade PR (see
   `.github/workflows/ci.yml` and `tests/web_snapshots/README.md`).
6. Note the upgrade + new hash in the PR body.
