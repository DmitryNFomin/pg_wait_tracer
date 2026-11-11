#!/usr/bin/env python3
"""#863 mutation controls. Build/run only in a private temporary directory.

Requires the registered C tests and pgwt-server already built. Never replaces
those binaries, sources, or fixtures. Every mutation names the assertion that
must reject it; an unmodified isolated build must pass first.
"""
import os
import hashlib
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

from server_harness import ServerHarness

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
CC = shlex.split(os.environ.get("CC", "cc"))
FLAGS = ["-O2", "-Wall", "-Werror=implicit-function-declaration", "-DPGWT_SERVER",
         f"-I{ROOT / 'src'}", f"-I{ROOT / 'include'}",
         "-include", str(TESTS / "sample_weight_compat.h")]
if sys.platform == "darwin":
    prefix = subprocess.check_output(["brew", "--prefix"], text=True).strip()
    FLAGS += [f"-I{prefix}/include", f"-L{prefix}/lib",
              "-include", str(ROOT / "demos/replay-compat.h")]


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], cwd=ROOT,
                          capture_output=True, text=True, **kwargs)


def checked(args):
    result = run(args)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result


def replace(source, old, new):
    assert source.count(old) == 1, (old, source.count(old))
    return source.replace(old, new)


def main():
    shared = {path: hashlib.sha256(path.read_bytes()).digest() for path in
              (ROOT / "pgwt-server", TESTS / "test_sample_weight", TESTS / "gen_test_traces")}
    sources = {name: (ROOT / "src" / f"{name}.c").read_text()
               for name in ("sampler", "event_writer", "event_reader", "replay_accum", "server")}
    with tempfile.TemporaryDirectory(prefix="pgwt-weight-controls-") as directory:
        tmp = Path(directory)
        (tmp / "fixtures").symlink_to(TESTS / "fixtures", target_is_directory=True)

        def obj(label, name, source):
            path = tmp / f"{label}-{name}.c"
            path.write_text(source)
            output = path.with_suffix(".o")
            checked(CC + FLAGS + ["-c", path, "-o", output])
            return output

        def weight(label, changes):
            sampler = tmp / f"{label}-sampler.c"
            sampler.write_text(changes.get("sampler", sources["sampler"]))
            objects = {}
            for name in ("event_writer", "event_reader", "replay_accum"):
                objects[name] = (obj(label, name, changes[name]) if name in changes else
                                 (TESTS / "replay_accum.o" if name == "replay_accum" else
                                  ROOT / "build" / f"server_{name}.o"))
            output = tmp / f"weight-{label}"
            checked(CC + FLAGS + ["-DPGWT_SAMPLE_ACCUM_TEST", TESTS / "test_sample_weight.c",
                    sampler, ROOT / "src/idle_rule.c", *objects.values(),
                    TESTS / "map_reader_core.o", TESTS / "replay_compute_core.o",
                    *[ROOT / "build" / f"server_{name}.o" for name in
                      ("wait_event", "spawn", "cJSON", "summary_reader", "summary_writer")],
                    "-llz4", "-lz", "-lm", "-o", output])
            return output

        def reject(label, command, names):
            result = run(command)
            output = result.stdout + result.stderr
            assert result.returncode == 1, (label, result.returncode, output)
            for name in names:
                assert f"FAIL: {name}" in output, (label, name, output)
            print(f"{label}: rejected by {', '.join(names)}", flush=True)

        baseline = weight("baseline", {})
        checked([baseline])
        checked([sys.executable, TESTS / "test_data_fidelity.py"])
        sampler, writer, reader, replay, server = (sources[k] for k in
            ("sampler", "event_writer", "event_reader", "replay_accum", "server"))
        cases = [
            ("floor", {"sampler": replace(sampler, "    uint64_t elapsed = now_ns - last_tick_ns;",
                "    uint64_t elapsed = now_ns - last_tick_ns;\n    if (elapsed < nominal_ns) return nominal_ns;")},
             ["jitter_sum_10000", "stream_elapsed_equals_credited_empty_and_stall_excess"]),
            ("first_restart", {"sampler": replace(sampler, "    if (last_tick_ns == 0 || now_ns <= last_tick_ns)",
                "    if (now_ns <= last_tick_ns)")},
             ["first_tick_nominal_edge", "restart_does_not_credit_gap"]),
            ("no_clamp", {"sampler": replace(sampler,
                "    if (elapsed > 60ULL * 1000000000ULL)\n        return 60ULL * 1000000000ULL;   /* sanity clamp on absurd stalls */\n", "")},
             ["stall_capped_at_60s", "stream_elapsed_equals_credited_empty_and_stall_excess"]),
            ("tick_clock", {"sampler": replace(sampler, "    s->last_tick_ns = tick_ts;", "    (void)tick_ts;")},
             ["empty_failed_tick_advances_clock", "stream_elapsed_equals_credited_empty_and_stall_excess"]),
            ("one_block_per_tick", {"event_writer": replace(writer,
                "ts <= last || sample_period_ns != ts - last",
                "ts <= last || sample_period_ns != w->sample_buf_period_ns")},
             ["writer_batches_jittered_ticks"]),
            ("gap_ignored", {"event_writer": replace(writer,
                "ts <= last || sample_period_ns != ts - last", "ts <= last")},
             ["stream_every_sample_weight_exact", "stream_empty_and_clamp_start_new_blocks"]),
            ("nominal_not_durable", {"event_writer": replace(writer,
                "fwrite(&w->nominal_sample_period_ns, sizeof(uint64_t), 1, w->fp)",
                "fwrite(&w->sample_buf_period_ns, sizeof(uint64_t), 1, w->fp)")},
             ["stream_durable_nominal_cadence",
              "nominal_metadata_survives_rotation_recovery_and_new_capture"]),
            ("mean_reader", {"event_reader": replace(reader,
                "r->header.version >= 4 && out[i].timestamp_ns != tick_ts", "0 && out[i].timestamp_ns != tick_ts")},
             ["writer_preserves_each_tick_weight", "stream_reader_equals_live_integer_sum"]),
            ("sample_lookahead_lost", {"event_reader": replace(reader,
                "return hdr.first_timestamp_ns - to_mono_ns <= hdr.max_sample_weight_ns;",
                "return 0;")}, ["writer_reader_sample_lookahead"]),
            ("legacy_rederived", {"event_reader": replace(reader,
                "r->header.version >= 4 && out[i].timestamp_ns != tick_ts", "r->header.version >= 2 && out[i].timestamp_ns != tick_ts")},
             ["legacy_stored_inflated_mean_weights_retained"]),
            ("replay_mean", {"replay_accum": replace(replay,
                "uint64_t sample_ns = evt->duration_ns;",
                "uint64_t sample_ns = bi ? bi->sample_period_ns : 100000000ULL;" )
                .replace("occ_ns = sample_ns;", "occ_ns = add_ns = sample_ns;")},
             ["stream_cli_replay_equals_live_integer_sum", "stream_windows_exact_and_additive_cli"]),
        ]
        for label, changes, names in cases:
            reject(label, [weight(label, changes)], names)

        server_objects = [ROOT / "build" / f"server_{name}.o" for name in (
            "server", "compute", "backend_span", "open_overlay", "block_agg",
            "event_reader", "event_writer", "summary_writer", "summary_reader",
            "wait_event", "idle_rule", "spawn", "cmdline", "backend_meta", "cJSON")]

        def server_test(label, changes, names):
            objects = list(server_objects)
            for name, source in changes.items():
                index = objects.index(ROOT / "build" / f"server_{name}.o")
                objects[index] = obj(label, name, source)
            binary = tmp / f"server-{label}"
            checked(CC + FLAGS + objects + ["-llz4", "-lz", "-lm", "-o", binary])
            # Module overrides stay inside the child; fixture writer is an
            # isolated binary too. The harness launches only this server.
            fixture = weight(label + "-fixture", changes)
            script = ("import sys; sys.path.insert(0, sys.argv[1]); "
                      "import server_harness; server_harness.SERVER_BIN=sys.argv[2]; "
                      "import test_data_fidelity as t; from pathlib import Path; "
                      "t.WEIGHT_BIN=Path(sys.argv[3]); t.main()")
            reject(label, [sys.executable, "-c", script, TESTS, binary, fixture], names)

        server_test("lookahead_first", {"event_writer": replace(writer,
            "? w->sample_buf_max_period_ns : 0;", "? sample_period_ns : 0;")},
            ["maximum lookahead storage=current cache=0 request=0: DB Time",
             "maximum lookahead storage=archive cache=1 request=1: DB Time"])
        v4_dir = tmp / "cadence-v4"
        v4_dir.mkdir()
        checked([baseline, "--lookahead-fixture", v4_dir])
        v3_dir = tmp / "cadence-v3"
        v3_dir.mkdir()
        shutil.copyfile(TESTS / "fixtures/sample_weights_v3/current.trace",
                        v3_dir / "current.trace")
        # write_lookahead_fixture configures 10 Hz despite jitter/stalls.
        # The frozen v3 fixture's last block stores 110 ms (see its README).
        # Both expectations come from fixture construction, not server output.
        for version, trace_dir, expected_period in (
                (4, v4_dir, 1_000_000_000 // 10),
                (3, v3_dir, 110_000_000)):
            (trace_dir / "backends.jsonl").write_text(
                '{"pid":1000,"type":"client","user":"test","db":"testdb"}\n')
            for cache in (0, 1):
                with ServerHarness(str(trace_dir), env={"PGWT_CURRENT_TRACE_CACHE": cache}) as srv:
                    for repeat in range(2):
                        response = srv.query("time_model")
                        assert response.get("sample_period_ns") == expected_period, (
                            version, cache, repeat, expected_period, response)
            print(f"v{version} cadence: sample_period_ns={expected_period}", flush=True)
        server_test("file_cache_mean", {"server": replace(server,
            "/* Reader already supplied this tick's exact duration_ns. */",
            "ce->events[ce->count].duration_ns = bi.sample_period_ns;")},
            ["maximum lookahead storage=archive cache=1 request=1: DB Time"])
        server_test("cache_mean", {"server": replace(server,
            "/* Preserve the reader-derived tick weight. */",
            "(*events)[*total].duration_ns = bi.sample_period_ns;")},
            ["maximum lookahead storage=current cache=1 request=1: DB Time"])
        assert all(hashlib.sha256(path.read_bytes()).digest() == digest
                   for path, digest in shared.items()), "shared binary was modified"
        print("sample_weight_controls: all mutations rejected; shared binaries untouched")


if __name__ == "__main__":
    main()
