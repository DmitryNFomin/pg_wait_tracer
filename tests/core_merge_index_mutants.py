#!/usr/bin/env python3
"""Each index mutant must build and fail its intended differential case cleanly."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
import test_core_merge_index as checks


def main():
    source = (checks.ROOT / "src/core/merge.c").read_text()
    mutations = (
        ("wrong_probe", "probe", "slot = (slot + 1) & (m->index_capacity - 1);", "break;"),
        ("missed_insert", "insert", "m->generation_index[bucket] = m->attachment_count;",
         "m->generation_index[bucket] = 0;"),
        ("stale_undo_entry", "plan_reset", "m->planned[m->touched[i]] = 0;",
         "m->planned[m->touched[i]] = i + 1;"),
    )
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:exitcode=86",
               UBSAN_OPTIONS="halt_on_error=1:exitcode=87:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="pgwt-merge-index-mutants-") as directory:
        work = Path(directory)
        base = checks.build_base(work)
        binary = checks.build(work, base, checks.ROOT / "src/core/merge.c")
        for _, case, _, _ in mutations:
            log = checks.run([str(binary), "--case", case], env=env)
            if f"index {case} PASS" not in log:
                raise RuntimeError("baseline did not pass named case")
        for name, case, before, after in mutations:
            if source.count(before) != 1:
                raise RuntimeError(f"{name}: mutation anchor must occur exactly once")
            mutant = work / "mutant.c"
            mutant.write_text(source.replace(before, after, 1))
            binary = checks.build(work, base, mutant)
            result = subprocess.run([str(binary), "--case", case], cwd=checks.ROOT,
                                    env=env, text=True, capture_output=True, timeout=30)
            log = result.stdout + result.stderr
            intended = re.findall(rf"^index {case} MISMATCH status at input [0-9]+$", log, re.M)
            if (result.returncode != 1 or f"index {case} REACHED\n" not in log or
                    len(intended) != 1 or log.count("MISMATCH") != 1 or
                    re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", log)):
                raise RuntimeError(f"{name}: not an intended kill (exit {result.returncode})\n{log}")
            print(f"{name} KILLED: {intended[0]}", flush=True)
    print("core merge index mutants: 3/3 killed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
