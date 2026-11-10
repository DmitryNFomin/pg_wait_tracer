#!/usr/bin/env python3
"""Offline #760 tests: execute the runner's real classifier and final summary.

Only this extracted harness supplies detector arguments; production has no
identity environment override. Every acceptance case has a source mutation
that its assertion must reject. No build, PostgreSQL, root, or eBPF is involved.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile


RUNNER = Path(__file__).with_name("run_all.sh").read_text()
ENTRY = "test_cross_validate_tiered|675|el8@4.18"
TEST = "test_cross_validate_tiered"
BASH = os.environ.get("PGWT_SELFTEST_BASH", "bash")  # harness only


def mutate(old, new):
    assert RUNNER.count(old) == 1, f"mutation must apply exactly once: {old!r}"
    return RUNNER.replace(old, new, 1)


def harness(source, extra=""):
    # These spans include the real table, startup validation/detection, run_test,
    # and every final gate condition. Never reproduce the classifier in Python.
    prefix = source[source.index("passed=0\n"):source.index("\nskip_test() {")]
    summary = source[source.index("# Summary\n"):]
    return "set -uo pipefail\n" + prefix + r'''
if [[ "$4" != "production" ]]; then
    known_failing_detect_platform "$1" "$2"
fi
REQUIRE_LIVE=0
tier_report() { :; }
tier_ok() { return 0; }
run_test test_cross_validate_tiered bash -c 'echo KF_EXECUTED; exit "$1"' fixture "$3"
''' + extra + "\n" + summary


def run(root, os_name, release, rc, source=RUNNER, production=False, env=None, extra=""):
    return subprocess.run(
        [BASH, "-c", harness(source, extra), "known-failing-selftest",
         str(root / os_name), release, str(rc),
         "production" if production else "fixture"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=dict(os.environ, **(env or {})), check=False)


def check_result(result, passed=0, failed=0, known=0, xpass=0, executed=1):
    lines = result.stdout.splitlines()
    records = [line[len("RUN_ALL_SUMMARY "):] for line in lines
               if line.startswith("RUN_ALL_SUMMARY ")]
    assert len(records) == 1, (result.stdout, result.stderr)
    summary = json.loads(records[0])
    assert result.returncode == (1 if failed else 0), result.stdout
    assert summary["ok"] == (not failed), summary
    assert summary["status"] == ("FAIL" if failed else "PASS"), summary
    assert (summary["passed"], summary["failed"], summary["known_failing_count"],
            summary["xpass"]) == (passed, failed, known, xpass), summary
    assert summary["executed"] == summary["total"] == executed, summary
    assert summary["refused"] == summary["skipped"] == summary["excluded"] == 0
    assert summary["issues"] == ([675] if known else []), summary
    expected = ([dict(test=TEST, issue=675, exit=0 if xpass else 1,
                      xpass=bool(xpass))] if known else [])
    assert summary["known_failing"] == expected, summary
    assert lines.count("KF_EXECUTED") == 1, "listed tests must execute exactly once"
    verdict = ("FAIL" if failed else
               "PASS with 1 known-failing (issues: #675)" if known else "PASS")
    assert lines[-1] == verdict, result.stdout
    if known:
        diagnostic = "UNEXPECTED PASS" if xpass else "KNOWN-FAILING"
        assert f"{diagnostic} (issue #675)" in result.stdout, result.stdout
        assert f"Known-failing: {TEST} (issue #675," in result.stdout
    else:
        assert "UNEXPECTED PASS" not in result.stdout, result.stdout
        assert "KNOWN-FAILING (" not in result.stdout, result.stdout
    return summary


def check_malformed(result):
    assert result.returncode == 2, (result.stdout, result.stderr)
    assert "malformed KNOWN_FAILING entry" in result.stderr, result.stderr
    assert "KF_EXECUTED" not in result.stdout, result.stdout
    assert "RUN_ALL_SUMMARY " not in result.stdout, result.stdout


def negative(label, assertion, result):
    try:
        assertion(result)
    except AssertionError:
        print(f"  PASS negative control: {label} mutant rejected")
    else:
        raise AssertionError(f"vacuous assertion: {label} mutant accepted")


with tempfile.TemporaryDirectory(prefix="pgwt-known-failing-") as temp:
    root = Path(temp)
    for name, contents in {
        "el8": 'ID=rocky\nID_LIKE="rhel centos fedora"\nVERSION_ID="8.10"\n',
        "el9": 'ID=rocky\nID_LIKE="rhel centos fedora"\nVERSION_ID="9.4"\n',
        "ubuntu": 'ID=ubuntu\nVERSION_ID="24.04"\nID_LIKE=debian\n',
        "rhel": 'ID=rhel\nVERSION_ID=8.10\n',
        "centos": 'ID=centos\nVERSION_ID=8\n',
        "almalinux": 'ID=almalinux\nVERSION_ID=8.10\n',
        "oracle": 'ID=ol\nID_LIKE=fedora\nVERSION_ID=8.10\n',
        "empty": '',
    }.items():
        (root / name).write_text(contents)

    # Pin precisely the authorized table; dropping the scope must break this
    # test as well as the Ubuntu failure case below.
    table = RUNNER.split("KNOWN_FAILING=(", 1)[1].split("\n)", 1)[0]
    entries = [line.strip().strip("'\"") for line in table.splitlines()
               if line.strip() and not line.lstrip().startswith("#")]
    assert entries == [ENTRY], entries
    assert "\nknown_failing_detect_platform\n" in RUNNER
    print(f"  PASS production table: {ENTRY}; detector called without arguments")

    unscoped = mutate(ENTRY, "test_cross_validate_tiered|675")
    cases = [
        ("in-scope failure", "el8", "4.18.0-553.el8.x86_64", 1,
         dict(known=1), mutate(ENTRY, "test_cross_validate_tiered|675|el9@4.18")),
        ("in-scope pass", "el8", "4.18.0-553.el8.x86_64", 0,
         dict(known=1, xpass=1), mutate("xpass=$((xpass + 1))", "xpass=$xpass")),
        ("out-of-scope failure", "ubuntu", "4.18.0", 1,
         dict(failed=1), unscoped),
        ("out-of-scope pass", "ubuntu", "6.8.0-31-generic", 0,
         dict(passed=1), unscoped),
    ]
    for label, os_name, release, rc, expected, mutant in cases:
        result = run(root, os_name, release, rc)
        summary = check_result(result, **expected)
        assert summary["os"] == os_name
        assert summary["kernel"] == ".".join(release.split(".")[:2])
        print(f"  PASS {label}: known-failing={summary['known_failing_count']}, "
              f"xpass={summary['xpass']}; {result.stdout.splitlines()[-1]}; exit={result.returncode}")
        negative(label, lambda r: check_result(r, **expected),
                 run(root, os_name, release, rc, source=mutant))

    malformed = mutate(ENTRY, ENTRY + ".0")
    check_malformed(run(root, "el8", "4.18.0", 0, source=malformed))
    print("  PASS malformed scope: refuses before execution; exit=2")
    negative("malformed scope", check_malformed,
             run(root, "el8", "4.18.0", 0,
                 source=malformed.replace("known_failing_validate || exit 2", ":")))

    for rc in (126, 127):
        result = run(root, "el8", "4.18.0", rc)
        check_result(result, failed=1)
        assert f"FAILED TO RUN (exit {rc})" in result.stdout
        print(f"  PASS exit {rc}: failed=1, known-failing=0; FAIL; exit=1")
        negative(f"exit {rc}", lambda r: check_result(r, failed=1),
                 run(root, "el8", "4.18.0", rc, source=mutate(
                     'if [[ "$rc" -eq 126 || "$rc" -eq 127 ]]; then', 'if false; then')))

    # Independent predicates, version boundary, EL family aliases, absent
    # identity, and optional-scope compatibility. An unknown OS cannot inherit
    # a caller's ID/ID_LIKE/VERSION_ID.
    for os_name, release in (("el9", "4.18.0"), ("el8", "5.14.0"),
                             ("el8", "4.180.0"), ("missing", "4.18.0"),
                             ("empty", "4.18.0"), ("el8", "unparseable")):
        check_result(run(root, os_name, release, 1, env={
            "ID": "rocky", "ID_LIKE": "rhel", "VERSION_ID": "8.10"}), failed=1)
    for alias in ("rhel", "centos", "almalinux", "oracle"):
        check_result(run(root, alias, "4.18.0-553", 1), known=1)
    for entry, os_name, release in (("test_cross_validate_tiered|675|el8", "el8", "5.14.0"),
                                     ("test_cross_validate_tiered|675|@4.18", "ubuntu", "4.18.0"),
                                     ("test_cross_validate_tiered|675", "ubuntu", "6.8.0")):
        check_result(run(root, os_name, release, 1, source=mutate(ENTRY, entry)), known=1)
    for bad in ("el8@", "@", "el8@4", "el8@4.18.0", "el8@@4.18", "el8|extra", ""):
        check_malformed(run(root, "ubuntu", "6.8.0", 0,
                            source=mutate(ENTRY, "test_cross_validate_tiered|675|" + bad)))
    print("  PASS boundaries: EL9, other kernels, OS aliases, missing identity, optional scopes, malformed inactive entries")

    mixed = run(root, "el8", "4.18.0", 1,
                extra="run_test new_el8_regression bash -c 'exit 1'")
    check_result(mixed, failed=1, known=1, executed=2)
    print("  PASS new EL8 failure alongside #675: known-failing=1, failed=1; FAIL; exit=1")

    # Production detection cannot be spoofed using any of its variable names
    # or os-release fields. Compare with a real native detection, no fixture
    # arguments, under opposite identities.
    # Empty table also exercises Bash 3.2's set -u array edge case, and makes
    # this detector check independent of the actual OS running the self-test.
    empty_table = mutate("    '" + ENTRY + "'\n", "")
    native = check_result(run(root, "missing", "ignored", 0,
                              source=empty_table, production=True), passed=1)

    def check_native_identity(result):
        observed = check_result(result, passed=1)
        assert (observed["os"], observed["kernel"]) == (native["os"], native["kernel"])

    for fake_os, fake_kernel in (("el8", "4.18"), ("ubuntu", "6.8")):
        spoof = run(root, "missing", "ignored", 0, source=empty_table, production=True, env={
            "KNOWN_FAILING_OS": fake_os, "KNOWN_FAILING_KERNEL": fake_kernel,
            "ID": "rocky", "ID_LIKE": "rhel", "VERSION_ID": "8.10",
            "OS": fake_os})
        check_native_identity(spoof)
    print("  PASS production environment spoofing: identity unchanged")
    negative("production environment spoofing", check_native_identity,
             run(root, "missing", "ignored", 0, production=True,
                 source=empty_table.replace("\nknown_failing_detect_platform\n", "\n:\n"),
                 env={"KNOWN_FAILING_OS": "impossible-env-os", "KNOWN_FAILING_KERNEL": "99.99"}))

print("test_run_all_known_failing: all cases and negative controls passed")
