#!/usr/bin/env python3
"""Red probes for the cache-truncation assertions (#334) and the startup
refusal reason (#348).

Run from any directory: python3 tests/data_current_trace_cache_mutations.py
Builds pgwt-server and gen_test_traces in throwaway copies of the tree, then
runs ONE test function per case (not the full suite). Nothing in the real tree
is modified. Requires the same build dependencies as that test. A baseline
failure, a missed source needle, a failed build, or a green mutant is a FAILED
probe, never evidence of coverage.

Why this exists: `bypass_truncated_file_under_meta` passed for a year while
the product was free to retain and serve blocks the file no longer had,
because it cut current.trace by byte count, landed inside block 0, and then
compared one empty answer to another. A mutation that is not demonstrated red
is not a gate.
"""

import os
import shutil
import subprocess
import sys
import tempfile


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ── the source sites the mutants replace ────────────────────────────────────
SYNC_CALL = "    if (cc)\n        cur_cache_sync(cc, path, &reader);"
REFUSAL_PRINT = ('        fprintf(stderr, "pgwt-server: no trace files found '
                 'in %s\\n", trace_dir);')
SERVE_FROM_CACHE = ("            rec = cc->events + cb->start;\n"
                    "            n = cb->count;")

# ── assertion markers (substrings of the check names) ───────────────────────
FEWER = "a byte-truncated current.trace serves a nonempty"
RETENTION = "the entry did not keep blocks the file no longer has"
DIFFERENTIAL = "the truncated read matches a fresh uncached read exactly"
HELD = "the cached entry held a block the file no longer has"
SERVED = "the cache served the pre-truncation request"
REPEAT = "the repeated pre-truncation request answers identically"
REFUSED = "an empty trace dir is refused outright"
REASON = "and the refusal names the reason"

# This mutant retains the stale entry but still obeys the reader's smaller
# block count. The response check should pass; the retention check must fail.
RETAIN_STALE = "    /* mutant: omit cache identity sync */"

# This mutant also reads the vanished suffix from the retained cache. It
# expands the reader's index with the cached descriptors so the block loop
# itself walks those stale blocks -- and because the cache answers them from
# memory, the bytes being gone from the file does not stop it.
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

# #348's mutant: the server still refuses an empty trace dir, and still exits
# before the request loop, but no longer says WHY. The harness fix makes the
# reason retrievable on both outcomes of that exit race; this proves the fix
# did not replace the assertion with a formality -- an absent reason is red.
SILENT_REFUSAL = '        fprintf(stderr, "pgwt-server: startup failed\\n");'

# A cache that SERVES a block differently from the way the decode path read it.
# One event short is the smallest version of the hazard, and it is invisible
# to any single request: the first request decodes, only a repeat is served.
# That is what the repeated-request check exists for, so that check -- and
# only a check like it -- must go red here.
SERVE_ONE_SHORT = ("            rec = cc->events + cb->start;\n"
                   "            /* mutant: serve the cached block one event "
                   "short */\n"
                   "            n = cb->count > 1 ? cb->count - 1 "
                   ": cb->count;")

RUN_TMPL = (
    "import test_data_current_trace_cache as m; "
    "r=m.TestRunner('%(label)s'); "
    "m.%(func)s(r); "
    "raise SystemExit(0 if r.summary() else 1)"
)
RUN_TRUNC = RUN_TMPL % {"label": "truncation mutation probe",
                        "func": "bypass_truncated_file_under_meta"}
RUN_DEPS = RUN_TMPL % {"label": "refusal mutation probe",
                       "func": "bypass_dependencies"}


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


def run_case(tree, run):
    return run_cmd([sys.executable, "-c", run], os.path.join(tree, "tests"))


def change_source(tree, needle, replacement):
    source = os.path.join(tree, "src", "server.c")
    with open(source) as f:
        content = f.read()
    hits = content.count(needle)
    if hits != 1:
        raise RuntimeError("source mutation needle matched %d times, "
                           "expected 1: %r" % (hits, needle[:60]))
    with open(source, "w") as f:
        f.write(content.replace(needle, replacement))


def names(result):
    return [line.strip() for line in result.stdout.splitlines()
            if line.strip().startswith(("PASS:", "FAIL:"))]


def check_baseline(label, result, markers):
    """A baseline that is not fully green, or that is missing any marker this
    probe later looks for, means the mutants below would be measuring a
    typo. Fail loudly instead."""
    if result.returncode or "FAIL:" in result.stdout:
        print("FAIL baseline (%s): pristine assertions are not green" % label)
        print(result.stdout[-3000:])
        return False
    passes = [line for line in names(result) if line.startswith("PASS:")]
    for marker in markers:
        if not any(marker in line for line in passes):
            print("FAIL baseline (%s): missing PASS for %r" % (label, marker))
            print(result.stdout[-3000:])
            return False
    print("PASS baseline (%s): %d assertions green, all markers present"
          % (label, len(passes)))
    return True


def verdict(label, result, wanted_fail, wanted_pass):
    lines = names(result)
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
            if not check_baseline("truncation", run_case(base, RUN_TRUNC),
                                  (FEWER, RETENTION, DIFFERENTIAL, HELD,
                                   SERVED, REPEAT)):
                return 1
            if not check_baseline("refusal", run_case(base, RUN_DEPS),
                                  (REFUSED, REASON)):
                return 1

            cases = (
                ("retained stale blocks", SYNC_CALL, RETAIN_STALE, RUN_TRUNC,
                 (RETENTION,), (FEWER, DIFFERENTIAL)),
                ("served stale superset", SYNC_CALL, SERVE_STALE, RUN_TRUNC,
                 (DIFFERENTIAL, FEWER), ()),
                ("refusal without a reason", REFUSAL_PRINT, SILENT_REFUSAL,
                 RUN_DEPS, (REASON,), (REFUSED,)),
                ("cached block served one event short", SERVE_FROM_CACHE,
                 SERVE_ONE_SHORT, RUN_TRUNC, (REPEAT,), ()),
            )
            ok = True
            for i, (label, needle, mutation, run, red, green) in \
                    enumerate(cases, 1):
                tree = os.path.join(work, "mutant%d" % i)
                copy_tree(tree)
                shutil.copy2(os.path.join(base, "tests", "gen_test_traces"),
                             os.path.join(tree, "tests", "gen_test_traces"))
                change_source(tree, needle, mutation)
                build(tree)
                result = run_case(tree, run)
                ok = verdict(label, result, red, green) and ok
            print("PASSED: every mutant went red" if ok else
                  "FAILED: mutation coverage not demonstrated")
            return 0 if ok else 1
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("FAIL probe: %s" % exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
