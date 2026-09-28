# 2026-09-28 — #213: overlay chip labels render inside their chips

Two baselines regenerated, both for one root cause.

| baseline | diff ratio vs old | what changed |
|---|---|---|
| `gallery/uplot-aas-mixed-escalation` | 0.0047 (> 0.002) | "8 CPUs" and "Escalated (anomaly)" chip labels move from **outside** their chips to **inside** them |
| `gallery/uplot-aas-escalated-live-edge` | 0.0044 (> 0.002) | same fix, "Escalated (manual)" chip label |

45 of 47 snapshots matched unchanged. The paired ECharts cells (`gallery/aas-*`)
are untouched, as expected: the defect is uPlot-only.

## Why the pixels moved

`drawOverlayLines` (`web/static/lib/uplot-aas.js`) draws a coloured chip sized
to its label and then writes the label inside it. It set `ctx.font` and
`ctx.textBaseline` but never `ctx.textAlign`, while vendored uPlot sets
`textAlign` per axis during its own label pass and leaves it set — so the
overlay inherited a leftover `textAlign='right'` and painted each label ending
at its chip's left edge instead of starting inside it. The fix resets
`textAlign` explicitly.

## The issue's premise was wrong, and this entry records that

#213 described the defect as devicePixelRatio-2 only. Measured with Canvas 2D
draw-call instrumentation (intercepting `fillRect`/`fillText` rather than
inferring from pixels), it reproduces **identically at DSF 1 and DSF 2**. DSF 1
merely reads as acceptable at a glance because a 10px-font offset (~45px) is
less obvious than a 20px-font offset (~90px) on the same panel. The fix
therefore addresses the real cause rather than gating on DPR, and the new
regression test (`tests/test_chip_label_alignment.py`, wired into `make check`)
asserts the label's computed span lies inside the chip rect at **both** scale
factors.

That test is deliberately not a `textAlign !== 'right'` assertion, which would
pass on `'center'` while still broken.

## How these were produced

CI `snapshots` job via `gh workflow run CI --ref agent/chip-dpr2 -f
update_snapshots=true` (run 36446747983, conclusion success) — never a local
Playwright run, per RULE 2. **Only the two changed cells were copied out of the
artifact**; copying the whole artifact would silently replace all 47 baselines
and hide any unrelated drift.

Pins unchanged: `playwright==1.60.0`, chromium build 1223.
