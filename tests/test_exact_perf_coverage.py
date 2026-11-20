#!/usr/bin/env python3
"""T-cov production capture/writer/verdict schedules and executable mutants.

No eBPF work. All copied sources, binaries and traces live outside the repo.
A mutant counts only after a successful build and its intended assertion.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = shlex.split(os.environ.get("CC", "cc"))
FLAGS = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
         "-DPGWT_SERVER", "-DPGWT_TRANSPORT_HOST_TEST", "-Isrc", "-Iinclude"]
FLAGS += shlex.split(os.environ.get("CFLAGS", ""))
if Path("/opt/homebrew/include/lz4.h").exists():
    FLAGS += ["-I/opt/homebrew/include"]
LINK = shlex.split(os.environ.get("LDFLAGS", ""))
if Path("/opt/homebrew/lib/liblz4.dylib").exists():
    LINK += ["-L/opt/homebrew/lib"]
SOURCES = ["tests/exact_perf_coverage_host.c", "src/open_snapshot.c", "src/percpu_transport.c",
           "src/core/merge_adapter.c", "src/core/merge.c", "src/event_reader.c",
           "src/coverage.c", "src/wait_event.c", "src/idle_rule.c", "src/cJSON.c"]
MUTANTS = [
    ("cursor_after_publish", "src/open_snapshot.c",
     "    ops->positions(ctx, in);\n    in->publish_ok = (!in->exact_perf || in->capture_healthy) && ops->publish(ctx);",
     "    in->capture_healthy = true;\n    in->publish_ok = ops->publish(ctx);\n    ops->positions(ctx, in);",
     "publish_order", "cursor read before publish cannot certify concurrent advance"),
    ("skip_publish", "src/open_snapshot.c",
     "in->publish_ok = (!in->exact_perf || in->capture_healthy) && ops->publish(ctx);",
     "in->publish_ok = true;",
     "progress", "successful publish puts certified DATA in trace"),
    ("publish_suspended", "src/open_snapshot.c",
     "in->publish_ok = (!in->exact_perf || in->capture_healthy) && ops->publish(ctx);",
     "in->publish_ok = ops->publish(ctx);",
     "failure", "suspended capture does not publish"),
    ("floors_at_promotion", "src/open_snapshot.c",
     "            cov_take_pending(t);",
     "            { t->pfd_ns = in->floor_db_ns; t->pfi_ns = in->floor_idle_ns; cov_take_pending(t); }",
     "progress", "floors stay paired with their P"),
    ("capacity_gaps_only", "src/merge_capture.c",
     "        coverage_gap(c, c->last_void);",
     "        if (c->last_void.recoverable) coverage_gap(c, c->last_void);",
     "reasons", "every VOID opens GAP"),
    ("close_at_rearm", "src/merge_capture.c",
     "    c->rearm_at = now; ++c->rearms;",
     "    c->rearm_at = now; ++c->rearms;\n"
     "    coverage_gap(c, (struct pgwt_capture_gap){.kind=PGWT_CAPTURE_GAP_CAPACITY_END,.at=now,.detected=now});",
     "rearm", "REARM must not close capacity gap"),
    ("first_revocation_only", "src/merge_capture.c",
     "c->feeding_data = in && in->kind == PGWT_MERGE_DATA;",
     "c->feeding_data = in && in->kind == PGWT_MERGE_DATA && !pgwt_capture_report(c).epoch_invalid;",
     "late", "each late record revokes"),
    ("ignore_preexisting", "src/merge_capture.c",
     "    if (count) {\n        memcpy(c->preexisting, pids, count * sizeof(*pids));",
     "    if (count && false) {\n        memcpy(c->preexisting, pids, count * sizeof(*pids));",
     "preexisting", "preexisting gap begins at boot_start"),
    ("omit_gap_fsync", "src/event_writer.c",
     "if (open && fsync(fileno(w->fp)) != 0)", "if (open && false)",
     "reasons", "every GAP_OPEN fsyncs"),
    ("forget_gap_failure", "src/open_snapshot.c",
     "    if (rc != 0) t->gap_failed = true;", "    /* failed revocation forgotten */",
     "sync_failure", "failed GAP_OPEN permanently disables watermarks"),
]


def run(command, tree, check=True):
    result = subprocess.run(command, cwd=tree, capture_output=True, text=True,
                            env={**os.environ, "PGWT_TCOV_TMPDIR": str(tree)})
    if check and result.returncode:
        raise AssertionError(f"{shlex.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result



def daemon_binding(tree):
    source = (tree / "src/open_snapshot.c").read_text()
    names = ["cov_capture_gap", "pgwt_coverage_start", "cov_thread_cpu_ns", "cov_clock",
             "cov_read_counters", "cov_publish", "cov_perf_positions", "cov_perf_counters",
             "cov_gather", "pgwt_coverage_tick", "pgwt_coverage_shutdown"]
    bodies = []
    for name in names:
        match = re.search(r"^(?:static )?[^\n;{}]+\b" + name + r"\([^;]*?\)\n\{.*?^\}",
                          source, re.M | re.S)
        assert match, name
        bodies.append(match[0])
    (tree / "tests/exact_perf_coverage_daemon.h").write_text("\n\n".join(bodies))


def build(tree):
    daemon_binding(tree)
    writer = tree / "writer.o"
    run([*CC, *FLAGS, "-Dfsync=coverage_test_fsync", "-c", "src/event_writer.c", "-o", str(writer)], tree)
    binary = tree / "tcov"
    run([*CC, *FLAGS, *SOURCES, str(writer), *LINK, "-llz4", "-lz", "-lm", "-o", str(binary)], tree)
    return binary



def reader_roundtrip(tree, binary):
    source = (tree / "src/event_writer.c").read_text()
    match = re.search(r"^int pgwt_decode_varint\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
    assert match
    fragment = tree / "varint.c"
    fragment.write_text('#include "event_writer.h"\n' + match[0] + "\n")
    reader = tree / "coverage-verdict"
    run([*CC, *FLAGS, "tests/coverage_trace_verdict.c", "src/event_reader.c", "src/coverage.c",
         str(fragment), *LINK, "-llz4", "-lz", "-o", str(reader)], tree)
    result = run([str(binary), "reader_export"], tree)
    line = next(line for line in result.stdout.splitlines() if line.startswith("EXPORTED "))
    _, directory, base = line.split()
    directory, base = Path(directory), int(base)
    try:
        path = directory / "current.trace"
        result = run([str(reader), "--events-before", str(base + 120), str(path)], tree)
        assert result.stdout == f"{base + 90} 30\n", result.stdout
        good = json.loads(run([str(reader), str(base + 1), str(base + 19), str(base + 1000), str(path)], tree).stdout)
        gap = json.loads(run([str(reader), str(base + 200), str(base + 201), str(base + 1000), str(path)], tree).stdout)
        assert good["verdict"] == "complete", good
        assert gap["verdict"] == "incomplete" and "capture_loss" in gap["reasons"], gap
        print("live reader helper: PASS (exact DATA identity, certified window, capture_loss window)", flush=True)
    finally:
        shutil.rmtree(directory)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutants", action="store_true")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="pgwt-tcov-host-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "src", tree / "src")
        (tree / "tests").mkdir()
        for name in ["exact_perf_coverage_host.c", "coverage_trace_verdict.c", "test_percpu_transport.c", "test_core_merge_adapter.c",
                     "test_core_merge_contract.c", "merge_adapter_goldens.h"]:
            shutil.copy2(ROOT / "tests" / name, tree / "tests" / name)
        binary = build(tree)
        result = run([str(binary)], tree)
        print(result.stdout, end="", flush=True)
        reader_roundtrip(tree, binary)
        if args.mutants:
            for name, path, old, new, case, assertion in MUTANTS:
                file = tree / path
                original = file.read_text()
                assert original.count(old) == 1, (name, original.count(old))
                try:
                    file.write_text(original.replace(old, new))
                    result = run([str(build(tree)), case], tree, check=False)
                    assert f"REACHED T-cov {case}" in result.stdout, (name, result.stdout, result.stderr)
                    assert result.returncode == 1 and f"ASSERT {case}: {assertion}" in result.stderr, (
                        name, result.returncode, result.stdout, result.stderr)
                    print(f"MUTANT {name}: RED (built, reached {case}, failed: {assertion})", flush=True)
                finally:
                    file.write_text(original)
    print("T-cov host: PASS", flush=True)


if __name__ == "__main__":
    main()
