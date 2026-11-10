#!/usr/bin/env python3
"""Subprocess fixtures for the tag evaluator's fail-closed boundaries."""
import ast
import csv
import importlib.util
import io
import json
import fnmatch
from pathlib import Path
import shutil
import re
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts/tag-checklist.py"
FIXTURE = ROOT / "tests/fixtures/tag_checklist"
SHA = "a" * 40


class TagChecklistTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture = Path(self.temp.name) / "evidence"
        shutil.copytree(FIXTURE, self.fixture)
        self.tsv = self.fixture / "checklist.tsv"

    def json_edit(self, filename, change):
        path = self.fixture / filename
        data = json.loads(path.read_text())
        change(data)
        path.write_text(json.dumps(data))

    def edit_row(self, **values):
        reader = csv.DictReader(io.StringIO(self.tsv.read_text()), delimiter="\t")
        rows = list(reader)
        rows[0].update(values)
        output = io.StringIO()
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        self.tsv.write_text(output.getvalue())

    def run_script(self, *extra, success=True):
        result = subprocess.run(["python3", str(SCRIPT), SHA, "--tsv", str(self.tsv),
                                 "--offline", str(self.fixture), "--json", *extra],
                                text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout, "", "refusal must not publish a partial readiness table")
        return result.stderr

    def blocker(self, labels):
        (self.fixture / "issues.json").write_text(json.dumps([
            {"number": 42, "title": "Wrong DB Time", "labels": [{"name": name} for name in ["tag-blocker", *labels]]}]))

    def test_success_requires_red_proof_and_exact_step(self):
        result = self.run_script()
        self.assertEqual(result["header"], "GREEN 1 / total 1")
        self.assertEqual(result["lines"][0]["state"], "GREEN")

    def test_a_skipped_step_not_green(self):
        self.json_edit("run-123.json", lambda view: view["jobs"][0]["steps"][0].update(conclusion="skipped"))
        result = self.run_script()
        self.assertEqual(result["lines"][0]["state"], "NOT-RUN")
        self.assertEqual(result["counts"]["GREEN"], 0)

    def test_b_successful_job_all_steps_skipped(self):
        def skip(view):
            for step in view["jobs"][0]["steps"]:
                step["conclusion"] = "skipped"
        self.json_edit("run-123.json", skip)
        self.assertEqual(self.run_script()["lines"][0]["state"], "NOT-RUN")

    def test_c_unmapped_blocker_own_red_line(self):
        self.blocker([])
        result = self.run_script()
        self.assertEqual(result["total"], 2)
        self.assertEqual(result["counts"]["RED"], 1)
        self.assertEqual(result["lines"][1]["state"], "RED")
        self.assertIn("unmapped", result["lines"][1]["reason"])

    def test_d_mapped_blocker_red_test_gap(self):
        self.blocker(["line:L001"])
        line = self.run_script()["lines"][0]
        self.assertEqual(line["state"], "RED")
        self.assertIn("test gap", line["reason"])

    def test_e_another_sha_refused(self):
        self.json_edit("run-123.json", lambda view: view.update(headSha="b" * 40))
        self.assertIn("wrong sha", self.run_script(success=False))

    def test_f_void_jobs_refused(self):
        for jobs in (None, [], {}, ""):
            with self.subTest(jobs=jobs):
                self.json_edit("run-123.json", lambda view: view.update(jobs=jobs))
                self.assertIn("void jobs", self.run_script(success=False))

    def test_g_removed_row_refused(self):
        baseline = self.fixture / "base.tsv"
        baseline.write_text(baseline.read_text() + baseline.read_text().splitlines()[1].replace("L001", "L002", 1) + "\n")
        self.assertIn("removed TSV row(s): L002", self.run_script(success=False))

    def test_h_none_is_green_unproven(self):
        self.edit_row(red_proof="NONE")
        result = self.run_script()
        self.assertEqual(result["lines"][0]["state"], "GREEN-UNPROVEN")
        self.assertEqual(result["counts"]["GREEN"], 0)

    def test_unknown_line_label_refused(self):
        self.blocker(["line:L999"])
        self.assertIn("unknown line label", self.run_script(success=False))

    def test_failed_step_red_and_no_test_gap(self):
        self.json_edit("run-123.json", lambda view: view["jobs"][0]["steps"][0].update(conclusion="failure"))
        self.blocker(["line:L001"])
        line = self.run_script()["lines"][0]
        self.assertEqual(line["state"], "RED")
        self.assertNotIn("test gap", line["reason"])

    def test_no_test_and_box_only_are_distinct(self):
        self.edit_row(tests="")
        self.assertEqual(self.run_script()["lines"][0]["state"], "NO-TEST")
        self.edit_row(tests="tests/example.py::test_db_time", evidence="box-check")
        self.assertEqual(self.run_script()["lines"][0]["state"], "NOT-RUN")
        self.edit_row(evidence="")
        self.assertEqual(self.run_script()["lines"][0]["state"], "NO-TEST")

    def test_absent_cancelled_and_unfinished_steps(self):
        for conclusion in ("cancelled", "neutral", "", None):
            self.json_edit("run-123.json", lambda view: view["jobs"][0]["steps"][0].update(conclusion=conclusion))
            self.assertEqual(self.run_script()["lines"][0]["state"], "NOT-RUN")
        self.json_edit("run-123.json", lambda view: view["jobs"][0].update(steps=[]))
        self.assertEqual(self.run_script()["lines"][0]["state"], "NOT-RUN")

    def test_duplicate_id_and_empty_tsv_refused(self):
        original = self.tsv.read_text()
        self.tsv.write_text(original + original.splitlines()[1] + "\n")
        self.assertIn("duplicate id", self.run_script(success=False))
        self.tsv.write_text(original.splitlines()[0] + "\n")
        self.assertIn("empty checklist", self.run_script(success=False))

    def test_retirement_retains_line_and_requires_reason(self):
        self.edit_row(state="retired", reason="Replaced by L002")
        line = self.run_script()["lines"][0]
        self.assertEqual(line["state"], "RETIRED")
        self.assertIn("L002", line["reason"])
        self.edit_row(reason="")
        self.assertIn("retired requires", self.run_script(success=False))

    def test_wrong_sha_in_list_and_missing_issues_refused(self):
        self.json_edit("run-list.json", lambda runs: runs["ci.yml"][0].update(headSha="b" * 40))
        self.assertIn("wrong sha", self.run_script(success=False))
        shutil.copyfile(FIXTURE / "run-list.json", self.fixture / "run-list.json")
        (self.fixture / "issues.json").unlink()
        self.assertIn("invalid/unavailable JSON", self.run_script(success=False))

    def test_gh_failure_refused(self):
        spec = importlib.util.spec_from_file_location("tag_checklist", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with self.assertRaisesRegex(module.Refused, "failed"):
            module.command(["python3", "-c", "import sys; sys.exit(7)"])

    def test_merge_base_includes_rows_added_at_current_head(self):
        spec = importlib.util.spec_from_file_location("tag_checklist", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        baseline = (self.fixture / "base.tsv").read_text()
        baseline += baseline.splitlines()[1].replace("L001", "L002", 1) + "\n"
        def git(argv):
            if argv == ["git", "merge-base", "HEAD", "origin/master"]:
                return SHA
            if argv == ["git", "ls-tree", SHA, "--", "tests/tag_checklist.tsv"]:
                return "100644 blob fixture tests/tag_checklist.tsv"
            if argv == ["git", "show", SHA + ":tests/tag_checklist.tsv"]:
                return baseline
            self.fail("unexpected Git baseline command: " + repr(argv))
        with mock.patch.object(module, "command", side_effect=git):
            data = module.baseline_from_git(ROOT / "tests/tag_checklist.tsv", "origin/master")
        with self.assertRaisesRegex(module.Refused, "removed TSV row.*L002"):
            module.append_only(module.parse_tsv(self.tsv.read_text()), module.parse_tsv(data))
        # On master workflow runs, explicitly compare the parent: HEAD is
        # already the merged commit, so origin/master may include a deletion.
        workflow = (ROOT / ".github/workflows/tag-checklist.yml").read_text()
        self.assertIn('--base-ref "$CHECKLIST_SHA^"', workflow)

    def test_inventory_citations_and_ci_registration(self):
        """A renamed or unregistered citation must not silently retain proof."""
        rows = list(csv.DictReader(io.StringIO((ROOT / "tests/tag_checklist.tsv").read_text()), delimiter="\t"))
        unit_entries = {line.split("#", 1)[0].strip() for line in (ROOT / "tests/unit_tests.list").read_text().splitlines()}
        ci = (ROOT / ".github/workflows/ci.yml").read_text()
        for row in rows:
            citations = next(csv.reader([row["tests"]])) if row["tests"] else []
            for citation in citations:
                with self.subTest(line=row["id"], citation=citation):
                    filename, name = citation.split("::", 1)
                    path = ROOT / filename
                    self.assertTrue(path.is_file(), citation)
                    source = path.read_text()
                    if path.suffix == ".py":
                        names = {node.name for node in ast.walk(ast.parse(source))
                                 if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}
                        names.update(re.findall(r'TestRunner\("([^"]+)"\)', source))
                        self.assertIn(name, names)
                    elif path.suffix == ".mjs":
                        titles = []
                        for match in re.finditer(r'''test\(\s*((?:'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*")(?:\s*\+\s*(?:'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*"))*)''', source):
                            titles.append("".join(ast.literal_eval(part.group()) for part in re.finditer(
                                r''''(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*"''', match[1])))
                        self.assertIn(name, titles)
                    else:
                        self.assertRegex(source, r"\b" + re.escape(name) + r"(?:\s*\(|\b)")
                    evidence = row["evidence"]
                    if evidence == "box-check":
                        # Includes dynamically registered live tests and the
                        # live-smoke helper called by the harness, not its units.
                        runner = (ROOT / "tests/run_all.sh").read_text()
                        self.assertTrue(path.name in runner or path.name in unit_entries or
                                        path.name == "ui_live_smoke_lib.py", citation)
                    elif evidence:
                        workflow, job, step = evidence.split("/", 2)
                        self.assertEqual(workflow, "ci.yml")
                        self.assertIn("- name: " + step, ci)
                        if step == "Run C unit tests":
                            self.assertIn(path.stem if path.suffix == ".c" else path.name, unit_entries)
                        elif step == "Run synthetic data tests":
                            self.assertTrue(fnmatch.fnmatch(filename, "tests/test_data_*.py"))
                            self.assertIn("for t in tests/test_data_*.py", ci)
                        elif "builder unit tests" in step:
                            self.assertTrue(fnmatch.fnmatch(filename, "tests/web_unit/*.test.mjs"))
                            self.assertIn("node --test 'tests/web_unit/*.test.mjs'", ci)
                        elif job == "web-ui":
                            self.assertIn("python3 " + filename, ci)
                        elif "Bridge integration" in step:
                            self.assertEqual(filename, "web/bridge_test.go")
                            self.assertIn("go test -race ./...", ci)
                        else:
                            self.assertIn("python3 " + filename, ci)

    def test_all_server_commands_and_platform_cells_are_inventoried(self):
        rows = list(csv.DictReader(io.StringIO((ROOT / "tests/tag_checklist.tsv").read_text()), delimiter="\t"))
        dispatch = (ROOT / "src/server.c").read_text().split('if (strcmp(req->cmd, "info") == 0)', 1)[1]
        commands = {"info", *re.findall(r'else if \(strcmp\(req->cmd, "([^"]+)"\) == 0\)', dispatch)}
        statements = {row["statement"].split(":", 1)[0] for row in rows if row["area"] == "Raw vs summary"}
        self.assertTrue(commands <= statements, commands - statements)
        cells = {f"{os} x PG{pg}" for os in ("EL8 kernel 4.18", "EL9 native kernel", "Ubuntu native kernel")
                 for pg in (13, 16, 17, 18)}
        self.assertTrue(cells <= {row["platforms"] for row in rows if row["area"] == "Platforms"})


if __name__ == "__main__":
    unittest.main()
