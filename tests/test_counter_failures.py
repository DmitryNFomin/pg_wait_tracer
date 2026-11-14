#!/usr/bin/env python3
"""Run real capture failure-path function bodies as host C with fake helpers.

This tests accounting only. Linux load/verifier and real probe stimulus remain
in test_counter_vector_live.py. No BPF bytecode is built or loaded here.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = ("count_ring_fail", "count_map_fail", "test_fail_once", "note_query_edge",
             "note_cmd_edge", "note_phase_edge", "read_wait_event_direct",
             "read_wait_event", "accum_add", "on_bootstrap", "on_fork",
             "on_report_query_id", "on_executor_start")


def function(source, name):
    # All selected functions have a unique, unadorned definition. Preserve
    # their exact bytes; replacements/mutations operate on the product source.
    import re
    found = list(re.finditer(r"^(?:static (?:__always_inline )?)?(?:void|bool|u32|int|struct pgwt_counter_(?:value|vector)) " + name + r"\(", source, re.M))
    if len(found) != 1:
        raise RuntimeError(f"expected one definition of {name}, found {len(found)}")
    start = found[0].start()
    brace = source.index("{", found[0].end())
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    return source[start:end]


def host_source(source):
    harness = (ROOT / "tests/counter_bpf_harness.c").read_text()
    bodies = "\n\n".join(function(source, name) for name in FUNCTIONS)
    assert harness.count("/* PRODUCTION_FUNCTIONS */") == 1
    return harness.replace("/* PRODUCTION_FUNCTIONS */", bodies)


def compile_host(source, directory):
    directory = Path(directory)
    unit = directory / "capture.c"
    unit.write_text(host_source(source))
    binary = directory / "capture"
    command = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-parameter", *shlex.split(os.environ.get("CFLAGS", "")),
               "-I", str(ROOT / "src"), str(unit), "-o", str(binary)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"host compilation failed:\n{result.stdout}{result.stderr}")
    return binary


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-counter-paths-") as tmp:
        binary = compile_host((ROOT / "src/bpf/pg_wait_tracer.bpf.c").read_text(), tmp)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
