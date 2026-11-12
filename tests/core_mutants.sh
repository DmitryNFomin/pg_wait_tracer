#!/usr/bin/env bash
# Each mutant is an actual edit to a temporary source copy. No checkout edits,
# expected-answer edits, compile-time mutant switches, or tests/results output.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 - <<'PY'
import hashlib
import json
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
# Sanitizers are selected by the caller, consistently with the differential.
flags += shlex.split(os.environ.get("CFLAGS", ""))
environment = os.environ.copy()
environment.setdefault("ASAN_OPTIONS", "abort_on_error=1")
environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")

def run(command, **kwargs):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, env=environment, **kwargs)

# Mutation sites, replacements and distinguishing inputs belong to the contract.
mutants = json.loads((root / "contracts/wait_time-v1/mutants.json").read_text())

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
        print("CORE CONTRACT BEGIN\n" + baseline.stdout.rstrip() + "\nCORE CONTRACT END", flush=True)
        if baseline.returncode != 0 or not summary(baseline.stdout) or has_sanitizer_error(baseline.stdout):
            print("core mutants: baseline FAILED")
            sys.exit(2)
        baseline_diff = run([sys.executable, "-B",
                             str(root / "tests/test_core_wait_time_differential.py"),
                             "--source-root", str(base)], timeout=55)
        if baseline_diff.returncode != 0 or has_sanitizer_error(baseline_diff.stdout):
            print("core mutants: baseline differential FAILED\n" + baseline_diff.stdout)
            sys.exit(2)
        print(baseline_diff.stdout.strip(), flush=True)

        failed = False
        killed = 0
        for mutant in mutants:
            name, old, new, case = (mutant[k] for k in
                                   ("id", "original", "replacement", "intended_case"))
            # Bind each kill to the actual replacement executed, not just its id.
            definition_hash = hashlib.sha256(json.dumps(
                [name, old, new, case], separators=(",", ":")).encode()).hexdigest()
            case = case if case.startswith("diff:") else case.removeprefix("case:wait_window_").removesuffix(".json")
            directory = work / name
            shutil.copytree(base, directory, ignore=shutil.ignore_patterns("contract-test", "*.dSYM"))
            source = directory / "src/core/wait_metrics.c"
            text = source.read_text()
            if text.count(old) != 1:
                print(f"{name} PATCH_ERROR (expected one mutation site)", flush=True)
                failed = True
                continue
            source.write_text(text.replace(old, new, 1))
            source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
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
            if case.startswith("diff:"):
                # Require a successful build, full evaluation, and
                # an actual reference mismatch on the named distinguishing input.
                try:
                    result = run([sys.executable, "-B",
                                  str(root / "tests/test_core_wait_time_differential.py"),
                                  "--source-root", str(directory)], timeout=55)
                except subprocess.TimeoutExpired:
                    print(f"{name} TIMEOUT (not a kill)", flush=True)
                    failed = True
                    continue
                intended = re.search(rf"^{case[5:]} MISMATCH \(.+\)$", result.stdout, re.M)
                finished = re.search(r"^core wait_time differential: \d+ cases, \d+ differences,", result.stdout, re.M)
                preserved = name != "X11" or "X11 whole-trace: MATCH" in result.stdout
                if (result.returncode == 1 and intended and finished and preserved
                        and not has_sanitizer_error(result.stdout)):
                    print(f"{name} KILLED (differential {case[5:]}, build OK, reference mismatch) "
                          f"definition-sha256={definition_hash} source-sha256={source_hash}", flush=True)
                    killed += 1
                else:
                    print(f"{name} SURVIVED/ERROR (exit {result.returncode}; not an intended reference mismatch)\n"
                          + result.stdout, flush=True)
                    failed = True
            else:
                intended = re.search(rf"^wait_window_{case}\.json MISMATCH \(.+\)$", result.stdout, re.M)
                cases = re.findall(r"^wait_window_\d+\.json (?:PASS|MISMATCH)\b", result.stdout, re.M)
                if (result.returncode == 1 and intended and summary(result.stdout)
                        and len(cases) >= 48 and not has_sanitizer_error(result.stdout)):
                    print(f"{name} KILLED (contract wait_window_{case}, build OK, case mismatch) "
                          f"definition-sha256={definition_hash} source-sha256={source_hash}", flush=True)
                    killed += 1
                else:
                    print(f"{name} SURVIVED/ERROR (exit {result.returncode}; not an intended case mismatch)\n"
                          + result.stdout, flush=True)
                    failed = True
        print(f"core mutants: {killed}/{len(mutants)} KILLED", flush=True)
        sys.exit(1 if failed else 0)
except (OSError, subprocess.TimeoutExpired) as error:
    print(f"core mutants: harness error: {error}")
    sys.exit(2)
PY
