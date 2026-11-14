#!/usr/bin/env python3
"""#871 increment/validity mutations and exhaustive named guard sweep.

A kill requires a successful host compile, execution and the intended failing
assertion. Source and tests are never edited: builds live in temporary dirs.
--guard-sweep removes every selected accounting increment independently,
including all five WAIT_READ paths and the phase re-lookup miss.
"""
from pathlib import Path
import argparse
import contextlib
import io
import os
import re
import shlex
import subprocess
import tempfile

from test_counter_failures import ROOT, compile_host, function


def run_result(binary, expected):
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    output = result.stdout + result.stderr
    if result.returncode == 0 or expected not in output:
        raise RuntimeError(f"mutant survived or failed wrong assertion (wanted {expected!r}): {output}")


def bpf_mutants():
    source = (ROOT / "src/bpf/pg_wait_tracer.bpf.c").read_text()
    cases = (
        ("count_map_fail", "__sync_fetch_and_add(c, 1);", ("wait zero address",)),
        ("note_query_edge", "count_map_fail(PGWT_BPF_FAIL_EXACT_QUERY_UPDATE);", ("exact query insert error",)),
        ("note_cmd_edge", "count_map_fail(PGWT_BPF_FAIL_EXACT_CMD_UPDATE);", ("exact cmd insert error",)),
        ("note_phase_edge", "count_map_fail(PGWT_BPF_FAIL_EXACT_PHASE_UPDATE);",
         ("exact phase insert error", "exact phase relookup miss")),
        ("read_wait_event_direct", "count_map_fail(PGWT_BPF_FAIL_WAIT_READ);",
         ("wait direct read error", "wait zero address")),
        ("read_wait_event", "count_map_fail(PGWT_BPF_FAIL_WAIT_READ);",
         ("wait pointer read error", "wait indirect read error")),
        ("on_bootstrap", "count_map_fail(PGWT_BPF_FAIL_WAIT_READ);", ("wait bootstrap pointer error",)),
        ("accum_add", "count_map_fail(PGWT_BPF_FAIL_ACCUM_UPDATE);", ("accum insert error",)),
        ("on_fork", "count_map_fail(PGWT_BPF_FAIL_FORK_STATE_UPDATE);", ("fork state insert error",)),
        ("on_report_query_id", "count_ring_fail(PGWT_RING_FAIL_QUERY_TEXT);",
         ("text reserve error", "query text read outcome")),
        ("on_executor_start", "count_ring_fail(PGWT_RING_FAIL_PG13_QUERY_TEXT);",
         ("text reserve error", "pg13 text read outcome")),
    )
    for name, needle, expected in cases:
        body = function(source, name)
        matches = list(re.finditer(re.escape(needle), body))
        if len(matches) != len(expected):
            raise RuntimeError(f"stale increment anchors for {name}: {len(matches)} != {len(expected)}")
        for index, (match, assertion) in enumerate(zip(matches, expected)):
            mutant = body[:match.start()] + "(void)0;" + body[match.end():]
            yield f"{name}-increment-{index + 1}", source.replace(body, mutant, 1), assertion


def compile_mutation(tmp, name, unit, source_file, body, extra=()):
    path = Path(tmp) / source_file
    path.write_text(body)
    with contextlib.redirect_stdout(io.StringIO()):
        # Compilation only: unlike compile_run, do not execute the mutant yet.
        binary = Path(tmp) / name
        cmd = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-parameter", *shlex.split(os.environ.get("CFLAGS", "")),
               "-I", str(ROOT / "src"), str(ROOT / "tests" / unit), str(path),
               *extra, "-o", str(binary)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"mutant did not build: {result.stdout}{result.stderr}")
    return binary


def one(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError(f"stale mutation anchor: {before!r}")
    return source.replace(before, after)


def main():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    parser = argparse.ArgumentParser()
    parser.add_argument("--guard-sweep", action="store_true")
    parser.parse_args()
    killed = 0
    # Establish green first, using exactly the harness every mutant runs.
    with tempfile.TemporaryDirectory(prefix="pgwt-counter-mutant-") as tmp:
        original = (ROOT / "src/bpf/pg_wait_tracer.bpf.c").read_text()
        subprocess.run([str(compile_host(original, tmp))], check=True)
        for name, source, assertion in bpf_mutants():
            binary = compile_host(source, tmp)
            run_result(binary, "FAIL " + assertion)
            killed += 1
            print(f"KILLED {name}: {assertion}")
        source = (ROOT / "src/counter_vector.c").read_text()
        vectors = (
            ("lookup-error-as-zero", "if (lookup(fd, &key, values) != 0)\n        return (struct pgwt_counter_value){0, false};",
             "if (lookup(fd, &key, values) != 0)\n        return (struct pgwt_counter_value){0, true};", "!v.valid"),
            ("missing-map-as-zero", "if (fd < 0 || ncpu <= 0 || !lookup)\n        return (struct pgwt_counter_value){0, false};",
             "if (fd < 0 || ncpu <= 0 || !lookup)\n        return (struct pgwt_counter_value){0, true};", ".valid"),
            ("overflow-as-zero", "if (!a.valid || UINT64_MAX - a.value < b)\n        return (struct pgwt_counter_value){0, false};",
             "if (!a.valid || UINT64_MAX - a.value < b)\n        return (struct pgwt_counter_value){0, true};", "!v.valid"),
            ("init-read-error-valid", "s->lifecycle_init_lost_total, s->init_state_read_fail_total == 0",
             "s->lifecycle_init_lost_total, true", "!v.valid"),
            ("vector-validity-ignored", "v.valid = v.valid && v.components[i].valid;",
             "v.valid = true;", "!v.valid"),
            ("init-no-opportunity-excluded", "s->lifecycle_init_lost_total, s->init_state_read_fail_total == 0",
             "0, s->init_state_read_fail_total == 0", "value == expected"),
        )
        for name, before, after, assertion in vectors:
            binary = compile_mutation(tmp, "vector", "test_counter_vector.c", "vector.c",
                                      one(source, before, after))
            run_result(binary, assertion)
            killed += 1
            print(f"KILLED {name}: {assertion}")
        # Ring metrics must not preserve either validity or a previous value.
        ring = (ROOT / "src/ringbuf_metrics.h").read_text()
        unit = (ROOT / "tests/test_ringbuf_metrics.c").read_text()
        for name, before, after, assertion in (
            ("ring-stale-aggregate", "m->ringbuf_drops_valid = c.valid;", "m->ringbuf_drops_valid = true;", "!m.ringbuf_drops_valid"),
            ("ring-stale-site", "m->ringbuf_site_valid[site] = c.valid;", "m->ringbuf_site_valid[site] = true;", "!m.ringbuf_site_valid"),
        ):
            header = Path(tmp) / "ringbuf_metrics.h"
            header.write_text(one(ring, before, after))
            test_unit = Path(tmp) / "ring.c"
            test_unit.write_text(unit) # quoted include finds the mutated header first
            binary = Path(tmp) / "ring"
            result = subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(test_unit),
                str(ROOT / "src/counter_vector.c"), "-o", str(binary)], capture_output=True, text=True)
            if result.returncode: raise RuntimeError(result.stderr)
            run_result(binary, assertion)
            killed += 1
            print(f"KILLED {name}: {assertion}")
        writer = (ROOT / "src/event_writer.c").read_text()
        extra = ["-llz4"]
        import sys
        if sys.platform == "darwin":
            prefix = subprocess.check_output(["brew", "--prefix", "lz4"], text=True).strip()
            extra = ["-I" + prefix + "/include", "-L" + prefix + "/lib", *extra]
        for name, func, before, after, assertion in (
            ("writer-sticky-rejection-removed", "pgwt_writer_push_event",
             "if (w->failed) return -1;", "if (w->failed) return 0;", "pgwt_writer_push_event_counted"),
            ("writer-count-removed", "pgwt_writer_push_event_counted",
             "++*writer_push_fail_total;", "(void)writer_push_fail_total;", "failures =="),
            ("sample-sticky-rejection-removed", "pgwt_writer_push_samples",
             "if (w->failed) return -1;", "if (w->failed) return 0;", "pgwt_writer_push_samples_counted"),
            ("sample-count-removed", "pgwt_writer_push_samples_counted",
             "++*writer_push_fail_total;", "(void)writer_push_fail_total;", "failures =="),
            ("sample-failure-not-sticky", "pgwt_writer_push_samples",
             "w->failed = true;", "w->failed = false;", "w->failed"),
            ("sample-flush-error-ignored", "pgwt_writer_push_samples",
             "if (flush_samples(w) != 0) goto fail;", "(void)flush_samples(w);", "pgwt_writer_push_samples_counted"),
        ):
            body = function(writer, func)
            binary = compile_mutation(tmp, "writer", "test_writer_failure.c", "writer.c",
                                      writer.replace(body, one(body, before, after), 1), extra)
            run_result(binary, assertion)
            killed += 1
            print(f"KILLED {name}: {assertion}")
        # Exercise the actual INIT read wrapper; removing its increment cannot
        # hide behind the vector's independently supplied fixture count.
        backend = (ROOT / "src/backend.c").read_text()
        mutant = one(backend, "d->counters.init_state_read_fail_total++;", "(void)d;")
        map_source = (ROOT / "src/capture_counters.c").read_text()
        bodies = [function(map_source, name) for name in (
            "pgwt_read_bpf_fail_counter", "pgwt_read_daemon_ring_site", "pgwt_daemon_counter_vector")]
        bodies.append(function(mutant, "read_init_state"))
        control = (ROOT / "src/control.c").read_text()
        bodies.append(function(control, "legacy_state_map_full"))
        daemon_unit = Path(tmp) / "daemon.c"
        daemon_unit.write_text((ROOT / "tests/counter_daemon_harness.c").read_text().replace(
            "/* PRODUCTION_FUNCTIONS */", "\n\n".join(bodies)))
        binary = Path(tmp) / "daemon"
        result = subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
            "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(daemon_unit),
            str(ROOT / "src/counter_vector.c"), "-o", str(binary)], capture_output=True, text=True)
        if result.returncode: raise RuntimeError(result.stderr)
        run_result(binary, "init_state_read_fail_total == 1")
        killed += 1
        print("KILLED init-read-error-uncounted: init_state_read_fail_total == 1")
        legacy = one(function(control, "legacy_state_map_full"),
                     "return (struct pgwt_counter_value){0, false};",
                     "return (struct pgwt_counter_value){0, true};")
        bodies[-2] = function(backend, "read_init_state")
        bodies[-1] = legacy
        daemon_unit.write_text((ROOT / "tests/counter_daemon_harness.c").read_text().replace(
            "/* PRODUCTION_FUNCTIONS */", "\n\n".join(bodies)))
        result = subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
            "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(daemon_unit),
            str(ROOT / "src/counter_vector.c"), "-o", str(binary)], capture_output=True, text=True)
        if result.returncode: raise RuntimeError(result.stderr)
        run_result(binary, "!legacy_state_map_full(&v).valid")
        killed += 1
        print("KILLED legacy-state-map-unknown-as-zero: !legacy_state_map_full(&v).valid")
    print(f"counter guard sweep: {killed}/{killed} mutants killed (built, reached, intended assertion)")


if __name__ == "__main__":
    main()
