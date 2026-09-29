#!/usr/bin/env python3
"""Focused checks for gallery provenance and tiered characterisation labels."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from PIL import Image


ROOT = Path(__file__).resolve().parent.parent


class GalleryEvidenceTests(unittest.TestCase):
    def test_report_records_distance_and_warns(self):
        for ahead, behind in ((0, 4), (1, 0), (0, 0)):
            with self.subTest(ahead=ahead, behind=behind), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                before, after = root / "before", root / "after"
                before.mkdir(); after.mkdir()
                Image.new("RGB", (2, 2), "black").save(before / "cell.png")
                Image.new("RGB", (2, 2), "black").save(after / "cell.png")
                result = subprocess.run([
                    "python3", str(ROOT / "tests/ui_gallery_report.py"),
                    str(before), str(after), str(root / "index.html"),
                    "--base", "old", "--head", "head", "--origin-master", "new",
                    "--base-ahead", str(ahead), "--base-behind", str(behind),
                ], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                summary = json.loads((root / "summary.json").read_text())
                self.assertEqual(summary["origin_master"], "new")
                self.assertEqual(summary["base_ahead_of_origin_master"], ahead)
                self.assertEqual(summary["base_behind_origin_master"], behind)
                self.assertNotIn("base_is_ancestor_of_head", summary)
                html = (root / "index.html").read_text()
                if ahead or behind:
                    self.assertIn("WARNING", html)
                    self.assertIn(f"{behind} commits behind", html)
                else:
                    self.assertNotIn("WARNING", html)

    def test_base_selection_and_ancestry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            git = root / "git"
            git.write_text("""#!/bin/sh
printf '%s\\n' "$*" >> "$GIT_CALLS"
while [ "$1" = -c ]; do shift 2; done
case "$1 $2" in
  'fetch --quiet')
    [ "$GIT_SSH_COMMAND" = 'ssh -o ConnectTimeout=15' ] || exit 2
    if [ "${FETCH_HANG:-0}" = 1 ]; then exec sleep 30; fi
    [ "${FETCH_FAIL:-0}" != 1 ] ;;
  'rev-parse --verify')
    case "$3" in
      origin/master*) echo originhash ;;
      custom*) echo customhash ;;
    esac ;;
  'merge-base --is-ancestor')
    [ "$BASE_IS_ANCESTOR" = 1 ] ;;
  'rev-list --left-right') echo '0 4' ;;
esac
""")
            git.chmod(0o755)
            py = root / "python3"
            py.write_text("""#!/bin/sh
if [ "$2" = 'import sys; print(sys.executable)' ]; then echo "$0"; exit 0; fi
exit 1
""")
            py.chmod(0o755)
            calls = root / "calls"
            # Give the script only its required stock commands. This excludes
            # timeout even on Linux hosts that ship GNU coreutils by default.
            stock_path = "/usr/bin:/bin:/usr/sbin:/sbin"
            for command in ("dirname", "sleep"):
                binary = shutil.which(command, path=stock_path)
                self.assertIsNotNone(binary)
                (root / command).symlink_to(binary)
            env = dict(os.environ, PATH=str(root),
                       GIT_CALLS=str(calls))
            self.assertIsNone(shutil.which("timeout", path=env["PATH"]))
            for ref, ancestor in ((None, "1"), ("custom", "1"), ("custom", "0")):
                with self.subTest(ref=ref, ancestor=ancestor):
                    calls.write_text("")
                    env["BASE_IS_ANCESTOR"] = ancestor
                    command = ["/bin/bash", str(ROOT / "tests/ui_gallery.sh")]
                    if ref:
                        command.append(ref)
                    result = subprocess.run(command, env=env, capture_output=True, text=True)
                    trace = calls.read_text()
                    self.assertIn("-c http.lowSpeedLimit=1 -c http.lowSpeedTime=15 fetch --quiet origin", trace)
                    self.assertIn(f"rev-parse --verify {ref or 'origin/master'}^{{commit}}", trace)
                    if ancestor == "0":
                        self.assertIn("not an ancestor", result.stdout + result.stderr)
                    else:
                        self.assertIn("rev-list --left-right --count", trace)
            calls.write_text("")
            env["FETCH_FAIL"] = "1"
            result = subprocess.run(["/bin/bash", str(ROOT / "tests/ui_gallery.sh")],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAIL: could not refresh origin/master", result.stdout)
            self.assertNotIn("rev-parse --verify", calls.read_text())

            calls.write_text("")
            env.pop("FETCH_FAIL")
            env["FETCH_HANG"] = "1"
            result = subprocess.run(["/bin/bash", str(ROOT / "tests/ui_gallery.sh")],
                                    env=env, capture_output=True, text=True, timeout=20)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAIL: timed out refreshing origin/master after 15s", result.stdout)
            self.assertNotIn("rev-parse --verify", calls.read_text())


class TieredOutputTests(unittest.TestCase):
    def test_each_rate_line_is_self_explanatory(self):
        script = f'''source "{ROOT / 'tests/cross_validate_rate_output.sh'}"
print_rate_result 10 'RESULT: PASS'
print_rate_result 50 'RESULT: FAIL'
print_rate_summary 10 PASS
print_rate_summary 50 FAIL
'''
        result = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 4)
        self.assertTrue(all("characterisation" in line for line in lines))
        self.assertTrue(all("shipped default" in line for line in (lines[0], lines[2])))
        self.assertTrue(all("exploratory" in line for line in (lines[1], lines[3])))
        self.assertNotIn("RESULT: FAIL", result.stdout)


if __name__ == "__main__":
    unittest.main()
