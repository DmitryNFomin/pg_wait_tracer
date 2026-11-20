#!/usr/bin/env python3
"""Fail closed on drift in CI's required full-check job (stdlib only).

This intentionally scans the workflow's house format: two-space job keys,
four-space job properties and literal run blocks. Unsupported shapes refuse
rather than silently approve an uninspected job. No YAML dependency is needed
by the local fast tier.
"""
from pathlib import Path
import re
import sys
import textwrap


class Refusal(ValueError):
    pass


def full_check_job(workflow):
    sections = workflow.split("\njobs:\n")
    if len(sections) != 2:
        raise Refusal("workflow must have one top-level jobs mapping")
    workflow = sections[1]
    jobs = list(re.finditer(r"(?m)^  ([\w-]+):[ \t]*$", workflow))
    matches = [i for i, job in enumerate(jobs) if job[1] == "full-check"]
    if len(matches) != 1:
        raise Refusal("full-check job must exist exactly once")
    index = matches[0]
    end = jobs[index + 1].start() if index + 1 < len(jobs) else len(workflow)
    return workflow[jobs[index].end():end]


def full_check_run(job):
    blocks = re.findall(r"(?m)^        run: \|\n((?:          [^\n]*\n|\n)+)", job)
    checks = [textwrap.dedent(block) for block in blocks if "scripts/check.sh" in block]
    if len(checks) != 1:
        raise Refusal("full-check must have one literal check.sh run block")
    return checks[0]


def validate(workflow):
    job = full_check_job(workflow)
    runners = re.findall(r"(?m)^    runs-on: ([^\n]+)", job)
    if len(runners) != 1 or runners[0].split("#", 1)[0].strip() != "ubuntu-24.04":
        raise Refusal("full-check runner must be hosted ubuntu-24.04")
    if re.search(r"(?m)^    needs:|^ +(?:if|continue-on-error):", job):
        raise Refusal("full-check must run unconditionally and propagate failure")
    run = full_check_run(job)
    if "--fast" in job:
        raise Refusal("full-check must not pass --fast")
    if not re.search(r'(?m)^scripts/check-lock\.sh scripts/check\.sh \| tee "\$check_log"$', run):
        raise Refusal("full-check must run the entire check.sh through the check lock")
    if "set -euo pipefail\n" not in run:
        raise Refusal("full-check must preserve check.sh failure through tee")
    if not re.search(r"(?m)^grep -F 'CHECK PASSED \(full\)' \"\$check_log\"$", run):
        raise Refusal("full-check must require check.sh's own full verdict")
    if not re.search(r"if grep -F 'CHECK PASSED \(fast\)' \"\$check_log\"; then\n"
                     r"(?:  [^\n]*\n)*  exit 1\nfi", run):
        raise Refusal("full-check must reject a fast verdict")
    if not re.search(r"(?m)^          fetch-depth: 0$", job):
        raise Refusal("full-check history guards require full checkout history")


def main():
    path = Path(sys.argv[1]) if len(sys.argv) == 2 else (
        Path(__file__).resolve().parent.parent / ".github/workflows/ci.yml")
    try:
        validate(path.read_text())
    except (OSError, ValueError) as exc:
        print(f"REFUSE: CI full-check: {exc}", file=sys.stderr)
        return 1
    print("CI full-check guard: OK (hosted, full suite, failure and mode verdicts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
