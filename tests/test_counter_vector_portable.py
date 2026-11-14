#!/usr/bin/env python3
"""Mac/Linux capture accounting C units; build only host code into /tmp."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile
import contextlib
import io
from unittest.mock import patch

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


def run_phase_routing():
    # Run the actual live entry point and assertions with simulated metrics;
    # no Linux program or probe is invoked by this host-side routing check.
    import test_counter_vector_live as live
    import test_counter_vector_phase_live as phase
    from demo_rehearsal_lib import DAEMON_INTEGRITY_COUNTERS
    from testutil import PHASE_REFUSED_MARKER

    assert [fault for fault, _ in live.FAULTS] == [
        "wait_read", "exact_query", "exact_cmd", "accum", "fork_state"]

    def start_daemon(argv, *, stderr, env, **kwargs):
        assert argv[argv.index("--mode") + 1] == "full"
        assert env["PGWT_TEST_COUNTER_FAILURE"] == "exact_phase"
        assert env.get("PGWT_TEST_FORCE_NO_PHASE_PROBES") == forced
        assert env.get("PGWT_TEST_NO_USDT_SEMA_REFCNT") == ("1" if forced == "1" else None)
        assert "PGWT_TEST_UNRELATED" not in env
        stderr.write("WARN: PGWT_TEST_COUNTER_FAILURE=exact_phase (TEST ONLY)\n")
        stderr.flush()
        return object()

    for declared, forced, mask, failures, exit_code in (
        ("0", None, 3, 0, 78), ("0", None, 15, 0, 1), ("0", None, 3, 1, 1),
        ("1", None, 15, 1, 0), ("1", None, 3, 0, 1), ("1", None, 15, 0, 1),
        ("0", None, None, 0, 1),
        ("0", "1", 3, 0, 78), ("0", "1", 15, 0, 1), ("0", "1", 3, 1, 1),
        ("0", "0", 3, 0, 78),
    ):
        env = {k: v for k, v in os.environ.items() if not k.startswith("PGWT_TEST_")}
        env.update(PGWT_EXPECT_PHASE_PROBES=declared, PGWT_TEST_UNRELATED="1")
        if forced is not None:
            env["PGWT_TEST_FORCE_NO_PHASE_PROBES"] = forced
        metrics = dict.fromkeys(DAEMON_INTEGRITY_COUNTERS, 0)
        metrics.update(counter_vector_valid=True, exact_probe_attached_mask=mask,
                       exact_phase_update_fail_total=failures)
        output = io.StringIO()
        load = type("Load", (), {"returncode": 0, "stderr": "",
                    "stdout": "number of transactions actually processed: 100"})()
        with (patch.dict(os.environ, env, clear=True),
              patch.object(sys, "argv", ["test_counter_vector_phase_live.py", "--pid", "42"]),
              patch.object(live.subprocess, "Popen", start_daemon),
              patch.object(live.subprocess, "run", return_value=load),
              patch.object(live, "wait_for"), patch.object(live, "stop") as stop,
              patch.object(live, "ctl", return_value=metrics),
              contextlib.redirect_stdout(output)):
            actual = 0
            try:
                phase.main()
            except SystemExit as exc:
                actual = exc.code
            assert actual == exit_code, (declared, forced, mask, failures, output.getvalue())
            assert (PHASE_REFUSED_MARKER in output.getvalue()) == (exit_code == 78)
            stop.assert_called_once()
    def start_non_phase_daemon(argv, *, env, **kwargs):
        assert not any(k.startswith("PGWT_TEST_") for k in env), env
        return object()

    metrics = dict.fromkeys(DAEMON_INTEGRITY_COUNTERS, 0)
    metrics.update(counter_vector_valid=True)
    with (patch.dict(os.environ, {"PGWT_TEST_FORCE_NO_PHASE_PROBES": "1",
                                  "PGWT_TEST_NO_USDT_SEMA_REFCNT": "1",
                                  "PGWT_TEST_UNRELATED": "1"}),
          patch.object(live.subprocess, "Popen", start_non_phase_daemon),
          patch.object(live.subprocess, "run", return_value=load),
          patch.object(live, "wait_for"), patch.object(live, "stop") as stop,
          patch.object(live, "ctl", return_value=metrics),
          contextlib.redirect_stdout(io.StringIO())):
        live.run_arm(42, None)
        stop.assert_called_once()
    print("live phase routing (mock process/metrics): PASS "
          "(pass, refusal, forced-no-phase rehearsal exit 78 + marker, "
          "contradictory/missing evidence, non-phase environment isolation)")


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-counter-units-") as tmp:
        run_units(tmp)
        run_daemon_reads(tmp)
    run_phase_routing()


if __name__ == "__main__":
    main()
