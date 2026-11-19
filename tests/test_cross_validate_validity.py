#!/usr/bin/env python3
"""Host-only replay of the live per-rate decisions, summary and runner.

The real K1-10 comparator artifact below is evidence of the false PASS, not
an expected numeric oracle. Expected workload dispositions and boundary
outcomes are pre-registered in the dispatch brief. No PostgreSQL or BPF.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "tests/test_cross_validate_tiered.sh").read_text()
BASH = os.environ.get("PGWT_SELFTEST_BASH", "bash")
# Verbatim xval-env/tps2/K1.cmp.10, 2026-11-19: exact CPU 17.1%, PASS 2.0 pp.
K1 = """Overlap window: 63.0s (sampled + exact both present)

wait_event                   exact_share%     sampled%   delta_pp
------------------------------------------------------------------
WALWrite                            43.7%        45.3%       1.6
transactionid                       22.9%        22.8%       0.1
CPU                                 17.1%        15.1%       2.0
WalSync                             12.7%        12.9%       0.3
tuple                                2.5%         2.7%       0.2
DataFileFlush                        0.5%         0.5%       0.0
WalWrite                             0.2%         0.2%       0.0
DataFileWrite                        0.2%         0.2%       0.0
DataFileRead                         0.1%         0.1%       0.0
LockManager                          0.0%         0.0%       0.0
SpinDelay                            0.0%         0.0%       0.0
WALInsert                            0.0%         0.0%       0.0
BufferContent                        0.0%         0.0%       0.0
DataFileExtend                       0.0%         0.0%       0.0
CheckpointDelayStart                 0.0%         0.0%       0.0
AioIoCompletion                      0.0%         0.0%       0.0
XactSLRU                             0.0%         0.0%       0.0
ProcArray                            0.0%         0.0%       0.0
XidGen                               0.0%         0.0%       0.0
SlruWrite                            0.0%         0.0%       0.0
XactGroupUpdate                      0.0%         0.0%       0.0
WALBufMapping                        0.0%         0.0%       0.0
BufferMapping                        0.0%         0.0%       0.0
CheckpointerComm                     0.0%         0.0%       0.0
extend                               0.0%         0.0%       0.0
ProcarrayGroupUpdate                 0.0%         0.0%       0.0
AioWorkerSubmissionQueue             0.0%         0.0%       0.0
RelationMapRead                      0.0%         0.0%       0.0
DataFilePrefetch                     0.0%         0.0%       0.0

Samples in window: 4402
Top-5 event overlap: 5/5
Max share disagreement (events >= 2% exact share): 2.0 pp (CPU)
Tolerance: +/- 8.8 pp
On-CPU-during-wait (spin): 1.9673% of wait time — exact on-CPU spin, attributed to its wait class (informational)

RESULT: PASS
"""


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def functions(source):
    return source[source.index("tiered_classify() {"):source.index("\nwhile [[ $#")]


def progress(values, first=5):
    return "".join(f"progress: {first + 5*i:.1f} s, {v:.1f} tps, lat 1.0 ms stddev 0.1, 0 failed\n"
                   for i, v in enumerate(values))


NORMAL = progress([1000] * 14)
STOP = progress([1000, 1000, 1000, 1000, 1000, 0, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000])
# Actual K1-10 progress TPS; the first 5 s interval straddles warm-up.
K1_PROGRESS = progress([3304.0, 2599.4, 2589.6, 2595.4, 2510.8, 2557.6,
                        751.0, 107.6, 117.8, 101.2, 114.2, 64.0, 172.6])


def run_rates(root, arms, source=SOURCE):
    """Run the production compare block and final exit mapping, with files as input."""
    for rate, (output, rc, pgb) in zip((10, 50, 100, 200), arms):
        (root / f"{rate}.out").write_text(output)
        (root / f"{rate}.rc").write_text(str(rc))
        (root / f"{rate}.pgb").write_text(pgb)
    loop = source[source.index("    # Compare\n"):source.index('\ndone\n\necho ""\necho "===== Cross-validation summary')]
    summary = source[source.index('echo "===== Cross-validation summary ====="'):]
    setup = r'''
set -uo pipefail
source "$2/tests/cross_validate_rate_output.sh"
source "$2/tests/xval_tolerance.sh"
TOLERANCE=8.8
RATES="$3"
best_rate=""
non_tolerance_failure=0
declare -A rate_result rate_delta rate_event rate_assertions rate_invalid
XVAL=fixture_xval
fixture_xval() { cat "$1/fixture.out"; return "$(cat "$1/fixture.rc")"; }
for RATE in $RATES; do
    TRACE_DIR=$(mktemp -d "$1/run.XXXXXX")
    LOG="$TRACE_DIR/daemon.log"
    PGB_LOG="$TRACE_DIR/pgbench.log"
    cp "$1/$RATE.out" "$TRACE_DIR/fixture.out"
    cp "$1/$RATE.rc" "$TRACE_DIR/fixture.rc"
    cp "$1/$RATE.pgb" "$PGB_LOG"
    ESC_START=3
    ESC_END=63
'''
    rates = " ".join(map(str, (10, 50, 100, 200)[:len(arms)]))
    return subprocess.run([BASH, "-c", functions(source) + setup + loop + "\ndone\n" + summary,
                           "validity-host", str(root), str(ROOT), rates],
                          capture_output=True, text=True, check=False)


def expect(result, rc, contains):
    check(result.returncode == rc, f"expected exit {rc}, got {result.returncode}\n{result.stdout}\n{result.stderr}")
    for token in contains:
        check(token in result.stdout, f"missing {token!r}\n{result.stdout}")
    if rc == 80:
        check("PASS" not in result.stdout, f"invalid workload printed PASS\n{result.stdout}")
        check("within tolerance" not in result.stdout, f"invalid workload advertised tolerance success\n{result.stdout}")


def build_formatter(root):
    """Compile the production printf formats, exercising C's actual rounding."""
    c_source = (ROOT / "tests/cross_validate.c").read_text()
    row = re.search(r'printf\("(%-28s [^"\n]+)"', c_source)[1]
    maximum = re.search(r'printf\("(Max share disagreement [^"\n]+)"\s*"([^"\n]+)"', c_source)
    check(maximum is not None, "comparator maximum printf was not found")
    c = r'''
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
int main(int argc, char **argv) {
    double max_delta = 0;
    const char *event = "n/a";
    puts("wait_event exact_share% sampled% delta_pp");
    puts("------------------------------------------------------------------");
    int rows = (argc - 1) / 3;
    for (int i = 1; i < argc; i += 3) {
        double ex = strtod(argv[i+1], NULL), sm = strtod(argv[i+2], NULL);
        double d = fabs(ex - sm);
        printf(ROW_FORMAT, argv[i], ex, sm, d);
        if (ex >= 2 && d > max_delta) { max_delta = d; event = argv[i]; }
    }
    printf("\nSamples in window: 4402\nTop-%d event overlap: %d/%d\n", rows, rows, rows);
    printf(MAX_FORMAT, 2.0, max_delta, event);
    puts("Tolerance: +/- 8.8 pp\n");
    int rc = max_delta <= 8.8 ? 0 : 1;
    printf("RESULT: %s\n", rc ? "FAIL" : "PASS");
    return rc;
}
'''.replace("ROW_FORMAT", '"' + row + '"').replace("MAX_FORMAT", '"' + maximum[1] + maximum[2] + '"')
    path = root / "format.c"
    path.write_text(c)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path), "-o", str(root / "format")], check=True)

    def fixture(rows):
        result = subprocess.run([str(root / "format"), *[str(v) for row in rows for v in row]],
                                capture_output=True, text=True, check=False)
        check(result.returncode in (0, 1), result.stderr)
        check(re.search(r"\s[0-9]+\.[0-9]{2}$", result.stdout, re.M), "table delta must have two decimals")
        check(re.search(r"disagreement .*: [0-9]+\.[0-9]{2} pp", result.stdout), "headline must have two decimals")
        return result.stdout, result.returncode, NORMAL
    return fixture


def runner_accepts_invalid(root):
    """Execute run_all's real known-failing and ordinary-nonzero paths."""
    runner = (ROOT / "tests/run_all.sh").read_text()
    prefix = runner[runner.index("passed=0\n"):runner.index("\nskip_test() {")]
    for platform, release in (("el8", "4.18.0"), ("ubuntu", "6.8.0")):
        os_file = root / "os-release"
        os_file.write_text("ID=" + ("rocky" if platform == "el8" else platform) + "\nVERSION_ID=" + ("8.10" if platform == "el8" else "24.04") + "\n")
        command = prefix + r'''
known_failing_detect_platform "$1" "$2"
PHASE_REFUSED_MARKER=fixture
phase_dependent_test() { return 1; }
run_test test_cross_validate_tiered bash -c 'exit 80'
[[ "$failed" == 1 && "$known_failing" == 0 && "$refused" == 0 ]]
'''
        result = subprocess.run([BASH, "-c", "set -uo pipefail\n" + command, "runner-validity", str(os_file), release],
                                capture_output=True, text=True, check=False)
        check(result.returncode == 0, result.stdout + result.stderr)
        if platform == "el8":
            check("exit 80, expected exit 79" in result.stdout, result.stdout)
        print(f"PASS run_all {platform}: exit 80 counts as real failure")


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-xval-validity-") as directory:
        root = Path(directory)
        fixture = build_formatter(root)
        normal = fixture([("CPU", 60, 56), ("WALWrite", 20, 22), ("transactionid", 20, 22)])
        below_cpu = fixture([("CPU", 17.1, 15.1), ("WALWrite", 41.45, 42.45), ("transactionid", 41.45, 42.45)])
        boundary = fixture([("CPU", 60, 51.2), ("WALWrite", 20, 24.4), ("transactionid", 20, 24.4)])
        fail = fixture([("CPU", 60, 51.17), ("WALWrite", 20, 24.415), ("transactionid", 20, 24.415)])
        negative = fixture([("CPU", 50, 59), ("WALWrite", 25, 20.5), ("transactionid", 25, 20.5)])
        cases = [
            ("K1-10 real comparator and real stalled progress", [(K1, 0, K1_PROGRESS)], 80, ["RATE 10Hz: INVALID — b':", "assertions[10]=WORKLOAD_INVALID xval_rc=0"]),
            ("low exact CPU independently", [below_cpu], 80, ["RATE 10Hz: INVALID — c:", "< 30%"]),
            ("K1-10 low CPU with healthy progress", [(K1, 0, NORMAL)], 80, ["RATE 10Hz: INVALID — c: exact CPU share=17.1%"]),
            ("SIGSTOP-style flat-zero interval", [(normal[0], 0, STOP)], 80, ["RATE 10Hz: INVALID — b':", "0.0"]),
            ("8.83 at 10Hz, other rates pass", [fail, normal, normal, normal], 79, ["assertions[10]=", "xval_rc=1", "TIERED_DEVIATIONS 10=8.83", "exit 79"]),
            ("8.80 exactly", [boundary], 0, ["PASS: every requested rate", "xval_rc=0"]),
            ("normal workload", [normal], 0, ["PASS: every requested rate"]),
            ("negative delta beyond tolerance", [negative], 1, ["xval_rc=1", "FAIL: not every requested rate"]),
        ]
        # Closer than two decimals: use true C printf inputs, not the rounded
        # human deltas, to distinguish both signs and eligibility at 2%.
        for sample, rc in ((51.199, 79), (51.201, 0)):
            arm = fixture([("CPU", 60, sample), ("WALWrite", 20, (100-sample)/2), ("transactionid", 20, (100-sample)/2)])
            cases.append((f"sub-cent boundary sample={sample}", [arm], rc, ["xval_rc="]))
        cases.append(("ineligible 1.999 share rounds to 2.00", [fixture([("CPU", 60, 60), ("WALWrite", 38.001, 28.001), ("tiny", 1.999, 11.999)])], 79, ["xval_rc=1"]))
        for label, arms, rc, tokens in cases:
            expect(run_rates(root, arms), rc, tokens)
            print(f"PASS {label}: exit {rc}")
        negative_subcent = fixture([("CPU", 60, 48), ("WALWrite", 25, 33.801), ("transactionid", 15, 18.199)])
        expect(run_rates(root, [negative_subcent]), 1, ["xval_rc=1"])
        print("PASS non-max negative delta 8.801: exit 1")
        cpu_boundary = fixture([("CPU", 30, 30), ("WALWrite", 35, 35), ("transactionid", 35, 35)])
        expect(run_rates(root, [cpu_boundary]), 0, ["PASS: every requested rate"])
        print("PASS CPU 30% exactly: exit 0")
        # Boundary semantics: 50% is inclusive, only complete in-window
        # intervals enter the median, CPU 30% is inclusive, missing evidence refuses.
        for label, arm, rc, tokens in (
            ("exactly half median is valid", (normal[0], 0, progress([1000]*6 + [500] + [1000]*7)), 0, ["PASS: every requested rate"]),
            ("partial/outside-window stalls excluded", (normal[0], 0, progress([0] + [1000]*11 + [0, 0])), 0, ["PASS: every requested rate"]),
            ("progress missing", (normal[0], 0, ""), 80, ["INVALID — b': no complete"]),
            ("CPU missing", (normal[0].replace("CPU ", "Other "), 0, NORMAL), 80, ["INVALID — c:"]),
            ("sample count guard retained", (normal[0].replace("4402", "1"), 0, NORMAL), 1, ["REFUSED — capture too thin"]),
            ("overlap guard retained", (normal[0].replace("3/3", "2/3").replace("RESULT: PASS", "RESULT: FAIL"), 1, NORMAL), 1, ["assertions[10]=INVALID xval_rc=1"]),
        ):
            expect(run_rates(root, [arm]), rc, tokens)
            print(f"PASS {label}: exit {rc}")
        mutants = [
            ("drop rule b'", "if stalled:", "if False:", [(normal[0], 0, STOP)], 80, ["INVALID — b':"]),
            ("drop rule c", "elif float(cpu[0]) < 30:", "elif False:", [below_cpu], 80, ["INVALID — c:"]),
            ("revert to 1-decimal parsing", "[0-9]{1,2}", "[0-9]{1}", [fail, normal, normal, normal], 79, ["exit 79"]),
            ("make INVALID exit 0", "sys.exit(80)", "sys.exit(0)", [(normal[0], 0, STOP)], 80, ["INVALID — b':"]),
        ]
        for label, old, new, arms, rc, tokens in mutants:
            check(old in SOURCE, "mutation anchor missing: " + old)
            result = run_rates(root, arms, SOURCE.replace(old, new))
            check("Traceback" not in result.stderr and "SyntaxError" not in result.stderr, "mutant failed before its target case: " + result.stderr)
            try:
                expect(result, rc, tokens)
            except AssertionError:
                print(f"MUTANT RED {label}: actual exit {result.returncode}, required {rc}")
            else:
                raise AssertionError("surviving mutant: " + label)
        # Existing permissive consumers must retain the new two-decimal text.
        extracted = subprocess.run([BASH, "-c", 'source "$1/tests/cross_validate_rate_output.sh"; extract_max_delta "$2"',
                                    "extract-host", str(ROOT), fail[0]], capture_output=True, text=True, check=True)
        check(extracted.stdout.strip() == "8.83|CPU", extracted.stdout)
        print("PASS extract_max_delta: 8.83|CPU")
        runner_accepts_invalid(root)
    print("test_cross_validate_validity: all cases and required mutants passed")


if __name__ == "__main__":
    main()
