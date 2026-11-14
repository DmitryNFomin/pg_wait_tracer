#!/usr/bin/env python3
"""Queue regressions must build and fail their exact work/semantic assertion."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
import test_core_merge_queue as queue
import test_core_merge_index as checks


def main():
    source = (checks.ROOT / "src/core/merge.c").read_text()
    mutations = (
        ("full_queue_pass", True, "    size_t begin = m->first, prior_count = 0, tail = SIZE_MAX;",
         "    for (size_t scan = m->first; scan != SIZE_MAX; scan = m->queue_nodes[scan].next)\n"
         "        (void)staged_order(m, &m->queue[scan], &m->queue[m->first]);\n"
         "    size_t begin = m->first, prior_count = 0, tail = SIZE_MAX;",
         "input queue comparisons <= log bound"),
        ("whole_queue_move", True, "    size_t slot = m->first;",
         "    MERGE_WORK(QMOVE, m->count - 1);\n"
         "    memmove(m->queue, m->queue + 1, (m->count - 1) * sizeof(*m->queue));\n"
         "    size_t slot = m->first;", "publication queue moves <= 4/record"),
        ("revalidate_held_prefix", True,
         "    if (m->held_cached && m->waiting_count) {",
         "    if (false && m->held_cached && m->waiting_count) {",
         "dependency held queue comparisons <= 64"),
        ("linear_dependency_lookup", True,
         "    return id && m->dependency_index[dependency_bucket(m, id)].epoch == m->report.epoch;",
         "    if (!id) return false;\n"
         "    for (size_t i = 0; i < m->dependency_count; ++i) {\n"
         "        MERGE_WORK(QCOMPARE, 1);\n"
         "        if (m->dependencies[i] == id) return true;\n"
         "    }\n"
         "    return false;", "distinct dependency queue comparisons <= bound"),
        ("source_before_pid", False, "    CMP(t);\n    CMP(pid);",
         "    CMP(t);\n    CMP(source);\n    CMP(pid);", "output"),
    )
    with tempfile.TemporaryDirectory(prefix="pgwt-queue-mutants-") as directory:
        work = Path(directory)
        base = checks.build_base(work)
        for instrument in (False, True):
            binary = queue.build(work, base, checks.ROOT / "src/core/merge.c", instrument)
            print(checks.run([str(binary)] + (["--cost"] if instrument else [])), end="", flush=True)
        for name, instrument, before, after, assertion in mutations:
            if source.count(before) != 1:
                raise RuntimeError(f"{name}: anchor must occur exactly once")
            mutant = work / "mutant.c"
            mutant.write_text(source.replace(before, after, 1))
            binary = queue.build(work, base, mutant, instrument)
            start = time.monotonic()
            result = subprocess.run([str(binary)] + (["--cost"] if instrument else []),
                                    cwd=checks.ROOT, text=True, capture_output=True, timeout=20)
            elapsed = time.monotonic() - start
            log = result.stdout + result.stderr
            case = "work" if instrument else "corpus"
            intended = re.findall(rf"^queue {case} MISMATCH {re.escape(assertion)} at input [0-9]+$", log, re.M)
            reached = "queue work REACHED" if instrument else "queue backlog REACHED mode=N action=0 held=12000"
            if (result.returncode != 1 or reached not in log or len(intended) != 1 or
                    log.count("MISMATCH") != 1 or re.search(
                        r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", log)):
                raise RuntimeError(f"{name}: not an intended kill (exit {result.returncode})\n{log}")
            print(f"{name} KILLED in {elapsed:.3f}s: {intended[0]}", flush=True)
        before = "        begin = m->queue_nodes[m->held_tail].next;"
        if source.count(before) != 1:
            raise RuntimeError("skipped_suffix: anchor must occur exactly once")
        mutant = work / "mutant.c"
        mutant.write_text(source.replace(before, "        begin = SIZE_MAX;", 1))
        binary = queue.build(work, base, mutant)
        start = time.monotonic()
        result = subprocess.run([str(binary), "--incremental"], cwd=checks.ROOT,
                                text=True, capture_output=True, timeout=20)
        log = result.stdout + result.stderr
        intended = re.findall(r"^queue incremental MISMATCH status at input [0-9]+$", log, re.M)
        if (result.returncode != 1 or "queue incremental REACHED held=12000" not in log or
                len(intended) != 1 or log.count("MISMATCH") != 1 or re.search(
                    r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", log)):
            raise RuntimeError(f"skipped_suffix: not an intended kill\n{log}")
        print(f"skipped_suffix KILLED in {time.monotonic() - start:.3f}s: {intended[0]}", flush=True)
    print("core merge queue mutants: 4/4 counted cost, 1/1 tie-break, 1/1 held suffix killed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
