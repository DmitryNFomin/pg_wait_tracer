#!/usr/bin/env python3
"""Portable syscall-injected policy/watchdog gate and built, exercised mutants."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def build(directory, source, loop=False):
    path = directory / "rt_priority.c"
    path.write_text(source)
    binary = directory / "rt-test"
    extra = []
    fixture = ROOT / "tests/test_rt_priority.c"
    if loop:
        from test_sampled_attr_io import function
        loop_source = directory / "loop.h"
        loop_source.write_text(function((ROOT / "src/daemon.c").read_text(), "pgwt_daemon_run"))
        extra = [f'-DPGWT_RT_LOOP_SOURCE="{loop_source}"']
        fixture = ROOT / "tests/rt_priority_loop.c"
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                    "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I" + str(ROOT / "src"), *extra, str(fixture),
                    str(path), "-o", str(binary)], check=True)
    return binary


def run(binary, arm):
    result = subprocess.run([str(binary), arm], capture_output=True, text=True, timeout=10)
    assert f"REACHED: {arm}" in result.stdout, result
    return result


def main():
    source = (ROOT / "src/rt_priority.c").read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-rt-host-") as tmp:
        directory = Path(tmp)
        binary = build(directory, source)
        for arm in ("success", "inherited", "refused", "disabled", "signal", "limits",
                    "handler", "mask", "readback", "enumeration", "demote-fails"):
            result = run(binary, arm)
            assert result.returncode == 0 and "PASS" in result.stdout, (arm, result)
            print(f"host {arm}: PASS")
        loop_binary = build(directory, source, loop=True)
        for arm in ("success", "inherited", "refused", "disabled", "signal"):
            result = subprocess.run([str(loop_binary), arm], capture_output=True, text=True, timeout=10)
            assert result.returncode == 0 and "LOOP CAPTURE CONTINUES: PASS" in result.stdout, (arm, result)
            print(f"host actual daemon loop {arm}: PASS")
        # A mutant counts only after successful compilation and entering its arm.
        # The oracle is the same behavioral check as the unmodified implementation.
        mutants = (
            ("all threads FIFO", "success",
             "rt->state = PGWT_RT_ACTIVE;", "rt->state = PGWT_RT_ACTIVE;\n"
             "    rt->ops->set(2, PGWT_RT_FIFO, 1);", "success active"),
            ("refusal exits", "refused", "/* Demote main without",
             "_Exit(23);\n    /* Demote main without", "CONTINUES"),
            ("refusal silent", "refused", "rt->ops->warning(msg, (size_t)n);",
             "(void)n;", "refusal actionable warning"),
            ("SIGXCPU kills", "signal", "(void)signo;", "_Exit(24); (void)signo;", "CONTINUES"),
            ("SIGXCPU ignored", "signal", "(void)signo;", "return; (void)signo;", "SIGXCPU demotes"),
            ("demotion unrecorded", "signal", "rt->demotions = 1;", "rt->demotions = 0;",
             "demotion recorded"),
        )
        for label, arm, old, new, assertion in mutants:
            assert source.count(old) == 1, (label, old)
            mutated = "#include <stdlib.h>\n" + source.replace(old, new)
            binary = build(directory, mutated)
            result = run(binary, arm)
            assert result.returncode != 0, (label, "SURVIVED", result)
            if assertion == "CONTINUES":
                assert "CONTINUES" not in result.stdout and result.returncode in (23, 24), result
            else:
                assert assertion in result.stderr, (label, result)
            print(f"mutant {label}: RED (reached {arm}; {assertion})")


if __name__ == "__main__":
    main()
