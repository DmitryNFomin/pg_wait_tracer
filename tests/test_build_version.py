#!/usr/bin/env python3
"""#572: compare the stamps passed by real make/script build recipes.

Runs on macOS and Linux, directly or via unit_tests.list (CI build-and-unit
and the box suite). Both Darwin and Linux script branches run on either host;
only uname, brew and the compiler/Go executable are substitutes. Git, make,
bash and the version derivation are real. No BPF, Homebrew, C or Go toolchain
is needed. This is not registered in the local make-check script.

An inherited PGWT_BUILD_VERSION can hide the original bug. Each case starts
with an isolated environment and repository, including a tagged commit one
commit behind HEAD (so describe cannot equal rev-parse), dirty and untagged
trees, explicit/empty overrides, and an archive without .git. Build artifacts
are ignored; builds run sequentially without changing tracked inputs.
Every subprocess has a deadline, kills its process group on timeout, and
fails on errors/missing tools; there is no skip path.

Optional argument: source tree containing the Makefile and demo script to
exercise, e.g. an archive of origin/master to demonstrate RED.
"""

import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
if len(sys.argv) > 1:
    SOURCE = Path(sys.argv.pop(1)).resolve()

# These record the arguments AFTER the real shell/make expansions, not source
# text or a parallel implementation of version derivation. Output files let
# the real recipes finish normally. The final -D value is checked below.
TOOLS = r'''
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "uname":
    print(os.environ["PGWT_TEST_OS"] if args == ["-s"] else "x86_64")
elif name == "brew":
    assert args == ["--prefix"], args
    print(os.environ["PGWT_TEST_BREW"])
else:
    assert name in ("cc", "go"), name
    with open(os.environ["PGWT_TEST_LOG"], "a") as out:
        out.write(json.dumps([name, args]) + "\n")
    assert "-o" in args, args
    pathlib.Path(args[args.index("-o") + 1]).touch()
'''


def run(args, cwd, env):
    with subprocess.Popen(args, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          start_new_session=True) as proc:
        try:
            stdout, stderr = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            raise AssertionError(f"timed out: {args!r}") from None
        if proc.returncode:
            raise AssertionError(
                f"exit {proc.returncode}: {args!r}\n{stdout}{stderr}")
        return stdout.strip()


class BuildVersionTest(unittest.TestCase):
    def fixture(self, parent, platform):
        repo = parent / "repo"
        repo.mkdir()
        for name in ("Makefile", "demos/build-server-local.sh",
                     "scripts/detect_libbpf_usdt.sh"):
            dest = repo / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / name, dest)
        for name in ("src/server.c", "src/pg_wait_tracer.h", "web/.keep"):
            dest = repo / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.touch()
        (repo / ".gitignore").write_text("build/\npgwt-server\nweb/pgwt\n")
        tools = parent / "tools"
        tools.mkdir()
        for name in ("cc", "go", "uname", "brew"):
            tool = tools / name
            tool.write_text(f"#!{sys.executable}\n" + TOOLS)
            tool.chmod(0o755)
        # Resolve dependencies before replacing PATH; fail if one is missing.
        for name in ("git", "make"):
            executable = shutil.which(name)
            self.assertIsNotNone(executable, f"required tool missing: {name}")
            (tools / name).symlink_to(executable)
        brew = parent / "brew"
        (brew / "include").mkdir(parents=True)
        home = parent / "home"
        home.mkdir()
        env = {
            "PATH": str(tools) + os.pathsep + os.defpath,
            "HOME": str(home), "LC_ALL": "C", "CC": "cc",
            "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CEILING_DIRECTORIES": str(parent),
            "GIT_AUTHOR_NAME": "Build version test",
            "GIT_AUTHOR_EMAIL": "test@example.invalid",
            "GIT_COMMITTER_NAME": "Build version test",
            "GIT_COMMITTER_EMAIL": "test@example.invalid",
            "GIT_AUTHOR_DATE": "2020-01-01T00:00:00Z",
            "GIT_COMMITTER_DATE": "2020-01-01T00:00:00Z",
            "PGWT_TEST_OS": platform,
            "PGWT_TEST_BREW": str(brew),
            "PGWT_TEST_LOG": str(parent / "calls.jsonl"),
            # Limit compilation to one representative real server recipe and
            # bypass the unrelated libbpf probe, without overriding versions.
            "MAKEFLAGS": "--no-print-directory SERVER_SRCS=src/server.c "
                         "SYSTEM_LIBBPF_HAS_USDT=yes",
        }
        run(["git", "init", "-q"], repo, env)
        run(["git", "add", "."], repo, env)
        run(["git", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "tagged"], repo, env)
        run(["git", "tag", "v572.0"], repo, env)
        (repo / "src/server.c").write_text("/* next commit */\n")
        run(["git", "add", "."], repo, env)
        run(["git", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "head"], repo, env)
        return repo, env

    def stamps(self, repo, env):
        log = Path(env["PGWT_TEST_LOG"])
        log.unlink(missing_ok=True)
        run(["make", "pgwt-client"], repo, env)
        run(["bash", "demos/build-server-local.sh"], repo, env)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        go = [args for name, args in calls if name == "go"]
        cc = [args for name, args in calls
              if name == "cc" and "-DPGWT_SERVER" in args and "-c" in args]
        self.assertEqual(len(go), 1, calls)
        self.assertEqual(len(cc), 1, calls)
        self.assertEqual(go[0].count("-ldflags"), 1, go)
        flags = shlex.split(go[0][go[0].index("-ldflags") + 1])
        self.assertEqual(len(flags), 2, flags)
        self.assertEqual(flags[0], "-X", flags)
        self.assertTrue(flags[1].startswith("main.buildVersion="), flags)
        client = flags[1].split("=", 1)[1]
        defines = [a for a in cc[0] if a.startswith("-DPGWT_BUILD_VERSION=")]
        self.assertEqual(len(defines), 1, cc)
        self.assertNotIn("-UPGWT_BUILD_VERSION", cc[0])
        server = json.loads(defines[0].split("=", 1)[1])
        return client, server

    def test_actual_build_stamps(self):
        for platform in ("Darwin", "Linux"):
            for case in ("clean", "dirty", "untagged", "override", "empty", "archive"):
                with self.subTest(platform=platform, case=case):
                    with tempfile.TemporaryDirectory(prefix="pgwt-version-") as tmp:
                        repo, env = self.fixture(Path(tmp), platform)
                        if case == "dirty":
                            with (repo / "src/server.c").open("a") as out:
                                out.write("/* dirty tracked input */\n")
                        elif case == "untagged":
                            run(["git", "tag", "-d", "v572.0"], repo, env)
                        elif case == "archive":
                            shutil.rmtree(repo / ".git")
                        expected = "unknown" if case == "archive" else run(
                            ["git", "describe", "--tags", "--always", "--dirty"], repo, env)
                        if case in ("clean", "dirty"):
                            self.assertTrue(expected.startswith("v572.0-1-g"), expected)
                            self.assertEqual(expected.endswith("-dirty"), case == "dirty")
                            self.assertNotEqual(expected, run(["git", "rev-parse", "HEAD"], repo, env))
                        if case in ("override", "empty"):
                            expected = "release-572-pinned" if case == "override" else ""
                            env["PGWT_BUILD_VERSION"] = expected
                        client, server = self.stamps(repo, env)
                        print(f"{platform}/{case}: client={client!r} server={server!r}", flush=True)
                        self.assertEqual(client, server, "build paths stamped different versions")
                        self.assertEqual(client, expected, "agreement must not be vacuous")
                        # Neither build may dirty the tracked version inputs.
                        after = "unknown" if case == "archive" else run(
                            ["git", "describe", "--tags", "--always", "--dirty"], repo, env)
                        if case not in ("override", "empty"):
                            self.assertEqual(after, expected)


if __name__ == "__main__":
    unittest.main()
