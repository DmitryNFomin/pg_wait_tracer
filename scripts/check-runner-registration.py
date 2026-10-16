#!/usr/bin/env python3
"""check-runner-registration.py -- two checks for issue #433's failure
class ("a check that passes because it never looked"), both about who
actually runs a test file:

1. RECONCILIATION: every test file tests/ci_smoke.sh actually invokes must
   also be actually invoked by tests/run_all.sh (make box-check), unless it
   is named in tests/runner_registration_exemptions.list with a reason.
2. ORPHAN SCAN: every tests/test_*.py / tests/test_*.sh file on disk must be
   actually invoked by at least one of tests/run_all.sh (directly, via its
   LIVE_TESTS array, or via tests/unit_tests.list -- run_all.sh executes
   every line of that file too), tests/ci_smoke.sh, or scripts/check.sh
   (directly, or via a glob it runs against, e.g. tests/web_unit/*.test.mjs),
   or named in the same exemption list. A file invoked by NOTHING is a test
   nobody runs -- found twice for real while this guard was being written:
   tests/test_check_runner_registration.sh and
   tests/test_results_evidence_ignore.sh existed, unregistered, before being
   wired into scripts/check.sh in this same area of work.

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
  a scratch directory with its own tests/run_all.sh, tests/ci_smoke.sh,
  scripts/check.sh and (optionally) tests/unit_tests.list /
  tests/runner_registration_exemptions.list to exercise the logic in
  isolation (see tests/test_check_runner_registration.sh).
Exit: 0 both checks clean (or every gap is a documented exemption), 1
  refused or found an undocumented gap.
"""
import fnmatch as fnmatchmod
import glob as globmod
import os
import re
import sys

LIVE_TESTS_RE = re.compile(r"LIVE_TESTS=\((.*?)\n\)", re.S)
DIRECT_INVOKE_RE = re.compile(
    r'\b(?:python3|bash)\s+"\$SCRIPT_DIR/([A-Za-z0-9_]+\.(?:py|sh))"'
)
# scripts/check.sh invokes by a bare `tests/<file>` path (no $SCRIPT_DIR,
# often no python3/bash prefix either -- it relies on the shebang + exec
# bit, e.g. `run tests/test_check_snapshot_history.sh`), and some of those
# paths are a glob rather than one literal file (e.g.
# `tests/web_unit/*.test.mjs`, run via `node --test`).
CHECK_SH_PATH_RE = re.compile(r"\btests/([A-Za-z0-9_./*-]+\.(?:py|sh|mjs))")
TEST_FILE_NAME_RE = re.compile(r"^test_[A-Za-z0-9_]+\.(?:py|sh)$")


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


def extract_unit_tests_list_entries(repo, errors):
    """tests/run_all.sh's Step 2 reads tests/unit_tests.list line by line and
    executes EVERY entry (`python3 "$t"` for a .py name, `./"$t"` for
    anything else -- including .sh names, via their shebang + exec bit), so
    every name in this file is "invoked by run_all.sh" regardless of
    run_all.sh's own source text ever naming it directly. Absent file: not
    an error (a repo need not have this file; check-test-registration.sh's
    own Case 8 applies the same "absent rather than wrong" rule) -- treated
    as zero additional entries, not refused.
    '#' comments and blank lines ignored, same convention as the exemption
    list and as scripts/check-test-registration.sh's own parser.
    """
    path = os.path.join(repo, "tests", "unit_tests.list")
    if not os.path.isfile(path):
        return set()
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()
    except OSError as e:
        errors.append(f"could not read {path}: {e} -- refusing to approve blind.")
        return None
    entries = set()
    for line in raw.splitlines():
        s = line.split("#", 1)[0].strip()
        if s:
            entries.add(s)
    if raw.strip() and not entries:
        errors.append(
            f"{path} is non-empty but this parser extracted zero entries from "
            "it -- the format drifted out from under this guard; refusing to "
            "approve blind."
        )
        return None
    return entries


def extract_bare_tests_path_registrations(clean_text):
    """Bare `tests/<file>` paths (see CHECK_SH_PATH_RE's docstring above),
    used by BOTH scripts/check.sh and the root Makefile's `check:` target
    (`@bash tests/test_check_lock.sh` -- a 4th place a test can be wired in,
    found by this guard flagging test_check_lock.sh as an orphan when it is
    not one). Skips any line containing `py_compile` deliberately --
    compiling a module is not running it as a test, and that glob matches
    EVERY .py file in tests/, so counting it as "registered" would make
    every python test file permanently invisible to the orphan scan (the
    exact unreachable-gate failure class this guard exists to avoid
    creating). Returns (direct_files, glob_patterns): direct_files are
    literal basenames with no '*'; glob_patterns are fnmatch-style patterns
    (relative to the repo) for files discovered by a glob rather than named
    individually (e.g. tests/web_unit/*.test.mjs).
    """
    direct = set()
    globs = []
    for line in clean_text.splitlines():
        if "py_compile" in line:
            continue
        for m in CHECK_SH_PATH_RE.finditer(line):
            token = m.group(1)
            if "*" in token:
                globs.append("tests/" + token)
            else:
                # May include a subdirectory (e.g. web_unit/foo.test.mjs);
                # orphan candidates are flat basenames, so direct files are
                # matched by their basename.
                direct.add(os.path.basename(token))
    return direct, globs


def scan_candidate_test_files(repo):
    """Candidate test files on disk, as repo-relative paths under tests/:

    - tests/test_*.py / tests/test_*.sh -- the naming convention every real
      entry in all three runners already uses (the one exception,
      run_all.sh's LIVE_TESTS "ui_live_smoke.sh" file, is itself a driver
      script a test_-prefixed LIVE_TESTS entry wraps, not a test file by
      this convention, and is registered correctly regardless). A file that
      does not start with test_ is NOT scanned here -- a known, deliberate
      scope limit, stated rather than silently assumed.
    - tests/web_unit/*.test.mjs -- a DIFFERENT naming convention (location,
      not a test_ prefix), included specifically so glob-discovery is a real,
      checked code path here rather than a candidate set that can never
      exercise it.

    Returns repo-relative paths (e.g. "test_accuracy.py",
    "web_unit/aas.test.mjs"), not basenames, so a glob match can be checked
    against the full path.
    """
    found = set()
    for pattern in ("test_*.py", "test_*.sh"):
        for path in globmod.glob(os.path.join(repo, "tests", pattern)):
            found.add(os.path.basename(path))
    for path in globmod.glob(os.path.join(repo, "tests", "web_unit", "*.test.mjs")):
        found.add("web_unit/" + os.path.basename(path))
    return found


def matches_any_glob(relpath, glob_patterns):
    """glob_patterns are repo-relative, e.g. 'tests/web_unit/*.test.mjs';
    relpath is relative to tests/ (e.g. 'web_unit/aas.test.mjs' or
    'test_accuracy.py') -- compare against the 'tests/' + relpath form."""
    full = "tests/" + relpath
    return any(fnmatchmod.fnmatch(full, pat) for pat in glob_patterns)


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
    check_sh_path = os.path.join(repo, "scripts", "check.sh")
    makefile_path = os.path.join(repo, "Makefile")

    # scripts/check.sh counts as a runner too (it is what wires
    # tests/test_check_runner_registration.sh and
    # tests/test_results_evidence_ignore.sh in), and so does the root
    # Makefile's `check:` target (`@bash tests/test_check_lock.sh` -- found
    # by this guard flagging a real, wired-in test as an orphan because it
    # was the one place this guard had not looked). All required to exist,
    # for the same reason: cannot scan for orphans against a runner that is
    # not there to have registered anything.
    runner_paths = (
        ("tests/run_all.sh", run_all_path),
        ("tests/ci_smoke.sh", ci_smoke_path),
        ("scripts/check.sh", check_sh_path),
        ("Makefile", makefile_path),
    )
    for label, path in runner_paths:
        if not os.path.isfile(path):
            print(
                f"FAIL: runner-registration guard: {label} not found at {path} -- "
                "refusing to approve blind (cannot check membership against a "
                "runner that does not exist).",
                file=sys.stderr,
            )
            return 1

    try:
        with open(run_all_path, "r", encoding="utf-8") as f:
            run_all_raw = f.read()
        with open(ci_smoke_path, "r", encoding="utf-8") as f:
            ci_smoke_raw = f.read()
        with open(check_sh_path, "r", encoding="utf-8") as f:
            check_sh_raw = f.read()
        with open(makefile_path, "r", encoding="utf-8") as f:
            makefile_raw = f.read()
    except OSError as e:
        print(
            f"FAIL: runner-registration guard: could not read a runner file: {e} -- "
            "refusing to approve blind.",
            file=sys.stderr,
        )
        return 1

    run_all_clean = strip_full_line_comments(run_all_raw)
    ci_smoke_clean = strip_full_line_comments(ci_smoke_raw)
    check_sh_clean = strip_full_line_comments(check_sh_raw)
    makefile_clean = strip_full_line_comments(makefile_raw)

    ra_direct = extract_direct_invocations(run_all_clean)
    ra_live, live_tests_found = extract_live_tests_array(run_all_clean)

    errors = []
    unit_list_entries = extract_unit_tests_list_entries(repo, errors)
    if unit_list_entries is None:
        for e in errors:
            print(f"FAIL: runner-registration guard: {e}", file=sys.stderr)
        return 1

    ra_files = ra_direct | ra_live | unit_list_entries

    ci_files = extract_direct_invocations(ci_smoke_clean)

    check_direct, check_globs = extract_bare_tests_path_registrations(check_sh_clean)
    mk_direct, mk_globs = extract_bare_tests_path_registrations(makefile_clean)
    check_direct |= mk_direct
    check_globs += mk_globs

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
    if looks_nontrivial(check_sh_clean) and not (check_direct or check_globs):
        print(
            "FAIL: runner-registration guard: scripts/check.sh mentions "
            "python3/bash but this parser extracted zero invocations or globs "
            "from it -- the format drifted out from under this guard; refusing "
            "to approve blind.",
            file=sys.stderr,
        )
        return 1
    if looks_nontrivial(makefile_clean) and not (mk_direct or mk_globs):
        print(
            "FAIL: runner-registration guard: Makefile mentions python3/bash "
            "but this parser extracted zero invocations or globs from it -- "
            "the format drifted out from under this guard; refusing to "
            "approve blind.",
            file=sys.stderr,
        )
        return 1

    exemptions = read_exemptions(repo, errors)
    if exemptions is None:
        for e in errors:
            print(f"FAIL: runner-registration guard: {e}", file=sys.stderr)
        return 1
    if errors:
        for e in errors:
            print(f"FAIL: runner-registration guard: {e}", file=sys.stderr)
        return 1

    problems = []

    gaps = sorted(f for f in ci_files if f not in ra_files and f not in exemptions)
    if gaps:
        problems.append(
            (
                "tests/ci_smoke.sh invokes these test files but tests/run_all.sh "
                '(what `make box-check` actually runs) does not -- a box-check '
                '"green" proves nothing about them:',
                gaps,
                "  Either wire each one into tests/run_all.sh's LIVE_TESTS (same "
                "commit as whatever made it ci_smoke-only), or document why it is "
                "legitimately CI/nightly-only in "
                "tests/runner_registration_exemptions.list ('filename|reason', one "
                "per line).",
            )
        )

    # Orphan scan: a test file on disk invoked by NONE of the three runners
    # and not a documented exemption. Found twice for real while this guard
    # was being written (tests/test_check_runner_registration.sh and
    # tests/test_results_evidence_ignore.sh, before being wired into
    # scripts/check.sh).
    registered_anywhere = ra_files | ci_files | check_direct
    candidates = scan_candidate_test_files(repo)
    orphans = sorted(
        f
        for f in candidates
        if f not in registered_anywhere
        and not matches_any_glob(f, check_globs)
        and f not in exemptions
    )
    if orphans:
        problems.append(
            (
                "these test files exist on disk under tests/ but are invoked by "
                "NONE of tests/run_all.sh, tests/ci_smoke.sh, or scripts/check.sh "
                "(and are not covered by a glob either runner uses) -- nothing "
                "runs them, ever:",
                orphans,
                "  Wire each one into the runner it belongs to, or document why it "
                "is deliberately unregistered in "
                "tests/runner_registration_exemptions.list ('filename|reason', one "
                "per line).",
            )
        )

    if problems:
        for header, names, fix in problems:
            print(f"FAIL: runner-registration guard: {header}", file=sys.stderr)
            for f in names:
                print(f"    {f}", file=sys.stderr)
            print(fix, file=sys.stderr)
        return 1

    exempted_and_absent = sorted(
        f for f in exemptions if f not in ci_files and f not in ra_files and f not in check_direct
    )
    print(
        "OK: runner-registration guard -- every tests/ci_smoke.sh invocation "
        f"({len(ci_files)}) is also invoked by tests/run_all.sh ({len(ra_files)} "
        "invocations total, including tests/unit_tests.list entries); every "
        f"candidate test file on disk ({len(candidates)} scanned: "
        "tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a "
        "runner or covered by a glob; or is a documented exemption "
        f"({len(exemptions)} listed"
        + (f", {len(exempted_and_absent)} currently invoke no runner" if exempted_and_absent else "")
        + ")."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
