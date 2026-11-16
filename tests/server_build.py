"""Build inputs shared by tests that compile the product server.

Read only SERVER_SRCS, without evaluating the Linux/BPF Makefile on macOS.
Refuse unsupported syntax instead of silently returning a partial link list.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def server_sources(root=ROOT):
    text = (root / "Makefile").read_text().replace("\\\n", " ")
    if len(re.findall(r"^SERVER_SRCS\s*[:+?]?=", text, re.MULTILINE)) != 1:
        raise ValueError("expected one unextended SERVER_SRCS assignment")
    assignments = re.findall(r"^SERVER_SRCS\s*=\s*(.*)$", text, re.MULTILINE)
    if len(assignments) != 1:
        raise ValueError("expected one SERVER_SRCS assignment")
    tokens = assignments[0].split()
    if not tokens or any(not re.fullmatch(r"\$\(SRC_DIR\)/[\w/]+\.c", t) for t in tokens):
        raise ValueError("unsupported SERVER_SRCS syntax")
    sources = [root / t.replace("$(SRC_DIR)", "src") for t in tokens]
    if sources.count(root / "src/server.c") != 1 or len(set(sources)) != len(sources):
        raise ValueError("SERVER_SRCS must contain server.c once and no duplicates")
    return sources


def server_objects(build, include_main=True):
    return [Path(build) / ("server_" + str(s.relative_to(ROOT / "src").with_suffix(".o")))
            for s in server_sources() if include_main or s != ROOT / "src/server.c"]


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--objects":
        sys.exit("usage: server_build.py --objects BUILD_DIR")
    print(" ".join(map(str, server_objects(sys.argv[2], include_main=False))))
