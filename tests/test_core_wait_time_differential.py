#!/usr/bin/env python3
"""Fixed-seed differential against the independent contract reference, stdlib only.

Adapted from review-r1-diff.py. The C driver only emits actual answers; every
expected answer comes from time_grid.evaluate, including the named regressions.
Temporary sources, binaries and cases live outside the checkout. Exit 1 means
an actual metric mismatch; exit 2 means harness/build/sanitizer failure.
"""
import argparse
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
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
MAX = (1 << 64) - 1
rnd = random.Random(868_20261111)

EV = ["A", "B"]
def T(): return rnd.randint(0, 40)
def valid_case():
    ls = rnd.randint(0, 10)
    le = None if rnd.random() < 0.4 else rnd.randint(ls, 40)
    hi = 40 if le is None else le
    # waits laid out sequentially
    waits = []; t = ls
    for _ in range(rnd.randint(0, 4)):
        s = rnd.randint(t, min(hi, t + 8)); e = rnd.randint(s, min(hi, s + rnd.choice([0, 0, 3, 8])))
        waits.append([rnd.choice(EV), s, e]); t = e
    op = None
    if rnd.random() < 0.3:
        s = rnd.randint(t, hi) if t <= hi else t
        lim = hi - 1 if le is not None else 40
        if s <= lim:
            op = [rnd.choice(EV), s, rnd.randint(s, lim)]
    covhi = hi if op is None else op[2]
    cov = []; t = ls
    for _ in range(rnd.randint(0, 3)):
        if t > covhi: break
        a = rnd.randint(t, covhi); b = rnd.randint(a, covhi)
        if rnd.random() < 0.5: a = t; b = rnd.randint(a, covhi) if rnd.random()<0.5 else covhi
        cov.append([a, b]); t = b
    rnd.shuffle(waits)
    return {"life": [ls, le], "coverage": cov, "waits": waits, "open": op}
def perturb(e):
    k = rnd.randint(0, 11)
    if any(x not in e for x in ('life','coverage','waits','open')): return e
    if k == 0 and e["waits"]: w = rnd.choice(e["waits"]); w[1], w[2] = T(), T()
    elif k == 1: e["waits"].append([rnd.choice(EV), T(), T()])
    elif k == 2: e["coverage"].append([T(), T()])
    elif k == 3 and e["coverage"]: rnd.shuffle(e["coverage"])
    elif k == 4: e["open"] = [rnd.choice(EV), T(), T()]
    elif k == 5: e["life"] = [T(), rnd.choice([None, T()])]
    elif k == 6:
        z = T(); e["waits"].append([rnd.choice(EV), z, z])
    elif k == 7:  # shape breakage
        f = rnd.choice(["life", "coverage", "waits", "open"])
        bad = rnd.choice([True, "1", 1.0, None, [1], {}, -1, 18446744073709551616])
        if f == "life": e["life"] = [bad, e["life"][1]] if rnd.random()<.5 else [e["life"][0], bad]
        elif f == "coverage": e["coverage"].append([bad, 3])
        elif f == "waits": e["waits"].append(["A", bad, 5] if rnd.random()<.5 else ["", 1, 2])
        else: e["open"] = rnd.choice([["A", bad, 5], ["A", 1, bad], ["A", 1, 5, 9], ["A", 1], "x", True, ["", 1, 5]])
    elif k == 8: del e[rnd.choice(["life", "coverage", "waits", "open"])]
    elif k == 9 and e["waits"]:
        w = rnd.choice(e["waits"]); e["waits"].append(list(w))
    elif k == 10: e["coverage"] = [[T(), T()] for _ in range(rnd.randint(1,3))]
    else: e["open"] = None if e.get("open") else [rnd.choice(EV), T(), T()]
    return e
def query():
    f, t = T(), T()
    r = rnd.random()
    if r < 0.6 and f > t: f, t = t, f
    if r < 0.05: t = f
    q = {"event": rnd.choice(EV + ["A"] * 2), "from": f, "to": t}
    if rnd.random() < 0.04: q["event"] = ""
    if rnd.random() < 0.03: q[rnd.choice(["from", "to"])] = rnd.choice([1.0, True, "2", -1])
    if rnd.random() < 0.02: del q[rnd.choice(["event", "from", "to"])]
    return q

def shifted(value, offset):
    if type(value) is int and value >= 0:
        return value + offset
    if isinstance(value, list):
        return [shifted(v, offset) for v in value]
    if isinstance(value, dict):
        return {k: shifted(v, offset) for k, v in value.items()}
    return value


def generate():
    # The contract freezes both evidence and queries, independently of this runner.
    cases = json.loads((ROOT / "contracts/wait_time-v1/differential_cases.json").read_text())
    for i in range(6300):
        e = valid_case()
        if rnd.random() < 0.45:
            for _ in range(rnd.randint(1, 2)):
                e = perturb(e)
        q = query()
        mode = i % 3
        if mode == 1:
            e, q = shifted(e, MAX - 40), shifted(q, MAX - 40)
        elif mode == 2:
            if rnd.random() < 0.5:
                e = shifted(e, MAX - 40)
            q["from"], q["to"] = rnd.choice([(0, MAX), (0, 2**63 + 7), (5, MAX)])
        cases.append({"id": f"random_{i}", "evidence": e, "query": q})
    # JSON exponent tokens must be decoded independently on both paths.
    for token in (True, "1", 1.0, "__EXP__", -1, MAX + 1):
        cases.append({"id": f"token_{len(cases)}", "evidence": {
            "life": [0, None], "coverage": [[0, 40]], "waits": [["A", token, 10]],
            "open": None}, "query": {"event": "A", "from": 0, "to": 40}})
    return cases


DRIVER = r"""
#include "core/contract_json.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        pgwt_contract_case c;
        char error[256] = {0}, t[2048], n[2048];
        if (pgwt_contract_case_load(argv[i], &c, error, sizeof(error))) {
            fprintf(stderr, "%s: %s\n", argv[i], error); return 2;
        }
        size_t count = c.evidence.wait_count;
        pgwt_core_wait *scratch = count ? calloc(count, sizeof(*scratch)) : NULL;
        pgwt_core_prepared p;
        if ((count && !scratch) || pgwt_core_prepare(&c.evidence, scratch, count, &p)) {
            free(scratch); pgwt_contract_case_free(&c); return 2;
        }
        free(scratch);
        pgwt_core_result time, completed;
        pgwt_core_wait_metrics(&p, &c.query, &time, &completed);
        if (pgwt_contract_result_json(&time, t, sizeof(t)) ||
            pgwt_contract_result_json(&completed, n, sizeof(n))) {
            pgwt_contract_case_free(&c); return 2;
        }
        printf("{\"wait_time\":%s,\"completed_waits\":%s}\n", t, n);
        pgwt_contract_case_free(&c);
    }
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT,
                        help="core sources to test (mutation harness)")
    args = parser.parse_args()
    started = time.monotonic()
    spec = importlib.util.spec_from_file_location(
        "time_grid", ROOT / "contracts/wait_time-v1/reference/time_grid.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "abort_on_error=1")
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="pgwt-core-differential-") as temporary:
        work = Path(temporary)
        driver = work / "driver.c"
        driver.write_text(DRIVER)
        binary = work / "driver"
        src = args.source_root.resolve() / "src"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-g", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-Werror=implicit-function-declaration"]
        # Select sanitizers through CFLAGS, like the test-asan workflow. A
        # normal run must compare every case without sanitizer runtimes too.
        command += shlex.split(os.environ.get("CFLAGS", ""))
        command += ["-I" + str(src), str(driver), str(src / "core/wait_metrics.c"),
                    str(src / "core/contract_json.c"), "-o", str(binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=45)
        if built.returncode:
            raise RuntimeError("build failed:\n" + built.stdout + built.stderr)
        cases = generate()
        paths = []
        for i, case in enumerate(cases):
            raw = json.dumps(case).replace('"__EXP__"', '1e0')
            cases[i] = json.loads(raw)
            path = work / f"{i}.json"
            path.write_text(raw)
            paths.append(str(path))
        output = []
        for i in range(0, len(paths), 500):
            run = subprocess.run([str(binary)] + paths[i:i+500], capture_output=True,
                                 text=True, env=environment, timeout=15)
            if run.returncode or re.search(
                    r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", run.stderr):
                raise RuntimeError(f"driver failed ({run.returncode}):\n{run.stderr}")
            output.extend(run.stdout.splitlines())
        if len(output) != len(cases):
            raise RuntimeError(f"driver returned {len(output)} answers for {len(cases)} cases")
        named_ids = {case["id"] for case in json.loads(
            (ROOT / "contracts/wait_time-v1/differential_cases.json").read_text())}
        mismatches = 0
        for case, line in zip(cases, output):
            actual, expected = json.loads(line), reference.evaluate(case)
            if case["id"] in named_ids:
                equal = all(actual[m] == expected[m] for m in ("wait_time", "completed_waits"))
                print(f"NAMED {case['id']} " + ("MATCH" if equal else "MISMATCH"), flush=True)
            if case["id"] == "X11_whole":
                equal = all(actual[m] == expected[m] for m in ("wait_time", "completed_waits"))
                print("X11 whole-trace: " + ("MATCH" if equal else "MISMATCH"))
            for metric in ("wait_time", "completed_waits"):
                if actual[metric] != expected[metric]:
                    mismatches += 1
                    if mismatches <= 12:
                        print(f"{case['id']} MISMATCH ({metric})")
                        print("  input:", json.dumps(case))
                        print("  core:", json.dumps(actual[metric]))
                        print("  reference:", json.dumps(expected[metric]))
        elapsed = time.monotonic() - started
        print(f"core wait_time differential: {len(cases)} cases, {mismatches} differences, {elapsed:.2f}s")
        return 1 if mismatches else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"core differential: harness error: {error}", file=sys.stderr)
        sys.exit(2)
