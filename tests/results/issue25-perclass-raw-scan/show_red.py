#!/usr/bin/env python3
"""Show what RED looks like for every guard in tests/wait_cpu_scan.c.

Each mutant disables ONE guard in a COPY of the scanner source, rebuilds it,
and runs the bypass suite against the mutant. The named case must go red —
a guard whose removal changes nothing is not a guard. Logs land in
tests/results/issue25-perclass-raw-scan/red/.

Run: python3 tests/results/issue25-perclass-raw-scan/show_red.py
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SRC = os.path.join(ROOT, "tests", "wait_cpu_scan.c")
RED = os.path.join(HERE, "red")

MUTANTS = {
    # name: (list of (old, new) exact replacements, cases that must go red)
    "M1_no_coverage_cross_check": (
        [('if (no_expect_check) {\n        REFUSE("coverage_unverified:expect_check_disabled");',
          'if (no_expect_check) {\n        /* MUTANT: guard removed */ (void)0;'),
         ('if (expect_transitions < 0) REFUSE("coverage_unverified:no_expect_transitions");',
          'if (expect_transitions < 0) (void)0;'),
         ('if (expect_samples < 0) REFUSE("coverage_unverified:no_expect_samples");',
          'if (expect_samples < 0) (void)0;')],
        ["no_expected_total_refuses", "disabled_coverage_check_refuses"]),
    "M2_unknown_cpu_as_zero": (
        [('if (e->cpu_ns == PGWT_CPU_NS_UNKNOWN) { k->unknown++; continue; }\n'
          '                k->measured++;\n'
          '                k->sum_cpu += e->cpu_ns;',
          'uint64_t _cpu = e->cpu_ns == PGWT_CPU_NS_UNKNOWN ? 0 : e->cpu_ns;\n'
          '                k->measured++;\n'
          '                k->sum_cpu += _cpu;  /* MUTANT: unknown read as 0 */')],
        ["unknown_cpu_is_unmeasured_not_clean", "unknown_cpu_counted_not_zeroed"]),
    "M3_unmeasured_class_reads_clean": (
        [('else if (num_unmeasured) { verdict = "UNMEASURED"; rc = 4; }',
          'else if (0) { verdict = "UNMEASURED"; rc = 4; }  /* MUTANT */')],
        ["unknown_cpu_is_unmeasured_not_clean",
         "absent_class_is_unmeasured_not_clean"]),
    "M4_decode_failure_ignored": (
        [('REFUSE("block_decode_failed:file%d:block%d", fi, b);',
          '/* MUTANT */ (void)0;'),
         ('REFUSE("block_header_unreadable:file%d:block%d", fi, b);',
          '/* MUTANT */ (void)0;'),
         ('REFUSE("trace_file_unreadable:%s", files[fi].path);',
          '/* MUTANT */ (void)0;'),
         ('REFUSE("record_shortfall:walked=%" PRIu64 ":headers=%" PRIu64,\n'
          '               records_walked, header_records);',
          '/* MUTANT */ (void)0;')],
        ["truncated_trace_refuses", "unreadable_trace_file_refuses"]),
    "M5_unresolvable_name_ignored": (
        [('if (unresolved_records)\n'
          '        REFUSE("unresolved_event_names:%" PRIu64, unresolved_records);',
          '/* MUTANT: an unnameable class is silently bucketed */ (void)0;')],
        ["unresolvable_name_refuses"]),
    "M6_no_crossing_detection": (
        [('if (over_abs && ratio_met) {',
          'if (0) {  /* MUTANT: crossing detection removed */')],
        ["crossing_detected", "crossing_carries_full_identity",
         "zero_duration_cpu_is_not_cleared_by_the_ratio_half",
         "detection_survives_the_refusal_cases"]),
    "M7_empty_input_reads_clean": (
        [('REFUSE("no_trace_files");', '/* MUTANT */ (void)0;'),
         ('if (records_walked == 0) REFUSE("no_records_walked");',
          '/* MUTANT */ (void)0;'),
         ('if (wait_records == 0) REFUSE("no_wait_records");',
          '/* MUTANT */ (void)0;')],
        ["empty_dir_refuses", "marker_only_trace_refuses"]),
    "M8_partial_range_accepted": (
        [('else if ((uint64_t)expect_transitions != transitions_total + markers)\n'
          '            REFUSE("transition_census_mismatch:walked=%" PRIu64 ":expected=%lld",\n'
          '                   transitions_total + markers, expect_transitions);',
          'else if (0) (void)0;  /* MUTANT: census mismatch accepted */'),
         ('else if ((uint64_t)expect_samples != samples)\n'
          '            REFUSE("sample_census_mismatch:walked=%" PRIu64 ":expected=%lld",\n'
          '                   samples, expect_samples);',
          'else if (0) (void)0;  /* MUTANT */')],
        ["census_mismatch_refuses", "sample_census_mismatch_refuses"]),
    # The §3 conjunction's own two blinding paths (added with it).
    "M9_zero_duration_cleared": (
        [('                    : e->cpu_ns > 0;',
          '                    : 0;  /* MUTANT: no ratio -> treated clean */')],
        ["zero_duration_cpu_is_not_cleared_by_the_ratio_half"]),
    "M10_unreachable_ratio_accepted": (
        [('if (!(min_ratio_pct >= 0.0 && min_ratio_pct <= 100.0))\n'
          '        REFUSE("min_ratio_unreachable:%g", min_ratio_pct);',
          '/* MUTANT: a bar no record can reach is accepted */ (void)0;')],
        ["unreachable_min_ratio_refuses", "nan_min_ratio_refuses"]),
}


def main():
    os.makedirs(RED, exist_ok=True)
    original = open(SRC).read()
    overall = 0
    for name, (edits, must_fail) in MUTANTS.items():
        tmp = tempfile.mkdtemp(prefix=f"mutant_{name}_")
        mutated = original
        for old, new in edits:
            # Already here, and load-bearing for the conjunction change
            # below: M6's old anchor (`if (e->cpu_ns >= threshold) {`) no
            # longer exists, and without this assert that mutant would have
            # silently become the UNMUTATED scanner -- a red log proving
            # nothing. Verified by running a copy of this file with the
            # stale anchor: AssertionError, exit 1, no log written.
            assert old in mutated, f"{name}: anchor not found:\n{old}"
            mutated = mutated.replace(old, new, 1)
        msrc = os.path.join(tmp, "wait_cpu_scan.c")
        with open(msrc, "w") as f:
            f.write(mutated)
        stub = os.path.join(tmp, "proc_stub.c")
        with open(stub, "w") as f:
            f.write('#include <errno.h>\n#include "spawn.h"\n'
                    'int pgwt_proc_open(struct pgwt_proc *p, char *const a[])'
                    '{(void)p;(void)a;errno=ENOSYS;return -1;}\n'
                    'int pgwt_proc_close(struct pgwt_proc *p)'
                    '{(void)p;errno=ENOSYS;return -1;}\n')
        # Build the mutant with the same flags the evidence build uses.
        build = subprocess.run(
            ["bash", "-c",
             f'set -e; cd "{ROOT}"; PREFIX="$(brew --prefix)"; '
             f'cc -O2 -g -w -DPGWT_SERVER -I"{ROOT}/src" -I"$PREFIX/include" '
             f'-L"$PREFIX/lib" "{msrc}" src/event_reader.c src/event_writer.c '
             f'src/wait_event.c src/idle_rule.c src/cJSON.c "{stub}" '
             f'-lz -llz4 -lm -o "{tmp}/wait_cpu_scan"'],
            capture_output=True, text=True)
        if build.returncode != 0:
            print(f"{name}: BUILD FAILED\n{build.stderr}")
            overall = 1
            continue
        run = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tests", "test_wait_cpu_scan.py")],
            capture_output=True, text=True,
            env={**os.environ, "SCAN_BIN": os.path.join(tmp, "wait_cpu_scan"),
                 "FIXTURE_BIN": os.environ.get(
                     "FIXTURE_BIN", os.path.join(HERE, "bin", "wait_cpu_fixture"))})
        log = os.path.join(RED, f"{name}.log")
        with open(log, "w") as f:
            f.write(f"$ SCAN_BIN=<mutant {name}> python3 test_wait_cpu_scan.py\n")
            f.write(run.stdout + run.stderr)
        red = set(re.findall(r"^  FAIL (\w+):", run.stdout, re.M))
        missing = [c for c in must_fail if c not in red]
        status = "RED as required" if not missing and run.returncode != 0 else "PROBLEM"
        if missing or run.returncode == 0:
            overall = 1
        print(f"{name}: {status} — red cases: {sorted(red)}"
              + (f" MISSING: {missing}" if missing else ""))
        shutil.rmtree(tmp, ignore_errors=True)
    return overall


if __name__ == "__main__":
    sys.exit(main())
