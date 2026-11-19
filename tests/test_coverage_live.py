#!/usr/bin/env python3
"""#947 U1 live: trace v6 COVERAGE records written by a real daemon.

Linux/root, real PostgreSQL. Spec r2 §5 U1 "Box":

  boot 1  --mode full under pgbench -c 16 with one idle psql attached,
          SIGKILL mid-load. Asserts: the file carries a BOOT, one WATERMARK
          about every second (heartbeats included), no SEAL, and its last W is
          at most 2 s before the kill.
  boot 2  a restart into the same directory. Asserts: recovery archived
          boot 1 with every COVERAGE block and still no SEAL; boot 2 opens
          with its own BOOT (new nonce, file_no 0). Then SIGTERM under load:
          asserts a SEAL is the last record and the closure bit is CLEAN.
  boot 3  PGWT_TEST_COVERAGE_YIELD=1 (every drain reports a yield): asserts
          the watermarks are heartbeats that never advance W.

PRINTED, NOT ASSERTED (the lead judges them against the budget):
  floor_db - W and floor_idle - W lag under the load (median, p95, max),
  coverage bytes/hour (budget < 1 MB/h) and coverage CPU as % of one core
  (budget < 0.5%), from the daemon's own shutdown line.

The COVERAGE payload is decoded here independently of the C codec (struct
layout + CRC32C written out in Python). Daemon -i 5 on purpose: the coverage
tick must not depend on the display interval.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import time

from test_init_hint_recovery import wait_for
from testutil import find_postmaster

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / "pg_wait_tracer"
PSQL = ["psql", "-X", "-U", "postgres", "-qAt", "-v", "ON_ERROR_STOP=1"]
DB = "pgwt_coverage_live"
SEC = 1_000_000_000

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


def coverage_records(path):
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
        else:
            data += 1
        off = end
    return recs, flags, data


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
                         env={**os.environ, **(env or {})})
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
    print(f"coverage live MEASURED {label}: floor_db-W lag ms "
          f"median={statistics.median(db):.1f} p95={pct(db, .95):.1f} "
          f"max={max(db):.1f}; floor_idle-W lag ms "
          f"median={statistics.median(idle):.1f} p95={pct(idle, .95):.1f} "
          f"max={max(idle):.1f} (n={len(settled)})", flush=True)


def watermarks(recs):
    return [(obs, a, b, c) for (obs, k, _r, a, b, c, _d, _n) in recs
            if k == "WATERMARK"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    args = ap.parse_args()
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
        cur = directory / "current.trace"

        # ── boot 1: SIGKILL under load ────────────────────────────────────
        d, err = start_daemon(pm_pid, directory, base / "boot1.log")
        idle = idle_psql()
        load = pgbench(pgb, 40)
        try:
            time.sleep(15)
            assert d.poll() is None and load.poll() is None, "died under load"
            kill_ns = time.monotonic_ns()   # CLOCK_MONOTONIC, the daemon's clock
            os.kill(d.pid, signal.SIGKILL)
            d.wait(timeout=10)
        finally:
            err.close()
        recs1, flags1, _ = coverage_records(cur)
        kinds1 = [r[1] for r in recs1]
        assert kinds1 and kinds1[0] == "BOOT", kinds1[:3]
        assert recs1[0][4] == 0, "boot 1 file_no 0"
        assert "SEAL" not in kinds1, "a SIGKILLed boot has no SEAL"
        wm1 = watermarks(recs1)
        assert len(wm1) >= 12, ("about one WATERMARK per second", len(wm1))
        obs = [o for (o, *_rest) in wm1]
        gaps = [(b - a) / 1e9 for a, b in zip(obs, obs[1:])]
        print(f"coverage live MEASURED boot1: {len(wm1)} watermarks, "
              f"observation spacing max={max(gaps):.3f}s "
              f"median={statistics.median(gaps):.3f}s", flush=True)
        assert max(gaps) < 2.5, ("a live boot writes one every tick", max(gaps))
        last_w = wm1[-1][1]
        lag_s = (kill_ns - last_w) / 1e9
        print(f"coverage live MEASURED boot1: last W is {lag_s:.3f}s before "
              f"the kill", flush=True)
        assert 0 <= lag_s <= 2.0, ("last W at most 2 s before the kill", lag_s)
        for (_o, w, fd, fi) in wm1:
            assert fi <= fd <= w, "floor_idle <= floor_db <= W"
        assert all(b[1] >= a[1] for a, b in zip(wm1, wm1[1:])), "W monotone"
        report_lags(wm1, "boot1 (pgbench -c16 + 1 idle psql)")
        nonce1 = recs1[0][7]

        # ── boot 2: restart, then SIGTERM under load ──────────────────────
        d, err = start_daemon(pm_pid, directory, base / "boot2.log")
        try:
            time.sleep(12)
            assert d.poll() is None, "boot 2 died"
            t_start = time.monotonic_ns()
            os.kill(d.pid, signal.SIGTERM)
            d.wait(timeout=30)
            assert d.returncode == 0, d.returncode
        finally:
            err.close()
            load.wait(timeout=60)
            idle.stdin.close()
            idle.wait(timeout=10)
        archives = sorted(directory.glob("*.trace.lz4"))
        assert len(archives) == 1, archives
        arec, aflags, _ = coverage_records(archives[0])
        assert [r[1:7] for r in arec] == [r[1:7] for r in recs1], \
            "recovery kept boot 1's COVERAGE blocks unchanged"
        assert aflags & FLAG_RECOVERED and not aflags & FLAG_CLOSED_CLEAN, \
            hex(aflags)
        recs2, flags2, _ = coverage_records(cur)
        k2 = [r[1] for r in recs2]
        assert k2[0] == "BOOT" and recs2[0][4] == 0, "boot 2 opens with BOOT"
        assert recs2[0][7] != nonce1, "boot 2 has its own nonce"
        assert recs2[0][3] > recs1[0][3], "boot 2 starts later"
        assert k2[-1] == "SEAL" and k2.count("SEAL") == 1, k2[-3:]
        wm2 = watermarks(recs2)
        assert recs2[-1][3] == wm2[-1][1], "SEAL W_final = the last W"
        assert t_start - 2 * SEC <= recs2[-1][3] <= t_start, \
            "W_final within the last tick before SIGTERM"
        assert flags2 & FLAG_CLOSED_CLEAN, hex(flags2)
        report_lags(wm2, "boot2 (pgbench -c16 + 1 idle psql)")
        line = [l for l in (base / "boot2.log").read_text().splitlines()
                if l.startswith("INFO: coverage: ")]
        assert line, "the daemon's coverage shutdown line"
        stats = dict(kv.split("=") for kv in line[-1].split(": ", 1)[1].split())
        runtime_s = (recs2[-1][0] - recs2[0][3]) / 1e9
        hz = os.sysconf("SC_CLK_TCK")
        bph = int(stats["bytes"]) / runtime_s * 3600
        cpu = int(stats["tick_cpu_ns"]) / 1e9 / runtime_s * 100
        cpu_nodrain = (int(stats["tick_cpu_ns"]) - int(stats["drain_cpu_ns"])) \
            / 1e9 / runtime_s * 100
        print(f"coverage live MEASURED cost: {stats['records']} records, "
              f"{int(stats['bytes'])} bytes in {runtime_s:.1f}s = "
              f"{bph / 1e6:.3f} MB/h (budget < 1 MB/h); coverage tick CPU "
              f"{cpu:.3f}% of one core incl. its ring drain, {cpu_nodrain:.3f}% "
              f"excl. (budget < 0.5%); ticks={stats['ticks']} "
              f"good={stats['good']} heartbeats={stats['heartbeats']} "
              f"gaps={stats['gaps']} hz={hz}", flush=True)
        assert int(stats["write_failures"]) == 0, stats

        # ── boot 3: forced drain yield → heartbeats only ──────────────────
        d, err = start_daemon(pm_pid, directory, base / "boot3.log",
                              {"PGWT_TEST_COVERAGE_YIELD": "1"})
        try:
            time.sleep(5)
            os.kill(d.pid, signal.SIGTERM)
            d.wait(timeout=30)
        finally:
            err.close()
        recs3, _f, _ = coverage_records(cur)
        wm3 = watermarks(recs3)
        assert len(wm3) >= 3, len(wm3)
        assert all(w == recs3[0][3] for (_o, w, _fd, _fi) in wm3), \
            "with every drain yielding, W stays at boot_start (heartbeats)"
        assert recs3[-1][1] == "SEAL" and recs3[-1][3] == recs3[0][3], \
            "and W_final = boot_start"
        print("coverage live: forced-yield heartbeats hold W at boot_start "
              f"({len(wm3)} watermarks)", flush=True)
    print("test_coverage_live: PASS (SIGKILL unsealed, restart BOOT, SIGTERM "
          "SEAL, heartbeat; cost and lag printed above)", flush=True)


if __name__ == "__main__":
    main()
