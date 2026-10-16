#!/usr/bin/env python3
"""check-runner-registration.py -- reconcile tests/run_all.sh (make
box-check) against tests/ci_smoke.sh (CI/nightly's capture-smoke job):
every test file tests/ci_smoke.sh actually invokes must also be actually
invoked by tests/run_all.sh, unless it is named in
tests/runner_registration_exemptions.list with a reason (issue #433).

Why this direction and not the reverse: tests/run_all.sh is the broad
matrix `make box-check` runs; tests/ci_smoke.sh is deliberately a small,
narrow real-PG capture smoke test, so the great majority of run_all.sh's
tests are legitimately absent from ci_smoke.sh with no exemption needed --
flagging that direction would produce dozens of false positives for no
signal. The direction that actually burned this project three times is the
other one: a test wired into ci_smoke.sh (so it only ever runs in CI or
nightly, on the self-hosted gate boxes) that nobody also wired into
run_all.sh, so `make box-check` -- the thing an agent runs before every PR
-- reports green while that gate never executed at all:
  1. test_v9_open_cpu_wired registered only in ci_smoke.sh; box-check
     scored 134/134 while the gate it was meant to prove never ran.
  2. the same gate deregistered from run_all.sh but left invoked in
     ci_smoke.sh -- box-check stayed green for the same reason.
  3. test_capture_smoke appears in run_all.sh only inside a COMMENT (a
     naive grep would count it as "registered"); it is executed solely by
     ci_smoke.sh, so box-check never exercises the phase most affected by
     src/daemon.c changes.

"Actually invokes", not "mentions": a test file name appearing in a shell
comment, a docstring, or a human-readable explanation of why something is
NOT wired does not count -- only an executed `python3 "$SCRIPT_DIR/x.py"` /
`bash "$SCRIPT_DIR/x.sh"` line, or a tests/run_all.sh LIVE_TESTS array
entry whose runner field is "bash" or "python3", counts as registered.

A guard that cannot parse one of these two files must REFUSE, not approve
blind (issue #431's defect, the one this file exists not to repeat): if
either file is missing/unreadable, or looks non-trivial (mentions
"python3"/"bash" at all) yet this parser extracts zero invocations from it,
this refuses rather than silently comparing against an empty set (which
would make every real registration gap invisible).

Usage: check-runner-registration.py [repo-dir]
  repo-dir defaults to this script's own repository; a test harness passes
  a scratch directory with its own tests/run_all.sh, tests/ci_smoke.sh and
  (optionally) tests/runner_registration_exemptions.list to exercise the
  logic in isolation (see tests/test_check_runner_registration.sh).
Exit: 0 reconciled (or every gap is a documented exemption), 1 refused or
  found an undocumented gap.
"""
import os
import re
import sys

LIVE_TESTS_RE = re.compile(r"LIVE_TESTS=\((.*?)\n\)", re.S)
DIRECT_INVOKE_RE = re.compile(
    r'\b(?:python3|bash)\s+"\$SCRIPT_DIR/([A-Za-z0-9_]+\.(?:py|sh))"'
)


def strip_full_line_comments(text):
    """Drop every line whose first non-whitespace character is '#'.

    Deliberately does NOT strip a trailing "  # ..." suffix from a
    code line: every invocation line in both real files here is either a
    bare command or ends in the quoted script path with no inline
    comment, and trying to strip inline comments generically risks
    truncating a quoted argument that happens to contain '#'. A comment
    that shares a line with real code is not this guard's blind spot --
    instance 3 (a FULL-LINE comment mentioning a filename) is, and that is
    what this function removes.
    """
    kept = []
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        kept.append(line)
    return "\n".join(kept)


def extract_direct_invocations(clean_text):
    return set(DIRECT_INVOKE_RE.findall(clean_text))


def extract_live_tests_array(clean_text):
    """tests/run_all.sh's LIVE_TESTS=( "name|runner|file|min_pg" ... )."""
    files = set()
    m = LIVE_TESTS_RE.search(clean_text)
    if not m:
        return files, False
    for raw_line in m.group(1).splitlines():
        s = raw_line.strip()
        if not (s.startswith('"') and '"' in s[1:]):
            continue
        entry = s.split('"')[1]
        parts = entry.split("|")
        if len(parts) >= 3:
            _name, runner, fname = parts[0], parts[1], parts[2]
            if fname and runner in ("bash", "python3"):
                files.add(fname)
    return files, True


def looks_nontrivial(raw_text):
    return ("python3" in raw_text) or ("bash " in raw_text) or ('bash"' in raw_text)


def read_exemptions(repo, errors):
    """tests/runner_registration_exemptions.list: 'filename|reason' per
    line, '#' comments and blank lines ignored. Absent file == no
    exemptions (not an error: a repo need not have any yet)."""
    path = os.path.join(repo, "tests", "runner_registration_exemptions.list")
    exemptions = {}
    if not os.path.exists(path):
        return exemptions
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()
    except OSError as e:
        errors.append(
            f"could not read {path}: {e} -- refusing to approve blind "
            "(an exemption list that cannot be read must not be treated as empty)."
        )
        return None
    for lineno, line in enumerate(raw.splitlines(), start=1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if "|" not in s:
            errors.append(
                f"{path}:{lineno}: malformed exemption entry (no '|'): {s!r} -- "
                "refusing to approve blind (expected 'filename|reason')."
            )
            continue
        fname, reason = s.split("|", 1)
        fname = fname.strip()
        reason = reason.strip()
        if not fname or not reason:
            errors.append(
                f"{path}:{lineno}: malformed exemption entry (empty name or "
                f"reason): {s!r} -- refusing to approve blind."
            )
            continue
        exemptions[fname] = reason
    return exemptions


def main(argv):
    repo = argv[1] if len(argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    run_all_path = os.path.join(repo, "tests", "run_all.sh")
    ci_smoke_path = os.path.join(repo, "tests", "ci_smoke.sh")

    for label, path in (("tests/run_all.sh", run_all_path), ("tests/ci_smoke.sh", ci_smoke_path)):
        if not os.path.isfile(path):
            print(
                f"FAIL: runner-registration guard: {label} not found at {path} -- "
                "refusing to approve blind (cannot reconcile membership against a "
                "runner that does not exist).",
                file=sys.stderr,
            )
            return 1

    try:
        with open(run_all_path, "r", encoding="utf-8") as f:
            run_all_raw = f.read()
        with open(ci_smoke_path, "r", encoding="utf-8") as f:
            ci_smoke_raw = f.read()
    except OSError as e:
        print(
            f"FAIL: runner-registration guard: could not read a runner file: {e} -- "
            "refusing to approve blind.",
            file=sys.stderr,
        )
        return 1

    run_all_clean = strip_full_line_comments(run_all_raw)
    ci_smoke_clean = strip_full_line_comments(ci_smoke_raw)

    ra_direct = extract_direct_invocations(run_all_clean)
    ra_live, live_tests_found = extract_live_tests_array(run_all_clean)
    ra_files = ra_direct | ra_live

    ci_files = extract_direct_invocations(ci_smoke_clean)

    # Parser self-check (the #431 class): a file that plainly mentions
    # python3/bash somewhere yet yields zero extracted invocations means
    # the FORMAT drifted out from under this parser, not that nothing is
    # registered -- comparing empty-against-empty would silently pass
    # every real gap. Refuse instead of guessing.
    if looks_nontrivial(ci_smoke_clean) and not ci_files:
        print(
            "FAIL: runner-registration guard: tests/ci_smoke.sh mentions "
            "python3/bash but this parser extracted zero invocations from it -- "
            "the format drifted out from under this guard (this guard needs "
            "updating, not bypassing); refusing to approve blind.",
            file=sys.stderr,
        )
        return 1
    if looks_nontrivial(run_all_clean) and not ra_files:
        print(
            "FAIL: runner-registration guard: tests/run_all.sh mentions "
            "python3/bash but this parser extracted zero invocations from it "
            f"(LIVE_TESTS array found: {live_tests_found}) -- the format drifted "
            "out from under this guard; refusing to approve blind.",
            file=sys.stderr,
        )
        return 1

    errors = []
    exemptions = read_exemptions(repo, errors)
    if exemptions is None:
        for e in errors:
            print(f"FAIL: runner-registration guard: {e}", file=sys.stderr)
        return 1
    if errors:
        for e in errors:
            print(f"FAIL: runner-registration guard: {e}", file=sys.stderr)
        return 1

    gaps = sorted(f for f in ci_files if f not in ra_files and f not in exemptions)

    if gaps:
        print(
            "FAIL: runner-registration guard: tests/ci_smoke.sh invokes these test "
            "files but tests/run_all.sh (what `make box-check` actually runs) does "
            "not -- a box-check \"green\" proves nothing about them:",
            file=sys.stderr,
        )
        for f in gaps:
            print(f"    {f}", file=sys.stderr)
        print(
            "  Either wire each one into tests/run_all.sh's LIVE_TESTS (same commit "
            "as whatever made it ci_smoke-only), or document why it is legitimately "
            "CI/nightly-only in tests/runner_registration_exemptions.list "
            "('filename|reason', one per line).",
            file=sys.stderr,
        )
        return 1

    exempted_and_absent = sorted(f for f in exemptions if f not in ci_files and f not in ra_files)
    print(
        "OK: runner-registration guard -- every tests/ci_smoke.sh invocation "
        f"({len(ci_files)}) is also invoked by tests/run_all.sh ({len(ra_files)} "
        f"invocations total), or is a documented exemption ({len(exemptions)} listed"
        + (f", {len(exempted_and_absent)} currently invoke neither runner" if exempted_and_absent else "")
        + ")."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
