#!/usr/bin/env python3
"""T-cov production capture/writer/verdict schedules and executable mutants.

No eBPF work. All copied sources, binaries and traces live outside the repo.
A mutant counts only after a successful build and its intended assertion.
"""
import argparse
import ast
import contextlib
import io
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

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
    ("idle_default_off", "src/pg_wait_tracer.c",
     "    bool idle_round = true;", "    bool idle_round = false;",
     "idle_on", "idle round requests E/L"),
    ("idle_opt_out_ignored", "src/pg_wait_tracer.c",
     "            idle_round = false;", "            idle_round = true;",
     "idle_off", "opted-out idle ticks neither wake helpers nor promote W"),
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
    ("count_only_open", "src/merge_capture.c",
     "        if (c->coverage_begun) {", "        if (c->coverage_begun && false) {",
     "later_open", "unlisted backend holds existing gap open"),
    ("count_only_closed", "src/merge_capture.c",
     "        if (c->coverage_begun) {", "        if (c->coverage_begun && false) {",
     "later_closed", "unlisted backend reopens closed gap"),
    ("count_only_empty", "src/merge_capture.c",
     "        if (c->coverage_begun) {", "        if (c->coverage_begun && false) {",
     "later_empty", "unlisted backend opens initially absent gap"),
    ("idle_promote_without_acks", "src/open_snapshot.c",
     "    in->positions_known = in->capture_healthy;",
     "    in->certified_through_ns = now_ns();\n    in->positions_known = in->capture_healthy;",
     "idle_ack", "withheld acknowledgement prevents W promotion"),
    ("idle_release_without_acks", "src/percpu_transport.c",
     "    for (size_t i = 0; i < t->ncpu; ++i) if (!t->acks[i]) return 0;",
     "    /* missing CPU acknowledgements ignored */",
     "idle_ack", "withheld acknowledgement prevents boundary release"),
    ("idle_round_under_load", "src/percpu_transport.c",
     "enabled && frontiers == t->coverage_tick_frontiers &&",
     "enabled &&",
     "idle_loaded", "loaded ticks add zero rounds"),
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


def cli_binding(tree):
    """Compile the production argument parser, stopping before discovery/BPF.

    Feed its default and explicit opt-out into the daemon schedules, so a
    changed CLI default cannot hide behind a hand-initialised test bool.
    """
    source = (tree / "src/pg_wait_tracer.c").read_text()
    prefix = source[:source.index("    /* #500:")]
    prefix += """
    (void)pm_pid; (void)pgdata; (void)format_set; (void)view_set;
    (void)replay_mode; (void)daemon_mode; (void)from_str; (void)to_str;
    printf("%d %d\\n", d->exact_perf, d->exact_perf_idle_round);
    free(d);
    return 0;
}
int pgwt_percpu_transport_prepare(struct pgwt_daemon *d)
{ (void)d; return 0; }
"""
    unit = tree / "cli.c"
    unit.write_text(prefix)
    binary = tree / "coverage-cli"
    run([*CC, *FLAGS, str(unit), "-o", str(binary)], tree)
    values = []
    for options in (["--exact-perf"], ["--exact-perf", "--no-exact-perf-idle-round"]):
        result = run([str(binary), *options], tree)
        exact, idle = map(int, result.stdout.split())
        assert exact == 1 and idle in (0, 1), result
        values.append(idle)
    (tree / "tests/exact_perf_coverage_cli.h").write_text(
        f"#define TCOV_IDLE_DEFAULT {values[0]}\n#define TCOV_IDLE_OFF {values[1]}\n")
    return binary


def cli_controls(tree, binary):
    on, off = "--exact-perf-idle-round", "--no-exact-perf-idle-round"
    for options, expected in [([], "0 0\n"), (["--exact-perf"], "1 1\n"),
                              (["--exact-perf", on], "1 1\n"),
                              ([off, "--exact-perf"], "1 0\n"),
                              (["--exact-perf", off, on], "1 1\n"),
                              (["--exact-perf", on, off], "1 0\n")]:
        assert run([str(binary), *options], tree).stdout == expected, options
    for option in (on, off):
        result = run([str(binary), option], tree, check=False)
        assert result.returncode == 1 and f"{option} requires --exact-perf" in result.stderr, result
    help_text = run([str(binary), "--help"], tree).stderr
    assert any(on in line and "default on" in line for line in help_text.splitlines()), help_text
    assert off in help_text, help_text
    print("idle CLI: PASS (default, opt-out, compatibility flag, option order, exact_perf requirement, help)", flush=True)


def build(tree):
    cli_binding(tree)
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


def live_helpers(tree):
    """Execute the live CPU parser and both cost arms with a virtual clock.

    Real stat files include a comm containing spaces and ')'. Helpers use
    3% of one core here, deliberately above the suggested cost budget; the
    measurement must report that value and exclude a busy reader thread.
    """
    source = ast.parse((ROOT / "tests/test_exact_perf_coverage_live.py").read_text())
    start_function = next(node for node in source.body if isinstance(node, ast.FunctionDef)
                          and node.name == "start_daemon")
    commands = []
    namespace = dict(os=os, shlex=shlex, w_now=lambda trace: 1,
                     wait_for=lambda *args: None,
                     subprocess=SimpleNamespace(DEVNULL=-3, Popen=lambda command, **kwargs: commands.append(command)))
    exec(compile(ast.Module(body=[start_function], type_ignores=[]), "<live start controls>", "exec"), namespace)
    with contextlib.redirect_stdout(io.StringIO()):
        for option in (None, True, False):
            trace = tree / f"live-start-{option}"
            kwargs = {} if option is None else dict(idle_round=option)
            _, error, _ = namespace["start_daemon"]("tracer", 1, trace, **kwargs)
            error.close()
            command = commands[-1]
            assert "--exact-perf" in command and "--exact-perf-idle-round" not in command, command
            assert ("--no-exact-perf-idle-round" in command) == (option is False), command
    functions = [node for node in source.body if isinstance(node, ast.FunctionDef)
                 and node.name in ("task_cpu_ticks", "idle_cost_gate")]
    assert len(functions) == 2
    root = tree / "live-helper-controls"
    root.mkdir()
    tasks = root / "task"
    tasks.mkdir()
    clock = SimpleNamespace(now=0.0, started=0.0)
    samples = []

    def task_path(value):
        assert value == "/proc/101/task", value
        elapsed = int(clock.now - clock.started)
        samples.append(elapsed)
        for tid, rate in ((101, 10000), (201, 1), (202, 2)):
            directory = tasks / str(tid)
            directory.mkdir(exist_ok=True)
            fields = ["S"] + ["0"] * 10 + [str(10 + elapsed * rate), "20"] + ["0"] * 10
            (directory / "stat").write_text(f"{tid} (comm with ) spaces) " + " ".join(fields))
        return tasks

    class Process:
        pid = 101
        returncode = None

        def poll(self):
            return self.returncode

    def start(binary, pm, trace, *, idle_round):
        assert pm == 1 and idle_round == trace.name.endswith("on")
        trace.mkdir()
        clock.started = clock.now
        log = trace / "daemon.stderr"
        log.write_text("\n".join(f"exact-perf: schedstat role=helper tid={tid} cpu=0" for tid in (201, 202)))
        return Process(), io.StringIO(), log

    def sleep(seconds):
        assert seconds == 1
        clock.now += seconds

    namespace = dict(Path=task_path, os=SimpleNamespace(sysconf=lambda key: 100 if key == "SC_CLK_TCK" else None),
                     time=SimpleNamespace(monotonic=lambda: clock.now, sleep=sleep), re=re, SEC=1_000_000_000,
                     TRACER="virtual", start_daemon=start,
                     stop=lambda process: setattr(process, "returncode", 0),
                     w_now=lambda trace: 1000 + (int((clock.now-clock.started)*1_000_000_000)
                                                 if trace.name.endswith("on") else 0),
                     qb_prefix=lambda path: ({}, 0, []))
    exec(compile(ast.Module(body=functions, type_ignores=[]), "<live cost controls>", "exec"), namespace)
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        namespace["idle_cost_gate"](1, root)
    lines = [line for line in output.getvalue().splitlines() if line.startswith("IDLE-COST:")]
    assert lines == ["IDLE-COST: option=off helper_cpu_pct_of_one_core=3.000000 w_advance_s=0.000000",
                     "IDLE-COST: option=on helper_cpu_pct_of_one_core=3.000000 w_advance_s=60.000000"], lines
    assert samples == [0, 60, 0, 60], samples
    print("idle live helpers: PASS (default/opt-out commands, stat fields, helper-only CPU, both 60 s arms, cost reported without threshold)", flush=True)


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
        cli_controls(tree, tree / "coverage-cli")
        result = run([str(binary)], tree)
        print(result.stdout, end="", flush=True)
        reader_roundtrip(tree, binary)
        live_helpers(tree)
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
