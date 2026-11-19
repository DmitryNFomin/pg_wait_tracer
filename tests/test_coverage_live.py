#!/usr/bin/env python3
"""#947 U1 live: one loss-witness rule on every Linux kernel.

A five-second quiet control proves W advances before each load. Independent
ringbuf_drops_total deltas select freshness/no-drop or per-stalled-tick drop
pairs with a gap chain to the load endpoint. Boot 1 is killed under load;
boot 2 demonstrates recovery after load, then seals on SIGTERM; boot 3 forces
heartbeat-only ticks. Files are grouped by nonce and read in file_no order.

Quiet-leg floor lags, coverage and TOTAL trace bytes/hour (including the
per-second publish), and tick CPU are printed for the lead to judge. The
payload/CRC decoder is independent of the C codec. -i 5 tests timer autonomy.
--self-test exercises the gate itself without Linux, root, PostgreSQL or BPF.
"""
import argparse
import ctypes
import ctypes.util
from collections import Counter
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time

from test_init_hint_recovery import ctl, wait_for
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / "pg_wait_tracer"
PSQL = ["psql", "-X", "-U", "postgres", "-qAt", "-v", "ON_ERROR_STOP=1"]
DB = "pgwt_coverage_live"
SEC = 1_000_000_000
TICK_NS = SEC
# Healthy loop drains cross a reservation fence within one tick. A record
# arriving just after P can wait one tick for a fence and one more for its
# promotion: at most 2 ticks. Allow half a tick for timer scheduling, floor
# reads and publication (the existing 2.5 s tick-spacing gate). Compare with
# the LAST TICK'S observation, so kill/control-socket latency is excluded.
FRESHNESS_NS = 2 * TICK_NS + TICK_NS // 2

FILE_HDR = struct.Struct("<IHHIQQQ")        # v4/v6: 36 bytes
BLOCK_HDR = struct.Struct("<QQIIIHHQQ")     # v4/v6: 48 bytes
FLAG_CLOSED_CLEAN = 0x0004
FLAG_RECOVERED = 0x0010
KINDS = {1: "BOOT", 2: "WATERMARK", 3: "GAP_OPEN", 4: "GAP_CLOSE", 5: "SEAL"}


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def data_timestamps(payload, usize, count):
    """v6 TRANSITIONS and SAMPLES share the first delta-varint column.

    Decode every record, including mixed blocks that straddle W. Block
    first/last timestamps cannot count individual late reservations.
    """
    assert 0 < usize <= 10 * 1024 * 1024, ("uncompressed size", usize)
    if not hasattr(data_timestamps, "lz4"):
        library = ctypes.util.find_library("lz4")
        assert library, "liblz4 required for the file-order completeness scan"
        lz4 = ctypes.CDLL(library)
        lz4.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_void_p,
                                            ctypes.c_int, ctypes.c_int]
        lz4.LZ4_decompress_safe.restype = ctypes.c_int
        data_timestamps.lz4 = lz4
    out = ctypes.create_string_buffer(usize)
    size = data_timestamps.lz4.LZ4_decompress_safe(payload, out, len(payload), usize)
    assert size == usize, ("LZ4 data block", size, usize)
    data, offset, previous = out.raw, 0, 0
    for _ in range(count):
        delta = 0
        for shift in range(0, 70, 7):
            assert offset < size, "truncated timestamp varint"
            byte = data[offset]
            offset += 1
            assert shift < 63 or byte <= 1, "timestamp varint overflow"
            delta |= (byte & 127) << shift
            if not byte & 128:
                break
        else:
            raise AssertionError("unterminated timestamp varint")
        previous = (previous + delta) & ((1 << 64) - 1)
        yield previous


def coverage_records(path, scan=None):
    """[(observation, kind, reason, a, b, c, d, nonce)] in file order, the
    header flags and the number of data blocks. Walks blocks from the header
    until the bytes run out (a live file has no footer; an archive's footer
    fails the block sanity test and ends the walk)."""
    blob = Path(path).read_bytes()
    magic, version, flags = FILE_HDR.unpack_from(blob)[:3]
    assert magic == 0x54574750 and version == 6, (path, version)
    off, recs, data = FILE_HDR.size, [], 0
    while off + BLOCK_HDR.size <= len(blob):
        first, last, nev, csize, usize, btype = BLOCK_HDR.unpack_from(blob, off)[:6]
        if nev == 0 or nev > 4096 or csize == 0 or csize > 10 * 1024 * 1024:
            break
        end = off + BLOCK_HDR.size + csize
        if end > len(blob):
            break
        if btype == 3:
            p = blob[off + BLOCK_HDR.size:end]
            assert first == last and nev == 1 and csize == usize, path
            assert len(p) in (48, 80), len(p)
            pm, schema, kind, reason, a, b, c, d = struct.unpack_from("<IHBBQQQQ", p)
            assert pm == 0x43574750 and schema == 1 and kind in KINDS, (pm, kind)
            crc, pad = struct.unpack_from("<II", p, len(p) - 8)
            assert crc == crc32c(p[:len(p) - 8]) and pad == 0, "COVERAGE CRC"
            assert (len(p) == 80) == (kind == 1), "BOOT is 80 bytes, others 48"
            nonce = p[40:56] if kind == 1 else None
            recs.append((first, KINDS[kind], reason, a, b, c, d, nonce))
            if scan is not None and kind == 2:
                scan["W"] = max(scan["W"], a)
        else:
            data += 1
            if scan is not None:
                assert btype in (1, 2), ("unknown data block", btype)
                for timestamp in data_timestamps(blob[off + BLOCK_HDR.size:end], usize, nev):
                    scan["data"] += 1
                    if timestamp <= scan["W"]:
                        scan["late"] += 1
        off = end
    return recs, flags, data


def report_leg(recs, files, log, label, start_ns=0, end_ns=None):
    """Scan a boot in file_no order; count each late data record once.

    W persists across rotation BOOTs and repeated heartbeats. Any data record
    timestamp <= the latest preceding WATERMARK is the residual from item 6,
    whether it was reserved late or delayed by some other capture path.
    """
    scan = {"W": -1, "late": 0, "data": 0}
    for _, path, _, _ in files:
        coverage_records(path, scan)
    wms = watermarks(recs)
    previous = recs[0][3]
    heartbeats = promotions = 0
    for obs, w, _, _ in wms:
        if start_ns <= obs and (end_ns is None or obs <= end_ns):
            heartbeats += w == previous
            promotions += w > previous
        previous = w
    ticks = [tuple(map(int, m)) for m in re.findall(
        r"COVERAGE-TICK: obs=(\d+) W=(\d+) fence=(\d+) consumer=(\d+) "
        r"promotions=\d+ heartbeats=\d+", log)]
    ticks = [t for t in ticks if start_ns <= t[0] <= (end_ns or wms[-1][0])]
    assert ticks, (label, "position diagnostics for every leg")
    lag = [max(0, fence - consumer) for _, _, fence, consumer in ticks]
    print(f"coverage live {label}: heartbeats={heartbeats} promotions={promotions}; "
          f"fence={ticks[-1][2]} consumer={ticks[-1][3]} "
          f"fence_consumer_lag_bytes last={lag[-1]} max={max(lag)}; "
          f"late_after_W={scan['late']} data_records={scan['data']}", flush=True)
    assert scan["late"] == 0, (label, "data timestamp <= preceding WATERMARK", scan)
    return scan


def find_pgbench():
    for cand in [shutil.which("pgbench")] + sorted(
            str(p) for p in Path("/usr").glob("pgsql-*/bin/pgbench")) + sorted(
            str(p) for p in Path("/usr/lib/postgresql").glob("*/bin/pgbench")):
        if cand and os.access(cand, os.X_OK):
            return cand
    raise AssertionError("pgbench not found")


def sql(statement, db="postgres"):
    return subprocess.check_output([*PSQL, "-d", db, "-c", statement],
                                   text=True, timeout=60).strip()


def start_daemon(pm_pid, directory, log, env=None):
    cmd = [str(TRACER), "--mode", "full", "--pid", str(pm_pid), "-T",
           str(directory), "-i", "5", "-q"]
    print("coverage live: " + " ".join(cmd), flush=True)
    err = open(log, "w")
    d = subprocess.Popen(cmd, stderr=err, stdout=subprocess.DEVNULL,
                         env={**os.environ, "PGWT_DEBUG_DUMP_STATE": "1", **(env or {})})
    wait_for(d, lambda: (directory / "pgwt.sock").exists(), 45,
             "control socket not ready")
    return d, err


def pgbench(pgb, seconds):
    return subprocess.Popen([pgb, "-n", "-c", "16", "-j", "4", "-T",
                             str(seconds), "-U", "postgres", DB],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True)


def idle_psql():
    # Connected and idle in ClientRead for the whole boot.
    return subprocess.Popen([*PSQL, "-d", DB], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            text=True)


def pct(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(round(q * (len(v) - 1))))]


def report_lags(wms, label):
    settled = wms[3:] if len(wms) > 6 else wms
    db = [(w - fd) / 1e6 for (_, w, fd, fi) in settled]
    idle = [(w - fi) / 1e6 for (_, w, fd, fi) in settled]
    print(f"coverage live MEASURED {label}: W-floor_db lag ms "
          f"median={statistics.median(db):.1f} p95={pct(db, .95):.1f} "
          f"max={max(db):.1f}; W-floor_idle lag ms "
          f"median={statistics.median(idle):.1f} p95={pct(idle, .95):.1f} "
          f"max={max(idle):.1f} (n={len(settled)})", flush=True)


def watermarks(recs):
    return [(obs, a, b, c) for (obs, k, _r, a, b, c, _d, _n) in recs
            if k == "WATERMARK"]


REASONS = {0: "none", 1: "pause", 2: "drop", 3: "revoked", 4: "counters_unknown"}


def boot_files(directory, nonce=None):
    """Read each boot's files in file_no order, regardless of archive name.

    Called at quiet checkpoints or after stopping, never by retrying a failed
    assertion. A rotation contributes another BOOT, not another daemon boot.
    Returns records and (file_no, path, flags, bytes) for the selected boot.
    """
    boots = {}
    paths = list(directory.glob("*.trace.lz4"))
    current = directory / "current.trace"
    if current.exists():
        paths.append(current)
    for path in paths:
        recs, flags, _ = coverage_records(path)
        assert recs and recs[0][1] == "BOOT", (path, recs[:1])
        boot = recs[0]
        boots.setdefault(boot[7], []).append((boot[4], path, flags,
                                              path.stat().st_size, recs))
    if nonce is None:
        nonce = max(boots, key=lambda n: boots[n][0][4][0][3])
    files = sorted(boots[nonce], key=lambda f: f[0])
    assert [f[0] for f in files] == list(range(len(files))), \
        ("contiguous file_no from zero", [(f[0], str(f[1])) for f in files])
    assert len({f[4][0][3] for f in files}) == 1, "one boot_start per nonce"
    recs = [r for f in files for r in f[4]]
    return recs, [f[:4] for f in files]


def drop_counter(directory):
    value = ctl(directory).get("ringbuf_drops_total")
    assert type(value) is int and value >= 0, ("independent drop witness", value)
    return value


def drop_counter_tick(d, directory):
    """Read the witness, then await its next coverage tick, even if W repeats.

    A pending drop just before this read belongs to that tick. At the start
    boundary it must be written before we start checking load observations;
    at the end boundary it must be written before we kill/read the daemon.
    Poll for a new observation, never retry a coverage assertion.
    """
    count = drop_counter(directory)
    read_ns = time.monotonic_ns()

    def next_tick():
        recs, _ = boot_files(directory)
        return any(obs > read_ns for obs, *_ in watermarks(recs))

    wait_for(d, next_tick, 5, "no coverage tick after drop-counter read")
    return count


def quiet_leg(d, directory, label, after=None, log=None):
    time.sleep(5)
    assert d.poll() is None, (label, "daemon died during quiet control")
    recs, files = boot_files(directory)
    wms = watermarks(recs)
    if log is not None:
        report_leg(recs, files, Path(log).read_text(), label + " QUIET leg", start_ns=after or 0)
    assert len(wms) >= 3 and wms[-1][1] > recs[0][3], \
        (label, "quiet control must advance W beyond boot_start", wms)
    if after is not None:
        assert wms[-1][1] > after, (label, "W must resume past load end", wms[-1], after)
    report_lags([w for w in wms if w[0] >= (after or recs[0][3])], label + " QUIET leg")
    return recs


def assert_load_coverage(recs, start_ns, end_ns, delta, label):
    """The drop witness selects a rule; an unexplained stall always fails."""
    wms = watermarks(recs)
    assert wms, (label, "watermarks missing")
    assert all(b[1] >= a[1] for a, b in zip(wms, wms[1:])), "W monotone"
    assert all(fi <= fd <= w for _, w, fd, fi in wms), "floor_idle <= floor_db <= W"
    kinds = Counter(r[1] for r in recs)
    reasons = Counter(REASONS.get(r[2], str(r[2])) for r in recs if r[1] == "GAP_OPEN")
    branch = "no drops: fresh W, no drop gap" if delta == 0 else "drops: paired ticks and gap chain"
    print(f"coverage live {label}: ringbuf_drops_total delta={delta}; "
          f"kinds={dict(kinds)} gap_reasons={dict(reasons)}; branch={branch}", flush=True)
    assert delta >= 0, "drop counter monotone"
    opened, pairs = {}, []
    for r in recs:
        if r[1] == "GAP_OPEN":
            assert r[5] not in opened, ("unique gap id", r)
            opened[r[5]] = r
        elif r[1] == "GAP_CLOSE":
            op = opened.pop(r[5], None)
            assert op is not None and op[3] <= r[3], ("paired gap bounds", op, r)
            pairs.append((op, r))
    loaded = [w for w in wms if start_ns <= w[0] <= end_ns]
    assert len(loaded) >= 8, (label, "load exercises several ticks", len(loaded))
    lag_s = (loaded[-1][0] - loaded[-1][1]) / SEC
    print(f"coverage live MEASURED {label}: last W lag={lag_s:.3f}s", flush=True)
    drop_pairs = [(op, cl) for op, cl in pairs if op[2] == 2 and start_ns <= op[0] <= end_ns]
    if delta == 0:
        assert 0 <= lag_s <= FRESHNESS_NS / SEC, (label, "fresh W within 2 ticks + 0.5 s of last observation", lag_s)
        drop_opens = [r for r in recs if r[1] == "GAP_OPEN" and r[2] == 2
                      and start_ns <= r[0] <= end_ns]
        assert not drop_opens, (label, "no drop gap without an independent drop", drop_opens)
    else:
        assert drop_pairs, (label, "witnessed loss must have paired drop gaps")
        previous = max((w for w in wms if w[0] < start_ns), default=None)
        assert previous is not None, "quiet control supplies the pre-load W"
        for tick in loaded:
            if previous is not None and tick[1] == previous[1]:
                tick_pairs = [(op, cl) for op, cl in drop_pairs if op[0] == tick[0] == cl[0]]
                assert len(tick_pairs) == 1, (label, "every stalled tick needs its own drop pair", tick)
                assert tick_pairs[0][0][3] == tick[1], "gap opens at stalled W"
            previous = tick
        # Merge half-open drop intervals. Merely finding SOME gap cannot
        # excuse a stall: the chain must reach the freshness bound.
        through = loaded[-1][1]
        for op, cl in sorted(drop_pairs, key=lambda pair: pair[0][3]):
            if op[3] <= through:
                through = max(through, cl[3])
        assert through >= loaded[-1][0] - FRESHNESS_NS, (label, "drop gaps must chain from last W to last observation - freshness bound", through, loaded[-1][0])
    return branch


def coverage_stats(log):
    lines = [line for line in log.splitlines() if line.startswith("INFO: coverage: ")]
    assert lines, "the daemon's coverage shutdown line"
    return dict(re.findall(r"([a-zA-Z_]+)=([0-9]+)", lines[-1]))


def self_test():
    import contextlib
    import io

    def rec(t, kind, a=0, c=0, reason=0, b=0, nonce=None):
        return (int(t * SEC), kind, reason, int(a * SEC), b, c, 0, nonce)

    quiet = [rec(0, "BOOT")] + [rec(t, "WATERMARK", t - 1) for t in range(1, 5)]
    clean = quiet + [rec(t, "WATERMARK", t - 1) for t in range(5, 17)]
    stalled = list(quiet)
    for t in range(5, 17):
        stalled += [rec(t, "GAP_OPEN", 3, c=t, reason=2),
                    rec(t, "GAP_CLOSE", t, c=t), rec(t, "WATERMARK", 3)]
    assert_load_coverage(clean, 5 * SEC, int(16.25 * SEC), 0, "self clean")
    assert_load_coverage(stalled, 5 * SEC, int(16.25 * SEC), 12, "self drops")
    resumed = stalled + [rec(17, "WATERMARK", 4)] + [rec(t, "WATERMARK", t - 1) for t in range(18, 22)]
    assert_load_coverage(resumed, 5 * SEC, int(21.25 * SEC), 12, "self resumed")

    # Delaying SIGKILL/control I/O must not change a last-tick verdict.
    assert_load_coverage(clean, 5 * SEC, 30 * SEC, 0, "self delayed kill")
    at_bound = clean[:-2] + [rec(15, "WATERMARK", 13.5), rec(16, "WATERMARK", 13.5)]
    assert_load_coverage(at_bound, 5 * SEC, 30 * SEC, 0, "self two ticks plus margin")

    def refused(records, delta, label):
        with contextlib.redirect_stdout(io.StringIO()):
            try:
                assert_load_coverage(records, 5 * SEC, int(16.25 * SEC), delta, label)
            except AssertionError:
                return
        raise AssertionError("live gate accepted " + label)

    refused(stalled, 0, "unwitnessed stall")
    refused(clean[:-3] + [rec(t, "WATERMARK", 13.49) for t in (14, 15, 16)],
            0, "last observation beyond two ticks plus margin")
    refused(clean + [rec(6, "GAP_OPEN", 3, c=99, reason=2)], 0, "unclosed false drop")
    refused(clean + [rec(6, "GAP_OPEN", 3, c=99, reason=2), rec(6, "GAP_CLOSE", 6, c=99)], 0, "false drop comparison")
    refused([r for r in stalled if not (r[0] == 10 * SEC and r[1].startswith("GAP_"))], 12, "one stalled tick without its own pair")
    refused([r if r[1] != "GAP_CLOSE" else (*r[:3], int(3.5 * SEC), *r[4:]) for r in stalled], 12, "short gap chain")
    refused([r if r[1] != "GAP_OPEN" else (*r[:2], 4, *r[3:]) for r in stalled], 12, "unknown counters instead of drops")
    refused(clean + [rec(16.1, "WATERMARK", 1)], 0, "W moves backwards")
    assert coverage_stats("INFO: unrelated\nINFO: coverage: ticks=22 bytes=123 tick_cpu_ns=4 W_final=999\n") == {
        "ticks": "22", "bytes": "123", "tick_cpu_ns": "4", "W_final": "999"}

    # A permanently stuck implementation must fail the quiet controls even
    # when its load records have otherwise well-formed drop gaps.
    from unittest.mock import patch
    class Alive:
        def poll(self):
            return None

    # The read boundary must wait for an observation, not for W to advance:
    # losses and forced yields deliberately repeat W. A tick already in the
    # file when the witness was read does not satisfy the wait.
    snapshots = iter([quiet, quiet + [rec(5, "WATERMARK", 3)]])
    calls = []
    def read_witness(_):
        calls.append("counter")
        return 7
    def await_tick(d, predicate, seconds, reason):
        assert isinstance(d, Alive) and seconds == 5 and reason
        assert calls == ["counter"]
        assert not predicate(), "old tick must not satisfy the boundary"
        assert predicate(), "heartbeat observation must satisfy the boundary"
        calls.append("tick")
    with patch.object(time, "monotonic_ns", return_value=int(4.5 * SEC)), \
         patch.dict(globals(), {"drop_counter": read_witness, "wait_for": await_tick,
                               "boot_files": lambda _: (next(snapshots), [])}):
        assert drop_counter_tick(Alive(), None) == 7
    assert calls == ["counter", "tick"]

    for records, after in [(quiet[:1] + [rec(t, "WATERMARK", 0) for t in range(1, 6)], None),
                           (stalled, 16 * SEC)]:
        with patch.object(time, "sleep"), patch.dict(globals(), {"boot_files": lambda _: (records, [])}):
            try:
                quiet_leg(Alive(), None, "self stuck quiet", after)
            except AssertionError:
                pass
            else:
                raise AssertionError("quiet control accepted a stuck W")

    def block(r):
        obs, kind, reason, a, b, c, d, nonce = r
        payload = struct.pack("<IHBBQQQQ", 0x43574750, 1,
                              list(KINDS.values()).index(kind) + 1, reason, a, b, c, d)
        if kind == "BOOT":
            payload += nonce + bytes(16)
        payload += struct.pack("<II", crc32c(payload), 0)
        return BLOCK_HDR.pack(obs, obs, 1, len(payload), len(payload), 3, 0, 0, 0) + payload

    with tempfile.TemporaryDirectory(prefix="pgwt-coverage-gate-") as tmp:
        directory = Path(tmp)
        n1, n2 = bytes(range(16)), bytes(range(1, 17))
        # Lexical archive order intentionally disagrees with file_no order.
        first = [rec(1, "BOOT", a=0, b=0, nonce=n1), rec(2, "WATERMARK", 1)]
        second = [rec(3, "BOOT", a=0, b=1, nonce=n1), rec(4, "WATERMARK", 3)]
        latest = [rec(5, "BOOT", a=5, b=0, nonce=n2), rec(6, "WATERMARK", 5)]
        for name, records in [("z.trace.lz4", first), ("a.trace.lz4", second), ("current.trace", latest)]:
            (directory / name).write_bytes(FILE_HDR.pack(0x54574750, 6, 0, 18, 0, 0, 0) + b"".join(map(block, records)))
        records, files = boot_files(directory, n1)
        assert records == first + second and [f[0] for f in files] == [0, 1], "read rotated boot in file_no order"
        assert boot_files(directory)[0] == latest, "select latest boot by start, not filename"
        # Rotation may leave only archives until the next lazy file open.
        current_blob = (directory / "current.trace").read_bytes()
        (directory / "current.trace").unlink()
        assert boot_files(directory, n1)[0] == first + second, "rotation without current.trace"
        (directory / "current.trace").write_bytes(current_blob)
        corrupt = bytearray((directory / "current.trace").read_bytes())
        corrupt[-8] ^= 1
        (directory / "current.trace").write_bytes(corrupt)
        try:
            coverage_records(directory / "current.trace")
        except AssertionError as exc:
            assert "CRC" in str(exc)
        else:
            raise AssertionError("corrupt COVERAGE accepted")
    def data_block(timestamps, kind=1):
        # Independent LZ4 literals-only block and unsigned delta-varints;
        # only the shared timestamp column is needed by this scanner.
        raw, previous = bytearray(), 0
        for timestamp in timestamps:
            delta = (timestamp - previous) & ((1 << 64) - 1)
            previous = timestamp
            while delta >= 128:
                raw.append((delta & 127) | 128)
                delta >>= 7
            raw.append(delta)
        size = len(raw)
        payload = bytearray([min(15, size) << 4])
        if size >= 15:
            extra = size - 15
            while extra >= 255:
                payload.append(255)
                extra -= 255
            payload.append(extra)
        payload += raw
        return BLOCK_HDR.pack(timestamps[0], timestamps[-1], len(timestamps),
                              len(payload), size, kind, 0, 0, 0) + payload

    def tick_log(records):
        return "\n".join(f"COVERAGE-TICK: obs={obs} W={w} fence=800 consumer=792 "
                         "promotions=1 heartbeats=1" for obs, w, _, _ in watermarks(records))

    with tempfile.TemporaryDirectory(prefix="pgwt-coverage-order-") as tmp:
        directory = Path(tmp)
        nonce = bytes(range(16))
        header = FILE_HDR.pack(0x54574750, 6, 0, 18, 0, 0, 0)
        first = [rec(0, "BOOT", b=0, nonce=nonce), rec(6, "WATERMARK", 5),
                 rec(7, "WATERMARK", 5)]
        second = [rec(8, "BOOT", b=1, nonce=nonce), rec(10, "WATERMARK", 8)]
        (directory / "z.trace.lz4").write_bytes(
            header + block(first[0]) + data_block([1 * SEC, 2 * SEC]) + block(first[1])
            + data_block([6 * SEC, 5 * SEC, 4 * SEC, 7 * SEC]) + block(first[2])
            + data_block([5 * SEC]))
        (directory / "a.trace.lz4").write_bytes(
            header + block(second[0]) + data_block([5 * SEC]) + block(second[1])
            + data_block([9 * SEC, 8 * SEC, 6 * SEC], kind=2))
        records, files = boot_files(directory, nonce)
        state = {"W": -1, "late": 0, "data": 0}
        for _, path, _, _ in files:
            coverage_records(path, state)
        assert state == {"W": 8 * SEC, "late": 6, "data": 11}, state
        # Exact W equality, both data types, unsorted timestamps, repeated
        # watermarks and a rotation BOOT must all be counted in file order.
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            try:
                report_leg(records, files, tick_log(records), "self late")
            except AssertionError as exc:
                assert "data timestamp <= preceding WATERMARK" in str(exc), exc
            else:
                raise AssertionError("late-after-W scan accepted six late records")
        assert "late_after_W=6" in printed.getvalue()
        (directory / "a.trace.lz4").unlink()
        clean_records = [first[0], first[1], rec(8, "WATERMARK", 7)]
        (directory / "z.trace.lz4").write_bytes(
            header + block(first[0]) + data_block([1 * SEC, 2 * SEC])
            + block(first[1]) + data_block([6 * SEC, 7 * SEC], kind=2)
            + block(clean_records[-1]) + data_block([8 * SEC]))
        records, files = boot_files(directory, nonce)
        state = report_leg(records, files, tick_log(records), "self in-order")
        assert state == {"W": 7 * SEC, "late": 0, "data": 5}, state
    print("file-order scan: PASS (exact-W boundary, transitions, samples, heartbeat, rotation, inversion)")

    print("test_coverage_live --self-test: PASS (witness branches, per-tick pairs, chain, recovery, stats, rotation, CRC)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    assert sys.platform.startswith("linux") and os.geteuid() == 0, \
        "Linux/root live test only"
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "no postmaster"
    pgb = find_pgbench()
    if sql(f"SELECT 1 FROM pg_database WHERE datname = '{DB}'") != "1":
        sql(f"CREATE DATABASE {DB}")
    subprocess.run([pgb, "-i", "-q", "-s", "4", "-U", "postgres", DB],
                   check=True, capture_output=True, timeout=300)
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip() or \
        "unknown (no .git: box-check syncs the tree without it)"
    print(f"coverage live commit={commit}", flush=True)

    with tempfile.TemporaryDirectory(prefix="pgwt-coverage-live-") as tmp:
        base = Path(tmp)
        directory = base / "trace"
        d = idle = load = err = None
        try:
            # Boot 1: quiet positive control, then SIGKILL under load.
            d, err = start_daemon(pm_pid, directory, base / "boot1.log")
            idle = idle_psql()
            quiet_leg(d, directory, "boot1 before load", log=base / "boot1.log")
            drops0 = drop_counter_tick(d, directory)
            load_start = time.monotonic_ns()
            load = pgbench(pgb, 30)
            time.sleep(15)
            assert d.poll() is None and load.poll() is None, "died under load"
            delta1 = drop_counter_tick(d, directory) - drops0
            kill_ns = time.monotonic_ns()
            d.kill()
            d.wait(timeout=10)
            err.close()
            recs1, files1 = boot_files(directory)
            assert "SEAL" not in [r[1] for r in recs1], "SIGKILL has no SEAL"
            assert not files1[-1][2] & FLAG_CLOSED_CLEAN, "killed file unclean"
            report_leg(recs1, files1, (base / "boot1.log").read_text(),
                       "boot1 SIGKILL", load_start, kill_ns)
            assert_load_coverage(recs1, load_start, kill_ns, delta1, "boot1 SIGKILL")
            assert len([w for w in watermarks(recs1) if w[0] >= load_start]) >= 12, \
                "about one WATERMARK per second during boot 1 load"
            obs = [o for o, *_ in watermarks(recs1)]
            spacing = [(b - a) / SEC for a, b in zip(obs, obs[1:])]
            print(f"coverage live MEASURED boot1: {len(obs)} watermarks; "
                  f"spacing max={max(spacing):.3f}s median={statistics.median(spacing):.3f}s", flush=True)
            assert max(spacing) < 2.5, ("one WATERMARK every tick", spacing)
            nonce1 = recs1[0][7]
            load.wait(timeout=40)
            assert load.returncode == 0, load.stderr.read()

            # Boot 2: restart proves all boot-1 files survive, even if rotated.
            d, err = start_daemon(pm_pid, directory, base / "boot2.log")
            quiet_leg(d, directory, "boot2 before load", log=base / "boot2.log")
            arecs, afiles = boot_files(directory, nonce1)
            assert arecs == recs1, "recovery preserves every COVERAGE record in file order"
            assert afiles[-1][2] & FLAG_RECOVERED and not afiles[-1][2] & FLAG_CLOSED_CLEAN, afiles
            drops0 = drop_counter_tick(d, directory)
            load_start = time.monotonic_ns()
            load = pgbench(pgb, 14)
            time.sleep(12)
            assert d.poll() is None and load.poll() is None, "boot2 died under load"
            delta2 = drop_counter_tick(d, directory) - drops0
            load_check = time.monotonic_ns()
            loaded_recs, loaded_files = boot_files(directory)
            report_leg(loaded_recs, loaded_files, (base / "boot2.log").read_text(),
                       "boot2 load", load_start, load_check)
            assert_load_coverage(loaded_recs, load_start, load_check, delta2, "boot2 load")
            load.wait(timeout=15)
            assert load.returncode == 0, load.stderr.read()
            load_end = time.monotonic_ns()
            quiet_leg(d, directory, "boot2 after load", after=load_end, log=base / "boot2.log")
            # The final check uses the same witness rule up to SIGTERM, and
            # additionally requires recovery freshness after losses stop.
            delta2 = drop_counter_tick(d, directory) - drops0
            term_ns = time.monotonic_ns()
            d.terminate()
            d.wait(timeout=30)
            assert d.returncode == 0, d.returncode
            err.close()
            recs2, files2 = boot_files(directory)
            assert recs2[0][7] != nonce1 and recs2[0][3] > recs1[0][3], "new boot"
            kinds2 = [r[1] for r in recs2]
            assert kinds2[-1] == "SEAL" and kinds2.count("SEAL") == 1, kinds2
            wm2 = watermarks(recs2)
            assert recs2[-1][3] == wm2[-1][1], "SEAL W_final = last W"
            # Exclude the shutdown tick's observation after the signal; its W
            # is checked separately against the actual termination boundary.
            before_term = [r for r in recs2 if r[0] <= term_ns]
            report_leg(recs2, files2, (base / "boot2.log").read_text(),
                       "boot2 SIGTERM", load_start, term_ns)
            assert_load_coverage(before_term, load_start, term_ns, delta2, "boot2 before SIGTERM")
            assert 0 <= wm2[-1][0] - recs2[-1][3] <= FRESHNESS_NS and recs2[-1][3] <= term_ns, \
                "fresh W_final relative to last observation after quiet recovery"
            assert files2[-1][2] & FLAG_CLOSED_CLEAN, files2
            stats = coverage_stats((base / "boot2.log").read_text())
            runtime_s = (recs2[-1][0] - recs2[0][3]) / SEC
            bph = int(stats["bytes"]) / runtime_s * 3600
            total_bph = sum(f[3] for f in files2) / runtime_s * 3600
            cpu = int(stats["tick_cpu_ns"]) / SEC / runtime_s * 100
            cpu_nodrain = (int(stats["tick_cpu_ns"]) - int(stats["drain_cpu_ns"])) / SEC / runtime_s * 100
            print(f"coverage live MEASURED cost: {stats['records']} records, "
                  f"{stats['bytes']} coverage bytes in {runtime_s:.1f}s = {bph / 1e6:.3f} MB/h "
                  f"(budget < 1 MB/h); TOTAL trace {total_bph / 1e6:.3f} MB/h "
                  f"including per-second publish; tick CPU {cpu:.3f}% of one core "
                  f"incl. drain, {cpu_nodrain:.3f}% excl. (budget < 0.5%); "
                  f"ticks={stats['ticks']} good={stats['good']} "
                  f"heartbeats={stats['heartbeats']} gaps={stats['gaps']}", flush=True)
            assert int(stats["write_failures"]) == 0, stats

            # Boot 3: a held consumer witness must still write heartbeat records.
            d, err = start_daemon(pm_pid, directory, base / "boot3.log",
                                  {"PGWT_TEST_COVERAGE_YIELD": "1"})
            time.sleep(5)
            d.terminate()
            d.wait(timeout=30)
            assert d.returncode == 0, d.returncode
            err.close()
            recs3, files3 = boot_files(directory)
            report_leg(recs3, files3, (base / "boot3.log").read_text(), "boot3 held consumer")
            wm3 = watermarks(recs3)
            assert len(wm3) >= 3, len(wm3)
            assert all(w == recs3[0][3] for _, w, _, _ in wm3), "held consumer holds W at boot_start"
            assert recs3[-1][1] == "SEAL" and recs3[-1][3] == recs3[0][3], "W_final = boot_start"
            print(f"coverage live: forced-yield heartbeats ({len(wm3)} watermarks)", flush=True)
        finally:
            for proc in (d, load, idle):
                if proc is not None and proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=10)
            if err is not None:
                err.close()
    print("test_coverage_live: PASS (quiet controls, loss witness, gap chains, restart, seal, heartbeat)", flush=True)


if __name__ == "__main__":
    main()
