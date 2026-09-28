#!/usr/bin/env python3
"""Import every listed Python test with Playwright unavailable (issue #221).

Shell entries run as scripts and extensionless entries are C binaries; neither
has a Python module import to check. Each .py entry gets a fresh interpreter
so an earlier test cannot leave its imports cached for a later one.
"""

import os
import subprocess
import sys


TEST_DIR = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(TEST_DIR, "unit_tests.list")

# Run as `python -c`, with tests/ as the working directory just like
# `make -C tests check`. This finder makes direct and transitive imports fail,
# even on a developer machine where Playwright is installed.
IMPORT_CHECK = """
import importlib
import importlib.abc
import sys

class NoPlaywright(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == 'playwright' or fullname.startswith('playwright.'):
            raise ModuleNotFoundError("No module named 'playwright'", name='playwright')

sys.meta_path.insert(0, NoPlaywright())
importlib.import_module(sys.argv[1])
"""


def main():
    failures = []
    checked = 0
    skipped = 0

    with open(LIST, encoding="utf-8") as entries:
        for raw in entries:
            entry = raw.strip()
            if not entry or entry.startswith("#"):
                continue
            if entry.endswith(".sh") or "." not in entry:
                skipped += 1  # Shell scripts and C binaries have no Python import.
                continue
            if not entry.endswith(".py"):
                failures.append((entry, "unknown entry type"))
                continue

            checked += 1
            result = subprocess.run(
                [sys.executable, "-c", IMPORT_CHECK, entry[:-3]],
                cwd=TEST_DIR, capture_output=True, text=True,
            )
            if result.returncode:
                failures.append((entry, result.stderr or result.stdout))
                print("  FAIL: %s" % entry)
            else:
                print("  PASS: %s imports without Playwright" % entry)

    print("test_unit_import_guard: %d Python imports checked; "
          "%d shell/C entries skipped" % (checked, skipped))
    for entry, detail in failures:
        print("  FAIL: %s: %s" % (entry, detail.strip()))
    if failures:
        print("Keep the test in unit_tests.list. Move the Playwright import "
              "inside the function that drives the browser, so pure logic "
              "remains importable without a browser.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
