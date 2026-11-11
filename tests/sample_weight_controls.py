#!/usr/bin/env python3
"""#863 negative controls (manual, after building pgwt-server/test_sample_weight).

Run: python3 tests/sample_weight_controls.py
Compiles mutated copies in a temporary directory; restores the two ignored
executables even on failure. Never edits production sources or Git state.
"""
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
CC = shlex.split(os.environ.get("CC", "cc"))
FLAGS = ["-O2", "-Wall", "-Werror=implicit-function-declaration", "-DPGWT_SERVER",
         f"-I{ROOT / 'src'}", f"-I{ROOT / 'include'}",
         "-include", str(TESTS / "sample_weight_compat.h")]
LIBS = ["-llz4", "-lz", "-lm"]
if platform.system() == "Darwin":
    prefix = subprocess.check_output(["brew", "--prefix"], text=True).strip()
    FLAGS += [f"-I{prefix}/include", f"-L{prefix}/lib",
              "-include", str(ROOT / "demos/replay-compat.h")]


def run(args):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True,
                          capture_output=True, text=True)


def replace_once(source, old, new):
    assert source.count(old) == 1, old
    return source.replace(old, new)


def reject(label, command, names):
    result = subprocess.run([str(a) for a in command], cwd=ROOT,
                            capture_output=True, text=True)
    output = result.stdout + result.stderr
    assert result.returncode == 1, (label, result.returncode, output)
    for name in names:
        assert f"FAIL: {name}" in output, (label, name, output)
    print(f"{label}: rejected (exit 1)")
    for line in output.splitlines():
        if ("FAIL:" in line or "checks passed" in line or "tests passed" in line
                or "weight=" in line):
            print(f"  {line.strip()}")


def main():
    sampler = (ROOT / "src/sampler.c").read_text()
    writer = (ROOT / "src/event_writer.c").read_text()
    server = (ROOT / "src/server.c").read_text()
    weight_bin = TESTS / "test_sample_weight"
    server_bin = ROOT / "pgwt-server"
    with tempfile.TemporaryDirectory(prefix="pgwt-weight-controls-") as tmp:
        tmp = Path(tmp)
        shutil.copy2(weight_bin, tmp / "weight.saved")
        shutil.copy2(server_bin, tmp / "server.saved")

        def build_weight(sample_source, writer_source=None):
            (tmp / "sampler.c").write_text(sample_source)
            writer_obj = ROOT / "build/server_event_writer.o"
            if writer_source is not None:
                (tmp / "event_writer.c").write_text(writer_source)
                writer_obj = tmp / "writer.o"
                run(CC + FLAGS + ["-c", tmp / "event_writer.c", "-o", writer_obj])
            run(CC + FLAGS + [TESTS / "test_sample_weight.c", tmp / "sampler.c",
                ROOT / "src/idle_rule.c", writer_obj,
                ROOT / "build/server_event_reader.o", "-o", weight_bin] + LIBS)

        try:
            # Final tests against the unmodified origin/master implementation.
            original_sampler = run(["git", "show", "origin/master:src/sampler.c"]).stdout
            original_writer = run(["git", "show", "origin/master:src/event_writer.c"]).stdout
            build_weight(original_sampler, original_writer)
            reject("origin/master C red", [weight_bin],
                   ["jitter_sum_10000", "writer_preserves_each_tick_weight"])
            reject("origin/master server red", ["python3", TESTS / "test_data_fidelity.py"],
                   ["measured ticks: 60s active backend = 60000ms DB Time",
                    "measured ticks: 60s active backend = AAS 1.0"])

            floor = replace_once(sampler, "    uint64_t elapsed = now_ns - last_tick_ns;",
                "    uint64_t elapsed = now_ns - last_tick_ns;\n"
                "    if (elapsed < nominal_ns) return nominal_ns;")
            build_weight(floor)
            reject("restored floor", [weight_bin], ["jitter_sum_10000"])

            nominal = replace_once(sampler,
                "    return elapsed;\n}\n\nuint64_t pgwt_sampler_tick_lateness",
                "    return nominal_ns;\n}\n\nuint64_t pgwt_sampler_tick_lateness")
            build_weight(nominal)
            reject("late interval lost", [weight_bin], ["five_period_gap_sum"])

            no_cap = replace_once(sampler,
                "    if (elapsed > 60ULL * 1000000000ULL)\n"
                "        return 60ULL * 1000000000ULL;   /* sanity clamp on absurd stalls */\n", "")
            build_weight(no_cap)
            reject("removed stall cap", [weight_bin],
                   ["stall_capped_at_60s", "stall_remainder_uncredited"])

            edge = replace_once(sampler,
                "    if (last_tick_ns == 0 || now_ns <= last_tick_ns)\n        return nominal_ns;",
                "    if (last_tick_ns == 0) return 60ULL * 1000000000ULL;\n"
                "    if (now_ns <= last_tick_ns) return 0;")
            build_weight(edge)
            reject("invented restart gap and invalid clock", [weight_bin],
                   ["first_tick_nominal_edge", "restart_does_not_credit_gap",
                    "equal_clock_nominal_fallback", "backwards_clock_nominal_fallback"])

            averaged = replace_once(writer, "            sample_period_ns != p0)",
                "            (sample_period_ns > p0 ? sample_period_ns - p0 : "
                "p0 - sample_period_ns) > p0 / 64)")
            build_weight(sampler, averaged)
            reject("restored writer averaging", [weight_bin],
                   ["writer_preserves_each_tick_weight", "writer_preserves_integer_sum"])

            shutil.copy2(tmp / "weight.saved", weight_bin)
            before, after = server.split("                bool contributed = false;", 1)
            after = after.replace("if (gc && gc->n_exact > 0)",
                                  "if (false && gc && gc->n_exact > 0)", 1)
            assert before + "                bool contributed = false;" + after != server
            (tmp / "server.c").write_text(before + "                bool contributed = false;" + after)
            objects = sorted((ROOT / "build").glob("server_*.o"))
            objects = [p for p in objects if p.name != "server_server.o"]
            run(CC + FLAGS + [tmp / "server.c"] + objects + ["-o", server_bin] + LIBS)
            reject("removed exact coverage subtraction",
                   ["python3", TESTS / "test_data_fidelity.py"],
                   ["post-escalation tick: 60ms exact + 40ms sampled",
                    "post-escalation tick: AAS 1.0"])
        finally:
            shutil.copy2(tmp / "weight.saved", weight_bin)
            shutil.copy2(tmp / "server.saved", server_bin)
    print("sample_weight_controls: all negative controls rejected")


if __name__ == "__main__":
    main()
