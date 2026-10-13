#!/usr/bin/env python3
"""Targeted Histogram no_blink probe (diagnosis only -- NOT a gate).

Question: for each diverging sweep frame pair on the histogram tab, did the
underlying heatmap data change between those two frames?

Reuses tests/ui_live_smoke.py's own sweep machinery (same mount anchor, same
SWEEP_OFFSETS_MS, same clip/screenshot path, same blink_check) so the ratio it
reports is the gate's ratio, then adds three page-clock instruments the gate
does not have:

  1. a setOption wrapper on the view's own ECharts instance: every data state
     handed to the chart, with its Date.now() and a digest of the heatmap
     cells. A data change CANNOT hide between two frames -- it is logged when
     it happens, not sampled after the fact.
  2. a zrender 'rendered' listener: every actual paint, with Date.now().
  3. a per-frame digest read (option data as currently held + canvas size +
     mount seq) with the frame's [start_ms, end_ms] capture window in the
     SAME page clock, so each frame's raster instant is bracketed.

Refusal contract: if the ECharts instance or the digest cannot be read, the
frame's digest is recorded as unavailable and the pair verdict is "UNKNOWN",
never "data unchanged".
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ui_live_smoke as live  # noqa: E402
import ui_live_smoke_lib as lib  # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

# Installed once per page, AFTER the histogram tab has mounted at least once
# (the ECharts instance is created lazily inside renderHeatmap).
INSTALL_JS = """() => {
    const host = document.querySelector('#heatmap-container');
    if (!host) return {ok: false, reason: 'no #heatmap-container'};
    if (!window.echarts || !window.echarts.getInstanceByDom)
        return {ok: false, reason: 'window.echarts missing'};
    const ec = window.echarts.getInstanceByDom(host);
    if (!ec) return {ok: false, reason: 'no ECharts instance on host'};
    if (window.__histProbe && window.__histProbe.instance === ec)
        return {ok: true, reason: 'already installed'};
    const digest = (opt) => {
        const s = (opt && opt.series && opt.series[0]) || null;
        const cells = (s && s.data) || [];
        let h = 0, sum = 0;
        for (const c of cells) {
            const raw = Number(c[3]) || 0;
            sum += raw;
            h = (h * 131 + (Number(c[0])|0) * 7 + (Number(c[1])|0) * 13 + raw) % 2147483647;
        }
        const xa = (opt && opt.xAxis && opt.xAxis[0] && opt.xAxis[0].data) || [];
        const ya = (opt && opt.yAxis && opt.yAxis[0] && opt.yAxis[0].data) || [];
        const vm = (opt && opt.visualMap && opt.visualMap[0]) || {};
        return {ncells: cells.length, cellsum: sum, hash: h,
                nx: xa.length, ny: ya.length,
                x_first: xa.length ? String(xa[0]) : null,
                x_last: xa.length ? String(xa[xa.length - 1]) : null,
                vmax: vm.max === undefined ? null : vm.max,
                nseries: (opt && opt.series && opt.series.length) || 0,
                progressive: s ? s.progressive : null,
                progressiveThreshold: s ? s.progressiveThreshold : null};
    };
    const probe = {instance: ec, setOptions: [], paints: [], resizes: [],
                   install_at: Date.now(), digest: digest};
    const origSet = ec.setOption.bind(ec);
    ec.setOption = function (opt) {
        const args = Array.prototype.slice.call(arguments);
        const r = origSet.apply(null, args);
        try { probe.setOptions.push({at: Date.now(), d: digest(ec.getOption())}); }
        catch (e) { probe.setOptions.push({at: Date.now(), d: null, err: String(e)}); }
        return r;
    };
    const origResize = ec.resize.bind(ec);
    ec.resize = function () {
        probe.resizes.push(Date.now());
        return origResize.apply(null, Array.prototype.slice.call(arguments));
    };
    try {
        ec.getZr().on('rendered', () => probe.paints.push(Date.now()));
    } catch (e) { probe.paint_hook_err = String(e); }
    window.__histProbe = probe;
    return {ok: true, reason: 'installed'};
}"""

# Per-frame read. Deliberately returns an explicit `unavailable` reason rather
# than a plausible-looking empty digest.
READ_JS = """() => {
    const out = {at: Date.now()};
    const host = document.querySelector('#heatmap-container');
    const p = window.__histProbe;
    const fn = window.__pgwt && window.__pgwt.viewMount;
    out.mount = (typeof fn === 'function') ? fn() : null;
    if (!host) { out.unavailable = 'no #heatmap-container'; return out; }
    const cv = host.querySelector('canvas');
    out.host_px = {w: host.clientWidth, h: host.clientHeight};
    out.canvas_px = cv ? {w: cv.width, h: cv.height,
                          cssw: cv.style.width, cssh: cv.style.height} : null;
    if (!p) { out.unavailable = 'probe not installed'; return out; }
    const ec = window.echarts.getInstanceByDom(host);
    if (!ec) { out.unavailable = 'no ECharts instance'; return out; }
    if (ec !== p.instance) { out.unavailable = 'instance replaced since install'; }
    try { out.digest = p.digest(ec.getOption()); }
    catch (e) { out.unavailable = 'getOption failed: ' + String(e); }
    out.n_setoptions = p.setOptions.length;
    out.n_paints = p.paints.length;
    out.last_paint_at = p.paints.length ? p.paints[p.paints.length - 1] : null;
    out.n_resizes = p.resizes.length;
    return out;
}"""

LOG_JS = """() => {
    const p = window.__histProbe;
    if (!p) return null;
    return {setOptions: p.setOptions, paints: p.paints, resizes: p.resizes,
            install_at: p.install_at, paint_hook_err: p.paint_hook_err || null};
}"""


def digests_equal(a, b):
    if not a or not b:
        return None
    keys = ("ncells", "cellsum", "hash", "nx", "ny", "x_first", "x_last",
            "vmax", "nseries")
    return all(a.get(k) == b.get(k) for k in keys)


# Self-test (false-negative check for THIS probe, never used on a diagnostic
# run): mutate one heatmap cell through the view's own ECharts instance between
# two sweep frames. The probe must then report DATA_CHANGED on that pair -- a
# digest comparison that cannot see a real data change would report
# DATA_UNCHANGED here and wrongly blame the renderer.
MUTATE_JS = """() => {
    const host = document.querySelector('#heatmap-container');
    const ec = window.echarts.getInstanceByDom(host);
    const opt = ec.getOption();
    const s = opt.series[0];
    if (!s.data || !s.data.length) return {ok: false, reason: 'no cells'};
    const d = s.data.map(c => c.slice());
    d[0] = [d[0][0], d[0][1], d[0][2] + 1.5, (d[0][3] || 0) + 9999];
    ec.setOption({series: [{type: 'heatmap', data: d}]});
    return {ok: true, mutated_cell: d[0]};
}"""


# MECHANISM EXPERIMENT (not a product change, not a fix): re-issue the view's
# CURRENT data through its own ECharts instance with ECharts' progressive
# chunking threshold forced. The data is byte-identical (same cells array);
# only `progressiveThreshold` differs. Control arm forces 1e9 (never
# progressive) -- so any pixel diff that appears only in the low-threshold arm
# is attributable to progressive/incremental rendering, not to the extra
# setOption itself.
FORCE_PROG_JS = """(thr) => {
    const host = document.querySelector('#heatmap-container');
    const ec = window.echarts.getInstanceByDom(host);
    const opt = ec.getOption();
    const s = opt.series[0];
    const n = (s.data || []).length;
    ec.setOption({series: [{type: 'heatmap', progressive: 400,
                            progressiveThreshold: thr,
                            data: s.data}]});
    const sm = ec.getModel().getSeriesByIndex(0);
    return {ncells: n, threshold: thr, model_progressive: sm.getProgressive(),
            model_threshold: sm.getProgressiveThreshold(),
            at: Date.now()};
}"""


def walk(browser, url, out_dir, walk_i, ticks, threshold, viewport, dsf,
         save_frames, mutate_before_frame=None, force_prog=None):
    tab = "histogram"
    tab_dir = os.path.join(out_dir, "walk-%d" % walk_i)
    os.makedirs(tab_dir, exist_ok=True)
    context = browser.new_context(
        viewport=viewport, device_scale_factor=dsf,
        record_video_dir=tab_dir,
        record_video_size={"width": viewport["width"], "height": viewport["height"]})
    context.add_init_script(live.UNHANDLED_REJECTION_HOOK)
    page = context.new_page()
    rec = {"walk": walk_i, "ticks": [], "install": None, "reproduced": False}
    try:
        page.goto(url, timeout=60000)
        page.wait_for_selector("#status.connected", timeout=60000)
        deadline = time.monotonic() + 60
        while page.query_selector("#table-container table tbody tr") is None:
            if time.monotonic() > deadline:
                raise live.SmokeFailure("no data in 60s")
            page.wait_for_timeout(200)
        page.evaluate(live.TICK_HOOK_JS)
        live._navigate_to_tab(page, tab, 60)
        rec["install"] = page.evaluate(INSTALL_JS)
        print("  walk %d: install -> %s" % (walk_i, rec["install"]), flush=True)
        baseline = page.evaluate("window.__uiLiveTicks.length")
        last_seq = None
        for i in range(1, ticks + 1):
            if not live._wait_for_tick(page, baseline + i, live.TICK_TIMEOUT_S):
                raise live.SmokeFailure("tick %d never arrived" % i)
            tick_ts = page.evaluate("idx => window.__uiLiveTicks[idx]",
                                    lib.tick_hook_index(baseline, i))
            mount = live._wait_for_mount_at_or_after(page, tab, tick_ts,
                                                     live.TICK_TIMEOUT_S,
                                                     min_seq=last_seq)
            last_seq = mount["seq"]
            seq_before = mount["seq"]
            # Idempotent reinstall: the view disposes/recreates its instance on
            # an unavailable/no-data render, and a replaced instance must never
            # be read as "nothing changed".
            reinstall = page.evaluate(INSTALL_JS)
            clip, dims = live._panel_clip_and_dims(page, tab)
            if force_prog is not None:
                forced = page.evaluate(FORCE_PROG_JS, force_prog)
                print("  walk %d tick %d: forced progressive -> %s"
                      % (walk_i, i, forced), flush=True)
            frames, achieved, caps, reads, windows = [], [], [], [], []
            for fi, off in enumerate(lib.SWEEP_OFFSETS_MS):
                if mutate_before_frame is not None and fi == mutate_before_frame:
                    m = page.evaluate(MUTATE_JS)
                    print("  walk %d tick %d: self-test mutation before frame "
                          "%d -> %s" % (walk_i, i, fi, m), flush=True)
                f, a, c, end = live._capture_at_offset(page, tab, mount["at"],
                                                       off, clip,
                                                       read_end_clock=True)
                r = page.evaluate(READ_JS)
                frames.append(f)
                achieved.append(a)
                caps.append(c)
                reads.append(r)
                windows.append(None if f is None or end is None
                               else (mount["at"] + a, end))
            after = live._view_mount(page)
            seq_after = after["seq"] if after else None
            arrays = [lib.png_bytes_to_array(f) if f is not None else None
                      for f in frames]
            pairs = lib.sweep_consecutive_diff_ratios(arrays)
            ratio, note = lib.blink_sweep_gate_verdict(arrays, seq_before, seq_after)
            tick = {"tick": i, "mount_seq": mount["seq"], "mount_at": mount["at"],
                    "tick_ts": tick_ts, "reinstall": reinstall,
                    "seq_before": seq_before, "seq_after": seq_after,
                    "achieved_offsets_ms": achieved,
                    "capture_ms": caps, "panel_dims": dims,
                    "frame_windows_ms": windows,
                    "gate_ratio": ratio, "gate_note": note,
                    "pair_ratios": [p[0] for p in pairs],
                    "pair_notes": [p[1] for p in pairs],
                    "frame_reads": reads, "pairs": []}
            for j, (pr, _n) in enumerate(pairs):
                da = reads[j].get("digest")
                db = reads[j + 1].get("digest")
                same = digests_equal(da, db)
                if reads[j].get("unavailable") or reads[j + 1].get("unavailable"):
                    verdict = "UNKNOWN"
                elif same is None:
                    verdict = "UNKNOWN"
                elif same:
                    verdict = "DATA_UNCHANGED"
                else:
                    verdict = "DATA_CHANGED"
                tick["pairs"].append({
                    "pair": "%d->%d" % (lib.SWEEP_OFFSETS_MS[j],
                                        lib.SWEEP_OFFSETS_MS[j + 1]),
                    "ratio": pr, "data_verdict": verdict,
                    "setoptions_between": (reads[j + 1].get("n_setoptions", 0)
                                           - reads[j].get("n_setoptions", 0)),
                    "paints_between": (reads[j + 1].get("n_paints", 0)
                                       - reads[j].get("n_paints", 0)),
                    "diverging": pr is not None and pr >= threshold,
                    "signature": (lib.frame_diff_signature(arrays[j], arrays[j + 1])
                                  if arrays[j] is not None and arrays[j + 1] is not None
                                  else None)})
            tick["log"] = page.evaluate(LOG_JS)
            diverging = [p for p in tick["pairs"] if p["diverging"]]
            print("  walk %d tick %d: ratio=%s offsets=%s seq=%s diverging=%s"
                  % (walk_i, i, ratio, achieved, mount["seq"],
                     [(p["pair"], round(p["ratio"], 4), p["data_verdict"],
                       p["setoptions_between"], p["paints_between"])
                      for p in diverging]), flush=True)
            if diverging or save_frames == "all":
                fdir = os.path.join(tab_dir, "tick-%d" % i)
                os.makedirs(fdir, exist_ok=True)
                for k, f in enumerate(frames):
                    if f is not None:
                        name = "frame-%d-%dms.png" % (k, lib.SWEEP_OFFSETS_MS[k])
                        with open(os.path.join(fdir, name), "wb") as fh:
                            fh.write(f)
                tick["frames_dir"] = fdir
            if diverging:
                rec["reproduced"] = True
            rec["ticks"].append(tick)
    except Exception as e:
        rec["error"] = "%s: %s" % (type(e).__name__, e)
        print("  walk %d: ERROR %s" % (walk_i, rec["error"]), flush=True)
    finally:
        try:
            page.close()
            context.close()
        except Exception:
            pass
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", default="tests/results/hist_blink_probe")
    ap.add_argument("--ticks", type=int, default=6)
    ap.add_argument("--walks", type=int, default=6)
    ap.add_argument("--max-min", type=float, default=12.0)
    ap.add_argument("--threshold", type=float, default=lib.BLINK_THRESHOLD)
    ap.add_argument("--stop-on-repro", type=int, default=1)
    ap.add_argument("--save-frames", default="diverging")
    ap.add_argument("--force-prog-threshold", type=int, default=None,
                    help="MECHANISM EXPERIMENT: on EVEN walks force ECharts' "
                         "progressiveThreshold to this value (same data); ODD "
                         "walks are the control arm at 1e9 (never progressive)")
    ap.add_argument("--mutate-before-frame", type=int, default=None,
                    help="SELF-TEST ONLY: mutate one heatmap cell before this "
                         "sweep frame index, so the probe's own data-change "
                         "detection can be shown going DATA_CHANGED")
    args, _unknown = ap.parse_known_args()
    os.makedirs(args.out, exist_ok=True)
    viewport = {"width": 1710, "height": 981}
    t0 = time.monotonic()
    out = {"url": args.url, "threshold": args.threshold,
           "offsets_ms": list(lib.SWEEP_OFFSETS_MS),
           "viewport": viewport, "dsf": 2, "walks": []}
    with sync_playwright() as p:
        browser = p.chromium.launch(args=["--no-sandbox"])
        for w in range(1, args.walks + 1):
            if (time.monotonic() - t0) / 60.0 > args.max_min:
                print("probe: max-min reached before walk %d" % w, flush=True)
                break
            print("=== walk %d (t+%.1f min) ===" % (w, (time.monotonic() - t0) / 60.0),
                  flush=True)
            rec = walk(browser, args.url, args.out, w, args.ticks,
                       args.threshold, viewport, 2, args.save_frames,
                       mutate_before_frame=args.mutate_before_frame,
                       force_prog=(None if args.force_prog_threshold is None
                                   else (1000000000 if w % 2 == 1
                                         else args.force_prog_threshold)))
            rec["t_min"] = round((time.monotonic() - t0) / 60.0, 2)
            out["walks"].append(rec)
            with open(os.path.join(args.out, "probe.json"), "w") as fh:
                json.dump(out, fh, indent=1)
            if rec["reproduced"] and args.stop_on_repro:
                print("probe: reproduced on walk %d; stopping" % w, flush=True)
                break
        browser.close()
    with open(os.path.join(args.out, "probe.json"), "w") as fh:
        json.dump(out, fh, indent=1)
    repro = [w["walk"] for w in out["walks"] if w.get("reproduced")]
    print("probe: walks=%d reproduced_on=%s" % (len(out["walks"]), repro), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
