#!/usr/bin/env python3
"""#339 teeth demonstration; run manually in an isolated Linux checkout.

Build dependencies first: make pgwt-server; make -C tests test_transitions_loader
Then: python3 tests/transitions_loader_mutations.py
Temporarily mutates server.c, restores it in finally, and rebuilds/runs green.
Not in unit_tests.list: this is mutation evidence, not a routine suite entry.
"""
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src/server.c"


def run_test():
    build = subprocess.run(
        ["make", "-C", "tests", "test_transitions_loader"], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if build.returncode:
        raise RuntimeError("mutation build failed (not a test red):\n" + build.stdout)
    result = subprocess.run(
        ["tests/test_transitions_loader"], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="", flush=True)
    return result


def require_green(label):
    print(label, flush=True)
    result = run_test()
    if result.returncode != 0:
        raise RuntimeError("expected green, exit=%d" % result.returncode)
    if "PASS: no-CPU-source multi-file equality; 8 warm windows merged blocks" not in result.stdout:
        raise RuntimeError("no evidence that the equality test ran")
    if "PASS: decoded-block candidate refused; response matches tagged loader" not in result.stdout:
        raise RuntimeError("no evidence that the production refusal test ran")


def main():
    original = SOURCE.read_text()
    require_green("GREEN baseline")
    # Limit replacement to transitions_from_block_aggs: the helper itself
    # and bagg_window_eligible's unreachable eligibility body stay intact.
    prefix, body = original.split("static int transitions_from_block_aggs(", 1)
    call = """if (!pgwt_block_agg_file_can_contribute(fc->mono_first, fc->mono_last,
                                                from_m, to_m))"""
    if body.count(call) != 1:
        raise RuntimeError("multi-file call-site anchor must match exactly once")
    off_by_one = prefix + "static int transitions_from_block_aggs(" + body.replace(
        call, "if (fc->mono_first >= to_m || fc->mono_last < from_m)", 1)
    refusal = "return 0;\n\n    if (!pgwt_block_agg_filter_supported(f))"
    if original.count(refusal) != 1:
        raise RuntimeError("production refusal anchor must match exactly once")
    mutations = [
        ("RED call-site >=", off_by_one,
         "file first == to: aggregate=8192 raw=8193 exact=8192",
         "agg.total_transitions == oracle.total_transitions"),
        ("RED production refusal removed", original.replace(
            refusal, "if (!pgwt_block_agg_filter_supported(f))", 1),
         "cold full: aggregate=24576 raw=24576", "value->valuedouble == expected"),
    ]
    for label, mutated, counts, assertion in mutations:
        try:
            SOURCE.write_text(mutated)
            print(label, flush=True)
            result = run_test()
            if result.returncode != 1 or counts not in result.stdout or assertion not in result.stdout:
                raise RuntimeError("expected assertion red with observed counts, exit=%d" % result.returncode)
        finally:
            SOURCE.write_text(original)
        require_green("GREEN restored after " + label)
    print("PASS: both mutations failed the intended assertions; restored source is green")


if __name__ == "__main__":
    main()
