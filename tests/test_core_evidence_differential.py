#!/usr/bin/env python3
"""Deterministic U1/U2 logical replay versus the frozen independent evaluator.

Builds only portable C in a temporary directory. CFLAGS selects sanitizers.
The contract mode runs the C test against both frozen answer files.
"""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contracts/evidence-v1"
MAX = (1 << 64) - 1
SEED = 0x868001


def environment():
    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    return env


def execute(command, timeout=45):
    return subprocess.run([str(v) for v in command], text=True, capture_output=True,
                          env=environment(), timeout=timeout)


def sanitizer_error(text):
    return re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", text)


def build(source_root, binary):
    source_root = Path(source_root).resolve()
    command = shlex.split(os.environ.get("CC", "cc"))
    command += ["-std=c11", "-g", "-O2", "-Wall", "-Wextra", "-Werror=implicit-function-declaration"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    command += ["-I" + str(source_root / "src"), "-I" + str(source_root / "tests")]
    command += [str(source_root / name) for name in (
        "tests/test_core_evidence_contract.c", "tests/evidence_json.c",
        "src/core/evidence_replay.c", "src/core/wait_metrics.c", "src/core/contract_json.c")]
    command += ["-o", str(binary)]
    result = execute(command)
    if result.returncode:
        raise RuntimeError("build failed:\n" + result.stdout + result.stderr)


def fixture(prefix):
    paths = list((CONTRACT / "scenarios").glob(prefix + "_*.json"))
    if len(paths) != 1:
        raise RuntimeError(f"missing/duplicate fixture {prefix}")
    return json.loads(paths[0].read_text())


def named_cases():
    cases = [fixture(v) for v in ("01", "02", "03", "04", "06", "07", "08", "16", "17", "24", "25", "27", "28", "29", "30")]
    ordinary = fixture("01")
    records = ordinary["files"][0]["records"]
    c = copy.deepcopy(ordinary)
    c["id"] = "transitions_before_attach"
    r = c["files"][0]["records"]
    r.insert(-1, r.pop(1))
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "frontier_exact_transition"
    c["files"][0]["records"][-1]["t"] = 9
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "no_frontier"
    c["files"][0]["records"] = copy.deepcopy(records[:-1])
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "life_after_attach_and_transitions"
    r = c["files"][0]["records"]
    r.insert(-1, r.pop(0))
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "missing_life"
    c["files"][0]["records"].pop(0)
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "transitions_through_reread"
    # A prior A->null close at the reread anchor is already reconciled and
    # must not enter either metric; duration is no invented entry proof.
    c["files"][0]["records"].insert(1, {
        "kind": "TRANS", "t": 1, "pid": 101, "old": "A", "new": None, "duration": 1})
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "empty_certified_point"
    c["files"][0]["records"] = copy.deepcopy(records[:2]) + [{
        "kind": "FRONTIER", "t": 1, "counters": [0] * 15}]
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "qualification_absent"
    del c["files"][0]["qualified"]
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "present_flag_absent"
    del c["files"][0]["present"]
    cases.append(c)
    c = fixture("28")
    c["id"] = "legacy_qualified_flag"
    c["files"][0]["qualified"] = True
    cases.append(c)
    c = copy.deepcopy(ordinary)
    c["id"] = "transition_after_frontier"
    r = c["files"][0]["records"]
    r.append(r.pop(5))  # Lock closes only after the certifying physical prefix.
    c["query"]["event"] = "Lock"
    cases.append(c)
    for name, field, value in (("inconsistent_duration", "duration", 5),
                               ("inconsistent_old_state", "old", "Lock")):
        c = copy.deepcopy(ordinary)
        c["id"] = name
        c["files"][0]["records"][3][field] = value
        cases.append(c)
    return cases


def guard_cases():
    """Reviewer witnesses for individual semantic guards, derived from fixtures."""
    cases = []
    def add(prefix, name, edit):
        c = fixture(prefix)
        c["id"] = "guard_" + name
        edit(c)
        cases.append(c)
    def records(c): return c["files"][0]["records"]
    add("01", "attach_transition", lambda c: records(c)[1].update(method="transition"))
    add("01", "attach_seed_nonnull", lambda c: records(c)[1].update(seed="Lock"))
    add("01", "attach_state_nonnull", lambda c: records(c)[1].update(state="Lock"))
    def edge_at_exit(c):
        r = records(c)
        r[3]["new"] = None
        r.insert(4, {"kind": "TRANS", "t": 10, "pid": 101,
                     "old": None, "new": "$EXIT", "duration": 0})
    add("03", "ordinary_edge_then_terminal_at_exit", edge_at_exit)
    def same_event(c):
        r = records(c)
        r.insert(3, {"kind": "TRANS", "t": 5, "pid": 101,
                     "old": "PgSleep", "new": "PgSleep", "duration": 2})
        r[4]["duration"] = 2
    add("01", "same_event_transition", same_event)
    add("01", "duration_exceeds_timestamp", lambda c: records(c)[2].update(duration=5))
    add("01", "empty_old_state", lambda c: records(c)[2].update(old=""))
    def future_start(c):
        c["target"]["fork"] = 1
        records(c)[0].update(fork=1, start=1)  # observation remains at t=0
        records(c)[1]["fork"] = 1
    add("01", "life_start_after_observation", future_start)
    add("01", "attach_nonzero_counter", lambda c: records(c)[1]["counters"].__setitem__(11, 1))
    def earlier_duplicate(c):
        r = records(c)
        r.insert(1, dict(r[0], t=5))  # first physical discovery remains t=10
        r.insert(2, {"kind": "TRANS", "t": 7, "pid": 101,
                     "old": None, "new": "Lock", "duration": 0})
    add("06", "first_physical_discovery", earlier_duplicate)
    return cases


def lifecycle_cases():
    cases = []
    def add(prefix, name, edit):
        c = fixture(prefix)
        c["id"] = name
        edit(c)
        cases.append(c)
    def records(c): return c["files"][-1]["records"]
    add("02", "reuse_old_target", lambda c: c["target"].update(fork=0))
    add("02", "reuse_missing_old_exit", lambda c: records(c).pop(-2))
    add("02", "reuse_missing_new_life", lambda c: records(c).pop(4))
    add("02", "reuse_bad_old_run", lambda c: records(c)[3].update(duration=4))
    add("02", "reuse_bad_new_run", lambda c: records(c)[6].update(old="A"))
    add("02", "reuse_same_exit", lambda c: records(c)[4].update(exit=8, t=12, start=7, fork=7))
    def late(c):
        r = records(c)
        r.append(r.pop(-2))
    add("02", "reuse_life_after_frontier", late)
    add("03", "exit_life_after_frontier", late)
    add("03", "exit_without_terminal", lambda c: records(c).pop(3))
    def earlier(c):
        records(c).insert(3, {"kind": "FRONTIER", "t": 7, "counters": [0]*15})
        records(c).pop(4)
    add("03", "exit_missing_terminal_fallback", earlier)
    def late_exit(c):
        earlier(c)
        records(c).insert(3, {"kind": "FRONTIER", "t": 9, "counters": [0]*15})
        records(c).append(records(c).pop(-2))
    add("03", "exit_late_life_fallback", late_exit)
    add("03", "exit_at_frontier", lambda c: records(c)[-1].update(t=10))
    add("03", "exit_after_frontier", lambda c: records(c)[-1].update(t=9))
    add("03", "exit_unmatched_sentinel", lambda c: records(c)[4].update(exit=11, t=11))
    add("03", "exit_nonwaiting", lambda c: [records(c).pop(2), records(c)[2].update(old=None)])
    add("03", "exit_duplicate_terminal", lambda c: records(c).insert(4, copy.deepcopy(records(c)[3])))
    add("03", "ordinary_edge_at_exit", lambda c: records(c)[3].update(new=None))
    add("03", "exit_attach_at_exit", lambda c: records(c)[1].update(t=10))
    add("01", "life_identical_repeat", lambda c: records(c).append(copy.deepcopy(records(c)[0])))
    add("03", "life_exit_repeat", lambda c: records(c).append(copy.deepcopy(records(c)[4])))
    add("03", "life_exit_retracted", lambda c: records(c).append(copy.deepcopy(records(c)[0])))
    add("03", "life_exit_changed", lambda c: records(c).append(dict(records(c)[4], t=11, exit=11)))
    add("06", "life_lower_bound_changed", lambda c: records(c).append(dict(records(c)[0], start=1)))
    add("06", "provisional_nonzero_bound", lambda c: records(c)[0].update(start=5))
    add("06", "provisional_duplicate_discovery", lambda c: records(c).insert(2, dict(records(c)[0], t=19)))
    add("06", "provisional_edge_before_discovery", lambda c: records(c).insert(1, {
        "kind":"TRANS", "t":9, "pid":101, "old":None, "new":"A", "duration":0}))
    add("06", "provisional_edge_at_discovery", lambda c: records(c).insert(1, {
        "kind":"TRANS", "t":10, "pid":101, "old":None, "new":"A", "duration":0}))
    add("06", "provisional_life_after_attach", lambda c: records(c).insert(2, records(c).pop(0)))
    def restart_same_key(c):
        for r in records(c):
            if r["kind"] in ("LIFE", "ATTACH_OK"): r["fork"] = 0
            if r["kind"] == "LIFE": r["start_known"] = True
        c["target"]["fork"] = 0
    add("24", "restart_same_generation_key", restart_same_key)
    add("24", "restart_old_target", lambda c: c["target"].update(capture="A", fork=0))
    add("24", "restart_reversed_files", lambda c: c["files"].reverse())
    add("24", "restart_no_fresh_attach", lambda c: records(c).pop(1))
    add("24", "restart_no_fresh_life", lambda c: records(c).pop(0))
    add("24", "restart_frontier_resets", lambda c: c["files"][0]["records"][-1].update(t=100))
    add("24", "restart_bad_old_run", lambda c: c["files"][0]["records"][2].update(old="A"))
    add("01", "legacy_life_shaped_records", lambda c: c["files"][0].update(version=4))
    def mixed(c):
        r = records(c)
        for v in (r[0], r[1], r[-2]):
            v["fork"] = None
            if v["kind"] == "LIFE": v.update(start_known=False, t=max(1, v["t"]))
        c["target"]["fork"] = None
    add("02", "reuse_provisional_then_observed", mixed)
    add("01", "exit_without_life_exit_at_zero", lambda c: records(c).insert(1, {
        "kind":"TRANS", "t":0, "pid":101, "old":None, "new":"$EXIT", "duration":0}))
    def invalid_other_life(c):
        mixed(c)
        r = records(c)
        r.insert(-1, dict(r[0], start=1, t=1))
        c["target"]["fork"] = 12
    add("02", "reuse_invalid_old_life", invalid_other_life)
    def invalid_provisional_attach(c):
        r = records(c)
        r[0]["start"] = 2
        r.insert(1, dict(r[0], start=3))
        r[2]["t"] = 1  # invalid generation must be skipped before attach lifetime validation
    add("06", "invalid_life_skips_attach", invalid_provisional_attach)
    def zero_exit_retracted(c):
        r = records(c)
        r[:] = [dict(r[0], exit=0), copy.deepcopy(r[0]),
                {"kind":"FRONTIER", "t":1, "counters":[0]*15}]
    add("01", "life_zero_exit_retracted", zero_exit_retracted)
    def distinct_restart_life(c):
        c["target"].update(capture="A", fork=0)
        records(c)[0]["start"] = 5
    add("24", "restart_old_target_distinct_life", distinct_restart_life)
    add("03", "duplicate_nonwaiting_exit", lambda c: records(c).insert(4, {
        "kind":"TRANS", "t":10, "pid":101, "old":None, "new":"$EXIT", "duration":0}))
    def reversed_same_key(c):
        restart_same_key(c)
        c["files"].reverse()
    add("24", "restart_same_key_reversed", reversed_same_key)
    add("02", "observed_life_delayed_observation", lambda c: records(c)[4].update(t=17))
    def restart_life_exit(c):
        restart_same_key(c)
        records(c)[0].update(t=24, exit=24)
        c["target"].update(capture="A", fork=0)
    add("24", "restart_life_exit_isolation", restart_life_exit)
    def other_terminal(c):
        c["target"]["fork"] = 0
        r = records(c)
        r[4].update(t=18, exit=18)
        r.insert(-1, {"kind":"TRANS", "t":18, "pid":101, "old":"Lock", "new":"$EXIT", "duration":3})
    add("02", "reuse_other_generation_terminal", other_terminal)
    return cases


def generated_lifecycles():
    rng = random.Random(SEED + 2)
    cases = lifecycle_cases()
    for i in range(1800):
        c = fixture(rng.choice(["02", "03", "04", "06", "07", "24"]))
        c["id"] = f"lifecycle_{i:04d}"
        scale = rng.randrange(1, 8)
        offset = rng.choice([0, 100, (1 << 53)+7, (1 << 63), MAX-1000])
        for f in c["files"]:
            for r in f["records"]:
                for k in ("t", "fork", "start", "exit"):
                    if k in r and r[k] is not None: r[k] = offset + scale*r[k]
                if "duration" in r: r["duration"] *= scale
        if c["target"]["fork"] is not None:
            c["target"]["fork"] = offset + scale*c["target"]["fork"]
        if i % 6 == 0 and len(c["files"][0]["records"]) == 9:
            c["target"]["fork"] = offset  # old generation after PID reuse
        r = c["files"][-1]["records"]
        fault = i % 19
        if fault == 0:
            r.append(r.pop(0))  # LIFE physically after certification
        elif fault == 1:
            deps = r[:-1]
            rng.shuffle(deps)
            r[:] = deps + r[-1:]
        elif fault == 2:
            r[:] = [v for v in r if v["kind"] != "LIFE" or v["exit"] is None]
        elif fault == 3:
            r[:] = [v for v in r if v.get("new") != "$EXIT"]
        elif fault == 4:
            r.append(copy.deepcopy(next(v for v in r if v["kind"] == "LIFE")))
        elif fault == 5:
            next(v for v in r if v["kind"] == "TRANS")["old"] = "bad"
        elif fault == 6:
            c["files"][-1]["qualified"] = False
        elif fault == 7:
            r.insert(-1, {"kind":"FRONTIER", "t":offset+scale*7, "counters":[0]*15})
        elif fault == 8:
            tr = next(v for v in r if v["kind"] == "TRANS")
            tr["t"] = offset
            tr["duration"] = 0
        left, right = sorted(rng.choices(list(range(26)), k=2))
        if i % 5 == 0: left, right = 10, 12  # query starts at exit
        c["query"] = {"event":rng.choice(["Lock", "PgSleep", "absent"]),
                      "from":offset+scale*left, "to":offset+scale*right}
        cases.append(c)
    return cases


def generate():
    rng = random.Random(SEED)
    cases = named_cases() + guard_cases() + generated_lifecycles()
    names = ["PgSleep", "Lock", "A", "other", 'quote"slash\\', "\u2603", "A\0B", "\U0001f600"]
    for i in range(2400):
        offset = rng.choice([0, 0, 100, (1 << 53) + 7, (1 << 63), MAX - 100])
        fork = offset + rng.randrange(3)
        anchor = fork + rng.randrange(4)
        life = {"kind": "LIFE", "t": fork, "pid": 101, "fork": fork,
                "start": fork, "start_known": True, "exit": None}
        attach = {"kind": "ATTACH_OK", "t": anchor, "pid": 101, "fork": fork,
                  "method": "reread", "seed": None, "state": None, "counters": [0] * 15}
        transitions = []
        current, entry, t = None, anchor, anchor + 1
        for _ in range(rng.randrange(18)):
            t += rng.randrange(4)  # zero deltas deliberately preserve ties
            new = rng.choice([n for n in [None] + names if n != current])
            transitions.append({"kind": "TRANS", "t": t, "pid": 101,
                                "old": current, "new": new, "duration": t - entry})
            current, entry = new, t
        # Reorder timestamp groups while retaining physical order within ties.
        # Start with dependencies before FRONTIER, then vary physical prefixes.
        groups = []
        for tr in transitions:
            if not groups or groups[-1][0]["t"] != tr["t"]:
                groups.append([])
            groups[-1].append(tr)
        rng.shuffle(groups)
        before = [tr for group in groups for tr in group]
        deps = [life, attach] + before
        if i % 2:
            deps = before + [attach, life]
        if i % 17 == 0:
            deps = [r for r in deps if r["kind"] != "ATTACH_OK"]
        cutoff = rng.randint(anchor, max(anchor, t + 3))
        points = sorted({cutoff, rng.randint(fork, cutoff), rng.randint(fork, cutoff)})
        frontiers = [{"kind": "FRONTIER", "t": p, "counters": [0] * 15} for p in points]
        records = deps + frontiers
        if i % 11 == 0:
            # The physical tail does not move the last certified endpoint.
            records.append({"kind": "TRANS", "t": t + 5, "pid": 101,
                            "old": current, "new": None if current else "PgSleep",
                            "duration": t + 5 - entry})
        if i % 19 == 0:
            records = deps  # no FRONTIER at all
        if transitions:
            fault = i % 5
            tr = rng.choice(transitions)
            if fault == 0 and frontiers[0] in records:
                # Include timestamps inside a certificate but outside its prefix.
                records.remove(tr)
                records.append(tr)
            elif fault == 1:
                tr["pid"] = 102
            elif fault == 2:
                # Keep the duration individually valid, but inconsistent with entry.
                tr["duration"] = (tr["duration"] + 1) % (tr["t"] + 1)
            elif fault == 3:
                tr["old"] = next(n for n in names if n not in (tr["old"], tr["new"]))
        qualified, version, target_fork = i % 7 != 0, 5, fork
        if i % 13 == 0:
            version, qualified, target_fork = 2 + (i % 3), False, None
            records = transitions
        left = offset + rng.randrange(65)
        right = offset + rng.randrange(65)
        query = {"event": rng.choice(names + ["absent"]), "from": min(left, right), "to": max(left, right)}
        if i % 23 == 0:
            query["from"], query["to"] = query["to"], query["from"]
        if i % 37 == 0:
            del query[rng.choice(["event", "from", "to"])]
        if i % 41 == 0:
            query["from"] = True
        if i % 43 == 0:
            query["event"] = ""
        if i % 47 == 0:
            query["to"] = MAX
        cases.append({"id": f"seed_{SEED:x}_{i:04d}", "files": [{
            "version": version, "boot": "boot-1", "capture": "A", "file_no": 0,
            "previous": None, "qualified": qualified, "loss_baseline": [0] * 15,
            "present": True, "records": records}],
            "target": {"capture": "A", "pid": 101, "fork": target_fork}, "query": query})
    return cases


def refusal_cases():
    """Whole-input refusals and metric refusals, including uncertified suffixes."""
    cases = []
    def add(name, edit):
        c = fixture("01")
        c["id"] = "refuse_" + name
        edit(c)
        cases.append(c)
    for kind in ("END", "VOID", "SAMPLE", "UNKNOWN"):
        for mode in ("qualified", "unqualified", "legacy"):
            def edit(c, kind=kind, mode=mode):
                f = c["files"][0]
                if mode == "unqualified": f["qualified"] = False
                if mode == "legacy": f["version"] = 4
                f["records"].append({"kind": kind, "t": 30})
            add(kind + "_" + mode, edit)
    for i in range(15):
        add(f"counter_{i}", lambda c, i=i: c["files"][0]["records"][-1]["counters"].__setitem__(i, 1))
    add("baseline", lambda c: c["files"][0]["loss_baseline"].__setitem__(14, 1))
    add("unreadable_baseline", lambda c: c["files"][0].__setitem__("loss_baseline", None))
    add("short_vector", lambda c: c["files"][0]["records"][-1].__setitem__("counters", [0] * 14))
    add("null_vector", lambda c: c["files"][0]["records"][-1].__setitem__("counters", None))
    add("multiple_files", lambda c: c["files"].append(copy.deepcopy(c["files"][0])))
    add("no_files", lambda c: c.__setitem__("files", []))
    add("rotation", lambda c: c["files"][0].__setitem__("file_no", 1))
    add("previous", lambda c: c["files"][0].__setitem__("previous", 0))
    add("missing_file", lambda c: c["files"][0].__setitem__("present", False))
    add("second_attach", lambda c: c["files"][0]["records"].append(copy.deepcopy(c["files"][0]["records"][1])))
    add("other_generation", lambda c: c["files"][0]["records"].append({
        "kind": "LIFE", "t": 20, "pid": 102, "fork": 20, "start": 20, "start_known": True, "exit": None}))
    add("future_schema", lambda c: c["files"][0].__setitem__("version", 6))
    add("record_extension", lambda c: c["files"][0]["records"][-1].__setitem__("run", 2))
    add("invalid_time_token", lambda c: c["files"][0]["records"][-1].__setitem__("t", 15.0))
    add("other_pid_transition", lambda c: c["files"][0]["records"].insert(6, {
        "kind": "TRANS", "t": 13, "pid": 102, "old": None,
        "new": "PgSleep", "duration": 0}))
    def transition_before_fork(c):
        c["target"]["fork"] = 1
        r = c["files"][0]["records"]
        r[0].update(t=1, fork=1, start=1)
        r[1]["fork"] = 1
        r.insert(2, {"kind": "TRANS", "t": 0, "pid": 101, "old": None,
                     "new": "Lock", "duration": 0})
    before_fork = fixture("01")
    before_fork["id"] = "transition_before_fork"
    transition_before_fork(before_fork)
    def attach_before_fork(c):
        c["target"]["fork"] = 2
        c["files"][0]["records"][0].update(t=2, fork=2, start=2)
        c["files"][0]["records"][1]["fork"] = 2
    add("attach_before_fork", attach_before_fork)
    for t in (15, 14):
        add(f"nonadvancing_frontier_{t}", lambda c, t=t: c["files"][0]["records"].append({
            "kind": "FRONTIER", "t": t, "counters": [0] * 15}))
    restart_duplicate = fixture("24")
    restart_duplicate["id"] = "refuse_restart_duplicate_non_target"
    restart_duplicate["files"].append(copy.deepcopy(restart_duplicate["files"][0]))
    cases.append(restart_duplicate)
    add("target_capture_missing", lambda c: c["target"].update(capture="missing"))
    add("restart_other_boot", lambda c: [c["files"].append(copy.deepcopy(c["files"][0])),
        c["files"][1].update(capture="B", boot="boot-2")])
    add("life_observed_no_fork", lambda c: c["files"][0]["records"][0].update(fork=None))
    add("life_observed_fork_mismatch", lambda c: c["files"][0]["records"][0].update(start=1, t=1))
    add("life_provisional_with_fork", lambda c: c["files"][0]["records"][0].update(start_known=False))
    add("life_exit_before_start", lambda c: c["files"][0]["records"][0].update(start=2, fork=2, t=3, exit=1))
    add("life_exit_after_observation", lambda c: c["files"][0]["records"][0].update(exit=20))
    add("transition_empty_new", lambda c: c["files"][0]["records"][-2].update(new=""))
    # A frontier before the attach withholds coverage, rather than invalidating
    # the entire input. Compare these unavailable metrics with the reference.
    unavailable = fixture("01")
    unavailable["id"] = "refuse_last_frontier_before_attach"
    unavailable["files"][0]["records"][1]["t"] = 20
    return cases, [unavailable, before_fork]


def reference_module():
    spec = importlib.util.spec_from_file_location("evidence_v1_reference", CONTRACT / "reference/replay.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolves its declaring module
    spec.loader.exec_module(module)
    return module


def reference_answer(reference, case):
    try:
        output = reference.section_7_scenario(case)
    except reference.Ambiguity:
        # No independent numerical answer exists. Require a whole-input refusal,
        # never a result object (even one whose metrics claim unavailability).
        return {"error": "unsupported"}
    if len(output) != 3:
        raise RuntimeError(f"reference did not produce all objects for {case['id']}")
    target = case["target"]
    # The reference handles multiple PIDs; U2 explicitly supports only one.
    if any(r["kind"] == "TRANS" and r["pid"] != target["pid"]
           for f in case["files"] for r in f["records"]):
        return {"error": "unsupported"}
    cores = [p["core"] for p in output[0]["projection"]
             if all(p["generation"][key] == value for key, value in target.items())]
    if len(cores) != 1:
        raise RuntimeError(f"reference target ambiguous for {case['id']}")
    return {"projection": cores[0], "wait_time": output[1], "completed_waits": output[2]}


def first_difference(actual, expected, path=""):
    if type(actual) is not type(expected):
        return path or "shape"
    if isinstance(expected, dict):
        if actual.keys() != expected.keys(): return (path + ".keys").lstrip(".")
        for key, value in expected.items():
            diff = first_difference(actual[key], value, (path + "." + key).lstrip("."))
            if diff: return diff
    elif isinstance(expected, list):
        if len(actual) != len(expected): return path + ".length"
        for i, (a, b) in enumerate(zip(actual, expected)):
            diff = first_difference(a, b, f"{path}[{i}]")
            if diff: return diff
    elif actual != expected:
        return path
    return None


def differential(binary, work, selected=None):
    reference = reference_module()
    cases = generate()
    refusals, metric_refusals = refusal_cases()
    cases.extend(metric_refusals)
    if selected:
        cases = [c for c in cases if c["id"] == selected]
        refusals = [c for c in refusals if c["id"] == selected]
        if len(cases) + len(refusals) != 1:
            raise RuntimeError("unknown/duplicate selected differential case")
    expected = []
    scope_refusals = {"guard_attach_transition", "guard_attach_nonzero_counter"}
    for case in cases:
        answer = reference_answer(reference, case)
        if case["id"] in scope_refusals:
            # The reference supports more attachment shapes than U2's public
            # API (nonwaiting rereads and zero counters only). Prove the
            # reference withholds coverage and both values before requiring
            # the core's documented whole-input refusal for these shapes.
            if (answer.get("projection", {}).get("coverage") != [] or
                    any(answer.get(metric, {}).get("completeness") != "unavailable"
                        for metric in ("wait_time", "completed_waits"))):
                raise RuntimeError(f"reference did not refuse coverage for {case['id']}")
            answer = {"error": "unsupported"}
        expected.append(answer)
    expected += [{"error": "unsupported"} for _ in refusals]
    all_cases = cases + refusals
    paths = []
    for i, case in enumerate(all_cases):
        path = work / f"input-{i}.json"
        path.write_text(json.dumps(case, ensure_ascii=True))
        paths.append(path)
    answers = []
    for start in range(0, len(paths), 250):
        result = execute([binary, "--json"] + paths[start:start + 250])
        if result.returncode or sanitizer_error(result.stdout + result.stderr):
            raise RuntimeError(f"driver failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
        answers.extend(json.loads(line) for line in result.stdout.splitlines())
    if len(answers) != len(all_cases):
        raise RuntimeError(f"driver emitted {len(answers)} answers for {len(all_cases)} inputs")
    mismatches = 0
    for case, actual, want in zip(all_cases, answers, expected):
        field = first_difference(actual, want)
        if field:
            mismatches += 1
            print(f"evidence differential {case['id']} MISMATCH {field} "
                  f"actual={json.dumps(actual, ensure_ascii=True, separators=(',', ':'))} "
                  f"expected={json.dumps(want, ensure_ascii=True, separators=(',', ':'))}")
        elif case["id"].startswith("guard_"):
            outcome = "unsupported" if "error" in want else want["wait_time"]["completeness"]
            reference_outcome = "unavailable" if case["id"] in scope_refusals else outcome
            print(f"evidence differential {case['id']} PASS "
                  f"reference={reference_outcome} core={outcome}")
    print(f"evidence differential: {len(cases)} reference cases, {len(refusals)} unsupported cases, "
          f"{mismatches} differences, seed=0x{SEED:x}")
    return 1 if mismatches else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--contract-only", action="store_true")
    parser.add_argument("--case", help="one named distinguishing case, for mutation reachability")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pgwt-evidence-") as temp:
        work = Path(temp)
        binary = work / "evidence-test"
        build(args.source_root, binary)
        if args.contract_only:
            result = execute([binary, CONTRACT])
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if sanitizer_error(result.stdout + result.stderr): return 2
            return result.returncode
        return differential(binary, work, args.case)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"evidence differential: harness error: {error}", file=sys.stderr)
        sys.exit(2)
