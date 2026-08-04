#!/usr/bin/env python3
"""test_data_transitions.py — Verify transition matrix and fingerprint computation."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

# Use numeric event IDs
IO_WRITE = 0x0A000018   # IO:DataFileWrite
LOCK_EXT = 0x03000001   # Lock:extend

SCENARIO = {
    "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
    "queries": [{"id": 100, "text": "SELECT 1"}],
    "events": [
        # CPU → IO_READ → CPU → IO_WRITE → CPU → LOCK → CPU → IO_READ → CPU
        {"pid": 1000, "ts": 10000000000000, "dur": 1000000, "old": CPU,             "new": IO_DATA_FILE_READ, "qid": 100},
        {"pid": 1000, "ts": 10000001000000, "dur": 2000000, "old": IO_DATA_FILE_READ, "new": CPU,             "qid": 100},
        {"pid": 1000, "ts": 10000003000000, "dur": 1000000, "old": CPU,             "new": IO_WRITE,         "qid": 100},
        {"pid": 1000, "ts": 10000004000000, "dur": 3000000, "old": IO_WRITE,        "new": CPU,              "qid": 100},
        {"pid": 1000, "ts": 10000007000000, "dur": 1000000, "old": CPU,             "new": LOCK_EXT,         "qid": 100},
        {"pid": 1000, "ts": 10000008000000, "dur": 4000000, "old": LOCK_EXT,        "new": CPU,              "qid": 100},
        {"pid": 1000, "ts": 10000012000000, "dur": 1000000, "old": CPU,             "new": IO_DATA_FILE_READ, "qid": 100},
        {"pid": 1000, "ts": 10000013000000, "dur": 2000000, "old": IO_DATA_FILE_READ, "new": CPU,             "qid": 100},
    ],
}

t = TestRunner("test_data_transitions")
trace_dir = generate_traces(SCENARIO)

try:
    with ServerHarness(trace_dir) as srv:
        # Test transitions endpoint
        data = srv.query("transitions")
        links = data.get("links", [])

        t.check(data.get("total") == 8, "total transitions = 8")
        t.check(data.get("truncated") is False and
                data.get("link_count") == data.get("total_link_count"),
                "uncapped response declares complete link coverage")

        capped = srv.query("transitions", buckets=2)
        t.check(capped.get("truncated") is True,
                "server discloses when its transition-link cap fires")
        t.check(capped.get("link_count") == 2 and
                capped.get("total_link_count", 0) > 2 and
                capped.get("total") == 8,
                "capped links retain total link and transition volumes")

        def find_link(src_substr, tgt_substr):
            for l in links:
                if src_substr in l["source"] and tgt_substr in l["target"]:
                    return l
            return None

        cpu_to_read = find_link("CPU", "DataFileRead")
        t.check(cpu_to_read is not None and cpu_to_read["value"] == 2,
                "CPU→IO:DataFileRead count=2")

        read_to_cpu = find_link("DataFileRead", "CPU")
        t.check(read_to_cpu is not None and read_to_cpu["value"] == 2,
                "IO:DataFileRead→CPU count=2")

        cpu_to_write = find_link("CPU", "DataFileWrite")
        t.check(cpu_to_write is not None and cpu_to_write["value"] == 1,
                "CPU→IO:DataFileWrite count=1")

        lock_to_cpu = find_link("extend", "CPU")
        t.check(lock_to_cpu is not None and lock_to_cpu["value"] == 1,
                "Lock:extend→CPU count=1")

        # Duration: IO:DataFileRead→CPU has 2 events × 2ms each = 4ms
        t.check(read_to_cpu is not None and abs(read_to_cpu["duration_ms"] - 4.0) < 0.01,
                "IO:DataFileRead→CPU duration=4ms")

        # Test fingerprints endpoint
        fp = srv.query("fingerprints")
        rows = fp.get("rows", [])
        t.check(len(rows) == 1, "1 query fingerprint")
        if rows:
            t.check(rows[0]["query_id"] == 100, "query_id = 100")
            t.check(rows[0]["transitions"] == 8, "8 transitions")
            sig = rows[0].get("signature", "")
            t.check("IO:" in sig and "CPU:" in sig, "signature has IO and CPU")
finally:
    cleanup_traces(trace_dir)

sys.exit(0 if t.summary() else 1)
