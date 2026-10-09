#!/usr/bin/env python3
"""Red probes for issue #334's two cache truncation assertions.

Run from any directory: python3 tests/data_current_trace_cache_mutations.py
Builds pgwt-server and gen_test_traces in throwaway copies, then runs only
bypass_truncated_file_under_meta (not the full suite). Requires the same build
dependencies as that test. A baseline failure, missed source needle, failed
build, or green mutant is a failed probe, never evidence of coverage.
"""

import os
import shutil
import subprocess
import sys
import tempfile


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYNC_CALL = "    if (cc)\n        cur_cache_sync(cc, path, &reader);"
FEWER = "a lower committed watermark serves a nonempty prefix"
RETENTION = "the entry did not keep blocks the file no longer has"
DIFFERENTIAL = "the lower-watermark read matches a fresh uncached read exactly"

# This mutant retains the stale entry but still obeys the reader's smaller
# block count. The response check should pass; the retention check must fail.
RETAIN_STALE = "    /* mutant: omit cache identity sync */"

# This mutant also reads the vanished suffix from the retained cache. It
# expands the reader's index with the cached descriptors so the block loop
# itself walks those stale blocks. The data bytes remain on disk, but the
# lowered .meta watermark explicitly says they are no longer committed.
SERVE_STALE = """    /* mutant: ignore the lower committed watermark for cached blocks */
    if (cc && cc->lo_block == 0 && cc->n_blocks > reader.num_blocks) {
        int held = cc->n_blocks;
        struct pgwt_block_index_entry *idx =
            realloc(reader.block_index, (size_t)held * sizeof(*idx));
        if (idx) {
            reader.block_index = idx;
            for (int b = reader.num_blocks; b < held; b++) {
                idx[b].timestamp_ns = cc->blk[b].index_ts;
                idx[b].file_offset = cc->blk[b].file_offset;
            }
            reader.num_blocks = held;
        }
    }"""

RUN = (
    "import test_data_current_trace_cache as m; "
    "r=m.TestRunner('truncation mutation probe'); "
    "m.bypass_truncated_file_under_meta(r); "
    "raise SystemExit(0 if r.summary() else 1)"
)


def copy_tree(dst):
    shutil.copytree(ROOT, dst, ignore=shutil.ignore_patterns(
        ".git", "build", "__pycache__", "results", "pgwt-server",
        "gen_test_traces", "*.o", "*.d"))


def run_cmd(argv, cwd, timeout=600):
    return subprocess.run(argv, cwd=cwd, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)


def build(tree, generator=False):
    commands = [["make", "pgwt-server"]]
    if generator:
        commands.append(["make", "-C", "tests", "gen_test_traces"])
    for cmd in commands:
        result = run_cmd(cmd, tree)
        if result.returncode:
            raise RuntimeError("%s failed in %s:\n%s" %
                               (" ".join(cmd), tree, result.stdout[-3000:]))


def run_case(tree):
    return run_cmd([sys.executable, "-c", RUN], os.path.join(tree, "tests"))


def change_source(tree, replacement):
    source = os.path.join(tree, "src", "server.c")
    with open(source) as f:
        content = f.read()
    hits = content.count(SYNC_CALL)
    if hits != 1:
        raise RuntimeError("source mutation needle matched %d times, expected 1"
                           % hits)
    with open(source, "w") as f:
        f.write(content.replace(SYNC_CALL, replacement))


def verdict(label, result, wanted_fail, wanted_pass):
    lines = [line.strip() for line in result.stdout.splitlines()
             if line.strip().startswith(("PASS:", "FAIL:"))]
    failures = [line for line in lines if line.startswith("FAIL:")]
    target = all(any(marker in line for line in failures)
                 for marker in wanted_fail)
    isolation = all(any(marker in line for line in lines
                        if line.startswith("PASS:"))
                    for marker in wanted_pass)
    ok = result.returncode != 0 and target and isolation
    print("%s %s: %s" % ("RED " if ok else "FAIL", label,
                        "target assertion failed" if ok else
                        "target assertion did not prove red"))
    for line in failures:
        print("    %s" % line[:240])
    if not ok:
        print(result.stdout[-1500:])
    return ok


def main():
    try:
        with tempfile.TemporaryDirectory(prefix="pgwt-334-mutants-") as work:
            base = os.path.join(work, "base")
            copy_tree(base)
            build(base, generator=True)
            baseline = run_case(base)
            if baseline.returncode or "FAIL:" in baseline.stdout:
                print("FAIL baseline: pristine assertion is not green")
                print(baseline.stdout[-3000:])
                return 1
            for marker in (FEWER, RETENTION, DIFFERENTIAL):
                if not any(marker in line for line in baseline.stdout.splitlines()
                           if line.strip().startswith("PASS:")):
                    print("FAIL baseline: missing PASS for %s" % marker)
                    return 1
            print("PASS baseline: readable prefix and both assertions green")

            cases = (
                ("retained stale blocks", RETAIN_STALE,
                 (RETENTION,), (FEWER, DIFFERENTIAL)),
                ("served stale superset", SERVE_STALE,
                 (DIFFERENTIAL, FEWER), ()),
            )
            ok = True
            for i, (label, mutation, red, green) in enumerate(cases, 1):
                tree = os.path.join(work, "mutant%d" % i)
                copy_tree(tree)
                shutil.copy2(os.path.join(base, "tests", "gen_test_traces"),
                             os.path.join(tree, "tests", "gen_test_traces"))
                change_source(tree, mutation)
                build(tree)
                result = run_case(tree)
                ok = verdict(label, result, red, green) and ok
            print("PASSED: both assertions went red" if ok else
                  "FAILED: mutation coverage not demonstrated")
            return 0 if ok else 1
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("FAIL probe: %s" % exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
