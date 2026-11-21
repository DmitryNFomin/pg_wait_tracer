#!/usr/bin/env python3
"""Refs #933: compile and kill each required A2 carrier mutant on host.

All mutated headers and binaries live outside the repository. A kill requires
successful compilation, a failing run, and the intended assertion's marker.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/coverage_record.h"
TEST = ROOT / "tests/test_coverage_a2_codec.c"
MUTANTS = [
    ("reason-5-on-GAP_CLOSE",
     [(": reason == PGWT_COVR_NONE;",
       ": reason == PGWT_COVR_NONE || (kind == PGWT_COVK_GAP_CLOSE && reason == 5);")],
     "reason 5 GAP_CLOSE refused"),
    ("A2_BUCKET-size-80",
     [("return kind == PGWT_COVK_BOOT ? PGWT_COVERAGE_BOOT_PAYLOAD_SIZE",
       "return (kind == PGWT_COVK_BOOT || kind == PGWT_COVK_A2_BUCKET) ? PGWT_COVERAGE_BOOT_PAYLOAD_SIZE")],
     "a2_bucket golden fields and bytes"),
    ("A2_BUCKET-a-b-swapped",
     [("pgwt_cov_put_u64(out + 8,  r->a);",
       "pgwt_cov_put_u64(out + 8,  r->kind == PGWT_COVK_A2_BUCKET ? r->b : r->a);"),
      ("pgwt_cov_put_u64(out + 16, r->b);",
       "pgwt_cov_put_u64(out + 16, r->kind == PGWT_COVK_A2_BUCKET ? r->a : r->b);")],
     "a2_bucket golden fields and bytes"),
    ("kind-8-accepted",
     [("kind == PGWT_COVK_A2_BUCKET;", "kind == PGWT_COVK_A2_BUCKET || kind == 8;")],
     "kind 8 decode refused"),
]


def run_case(tmp, name, source, marker=None):
    case = tmp / name
    case.mkdir()
    (case / HEADER.name).write_text(source)
    binary = case / "test"
    command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
               "-Wall", "-Wextra", "-Werror", "-I", str(case),
               *shlex.split(os.environ.get("CFLAGS", "")), str(TEST), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    if built.returncode:
        raise SystemExit(f"FAIL {name}: mutant did not build\n{built.stdout}{built.stderr}")
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    output = result.stdout + result.stderr
    if marker is None:
        if result.returncode or "PASS A2 codec" not in output:
            raise SystemExit(f"FAIL baseline\n{output}")
        print("PASS A2 mutant baseline")
    elif result.returncode <= 0 or f"FAIL {marker}" not in output:
        raise SystemExit(f"FAIL {name}: intended assertion not reached/red\n{output}")
    else:
        print(f"KILLED {name}: built, reached and failed '{marker}' (exit {result.returncode})")


def main():
    original = HEADER.read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-a2-codec-mutants-") as directory:
        tmp = Path(directory)
        run_case(tmp, "baseline", original)
        for name, edits, marker in MUTANTS:
            changed = original
            for before, after in edits:
                if changed.count(before) != 1:
                    raise SystemExit(f"FAIL {name}: stale mutation anchor {before!r}")
                changed = changed.replace(before, after)
            run_case(tmp, name, changed, marker)
    print("PASS A2 codec: all four required mutants killed")


if __name__ == "__main__":
    main()
