"""Artifact logic for demo-walk-record.py; no browser imports."""
import bisect
import json
import math
from pathlib import Path

from PIL import Image, ImageChops

GIF_LIMIT = 10_000_000  # Deliberately decimal MB, strictly less than this.
GIF_MIN_SECONDS = 4
MOTION_LEVEL = 24  # Ignore JPEG/quantization noise below this luminance delta.
MOTION_FLOOR = 0.001  # At least 0.1% of the output canvas must visibly change.


class Refusal(Exception):
    def __init__(self, code, detail):
        super().__init__(detail)
        self.code = code


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
    def delta(a, b):
        hist = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L").histogram()
        return sum(hist[MOTION_LEVEL:]) / (a.width * a.height)
    endpoint = delta(pictures[0], pictures[-1])
    consecutive = [delta(a, b) for a, b in zip(pictures, pictures[1:])]
    for pair, value in [("first/last", endpoint)] + [
            (f"consecutive {n}/{n+1}", value) for n, value in enumerate(consecutive)]:
        if value < MOTION_FLOOR:
            raise Refusal(8, f"{name}: motion gate ({stage}, {pair}): "
                          f"{value:.6%} changed pixels, need >= {MOTION_FLOOR:.3%} "
                          f"at luminance delta >= {MOTION_LEVEL}")
    return {"first_last_changed_fraction": endpoint,
            "minimum_consecutive_changed_fraction": min(consecutive),
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
    try:
        with Image.open(path) as check:
            if check.n_frames < minimum:
                raise Refusal(7, f"{path.name}: frame count gate after encoding: "
                              f"{check.n_frames} decoded frames, need {minimum}")
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
                      minimum_frame_count=minimum, decoded_frame_count=len(decoded),
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
