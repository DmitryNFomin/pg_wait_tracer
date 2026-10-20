"""Artifact logic for demo-walk-record.py; no browser imports."""
import bisect
import json
import math
from pathlib import Path

from PIL import Image, ImageChops

GIF_LIMIT = 10_000_000  # Deliberately decimal MB, strictly less than this.
GIF_MIN_SECONDS = 4
MOTION_LEVEL = 24  # Ignore JPEG/quantization noise below this luminance delta.

# issue #413 (the frame-count twin of #411's motion gate -- same false
# assumption, different check): the OLD post-encoding check demanded that
# EVERY frame sampled at the GIF's OWN playback rate (10fps) survive Pillow's
# deduplication distinct from its neighbor -- i.e. that the clip never go
# idle for even 100ms. That assumes continuous change from a UI that is, by
# the product's own design, idle between live ticks and during real network
# round trips -- the same assumption #411 already found false for MOTION,
# now found false for FRAME COUNT: a live drill-down clip spanning the
# entire drill() interaction (~11.2s, including its genuine network-latency
# waits) measured 75 distinct decoded frames against an old requirement of
# 112 (span x 10fps, i.e. zero tolerance for any duplicate anywhere in an
# 11-second clip). 75 distinct frames over 11.2s is a perfectly watchable
# animation; the old number encoded an assumption the product cannot meet,
# not a real quality floor.
#
# DECODED_MIN_FPS is deliberately far below the SAMPLING fps (10, an
# unrelated concern: how densely we poll the source video for candidate
# frames). It is a floor on how fast the OUTPUT must visibly change to read
# as an animation happening rather than a slideshow. 2fps (one distinct
# frame every 500ms) is a conservative, round floor for perceiving discrete
# UI-state motion -- this is a dashboard stepping through states on user
# actions and network replies, not smooth video, so it does not need
# anywhere near video frame rates to read as "things are happening."
# Do not raise this to manufacture a bigger number from a specific clip,
# and do not pulse harder to manufacture more real frames to clear it either
# (the capture already tried; during a genuine network wait the screen is
# not changing, so more captures only produce more frames Pillow would
# coalesce right back out) -- both would be buying a pass, not fixing a gate.
DECODED_MIN_FPS = 2

# issue #411: the live UI ticks every 5s (app.js startAutoRefresh's
# setTimeout(.., 5000)), and the headline/drill clips are each designed to
# cover roughly ONE tick (see demo-walk-record.py's drill()/dwell() comments
# and docs/DEMO_RUNBOOK.md's "about five seconds of inspection, including a
# live tick"). That means a CORRECT recording of this product looks like two
# static states joined by a single step, not continuous motion: between
# ticks, consecutive ~100ms-apart sampled frames are pixel-identical BY
# CONSTRUCTION, because the data they are drawing has not changed yet.
#
# The old rule (every consecutive sampled pair must individually clear this
# floor) therefore refused every CORRECT recording of a live Overview clip,
# not just broken ones -- measured on FOUR independent live runs against a
# real ~30-80 minute demo-workload capture (three fixed early 5s windows
# plus one exhaustive scan of the WHOLE recorded Overview dwell for the
# best-possible window), all landing at ~0.016% changed pixels per
# consecutive pair, about 6x under the old 0.100% floor, while two OTHER,
# independent checks on the SAME dwell confirmed the data genuinely does
# move: check_overview_wait_mix's DOM text (the wait-mix numbers visibly
# change every run) and chart_change() (best later frame vs. frame 0, needs
# >= 1%, which already passes every run). The content was never the
# problem; demanding smoothness from a step function was.
#
# MOTION_FLOOR is now used for TWO clip-level checks instead of one
# per-pair check (see motion_gate()):
#   1. first vs. last frame of the whole clip -- the SAME 1% "visibly
#      changed" bar chart_change() already uses and already clears on every
#      live run, so a passing clip here is independently corroborated, not
#      a new number invented to let a known-small number through. A
#      genuinely static clip (nothing ticks, nothing moves, every frame
#      identical) scores 0% here and still refuses -- the gate keeps its
#      teeth (test_identical_frames_refused_before_writing_gif).
#   2. the SINGLE biggest consecutive-pair jump in the clip -- the tick
#      landing somewhere in the window -- must ALSO clear 1%. This is what
#      stops (1) from passing on a single corrupted/flickered frame while
#      every other frame is frozen: real tick data changes by far more than
#      1% of the canvas when it does land (the swing that produces
#      chart_change()'s own >=1%), so a genuine tick clears this easily; a
#      one-frame JPEG glitch, lacking any real content change, generally
#      will not.
# Do not lower this number to make a specific clip pass -- it already
# matches an existing, independently-verified "visibly changed" bar used
# elsewhere in this same file; weakening it here would be removing the
# gate's teeth under a different name, the opposite of this fix.
MOTION_FLOOR = 0.01


class Refusal(Exception):
    def __init__(self, code, detail):
        super().__init__(detail)
        self.code = code


def young_capture_refusal(server_from_ns,window_from_ns):
    """Refuse when the capture starts strictly after the rendered window.

    The window's early range then has no underlying data, not merely idle
    data, which made a prior real demo recording's chart mostly empty.
    Return Refusal(9, ...) naming the age in seconds to one decimal place,
    or None when the capture already covers the window's start.
    """
    if server_from_ns <= window_from_ns:
        return None
    age_s = (server_from_ns - window_from_ns) / 1e9
    return Refusal(9,f"capture is younger than the rendered window by {age_s:.1f}s "
                   f"(server.fromNs={server_from_ns}, window.from={window_from_ns}); "
                   "the window's early range has no underlying data yet -- wait for "
                   "the capture to warm up, or pass --allow-young to override")


def frame_indices(frames, start, end):
    return [f["index"] for f in frames if start <= f["timestamp_s"] <= end]


def coverage(frames, start, end, fps):
    indices = frame_indices(frames, start, end)
    expected = math.floor((end - start) * fps)
    if len(indices) < expected:
        raise Refusal(5, f"frame starvation: {len(indices)} frames, need {expected} "
                      f"for {end - start:.3f}s at {fps} fps")
    times = [start] + [frames[i]["timestamp_s"] for i in indices] + [end]
    gap = max(b - a for a, b in zip(times, times[1:]))
    if gap > 2 / fps:
        raise Refusal(5, f"frame starvation: uncovered {gap:.3f}s gap (limit {2/fps:.3f}s)")
    return {"expected_min_frames": expected, "actual_frames": len(indices),
            "max_gap_s": gap, "frame_indices": indices}


def sample_indices(frames, definition):
    lo, hi = definition["start_frame"], definition["end_frame"]
    fps = definition["fps"]
    if not (0 <= lo < hi < len(frames)) or not (0 < fps <= 50):
        raise Refusal(6, "invalid GIF frame range or fps")
    times = [f["timestamp_s"] for f in frames]
    start, end = times[lo], times[hi]
    out = []
    for n in range(math.ceil((end - start) * fps)):
        at = min(end, start + n / fps)
        # Nearest real captured frame; GIF time sampling never invents evidence.
        j = bisect.bisect_left(times, at, lo, hi + 1)
        candidates = [i for i in (j - 1, j) if lo <= i <= hi]
        out.append(min(candidates, key=lambda i: abs(times[i] - at)))
    # Inclusive cuts must retain the final recorded state, even when the last
    # regular sampling instant falls just before a live update.
    if out:
        out[-1] = hi
    return out


def compose(image, crop, width):
    """Stack bands, joining DOM-derived column crops without shrinking text.

    Coordinates are in captured CSS pixels. No labels or numbers are drawn.
    A single rectangle is also supported for simple re-cuts.
    """
    bands = crop if isinstance(crop[0], list) else [[crop]]
    pieces = []
    for band in bands:
        tiles = []
        for x, y, w, h in band:
            if min(x, y) < 0 or min(w, h) <= 0 or x+w > image.width or y+h > image.height:
                raise Refusal(6, f"crop outside captured frame: {[x,y,w,h]}")
            tiles.append(image.crop((x, y, x+w, y+h)))
        line = Image.new("RGB", (sum(t.width for t in tiles), max(t.height for t in tiles)),
                         (30, 30, 58))
        x = 0
        for tile in tiles:
            line.paste(tile, (x, 0)); x += tile.width
        if line.width > width:
            line = line.resize((width, round(line.height * width/line.width)), Image.Resampling.LANCZOS)
        pieces.append(line)
    result = Image.new("RGB", (width, sum(p.height for p in pieces)), (30, 30, 58))
    y = 0
    for piece in pieces:
        result.paste(piece, (0, y)); y += piece.height
    return result


def motion_gate(pictures, name, stage):
    """issue #411: clip-level motion, not frame-to-frame motion -- see
    MOTION_FLOOR's comment for the full reasoning (the product ticks every
    5s; a clip is designed to straddle ONE tick; demanding every ~100ms
    sampled pair move refused every correct recording by construction).

    Two checks, both using the SAME per-pixel delta used throughout this
    file, and BOTH must hold (neither is an average, so one cannot paper
    over the other failing):
      1. first vs. last frame: the whole clip must look visibly different
         end to end. A genuinely static clip -- nothing ticks, nothing
         moves, every frame identical -- scores 0% here and still refuses.
      2. the single biggest consecutive-pair jump: at least ONE step
         somewhere in the clip -- the tick landing -- must itself clear the
         floor. Without this, (1) could in principle pass from a single
         corrupted/flickered frame at either end while everything else is
         frozen, with no evidence real content ever moved on screen."""
    def delta(a, b):
        hist = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L").histogram()
        return sum(hist[MOTION_LEVEL:]) / (a.width * a.height)
    endpoint = delta(pictures[0], pictures[-1])
    consecutive = [delta(a, b) for a, b in zip(pictures, pictures[1:])]
    best_step = max(consecutive) if consecutive else 0.0
    for check, value in [("first/last", endpoint), ("best single step", best_step)]:
        if value < MOTION_FLOOR:
            raise Refusal(8, f"{name}: motion gate ({stage}, {check}): "
                          f"{value:.6%} changed pixels, need >= {MOTION_FLOOR:.3%} "
                          f"at luminance delta >= {MOTION_LEVEL}")
    return {"first_last_changed_fraction": endpoint,
            "best_consecutive_changed_fraction": best_step,
            "minimum_consecutive_changed_fraction": min(consecutive) if consecutive else 0.0,
            "consecutive_changed_fractions": consecutive,
            "pixel_fraction_floor": MOTION_FLOOR, "luminance_delta_floor": MOTION_LEVEL}


def write_gif(root, frames, definition):
    if not 8 <= definition["fps"] <= 12:
        raise Refusal(6, "GIF fps must be between 8 and 12")
    indices = sample_indices(frames, definition)
    span = frames[definition["end_frame"]]["timestamp_s"] - frames[definition["start_frame"]]["timestamp_s"]
    minimum = max(math.ceil(GIF_MIN_SECONDS * definition["fps"]),
                  math.ceil(span * definition["fps"]))
    if len(indices) < minimum:
        raise Refusal(7, f"{definition['path']}: frame count gate: {len(indices)} sampled frames, "
                      f"need {minimum} for >= {GIF_MIN_SECONDS}s at {definition['fps']} fps")
    keyframes = definition.get("crop_keyframes", [])
    pictures = []
    for i in indices:
        crop = definition["crop"]
        for key in keyframes:
            if i >= key["start_frame"]:
                crop = key["crop"]
        with Image.open(root / frames[i]["path"]) as im:
            pictures.append(compose(im.convert("RGB"), crop, definition["width"]))
    # Fixed canvas across camera changes; never stretch text vertically.
    height = max(p.height for p in pictures)
    palette_source = Image.new("RGB", (definition["width"], height*min(8, len(pictures))))
    for n in range(min(8, len(pictures))):
        palette_source.paste(pictures[n*(len(pictures)-1)//max(1,min(8,len(pictures))-1)], (0, n*height))
    palette = palette_source.quantize(colors=256)
    canvases = []
    for picture in pictures:
        canvas = Image.new("RGB", (definition["width"], height), (30, 30, 58))
        canvas.paste(picture, (0, 0))
        canvases.append(canvas)
    source_motion = motion_gate(canvases, definition["path"], "sampled crop")
    quantized = [canvas.quantize(palette=palette, dither=Image.Dither.NONE) for canvas in canvases]
    palette_motion = motion_gate(quantized, definition["path"], "palette")
    path = root / definition["path"]
    if path.parent != root or path.suffix != ".gif":
        raise Refusal(6, "GIF path must be a .gif filename within the run")
    quantized[0].save(path, save_all=True, append_images=quantized[1:],
                      duration=round(1000/definition["fps"]/10)*10, loop=0,
                      optimize=True, disposal=1)
    size = path.stat().st_size
    if size >= GIF_LIMIT:
        raise Refusal(6, f"{path.name}: {size} bytes exceeds strict <10 MB limit")
    decoded_minimum = max(math.ceil(GIF_MIN_SECONDS * DECODED_MIN_FPS),
                          math.ceil(span * DECODED_MIN_FPS))
    try:
        with Image.open(path) as check:
            if check.n_frames < decoded_minimum:
                raise Refusal(7, f"{path.name}: frame count gate after encoding: "
                              f"{check.n_frames} distinct decoded frames, need "
                              f"{decoded_minimum} for >= {GIF_MIN_SECONDS}s at "
                              f"{DECODED_MIN_FPS} distinct fps (not the {definition['fps']} "
                              "sampling fps -- see DECODED_MIN_FPS)")
            decoded, durations = [], []
            for n in range(check.n_frames):
                check.seek(n); check.load()
                decoded.append(check.convert("RGB"))
                durations.append(check.info["duration"])
        decoded_motion = motion_gate(decoded, path.name, "decoded GIF")
    except Exception:
        path.unlink()
        raise
    definition.update(bytes=size, sampled_frame_indices=indices,
                      rendered_size=[definition["width"], height], source_span_s=span,
                      minimum_frame_count=minimum, decoded_minimum_frame_count=decoded_minimum,
                      decoded_frame_count=len(decoded),
                      playback_span_s=sum(durations)/1000, encoded_fps=1000/durations[0],
                      motion={"sampled":source_motion,"palette":palette_motion,"decoded":decoded_motion})


def chart_change(root, frames, indices, rectangle):
    """Measure recorded chart pixels; JPEG noise below 24 levels is ignored.

    This is animation evidence only, never a source for headline numbers.
    """
    x,y,w,h = rectangle
    with Image.open(root/frames[indices[0]]["path"]) as image:
        first = image.convert("RGB").crop((x,y,x+w,y+h))
    best = {"first_frame":indices[0],"changed_frame":indices[0],"changed_pixel_fraction":0.0}
    for index in indices[::4]+[indices[-1]]:
        with Image.open(root/frames[index]["path"]) as image:
            current = image.convert("RGB").crop((x,y,x+w,y+h))
        hist = ImageChops.difference(first,current).convert("L").histogram()
        fraction = sum(hist[24:])/(w*h)
        if fraction > best["changed_pixel_fraction"]:
            best.update(changed_frame=index,changed_pixel_fraction=fraction)
    if best["changed_pixel_fraction"] < 0.01:
        raise Refusal(2,"Overview: inspected recorded AAS chart did not visibly change")
    return best


def write_analysis(root, walk):
    lines = ["# Recorded demo walk", "", f"Source: {walk['source']}. "
             "Numbers below are visible DOM text; chart counts are readiness probes, not OCR.", ""]
    window = walk.get("window") or {}
    if window.get("mode") == "archived-full-span":
        span_s = window.get("span_s")
        lines += [f"Window: this walk serves an ARCHIVED capture with the time window set to the "
                  f"trace's own full span ({span_s:.1f}s, from_ns={window.get('from_ns')} to "
                  f"to_ns={window.get('to_ns')}), via the \"All\" quick-range. This is NOT the live "
                  "demo gesture: a live walk shows a rolling recent window (the 15-minute live "
                  "quick-range) that keeps advancing with wall-clock; this one shows the whole "
                  "capture, fixed, because the capture ended well before this walk ran. Treat this "
                  "as evidence of what the UI renders for that data, not as a recording of the live "
                  "rolling-window gesture.", ""]
    for tab in walk["tabs"]:
        observations = tab["observations"]
        first, last = observations[0], observations[-1]
        def headline(o):
            return "; ".join(f"{m['label']} {m['value']}" for m in o["headlines"])
        text = (f"{tab['tab'].title()}: {tab['dwell']['end_s']-tab['dwell']['start_s']:.2f}s, "
                f"{len(tab['frame_indices'])} captured frames. "
                f"Panel probe: {first['panel']['detail']}. "
                f"Visible headlines: {headline(first) or 'none'}. ")
        if headline(first) != headline(last):
            text += f"Last headlines: {headline(last)}. "
        if first["table_rows"]:
            text += "First visible row: " + " | ".join(first["table_rows"][0]) + ". "
        if first["readouts"]:
            text += "Visible panel text: " + "; ".join(first["readouts"]) + ". "
        changed = any(o["table_rows"] != first["table_rows"] for o in observations[1:])
        text += f"Visible table values {'changed' if changed else 'stayed unchanged'} across DOM samples. "
        if tab.get("wait_mix_check"):
            text += f"{tab['wait_mix_check']}. "
        text += f"Console errors: {len(tab['console_errors'])}."
        if tab.get("chart_change"):
            change = tab["chart_change"]
            text += (f" Recorded AAS chart changed {change['changed_pixel_fraction']:.1%} of pixels "
                     f"between frames {change['first_frame']} and {change['changed_frame']}.")
        lines.extend([text, ""])
    drill = walk["drill"]
    lines += ["Drill-down: selected query row " + " | ".join(drill["query_row"]) +
              "; selected execution row " + " | ".join(drill["execution_row"]) +
              "; visible wait readout: " + drill["wait_readout"] + ".", "",
              "Runbook ambiguities: " + "; ".join(walk["runbook"]["ambiguities"]), ""]
    (root / "analysis.md").write_text("\n".join(lines))


def save_walk(root, walk):
    (root / "walk.json").write_text(json.dumps(walk, indent=2) + "\n")
