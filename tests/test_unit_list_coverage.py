#!/usr/bin/env python3
"""Every C unit test that tests/Makefile BUILDS must also be RUN.

TST-3 was exactly this drift: test_event_writer and test_event_reader were
built by tests/Makefile and executed by nothing. A C unit test that is not in
tests/unit_tests.list is compiled by CI, linked, and then silently never run —
the purest false negative there is, because the suite stays green and the
binary looks maintained. Nothing caught that class of mistake; this does.

The rule: every `test_*` target in tests/Makefile's TESTS variable appears in
tests/unit_tests.list. Non-test targets (gen_test_traces, dump_markers,
cross_validate, wait_cpu_canary — generators and helper tools invoked by other
tests) are exempt by name prefix.

A checker that cannot see must REFUSE, never approve: an unreadable Makefile,
a TESTS variable it could not find, an empty parse result and an empty or
missing list each raise instead of returning "nothing missing". The negative
cases below drive every one of those paths, because "the guard was
unreachable" is how a guard fails in practice — not by missing a real hit.
"""

import os
import re
import sys

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
MAKEFILE = os.path.join(TEST_DIR, "Makefile")
LIST = os.path.join(TEST_DIR, "unit_tests.list")

# Built by tests/Makefile but deliberately not standalone unit tests: trace
# generators and helper tools other tests invoke.
NOT_A_UNIT_TEST_PREFIX = ("gen_", "dump_", "cross_", "wait_cpu_canary")


def parse_make_tests(text):
    """Names in the Makefile's `TESTS = ...` assignment (backslash-continued).

    Raises ValueError when there is no TESTS assignment or it yields nothing —
    a silent empty list would make every later comparison vacuously true.
    """
    m = re.search(r"^TESTS\s*=(.*?)(?<!\\)$", text, re.M | re.S)
    if not m:
        raise ValueError("no TESTS assignment in tests/Makefile")
    body = m.group(1).replace("\\\n", " ")
    names = [w for w in body.split() if w and not w.startswith("#")]
    if not names:
        raise ValueError("TESTS assignment parsed to zero targets")
    return names


def parse_unit_list(text):
    """Entries of unit_tests.list, comments and blanks dropped.

    Raises ValueError on an empty result: an empty list must never read as
    "everything is covered".
    """
    names = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        names.append(line)
    if not names:
        raise ValueError("unit_tests.list contains no entries")
    return names


def missing_entries(make_tests, listed):
    """C unit tests the Makefile builds that unit_tests.list never runs."""
    listed = set(listed)
    return sorted(
        t for t in make_tests
        if t.startswith("test_")
        and not t.startswith(NOT_A_UNIT_TEST_PREFIX)
        and t not in listed
    )


def _read(path):
    """Read a required input, or fail loudly — never fall back to ''."""
    if not os.path.exists(path):
        raise FileNotFoundError(path)
    with open(path) as fh:
        text = fh.read()
    if not text.strip():
        raise ValueError("%s is empty" % path)
    return text


def main():
    failures = 0

    def check(cond, msg):
        nonlocal failures
        if cond:
            print("  ok: %s" % msg)
        else:
            failures += 1
            print("  FAIL: %s" % msg)

    print("--- 1. the real tree: every built test_* is listed ---")
    make_tests = parse_make_tests(_read(MAKEFILE))
    listed = parse_unit_list(_read(LIST))
    gap = missing_entries(make_tests, listed)
    check(not gap,
          "no built-but-never-run unit test (missing: %s)" % (gap or "none"))
    check(len(make_tests) >= 20,
          "parsed %d Makefile targets (sanity: the parse found them)"
          % len(make_tests))
    check("test_window_clip" in listed,
          "test_window_clip is in unit_tests.list, so it actually runs")

    print("--- 2. the guard must REFUSE when it cannot see ---")
    for name, text, fn in (
        ("Makefile without a TESTS assignment", "all: foo\n", parse_make_tests),
        ("TESTS assignment with no targets", "TESTS =\n", parse_make_tests),
        ("unit_tests.list of only comments", "# nothing\n\n", parse_unit_list),
        ("empty unit_tests.list", "", parse_unit_list),
    ):
        try:
            fn(text)
        except ValueError:
            check(True, "refuses: %s" % name)
        else:
            check(False, "APPROVED a %s — it must raise" % name)

    for name, path in (("missing file", os.path.join(TEST_DIR, "no-such-file")),
                       ("empty file", os.devnull)):
        try:
            _read(path)
        except (FileNotFoundError, ValueError):
            check(True, "refuses to read a %s" % name)
        else:
            check(False, "APPROVED a %s — it must raise" % name)

    print("--- 3. the guard must DETECT a real omission ---")
    # The true-positive path, on synthetic input: a test built but unlisted.
    synth_make = "TESTS     = test_alpha test_beta gen_traces \\\n            test_gamma\n"
    synth_list = "# comment\ntest_alpha\ntest_gamma\n"
    got = missing_entries(parse_make_tests(synth_make), parse_unit_list(synth_list))
    check(got == ["test_beta"],
          "spots the unlisted test_beta (got %s)" % got)
    # ...and does not invent one when the list is complete.
    got = missing_entries(parse_make_tests(synth_make),
                          parse_unit_list("test_alpha\ntest_beta\ntest_gamma\n"))
    check(got == [], "reports nothing when the list is complete")
    # Non-test targets are exempt, not silently required.
    got = missing_entries(parse_make_tests("TESTS = gen_test_traces dump_markers\n"),
                          parse_unit_list("test_alpha\n"))
    check(got == [], "generators and helper tools are exempt by name")

    print("\n%s" % ("FAILED" if failures else "PASSED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
