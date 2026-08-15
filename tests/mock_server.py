#!/usr/bin/env python3
"""mock_server.py -- Serves web/static/ + WebSocket with canned JSON responses.

No SSH, no pgwt-server, no root needed.  Designed for Playwright testing.

Architecture:
  - HTTP on port P   (serves static files from web/static/)
  - WS   on port P+1 (WebSocket endpoint, same protocol as pgwt)

The test_web_ui.py injects a script to point the WS at the correct port.

Usage:
    python3 tests/mock_server.py              # HTTP :18765, WS :18766
    python3 tests/mock_server.py --port 9000  # HTTP :9000,  WS :9001

Chaos mode (Phase B2)
─────────────────────
By default the mock answers every request synchronously with zero latency,
in arrival order.  That is friendly to deterministic assertions but it never
fires the async races the real network does — which is exactly why CI stays
green while manual testing keeps finding bugs.

Chaos mode reproduces real network conditions so the race-exposing tests in
tests/test_web_ui_chaos.py can drive them.  It is OFF unless explicitly
enabled (so existing tests are unaffected) and DETERMINISTIC: every chaos
decision is derived from the request's monotonically-increasing index, never
from wall-clock RNG, so a given action sequence always produces the same
interleaving.

Enable + tune via env vars (or --chaos on the CLI):
    PGWT_CHAOS=1            enable chaos (default off)
    PGWT_CHAOS_MIN_MS=50    min per-response latency  (default 50)
    PGWT_CHAOS_MAX_MS=300   max per-response latency  (default 300)
    PGWT_CHAOS_REORDER=1    deliver concurrent in-flight responses shuffled
                            (default on when chaos is on)
    PGWT_CHAOS_LATE_EVERY=7 every Nth request is a "late" response that is
                            delayed far beyond the others, so it lands after
                            the client has navigated away (0 = never;
                            default 7)
    PGWT_CHAOS_LATE_MS=900  extra delay added to a "late" response (default
                            900, i.e. ~1s after a normal one)
    PGWT_CHAOS_LATE_CMDS=   comma-separated command names whose responses are
                            ALWAYS late (e.g. "transitions,heatmap"). Lets a
                            test deterministically force one view's response
                            to land after the user has moved on, without
                            depending on request-index arithmetic. (default:
                            empty)
    PGWT_CHAOS_SEED=1337    seed mixed into the deterministic hash (default
                            1337) — vary only to explore other interleavings
"""
import asyncio
import json
import os
import sys
import argparse
import signal
from http.server import SimpleHTTPRequestHandler, HTTPServer
import threading

# websockets is only needed to *run* the server.  Importers that just use
# handle_request() (e.g. tests/test_protocol_drift.py) work without it.
try:
    import websockets.asyncio.server
    _HAVE_WEBSOCKETS = True
except ImportError:
    _HAVE_WEBSOCKETS = False

STATIC_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "web", "static")

# ── Canned data ──────────────────────────────────────────────────────────────

def _env_int(name, default):
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


_TO_NS   = 1_774_000_000_000_000_000
_FROM_NS = _TO_NS - 3600_000_000_000
_BUCKET_NS = 60_000_000_000
_COMPARE_MODE = os.environ.get("PGWT_MOCK_COMPARE", "0") not in ("0", "", "false")
_INFO_TICKS = 0


def _is_compare_baseline(msg):
    """Test-only discriminator for U4's second use of the SAME commands.

    The compare suite selects a 5-minute offset. Table B ends before the fixed
    NOW; AAS B is a 3x strip, so distinguish it by its earlier strip centre.
    Ordinary mocks never enter this path.
    """
    if not _COMPARE_MODE:
        return False
    req_from, req_to = msg.get("from"), msg.get("to")
    if req_from is None or req_to is None:
        return False
    if msg.get("cmd") == "aas":
        return (req_from + req_to) / 2 < _TO_NS - 10 * 60_000_000_000
    return req_to < _TO_NS


def _baseline_rows(rows, value_key):
    """Deterministic B variants: real changes, one gone entity, one floor row."""
    out = []
    for source in rows:
        row = dict(source)
        if row.get("event_id") == 0:
            row[value_key] = 4000
        elif row.get("event_id") == 0x01000015:
            row[value_key] = 1000
        elif row.get("event_id") == 0x03000000:
            continue                         # A-only => new
        elif row.get("event_id") == 0x01000050:
            row[value_key] = 250             # 50 ms => below 1% floor
        out.append(row)
    out.append({"name": "Client:gone_from_A", "event_id": 0x06000009,
                "class": "Client", "count": 700, "total_ms": 700,
                "avg_us": 1000, "p50_us": 1000, "p95_us": 1000,
                "p99_us": 1000, "max_us": 1000, "pct": 5.6, "aas": 0.19})
    return out

def _make_aas_buckets():
    buckets = []
    for i in range(60):
        t = _FROM_NS + i * _BUCKET_NS
        buckets.append({
            "t": t,
            "cpu": round(1.2 + 0.3 * (i % 5), 4),
            # T8 (§5.5, additive): measured off-CPU AAS — a stacked sibling of
            # "cpu" (the on-CPU share). The real server always emits it; 0 where
            # no measured cpu_ns exists. Kept small/nonzero here so the mirror is
            # faithful without perturbing the aas snapshot (aas.js renders a
            # fixed WAIT_CLASSES list and does not pick up this key).
            "offcpu": round(0.1 + 0.02 * (i % 4), 4),
            "io":  round(0.8 + 0.2 * (i % 3), 4),
            "lock": round(0.1 + 0.05 * (i % 7), 4),
            "lwlock": round(0.3 + 0.1 * (i % 4), 4),
            "ipc": 0.02,
            "client": 0.0,
            "timeout": round(0.5 + 0.1 * (i % 6), 4),
            "bufferpin": 0.0,
            "activity": 0.0,
            "extension": round(0.9 + 0.05 * (i % 3), 4),
            "unknown": 0.0,
            # T2 (additive): the same AAS decomposed by category.
            # io_worker appears ONLY here, never in the class keys above.
            "cat": {
                "planning": 0.05,
                "execution": round(2.0 + 0.4 * (i % 5), 4),
                "command": 0.3,
                "maintenance": 0.1,
                "background": 0.05,
                "io_worker": round(0.5 + 0.1 * (i % 3), 4),
            },
        })
    return buckets

_AAS_BUCKETS = _make_aas_buckets()

def _aas_event_series():
    series = [
        {"name": "IO:DataFileRead", "event_id": 0x01000015},
        {"name": "IO:WalSync",      "event_id": 0x0100004e},
        {"name": "IO:WalWrite",     "event_id": 0x01000050},
    ]
    buckets = []
    for i in range(60):
        t = _FROM_NS + i * _BUCKET_NS
        buckets.append({
            "t": t,
            "aas": [
                round(0.4 + 0.1 * (i % 3), 4),
                round(0.2 + 0.05 * (i % 5), 4),
                round(0.2 + 0.05 * (i % 4), 4),
            ],
        })
    return series, buckets

_CANNED = {}

# Protocol revision the mock speaks — mirrors PGWT_PROTOCOL_REV in
# src/pg_wait_tracer.h. test_protocol_drift.py enforces that the real server's
# `info` response carries the same (server_version + protocol) shape.
PROTOCOL_REV = 1

_CANNED["info"] = {
    "from_ns": _FROM_NS,
    "to_ns": _TO_NS,
    "now_ns": _TO_NS,
    "num_cpus": 4,
    "num_events": 1_250_000,
    # Version handshake (T7 / TST-11): the real server reports these so the
    # client can warn on skew. Canned value differs from the real fixture's
    # git-describe string — protocol-drift compares SHAPE (string/number), not
    # values, so that is fine.
    "server_version": "v0.0-mock",
    "protocol": PROTOCOL_REV,
}

_CANNED["time_model"] = {
    "wall_ms": 3600000,
    "db_time_ms": 12500,
    "idle_time_ms": 45000,
    "aas": 3.47,
    # T2 (additive): category decomposition + io_worker utilization
    # (raw path; io_worker ms is OUTSIDE DB Time).
    "categories": [
        {"name": "planning",    "ms": 400,   "aas": 0.11},
        {"name": "execution",   "ms": 9000,  "aas": 2.50},
        {"name": "command",     "ms": 1600,  "aas": 0.44},
        {"name": "maintenance", "ms": 900,   "aas": 0.25},
        {"name": "background",  "ms": 600,   "aas": 0.17},
        {"name": "io_worker",   "ms": 2400,  "aas": 0.67},
    ],
    "io_worker_busy_pct": 22.2,
    # T8 (§5.5, additive): measured-CPU decomposition + self-checks. The real
    # server always emits these on time_model. has_measured_cpu gates the
    # Off-CPU* row (present on v3 exact data, absent on sampled/v2). cpu_ms is
    # the measured CPU* total, offcpu_ms its off-CPU sibling (both in DB Time).
    # wait_gap_cpu_ms / cpu_clamped_ms are the accounting self-checks. NOTE: no
    # Off-CPU* row is added to `rows` below — the visual-snapshot baselines are
    # keyed to this exact table layout (see the top_events note) and the row
    # schema is identical to any other, so protocol-drift is satisfied by the
    # top-level keys alone.
    "has_measured_cpu": True,
    "cpu_ms": 4800,
    "offcpu_ms": 260,
    "wait_gap_cpu_ms": 0.003,
    "cpu_clamped_ms": 0.0,
    "rows": [
        {"indent": 0, "name": "DB Time",  "ms": 12500, "pct": 100.0, "aas": 3.47},
        {"indent": 1, "name": "CPU*",     "ms": 4800,  "pct": 38.4,  "aas": 1.33},
        {"indent": 1, "name": "IO",       "ms": 3200,  "pct": 25.6,  "aas": 0.89},
        {"indent": 2, "name": "IO:DataFileRead", "ms": 2100, "pct": 16.8, "aas": 0.58},
        {"indent": 2, "name": "IO:WalSync",      "ms": 800,  "pct": 6.4,  "aas": 0.22},
        {"indent": 2, "name": "IO:WalWrite",     "ms": 300,  "pct": 2.4,  "aas": 0.08},
        {"indent": 1, "name": "Lock",     "ms": 1500,  "pct": 12.0,  "aas": 0.42},
        {"indent": 1, "name": "LWLock",   "ms": 1200,  "pct": 9.6,   "aas": 0.33},
        {"indent": 1, "name": "Timeout",  "ms": 1000,  "pct": 8.0,   "aas": 0.28},
        {"indent": 1, "name": "Extension","ms": 800,   "pct": 6.4,   "aas": 0.22},
        {"indent": 0, "name": "Idle",     "ms": 45000, "pct": 0,     "aas": 0.0},
    ],
}

_CANNED["top_events"] = {
    "db_time_ms": 12500,
    "rows": [
        {"name": "CPU*",             "event_id": 0,          "class": "CPU",
         "count": 250000, "total_ms": 4800, "avg_us": 19.2, "p50_us": 12,
         "p95_us": 45, "p99_us": 120, "max_us": 5000, "pct": 38.4, "aas": 1.33},
        {"name": "IO:DataFileRead",  "event_id": 0x01000015, "class": "IO",
         "count": 85000, "total_ms": 2100, "avg_us": 24.7, "p50_us": 15,
         "p95_us": 80, "p99_us": 250, "max_us": 12000, "pct": 16.8, "aas": 0.58},
        {"name": "Lock:relation",    "event_id": 0x03000000, "class": "Lock",
         "count": 12000, "total_ms": 1500, "avg_us": 125.0, "p50_us": 50,
         "p95_us": 500, "p99_us": 2000, "max_us": 50000, "pct": 12.0, "aas": 0.42},
        {"name": "LWLock:WALWrite",  "event_id": 0x04000008, "class": "LWLock",
         "count": 30000, "total_ms": 1200, "avg_us": 40.0, "p50_us": 25,
         "p95_us": 100, "p99_us": 400, "max_us": 8000, "pct": 9.6, "aas": 0.33},
        {"name": "Timeout:PgSleep",  "event_id": 0x09000002, "class": "Timeout",
         "count": 500, "total_ms": 1000, "avg_us": 2000000, "p50_us": 2000000,
         "p95_us": 2000000, "p99_us": 2000000, "max_us": 2001000, "pct": 8.0, "aas": 0.28},
        {"name": "IO:WalSync",       "event_id": 0x0100004e, "class": "IO",
         "count": 15000, "total_ms": 800, "avg_us": 53.3, "p50_us": 30,
         "p95_us": 150, "p99_us": 500, "max_us": 10000, "pct": 6.4, "aas": 0.22},
        {"name": "Extension:Extension", "event_id": 0x0a000000, "class": "Extension",
         "count": 800, "total_ms": 800, "avg_us": 1000000, "p50_us": 1000000,
         "p95_us": 1000000, "p99_us": 1000000, "max_us": 1001000, "pct": 6.4, "aas": 0.22},
        {"name": "IO:WalWrite",      "event_id": 0x01000050, "class": "IO",
         "count": 10000, "total_ms": 300, "avg_us": 30.0, "p50_us": 20,
         "p95_us": 80, "p99_us": 200, "max_us": 5000, "pct": 2.4, "aas": 0.08},
    ],
}
# NOTE on the idle-but-visible %DB shape:
# The real server emits pct=null for idle-but-visible events (Client:ClientRead)
# in top_events — %DB is "share of DB Time" and idle time is not part of DB
# Time, so a numeric %DB is meaningless (see src/server.c handle_top_events).
# We deliberately do NOT add such a row to this canned top_events dataset:
# the visual-snapshot baselines (tests/web_snapshots/*.png, GATING in CI and
# regenerated only via workflow_dispatch) are keyed to this exact layout, and
# adding a row would change the rendered table height. The null-pct -> "—"
# rendering is covered deterministically by the Node builder unit test
# (tests/web_unit/events.test.mjs) and end-to-end against the real server by
# tests/test_data_idle.py. protocol-drift stays consistent because a null
# field merges optionally into the same numeric schema (test_protocol_drift.py).

_CANNED["top_sessions"] = {
    "rows": [
        {"pid": 1001, "type": "client", "user": "postgres", "db": "testdb",
         "db_time_ms": 5200, "cpu_pct": 45.0, "wait_pct": 55.0,
         "top_wait": "IO:DataFileRead", "top_wait_id": 0x01000015},
        {"pid": 1002, "type": "client", "user": "postgres", "db": "testdb",
         "db_time_ms": 3800, "cpu_pct": 38.0, "wait_pct": 62.0,
         "top_wait": "Lock:relation", "top_wait_id": 0x03000000},
        {"pid": 1003, "type": "client", "user": "app", "db": "mydb",
         "db_time_ms": 2500, "cpu_pct": 52.0, "wait_pct": 48.0,
         "top_wait": "LWLock:WALWrite", "top_wait_id": 0x04000008},
        {"pid": 1004, "type": "client", "user": "app", "db": "mydb",
         "db_time_ms": 1000, "cpu_pct": 30.0, "wait_pct": 70.0,
         "top_wait": "Timeout:PgSleep", "top_wait_id": 0x09000002},
        {"pid": 4870, "type": "checkpointer", "user": "", "db": "",
         "db_time_ms": 800, "cpu_pct": 10.0, "wait_pct": 90.0,
         "top_wait": "Timeout:CheckpointWriteDelay", "top_wait_id": 0x09000000},
        {"pid": 4871, "type": "bgwriter", "user": "", "db": "",
         "db_time_ms": 200, "cpu_pct": 5.0, "wait_pct": 95.0,
         "top_wait": "IO:DataFileWrite", "top_wait_id": 0x01000018},
    ],
}

_CANNED["top_queries"] = {
    "db_time_ms": 12500,
    "rows": [
        {"query_id": "3886912043147135675", "text": "UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2",
         "total_ms": 4200, "pct": 33.6, "count": 45000, "avg_us": 93.3,
         "top_wait": "IO:DataFileRead", "top_wait_id": 0x01000015,
         # Lifecycle stats (emitted by the real server when plan/exec
         # markers are present in the trace)
         "exec_count": 45000, "plan_count": 45000,
         "exec_total_ms": 4150.0, "avg_exec_ms": 0.092,
         "p95_exec_ms": 0.31, "p99_exec_ms": 1.2,
         "avg_plan_ms": 0.011, "p95_plan_ms": 0.04, "p99_plan_ms": 0.09,
         "classes": [2000, 1200, 500, 300, 0, 0, 100, 0, 0, 100, 0],
         "events": [
            {"name": "CPU*", "id": 0, "ms": 2000},
            {"name": "IO:DataFileRead", "id": 0x01000015, "ms": 1200},
            {"name": "Lock:relation", "id": 0x03000000, "ms": 500},
            {"name": "LWLock:WALWrite", "id": 0x04000008, "ms": 300},
            {"name": "Extension:Extension", "id": 0x0a000000, "ms": 100},
            {"name": "Timeout:PgSleep", "id": 0x09000002, "ms": 100},
         ]},
        {"query_id": "5371305355164922084", "text": "SELECT abalance FROM pgbench_accounts WHERE aid = $1",
         "total_ms": 3100, "pct": 24.8, "count": 45000, "avg_us": 68.9,
         "top_wait": "IO:DataFileRead", "top_wait_id": 0x01000015,
         "classes": [1500, 900, 200, 300, 0, 0, 100, 0, 0, 100, 0],
         "events": [
            {"name": "CPU*", "id": 0, "ms": 1500},
            {"name": "IO:DataFileRead", "id": 0x01000015, "ms": 900},
            {"name": "Lock:relation", "id": 0x03000000, "ms": 200},
            {"name": "LWLock:WALWrite", "id": 0x04000008, "ms": 300},
            {"name": "Extension:Extension", "id": 0x0a000000, "ms": 100},
            {"name": "Timeout:PgSleep", "id": 0x09000002, "ms": 100},
         ]},
        {"query_id": "-2312456789012345678", "text": "INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP)",
         "total_ms": 2800, "pct": 22.4, "count": 45000, "avg_us": 62.2,
         "top_wait": "LWLock:WALWrite", "top_wait_id": 0x04000008,
         "classes": [1000, 800, 300, 500, 0, 0, 100, 0, 0, 100, 0],
         "events": [
            {"name": "CPU*", "id": 0, "ms": 1000},
            {"name": "IO:WalSync", "id": 0x0100004e, "ms": 800},
            {"name": "LWLock:WALWrite", "id": 0x04000008, "ms": 500},
            {"name": "Lock:relation", "id": 0x03000000, "ms": 300},
            {"name": "Extension:Extension", "id": 0x0a000000, "ms": 100},
            {"name": "Timeout:PgSleep", "id": 0x09000002, "ms": 100},
         ]},
    ],
}

_CANNED["variants"] = {
    "exec": {
        "total": 45000,
        "num_variants": 2,
        "variants": [
            {"exec_count": 30000, "num_queries": 1, "total_ms": 2790.0,
             "avg_ms": 0.093, "p95_ms": 0.30, "avg_loop_n": 1,
             "top_query_id": 3886912043147135675,
             "steps": [
                 {"name": "CPU*", "avg_ms": 0.04, "class": "cpu"},
                 {"name": "IO:DataFileRead", "avg_ms": 0.03, "class": "IO"},
                 {"name": "CPU*", "avg_ms": 0.023, "class": "cpu"},
             ],
             "query_text": "UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2"},
            {"exec_count": 15000, "num_queries": 1, "total_ms": 1360.0,
             "avg_ms": 0.09, "p95_ms": 0.28, "avg_loop_n": 1,
             "top_query_id": 5371305355164922084,
             "steps": [
                 {"name": "CPU*", "avg_ms": 0.05, "class": "cpu"},
                 {"name": "LWLock:WALWrite", "avg_ms": 0.04, "class": "LWLock"},
             ],
             "query_text": "SELECT abalance FROM pgbench_accounts WHERE aid = $1"},
        ],
    },
    "plan": {
        "total": 45000,
        "num_variants": 1,
        "variants": [
            {"exec_count": 45000, "num_queries": 2, "total_ms": 495.0,
             "avg_ms": 0.011, "p95_ms": 0.04, "avg_loop_n": 1,
             "top_query_id": 3886912043147135675,
             "steps": [
                 {"name": "CPU*", "avg_ms": 0.011, "class": "cpu"},
             ],
             "query_text": "UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2"},
        ],
    },
}

_CANNED["heatmap"] = {
    "bucket_ns": _BUCKET_NS,
    "max_count": 5000,
    "total_events": 402500,
    "times": [_FROM_NS + i * _BUCKET_NS for i in range(60)],
    "labels": ["<1", "1-2", "2-4", "4-8", "8-16", "16-32", "32-64",
               "64-128", "128-256", "256-512", "512-1K", "1K-2K",
               "2K-4K", "4K-8K", "8K-16K", ">=16K"],
    "cells": [
        [i, j, max(0, 5000 - abs(j - 3) * 800 - abs(i - 30) * 50)]
        for i in range(0, 60, 3)
        for j in range(16)
        if max(0, 5000 - abs(j - 3) * 800 - abs(i - 30) * 50) > 0
    ],
}

_CANNED["session_timeline"] = {
    "truncated": False,
    "total_count": 8,
    "pids": [1001, 1002],
    "events": [
        {"s": _FROM_NS + 100_000_000_000, "d": 50_000_000_000, "p": 1001,
         "n": "CPU*", "e": 0, "c": 0, "q": "3886912043147135675"},
        {"s": _FROM_NS + 150_000_000_000, "d": 30_000_000_000, "p": 1001,
         "n": "IO:DataFileRead", "e": 0x01000015, "c": 1, "q": "3886912043147135675"},
        {"s": _FROM_NS + 180_000_000_000, "d": 20_000_000_000, "p": 1001,
         "n": "Lock:relation", "e": 0x03000000, "c": 2, "q": "3886912043147135675"},
        {"s": _FROM_NS + 200_000_000_000, "d": 40_000_000_000, "p": 1001,
         "n": "CPU*", "e": 0, "c": 0, "q": "3886912043147135675"},
        {"s": _FROM_NS + 100_000_000_000, "d": 80_000_000_000, "p": 1002,
         "n": "Lock:relation", "e": 0x03000000, "c": 2, "q": "5371305355164922084"},
        {"s": _FROM_NS + 180_000_000_000, "d": 25_000_000_000, "p": 1002,
         "n": "CPU*", "e": 0, "c": 0, "q": "5371305355164922084"},
        {"s": _FROM_NS + 205_000_000_000, "d": 35_000_000_000, "p": 1002,
         "n": "IO:DataFileRead", "e": 0x01000015, "c": 1, "q": "5371305355164922084"},
        {"s": _FROM_NS + 240_000_000_000, "d": 15_000_000_000, "p": 1002,
         "n": "LWLock:WALWrite", "e": 0x04000008, "c": 3, "q": "5371305355164922084"},
    ],
}


# View commands that carry a per-window "fidelity" indicator (A3 / D3).
# The mock's canned dataset is full-fidelity transition data, so every view
# reports "exact" — matching what the real server returns for the
# transition-only protocol-drift fixture.
_FIDELITY_VIEWS = {
    "aas", "time_model", "top_events", "top_sessions", "top_queries",
    "heatmap", "session_timeline", "transitions", "fingerprints",
    "concurrency", "lock_chains", "interference", "variants",
    "executions", "execution_detail", "exec_scatter",
}

# EXACT-required views (mirror src/server.c's PGWT_REQ_EXACT handlers): over a
# sampled-only window these return the structured unavailable marker instead of
# data. The web UI renders the "escalate to capture" panel for these.
_EXACT_VIEWS = {"heatmap", "transitions", "concurrency", "lock_chains",
                "interference", "executions", "execution_detail",
                "exec_scatter"}

# ── Fidelity + control configuration (Phase B5) ───────────────────────────────
# The mock can present a SAMPLED or MIXED window so the UI's fidelity shading +
# unavailable panels can be driven deterministically. Configured via env (like
# chaos) so test_web_ui.py can launch a mock in any fidelity mode without a new
# protocol surface. The control command implements a small escalate/deescalate
# state machine mirroring src/control.c so the escalate flow is exercisable.
#
#   PGWT_MOCK_FIDELITY=exact|sampled|mixed   window fidelity for all views
#                                            (default exact — keeps existing
#                                            tests + protocol-drift green)
#   PGWT_MOCK_SAMPLE_PERIOD_NS=100000000     sample period reported when sampled
#   PGWT_MOCK_DAEMON=1                        enable the control command (escalate
#                                            etc.); 0 → "daemon not running"
#                                            (default 0, matching static replay)
#   PGWT_MOCK_TIER=sampled|escalated         initial daemon tier (default sampled)
#   PGWT_MOCK_BUDGET_S=300                    escalation budget remaining seconds


class FidelityConfig:
    def __init__(self):
        self.fidelity = os.environ.get("PGWT_MOCK_FIDELITY", "exact")
        if self.fidelity not in ("exact", "sampled", "mixed"):
            self.fidelity = "exact"
        self.sample_period_ns = _env_int("PGWT_MOCK_SAMPLE_PERIOD_NS", 100_000_000)


_FID = FidelityConfig()


class DaemonState:
    """Minimal escalate/deescalate state machine mirroring src/control.c.

    Lets the Playwright escalate-flow test drive a real state transition
    (sampled → escalated → budget decreases) against the mock without a daemon.
    """

    def __init__(self):
        self.enabled = os.environ.get("PGWT_MOCK_DAEMON", "0") not in ("0", "", "false")
        self.tier = os.environ.get("PGWT_MOCK_TIER", "sampled")
        self.budget_remaining_s = float(_env_int("PGWT_MOCK_BUDGET_S", 300))
        self.seconds_remaining = 60.0 if self.tier == "escalated" else 0.0
        self.reason = "manual" if self.tier == "escalated" else "none"
        self.windows_total = 1 if self.tier == "escalated" else 0
        self.denied_total = 0

    def status(self):
        return {
            "ok": True,
            "mode": "tiered",
            "uptime_s": 123.4,
            "backends": 6,
            "pg_pid": 4000,
            "version": "mock",
            "tier": self.tier,
            "escalation_supported": True,
            "escalation_seconds_remaining": self.seconds_remaining,
            "escalation_budget_remaining_s": self.budget_remaining_s,
            # ESC-6: unlimited surfaces as budget_remaining_s == -1 + this flag.
            "escalation_budget_unlimited": self.budget_remaining_s < 0,
            "escalation_reason": self.reason if self.tier == "escalated" else "none",
            # Sampler read health (T4/SMP-1): false + reason means the sampler
            # cannot read backend memory — "blind", not "idle".
            "sampler_healthy": True,
            "sampler_unhealthy_reason": "",
            # T8 (§5.4): measured-CPU capability. "measured" = schedstat present
            # (exact tier reads se.sum_exec_runtime); "legacy" = gap-inference.
            "cpu_accounting": "measured",
        }

    def metrics(self):
        return {
            "ok": True,
            "uptime_s": 123.4,
            "events_total": 1_250_000,
            "events_per_sec": 4200.0,
            "lifecycle_events_total": 90000,
            "wp_attach_failures_total": 0,
            "backends_tracked": 6,
            "samples_total": 540000,
            "samples_per_sec": 60.0,
            "sample_read_faults_total": 0,
            "sampled_attr_tick_read_failures_total": 0,
            "sampled_attr_shadow_total": 0,
            "sampled_attr_shadow_mismatch_total": 0,
            "sampled_attr_shadow_active_total": 0,
            "sampled_attr_shadow_active_mismatch_total": 0,
            "sampled_attr_shadow_cmd_open_mismatch_total": 0,
            "sampled_attr_shadow_query_id_mismatch_total": 0,
            # Capture hardening counters (T4): non-zero = loudly-logged
            # degradation (CAP-1/2/5/6, SMP-1/3).
            "sampler_ticks_missed_total": 0,
            "state_map_full_total": 0,
            "seen_query_ids_full_total": 0,
            "sampled_text_pending": 0,
            "sampled_text_resolved_total": 0,
            "sampled_text_absent_total": 0,
            "sampled_text_evicted_total": 0,
            "sampled_text_error_total": 0,
            "sampled_text_retry_exhausted_total": 0,
            "invalid_wait_reads_total": 0,
            "state_reseeds_total": 0,
            "sampler_healthy": True,
            "ringbuf_drops_total": 3,
            "trace_events_written_total": 1_250_000,
            "trace_bytes_written_total": 18_000_000,
            "tier": self.tier,
            "escalation_active": self.tier == "escalated",
            "escalation_seconds_remaining": self.seconds_remaining,
            "escalation_budget_remaining_s": self.budget_remaining_s,
            "escalation_budget_unlimited": self.budget_remaining_s < 0,
            "escalation_windows_total": self.windows_total,
            "escalation_denied_total": self.denied_total,
            "escalation_budget_closed_total": 0,
            "anomaly_fires_total": 2,
            "anomaly_near_total": 5,
            "anomaly_dropped_budget_total": 1,
            "anomaly_dropped_cooldown_total": 0,
            "anomaly_baseline_aas": 1.85,
            # T2 decomposed-AAS observability
            "io_worker_busy_pct": 22.2,
            # T8 measured-CPU counters (§5.6). Lifetime ns totals over exact-tier
            # measured intervals; wait_gap_cpu_ns_total is the ≈0 self-check.
            "cpu_accounting": "measured",
            "cpu_ns_total": 480_000_000_000,
            "offcpu_ns_total": 12_000_000_000,
            "cpu_clamped_total": 0,
            "wait_gap_cpu_ns_total": 3_000_000,
            "io_worker_samples_total": 18000,
            "io_worker_busy_total": 4000,
            "noncmd_cpu_samples_total": 120000,
        }

    def escalate(self, duration_s, reason):
        duration_s = float(duration_s or 60)
        if duration_s > self.budget_remaining_s:
            self.denied_total += 1
            return {"ok": False, "error": "over budget",
                    "budget_remaining_s": self.budget_remaining_s}
        if self.tier != "escalated":
            self.windows_total += 1
        self.tier = "escalated"
        self.reason = reason or "manual"
        self.seconds_remaining = max(self.seconds_remaining, duration_s)
        self.budget_remaining_s = max(0.0, self.budget_remaining_s - duration_s)
        return {"ok": True, "escalated": True, "granted_s": duration_s,
                "seconds_remaining": self.seconds_remaining,
                "budget_remaining_s": self.budget_remaining_s}

    def deescalate(self):
        self.tier = "sampled"
        self.seconds_remaining = 0.0
        self.reason = "none"
        return {"ok": True, "escalated": False,
                "budget_remaining_s": self.budget_remaining_s}


_DAEMON = DaemonState()


class UpstreamState:
    """Simulates the bridge's dead-upstream behavior (Trust Milestone UI-1/UI-3).

    In production the browser's WS goes to the local Go bridge; when the
    SSH/pgwt-server pipe behind it dies the WS STAYS OPEN and every request is
    answered with {"id":N,"error":"...","transport":true}. Tests flip this
    state via the test-only `test_upstream_down` / `test_upstream_up` commands
    to drive the UI's degraded-transport state deterministically.
    """

    def __init__(self):
        self.up = True


_UPSTREAM = UpstreamState()


def _handle_control(req_id, msg):
    """Proxy a control command (mirrors src/server.c handle_control)."""
    request = msg.get("request")
    if not isinstance(request, dict):
        return {"id": req_id, "error": "missing request object"}
    if not _DAEMON.enabled:
        return {"id": req_id, "error": "daemon not running"}
    cmd = request.get("cmd")
    if cmd == "status":
        return {"id": req_id, "response": _DAEMON.status()}
    if cmd == "metrics":
        return {"id": req_id, "response": _DAEMON.metrics()}
    if cmd == "escalate":
        return {"id": req_id, "response": _DAEMON.escalate(
            request.get("duration_s"), request.get("reason"))}
    if cmd == "deescalate":
        return {"id": req_id, "response": _DAEMON.deescalate()}
    return {"id": req_id, "response": {"ok": False, "error": "unknown command"}}


def handle_request(msg):
    """Dispatch a WebSocket JSON request and return canned response."""
    cmd = msg.get("cmd", "")
    req_id = msg.get("id", 0)

    # Test-only upstream-death simulation (mirrors web/bridge.go envelopes).
    if cmd == "test_upstream_down":
        _UPSTREAM.up = False
        return {"id": req_id, "ok": True}
    if cmd == "test_upstream_up":
        _UPSTREAM.up = True
        return {"id": req_id, "ok": True}
    if not _UPSTREAM.up:
        return {"id": req_id, "error": "server disconnected", "transport": True}

    if cmd == "control":
        return _handle_control(req_id, msg)

    # EXACT-required views over a sampled-only window return the structured
    # unavailable marker (mirrors src/server.c emit_unavailable). A mixed
    # window still has transition data, so it stays available.
    if cmd in _EXACT_VIEWS and _FID.fidelity == "sampled":
        return {"id": req_id, "unavailable": "requires full-fidelity data",
                "code": "full_fidelity_required",
                "hint": "capture an exact window (escalate) and retry",
                "fidelity": "sampled"}

    resp = _handle_request_inner(cmd, req_id, msg)
    # Tag every view response with its window fidelity (A3). Done centrally so
    # the schema stays aligned with the real server across all views.
    response_fidelity = "sampled" if _is_compare_baseline(msg) else _FID.fidelity
    if cmd in _FIDELITY_VIEWS and "fidelity" not in resp and "error" not in resp:
        resp["fidelity"] = response_fidelity
        if response_fidelity in ("sampled", "mixed"):
            resp["sample_period_ns"] = _FID.sample_period_ns
    # FID-3 (T1): over a sampled-only window the real server gates the
    # latency columns — samples carry no real durations, so avg/percentiles
    # are null and the UI renders "—". Mirror it so the web tests exercise
    # the same shape.
    if cmd == "top_events" and response_fidelity == "sampled" and "rows" in resp:
        gated = []
        for row in resp["rows"]:
            row = dict(row)
            for col in ("avg_us", "p50_us", "p95_us", "p99_us", "max_us"):
                row[col] = None
            gated.append(row)
        resp["rows"] = gated
    return resp


def _handle_request_inner(cmd, req_id, msg):
    if cmd == "info":
        global _INFO_TICKS
        resp = {"id": req_id, **_CANNED["info"]}
        if _COMPARE_MODE:
            resp["now_ns"] = _TO_NS + _INFO_TICKS * 30_000_000_000
            _INFO_TICKS += 1
        return resp

    if cmd == "aas":
        # Protocol-faithful: a window that does not intersect the canned data
        # returns EMPTY buckets (like the real server for an out-of-range
        # window). Lets Playwright drive the AAS empty-state (UI-5). Requests
        # without from/to (e.g. protocol-drift) keep the canned data.
        req_from, req_to = msg.get("from"), msg.get("to")
        if (req_from is not None and req_to is not None and
                (req_from > _TO_NS or req_to < _FROM_NS)):
            return {"id": req_id, "bucket_ns": _BUCKET_NS, "max_aas": 0,
                    "buckets": []}
        resp = {"id": req_id, "bucket_ns": _BUCKET_NS, "max_aas": 4.5}
        if msg.get("detail") == "events":
            series, buckets = _aas_event_series()
            resp["breakdown"] = "events"
            resp["series"] = series
            resp["buckets"] = buckets
        else:
            if _is_compare_baseline(msg):
                resp["buckets"] = [dict(b, cpu=round(b["cpu"] * 0.55, 4),
                    io=round(b["io"] * 1.35, 4), lock=round(b["lock"] * 0.4, 4))
                    for b in _AAS_BUCKETS]
            else:
                resp["buckets"] = _AAS_BUCKETS
        return resp

    if cmd == "time_model":
        resp = {"id": req_id, **_CANNED["time_model"]}
        if _is_compare_baseline(msg):
            resp["db_time_ms"] = 10000
            resp["rows"] = [dict(r, ms=round(r["ms"] * 0.8, 3))
                            for r in _CANNED["time_model"]["rows"]]
        return resp

    if cmd == "top_events":
        rows = _CANNED["top_events"]["rows"]
        filters = msg.get("filters", {})
        if "class" in filters:
            cls = filters["class"]
            rows = [r for r in rows if r["class"].lower() == cls.lower()]
        if _is_compare_baseline(msg):
            rows = _baseline_rows(rows, "total_ms")
        return {"id": req_id,
                "db_time_ms": _CANNED["top_events"]["db_time_ms"],
                "rows": rows}

    if cmd == "top_sessions":
        return {"id": req_id, **_CANNED["top_sessions"]}

    if cmd == "top_queries":
        return {"id": req_id, **_CANNED["top_queries"]}

    if cmd == "heatmap":
        return {"id": req_id, **_CANNED["heatmap"]}

    if cmd == "session_timeline":
        filters = msg.get("filters", {})
        if "pid" in filters or "query_id" in filters:
            resp = {"id": req_id, **_CANNED["session_timeline"]}
            # Protocol-faithful: the real server returns the timeline for the
            # filtered pid. Reflecting the requested pid into the response also
            # makes responses request-distinguishable, so a chaos test can
            # detect a stale (wrong-pid) render overwriting a fresh one.
            if "pid" in filters:
                pid = filters["pid"]
                resp["pids"] = [pid]
                resp["events"] = [{**e, "p": pid} for e in resp["events"]]
            return resp
        return {"id": req_id, "events": [], "pids": [], "truncated": False, "total_count": 0}

    if cmd == "executions":
        filters = msg.get("filters", {})
        qid = str(filters.get("query_id", "100"))
        rows = [
            # Latest first, like server.c handle_executions.
            {"pid": 1002, "query_id": qid,
             "start_ns": "10000100000000", "end_ns": "10000180000000",
             "duration_ms": 80.0, "plan_ms": None,
             "n_events": 2, "n_workers": 0, "in_progress": False,
             "started_before_window": False},
            {"pid": 1000, "query_id": qid,
             "start_ns": "10000000000000", "end_ns": "10000030001000",
             "duration_ms": 30.001, "plan_ms": 1.0,
             "n_events": 5, "n_workers": 2, "in_progress": False,
             "started_before_window": False},
        ]
        if "pid" in filters:
            rows = [r for r in rows if r["pid"] == filters["pid"]]
        return {"id": req_id, "rows": rows,
                "total_count": len(rows), "truncated": False}

    if cmd == "execution_detail":
        filters = msg.get("filters", {})
        pid = filters.get("pid", 1000)
        qid = str(filters.get("query_id", "100"))
        start = str(msg.get("start_ns", "10000000000000"))
        if pid == 1002:
            events = [
                {"we": 0x0100004e, "name": "IO:WalSync",
                 "start_ns": start, "dur_ns": "20000000", "cpu_ns": None},
                {"we": 0, "name": "CPU*", "start_ns": "10000130000000",
                 "dur_ns": "30000000", "cpu_ns": None},
            ]
            return {"id": req_id, "query_id": qid,
                    "leader": {"pid": pid, "query_id": qid,
                               "events": events, "total_count": 2,
                               "truncated": False},
                    "workers": [], "plan": None,
                    "total_count": 2, "kept_count": 2, "truncated": False}
        event = {"we": 0x01000015, "name": "IO:DataFileRead",
                 "start_ns": start, "dur_ns": "10000000", "cpu_ns": None}
        return {"id": req_id, "query_id": qid,
                "leader": {"pid": pid, "query_id": qid,
                           "events": [event,
                                {"we": 0, "name": "CPU*",
                                 "start_ns": "10000012000000",
                                "dur_ns": "8000000", "cpu_ns": None}],
                           "total_count": 2, "truncated": False},
                "workers": [
                    {"pid": 1006, "events": [dict(event)],
                     "total_count": 1, "truncated": False},
                    {"pid": 1008, "events": [dict(event)],
                     "total_count": 1, "truncated": False},
                ],
                "plan": {"start_ns": "9999998000000",
                         "end_ns": "9999999000000"},
                "total_count": 4, "kept_count": 4, "truncated": False}

    if cmd == "exec_scatter":
        return {"id": req_id, "points": [
            {"t": "10000000000000", "duration_ms": 30.001,
             "pid": 1000, "query_id": "100", "in_progress": False},
            {"t": "10000100000000", "duration_ms": 80.0,
             "pid": 1002, "query_id": "200", "in_progress": False},
            {"t": "10000200000000", "duration_ms": None,
             "pid": 1004, "query_id": "300", "in_progress": True},
        ], "total_count": 3, "kept_count": 3, "downsampled": False}

    if cmd == "transitions":
        # event_id mirrors server.c's DFG node JSON (U2 / P3 wire 5: node
        # click pivots on the event; 0 = the CPU* pseudo-node).
        return {"id": req_id, "total": 1800, "link_count": 5,
                "total_link_count": 8, "truncated": True, "nodes": [
            {"name": "CPU*", "total_ms": 4800, "class": "CPU", "event_id": 0},
            {"name": "IO:DataFileRead", "total_ms": 2100, "class": "IO", "event_id": 0x01000015},
            {"name": "LWLock:WALInsert", "total_ms": 900, "class": "LWLock", "event_id": 0x04000007},
            {"name": "IO:WalSync", "total_ms": 800, "class": "IO", "event_id": 0x0100004e},
        ], "links": [
            {"source": "CPU*", "target": "IO:DataFileRead", "value": 500, "duration_ms": 2500.0},
            {"source": "IO:DataFileRead", "target": "CPU*", "value": 480, "duration_ms": 1920.0},
            {"source": "CPU*", "target": "LWLock:WALInsert", "value": 300, "duration_ms": 900.0},
            {"source": "LWLock:WALInsert", "target": "CPU*", "value": 290, "duration_ms": 870.0},
            {"source": "CPU*", "target": "IO:WalSync", "value": 100, "duration_ms": 3200.0},
        ]}

    if cmd == "lock_chains":
        return {"id": req_id, "chains": [
            {"waiter": 1001, "blocker": 1000, "lock": "Lock:transactionid", "wait_ms": 45.2, "timestamp_ns": 1711936100000000000},
            {"waiter": 1003, "blocker": 1002, "lock": "Lock:tuple", "wait_ms": 12.8, "timestamp_ns": 1711936200000000000},
        ]}

    if cmd == "interference":
        return {"id": req_id, "rows": [
            {"pid_a": 1001, "pid_b": 1003, "score": 1.0, "top_event": "LWLock:BufferMapping", "overlap_ms": 234.5},
            {"pid_a": 1002, "pid_b": 1004, "score": 0.72, "top_event": "IO:DataFileRead", "overlap_ms": 168.3},
        ]}

    if cmd == "concurrency":
        nb = msg.get("num_buckets", 60)
        peaks = [{"t": 1711936000000000000 + i * 60000000000,
                  "t_ms": (1711936000000000000 + i * 60000000000) // 1000000,
                  "max": 3 + (i % 5), "event": "LWLock:BufferMapping"} for i in range(nb)]
        bursts = [
            {"timestamp_ns": 1711936180000000000, "timestamp_ms": 1711936180000,
             "event": "LWLock:BufferMapping", "sessions": 8,
             "pids": [1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008]},
            {"timestamp_ns": 1711936300000000000, "timestamp_ms": 1711936300000,
             "event": "IO:DataFileRead", "sessions": 5,
             "pids": [1001, 1003, 1005, 1007, 1009]},
        ]
        return {"id": req_id, "peaks": peaks, "bursts": bursts, "bucket_ns": 60000000000}

    if cmd == "variants":
        return {"id": req_id, **_CANNED["variants"]}

    if cmd == "fingerprints":
        return {"id": req_id, "rows": [
            {"query_id": 123456, "transitions": 800, "signature": "IO:40%|CPU:35%|LWLock:25%",
             "class_pct": {"io": 40, "cpu": 35, "lwlock": 25}, "top_from": "CPU*", "top_to": "IO:DataFileRead"},
        ]}

    return {"id": req_id, "error": f"unknown command: {cmd}"}


# ── Chaos layer ──────────────────────────────────────────────────────────────
# Reproduces real network conditions (jitter, out-of-order, late responses)
# without any wall-clock RNG: every decision is a pure function of the
# request's index, so chaos tests are reproducible.


class ChaosConfig:
    """Deterministic chaos parameters, read once from env or kwargs."""

    def __init__(self, enabled=None, min_ms=None, max_ms=None, reorder=None,
                 late_every=None, late_ms=None, seed=None, late_cmds=None):
        if enabled is None:
            enabled = os.environ.get("PGWT_CHAOS", "0") not in ("0", "", "false")
        self.enabled = enabled
        self.min_ms = min_ms if min_ms is not None else _env_int("PGWT_CHAOS_MIN_MS", 50)
        self.max_ms = max_ms if max_ms is not None else _env_int("PGWT_CHAOS_MAX_MS", 300)
        if reorder is None:
            reorder = os.environ.get("PGWT_CHAOS_REORDER", "1") not in ("0", "", "false")
        self.reorder = reorder
        self.late_every = late_every if late_every is not None else _env_int("PGWT_CHAOS_LATE_EVERY", 7)
        self.late_ms = late_ms if late_ms is not None else _env_int("PGWT_CHAOS_LATE_MS", 900)
        self.seed = seed if seed is not None else _env_int("PGWT_CHAOS_SEED", 1337)
        if late_cmds is None:
            raw = os.environ.get("PGWT_CHAOS_LATE_CMDS", "")
            late_cmds = [c.strip() for c in raw.split(",") if c.strip()]
        self.late_cmds = set(late_cmds)
        if self.max_ms < self.min_ms:
            self.max_ms = self.min_ms

    def delay_ms(self, index, cmd=None):
        """Deterministic per-request latency in ms.

        Spreads jitter across [min_ms, max_ms] using a cheap integer hash of
        the request index — so two requests in flight at once almost always
        get different delays, producing out-of-order completion. A request is
        additionally "late" (gets `late_ms` tacked on, so it lands long after
        the client has moved on — the stale-overwrite race) when either its
        index hits the late_every cadence or its command is in late_cmds.
        """
        if not self.enabled:
            return 0
        h = (index * 2654435761 + self.seed * 40503) & 0xFFFFFFFF
        span = self.max_ms - self.min_ms + 1
        base = self.min_ms + (h % span)
        if self.is_late(index, cmd):
            base += self.late_ms
        return base

    def is_late(self, index, cmd=None):
        if not self.enabled:
            return False
        if cmd is not None and cmd in self.late_cmds:
            return True
        if self.late_every <= 0:
            return False
        # index is 1-based per connection; fire on the Nth, 2Nth, ...
        return index % self.late_every == 0


# ── WebSocket server ─────────────────────────────────────────────────────────

async def ws_handler(websocket, chaos=None):
    """Serve one WebSocket connection.

    With chaos disabled this is the original synchronous request/response
    loop. With chaos enabled, each request is answered by an independent
    asyncio task whose send is delayed by ChaosConfig.delay_ms() — so
    responses to concurrent in-flight requests naturally complete out of
    order, and "late" requests land well after newer ones (and after the
    client has navigated away). A per-connection asyncio.Lock keeps the
    actual websocket.send() calls from interleaving on the wire.
    """
    if chaos is None:
        chaos = ChaosConfig()

    if not chaos.enabled:
        async for raw in websocket:
            try:
                msg = json.loads(raw)
                resp = handle_request(msg)
                await websocket.send(json.dumps(resp))
            except Exception as e:
                err = {"id": 0, "error": str(e)}
                await websocket.send(json.dumps(err))
        return

    send_lock = asyncio.Lock()
    tasks = set()
    req_index = 0

    async def respond(raw, index):
        try:
            cmd = None
            try:
                msg = json.loads(raw)
                cmd = msg.get("cmd")
                resp = handle_request(msg)
            except Exception as e:
                resp = {"id": 0, "error": str(e)}
            delay = chaos.delay_ms(index, cmd)
            if delay:
                await asyncio.sleep(delay / 1000.0)
            async with send_lock:
                try:
                    await websocket.send(json.dumps(resp))
                except Exception:
                    pass  # connection closed while a delayed response waited
        except asyncio.CancelledError:
            pass

    try:
        async for raw in websocket:
            req_index += 1
            t = asyncio.ensure_future(respond(raw, req_index))
            tasks.add(t)
            t.add_done_callback(tasks.discard)
    finally:
        # Connection closed: drop any responses still waiting out their delay.
        for t in list(tasks):
            t.cancel()


# ── HTTP server ──────────────────────────────────────────────────────────────

class StaticHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=STATIC_DIR, **kwargs)

    def do_GET(self):
        # The real Go server serves a per-session WS token at /session
        # (UI-12). The mock has no token requirement; answer with a null
        # token (rather than a 404, which would log a console error in every
        # browser test) so the client connects token-less.
        if self.path == "/session":
            body = b'{"token": null}\n'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, format, *args):
        pass  # suppress logs


def run_http(port):
    httpd = HTTPServer(("127.0.0.1", port), StaticHandler)
    httpd.serve_forever()


# ── Main ─────────────────────────────────────────────────────────────────────

async def run_ws(port, chaos):
    async def handler(ws):
        await ws_handler(ws, chaos)
    async with websockets.asyncio.server.serve(
        handler, "127.0.0.1", port,
        origins=None,
    ):
        await asyncio.Future()


def main():
    if not _HAVE_WEBSOCKETS:
        print("ERROR: pip install websockets", file=sys.stderr)
        sys.exit(1)

    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765,
                        help="HTTP port (WS = port+1)")
    parser.add_argument("--chaos", action="store_true",
                        help="enable chaos mode (jitter / out-of-order / "
                             "late responses); also via PGWT_CHAOS=1")
    args = parser.parse_args()

    chaos = ChaosConfig(enabled=True) if args.chaos else ChaosConfig()

    http_port = args.port
    ws_port = args.port + 1

    # HTTP in thread
    http_thread = threading.Thread(target=run_http, args=(http_port,), daemon=True)
    http_thread.start()

    mode = "CHAOS" if chaos.enabled else "normal"
    print(f"mock_server: HTTP http://127.0.0.1:{http_port}, "
          f"WS ws://127.0.0.1:{ws_port} [{mode}]", flush=True)
    if chaos.enabled:
        print(f"mock_server: chaos latency {chaos.min_ms}-{chaos.max_ms}ms, "
              f"reorder={chaos.reorder}, late_every={chaos.late_every} "
              f"(+{chaos.late_ms}ms), seed={chaos.seed}", flush=True)

    # WS in asyncio
    try:
        asyncio.run(run_ws(ws_port, chaos))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
