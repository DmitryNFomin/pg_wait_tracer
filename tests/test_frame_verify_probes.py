#!/usr/bin/env python3
"""Real-media bypass suite for scripts/frame_verify.py (#519), without pytest.

Requires numpy and real ffmpeg/ffprobe (including libvpx-vp9) on PATH. All
fixtures, wrappers, reports, and identical tool copies live in a temporary
scratchpad: the detector deliberately restricts output to its own directory.
Run directly, or use --tool /scratch/frame_verify.py --only numpy-absence for
the dependency-refusal mutation demonstration. Every case runs on two fixtures
with different dimensions AND counts; stub metadata comes from each real file.
"""
import argparse
from copy import deepcopy
from fractions import Fraction
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import numpy as np


TOOL = Path(__file__).resolve().parents[1] / 'scripts' / 'frame_verify.py'
GREY = (221, 221, 221)
FLAT = (18, 22, 30)
FPS = 25
# Both dimensions and counts differ. Long enough that eight startup frames
# remain within the detector's separate 5% total-flagged budget.
FIXTURES = ((64, 48, 180), (96, 64, 280))
tests_run = 0
tests_failed = 0


def report(ok, msg):
    global tests_run, tests_failed
    tests_run += 1
    if ok:
        print(f'  PASS: {msg}', flush=True)
    else:
        tests_failed += 1
        print(f'  FAIL: {msg}', flush=True)


def case(msg, check):
    try:
        detail = check()
        report(True, msg + (f' ({detail})' if detail else ''))
    except Exception as exc:
        report(False, f'{msg}: {type(exc).__name__}: {exc}')


def require(ok, msg):
    if not ok:
        raise AssertionError(msg)


def checked(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, timeout=90, **kwargs)


def probe(path, ffprobe):
    result = checked([ffprobe, '-v', 'error', '-count_frames', '-select_streams', 'v:0',
                      '-show_entries',
                      'stream=width,height,avg_frame_rate,r_frame_rate,nb_read_frames:format=duration',
                      '-of', 'json', str(path)], text=True)
    require(not result.stderr.strip(), f'fixture ffprobe diagnostic: {result.stderr}')
    return json.loads(result.stdout)


def write_webm(path, frames, ffmpeg):
    n, height, width, _ = frames.shape
    # A small timestamp gap keeps coverage >90%, while causing the default
    # rawvideo CFR decoder to duplicate frames. Native counts stay exactly n.
    gap = round(n * 0.04)
    checked([ffmpeg, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
             '-s', f'{width}x{height}', '-r', str(FPS), '-i', 'pipe:0',
             '-vf', f'setpts=PTS+{gap}*gte(N\\,{n // 2})',
             '-fps_mode', 'passthrough', '-c:v', 'libvpx-vp9', '-lossless', '1',
             '-pix_fmt', 'yuv420p', '-threads', '1', '-cluster_time_limit', '40',
             str(path)], input=frames.tobytes())
    # Use Matroska's valid unknown-length Segment encoding (as for a streaming
    # mux), retaining the muxer's duration in Info. Cutting a complete cluster
    # can then be silent: neither decoder's stderr nor their count disagreement
    # may stand in for the independent duration/coverage check being tested.
    data = bytearray(path.read_bytes())
    offset = data.index(b'\x18\x53\x80\x67') + 4  # Segment ID, then size VINT.
    require(data[offset] == 1, 'expected ffmpeg eight-byte Segment size VINT')
    data[offset:offset + 8] = b'\x01' + b'\xff' * 7
    path.write_bytes(data)


class Harness:
    def __init__(self, root, source, ffmpeg, ffprobe):
        self.root, self.source = root, source
        self.ffmpeg, self.ffprobe = ffmpeg, ffprobe
        self.serial = 0

    def run(self, path, *, metadata=None, stubs=None, numpy_absent=False, search_path=None):
        self.serial += 1
        scratch = self.root / f'run-{self.serial}'
        scratch.mkdir()
        tool = scratch / 'frame_verify.py'
        shutil.copyfile(self.source, tool)
        output = scratch / 'report.json'
        env = os.environ.copy()
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        bins = scratch / 'bin'
        bins.mkdir()
        if metadata is not None:
            payload = json.dumps(json.dumps(metadata))
            wrapper = (f'#!{sys.executable}\nimport os, sys\n'
                       f'if "-count_frames" in sys.argv:\n    print({payload})\n'
                       f'else:\n    os.execv({self.ffprobe!r}, [{self.ffprobe!r}, *sys.argv[1:]])\n')
            stub = bins / 'ffprobe'
            stub.write_text(wrapper)
            stub.chmod(0o755)
        for binary, body in (stubs or {}).items():
            stub = bins / binary
            stub.write_text(f'#!{sys.executable}\n{body}\n')
            stub.chmod(0o755)
        env['PATH'] = str(bins) + os.pathsep + (env.get('PATH', '') if search_path is None else str(search_path))
        if numpy_absent:
            shadow = scratch / 'shadow'
            shadow.mkdir()
            (shadow / 'numpy.py').write_text('raise ImportError("stub: numpy unavailable")\n')
            env['PYTHONPATH'] = str(shadow) + os.pathsep + env.get('PYTHONPATH', '')
        result = subprocess.run([sys.executable, str(tool), str(path), '--output', str(output),
                                 '--jobs', '1'], capture_output=True, text=True, env=env,
                                timeout=90)
        doc = json.loads(output.read_text()) if output.exists() else None
        return result, doc


def expect(result, doc, code):
    require(result.returncode == code,
            f'expected exit {code}, got {result.returncode}; stdout={result.stdout!r}; stderr={result.stderr!r}')
    verdict = {0: 'PASS', 1: 'FAIL', 2: 'ERROR'}[code]
    require(doc is not None and doc.get('verdict') == verdict, f'expected {verdict} report, got {doc}')
    require(doc['exit_code'] == code, 'report exit_code disagrees with process')
    require('Traceback' not in result.stderr, f'uncaught error: {result.stderr}')


def detection_case(harness, path, code, name, cap, frac_cap):
    result, doc = harness.run(path)
    expect(result, doc, code)
    item, = doc['per_file']
    require(not doc['verification_errors'], f'unexpected verification error: {doc["verification_errors"]}')
    require(item['decoded_frame_count'] == item['ffprobe_exact_frame_count'], 'native count mismatch')
    require(item['expected_frame_count'] > 0 and item['frame_coverage'] >= 0.90,
            'healthy container duration/coverage missing')
    if name.startswith('grey-'):
        require(item['grey_pad_episode_count'] > 0, 'grey-pad detector was not reached')
    elif name.startswith('flat-'):
        require(item['flat_colour_episode_count'] > 0, 'flat-colour detector was not reached')
    if name == 'flat-frame0-short':
        require(item['flat_colour_startup_episode_count'] == 1, 'startup exemption not reached')
        require(item['total_flagged_frac'] <= frac_cap, 'startup fixture exceeds total cap')
    if name == 'flat-frame0-long':
        require(item['flat_colour_episodes'][0]['length'] > cap, 'startup cap not exceeded')
    if name == 'fragmented':
        require(len(item['episodes']) > 1 and all(e['length'] < cap for e in item['episodes']),
                'fragmented episodes must individually be sub-cap')
        require(item['total_flagged_frac'] > frac_cap and item['total_flagged_frac_over_cap']
                and doc['total_flagged_frac_over_cap'], 'fragmentation check was not reached')
    return f'exit {code}, native={item["decoded_frame_count"]}, expected={item["expected_frame_count"]}'


def numpy_case(harness, path):
    result, doc = harness.run(path, numpy_absent=True)
    # The SAME interpreter normally imports numpy above. Require evidence that
    # the shadow was reached, not merely an unrelated nonzero exit somewhere.
    require('ImportError' in result.stderr and 'stub: numpy unavailable' in result.stderr,
            f'numpy shadow was not exercised: {result.stderr!r}')
    require(result.returncode == 2, f'numpy absence must refuse with exit 2; got exit {result.returncode}')
    require(doc is None or doc.get('verdict') != 'PASS', f'numpy absence wrote PASS: {doc}')
    return 'shadow ImportError observed, exit 2, no PASS report'


def empty_case(harness, empty):
    result, doc = harness.run(empty)
    expect(result, doc, 2)
    require('PASS' not in result.stderr and 'PASS' not in json.dumps(doc), 'empty input mentioned PASS')
    require('PASS' not in result.stdout, 'empty input printed PASS')


def shape_case(harness, path, metadata, field):
    lie = deepcopy(metadata)
    stream = lie['streams'][0]
    if field == 'multiple streams':
        lie['streams'].append(deepcopy(stream))
    else:
        stream[field] = '0/1' if field == 'avg_frame_rate' else 0
    result, doc = harness.run(path, metadata=lie)
    expect(result, doc, 2)
    require(doc['verification_errors'] and 'error' in doc['per_file'][0], 'shape was not refused')


def count_lie_case(harness, path, metadata):
    lie = deepcopy(metadata)
    real = int(metadata['streams'][0]['nb_read_frames'])
    wrong = real - max(1, round(real * 0.10))
    lie['streams'][0]['nb_read_frames'] = str(wrong)
    result, doc = harness.run(path, metadata=lie)
    expect(result, doc, 2)
    item, = doc['per_file']
    require(item['decoded_frame_count'] == real and item['ffprobe_exact_frame_count'] == wrong,
            'per-artifact frame-count lie was not exercised')
    require(item['frame_count_mismatch'] and any('frame_count_mismatch' in e for e in doc['verification_errors']),
            'frame-count integrity error missing')
    return f'real={real}, lie={wrong}, exit 2'


def cfr_case(harness, path, metadata):
    stream = metadata['streams'][0]
    size = stream['width'] * stream['height'] * 3
    result = checked([harness.ffmpeg, '-v', 'error', '-xerror', '-nostdin', '-threads', '1',
                      '-i', str(path), '-map', '0:v:0', '-an', '-sn', '-dn', '-pix_fmt', 'rgb24',
                      '-threads', '1', '-f', 'rawvideo', 'pipe:1'])
    require(not result.stderr.strip() and len(result.stdout) % size == 0, 'default decode failed')
    cfr_count = len(result.stdout) // size
    result, doc = harness.run(path)
    expect(result, doc, 0)
    native = int(stream['nb_read_frames'])
    require(doc['per_file'][0]['decoded_frame_count'] == native, 'tool did not preserve native count')
    require(cfr_count > native, f'no actual CFR duplication: default={cfr_count}, native={native}')
    return f'default CFR={cfr_count} > native/tool={native}'


def truncation_case(harness, path, metadata):
    data = path.read_bytes()
    target = round(len(data) * 0.01)
    marker = b'\x1f\x43\xb6\x75'  # Cluster ID.
    boundaries, offset = [], data.find(marker)
    while offset >= 0:
        boundaries.append(offset)
        offset = data.find(marker, offset + len(marker))
    require(len(boundaries) > 1, 'fixture needs several clusters')
    cut = min(boundaries[1:], key=lambda pos: abs(pos - target))
    require(0.005 <= cut / len(data) <= 0.025, 'cluster cut is not roughly the first 1%')
    truncated = path.with_name('truncated.webm')
    truncated.write_bytes(data[:cut])
    truncated_metadata = probe(truncated, harness.ffprobe)  # MUST have no stderr.
    result, doc = harness.run(truncated)
    expect(result, doc, 2)
    item, = doc['per_file']
    actual = int(truncated_metadata['streams'][0]['nb_read_frames'])
    require(item['decoded_frame_count'] == actual and not item['frame_count_mismatch'],
            'old count-mismatch path, rather than coverage, caught truncation')
    expected = round(Fraction(metadata['format']['duration']) * Fraction(metadata['streams'][0]['avg_frame_rate']))
    require(item['expected_frame_count'] == expected and expected > actual,
            'expected count must come from surviving original duration metadata')
    require(item['frame_coverage'] == actual / expected and item['frame_coverage'] < 0.90,
            'missing or incorrect frame coverage')
    require(item['truncated_input'] and doc['truncated_input'], 'truncated_input report missing')
    require(len(doc['verification_errors']) == 1 and 'truncated_input' in doc['verification_errors'][0],
            f'not solely the new coverage defence: {doc["verification_errors"]}')
    return f'{cut}/{len(data)} bytes, probe=tool={actual}, expected={expected}, coverage={item["frame_coverage"]:.2%}, exit 2'


def dependency_case(harness, path, binary, missing):
    if missing:
        # Only the OTHER real binary is resolvable. sys.executable is absolute,
        # so the detector still launches and must catch the dependency ENOENT.
        harness.serial += 1
        bins = harness.root / f'missing-{harness.serial}'
        bins.mkdir()
        other = 'ffprobe' if binary == 'ffmpeg' else 'ffmpeg'
        (bins / other).symlink_to(getattr(harness, other))
        result, doc = harness.run(path, search_path=bins)
    else:
        result, doc = harness.run(path, stubs={binary: 'import sys\nsys.exit(126)'})
    expect(result, doc, 2)
    require(any(binary in error for error in doc['verification_errors']), 'dependency refusal missing')
    require(not doc['per_file'], 'broken dependency should refuse before verifying files')


def decode_case(harness, path, zero):
    body = (f'import os, sys\n'
            f'if "-version" in sys.argv:\n    os.execv({harness.ffmpeg!r}, [{harness.ffmpeg!r}, *sys.argv[1:]])\n'
            f'else:\n    sys.exit({0 if zero else 1})')
    result, doc = harness.run(path, stubs={'ffmpeg': body})
    expect(result, doc, 2)
    needle = 'zero frames' if zero else 'ffmpeg exited'
    require(any(needle in error for error in doc['verification_errors']), 'decode refusal not reached')


def fps_case(harness, path, metadata):
    lie = deepcopy(metadata)
    real_fps = Fraction(lie['streams'][0]['avg_frame_rate'])
    wrong = real_fps * 2
    for key in ('avg_frame_rate', 'r_frame_rate'):
        lie['streams'][0][key] = str(wrong)
    result, doc = harness.run(path, metadata=lie)
    expect(result, doc, 2)
    require(doc['fps_mismatches'] and any('fps_mismatches' in e for e in doc['verification_errors']),
            'fps mismatch was not caught')


def duration_lie_case(harness, path, metadata):
    lie = deepcopy(metadata)
    lie['format']['duration'] = str(float(metadata['format']['duration']) * 2)
    result, doc = harness.run(path, metadata=lie)
    expect(result, doc, 2)
    item, = doc['per_file']
    require(not item['frame_count_mismatch'] and item['truncated_input'], 'duration coverage not checked')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, default=TOOL)
    parser.add_argument('--only', choices=['numpy-absence'])
    args = parser.parse_args()
    ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
    if not ffmpeg or not ffprobe:
        report(False, 'real ffmpeg and ffprobe must be installed on PATH')
        return 1
    # Read constants without importing the tool (which would bypass testing its
    # actual subprocess dependency handling). No bytecode or repo output files.
    constants = {}
    for line in args.tool.read_text().splitlines():
        if line.startswith(('STARTUP_EPISODE_MAX_FRAMES =', 'TOTAL_FLAGGED_FRAC_CAP =')):
            key, value = line.split('=', 1)
            constants[key.strip()] = float(value.split('#')[0].strip())
    cap = int(constants['STARTUP_EPISODE_MAX_FRAMES'])
    frac_cap = constants['TOTAL_FLAGGED_FRAC_CAP']
    with tempfile.TemporaryDirectory(prefix='frame-verify-probes-') as tmp:
        root = Path(tmp)
        harness = Harness(root, args.tool.resolve(), ffmpeg, ffprobe)
        for width, height, count in FIXTURES:
            label = f'{width}x{height}/{count} frames'
            fixtures = root / f'fixtures-{width}-{height}-{count}'
            fixtures.mkdir()
            rng = np.random.default_rng(seed=519 + count)
            noisy = rng.integers(0, 256, (count, height, width, 3), dtype=np.uint8)
            healthy = fixtures / 'healthy.webm'
            write_webm(healthy, noisy, ffmpeg)
            metadata = probe(healthy, ffprobe)
            stream, = metadata['streams']
            require((stream['width'], stream['height'], int(stream['nb_read_frames'])) == (width, height, count),
                    'encoded fixture properties do not match requested distinct shapes/counts')
            print(f'Fixture: {label}; {healthy.stat().st_size} bytes', flush=True)
            case(f'{label}: numpy absence (same interpreter, PYTHONPATH shadow)',
                 lambda: numpy_case(harness, healthy))
            if args.only:
                continue
            masks = {
                'flat-wholefile': (np.ones(count, dtype=bool), FLAT, 1),
                'flat-midstream': ((np.arange(count) >= count // 3) & (np.arange(count) < count // 3 + cap // 2), FLAT, 1),
                'grey-midstream': ((np.arange(count) >= count // 3) & (np.arange(count) < count // 3 + cap // 2), GREY, 1),
                'grey-frame0': (np.arange(count) < min(8, cap), GREY, 1),
                'flat-frame0-long': (np.arange(count) < cap + 1, FLAT, 1),
                'flat-frame0-short': (np.arange(count) < min(8, cap), FLAT, 0),
                'fragmented': ((np.arange(count) % cap >= cap // 2) & (np.arange(count) % cap < cap // 2 + 4), FLAT, 1),
            }
            case(f'{label}: healthy', lambda: detection_case(harness, healthy, 0, 'healthy', cap, frac_cap))
            for name, (mask, colour, code) in masks.items():
                frames = noisy.copy()
                frames[mask] = colour
                path = fixtures / f'{name}.webm'
                write_webm(path, frames, ffmpeg)
                case(f'{label}: {name}', lambda: detection_case(harness, path, code, name, cap, frac_cap))
            empty = fixtures / 'empty'
            empty.mkdir()
            case(f'{label}: no input', lambda: empty_case(harness, empty))
            for field in ('width', 'height', 'avg_frame_rate', 'multiple streams'):
                case(f'{label}: unaccountable shape ({field})',
                     lambda: shape_case(harness, healthy, metadata, field))
            case(f'{label}: ffprobe frame-count lie', lambda: count_lie_case(harness, healthy, metadata))
            case(f'{label}: default CFR duplicates / tool preserves native frames',
                 lambda: cfr_case(harness, healthy, metadata))
            case(f'{label}: silent truncated input', lambda: truncation_case(harness, healthy, metadata))
            for binary in ('ffmpeg', 'ffprobe'):
                for missing in (True, False):
                    case(f'{label}: {binary} {"missing" if missing else "broken"}',
                         lambda: dependency_case(harness, healthy, binary, missing))
            for zero in (True, False):
                case(f'{label}: {"zero frames" if zero else "decode exception"}',
                     lambda: decode_case(harness, healthy, zero))
            case(f'{label}: fps mismatch', lambda: fps_case(harness, healthy, metadata))
            case(f'{label}: duration lie (measured duration doubled)',
                 lambda: duration_lie_case(harness, healthy, metadata))
    print(f'frame-verify probe suite: {tests_run - tests_failed}/{tests_run} passed', flush=True)
    return 1 if tests_failed else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        report(False, f'fixture/setup error: {type(exc).__name__}: {exc}')
        sys.exit(1)
