#!/usr/bin/env python3
"""Portable attribution syscall budget and mutations; all outputs stay in /tmp."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(args):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT / 'src'}"] + shlex.split(os.environ.get("CFLAGS", ""))
    if os.uname().sysname == "Darwin":
        flags += ["-include", str(ROOT / "demos/replay-compat.h")]
    source = (ROOT / "src/backend_status_layout.c").read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-sampled-io-") as directory:
        tmp = Path(directory)

        def build(label, text):
            implementation = tmp / f"{label}.c"
            implementation.write_text(text)
            binary = tmp / label
            result = run(cc + flags + [
                f'-DPGWT_PGBS_SOURCE="{implementation}"',
                str(ROOT / "tests/test_sampled_attr_io.c"),
                str(ROOT / "src/spawn.c"), "-o", str(binary)])
            assert result.returncode == 0, result.stdout + result.stderr
            return binary

        result = run([str(build("baseline", source))])
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end="")
        mutations = [
            ("per_backend_open",
             "    *base = reader->status_addr ? reader->status_addr : backend_status_addr;",
             "    int old_fd = reader->mem_fd;\n"
             "    reader->mem_fd = open_mem(pid);\n    close(old_fd);\n"
             "    *base = reader->status_addr ? reader->status_addr : backend_status_addr;",
             "steady attribution syscall budget and no opens"),
            ("accept_torn",
             "        snapshot->changecount_after = (uint32_t)value;\n"
             "        if (snapshot->changecount_before == snapshot->changecount_after &&",
             "        snapshot->changecount_after = snapshot->changecount_before;\n"
             "        if (snapshot->changecount_before == snapshot->changecount_after &&",
             "torn snapshot fails closed"),
            ("leak_descriptor",
             "    if (reader->opened && reader->mem_fd >= 0)\n        close(reader->mem_fd);",
             "    if (reader->opened && reader->mem_fd >= 0)\n        (void)reader->mem_fd;",
             "churn closes every enrollment descriptor"),
        ]
        for label, before, after, assertion in mutations:
            assert source.count(before) == 1, (label, "mutation anchor changed")
            binary = build(label, source.replace(before, after))
            result = run([str(binary)])
            assert result.returncode == 1, (label, result.returncode, result.stdout, result.stderr)
            assert f"FAIL: {assertion}" in result.stderr, (label, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")


if __name__ == "__main__":
    main()
