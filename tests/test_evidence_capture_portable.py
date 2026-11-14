#!/usr/bin/env python3
"""Portable capture unit plus construction gate and a qualification mutant."""
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def build(binary, capture_source=None):
    command = [*shlex.split(os.environ.get("CC", "cc")),
               *shlex.split(os.environ.get("CFLAGS", "-g -O2")),
               "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
               str(ROOT / "tests/test_evidence_capture.c"),
               str(capture_source or ROOT / "src/evidence_capture.c"),
               str(ROOT / "src/core/evidence_codec.c"),
               str(ROOT / "src/core/evidence_replay.c"),
               str(ROOT / "src/core/wait_metrics.c"), "-o", str(binary)]
    subprocess.run(command, check=True)


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-evidence-portable-") as directory:
        tmp = Path(directory)
        env = {**os.environ, "TMPDIR": str(tmp)}
        binary = tmp / "capture"
        build(binary)
        subprocess.run([str(binary)], check=True, env=env, timeout=30)
        gate = ROOT / "tests/test_evidence_capture_kinds.sh"
        subprocess.run(["bash", str(gate)], check=True)
        (tmp / "src").mkdir()
        for name in ("daemon.c", "evidence_capture.c"):
            (tmp / "src" / name).write_text((ROOT / "src" / name).read_text())
        seeded = tmp / "src/evidence_capture.c"
        for violation in ("PGWT_EVIDENCE_FRONTIER", ".kind = 2"):
            seeded.write_text((ROOT / "src/evidence_capture.c").read_text() + "\n" + violation + "\n")
            result = subprocess.run(["bash", str(gate), str(tmp)], text=True, capture_output=True)
            assert result.returncode == 1 and "forbidden certificate kind" in result.stdout, result
            print(result.stdout.strip())
            print(f"seeded certificate-kind violation {violation}: killed (exit 1)", flush=True)
        # A failed startup runs cleanup. The only init call must follow every
        # fatal jump in daemon_init, so cleanup cannot publish an empty file.
        daemon = (ROOT / "src/daemon.c").read_text()
        init = daemon.split("int pgwt_daemon_init(", 1)[1].split("\nfail:", 1)[0]
        call = "d->evidence_capture = pgwt_evidence_capture_init("
        assert init.count(call) == 1 and init.index(call) > init.rindex("goto fail;")
        assert not re.search(r"\b(?:goto|return)\b", init[init.index(call):].split(";", 1)[1]
                             .replace("return 0;", ""))
        assert len(re.findall(r"pgwt_evidence_capture_(?:init|observe|close)\(", daemon)) == 3
        print("daemon startup failure: evidence setup follows final fatal step; three call sites PASS", flush=True)
        source = (ROOT / "src/evidence_capture.c").read_text()
        needle = ".qualified = false"
        assert source.count(needle) == 1
        mutant = tmp / "qualified.c"
        mutant.write_text(source.replace(needle, ".qualified = true"))
        build(binary, mutant)
        result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=30)
        assert result.returncode == 1 and "FAIL: qualified=false" in result.stderr, result
        print(result.stderr.strip())
        print("qualified=true mutant: built, reached qualified=false assertion, killed (exit 1)")


if __name__ == "__main__":
    main()
