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
ENTRY = "test_cross_validate_tiered|675|el8@4.18|exit=79"
TIERED = Path(__file__).with_name("test_cross_validate_tiered.sh").read_text()
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
run_test test_cross_validate_tiered bash -c 'echo KF_EXECUTED; [[ -z "$2" ]] || echo "TIERED_DEVIATIONS $2"; exit "$1"' fixture "$3" "$5"
''' + extra + "\n" + summary


def run(root, os_name, release, rc, source=RUNNER, production=False, env=None, extra="", deviations=""):
    return subprocess.run(
        [BASH, "-c", harness(source, extra), "known-failing-selftest",
         str(root / os_name), release, str(rc),
         "production" if production else "fixture", deviations],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=dict(os.environ, **(env or {})), check=False)


def check_result(result, passed=0, failed=0, known=0, xpass=0, executed=1, deviations=None):
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
    expected = ([dict(test=TEST, issue=675, exit=0 if xpass else 79,
                      xpass=bool(xpass))] if known else [])
    if known and deviations is not None:
        expected[0]["deviations_pp"] = deviations
        rendered = ", ".join(f"{rate}Hz={delta:.1f}" for rate, delta in deviations.items())
        assert f"deviations (pp): {rendered}" in result.stdout, result.stdout
    assert summary["known_failing"] == expected, summary
    assert lines.count("KF_EXECUTED") == 1, "listed tests must execute exactly once"
    verdict = ("FAIL" if failed else
               "PASS with 1 known-failing (issues: #675)" if known else "PASS")
    if known and not failed and deviations is not None:
        verdict += f"; deviations (pp): {TEST}[{rendered}]"
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
        ("in-scope expected exit 79", "el8", "4.18.0-553.el8.x86_64", 79,
         dict(known=1), mutate(ENTRY, "test_cross_validate_tiered|675|el9@4.18|exit=79")),
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

    # Exit 79 is reserved for a matching test AND scope, never a global pass.
    # Collect failures so an unlisted-exit mutant names every affected case.
    unexpected_79_failures = []
    for label, os_name, release, source in (
        ("out-of-scope exit 79", "ubuntu", "6.8.0-31-generic", RUNNER),
        ("unlisted exit 79", "el8", "4.18.0", mutate("    '" + ENTRY + "'\n", "")),
        ("different-test entry exit 79", "el8", "4.18.0",
         mutate(ENTRY, "another_test|675|el8@4.18|exit=79")),
    ):
        result = run(root, os_name, release, 79, source=source)
        try:
            summary = check_result(result, failed=1)
        except AssertionError:
            print(f"  FAIL {label}: {result.stdout}")
            unexpected_79_failures.append(label)
            continue
        print(f"  PASS {label}: failed={summary['failed']}, "
              f"known-failing={summary['known_failing_count']}; "
              f"{result.stdout.splitlines()[-1]}; exit={result.returncode}")
        old = 'elif [[ "$rc" -eq 0 ]]; then\n        passed=$((passed + 1))'
        new = 'elif [[ "$rc" -eq 0 || "$rc" -eq 79 ]]; then\n        passed=$((passed + 1))'
        assert source.count(old) == 1
        negative(label, lambda r: check_result(r, failed=1),
                 run(root, os_name, release, 79, source=source.replace(old, new, 1)))
    assert not unexpected_79_failures, unexpected_79_failures

    for rc in (1, 42):
        def check_mismatch(result):
            check_result(result, failed=1)
            assert f"exit {rc}, expected exit 79" in result.stdout, result.stdout
        result = run(root, "el8", "4.18.0", rc)
        check_mismatch(result)
        print(f"  PASS in-scope exit {rc}: actual {rc}, expected 79; FAIL; exit=1")
        negative(f"in-scope exit {rc}", check_mismatch,
                 run(root, "el8", "4.18.0", rc,
                     source=mutate('"$rc" -eq "$expected"', '"$rc" -ne 0')))

    measured = {"10": 9.8, "50": 10.1, "100": 11.1, "200": 12.4}
    def check_deviations(result):
        check_result(result, known=1, deviations=measured)
    check_deviations(run(root, "el8", "4.18.0", 79,
                         deviations="10=9.8 50=10.1 100=11.1 200=12.4"))
    print("  PASS measured deviations: human summary + RUN_ALL_SUMMARY carry all rates")
    negative("measured deviations", check_deviations,
             run(root, "el8", "4.18.0", 79, deviations="10=9.8"))

    malformed = mutate(ENTRY, ENTRY.replace("4.18", "4.18.0"))
    check_malformed(run(root, "el8", "4.18.0", 0, source=malformed))
    print("  PASS malformed scope: refuses before execution; exit=2")
    negative("malformed scope", check_malformed,
             run(root, "el8", "4.18.0", 0,
                 source=malformed.replace("known_failing_validate || exit 2", ":")))

    for rc in (126, 127):
        result = run(root, "el8", "4.18.0", rc)
        check_result(result, failed=1)
        assert f"FAILED TO RUN (exit {rc})" in result.stdout
        assert "expected exit 79" in result.stdout
        print(f"  PASS exit {rc}: failed=1, known-failing=0; FAIL; exit=1")
        negative(f"exit {rc}", lambda r: check_result(r, failed=1),
                 run(root, "el8", "4.18.0", rc, source=mutate(
                     'if [[ "$rc" -eq 126 || "$rc" -eq 127 ]]; then', 'if false; then').replace(
                         '"$rc" -eq "$expected"', '"$rc" -ne 0')))

    # Independent predicates, version boundary, EL family aliases, absent
    # identity, and optional-scope compatibility. An unknown OS cannot inherit
    # a caller's ID/ID_LIKE/VERSION_ID.
    for os_name, release in (("el9", "4.18.0"), ("el8", "5.14.0"),
                             ("el8", "4.180.0"), ("missing", "4.18.0"),
                             ("empty", "4.18.0"), ("el8", "unparseable")):
        check_result(run(root, os_name, release, 1, env={
            "ID": "rocky", "ID_LIKE": "rhel", "VERSION_ID": "8.10"}), failed=1)
    for alias in ("rhel", "centos", "almalinux", "oracle"):
        check_result(run(root, alias, "4.18.0-553", 79), known=1)
    for entry, os_name, release in (("test_cross_validate_tiered|675|el8|exit=79", "el8", "5.14.0"),
                                     ("test_cross_validate_tiered|675|@4.18|exit=79", "ubuntu", "4.18.0"),
                                     ("test_cross_validate_tiered|675", "ubuntu", "6.8.0")):
        check_result(run(root, os_name, release, 79, source=mutate(ENTRY, entry)), known=1)
    for bad in ("el8@", "@", "el8@4", "el8@4.18.0", "el8@@4.18", "el8|extra", ""):
        check_malformed(run(root, "ubuntu", "6.8.0", 0,
                            source=mutate(ENTRY, "test_cross_validate_tiered|675|" + bad)))
    for bad in ("el8@4.18", "el8@4.18|exit=0", "el8@4.18|exit=78",
                "el8@4.18|exit=126", "el8@4.18|exit=127", "el8@4.18|exit=130",
                "el8@4.18|exit=9999999999999999999999999"):
        check_malformed(run(root, "el8", "4.18.0", 79,
                            source=mutate(ENTRY, "test_cross_validate_tiered|675|" + bad)))
    print("  PASS boundaries: EL9, other kernels, OS aliases, missing identity, optional scopes, malformed inactive entries")

    mixed = run(root, "el8", "4.18.0", 79,
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
            "OS": fake_os, "PGWT_KERNEL": fake_kernel,
            "PGWT_OS": fake_os, "PGWT_OS_RELEASE": str(root / fake_os)})
        check_native_identity(spoof)
    print("  PASS production environment spoofing: identity unchanged")
    negative("production environment spoofing", check_native_identity,
             run(root, "missing", "ignored", 0, production=True,
                 source=empty_table.replace("\nknown_failing_detect_platform\n", "\n:\n"),
                 env={"KNOWN_FAILING_OS": "impossible-env-os", "KNOWN_FAILING_KERNEL": "99.99"}))

    # Kill the exact environment hook missed in round 1, even with a native
    # empty-table run: inspecting the emitted identity catches the spoof itself.
    spoof_kernel = "99.99"
    negative("PGWT_KERNEL production hook", check_native_identity,
             run(root, "missing", "ignored", 0, production=True,
                 source=empty_table.replace('release="${2:-$(uname -r)}"',
                                            'release="${2:-${PGWT_KERNEL:-$(uname -r)}}"'),
                 env={"PGWT_KERNEL": spoof_kernel}))

def tiered_run(function, args, source=TIERED):
    functions = source[source.index("tiered_classify() {"):source.index("\nwhile [[ $#")]
    return subprocess.run([BASH, "-c", "set -uo pipefail\n" + functions +
                           f'\n{function} "$@"', "tiered-selftest", *args],
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


def check_exit(result, expected):
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)


decision_cases = [
    ("all within band and under-reading", ["0", "8.8", "9.8,-4.0", "10.1", "11.1", "12.4"], 79,
     "sys.exit(79 if outside else 0)", "sys.exit(1 if outside else 0)"),
    ("one above ceiling", ["0", "8.8", "10.1", "15.1"], 1,
     "peak > 15.0", "peak > 150.0"),
    ("one over-reading beyond tolerance", ["0", "8.8", "12.4,-9.0"], 1,
     "if delta < 0:", "if delta < -150:"),
    ("non-tolerance failure present", ["1", "8.8", "10.1"], 1,
     'other_failure != "0"', 'other_failure not in ("0", "1")'),
    ("nothing failing", ["0", "8.8", "3.0,-2.0", "8.8"], 0,
     "sys.exit(79 if outside else 0)", "sys.exit(79 if outside else 1)"),
]
for label, args, expected, old, new in decision_cases:
    result = tiered_run("tiered_classify", args)
    check_exit(result, expected)
    print(f"  PASS tiered decision {label}: exit={result.returncode}")
    assert TIERED.count(old) == 1
    negative("tiered decision " + label, lambda r: check_exit(r, expected),
             tiered_run("tiered_classify", args, source=TIERED.replace(old, new, 1)))

for arms, expected in ((["15.0"], 79), (["8.8,-8.8"], 0), ([], 1),
                       (["INVALID"], 1), (["nan"], 1), (["inf"], 1), ([""], 1)):
    check_exit(tiered_run("tiered_classify", ["0", "8.8", *arms]), expected)
print("  PASS tiered boundaries: 15.0 ceiling, 8.8 tolerance, absent/invalid/nonfinite arms")

# Also run the live script's actual final call/exit mapping, feeding synthetic
# arms without running its PostgreSQL/capture loop. This catches a disconnected
# classifier or a caller that drops an arm or the non-tolerance failure flag.
def tiered_summary(arms, other_failure="0", source=TIERED):
    functions = source[source.index("tiered_classify() {"):source.index("\nwhile [[ $#")]
    final = source[source.index('echo "===== Cross-validation summary ====="'):]
    setup = '''
TOLERANCE=8.8
best_rate=""
RATES="10 50"
non_tolerance_failure="$1"
declare -A rate_result rate_delta rate_event rate_assertions
rate_assertions[10]="$2"
rate_assertions[50]="$3"
rate_delta[10]=10.1
rate_delta[50]=11.1
print_rate_summary() { :; }
'''
    return subprocess.run([BASH, "-c", "set -uo pipefail\n" + functions + setup + final,
                           "tiered-summary-selftest", other_failure, *arms],
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


# Associative arrays are a live-test prerequisite (EL8 bash 4.4); the extracted
# pure functions and runner also run under macOS /bin/bash 3.2 above.
modern_bash = subprocess.run([BASH, "-c", "declare -A probe"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
if modern_bash:
    for label, arms, other, expected, old, new in (
        ("bounded signature", ["10.1", "11.1"], "0", 79, "exit 79", "exit 1"),
        ("missing arm", ["10.1", ""], "0", 1, '${rate_assertions[$RATE]:-INVALID}', '${rate_assertions[$RATE]:-10.1}'),
        ("validity failure", ["10.1", "11.1"], "1", 1, 'tiered_classify "$non_tolerance_failure"', 'tiered_classify "0"'),
        ("all pass", ["3.0", "4.0"], "0", 0, "exit 0", "exit 1"),
    ):
        result = tiered_summary(arms, other)
        check_exit(result, expected)
        assert "TIERED_DEVIATIONS 10=10.1 50=11.1" in result.stdout
        print(f"  PASS tiered final wiring {label}: exit={expected}; deviations emitted")
        negative("tiered final wiring " + label, lambda r: check_exit(r, expected),
                 tiered_summary(arms, other, source=TIERED.replace(old, new)))

# cross_validate's real text format and all of its assertions feed the pure
# decision. In particular a second, non-maximal over-read cannot be hidden by
# the max-disagreement headline, and an overlap failure cannot be waived.
comparator = """wait_event                    exact_share%     sampled%   delta_pp
------------------------------------------------------------------
CPU                                 50.0%        39.0%      11.0
IO:DataFileRead                      25.0%        30.5%       5.5
LWLock:BufferContent                25.0%        30.5%       5.5

Samples in window: 3200
Top-3 event overlap: 3/3
Max share disagreement (events >= 2% exact share): 11.0 pp (CPU)
Tolerance: +/- 8.8 pp

RESULT: FAIL
"""


def check_arm(result, expected):
    check_exit(result, 0)
    assert result.stdout.strip() == expected, (result.stdout, result.stderr)


parsed = tiered_run("tiered_arm_assertions", [comparator, "1", "8.8"])
check_arm(parsed, "11.0,-5.5,-5.5")
check_exit(tiered_run("tiered_classify", ["0", "8.8", parsed.stdout.strip()]), 79)
print("  PASS tiered parser: all eligible signed deltas feed exit=79")
negative("tiered parser direction", lambda r: check_arm(r, "11.0,-5.5,-5.5"),
         tiered_run("tiered_arm_assertions", [comparator, "1", "8.8"],
                    source=TIERED.replace("delta if exact >= sampled else -delta", "delta")))

for label, output, rc, old, new in (
    ("overlap failure", comparator.replace("3/3", "2/3"), "1",
     "min(top, 4) <= overlap <= top", "0 <= overlap <= top"),
    ("tool error", comparator, "2", 'require(rc == ("0" if passed else "1"))', "require(True)"),
    ("missing data", comparator.replace("25.0%", "absent"), "1",
     "require(row)", "if not row: continue"),
    ("inconsistent verdict", comparator.replace("RESULT: FAIL", "RESULT: PASS"), "1",
     'require(verdicts[0] == ("PASS" if passed else "FAIL"))', "require(True)"),
):
    result = tiered_run("tiered_arm_assertions", [output, rc, "8.8"])
    check_arm(result, "INVALID")
    print(f"  PASS tiered parser {label}: INVALID -> exit=1")
    check_exit(tiered_run("tiered_classify", ["0", "8.8", result.stdout.strip()]), 1)
    assert TIERED.count(old) == 1
    negative("tiered parser " + label, lambda r: check_arm(r, "INVALID"),
             tiered_run("tiered_arm_assertions", [output, rc, "8.8"],
                        source=TIERED.replace(old, new, 1)))

print("test_run_all_known_failing: all cases and negative controls passed")
