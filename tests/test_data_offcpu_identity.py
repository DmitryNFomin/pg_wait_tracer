#!/usr/bin/env python3
"""test_data_offcpu_identity.py — T8 revision: conserved CPU* + residual Off-CPU*.

The regression that the rejected per-interval Off-CPU* models fail. Encodes
the mixed CPU/LWLock/IO sequence from docs/ROADMAP_AND_STATUS.md with
a KNOWN sub-millisecond CPU burst whose measured cpu_ns leaks into the
following wait interval (sum_exec_runtime is only current at a tick / context
switch). Asserts:

  1. CPU* = Σ of every interval's measured cpu_ns (CPU *and* wait intervals) —
     the conserved true CPU. The pre-revision code drops the wait intervals'
     cpu_ns, so it reports CPU* too low here.
  2. Wait-class durations are exact (unaffected by the CPU leak).
  3. Off-CPU* = DB Time - CPU* - Σ waits (residual, leak-immune) — 0 for the
     free-core sequence (Models A/B wrongly report >0), and the true runqueue
     time for a deliberately-preempted CPU-bound backend.
  4. The identity CPU* + Off-CPU* + Σ waits == DB Time holds exactly.

(#187: the time_model rows are now named "CPU (running)" / "CPU (waiting for
a core)" — this docstring keeps the shorthand CPU*/Off-CPU* for the
underlying quantities; the JSON keys stay 'cpu'/'offcpu' unchanged.)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE,
)

BASE_TS = 10_000_000_000_000
MS = 1_000_000  # ns


def build_scenario():
    # PID 1000 — the §1 mixed sequence (durations, measured cpu_ns):
    #   CPU1  5.0ms cpu 4.8   (0.2 tick-tail leaks →)
    #   LWLk1 20.0ms cpu 0.2  (CPU1's tail)
    #   CPU2  0.4ms cpu 0.0   (whole sub-tick burst leaks →)
    #   LWLk2 15.0ms cpu 0.4  (CPU2's burst)
    #   IO    10.0ms cpu 0.0  (blocked)
    #   CPU3  3.0ms cpu 3.0   (terminal, measured)
    # true: DB 53.4, CPU 8.4, LWLock 35, IO 10, Off-CPU 0 (free core)
    seq = [
        (CPU,               5000 * (MS // 1000), 4800 * (MS // 1000), IO_DATA_FILE_READ),
        (LWLOCK_WAL_WRITE, 20000 * (MS // 1000),  200 * (MS // 1000), CPU),
        (CPU,                400 * (MS // 1000),    0,                LWLOCK_WAL_WRITE),
        (LWLOCK_WAL_WRITE, 15000 * (MS // 1000),  400 * (MS // 1000), IO_DATA_FILE_READ),
        (IO_DATA_FILE_READ,10000 * (MS // 1000),    0,                CPU),
        (CPU,               3000 * (MS // 1000), 3000 * (MS // 1000), IO_DATA_FILE_READ),
    ]
    events = []
    ts = BASE_TS
    for old, dur, cpu, new in seq:
        ts += dur
        events.append({"pid": 1000, "ts": ts, "dur": dur,
                       "old": old, "new": new, "qid": 100, "cpu": cpu})

    # PID 1001 — a CPU-bound backend preempted 40% (oversubscription):
    #   one 10ms on-CPU gap that only got 6ms of CPU → Off-CPU* must read 4ms.
    ts2 = BASE_TS + 1_000_000
    ts2 += 10000 * (MS // 1000)
    events.append({"pid": 1001, "ts": ts2, "dur": 10000 * (MS // 1000),
                   "old": CPU, "new": IO_DATA_FILE_READ, "qid": 200,
                   "cpu": 6000 * (MS // 1000)})

    return {
        "cpu_measured": 1,
        "interleave": 1,
        "backends": [
            {"pid": 1000, "type": "client", "user": "t", "db": "d"},
            {"pid": 1001, "type": "client", "user": "t", "db": "d"},
        ],
        "queries": [{"id": 100, "text": "seq"}, {"id": 200, "text": "hog"}],
        "events": events,
    }


def main():
    t = TestRunner("test_data_offcpu_identity")
    print(f"=== {t.name} ===")
    trace_dir = generate_traces(build_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            rows = {r["name"]: r for r in srv.query("time_model")["rows"]}

            def ms(name):
                return rows.get(name, {}).get("ms", -1.0)

            # Combined totals: DB = 53.4 + 10 = 63.4; CPU* = 8.4 + 6 = 14.4;
            # LWLock 35; IO = 10 (seq) + 10 (hog's new is IO but that's a
            # different event's OLD... hog's OLD is CPU) → IO stays 10;
            # Off-CPU* = 0 (seq) + 4 (hog) = 4. Identity → 63.4.
            # #187: has_measured_cpu splits the row into "CPU (running)" and
            # "CPU (waiting for a core)" (the JSON keys stay 'cpu'/'offcpu' —
            # only these display names changed).
            db     = ms("DB Time")
            cpu    = ms("CPU (running)")
            offcpu = ms("CPU (waiting for a core)")
            lwlock = ms("LWLock")
            io     = ms("IO")

            print(f"--- DB={db} CPU(running)={cpu} CPU(waiting)={offcpu} "
                  f"LWLock={lwlock} IO={io} ---")
            t.check_approx(db, 63.4, 0.01, "DB Time = 63.4ms")
            # CPU (running) = query CPU (we==0 on-CPU) ONLY: 4.8+0+3 (seq) + 6 (hog).
            # The 0.6ms of on-CPU during the two LWLock intervals is LWLock SPIN
            # — it stays attributed to LWLock (the label is the diagnostic), not
            # folded into the anonymous running-CPU bucket.
            t.check_approx(cpu, 13.8, 0.01,
                           "CPU (running) = 13.8ms (query CPU only; LWLock spin stays in LWLock)")
            # Wait classes keep their FULL wall time (incl. the on-CPU spin):
            t.check_approx(lwlock, 35.0, 0.01, "LWLock = 35.0ms (full time, keeps label)")
            t.check_approx(io, 10.0, 0.01, "IO = 10.0ms")
            # CPU (waiting for a core) = residual runqueue of the we==0 gaps: 0
            # for the free-core sequence + 4.6 from the preempted hog (10 wall -
            # 6 on-CPU) and the sequence's own we==0 off-CPU (18.4 gap wall -
            # 13.8 on-CPU = 4.6).
            t.check_approx(offcpu, 4.6, 0.02,
                           "CPU (waiting for a core) = 4.6ms (residual runqueue of the we==0 gaps)")
            # The conservation identity (#187's core claim):
            # CPU (running) + CPU (waiting for a core) + Σ waits == DB Time.
            t.check_approx(cpu + offcpu + lwlock + io, 63.4, 0.02,
                           "IDENTITY CPU (running) + CPU (waiting for a core) + "
                           "Σwaits == DB Time")
    finally:
        cleanup_traces(trace_dir)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
