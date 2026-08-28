#!/usr/bin/env python3
"""ui_gallery_report.py — build the before/after contact sheet for ui_gallery.sh.

    ui_gallery_report.py BEFORE_DIR AFTER_DIR OUT_HTML [--base REF] [--head REF]

For every PNG present on either side: pixel-diff ratio (same channel threshold
as test_web_ui_snapshots.py), a highlighted diff image, and a verdict
(unchanged / changed / added / removed). The HTML lists changed cells first so a
reviewer — human or agent — sees what moved without scrolling past what didn't.
Exit status is 0 always; it is a report, not a gate (the gate is the reviewer).
"""
import argparse, glob, html, json, os, sys
import numpy as np
from PIL import Image

PIXEL_THRESHOLD = 24   # per-channel, absorbs antialiasing
NOISE_RATIO = 0.002    # below this: "unchanged"


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.int16)


def compare(a_path, b_path, diff_path):
    a, b = load(a_path), load(b_path)
    if a.shape != b.shape:
        return 1.0, f"size {a.shape[1]}x{a.shape[0]} -> {b.shape[1]}x{b.shape[0]}"
    mask = (np.abs(a - b) > PIXEL_THRESHOLD).any(axis=2)
    ratio = float(mask.mean())
    if ratio > 0:
        vis = (b * 0.35).astype(np.uint8)
        vis[mask] = [255, 40, 120]
        Image.fromarray(vis).save(diff_path)
    return ratio, f"{ratio*100:.2f}% pixels"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before"); ap.add_argument("after"); ap.add_argument("out")
    ap.add_argument("--base", default="base"); ap.add_argument("--head", default="head")
    args = ap.parse_args()
    out_dir = os.path.dirname(os.path.abspath(args.out))
    diff_dir = os.path.join(out_dir, "diff"); os.makedirs(diff_dir, exist_ok=True)

    def names(d):
        return {os.path.relpath(p, d)[:-4] for p in glob.glob(os.path.join(d, "**", "*.png"), recursive=True)
                if not p.endswith(("-actual.png", "-diff.png"))}
    before, after = names(args.before), names(args.after)
    rows = []
    for n in sorted(before | after):
        b, a = os.path.join(args.before, n + ".png"), os.path.join(args.after, n + ".png")
        if n not in before:
            rows.append((n, "added", 1.0, "new cell", None, a, None)); continue
        if n not in after:
            rows.append((n, "removed", 1.0, "cell gone", b, None, None)); continue
        dp = os.path.join(diff_dir, n.replace("/", "__") + ".png")
        ratio, detail = compare(b, a, dp)
        verdict = "unchanged" if ratio <= NOISE_RATIO else "changed"
        rows.append((n, verdict, ratio, detail, b, a, dp if ratio > 0 else None))
    order = {"changed": 0, "added": 1, "removed": 2, "unchanged": 3}
    rows.sort(key=lambda r: (order[r[1]], -r[2], r[0]))
    counts = {k: sum(1 for r in rows if r[1] == k) for k in order}

    def rel(p):
        return html.escape(os.path.relpath(p, out_dir)) if p else None
    def img(p):
        return f'<a href="{rel(p)}"><img src="{rel(p)}" loading="lazy"></a>' if p else '<div class="none">—</div>'

    parts = [f"""<!doctype html><meta charset="utf-8"><title>UI gallery {html.escape(args.base)} → {html.escape(args.head)}</title>
<style>
body{{font:14px/1.45 system-ui,sans-serif;margin:0;padding:1.5rem;background:#14181b;color:#e6e9ec}}
h1{{font-size:1.3rem;margin:0 0 .25rem}} .sum{{color:#9aa6af;margin-bottom:1.25rem}}
.sum b{{color:#e6e9ec}} .row{{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.6rem;margin-bottom:1.4rem;padding-top:.6rem;border-top:1px solid #2b353a}}
.row h3{{grid-column:1/-1;margin:0;font-size:.95rem;font-family:ui-monospace,monospace;font-weight:500}}
.v{{display:inline-block;padding:.05em .5em;border-radius:999px;font-size:.72rem;margin-left:.5rem;vertical-align:middle}}
.changed{{background:#5a1f3a;color:#ffb3d1}} .added{{background:#1f4a3a;color:#9be0c1}} .removed{{background:#4a2a1f;color:#e0b49b}} .unchanged{{background:#243036;color:#9aa6af}}
img{{max-width:100%;border:1px solid #2b353a;background:#000}} .none{{color:#5b6774;padding:2rem;text-align:center}}
.lbl{{font-size:.7rem;letter-spacing:.1em;text-transform:uppercase;color:#9aa6af}}
details{{margin-top:1rem}} summary{{cursor:pointer;color:#9aa6af}}
</style>
<h1>UI gallery: {html.escape(args.base)} → {html.escape(args.head)}</h1>
<div class="sum"><b>{counts['changed']}</b> changed · <b>{counts['added']}</b> added · <b>{counts['removed']}</b> removed · {counts['unchanged']} unchanged</div>
<div class="lbl" style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:.6rem"><span>before ({html.escape(args.base)})</span><span>after ({html.escape(args.head)})</span><span>diff (magenta = changed pixels)</span></div>"""]
    def row_html(r):
        n, v, ratio, detail, b, a, d = r
        return f'<div class="row"><h3>{html.escape(n)}<span class="v {v}">{v} · {html.escape(detail)}</span></h3>{img(b)}{img(a)}{img(d)}</div>'
    for r in rows:
        if r[1] != "unchanged": parts.append(row_html(r))
    parts.append(f'<details><summary>{counts["unchanged"]} unchanged cells</summary>')
    for r in rows:
        if r[1] == "unchanged": parts.append(row_html(r))
    parts.append("</details>")
    with open(args.out, "w") as f: f.write("\n".join(parts))
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump({"base": args.base, "head": args.head, "counts": counts,
                   "cells": [{"name": r[0], "verdict": r[1], "ratio": round(r[2], 5), "detail": r[3]} for r in rows]}, f, indent=1)
    print(f"gallery: {counts['changed']} changed, {counts['added']} added, {counts['removed']} removed, {counts['unchanged']} unchanged")
    for r in rows:
        if r[1] != "unchanged": print(f"  {r[1]:9s} {r[0]}  ({r[3]})")


if __name__ == "__main__":
    main()
