#!/usr/bin/env python3
"""Each diagnostic mutant must build, reach its case and fail its assertion."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ["src/percpu_transport.c", "src/merge_capture.c", "src/core/merge_adapter.c", "src/core/merge.c"]
MUTANTS = [
    ("wrong_pool", "src/merge_capture.c", "capture_full(c, PGWT_POOL_CAPTURE_ARRIVAL)",
     "capture_full(c, PGWT_POOL_CAPTURE_RETAINED)", "arrival", "a.pool==pool"),
    ("wrong_core_pool", "src/core/merge.c", "PGWT_POOL_CORE_STAGING : PGWT_POOL_CORE_PAYLOAD, obs)",
     "PGWT_POOL_CORE_IDENTITY : PGWT_POOL_CORE_PAYLOAD, obs)", "core_staging", "a.pool==pool"),
    ("dense_payload_scan", "src/core/merge.c",
     "for (size_t i = m->first; i != SIZE_MAX; i = m->queue_nodes[i].next)",
     "for (size_t i = 0; i < m->count; ++i)", "sparse_slots",
     "stats.pools[PGWT_POOL_CORE_PAYLOAD].occupancy==12"),
    ("lost_high_water", "src/merge_capture.c",
     "pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_ARRIVAL], ++c->arrival_count);",
     "++c->arrival_count;", "arrival", "s.pools[pool].occupancy==2"),
    ("wrong_ack_origin", "src/percpu_transport.c", "t->last_ack_at = now;",
     "t->last_ack_at = t->fence_at;", "phases", "d.count==count && d.total==total && d.max==max"),
]


def build(directory, changed=None):
    binary = directory / "diagnostics"
    sources = [str(changed[1]) if changed and path == changed[0] else path for path in SOURCES]
    command = (shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-Wall", "-Wextra", "-Werror",
               "-DPGWT_TRANSPORT_HOST_TEST", "-I", "src", "-I", "src/core"] + shlex.split(os.environ.get("CFLAGS", "")) +
               ["tests/test_transport_diagnostics.c"] + sources + ["-o", str(binary)])
    subprocess.run(command, cwd=ROOT, check=True)
    return binary


with tempfile.TemporaryDirectory(prefix="pgwt-transport-diagnostics-") as tmp:
    directory = Path(tmp)
    baseline = build(directory)
    subprocess.run([str(baseline)], cwd=ROOT, check=True)
    for name, path, before, after, case, assertion in MUTANTS:
        source = (ROOT / path).read_text()
        if source.count(before) != 1:
            raise SystemExit(f"{name}: mutation anchor must occur exactly once")
        changed = directory / Path(path).name
        changed.write_text(source.replace(before, after))
        binary = build(directory, (path, changed))
        result = subprocess.run([str(binary), case], cwd=ROOT, capture_output=True, text=True)
        if (result.returncode != 1 or f"REACHED diagnostics {case}" not in result.stdout or
                assertion not in result.stderr):
            raise SystemExit(f"{name}: not killed by intended assertion: {result.returncode}\n"
                             f"{result.stdout}\n{result.stderr}")
        print(f"KILLED {name}: built, reached {case}, failed {assertion}")
print(f"transport diagnostic mutants: {len(MUTANTS)}/{len(MUTANTS)} killed")
