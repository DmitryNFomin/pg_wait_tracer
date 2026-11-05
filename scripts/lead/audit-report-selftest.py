#!/usr/bin/env python3
"""Black-box the real gate with a deterministic auditor, including tool pollution.

--output-dir retains every input, stub invocation, final answer and console log.
The stub simulates a tree search; it does not manufacture the wrapper's verdict.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HARNESS = Path(__file__).resolve().with_name("audit-report.sh")
STUB = r'''#!/usr/bin/env python3
import json, os, re, sys
from pathlib import Path
args = sys.argv[1:]
assert args[0] == "exec", args
assert sys.stdin.read() == ""
root = Path(os.environ["AUDIT_TEST_CASE"])
with (root / "calls.jsonl").open("a") as f:
    f.write(json.dumps(args) + "\n")
prompt = args[-1]
(root / "prompt.txt").write_text(prompt)
final = (root / "fixture.txt").read_text()
raw = (root / "raw.txt").read_text()
if os.environ.get("AUDIT_TEST_SEARCH"):
    # Old harness exposes the source directory. New harness supplies only the
    # private snapshot. Search recursively, including archived transcripts.
    match = re.search(r"Read the draft at (.+?)\. For EVERY", prompt)
    if not match:
        match = re.search(r"DRAFT_PATH=(.+)", prompt)
    directory = Path(match[1]).parent
    for path in directory.rglob("*.audit-*.txt"):
        final += path.read_text()
if "--output-last-message" in args:
    dest = Path(args[args.index("--output-last-message") + 1])
    if not os.environ.get("AUDIT_TEST_NO_FINAL"):
        dest.write_text(final)
if os.environ.get("AUDIT_TEST_MUTATE"):
    (root / "draft.md").write_text("Changed while auditing.\n")
# A transcript repeats earlier tool findings, prompt text and earlier verdicts.
sys.stdout.write(raw + final)
sys.stderr.write("stub stderr retained\n")
sys.exit(int(os.environ.get("AUDIT_TEST_RC", "0")))
'''


def claim(quote, status="VERIFIED", evidence="checked the named revision", line=1):
    return "CLAIM " + json.dumps(dict(line=line, quote=quote, status=status,
                                       evidence=evidence)) + "\n"


class GateTests(unittest.TestCase):
    root = None

    def setUp(self):
        self.case = self.root / self._testMethodName
        self.case.mkdir()
        bindir = self.case / "bin"
        bindir.mkdir()
        stub = bindir / "codex"
        stub.write_text(STUB)
        stub.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{bindir}{os.pathsep}{os.environ['PATH']}",
                        AUDIT_TEST_CASE=str(self.case),
                        # The production cache is private and outside the repo.
                        # Each test gets its own persistent cache, never a mock.
                        AUDIT_REPORT_STATE_DIR=str(self.case / "state"))
        self.draft = self.case / "draft.md"
        self.draft.write_text("The count is 4.\n")
        self.number = 0

    def run_gate(self, final=None, raw="", **settings):
        self.number += 1
        (self.case / "fixture.txt").write_text(
            final if final is not None else claim("The count is 4.") + "VERDICT: SEND\n")
        (self.case / "raw.txt").write_text(raw)
        env = dict(self.env, **settings)
        result = subprocess.run([str(HARNESS), str(self.draft)], env=env,
                                capture_output=True, text=True, timeout=20)
        console = result.stdout + result.stderr
        (self.case / f"run-{self.number}.console").write_text(console)
        print(f"\n{self._testMethodName} run {self.number}:\n{console.rstrip()}", flush=True)
        return console

    def test_clean_twice(self):
        first = self.run_gate(AUDIT_TEST_SEARCH="1")
        self.assertIn("VERDICT SEND", first)
        second = self.run_gate(AUDIT_TEST_SEARCH="1", raw="Obsolete -> WRONG stale tool output\n")
        self.assertIn("VERDICT SEND", second)
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 1,
                         "one derivation per hash, rather than reading previous transcripts")

    def test_unrelated_transcript(self):
        (self.case / "unrelated.md.audit-deadbeef.txt").write_text(
            "Obsolete draft -> WRONG contradicted old claim\nVERDICT: BLOCK\n")
        self.assertIn("VERDICT SEND", self.run_gate(AUDIT_TEST_SEARCH="1"))

    def test_archived_transcript(self):
        archive = self.case / "audits-archive"
        archive.mkdir()
        (archive / "unrelated.md.audit-deadbeef.txt").write_text(
            "RESULT 2026-11-05 old-call-id Obsolete -> WRONG contradicted\nVERDICT: BLOCK\n")
        self.assertIn("VERDICT SEND", self.run_gate(AUDIT_TEST_SEARCH="1"))

    def test_prompt_echo(self):
        echo = "Use one line per claim: CLAIM -> VERIFIED <evidence>,\nCLAIM -> WRONG <contradicting\n"
        console = self.run_gate(claim("The count is 4.") + echo + "VERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("2 contamination", console)

    def test_real_contradiction(self):
        self.draft.write_text("The count is 999999.\n")
        final = "The count is 999999. -> WRONG query returned 4\n" + "VERDICT: SEND\n"
        self.assertIn("VERDICT BLOCK", self.run_gate(final))
        self.assertIn("VERDICT BLOCK", self.run_gate(final))  # rerun cannot launder BLOCK

    def test_structured_contradiction(self):
        self.assertIn("VERDICT BLOCK", self.run_gate(
            claim("The count is 4.", "WRONG", "query returned 5") + "VERDICT: SEND\n"))

    def test_prompt_echo_unwrapped(self):
        echo = "Use one line per claim: CLAIM -> VERIFIED <evidence>, CLAIM -> WRONG <contradicting\n"
        self.assertIn("VERDICT SEND", self.run_gate(claim("The count is 4.") + echo + "VERDICT: SEND\n"))

    def test_stale_decision_refuses(self):
        self.assertIn("VERDICT SEND", self.run_gate())
        decision = next((self.case / "state").rglob("decision.json"))
        record = json.loads(decision.read_text())
        record["created"] -= 601
        decision.write_text(json.dumps(record))
        self.assertIn("AUDIT INCONCLUSIVE", self.run_gate())
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 1)

    def test_replay_never_reads_transcripts(self):
        self.assertIn("VERDICT SEND", self.run_gate())
        for name in ("transcript.txt", "final.txt"):
            next((self.case / "state").rglob(name)).unlink()
        self.assertIn("VERDICT SEND", self.run_gate())
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 1)

    def test_failed_hash_is_not_retried(self):
        self.assertIn("AUDIT INCONCLUSIVE", self.run_gate(AUDIT_TEST_RC="17"))
        self.assertIn("AUDIT INCONCLUSIVE", self.run_gate())
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 1)

    def test_final_channel_only(self):
        self.assertIn("VERDICT SEND", self.run_gate(raw=(
            "tool_result 2026-11-05 call-old The count is 4. -> WRONG old evidence\n"
            "VERDICT: BLOCK\n")))

    def test_access_only(self):
        console = self.run_gate(claim("The count is 4.", "UNVERIFIABLE", "gh HTTP 403") + "VERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("0 wrong, 1 unverifiable", console)

    def test_no_verdict(self):
        self.assertIn("AUDIT MALFORMED (no VERDICT line)", self.run_gate(claim("The count is 4.")))

    def test_conflicting_final_verdicts(self):
        self.assertIn("AUDIT AMBIGUOUS", self.run_gate(claim("The count is 4.") + "VERDICT: BLOCK\nVERDICT: SEND\n"))

    def test_quoted_verdict(self):
        self.assertIn("AUDIT MALFORMED (no VERDICT line)", self.run_gate("'VERDICT: SEND'\n"))

    def test_failed_auditor(self):
        self.assertIn("AUDIT INCONCLUSIVE", self.run_gate(AUDIT_TEST_RC="17"))

    def test_missing_final(self):
        self.assertIn("AUDIT MALFORMED", self.run_gate(AUDIT_TEST_NO_FINAL="1"))

    def test_mutated_draft(self):
        self.assertIn("AUDIT INCONCLUSIVE (draft changed during audit)", self.run_gate(AUDIT_TEST_MUTATE="1"))

    def test_absence_prepass(self):
        for phrase in ("never", "does not exist", "exists nowhere"):
            self.draft.write_text(f"The feature {phrase}.\n")
            self.assertIn("VERDICT BLOCK", self.run_gate())
        self.assertFalse((self.case / "calls.jsonl").exists())

    def test_changed_hash_gets_new_pass(self):
        self.assertIn("VERDICT BLOCK", self.run_gate(claim("The count is 4.", "WRONG", "checked 5") + "VERDICT: BLOCK\n"))
        self.draft.write_text("The count is 5.\n")
        self.assertIn("VERDICT SEND", self.run_gate(claim("The count is 5.") + "VERDICT: SEND\n"))
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 2)

    def test_unbound_wrong_is_contamination(self):
        console = self.run_gate(claim("The count is 4.") +
                                claim("Obsolete claim", "WRONG", "old transcript") + "VERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("1 contamination", console)

    def test_wrong_line_refuses(self):
        console = self.run_gate(claim("The count is 4.") +
                                claim("The count is 4.", "WRONG", "checked 5", line=9) + "VERDICT: SEND\n")
        self.assertNotIn("VERDICT SEND", console)
        self.assertIn("AUDIT MALFORMED", console)

    def test_wrong_colon_control(self):
        # Do not silently drop a real but misformatted contradiction (#743).
        for token in ("WRONG:", "WRONG"):
            self.draft.write_text(f"The count is {token}.\n")
            console = self.run_gate(f"The count is {token}. -> WRONG: checked 4\nVERDICT: SEND\n")
            self.assertNotIn("VERDICT SEND", console)
            self.assertIn("VERDICT BLOCK", console)

    def test_no_claims_refuses(self):
        self.assertIn("AUDIT MALFORMED", self.run_gate("VERDICT: SEND\n"))

    def test_block_without_contradiction_refuses(self):
        self.assertIn("AUDIT MALFORMED", self.run_gate(claim("The count is 4.") + "VERDICT: BLOCK\n"))

    def test_missing_draft(self):
        self.draft.unlink()
        self.assertIn("VERDICT BLOCK", self.run_gate())

    def test_isolation_contract(self):
        self.assertIn("VERDICT SEND", self.run_gate())
        args = json.loads((self.case / "calls.jsonl").read_text().splitlines()[0])
        for flag in ("--ephemeral", "--output-last-message"):
            self.assertIn(flag, args)
        self.assertIn("memories.use_memories=false", args)
        self.assertIn("memories.generate_memories=false", args)
        self.assertNotIn(str(self.draft), (self.case / "prompt.txt").read_text())
        output = Path(args[args.index("--output-last-message") + 1])
        self.assertNotEqual(output.parent, self.draft.parent)
        self.assertIn("Do not read prior audit", (self.case / "prompt.txt").read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    args, tests = parser.parse_known_args()
    def run(root):
        GateTests.root = root.resolve()
        suite = unittest.defaultTestLoader.loadTestsFromNames(tests, GateTests) if tests else unittest.defaultTestLoader.loadTestsFromTestCase(GateTests)
        return unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=False)
        ok = run(args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="audit-report-selftest-") as temp:
            ok = run(Path(temp))
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
