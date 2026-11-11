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
        baseline = self.fixture / "history.json"
        versions = json.loads(baseline.read_text())
        versions.append(versions[0] + versions[0].splitlines()[1].replace("L001", "L002", 1) + "\n")
        baseline.write_text(json.dumps(versions))
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

    def module(self):
        spec = importlib.util.spec_from_file_location("tag_checklist", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_three_commit_history_keeps_deleted_ids(self):
        """Build real Git objects without modifying the checkout's index/.git."""
        repo = Path(self.temp.name) / "repo"
        repo.mkdir()
        repo = repo.resolve()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        one = self.tsv.read_text()
        two = one + one.splitlines()[1].replace("L001", "L002", 1) + "\n"
        stream = bytearray()
        for mark, data in enumerate((two, one, one), 1):
            stream.extend((f"commit refs/heads/main\nmark :{mark}\n"
                           f"committer Fixture <fixture@example.invalid> {1700000000 + mark} +0000\n"
                           f"data 8\ncommit {mark}\n").encode())
            if mark > 1:
                stream.extend(f"from :{mark - 1}\n".encode())
            path = "later.txt" if mark == 3 else "tests/tag_checklist.tsv"
            payload = b"later\n" if mark == 3 else data.encode()
            stream.extend(f"M 100644 inline {path}\ndata {len(payload)}\n".encode())
            stream.extend(payload + b"\n\n")
        subprocess.run(["git", "fast-import", "--quiet"], cwd=repo, input=stream, check=True)
        shas = subprocess.check_output(["git", "rev-list", "--reverse", "main"], cwd=repo, text=True).splitlines()
        module = self.module()
        original_command = module.command
        with mock.patch.object(module, "ROOT", repo), mock.patch.object(
                module, "command", side_effect=lambda argv: original_command(argv, cwd=repo)):
            module.append_only(module.parse_tsv(two), module.history_from_git(repo / "tests/tag_checklist.tsv", shas[0]))
            print("three-commit fixture added: OK (L001, L002)")
            for label, sha in zip(("deleting", "later"), shas[1:]):
                with self.assertRaisesRegex(module.Refused, "removed TSV row.*L002") as refused:
                    module.check_history(module.parse_tsv(one), repo / "tests/tag_checklist.tsv", sha)
                print(f"three-commit fixture {label}: REFUSED: {refused.exception}")
                (repo / "scripts").mkdir(exist_ok=True)
                shutil.copyfile(SCRIPT, repo / "scripts/tag-checklist.py")
                (repo / "tests").mkdir(exist_ok=True)
                (repo / "tests/tag_checklist.tsv").write_text(one)
                result = subprocess.run(["python3", str(repo / "scripts/tag-checklist.py"), sha,
                                         "--check-history"], text=True, capture_output=True)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("removed TSV row(s): L002", result.stderr)
            # The real --check-history entry point is used by make check.
            self.assertIn('--check-history', (ROOT / "scripts/check.sh").read_text())
            with self.assertRaisesRegex(module.Refused, "removed TSV row.*L002"):
                module.check_history(module.parse_tsv(one), repo / "tests/tag_checklist.tsv", shas[2])

    def test_unreadable_and_shallow_history_refused(self):
        module = self.module()
        with mock.patch.object(module, "command", return_value="true\n"):
            with self.assertRaisesRegex(module.Refused, "complete first-parent history"):
                module.history_from_git(ROOT / "tests/tag_checklist.tsv", SHA)
        with mock.patch.object(module, "command", side_effect=module.Refused("history read failed")):
            with self.assertRaisesRegex(module.Refused, "history read failed"):
                module.history_from_git(ROOT / "tests/tag_checklist.tsv", SHA)
        workflow = (ROOT / ".github/workflows/tag-checklist.yml").read_text()
        self.assertIn("fetch-depth: 0", workflow)
        self.assertNotIn("--base-ref", workflow)

    def test_old_retirement_cannot_be_reused_after_an_active_version(self):
        active = self.tsv.read_text()
        self.edit_row(state="retired", reason="Replaced by a new obligation")
        retired = self.tsv.read_text()
        self.tsv.write_text(active)
        (self.fixture / "history.json").write_text(json.dumps([active, retired, active]))
        self.assertIn("retired id reused: L001", self.run_script(success=False))

    def test_in_progress_run_cannot_certify_early_success(self):
        self.json_edit("run-list.json", lambda runs: runs["ci.yml"][0].update(status="in_progress"))
        self.json_edit("run-123.json", lambda view: view.update(status="in_progress", conclusion=None))
        self.assertEqual(self.run_script()["lines"][0]["state"], "NOT-RUN")

    def test_step_name_is_exact_not_substring(self):
        self.json_edit("run-123.json", lambda view: view["jobs"][0]["steps"][0].update(name="Run synthetic data tests (skipped)"))
        self.assertEqual(self.run_script()["lines"][0]["state"], "NOT-RUN")

    def add_rerun(self):
        self.json_edit("run-list.json", lambda runs: runs["ci.yml"].append({**runs["ci.yml"][0], "databaseId": 124}))
        view = json.loads((self.fixture / "run-123.json").read_text())
        view.update(databaseId=124, conclusion="failure", attempt=2)
        view["jobs"][0]["steps"][0]["conclusion"] = "failure"
        (self.fixture / "run-124.json").write_text(json.dumps(view))

    def test_highest_run_id_and_run_id_override(self):
        self.add_rerun()
        result = self.run_script()
        self.assertEqual(result["sources"][0]["run"], 124)
        self.assertEqual(result["lines"][0]["state"], "RED")
        pinned = self.run_script("--run-id", "123")
        self.assertEqual(pinned["sources"][0]["run"], 123)
        self.assertEqual(pinned["lines"][0]["state"], "GREEN")
        self.assertIn("requested run 999 absent", self.run_script("--run-id", "999", success=False))

    def test_latest_attempt_of_same_run_is_read(self):
        self.json_edit("run-123.json", lambda view: view.update(attempt=2))
        result = self.run_script()
        self.assertEqual(result["sources"][0]["attempt"], 2)
        self.assertEqual(result["counts"]["GREEN"], 1)

    def test_later_passing_run_supersedes_failed_run(self):
        self.add_rerun()
        self.json_edit("run-123.json", lambda view: view["jobs"][0]["steps"][0].update(conclusion="failure"))
        self.json_edit("run-124.json", lambda view: view["jobs"][0]["steps"][0].update(conclusion="success"))
        result = self.run_script()
        self.assertEqual(result["sources"][0]["run"], 124)
        self.assertEqual(result["counts"]["GREEN"], 1)

    def test_no_ci_run_refused(self):
        self.json_edit("run-list.json", lambda runs: runs.update({"ci.yml": []}))
        self.assertIn("no CI run", self.run_script(success=False))
        self.edit_row(tests="", evidence="")
        self.assertIn("no CI run", self.run_script(success=False))

    def test_missing_run_conclusion_refused(self):
        self.json_edit("run-123.json", lambda view: view.pop("conclusion"))
        self.assertIn("missing/invalid conclusion", self.run_script(success=False))

    def test_weak_tolerances_never_certify_numbers(self):
        for tolerance in ("substring", "presence", "ordering", "exit-code"):
            for proof in ("PR #1", "NONE"):
                with self.subTest(tolerance=tolerance, proof=proof):
                    self.edit_row(tolerance=tolerance, red_proof=proof)
                    result = self.run_script()
                    self.assertEqual(result["lines"][0]["state"], "WEAK")
                    self.assertEqual(result["counts"]["GREEN"], 0)
        self.blocker(["line:L001"])
        self.assertEqual(self.run_script()["lines"][0]["state"], "RED")

    def test_invalid_tolerance_refused(self):
        self.edit_row(tolerance="exact (substring)")
        self.assertIn("tolerance must", self.run_script(success=False))

    def test_pr_proof_must_exist_and_be_merged(self):
        for metadata in (None, {"number": 1, "state": "OPEN", "mergedAt": None},
                         {"number": 1, "state": "CLOSED", "mergedAt": None},
                         {"number": True, "state": "MERGED", "mergedAt": "2026-11-01"},
                         {"number": 1, "state": "MERGED", "mergedAt": True},
                         {"number": 2, "state": "MERGED", "mergedAt": "2026-11-01"}):
            with self.subTest(metadata=metadata):
                (self.fixture / "proofs.json").write_text(json.dumps({"PR #1": metadata}))
                self.assertIn("PR must exist and be merged", self.run_script(success=False))
        module = self.module()
        rows = module.parse_tsv(self.tsv.read_text())
        with mock.patch.object(module, "gh_json", return_value={
                "number": 1, "state": "MERGED", "mergedAt": "2026-11-01"}) as gh:
            module.verify_red_proofs(rows, SHA)
            gh.assert_called_once_with(["pr", "view", "1", "--json", "number,state,mergedAt"])
        with mock.patch.object(module, "gh_json", side_effect=module.Refused("gh unavailable")):
            with self.assertRaisesRegex(module.Refused, "gh unavailable"):
                module.verify_red_proofs(rows, SHA)

    def test_live_run_view_uses_latest_attempt(self):
        module = self.module()
        rows = module.parse_tsv(self.tsv.read_text())
        view = json.loads((self.fixture / "run-123.json").read_text())
        view["attempt"] = 2
        run_list = json.loads((self.fixture / "run-list.json").read_text())["ci.yml"]
        calls = []
        def gh(argv):
            calls.append(argv)
            if argv[:2] == ["run", "list"]:
                return run_list
            if argv[:3] == ["run", "view", "123"]:
                self.assertNotIn("--attempt", argv, "gh defaults to the latest attempt; never pin an old attempt")
                return view
            if argv[:2] == ["issue", "list"]:
                return []
            self.fail("unexpected gh command: " + repr(argv))
        with mock.patch.object(module, "gh_json", side_effect=gh):
            _, sources, _ = module.load_evidence(rows, SHA)
        self.assertEqual(sources[0]["attempt"], 2)
        self.assertEqual(len(calls), 3)

    def test_sha_proof_must_be_ancestor_of_evaluated_commit(self):
        ancestor = "b" * 40
        self.edit_row(red_proof=ancestor)
        for is_ancestor, descendant in ((False, SHA), (True, "c" * 40), (True, SHA)):
            (self.fixture / "proofs.json").write_text(json.dumps({ancestor: {
                "ancestor": ancestor, "descendant": descendant, "isAncestor": is_ancestor}}))
            if is_ancestor and descendant == SHA:
                self.assertEqual(self.run_script()["counts"]["GREEN"], 1)
            else:
                self.assertIn("not a verified ancestor", self.run_script(success=False))
        module = self.module()
        rows = module.parse_tsv(self.tsv.read_text())
        with mock.patch.object(module, "command", return_value="") as git:
            module.verify_red_proofs(rows, SHA)
            git.assert_called_once_with(["git", "merge-base", "--is-ancestor", ancestor, SHA])
        with mock.patch.object(module, "command", side_effect=module.Refused("not ancestor")):
            with self.assertRaisesRegex(module.Refused, "not ancestor"):
                module.verify_red_proofs(rows, SHA)

    def test_markdown_identifies_run_conclusion_sha(self):
        path = self.fixture / "report.md"
        self.run_script("--markdown-output", str(path))
        output = path.read_text()
        self.assertIn("run `123`", output)
        self.assertIn("conclusion `success`", output)
        self.assertIn("SHA `" + SHA + "`", output)

    def test_ci_name_guard(self):
        check = (ROOT / "scripts/check.sh").read_text()
        guard = next(line.removeprefix("run ") for line in check.splitlines()
                     if line.startswith("run python3 -c ") and "tag checklist trigger requires" in line)
        for name, expected in (("CI", 0), ("Renamed CI", 1)):
            repo = Path(self.temp.name) / name
            (repo / ".github/workflows").mkdir(parents=True)
            (repo / ".github/workflows/ci.yml").write_text("name: " + name + "\n")
            result = subprocess.run(["bash", "-c", guard], cwd=repo, capture_output=True)
            self.assertEqual(result.returncode, expected, result.stderr)

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
