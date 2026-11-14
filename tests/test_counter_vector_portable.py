#!/usr/bin/env python3
"""Mac/Linux capture accounting C units; build only host code into /tmp."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

from test_counter_failures import function

ROOT = Path(__file__).resolve().parents[1]


def compile_run(directory, name, sources, extra=()):
    binary = Path(directory) / name
    command = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-parameter", *shlex.split(os.environ.get("CFLAGS", "")),
               "-I", str(ROOT / "src"), *[str(ROOT / path) for path in sources],
               *extra, "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)


def run_units(directory, vector_source="src/counter_vector.c", writer_source="src/event_writer.c"):
    compile_run(directory, "vector", ["tests/test_counter_vector.c", vector_source])
    compile_run(directory, "ring", ["tests/test_ringbuf_metrics.c", vector_source])
    lz4 = ["-llz4"]
    if sys.platform == "darwin":
        prefix = subprocess.check_output(["brew", "--prefix", "lz4"], text=True).strip()
        lz4 = ["-I" + prefix + "/include", "-L" + prefix + "/lib", *lz4]
    compile_run(directory, "writer", ["tests/test_writer_failure.c", writer_source], lz4)


def run_daemon_reads(directory, map_source=None, backend_source=None):
    map_source = map_source or (ROOT / "src/capture_counters.c").read_text()
    backend_source = backend_source or (ROOT / "src/backend.c").read_text()
    bodies = [function(map_source, name) for name in (
        "pgwt_read_bpf_fail_counter", "pgwt_read_daemon_ring_site", "pgwt_daemon_counter_vector")]
    bodies.append(function(backend_source, "read_init_state"))
    bodies.append(function((ROOT / "src/control.c").read_text(), "legacy_state_map_full"))
    unit = Path(directory) / "daemon_reads.c"
    unit.write_text((ROOT / "tests/counter_daemon_harness.c").read_text().replace(
        "/* PRODUCTION_FUNCTIONS */", "\n\n".join(bodies)))
    compile_run(directory, "daemon_reads", [unit, "src/counter_vector.c"])


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-counter-units-") as tmp:
        run_units(tmp)
        run_daemon_reads(tmp)


if __name__ == "__main__":
    main()
