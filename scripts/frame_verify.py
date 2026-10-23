#!/usr/bin/env python3
"""Issue #470: decode and score EVERY native WebM frame. Requires numpy/ffmpeg/ffprobe.

Usage: python3 frame_verify.py INPUT --output scratchpad/report.json --jobs 4
Exit 0=PASS, 1=collapse detected, 2=incomplete/invalid verification.
RGB grey-pad tolerance is inclusive. Flat colour is the modal quantized RGB
colour (floor each channel / 4; 262144 bins). Indexes are zero-based, end_frame
inclusive. `episodes` preserves the legacy union of flagged frames; separate
signature lists ensure grey at frame 0 can never use the flat startup exception.
Only the requested report and temporary stderr files are written, under the
script's scratchpad. Inputs are read-only. No rate resampling is performed.

The frame-0 flat-colour carve-out (Chromium's pre-paint blank) applies ONLY to
a SHORT startup episode: documented baseline episodes are 5-11 frames
(200-440ms). STARTUP_EPISODE_MAX_FRAMES caps it at 25 frames (1s @25fps) --
two orders of magnitude of headroom over the known baseline, but a page that
never rendered at all is still caught. A frame-0 flat episode longer than the
cap is a FAILURE, not a disclosed startup blank.

TOTAL_FLAGGED_FRAC_CAP additionally bounds, per file, the fraction of ALL
frames flagged grey-pad-or-flat-colour (regardless of episode grouping) --
this is what catches a recording whose blankness is fragmented into several
sub-cap episodes, which the per-episode cap alone would miss.

Adjacent-frame substitution is NOT reliably caught on a 1:1 (unscaled) master
(mean |Δ| 2.64 versus 14.4 on a scaled cut); grey/flat thresholds do not attempt
to catch low-contrast substitution on unscaled footage.

FRAME_READ_TIMEOUT_SECONDS bounds how long a single frame read may block on
the decoder pipe; a stalled/hung ffmpeg becomes an ERROR (exit 2), not an
indefinite hang, since this runs unattended in a release chain.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import select
import shlex
import subprocess
import sys
import tempfile
try:
    import numpy as np
except ImportError as exc:
    print(f'ImportError: numpy unavailable: {exc}', file=sys.stderr)
    sys.exit(2)

STARTUP_EPISODE_MAX_FRAMES = 25  # 1s @25fps; baseline startup episodes are 5-11 frames
TOTAL_FLAGGED_FRAC_CAP = 0.05  # baseline's worst-case per-file fraction is ~1.3% (11/906)
FRAME_READ_TIMEOUT_SECONDS = 30.0
# Allow 10% timing/rounding headroom for real recordings, but reject tiny decoded
# prefixes even when ffprobe and ffmpeg agree on their truncated frame counts.
MIN_FRAME_COVERAGE = 0.90


class Runs:
    def __init__(self, fps):
        self.fps, self.active, self.episodes = fps, None, []

    def feed(self, index, flagged, grey, flat):
        if not flagged:
            self.finish()
            return
        if self.active is None:
            self.active = dict(start_frame=index, end_frame=index, length=0,
                               start_ms=float(index * 1000 / self.fps),
                               max_grey_frac=0.0, max_flat_frac=0.0)
        self.active['end_frame'] = index
        self.active['length'] += 1
        self.active['max_grey_frac'] = max(self.active['max_grey_frac'], grey)
        self.active['max_flat_frac'] = max(self.active['max_flat_frac'], flat)

    def finish(self):
        if self.active is not None:
            self.episodes.append(self.active)
            self.active = None


def read_frame(pipe, size, timeout=FRAME_READ_TIMEOUT_SECONDS):
    data = bytearray(size)
    view, offset = memoryview(data), 0
    while offset < size:
        ready, _, _ = select.select([pipe], [], [], timeout)
        if not ready:
            raise TimeoutError(
                f'decoder produced no data for {timeout}s (stalled at {offset}/{size} bytes of this frame)')
        n = pipe.readinto(view[offset:])
        if not n:
            if offset:
                raise RuntimeError(f'partial frame: {offset}/{size} bytes')
            return None
        offset += n
    return data


def score(data):
    pixels = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3)
    mask = (pixels[:, 0] >= 211) & (pixels[:, 0] <= 231)
    for channel in (1, 2):
        mask &= (pixels[:, channel] >= 211) & (pixels[:, channel] <= 231)
    grey = float(np.count_nonzero(mask) / len(pixels))
    bins = (pixels[:, 0].astype(np.uint32) >> 2) << 12
    bins |= (pixels[:, 1].astype(np.uint32) >> 2) << 6
    bins |= pixels[:, 2] >> 2
    flat = float(np.bincount(bins, minlength=262144).max() / len(pixels))
    return grey, flat


def verify(path):
    try:
        probe_command = ['ffprobe', '-v', 'error', '-count_frames', '-select_streams', 'v:0',
                         '-show_entries', 'stream=width,height,avg_frame_rate,r_frame_rate,nb_read_frames:format=duration',
                         '-of', 'json', str(path)]
        probe = subprocess.run(probe_command, capture_output=True, text=True, check=True)
        if probe.stderr.strip():
            raise RuntimeError(f'ffprobe decode error: {probe.stderr.strip()}')
        metadata = json.loads(probe.stdout)
        streams = metadata['streams']
        if len(streams) != 1:
            raise RuntimeError('expected one selected video stream')
        stream = streams[0]
        width, height = int(stream['width']), int(stream['height'])
        fps, rate = Fraction(stream['avg_frame_rate']), Fraction(stream['r_frame_rate'])
        exact_count = int(stream['nb_read_frames'])  # Full decode; NO metadata fallback.
        duration_seconds = Fraction(metadata['format']['duration'])
        expected_frames = round(duration_seconds * fps)
        if duration_seconds <= 0 or expected_frames <= 0:
            raise RuntimeError('invalid container duration or expected frame count')
        if min(width, height, fps, rate, exact_count) <= 0:
            raise RuntimeError('empty stream or invalid dimensions/frame rate')
        command = ['ffmpeg', '-v', 'error', '-xerror', '-nostdin', '-threads', '1', '-i', str(path),
                   '-map', '0:v:0', '-an', '-sn', '-dn', '-pix_fmt', 'rgb24',
                   '-fps_mode', 'passthrough', '-threads', '1', '-f', 'rawvideo', 'pipe:1']
        union, grey_runs, flat_runs = (Runs(fps) for _ in range(3))
        count, max_grey, max_flat = 0, 0.0, 0.0
        with tempfile.TemporaryFile(dir=Path(__file__).resolve().parent) as errors:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, bufsize=0)
            try:
                while True:
                    data = read_frame(process.stdout, width * height * 3)
                    if data is None:
                        break
                    grey, flat = score(data)
                    is_grey, is_flat = grey >= 0.85, flat >= 0.90
                    union.feed(count, is_grey or is_flat, grey, flat)
                    grey_runs.feed(count, is_grey, grey, flat)
                    flat_runs.feed(count, is_flat, grey, flat)
                    max_grey, max_flat = max(max_grey, grey), max(max_flat, flat)
                    count += 1
                returncode = process.wait()
                errors.seek(0)
                error_text = errors.read().decode(errors='replace').strip()
                if returncode or error_text:
                    raise RuntimeError(f'ffmpeg exited {returncode}: {error_text}')
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
                process.stdout.close()
        for runs in (union, grey_runs, flat_runs):
            runs.finish()
        if not count:
            raise RuntimeError('ffmpeg decoded zero frames')
        # A frame-0 flat episode is a disclosed "startup" blank ONLY if it is
        # short (<=STARTUP_EPISODE_MAX_FRAMES); longer than that is a FAILURE,
        # not an exemption -- otherwise a page that never rendered at all
        # (one episode spanning the whole file) would be certified clean.
        startup = sum(e['start_frame'] == 0 and e['length'] <= STARTUP_EPISODE_MAX_FRAMES
                      for e in flat_runs.episodes)
        total_flagged_frames = sum(e['length'] for e in union.episodes)
        total_flagged_frac = total_flagged_frames / count
        return dict(path=str(path), width=width, height=height, fps=float(fps),
                    avg_frame_rate=stream['avg_frame_rate'], r_frame_rate=stream['r_frame_rate'],
                    fps_mismatch=(fps != 25 or rate != 25),
                    ffprobe_exact_frame_count=exact_count, decoded_frame_count=count,
                    frame_count_mismatch=(count != exact_count),
                    expected_frame_count=expected_frames, frame_coverage=count / expected_frames,
                    truncated_input=(count < MIN_FRAME_COVERAGE * expected_frames),
                    episode_count=len(union.episodes), episodes=union.episodes,
                    grey_pad_episode_count=len(grey_runs.episodes), grey_pad_episodes=grey_runs.episodes,
                    flat_colour_episode_count=len(flat_runs.episodes), flat_colour_episodes=flat_runs.episodes,
                    flat_colour_startup_episode_count=startup,
                    flat_colour_nonstartup_episode_count=len(flat_runs.episodes)-startup,
                    total_flagged_frames=total_flagged_frames, total_flagged_frac=total_flagged_frac,
                    total_flagged_frac_over_cap=(total_flagged_frac > TOTAL_FLAGGED_FRAC_CAP),
                    max_grey_frac_overall=max_grey, max_flat_frac_overall=max_flat,
                    ffprobe_command=shlex.join(probe_command), ffmpeg_command=shlex.join(command))
    except Exception as exc:
        return dict(path=str(path), error=f'{type(exc).__name__}: {exc}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    tool = Path(__file__).resolve()
    tool_hash = hashlib.sha256(tool.read_bytes()).hexdigest()
    output, root = args.output.resolve(), args.input.resolve()
    if not output.is_relative_to(tool.parent) or output == tool:
        parser.error('--output must be inside the script scratchpad and cannot overwrite the tool')
    files = sorted(root.rglob('*.webm')) if root.is_dir() else (
        [root] if root.is_file() and root.suffix.lower() == '.webm' else [])
    if output in files:
        parser.error('--output cannot overwrite an input recording')
    report = dict(root=str(root), file_count=len(files), total_frames_decoded=0,
                  total_episode_count=0, fps_mismatches=[], per_file=[],
                  tool_path=str(tool), tool_sha256=tool_hash,
                  command_line=shlex.join([sys.executable, *sys.argv]),
                  command_argv=[sys.executable, *sys.argv], working_directory=str(Path.cwd()),
                  methodology=dict(native_frames=True, resampling=False, pixel_format='rgb24',
                                   pad_rgb=[221, 221, 221], pad_tolerance_per_channel=10,
                                   grey_pad_threshold=0.85, flat_colour_threshold=0.90,
                                   quantization='floor RGB / 4, 6 bits/channel', expected_fps=25.0,
                                   startup_episode_max_frames=STARTUP_EPISODE_MAX_FRAMES,
                                   total_flagged_frac_cap=TOTAL_FLAGGED_FRAC_CAP,
                                   frame_read_timeout_seconds=FRAME_READ_TIMEOUT_SECONDS,
                                   min_frame_coverage=MIN_FRAME_COVERAGE,
                                   verdict_rule=('zero grey-pad episodes AND zero flat-colour episodes that are '
                                                 'either not at frame 0 or longer than startup_episode_max_frames '
                                                 'AND every file\'s total_flagged_frac <= total_flagged_frac_cap; '
                                                 'integrity errors force ERROR')),
                  versions=dict(python=sys.version, numpy=np.__version__), verification_errors=[])
    for binary in ('ffmpeg', 'ffprobe'):
        try:
            info = subprocess.run([binary, '-version'], capture_output=True, text=True, check=True)
            report['versions'][binary] = info.stdout.splitlines()[0]
        except Exception as exc:
            report['verification_errors'].append(f'{binary}: {exc}')
    if not files:
        report['verification_errors'].append('NO .webm FILES FOUND; refusing empty input')
    if not report['verification_errors']:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for item in pool.map(verify, files):
                report['per_file'].append(item)
                if 'error' in item:
                    report['verification_errors'].append(f"{item['path']}: {item['error']}")
                    continue
                print(f"{item['path']}: {item['decoded_frame_count']}/{item['ffprobe_exact_frame_count']} frames", file=sys.stderr, flush=True)
                for key, label in (('grey_pad_episodes', 'grey-pad'), ('flat_colour_episodes', 'flat-colour')):
                    for episode in item[key]:
                        if label == 'grey-pad':
                            state = 'FAIL[grey-pad]'
                        elif episode['start_frame'] != 0:
                            state = 'FAIL[flat-colour-not-at-frame0]'
                        elif episode['length'] > STARTUP_EPISODE_MAX_FRAMES:
                            state = f'FAIL[flat-colour-startup-exceeds-cap:{episode["length"]}>{STARTUP_EPISODE_MAX_FRAMES}]'
                        else:
                            state = 'startup (disclosed, allowed)'
                        print(f"  {state}: {label} start_frame={episode['start_frame']} length={episode['length']} start_ms={episode['start_ms']} max_grey_frac={episode['max_grey_frac']} max_flat_frac={episode['max_flat_frac']}", file=sys.stderr, flush=True)
                if item['total_flagged_frac_over_cap']:
                    print(f"  FAIL[total-flagged-frac]: {item['total_flagged_frac']:.4f} > cap {TOTAL_FLAGGED_FRAC_CAP} "
                          f"({item['total_flagged_frames']}/{item['decoded_frame_count']} frames grey-pad-or-flat-colour)",
                          file=sys.stderr, flush=True)
    valid = [item for item in report['per_file'] if 'error' not in item]
    report['files_verified'] = len(valid)
    report['total_frames_decoded'] = sum(item['decoded_frame_count'] for item in valid)
    report['total_episode_count'] = sum(item['episode_count'] for item in valid)
    report['fps_mismatches'] = [dict(path=item['path'], fps=item['fps'], avg_frame_rate=item['avg_frame_rate'],
                                        r_frame_rate=item['r_frame_rate']) for item in valid if item['fps_mismatch']]
    report['frame_count_mismatch'] = [dict(path=item['path'], decoded_frame_count=item['decoded_frame_count'],
                                             ffprobe_exact_frame_count=item['ffprobe_exact_frame_count'])
                                     for item in valid if item['frame_count_mismatch']]
    report['truncated_input'] = [dict(path=item['path'], decoded_frame_count=item['decoded_frame_count'],
                                          expected_frame_count=item['expected_frame_count'],
                                          frame_coverage=item['frame_coverage'])
                                 for item in valid if item['truncated_input']]
    for key in ('grey_pad_episode_count', 'flat_colour_episode_count',
                'flat_colour_startup_episode_count', 'flat_colour_nonstartup_episode_count'):
        report['total_' + key] = sum(item[key] for item in valid)
    for key in ('max_grey_frac_overall', 'max_flat_frac_overall'):
        report[key] = max((item[key] for item in valid), default=None)
    report['total_flagged_frac_over_cap'] = [
        dict(path=item['path'], total_flagged_frac=item['total_flagged_frac'], total_flagged_frames=item['total_flagged_frames'])
        for item in valid if item['total_flagged_frac_over_cap']]
    if report['fps_mismatches']:
        report['verification_errors'].append('fps_mismatches: expected exactly 25.00fps')
    if report['frame_count_mismatch']:
        report['verification_errors'].append('frame_count_mismatch: native decode differs from ffprobe -count_frames')
    if report['truncated_input']:
        report['verification_errors'].append('truncated_input: native decode covers less than min_frame_coverage of container duration')
    if hashlib.sha256(tool.read_bytes()).hexdigest() != tool_hash:
        report['verification_errors'].append('tool changed while running; provenance invalid')
    if report['verification_errors']:
        verdict, code = 'ERROR', 2
    elif (report['total_grey_pad_episode_count'] or report['total_flat_colour_nonstartup_episode_count']
          or report['total_flagged_frac_over_cap']):
        verdict, code = 'FAIL', 1
    else:
        verdict, code = 'PASS', 0
    report['verdict'], report['exit_code'] = verdict, code
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print(f"{verdict}: files={len(valid)}/{len(files)} frames={report['total_frames_decoded']} grey_pad_episodes={report['total_grey_pad_episode_count']} flat_colour_episodes={report['total_flat_colour_episode_count']} flat_startup_episodes={report['total_flat_colour_startup_episode_count']} flat_nonstartup_episodes={report['total_flat_colour_nonstartup_episode_count']} files_over_flagged_frac_cap={len(report['total_flagged_frac_over_cap'])} fps_mismatches={len(report['fps_mismatches'])} frame_count_mismatch={len(report['frame_count_mismatch'])} report={output}", flush=True)
    for error in report['verification_errors']:
        print(error, file=sys.stderr)
    return code


if __name__ == '__main__':
    sys.exit(main())
