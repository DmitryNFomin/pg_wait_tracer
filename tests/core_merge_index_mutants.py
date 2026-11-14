#!/usr/bin/env python3
"""Mutants must build, reach their case, and fail only the intended assertion."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

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
        # Keep the differential mutants above unchanged. Cost mutants use
        # 1,000 retained generations: a scan still exceeds 64 visits by a wide
        # margin, while quadratic population finishes quickly.
        attachment = source[source.index("static struct generation_state *attachment("):
                            source.index("/* touched[plan slot]")]
        scan = """static struct generation_state *attachment(pgwt_merge *m, const pgwt_merge_record *r)
{
    MERGE_WORK(LOOKUP, 0);
    for (size_t i = 0; i < m->attachment_count; ++i) {
        MERGE_WORK(INDEX, 1);
        if (m->attachments[i].proof.pid == r->pid && m->attachments[i].proof.g == r->g)
            return &m->attachments[i];
    }
    return NULL;
}

"""
        cost_mutations = (
            ("linear_scan", [(attachment, scan)], "work lookup visits <= 64"),
            ("whole_table_copy", [
                ("size_t plan_capacity = c->attachment_capacity < c->staged_capacity ?\n"
                 "            c->attachment_capacity : c->staged_capacity;",
                 "size_t plan_capacity = c->attachment_capacity;"),
                ("    m->touched_count = 0;", "    m->touched_count = 0;\n"
                 "    copy_generations(m->prospective, m->attachments, m->attachment_count);")],
             "work copied slots <= 16"),
        )
        binary = checks.build(work, base, checks.ROOT / "src/core/merge.c", instrument=True)
        log = checks.run([str(binary), "--work-small"], env=env)
        if "index work PASS" not in log:
            raise RuntimeError("baseline did not pass cost case")
        for name, edits, assertion in cost_mutations:
            mutated = source
            for before, after in edits:
                if mutated.count(before) != 1:
                    raise RuntimeError(f"{name}: mutation anchor must occur exactly once")
                mutated = mutated.replace(before, after, 1)
            mutant = work / "mutant.c"
            mutant.write_text(mutated)
            binary = checks.build(work, base, mutant, instrument=True)
            start = time.monotonic()
            result = subprocess.run([str(binary), "--work-small"], cwd=checks.ROOT,
                                    env=env, text=True, capture_output=True, timeout=10)
            elapsed = time.monotonic() - start
            log = result.stdout + result.stderr
            intended = f"index work MISMATCH {assertion} at input 0"
            if (result.returncode != 1 or "index work REACHED\n" not in log or
                    log.splitlines().count(intended) != 1 or log.count("MISMATCH") != 1 or
                    re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", log)):
                raise RuntimeError(f"{name}: not an intended kill (exit {result.returncode})\n{log}")
            print(f"{name} KILLED: retained=1000, {elapsed:.3f}s, {intended}", flush=True)
    print("core merge index mutants: 3/3 differential, 2/2 deterministic cost killed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
