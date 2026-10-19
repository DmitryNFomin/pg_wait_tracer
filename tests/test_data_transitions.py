#!/usr/bin/env python3
"""test_data_transitions.py — Verify transition matrix and fingerprint computation.

The second half is #456: the served transitions payload must be identical
across server restarts and across permutations of the record order, and the
relation node the omission disclosure reads must be present with its exact
dwell. See the comment block above check_restart_determinism().
"""
import hashlib
import json
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, CLASS_LWLOCK, make_event_id,
)

BASE_TS = 10_000_000_000_000

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

# ── #456: the transitions payload must be identical across restarts ───────
#
# THE DEFECT AS REPORTED. Replaying one trace with byte-identical code, the
# Transitions tab's omission disclosure named `Lock:relation` in 3 of 5
# server restarts and was entirely absent in 2, with all three headline
# totals identical (3,466,745 / 849,416 / 70,110). The filed guess was a
# server-side tie-break without a total order.
#
# WHAT THE DIAGNOSTIC FOUND (10 fresh pgwt-server processes, frozen trace
# from rehearsal attempt 4, frozen request from the view itself —
# buckets=200, whole-capture bounds, no filters): ONE distinct payload. The
# nodes array, the links array and their order were byte-identical in all
# ten, and replaying that payload through the real builder named
# Lock:relation at every threshold >= 1 for both idle settings. So the
# divergence is NOT in this response path. What this section pins is the
# property the issue asks for ("start the server N times and assert the
# output is identical every time") at the layer where it can be asserted
# without a browser, and the two ways the payload COULD have gone
# non-deterministic here:
#
#   1. The pre-sort order of the link table. pgwt_compute_transitions fills
#      a flat array in HASH SLOT order and qsorts it. Two pairs that collide
#      land in slots (h, h+1) in ARRIVAL order, so the array handed to qsort
#      depends on the record order — and qsort is not stable. With the
#      pre-#440 comparator (count alone, no identity tie-break) two
#      equal-count colliding pairs could therefore come out in either order.
#      The fixture below contains exactly such a pair: LWLock:ShmemIndex ->
#      Lock:relation and LWLock:id=56 -> LWLock:id=40 both hash to slot 0
#      (((from*0x45d9f3b) ^ (to*0x9e3779b9)) & 4095 == 0) and both have
#      count 7. Trace A emits the first pair first, trace B the second, and
#      the served payload must be identical. A fixture WITHOUT a hash
#      collision cannot see this: with 6 pairs in a 4096-slot table the slot
#      order is otherwise fixed whatever the record order, which is why the
#      collision is constructed rather than hoped for.
#
#   2. The node array, which is where the disclosure's input lives:
#      findOmittedDominantNode (web/static/lib/builders/transitions.js) reads
#      ONLY data.nodes, while all three headline totals come from data.links.
#      So a nodes array that lost one entry reproduces the reported symptom
#      EXACTLY — clause gone, every total unchanged. The gate therefore
#      asserts the relation node's presence and its exact dwell, not just the
#      totals, and section 456.4 proves the gate can see that node go missing.
#
# Nothing here is timing-sensitive: synthetic trace, fixed timestamps, one
# frozen request, and every assertion is on exact integers or on a digest.

MS = 1_000_000
S = 1_000_000_000

# The colliding pair (see 1 above). Both ids are ordinary wait events.
LW_A = make_event_id(CLASS_LWLOCK, 0)
LW_B = make_event_id(CLASS_LWLOCK, 56)
LW_C = make_event_id(CLASS_LWLOCK, 40)

HOT_N = 200          # count of each CPU<->IO link: the frequency majority
LOCK_N = 3           # count of each CPU<->Lock:relation link: rare
LOCK_DWELL_MS = 5000  # but each Lock:relation wait is 5 s: dominant DWELL
TIE_N = 7            # count of BOTH colliding pairs: the tie


def _ev(pid, ts, dur, old, new, qid=100):
    return {"pid": pid, "ts": ts, "dur": dur, "old": old, "new": new,
            "qid": qid}


def _group_hot():
    """HOT_N round trips CPU -> IO:DataFileRead -> CPU, 1 ms each."""
    out = []
    for _ in range(HOT_N):
        out.append(("cpu->io", 1 * MS, CPU, IO_DATA_FILE_READ))
        out.append(("io->cpu", 1 * MS, IO_DATA_FILE_READ, CPU))
    return out


def _group_lock():
    """LOCK_N rare round trips whose Lock:relation dwell is 5 s each."""
    out = []
    for _ in range(LOCK_N):
        out.append(("cpu->lock", 1 * MS, CPU, LOCK_RELATION))
        out.append(("lock->cpu", LOCK_DWELL_MS * MS, LOCK_RELATION, CPU))
    return out


def _group_tie_first():
    return [("tie1", 1000, LW_A, LOCK_RELATION)] * TIE_N


def _group_tie_second():
    return [("tie2", 1000, LW_B, LW_C)] * TIE_N


def _scenario(groups):
    """Lay the groups out in the given order, timestamps assigned in that
    order so the loader's own sort preserves it. The event MULTISET is
    identical for every ordering — only arrival order changes."""
    events = []
    ts = BASE_TS
    for g in groups:
        for (_label, dur, old, new) in g:
            ts += dur
            events.append(_ev(1000, ts, dur, old, new))
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u",
                      "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT dfg()"}],
        "events": events,
    }


def _canon(resp):
    """Everything the Transitions tab's disclosure can depend on."""
    return {
        "total": resp.get("total"),
        "link_count": resp.get("link_count"),
        "total_link_count": resp.get("total_link_count"),
        "truncated": resp.get("truncated"),
        "fidelity": resp.get("fidelity"),
        "nodes": resp.get("nodes"),
        "links": resp.get("links"),
    }


def _digest(canon):
    return hashlib.sha256(
        json.dumps(canon, sort_keys=True).encode()).hexdigest()[:16]


def _node(canon, name):
    for n in (canon.get("nodes") or []):
        if n.get("name") == name:
            return n
    return None


def _one_restart(trace_dir, env=None):
    """ONE fresh pgwt-server process, ONE frozen transitions request."""
    with ServerHarness(trace_dir, env=env) as srv:
        info = srv.query("info")
        # Frozen bounds: the capture's own extent, widened by a fixed amount
        # so every process asks the identical question.
        frm = info["from_ns"] - 30 * S
        to = info["to_ns"] + 30 * S
        return _canon(srv.query("transitions", from_=frm, to_=to,
                                buckets=200))


def _one_restart_provenance(trace_dir):
    with ServerHarness(trace_dir,
                       env={"PGWT_TRANSITIONS_PROVENANCE": "1"}) as srv:
        info = srv.query("info")
        resp = srv.query("transitions", from_=info["from_ns"] - 30 * S,
                         to_=info["to_ns"] + 30 * S, buckets=200)
        return resp.get("merged_blocks"), resp.get("decoded_blocks")


RESTARTS = 10


def check_restart_determinism(t):
    print("--- #456: transitions determinism across restarts ---")
    order_a = [_group_hot(), _group_lock(), _group_tie_first(),
               _group_tie_second()]
    # Trace B: the colliding pair arrives in the OPPOSITE order, and the
    # majority/rare groups are swapped too.
    order_b = [_group_tie_second(), _group_tie_first(), _group_lock(),
               _group_hot()]
    dir_a = generate_traces(_scenario(order_a))
    dir_b = generate_traces(_scenario(order_b))
    try:
        runs = [_one_restart(dir_a) for _ in range(RESTARTS)]
        t.check_eq(len(runs), RESTARTS,
                   f"collected a payload from all {RESTARTS} fresh server "
                   f"processes (a failed start must fail this test, never "
                   f"skip it)")

        # 456.1 NONEMPTY. Ten identical EMPTY responses would otherwise
        # satisfy every determinism assertion below without checking
        # anything, so the expected population is asserted first and
        # exactly.
        first = runs[0]
        expect_total = 2 * HOT_N + 2 * LOCK_N + 2 * TIE_N
        t.check_eq(first.get("total"), expect_total,
                   f"456.1 nonempty: total transitions = {expect_total}")
        t.check_eq(len(first.get("links") or []), 6,
                   "456.1 nonempty: 6 distinct transition links served")
        t.check(len(first.get("nodes") or []) >= 4,
                f"456.1 nonempty: node array populated "
                f"({len(first.get('nodes') or [])} nodes)")

        # 456.2 THE RELATION NODE: present, with its EXACT dwell, and in the
        # expected VISIBILITY state (omitted, because every link incident on
        # it is below the 5% frequency cutoff the view defaults to).
        lock = _node(first, "Lock:relation")
        t.check(lock is not None, "456.2 Lock:relation node is present")
        if lock is not None:
            want = float(LOCK_N * LOCK_DWELL_MS)
            t.check(abs(lock["total_ms"] - want) < 1e-6,
                    f"456.2 Lock:relation dwell = {want} ms exactly "
                    f"(got {lock['total_ms']})")
        incident = [l for l in (first.get("links") or [])
                    if l["source"] == "Lock:relation"
                    or l["target"] == "Lock:relation"]
        max_edge = max(l["value"] for l in (first.get("links") or []))
        cutoff = max_edge * 5 / 100.0
        t.check(len(incident) == 3,
                f"456.2 three links are incident on Lock:relation "
                f"({[l['value'] for l in incident]})")
        t.check(incident and all(l["value"] < cutoff for l in incident),
                f"456.2 expected visibility status = OMITTED: every "
                f"incident count {[l['value'] for l in incident]} is below "
                f"the 5% cutoff {cutoff} of the busiest link {max_edge}")

        # 456.3 IDENTICAL ACROSS RESTARTS, and across the permutation.
        digests = sorted({_digest(r) for r in runs})
        t.check_eq(len(digests), 1,
                   f"456.3 all {RESTARTS} restarts served ONE payload "
                   f"(digests: {digests})")
        perm = [_one_restart(dir_b) for _ in range(2)]
        t.check_eq(len({_digest(r) for r in perm}), 1,
                   "456.3 the permuted trace is itself stable across "
                   "restarts")
        t.check_eq(_digest(perm[0]), _digest(first),
                   "456.3 permuting the record order — including the two "
                   "hash-colliding equal-count pairs — changes nothing")

        # 456.4 THE BYPASS SUITE: every way this gate could agree without
        # checking anything.
        import copy
        mutated = copy.deepcopy(first)
        mutated["nodes"] = [n for n in mutated["nodes"]
                            if n.get("name") != "Lock:relation"]
        t.check(_digest(mutated) != _digest(first),
                "456.4 deleting the Lock:relation NODE while keeping total, "
                "link_count and total_link_count identical is DETECTED — "
                "that deletion is the reported symptom exactly")
        t.check_eq(_node(mutated, "Lock:relation"), None,
                   "456.4 ...and the node lookup this gate uses reports it "
                   "ABSENT rather than defaulting to zero")

        swapped = copy.deepcopy(first)
        tie = [i for i, l in enumerate(swapped["links"])
               if l["value"] == TIE_N]
        t.check(len(tie) == 2,
                f"456.4 the fixture really does contain two equal-count "
                f"({TIE_N}) links for the tie-break to order")
        if len(tie) == 2:
            i, j = tie
            swapped["links"][i], swapped["links"][j] = (
                swapped["links"][j], swapped["links"][i])
            t.check(_digest(swapped) != _digest(first),
                    "456.4 reversing the order of the two equal-count links "
                    "is DETECTED (the pre-#440 count-only comparator could "
                    "emit either order)")

        empty = copy.deepcopy(first)
        empty["nodes"] = []
        empty["links"] = []
        t.check(_digest(empty) != _digest(first),
                "456.4 an EMPTY response is detected as different — ten "
                "agreeing empty payloads cannot pass this gate")

        # 456.5 PROVENANCE: which path actually served these responses.
        prov = [_one_restart_provenance(dir_a) for _ in range(2)]
        t.check(all(p[0] is not None and p[1] is not None for p in prov),
                f"456.5 provenance is reported when asked "
                f"(PGWT_TRANSITIONS_PROVENANCE=1): {prov}")
        t.check_eq(len(set(prov)), 1,
                   "456.5 the same path served every run")
        t.check_eq(prov[0], (0, 0),
                   "456.5 that path is RAW (merged_blocks=0, "
                   "decoded_blocks=0). The aggregate path cannot be "
                   "exercised through `transitions` today: "
                   "bagg_window_eligible (src/server.c) opens with an "
                   "unconditional `return 0` pending cross-block command "
                   "tagging, which tests/test_transitions_loader.c also "
                   "asserts. If this pair ever becomes non-zero the "
                   "aggregate path has been enabled and this gate must be "
                   "extended to cover both — it is not a stale assertion, "
                   "it is the signal to extend")
    finally:
        cleanup_traces(dir_a)
        cleanup_traces(dir_b)


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

check_restart_determinism(t)

sys.exit(0 if t.summary() else 1)
