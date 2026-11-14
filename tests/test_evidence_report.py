#!/usr/bin/env python3
"""Replay reports against frozen REDELIVERY cells and the independent model.

Codec reconciliation is outside this unit: admitted descriptors and logical
records are taken directly from cases.json's independent expected columns.
Physical positions stay here, never in replay. Full replay_summaries are used
verbatim; for older sparse cells the independent bounded replay model supplies
additional comparisons, cross-checked against every explicitly frozen field.
All builds, scenario files and mutant copies live outside the repository.
"""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contracts/evidence-v1"
REF = CONTRACT / "codec/redelivery-ref"
FIELDS = ("frontier", "high_water", "baseline", "frontiers", "revoked", "lifecycle_uncertainty")


def execute(args):
    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, env=env, timeout=60)
    if any(s in result.stdout + result.stderr for s in ("AddressSanitizer", "UndefinedBehaviorSanitizer", "runtime error:")):
        raise RuntimeError("sanitizer failure:\n" + result.stdout + result.stderr)
    return result


def build(binary, source=None):
    cmd = shlex.split(os.environ.get("CC", "cc"))
    cmd += ["-std=c11", "-g", "-O2", "-Wall", "-Wextra", "-Werror"]
    cmd += shlex.split(os.environ.get("CFLAGS", ""))
    cmd += ["-I" + str(ROOT / "src"), "-I" + str(ROOT / "tests")]
    cmd += [str(ROOT / "tests/test_core_evidence_report.c"), str(ROOT / "tests/evidence_json.c"),
            str(source or ROOT / "src/core/evidence_replay.c"), str(ROOT / "src/core/wait_metrics.c"),
            str(ROOT / "src/core/contract_json.c"), "-I" + str(ROOT / "src/core"), "-o", str(binary)]
    result = execute(cmd)
    if result.returncode:
        raise RuntimeError("build failed:\n" + result.stdout + result.stderr)


def reference():
    spec = importlib.util.spec_from_file_location("independent_replay_effects", REF / "replay_effects.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def same(actual, expected, field):
    if actual != expected:
        raise AssertionError(f"{field}: expected {expected!r}, got {actual!r}")


def logical_case(db, case, oracle):
    """No codec code executes: the frozen admission and RECORD columns are input."""
    expected = case["expected"]
    records = [db["records"][alias] for alias in expected["record_aliases"]]
    headers = {}
    for name in sorted(set(case["input"])):
        h = db["deliveries"][name]["header"]
        if h["valid"] and expected["admitted"].get("".join(map(str, h["physical"]))):
            physical = tuple(h["physical"])
            if physical in headers:
                for key in ("baseline", "qualified", "previous"):
                    same(h[key], headers[physical][key], f"fixture descriptor {physical}.{key}")
            headers[physical] = h
    files, positions = [], []
    for physical, h in sorted(headers.items()):
        rs = [r for r in records if tuple(r["physical"][:3]) == physical]
        files.append(dict(version=5, boot=physical[0], capture=physical[1], file_no=physical[2],
                          previous=None if not physical[2] else physical[2]-1,
                          present=True, qualified=h["qualified"], loss_baseline=h["baseline"],
                          records=[r["value"] for r in rs]))
        positions.append([r["physical"][-2:] for r in rs])
    captures = sorted({f["capture"] for f in files})
    summaries = {}
    for cap in captures:
        descriptors = [dict(file=list(p), baseline=h["baseline"], qualified=h["qualified"])
                       for p, h in sorted(headers.items()) if p[1] == cap]
        summaries[cap] = oracle.replay_probe([r for r in records if r["physical"][1] == cap], descriptors=descriptors)
    frozen = expected["replay_summaries"]
    if frozen is not None:
        same(set(summaries), set(frozen), f"case {case['case']} frozen capture set")
        for cap, wanted in frozen.items():
            for field, value in wanted.items():
                same(summaries[cap][field], value, f"case {case['case']} independent model/frozen {cap}.{field}")
            # The complete frozen answer is authoritative for these cells.
            summaries[cap] = wanted
    sparse = expected["replay"]
    if sparse is not None:
        s = summaries["a"]
        for field, value in sparse.items():
            actual = s["stop"]["reason"] if field == "reason" else s[field]
            if field == "stop": actual = actual["position"]
            same(actual, value, f"case {case['case']} independent model/frozen {field}")
    return files, positions, summaries


def run_case(binary, work, db, case, oracle):
    number = case["case"]
    files, positions, summaries = logical_case(db, case, oracle)
    targets = set()
    for f in files:
        for r in f["records"]:
            if r["kind"] == "LIFE": targets.add((f["capture"], r["pid"], r["fork"]))
    for cap in summaries:
        if not any(t[0] == cap for t in targets): targets.add((cap, 1, 0))
    if not targets: targets.add(("a", 1, 0))
    paths, selected = [], []
    for i, (cap, pid, fork) in enumerate(sorted(targets)):
        scenario = dict(id=f"redelivery_{number}_{i}", files=files,
                        target=dict(capture=cap, pid=pid, fork=fork),
                        query=dict(event="wait", **{"from": 0, "to": 100}))
        path = work / f"case-{number}-{i}.json"
        path.write_text(json.dumps(scenario))
        paths.append(path)
        selected.append((cap, pid, fork))
    result = execute([binary, "--json", *paths])
    if result.returncode:
        raise RuntimeError(f"case {number} harness error:\n" + result.stdout + result.stderr)
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    same(len(rows), len(selected), f"case {number} output count")
    for row, (cap, pid, fork) in zip(rows, selected):
        same(row["status"], 0 if summaries else 1, f"case {number} API status")
        got = {r["capture"]: r for r in row["captures"]}
        same(len(got), len(row["captures"]), f"case {number} duplicate capture report")
        same(set(got), set(summaries), f"case {number} capture set")
        for capture, want in summaries.items():
            actual = got[capture]
            label = f"case {number} capture {capture}"
            for field in FIELDS: same(actual[field], want[field], label + "." + field)
            stop = actual["stop"]
            if stop is not None:
                index = stop["file_index"]
                if not (0 <= index < len(files)):
                    raise AssertionError(label + ".stop.file_index out of range")
                f = files[index]
                mapped = dict(file=[f["boot"], f["capture"], f["file_no"]],
                              reason=stop["reason"])
                if stop["descriptor"]:
                    same(stop["record_index"], None, label + ".stop.descriptor kind")
                    mapped["position"] = [0, None]
                else:
                    ri = stop["record_index"]
                    if ri is None or not (0 <= ri < len(positions[index])):
                        raise AssertionError(label + ".stop.record_index out of range")
                    mapped["position"] = positions[index][ri]
                for field in ("expected", "declared"):
                    if field in stop: mapped[field] = stop[field]
                stop = mapped
            same(stop, want["stop"], label + ".stop")
        if summaries:
            want = next((p for p in summaries[cap]["projections"] if p["pid"] == pid and p["fork"] == fork), None)
            projection = {k: want[k] for k in ("life", "coverage", "waits", "open")} if want else dict(life=None, coverage=[], waits=[], open=None)
            # No fixture here has multiple candidate runs for one generation.
            same(row["result"]["projection"], projection, f"case {number} target {cap}/{pid}/{fork} LIFE/coverage")
    stops = [s["stop"] for s in summaries.values() if s["stop"] is not None]
    return stops


MUTANTS = [
    ("descriptor_baseline", "if (!restated) {", "if (false && !restated) {", 37, ".stop"),
    ("first_fault", "r->stop = stop;", "if (!r->stop.stopped) r->stop = stop;", "two_faults", "FAIL final_cutoff"),
    ("descriptor_index", "(struct record_position){j, k - extra}", "(struct record_position){j, k}", "two_faults", "FAIL final_cutoff"),
    ("wrong_capture", "r->stop = stop;", "report->captures[0].stop = stop;", "two_faults", "FAIL capture_stop_owner"),
    ("zero_H", "report->high_water = frontier;", "report->high_water = 0;", 22, ".high_water"),
]


def mutants(work, db, oracle):
    text = (ROOT / "src/core/evidence_replay.c").read_text()
    for name, old, new, witness, field in MUTANTS:
        same(text.count(old), 1, name + " mutation site count")
        source = work / (name + ".c")
        source.write_text(text.replace(old, new, 1))
        binary = work / name
        build(binary, source)
        if witness == "two_faults":
            result = execute([binary, "--two-faults"])
            if result.returncode != 1 or "report two_faults FAIL" not in result.stdout or field not in result.stdout:
                raise RuntimeError(name + " did not reach/fail its named assertion:\n" + result.stdout + result.stderr)
        else:
            try:
                run_case(binary, work, db, db["cases"][witness-1], oracle)
            except AssertionError as error:
                if field not in str(error): raise RuntimeError(name + " failed wrong assertion: " + str(error)) from error
            else:
                raise RuntimeError(name + " survived")
        print(f"report mutant {name}: KILLED case={witness} assertion={field}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-mutants", action="store_true")
    args = parser.parse_args()
    db = json.loads((REF / "cases.json").read_text())
    same([c["case"] for c in db["cases"]], list(range(1, 51)), "frozen 50-case corpus")
    oracle = reference()
    with tempfile.TemporaryDirectory(prefix="pgwt-replay-report-") as temp:
        work = Path(temp)
        binary = work / "report-test"
        build(binary)
        native = execute([binary, CONTRACT])
        print(native.stdout, end="", flush=True)
        if native.returncode: raise RuntimeError("native/r3 test failed:\n" + native.stderr)
        for case in db["cases"]:
            stops = run_case(binary, work, db, case, oracle)
            for stop in stops:
                print(f"report case {case['case']}: STOP file={stop['file']} position={stop['position']} reason={stop['reason']}", flush=True)
        print("report REDELIVERY comparison: 50/50 (state, final stop, descriptor baselines, LIFE and coverage)", flush=True)
        print("report case 40: no reachable successor descriptor; no replay stop", flush=True)
        if not args.no_mutants: mutants(work, db, oracle)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print("report harness FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
