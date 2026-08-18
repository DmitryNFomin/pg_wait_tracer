#!/usr/bin/env python3
"""test_data_fidelity.py — A3: fidelity-aware compute (sampled estimators,
exact-wins merge, unavailable for exact-only views).

Covers design decision D3 of docs/ROADMAP_AND_STATUS.md:

  1. Sampled-only trace with a KNOWN sample stream → exact expected
     time_model / system_event / AAS estimates (ASH math: N samples of
     event X at period P → X gets N*P time; AAS = active-sample-time /
     window). An idle event (Client:ClientRead) is visible in breakdowns
     but excluded from DB Time / AAS.
  2. EXACT-required view (histogram, transitions) over a sampled-only
     window → explicit {"unavailable": ...}, never a silent empty result.
  3. Mixed-window merge: overlapping transition + sample coverage →
     transition data wins inside its range, samples only contribute
     outside it (no double counting).

These run without a live PostgreSQL: gen_test_traces emits SAMPLES blocks
from the "samples"/"sample_period_ns" inputs (A1).
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner, SERVER_BIN,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, CLIENT_READ,
)

BASE_TS = 10_000_000_000_000  # 10000s in ns (> 1hr so server range expansion is safe)
PERIOD = 100_000_000          # 100ms = 10 Hz sample period


def sample_at(pid, ts, event, qid=0):
    return {"pid": pid, "ts": ts, "event": event, "qid": qid}


# ── 1. Sampled-only estimators ────────────────────────────────────────

def build_sampled_only():
    """A pure SAMPLES stream with a known composition.

    At 10 Hz (period = 100ms), each sample is worth 100ms = 100ms of time.

      PID 1000: 10 samples of IO:DataFileRead  → 10 * 100ms = 1000ms IO
      PID 1000:  5 samples of Lock:relation     →  5 * 100ms =  500ms Lock
      PID 1001:  3 samples of CPU (event=0)     →  3 * 100ms =  300ms CPU
      PID 1002:  4 samples of Client:ClientRead →  4 * 100ms =  400ms (idle,
                 visible in event list but excluded from DB Time / AAS)

    Expected DB Time = 1000 + 500 + 300 = 1800ms (ClientRead excluded).
    """
    samples = []
    ts = BASE_TS
    for _ in range(10):
        ts += PERIOD
        samples.append(sample_at(1000, ts, IO_DATA_FILE_READ, 100))
    for _ in range(5):
        ts += PERIOD
        samples.append(sample_at(1000, ts, LOCK_RELATION, 100))
    for _ in range(3):
        ts += PERIOD
        samples.append(sample_at(1001, ts, CPU, 200))
    for _ in range(4):
        ts += PERIOD
        samples.append(sample_at(1002, ts, CLIENT_READ, 0))
    # Keep sorted by ts (writer requires it)
    samples.sort(key=lambda s: s["ts"])
    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "SELECT 2"},
        ],
        "sample_period_ns": PERIOD,
        "samples": samples,
    }


def test_sampled_only(t):
    print("\n### 1. Sampled-only estimators (ASH math) ###")
    trace_dir = generate_traces(build_sampled_only())
    try:
        with ServerHarness(trace_dir) as srv:
            # --- time_model: ASH-estimated DB Time + class breakdown ---
            tm = srv.query("time_model")
            t.check_eq(tm.get("fidelity"), "sampled",
                       "time_model fidelity = sampled")
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_approx(rows["DB Time"]["ms"], 1800.0, 0.001,
                           "DB Time = 1800ms (3 non-idle classes, ClientRead excluded)")
            t.check_approx(rows.get("IO", {}).get("ms", -1), 1000.0, 0.001,
                           "IO = 1000ms (10 samples * 100ms)")
            t.check_approx(rows.get("Lock", {}).get("ms", -1), 500.0, 0.001,
                           "Lock = 500ms (5 samples * 100ms)")
            t.check_approx(rows.get("CPU*", {}).get("ms", -1), 300.0, 0.001,
                           "CPU = 300ms (3 samples * 100ms)")
            # ClientRead is idle → must NOT count toward DB Time.
            t.check("Client" not in rows or rows["Client"]["ms"] < 0.001,
                    "Client class excluded from DB Time (idle ClientRead)")

            # --- system_event (top_events): ClientRead visible but %DB excludes it ---
            ev = srv.query("top_events")
            t.check_eq(ev.get("fidelity"), "sampled",
                       "top_events fidelity = sampled")
            evrows = {r["name"]: r for r in ev["rows"]}
            t.check("Client:ClientRead" in evrows,
                    "ClientRead is visible in the event list")
            cr = evrows.get("Client:ClientRead", {})
            t.check_approx(cr.get("total_ms", -1), 400.0, 0.001,
                           "ClientRead total = 400ms (4 samples * 100ms)")
            t.check_approx(ev.get("db_time_ms", -1), 1800.0, 0.001,
                           "top_events db_time excludes idle ClientRead = 1800ms")
            io = evrows.get("IO:DataFileRead", {})
            t.check_eq(io.get("count"), 10, "IO:DataFileRead sample count = 10")

            # FID-3: latency columns are fabrications over sampled data —
            # gated (null) on sampled-only rows; count/total stay valid.
            for col in ("avg_us", "p50_us", "p95_us", "p99_us", "max_us"):
                t.check(io.get(col, "missing") is None,
                        f"sampled-only row gates latency column {col}")

            # --- AAS: active-sample-time / window ---
            # 28 total samples span (28-1) periods = 2700ms of wall-clock.
            # The harness queries the full file range. Verify AAS integrates
            # to the expected DB-Time across all buckets.
            aas = srv.query("aas", buckets=1)
            t.check_eq(aas.get("fidelity"), "sampled", "aas fidelity = sampled")
            b = aas["buckets"][0]
            bucket_ns = aas["bucket_ns"]
            # Sum non-idle class AAS for the single bucket → average active
            # sessions over the window. active_time = sum_class_aas * bucket_ns.
            active_aas = sum(v for k, v in b.items()
                             if k not in ("t",) and isinstance(v, (int, float)))
            active_ms = active_aas * bucket_ns / 1e6
            t.check_approx(active_ms, 1800.0, 0.02,
                           "AAS integrates to 1800ms active DB Time (idle excluded)")
            # ClientRead (client class) must contribute ~0 to AAS load.
            t.check(b.get("client", 0.0) < 1e-6,
                    "ClientRead contributes ~0 to AAS load (idle)")
    finally:
        cleanup_traces(trace_dir)


# ── 2. EXACT-required views unavailable over sampled-only ─────────────

def test_exact_unavailable(t):
    print("\n### 2. EXACT-required views → unavailable over sampled-only ###")
    trace_dir = generate_traces(build_sampled_only())
    try:
        with ServerHarness(trace_dir) as srv:
            for view in ("heatmap", "transitions", "lock_chains",
                         "interference", "concurrency", "fingerprints",
                         "variants"):
                resp = srv.query(view)
                t.check("unavailable" in resp,
                        f"{view}: explicit 'unavailable' (not silent-empty)")
                t.check_eq(resp.get("fidelity"), "sampled",
                           f"{view}: fidelity = sampled in unavailable response")
                t.check("full-fidelity" in resp.get("unavailable", ""),
                        f"{view}: unavailable message mentions full-fidelity data")
    finally:
        cleanup_traces(trace_dir)


# ── 3. Mixed-window exact-wins merge (no double counting) ─────────────

def build_mixed():
    """Transition coverage overlapping a continuous sample stream.

    This trace has samples + transitions but NO escalation markers, so the
    exact-wins merge uses the LEGACY per-TRANSITIONS-block coverage
    (marker windows are the authority when markers exist — see
    test_data_esc_coverage.py). The transition block must therefore
    actually SPAN the overlap window.

    TRANSITIONS (exact) for PID 1000: a chain of IO:DataFileRead intervals
    with event timestamps from BASE+1.0s to BASE+2.0s (the block's
    [first_ts,last_ts] = [1.0s, 2.0s]). Each is a 100ms IO interval, so the
    exact IO total over the window is 11 * 100ms = 1100ms.

    SAMPLES (10 Hz) of Lock:relation run continuously over
    [BASE+1.0s .. BASE+4.0s] (31 samples). Boundary rule (FID-6): a sample
    represents (ts − period, ts], and coverage is subtracted from that
    interval. These grid-aligned samples are therefore wholly kept or dropped;
    each survivor adds 100ms of Lock. No double counting in the overlap.
    """
    one_s = 1_000_000_000
    trans_start = BASE_TS + one_s
    trans_end = BASE_TS + 2 * one_s

    # Transition chain spanning [1.0s, 2.0s]: 11 IO intervals of 100ms each.
    events = []
    ts = trans_start
    while ts <= trans_end:
        events.append({"pid": 1000, "ts": ts, "dur": PERIOD,
                       "old": IO_DATA_FILE_READ, "new": IO_DATA_FILE_READ,
                       "qid": 100})
        ts += PERIOD
    n_trans = len(events)
    exact_io_ms = n_trans * (PERIOD / 1e6)  # 11 * 100ms = 1100ms

    # Continuous Lock samples over [1.0s, 4.0s].
    samples = []
    ts = trans_start
    while ts <= BASE_TS + 4 * one_s:
        samples.append(sample_at(1001, ts, LOCK_RELATION, 200))
        ts += PERIOD
    samples.sort(key=lambda s: s["ts"])

    # Grid-aligned samples wholly after exact coverage survive, as does the
    # sample ending exactly at the window start (its interval precedes exact
    # coverage and the trace's default range begins earlier).
    out_range = sum(1 for s in samples
                    if s["ts"] == trans_start or s["ts"] > trans_end)

    return ({
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "SELECT 2"},
        ],
        "events": events,
        "sample_period_ns": PERIOD,
        "samples": samples,
    }, out_range, exact_io_ms)


def test_mixed_merge(t):
    print("\n### 3. Mixed-window exact-wins merge (no double counting) ###")
    scenario, out_range, exact_io_ms = build_mixed()
    expected_lock_ms = out_range * (PERIOD / 1e6)  # surviving samples * 100ms
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            tm = srv.query("time_model")
            t.check_eq(tm.get("fidelity"), "mixed",
                       "mixed window fidelity = mixed")
            rows = {r["name"]: r for r in tm["rows"]}
            # IO comes only from the exact transition chain.
            t.check_approx(rows.get("IO", {}).get("ms", -1), exact_io_ms, 0.001,
                           f"IO = exact {exact_io_ms:.0f}ms from transition block")
            # Lock comes only from samples OUTSIDE the transition range.
            t.check_approx(rows.get("Lock", {}).get("ms", -1),
                           expected_lock_ms, 0.001,
                           f"Lock = {expected_lock_ms:.0f}ms "
                           f"(only {out_range} out-of-range samples survive)")

            # transitions view IS available (transition coverage exists).
            tr = srv.query("transitions")
            t.check("unavailable" not in tr,
                    "transitions available in a mixed window (has exact data)")
            t.check_eq(tr.get("fidelity"), "mixed",
                       "transitions fidelity = mixed")

            # FID-3 in a mixed window: exact-fed rows keep real latency
            # stats, sampled-only rows gate them.
            ev = srv.query("top_events")
            evrows = {r["name"]: r for r in ev["rows"]}
            io = evrows.get("IO:DataFileRead", {})
            lk = evrows.get("Lock:relation", {})
            t.check(isinstance(io.get("p95_us"), (int, float)),
                    "mixed window: exact-fed row keeps numeric p95")
            t.check(lk.get("p95_us", "missing") is None,
                    "mixed window: sampled-only row gates p95 (null)")
    finally:
        cleanup_traces(trace_dir)


# ── 4. --dump annotates fidelity ──────────────────────────────────────

def test_dump_annotation(t):
    print("\n### 4. --dump annotates fidelity ###")
    trace_dir = generate_traces(build_sampled_only())
    try:
        out = subprocess.run([SERVER_BIN, "--dump", trace_dir],
                             capture_output=True, text=True)
        text = out.stdout
        t.check("Fidelity: sampled" in text,
                "--dump reports 'Fidelity: sampled' for a sampled-only trace")
        t.check("require full-fidelity data" in text or
                "full-fidelity data" in text,
                "--dump notes which views require full-fidelity data")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_fidelity")
    print(f"=== {t.name} ===")
    test_sampled_only(t)
    test_exact_unavailable(t)
    test_mixed_merge(t)
    test_dump_annotation(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
