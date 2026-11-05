#!/usr/bin/env python3
"""RULE 0 vocabulary and non-vacuous evidence scopes, without network calls."""
import contextlib
import importlib.util
import io
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "absence_prepass", ROOT / "scripts/lead/absence-prepass.py")
PREPASS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPASS)

CORPUS_A = [
    "exists nowhere", "not merged", "unreachable", "I can't", "cannot",
    "The fix exists nowhere.",
    "The fix is not merged.",
    "The endpoint is unreachable.",
    "I can't access the repository.",
    "I cannot change permissions.",
    "Cannot access the repository with these permissions.",
    "I could not find any caller of foo()",
    "The branch doesn't exist on the remote",
    "No caller references this function",
    "The repo contains no test for this path",
    "There are no open PRs touching src/server.c",
    "Nothing references the old flag",
    "grep returned 0 matches",
    "The hook isn't wired into settings.json",
    "not called from anywhere in src/",
    "The script lacks a guard",
    "Nobody has filed an issue",
    "No guard exists for that case",
    "The PR has not been merged",
    "the labels do not work",
    "there is no Generated-with trailer anywhere",
    # Independent witnesses: removing ANY pattern must lose a corpus hit.
    "No mechanical check validates this field.",
    "Nothing checks this field.",
    "The feature is not implemented.",
    "The script has no validation.",
    "This operation never runs.",
    "The search yielded zero results.",
    "The feature is absent.",
    "The trailer is missing.",
    "The change is not on master.",
    "The branch is not on the remote.",
    "No path leads to this function.",
    "The branch does not exist.",
    "The marker is not present.",
    "No implementation exists anywhere.",
    "The symbol was not found.",
    "No such method is available.",
    "None exist for this case.",
    "The marker isn't there.",
    "The marker is not there.",
    "The code is implemented nowhere.",
]
CORPUS_B = [
    "No problem, thanks.",
    "the node_modules directory",
    "native code compiles",
    "I know the answer",
]
VALID_SCOPES = [
    "native code in src/...", "notes/ directory",
    "node_modules and vendored trees", "allocator tests under tests/",
]
VACUOUS_SCOPES = [
    "none at all today", "nothing else searched", "n/a for this claim",
    "na for this claim", "nil results here", "no places at all",
    "all of the places", "everything was searched", "nowhere in particular",
]


def exit_code(text):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "draft.txt"
        path.write_text(text, encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            return PREPASS.main(["absence-prepass.py", str(path)])


class AbsenceVocabulary(unittest.TestCase):
    def test_unevidenced_claims(self):
        for phrase in CORPUS_A:
            with self.subTest(phrase=phrase):
                self.assertEqual(exit_code(phrase), 1)
                self.assertEqual(exit_code(phrase.swapcase()), 1)

    def test_ordinary_prose(self):
        for phrase in CORPUS_B + ["no", "No", "NO"]:
            with self.subTest(phrase=phrase):
                self.assertEqual(exit_code(phrase), 0)

    def test_every_pattern_has_an_exclusive_witness(self):
        for index, pattern in enumerate(PREPASS.PATTERNS):
            witnesses = [phrase for phrase in CORPUS_A
                         if [i for i, compiled in enumerate(PREPASS.COMPILED)
                             if compiled.search(phrase)] == [index]]
            with self.subTest(pattern=pattern):
                self.assertTrue(witnesses, "add an exclusive Corpus-A witness")
                self.assertTrue(pattern.startswith(r"\b"))
                self.assertTrue(pattern.endswith(r"\b"))
                self.assertIsNone(re.search(pattern, "no", re.IGNORECASE))

    def test_non_vacuous_scopes(self):
        for scope in VALID_SCOPES + VACUOUS_SCOPES:
            for label in ("SEARCHED", "NOT SEARCHED"):
                with self.subTest(scope=scope, label=label):
                    valid = scope in VALID_SCOPES
                    self.assertGreaterEqual(len(scope), 8)
                    self.assertEqual(bool(PREPASS.non_vacuous(scope)), valid)
                    scopes = {"SEARCHED": "src/ and tests/", "NOT SEARCHED": "remote branches"}
                    scopes[label] = scope
                    draft = ("The feature is missing.\n```console\nrg feature src/\n"
                             "(no output)\n```\n" +
                             "\n".join(f"{key}: {value}" for key, value in scopes.items()))
                    self.assertEqual(exit_code(draft), 0 if valid else 1)


if __name__ == "__main__":
    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(AbsenceVocabulary))
    fired = sum(exit_code(phrase) == 1 for phrase in CORPUS_A)
    print(f"RULE0-PHRASINGS-FIRING: {fired}/{len(CORPUS_A)}")
    sys.exit(0 if result.wasSuccessful() else 1)
