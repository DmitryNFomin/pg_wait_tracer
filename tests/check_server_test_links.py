#!/usr/bin/env python3
"""Mac gate for server test link closure; no compiler, BPF or Linux required.

Inspect make's expanded commands, not just prerequisites. Record the real Python
builders' compiler calls without executing them. Both sanitizer flag variants
must carry every non-main source from the product's SERVER_SRCS.
"""
import argparse
import contextlib
import io
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from server_build import ROOT, server_sources


def make_targets():
    includers = {p.name for p in (ROOT / "tests").glob("*.c")
                 if re.search(r'^\s*#\s*include\s+"[^"\n]*server\.c"',
                              p.read_text(), re.MULTILINE)}
    # The serializer is built by the Python pilot builder, exercised below.
    includers.remove("pilot_serializer_probe.c")
    text = (ROOT / "tests/Makefile").read_text().replace("\\\n", " ")
    targets = []
    covered = set()
    for line in text.splitlines():
        match = re.match(r"^([^\s#:=][^:=]*):\s+(.*)$", line)
        if match:
            sources = includers.intersection(match[2].split())
            if sources or "../src/server.c" in match[2].split():
                targets.extend(match[1].split())
                covered.update(sources)
    if not targets or covered != includers:
        raise AssertionError(f"unregistered server includers: {sorted(includers - covered)}")
    return targets


def require_sources(label, actual, expected):
    missing = set(expected) - set(actual)
    if missing:
        raise AssertionError(f"{label}: missing " + ", ".join(sorted(missing)))


def make_links(tmp, targets, expected):
    # Supply already-built object prerequisites only. Never invoke the parent
    # Makefile's Linux probe or compile anything; make still expands each real
    # target's own recipe and automatic variables.
    stubs = tmp / "objects.mk"
    stubs.write_text(".PHONY: force-server-link-check\n" +
                     " ".join("../build/server_" + s.removeprefix("src/").removesuffix(".c") + ".o"
                              for s in expected) + ": ;\n")
    for flags in ("-O2", "-O1 -fsanitize=address,undefined -fno-omit-frame-pointer"):
        for target in targets:
            result = subprocess.run(
                ["make", "--no-print-directory", "-nB", "-f", "Makefile", "-f", str(stubs),
                 "CC=cc", "CFLAGS=" + flags, target], cwd=ROOT / "tests",
                capture_output=True, text=True, env={**os.environ, "MAKEFLAGS": ""})
            if result.returncode:
                raise AssertionError(f"make dry-run {target}: {result.stdout}{result.stderr}")
            links = [shlex.split(line) for line in result.stdout.replace("\\\n", " ").splitlines()
                     if re.search(r"-o\s+" + re.escape(target) + r"(?:\s|$)", line)]
            if len(links) != 1:
                raise AssertionError(f"expected one link command for {target}: {result.stdout}")
            actual = []
            for token in links[0]:
                if token.startswith("../build/server_") and token.endswith(".o"):
                    actual.append("src/" + token[len("../build/server_"):-2] + ".c")
                elif token.startswith("../src/") and token.endswith(".c"):
                    actual.append(token[3:])
            require_sources("tests/Makefile:" + target, actual, expected)
    print(f"PASS: tests/Makefile {len(targets)} expanded link recipes, plain + ASan/UBSan")


def python_links(tmp, expected):
    import test_pilot_route as pilot
    import test_sample_weight_controls as weight

    for flags in ("-O2", "-O1 -fsanitize=address,undefined -fno-omit-frame-pointer"):
        calls = []
        objects = {}

        def record(args, **kwargs):
            args = list(map(str, args))
            calls.append(args)
            if "-c" in args:
                objects[args[args.index("-o") + 1]] = args[args.index("-c") + 1]
            return SimpleNamespace(stdout="/mock-brew\n" if args[0] == "brew" else "")

        with patch.object(pilot, "command", record), patch.dict(os.environ, CFLAGS=flags):
            build = pilot.Build(tmp)
            build.link("mutant", {"src/server.c": "/* compiler is recorded only */"})
            with contextlib.redirect_stdout(io.StringIO()):
                pilot.serializer_identity(build, tmp)
        # Every executable link in the pilot builder other than its fixture
        # writer must retain the full server closure, including serializer and
        # replacement-object mutation links.
        links = [c for c in calls if "-o" in c and "-c" not in c
                 and Path(c[c.index("-o") + 1]).name != "writer"]
        if len(links) != 3:
            raise AssertionError(f"unexpected pilot link inventory: {links}")
        for call in links:
            actual = [objects.get(t, t) for t in call]
            require_sources("test_pilot_route.py:" + Path(call[call.index("-o") + 1]).name,
                            actual, [str(ROOT / s) for s in expected])

    calls = []
    with patch.object(weight, "checked", lambda args: calls.append(list(map(str, args)))):
        weight.build_server("baseline", {}, None, tmp)
        weight.build_server("mutant", {"server": "/* recorded */"},
                            lambda *args: tmp / "mutant-server.o", tmp)
    for call in calls:
        require_sources("test_sample_weight_controls.py:build_server", call,
                        [str(ROOT / "build" / ("server_" + s[4:-2] + ".o")) for s in expected])
    print("PASS: Python pilot server/serializer/mutant links (plain + ASan/UBSan), sample-weight links")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    expected = [str(s.relative_to(ROOT)) for s in server_sources() if s != ROOT / "src/server.c"]
    targets = make_targets()
    with tempfile.TemporaryDirectory(prefix="pgwt-server-links-") as directory:
        tmp = Path(directory)
        make_links(tmp, targets, expected)
        python_links(tmp, expected)
        if args.self_test:
            # Mutate a single target's link command in an external copy of the
            # recipe. Leave prerequisites intact: checking those alone lies.
            text = (ROOT / "tests/Makefile").read_text()
            old = "$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $< $(TRANSITIONS_LOADER_OBJS)"
            new = "$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $< $(filter-out $(BUILD)/server_core/evidence_query.o,$(TRANSITIONS_LOADER_OBJS))"
            if old not in text:
                raise AssertionError("self-test target recipe changed")
            mutant = tmp / "Makefile"
            mutant.write_text(text.replace(old, new, 1))
            # Reuse the same guard path, with make reading the external copy.
            original_run = subprocess.run

            def mutated_run(command, **kwargs):
                command = [str(mutant) if v == "Makefile" else v for v in command]
                return original_run(command, **kwargs)

            try:
                with patch.object(subprocess, "run", mutated_run):
                    make_links(tmp, targets, expected)
            except AssertionError as exc:
                if str(exc) != "tests/Makefile:test_transitions_loader: missing src/core/evidence_query.c":
                    raise
                print(f"negative control: server test link closure: FAIL: {exc}")
                print("PASS: removed-one-file negative control rejected")
            else:
                raise AssertionError("removed-one-file negative control survived")
    print("server test link closure: PASS")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, ValueError, OSError) as exc:
        raise SystemExit(f"server test link closure: FAIL: {exc}")
