#!/usr/bin/env python3
"""Deterministic U1 logical replay versus the frozen independent evaluator.

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
    cases = [fixture(v) for v in ("01", "08", "16", "17", "25", "27", "28", "29", "30")]
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
    return cases


def generate():
    rng = random.Random(SEED)
    cases = named_cases()
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
        # Every required dependency is still appended before its FRONTIER.
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
    """U1 must refuse the entire input, even when the suffix is uncertified."""
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
    return cases


def reference_module():
    spec = importlib.util.spec_from_file_location("evidence_v1_reference", CONTRACT / "reference/replay.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolves its declaring module
    spec.loader.exec_module(module)
    return module


def reference_answer(reference, case):
    try:
        output = reference.section_7_scenario(case)
    except reference.Ambiguity as error:
        raise RuntimeError(f"reference ambiguity for {case['id']}: {error}") from error
    if len(output) != 3:
        raise RuntimeError(f"reference did not produce all objects for {case['id']}")
    target = case["target"]
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
    cases, refusals = generate(), refusal_cases()
    if selected:
        cases = [c for c in cases if c["id"] == selected]
        refusals = [c for c in refusals if c["id"] == selected]
        if len(cases) + len(refusals) != 1:
            raise RuntimeError("unknown/duplicate selected differential case")
    expected = [reference_answer(reference, c) for c in cases]
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
