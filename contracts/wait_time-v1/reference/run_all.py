#!/usr/bin/env python3
"""Run time_grid.py as a subprocess for every supplied case and compare JSON.

Compares complete metric objects, including key presence and nulls. Derivations
are hand-only metadata; evaluator metadata is reference-only. A failed comparison
writes a persistent report (never overwrites one) and exits nonzero. Adjudicate
and record it under CHALLENGES.md / Self-disagreements before changing answers.
"""

import difflib
import json
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent.parent
    examples = root / "contracts" / "examples"
    expected_dir = root / "contracts" / "expected"
    evaluator = root / "reference" / "time_grid.py"
    cases = sorted(examples.glob("wait_window_*.json"))
    failures = []
    matched = 0
    large = 0
    for case_file in cases:
        case_id = case_file.stem
        try:
            expected = json.loads((expected_dir / case_file.name).read_text(encoding="utf-8"))
            process = subprocess.run(
                [sys.executable, "-B", str(evaluator), str(case_file)],
                capture_output=True, text=True, check=True, cwd=root)
            actual = json.loads(process.stdout)
            mode = actual["evaluator"]["mode"]
            if mode == "exact_integer_intervals":
                large += 1
            # Check IDs against the input, not only against each other.
            input_id = json.loads(case_file.read_text(encoding="utf-8"))["id"]
            if expected["id"] != input_id or actual["id"] != input_id:
                raise ValueError("id mismatch with input")
            if set(expected) != {"id", "wait_time", "completed_waits", "derivation"}:
                raise ValueError("unexpected expected-file keys")
            if not isinstance(expected["derivation"], str) or not 2 <= len(expected["derivation"].splitlines()) <= 3:
                raise ValueError("derivation must have two or three lines")
            expected_metrics = {k: expected[k] for k in ("wait_time", "completed_waits")}
            actual_metrics = {k: actual[k] for k in ("wait_time", "completed_waits")}
            # Canonical JSON distinguishes true/1 and 1.0/1 as well as null/key absence.
            before = json.dumps(expected_metrics, indent=2, sort_keys=True)
            after = json.dumps(actual_metrics, indent=2, sort_keys=True)
            if before == after:
                matched += 1
                print(f"{case_id}: PASS ({mode})")
            else:
                diff = "\n".join(difflib.unified_diff(
                    before.splitlines(), after.splitlines(),
                    fromfile="hand expected", tofile="independent reference", lineterm=""))
                failures.append({"id": case_id, "hand": expected_metrics,
                                 "reference": actual_metrics, "diff": diff})
                print(f"{case_id}: FAIL ({mode}); see persistent disagreement report")
        except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
            failure = {"id": case_id, "error": str(error)}
            if isinstance(error, subprocess.CalledProcessError):
                failure["stderr"] = error.stderr
            failures.append(failure)
            print(f"{case_id}: ERROR; see persistent disagreement report")

    expected_files = {p.name for p in expected_dir.glob("wait_window_*.json")}
    case_files = {p.name for p in cases}
    if len(cases) != 48 or expected_files != case_files:
        failures.append({"suite_error": "Expected exactly 48 cases with a one-to-one expected-file set",
                         "case_count": len(cases),
                         "extra_expected": sorted(expected_files - case_files),
                         "missing_expected": sorted(case_files - expected_files)})
    if failures:
        index = 1
        while (root / "reference" / f"disagreements_{index:03}.json").exists():
            index += 1
        report = root / "reference" / f"disagreements_{index:03}.json"
        with report.open("x", encoding="utf-8") as stream:
            json.dump(failures, stream, indent=2)
            stream.write("\n")
        print(f"Disagreement report: reference/{report.name}; adjudicate in CHALLENGES.md before correction.")
    print(f"RESULT: {matched}/{len(cases)} cases matched; {len(failures)} failures; {large} exact-integer fallbacks.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
