#!/usr/bin/env python3
"""Exercise the command used by board-labels with crafted commit bodies."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "board-author.py"


def derive(*bodies):
    commits = [{"messageHeadline": "Task commit", "messageBody": body} for body in bodies]
    return subprocess.check_output(
        [sys.executable, str(SCRIPT)], input=json.dumps({"commits": commits}), text=True
    ).strip()


class BoardAuthorTests(unittest.TestCase):
    def test_prose_is_not_codex(self):
        body = "Rule 0 was a mandatory codex audit of every status report.\nIt blocked one report."
        self.assertNotEqual(derive(body), "by:codex")
        self.assertEqual(derive(body), "by:unknown")

    def test_no_mention_or_trailer_is_unknown(self):
        self.assertEqual(derive("Explain the previous working agreement."), "by:unknown")

    def test_all_model_names_in_prose_are_unknown(self):
        for name in ("codex", "gpt-6", "Claude Opus", "Claude Sonnet"):
            with self.subTest(name=name):
                self.assertEqual(derive(f"Document the old {name} review rule."), "by:unknown")

    def test_identifying_trailers(self):
        for trailer, expected in (
            ("Generated-with: Codex", "by:codex"),
            ("Co-Authored-By: GPT-6 <bot@example.invalid>", "by:codex"),
            ("Co-Authored-By: Claude Opus 5 <bot@example.invalid>", "by:claude-opus"),
            ("Claude-session: Claude Sonnet 4", "by:claude-sonnet"),
            ("Signed-off-by: Codex <bot@example.invalid>", "by:codex"),
            ("Generated-with: human", "by:human"),
        ):
            with self.subTest(trailer=trailer):
                self.assertEqual(derive("Explanation.\n\n" + trailer), expected)

    def test_agent_wins_over_human_and_claude(self):
        self.assertEqual(derive("Generated-with: human", "Co-authored-by: Claude Opus 5",
                                "Generated-with: codex"), "by:codex")

    def test_claude_trailer_with_codex_prose(self):
        self.assertEqual(derive("Remove the codex review rule.\n\nCo-authored-by: Claude Opus 5"),
                         "by:claude-opus")

    def test_other_trailer_values_do_not_attribute(self):
        self.assertEqual(derive("Explanation.\n\nReviewed-by: codex\nTopic: Claude Opus"),
                         "by:unknown")

    def test_trailer_shaped_prose_outside_final_block(self):
        self.assertEqual(derive("Example:\n\nGenerated-with: codex\n\nThis is only an example."),
                         "by:unknown")

    def test_commit_boundaries_are_preserved(self):
        self.assertEqual(derive("Co-authored-by: Claude Sonnet 4", "codex is discussed here."),
                         "by:claude-sonnet")

    def test_folded_case_insensitive_trailer(self):
        self.assertEqual(derive("Explanation.\n\nco-AUTHORED-by: Claude\n Sonnet 4"),
                         "by:claude-sonnet")

    def test_ordinary_signoff_is_unknown(self):
        self.assertEqual(derive("Signed-off-by: A Person <person@example.invalid>"), "by:unknown")

    def test_empty_commits_are_unknown(self):
        self.assertEqual(derive(), "by:unknown")

    def test_workflow_recomputes_and_removes_both_families(self):
        root = SCRIPT.parent.parent
        workflow = (root / ".github/workflows/board-labels.yml").read_text()
        # Execute the actual workflow shell with a local GitHub fixture.
        run = textwrap.dedent(workflow.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory(prefix="board-author-") as tmp:
            fixture = Path(tmp)
            gh = fixture / "gh"
            gh.write_text(f"#!{sys.executable}\n" + textwrap.dedent('''\
                import json, os, sys
                from pathlib import Path
                args = sys.argv[1:]
                fixture = Path(os.environ["BOARD_FIXTURE"])
                if args[:2] == ["pr", "view"]:
                    if args[-1] == "commits":
                        print((fixture / "commits.json").read_text())
                    else:
                        print(json.dumps({"state": "OPEN", "isDraft": True,
                            "mergeStateStatus": "CLEAN", "statusCheckRollup": [],
                            "labels": [{"name": label} for label in [
                                "phase:ready", "by:human", "by:codex", "by:unknown"]]}))
                elif args[:2] == ["pr", "edit"]:
                    with (fixture / "edits").open("a") as out:
                        out.write(" ".join(args[-2:]) + "\\n")
                elif args[:2] != ["label", "create"]:
                    sys.exit("unexpected gh command: " + str(args))
                '''))
            gh.chmod(0o755)
            env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                       BOARD_FIXTURE=tmp, REPO="fixture/repo", EVENT_PR="",
                       DISPATCH_PR="768", SUITE_PRS="null")
            for body, expected in (
                ("Explain the codex review rule.", "by:unknown"),
                ("Explanation.\n\nCo-authored-by: Claude Opus 5", "by:claude-opus"),
            ):
                with self.subTest(body=body):
                    (fixture / "commits.json").write_text(json.dumps({"commits": [
                        {"messageHeadline": "Task commit", "messageBody": body}]}))
                    edits = fixture / "edits"
                    edits.write_text("")
                    output = subprocess.check_output(["bash", "-c", run], cwd=root,
                                                     env=env, text=True)
                    self.assertIn(f"phase=phase:coding author={expected}", output)
                    changes = set(edits.read_text().splitlines())
                    self.assertIn("--add-label phase:coding", changes)
                    self.assertIn("--remove-label phase:ready", changes)
                    for label in ("by:human", "by:codex", "by:unknown"):
                        if label != expected:
                            self.assertIn("--remove-label " + label, changes)
                    if expected == "by:claude-opus":
                        self.assertIn("--add-label " + expected, changes)


if __name__ == "__main__":
    unittest.main()
