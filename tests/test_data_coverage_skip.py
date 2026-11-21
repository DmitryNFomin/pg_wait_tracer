#!/usr/bin/env python3
"""#947 U1: pgwt-server answers a v6 trace exactly as if its COVERAGE blocks
were not there.

A differential test over the real server. gen_test_traces writes a capture
(two files: one rotated archive, served through the immutable-file cache,
and current.trace, served through the raw loader and the #283
current-trace cache). A copy of the directory then gets COVERAGE blocks
(trace v6 block type 3) spliced in: a BOOT first in each file, a WATERMARK
after every data block observed INSIDE the data's time range, gaps, a SEAL,
and one block observed far past every record. The footer index is rebuilt.

Every command below is asked of both directories over several windows; the
answers must be identical once wall-clock timings are dropped. Before #947
the server sent every non-SAMPLES block to its TRANSITIONS branch
(src/server.c cov_scan_file added an exact span at the block's write time),
and the reader called an unknown block damage -- either shows up here as a
different fidelity, span, generation, total or a refusal.

The COVERAGE encoder is written out in Python (struct + CRC32C), independent
of src/coverage_record.h.
"""
import json
import os
import shutil
import struct
import sys

from server_harness import ServerHarness, generate_traces, cleanup_traces

S = 1_000_000_000
MS = 1_000_000
BASE = 2_000 * S
IO_READ = 0x0A000015
LOCK_REL = 0x03000000
LWLOCK_WAL = 0x01000000 | 0x30
CPU = 0
FILE_HDR = struct.Struct("<IHHIQQQ")
BLOCK_HDR = struct.Struct("<QQIIIHHQQ")
INDEX = struct.Struct("<QQ")


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def cov_block(obs, kind, a=0, b=0, c=0, d=0, reason=0):
    body = struct.pack("<IHBBQQQQ", 0x43574750, 1, kind, reason, a, b, c, d)
    if kind == 1:
        body += bytes(range(16)) + bytes(16)
    payload = body + struct.pack("<II", crc32c(body), 0)
    hdr = BLOCK_HDR.pack(obs, obs, 1, len(payload), len(payload), 3, 0, 0, 0)
    return hdr + payload


def splice(path, far_obs):
    """Rewrite one v6 trace file with COVERAGE blocks around its data."""
    blob = open(path, "rb").read()
    magic, version = FILE_HDR.unpack_from(blob)[:2]
    assert magic == 0x54574750 and version == 6, (path, version)
    nb = struct.unpack_from("<I", blob, len(blob) - 4)[0]
    index_start = len(blob) - 4 - nb * INDEX.size
    offs = [INDEX.unpack_from(blob, index_start + i * INDEX.size)[1]
            for i in range(nb)]
    assert offs and offs[0] == FILE_HDR.size, "expected a footer"
    ends = offs[1:] + [index_start]
    out = bytearray(blob[:FILE_HDR.size])
    index = []

    def add(block, ts):
        index.append((ts, len(out)))
        out.extend(block)

    first_ts = BLOCK_HDR.unpack_from(blob, offs[0])[0]
    last_ts = max(BLOCK_HDR.unpack_from(blob, o)[1] for o in offs)
    # BOOT is observed when its file OPENS, which in a daemon's file is after
    # the record whose push opened it. Observed at the file's last record
    # here, the worst case for a range taken from block_index[0].
    add(cov_block(last_ts, 1, a=first_ts - S), last_ts)            # BOOT
    for k, (o, e) in enumerate(zip(offs, ends)):
        first, last = BLOCK_HDR.unpack_from(blob, o)[:2]
        add(blob[o:e], first)
        mid = (first + last) // 2
        add(cov_block(mid, 2, a=mid, b=mid - MS, c=mid - 2 * MS), mid)
        if k == 0:
            add(cov_block(mid + 1, 3, a=first, c=1, d=mid + 1, reason=2), mid + 1)
            add(cov_block(mid + 2, 4, a=mid, c=1), mid + 2)
            # The pre-A2 server leaves this gap unexpanded and the
            # A2_BUCKET certificate is a no-op, just like other metadata.
            add(cov_block(mid + 3, 3, a=first, b=4321, c=2, d=mid, reason=5), mid + 3)
            add(cov_block(mid + 4, 4, a=mid, b=17, c=2, d=first), mid + 4)
            add(cov_block(mid + 5, 7, a=first, b=last + 1, c=10, d=1), mid + 5)
    add(cov_block(far_obs, 2, a=far_obs, b=far_obs, c=far_obs), far_obs)
    add(cov_block(far_obs + 1, 5, a=far_obs), far_obs + 1)
    for ts, off in index:
        out.extend(INDEX.pack(ts, off))
    out.extend(struct.pack("<I", len(index)))
    open(path, "wb").write(bytes(out))
    meta = path + ".meta"
    if os.path.exists(meta):
        open(meta, "w").write("%d\n" % len(index))
    return len(index) - nb


def scenario(base, pids=6, per_pid=900, span_s=40):
    waits = [IO_READ, LOCK_REL, LWLOCK_WAL]
    step = span_s * S // per_pid
    events = []
    for p in range(pids):
        pid = 3000 + p
        for i in range(per_pid):
            ts = base + i * step + p * 7 * MS
            w = waits[(i + p) % len(waits)]
            events.append({"pid": pid, "ts": ts, "dur": (1 + i % 5) * MS,
                           "old": w if i % 2 else CPU,
                           "new": CPU if i % 2 else w,
                           "qid": 700 + p % 2})
    events.sort(key=lambda e: e["ts"])
    return {
        "backends": [{"pid": 3000 + p, "type": "client", "user": "u",
                      "db": "d"} for p in range(pids)],
        "queries": [{"id": 700, "text": "SELECT a()"},
                    {"id": 701, "text": "SELECT b()"}],
        "events": events,
        "samples": [{"pid": 3000, "ts": base + 10 * S + i * 100 * MS,
                     "event": IO_READ, "qid": 700} for i in range(50)],
        "sample_period_ns": 100 * MS,
    }


# Only fields that differ between ANY two servers: the request id and the
# server's clock. Everything else must match byte for byte.
UNCOMPARED = ("id", "now_ns")


def scrub(x):
    if isinstance(x, dict):
        return {k: scrub(v) for k, v in x.items() if k not in UNCOMPARED}
    if isinstance(x, list):
        return [scrub(v) for v in x]
    return x


COMMANDS = ["time_model", "top_events", "top_sessions", "top_queries", "aas",
            "heatmap", "session_timeline", "executions", "exec_scatter",
            "transitions", "fingerprints", "concurrency", "lock_chains",
            "variants", "interference", "info"]


def main():
    plain = generate_traces(scenario(BASE), rotate="2025-01-01_10")
    generate_traces(scenario(BASE + 60 * S), output_dir=plain)
    cov = plain + "-cov"
    shutil.copytree(plain, cov)
    failures = 0
    added = 0
    for name in sorted(os.listdir(cov)):
        if name.endswith(".trace.lz4") or name == "current.trace":
            added += splice(os.path.join(cov, name), BASE + 5000 * S)
    print("spliced %d COVERAGE blocks into %s" % (added, cov))
    assert added >= 10, added
    windows = [(BASE + 5 * S, BASE + 35 * S),          # archive only
               (BASE + 20 * S, BASE + 50 * S),         # archive, no samples
               (BASE + 30 * S, BASE + 70 * S),         # across the rotation
               (BASE + 62 * S, BASE + 98 * S),         # current.trace only
               (BASE + 98 * S, BASE + 99 * S)]         # current.trace tail
    try:
        with ServerHarness(plain) as a, ServerHarness(cov) as b:
            for cmd in COMMANDS:
                for (f, t) in windows:
                    kw = {} if cmd == "info" else {"from_": f, "to_": t}
                    # asked twice: the second answer comes from the caches
                    for attempt in range(2):
                        ra = scrub(a.query(cmd, timeout=60, **kw))
                        rb = scrub(b.query(cmd, timeout=60, **kw))
                        if ra != rb:
                            failures += 1
                            ja = json.dumps(ra, sort_keys=True)
                            jb = json.dumps(rb, sort_keys=True)
                            i = next((k for k in range(min(len(ja), len(jb)))
                                      if ja[k] != jb[k]), 0)
                            print("FAIL %s [%d,%d) pass %d differs near: %s | %s"
                                  % (cmd, (f - BASE) // S, (t - BASE) // S,
                                     attempt, ja[max(0, i - 80):i + 80],
                                     jb[max(0, i - 80):i + 80]))
                        elif "error" in ra:
                            print("note: %s answered an error on both: %s"
                                  % (cmd, ra.get("error")))
                    if cmd == "info":
                        break
    finally:
        cleanup_traces(plain)
        cleanup_traces(cov)
    if failures:
        print("test_data_coverage_skip: %d differing answers" % failures)
        return 1
    print("test_data_coverage_skip: PASS (%d commands x %d windows x 2 passes "
          "identical with and without COVERAGE blocks)"
          % (len(COMMANDS), len(windows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
