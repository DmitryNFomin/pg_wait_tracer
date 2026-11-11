#!/usr/bin/env bash
# Each mutant is an actual edit to a temporary source copy. No checkout edits,
# expected-answer edits, compile-time mutant switches, or tests/results output.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 - <<'PY'
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path.cwd()
compiler = shlex.split(os.environ.get("CC", "cc"))
flags = ["-std=c11", "-g", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-Werror=implicit-function-declaration"]
flags += shlex.split(os.environ.get("CFLAGS", ""))
environment = os.environ.copy()
environment.setdefault("ASAN_OPTIONS", "abort_on_error=1")
environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")

def run(command, **kwargs):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, env=environment, **kwargs)

# (name, original, replacement, intended distinguishing case).
# M7 clamps only at the double-to-uint64 boundary, avoiding undefined conversion
# of the rounded 2^64. The accumulation itself deliberately loses precision.
mutants = [
    ("M1", "q->from <= w.end && w.end < q->to",
     "q->from <= w.end && w.end <= q->to", "02"),
    ("M2", "q->from <= w.end && w.end < q->to",
     "q->from < w.end && w.end < q->to", "03"),
    ("M3", "clipped_ns(e->open.start, e->open.as_of, q->from, q->to)",
     "clipped_ns(e->open.start, q->to, q->from, q->to)", "17"),
    ("M4", "completion_gap = coverage_gap || !exit_certified(e, q);",
     "completion_gap = coverage_gap;", "32"),
    ("M5", "time->value.present = !coverage_gap;",
     "time->value.present = true;", "08"),
    ("M6", "if (!same_event(w.event, q->event))",
     "if (w.start == w.end || !same_event(w.event, q->event))", "19"),
    ("M7", "closed += clipped_ns(w.start, w.end, q->from, q->to);",
     "double sum = (double)closed + (double)clipped_ns(w.start, w.end, q->from, q->to);\n"
     "        closed = sum >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)sum;", "26"),
    ("M8", "bool coverage_gap = false;\n    bool completion_gap = false;",
     "bool coverage_gap = invalid_window;\n    bool completion_gap = invalid_window;", "36"),
    ("M9", "time->detail_present = time->value.present;",
     "time->detail_present = true;", "08"),
    ("M10", "if (e->has_open && s.end > e->open.as_of)",
     "if (false)", "37"),
]

def build(directory, output):
    return run(compiler + flags + ["-I" + str(directory / "src"),
               str(directory / "tests/test_core_wait_time_contract.c"),
               str(directory / "src/core/wait_metrics.c"),
               str(directory / "src/core/contract_json.c"), "-o", str(output)], timeout=45)

def has_sanitizer_error(output):
    return re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", output) is not None

def summary(output):
    lines = re.findall(r"^core wait_time contract: (\d+)/48 passed$", output, re.M)
    return len(lines) == 1

try:
    with tempfile.TemporaryDirectory(prefix="pgwt-core-mutants-") as tmp:
        work = Path(tmp)
        probe = work / "sanitizer-probe"
        sanitizer_flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        check = run(compiler + flags + sanitizer_flags + ["-x", "c", "-", "-o", str(probe)],
                    input="int main(void) { return 0; }\n", timeout=45)
        if check.returncode == 0 and run([str(probe)], timeout=15).returncode == 0:
            flags += sanitizer_flags
        else:
            print("core mutants: address/undefined sanitizers unavailable", flush=True)

        base = work / "baseline"
        (base / "src").mkdir(parents=True)
        (base / "tests").mkdir()
        shutil.copytree(root / "src/core", base / "src/core")
        shutil.copy2(root / "tests/test_core_wait_time_contract.c", base / "tests")
        executable = base / "contract-test"
        built = build(base, executable)
        if built.returncode != 0:
            print("core mutants: baseline BUILD_ERROR\n" + built.stdout)
            sys.exit(2)
        baseline = run([str(executable), str(root / "contracts/wait_time-v1/contracts")], timeout=15)
        if baseline.returncode != 0 or not summary(baseline.stdout) or has_sanitizer_error(baseline.stdout):
            print("core mutants: baseline FAILED\n" + baseline.stdout)
            sys.exit(2)
        print(baseline.stdout.splitlines()[-1], flush=True)

        failed = False
        for name, old, new, case in mutants:
            directory = work / name
            shutil.copytree(base, directory, ignore=shutil.ignore_patterns("contract-test", "*.dSYM"))
            source = directory / "src/core/wait_metrics.c"
            text = source.read_text()
            if text.count(old) != 1:
                print(f"{name} PATCH_ERROR (expected one mutation site)", flush=True)
                failed = True
                continue
            source.write_text(text.replace(old, new, 1))
            try:
                built = build(directory, directory / "contract-test")
                if built.returncode != 0:
                    print(f"{name} BUILD_ERROR (not a kill)\n{built.stdout}", flush=True)
                    failed = True
                    continue
                result = run([str(directory / "contract-test"),
                              str(root / "contracts/wait_time-v1/contracts")], timeout=15)
            except subprocess.TimeoutExpired:
                print(f"{name} TIMEOUT (not a kill)", flush=True)
                failed = True
                continue
            intended = re.search(rf"^wait_window_{case}\.json MISMATCH \(.+\)$", result.stdout, re.M)
            cases = re.findall(r"^wait_window_\d+\.json (?:PASS|MISMATCH)\b", result.stdout, re.M)
            if (result.returncode == 1 and intended and summary(result.stdout)
                    and len(cases) >= 48 and not has_sanitizer_error(result.stdout)):
                print(f"{name} KILLED (wait_window_{case}, build OK, case mismatch)", flush=True)
            else:
                print(f"{name} SURVIVED/ERROR (exit {result.returncode}; not an intended case mismatch)\n"
                      + result.stdout, flush=True)
                failed = True
        sys.exit(1 if failed else 0)
except (OSError, subprocess.TimeoutExpired) as error:
    print(f"core mutants: harness error: {error}")
    sys.exit(2)
PY
