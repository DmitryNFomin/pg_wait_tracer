#!/usr/bin/env python3
"""Black-box the real gate with a deterministic auditor, including tool pollution.

--output-dir retains every input, stub invocation, final answer and console log.
The stub simulates a tree search; it does not manufacture the wrapper's verdict.
"""
import argparse
from datetime import datetime, timezone
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
if os.environ.get("AUDIT_TEST_REPO_MUTATE"):
    Path(os.environ["AUDIT_TEST_REPO_MUTATE"]).write_text("Repository changed while auditing.\n")
# A transcript repeats earlier tool findings, prompt text and earlier verdicts.
sys.stdout.write(raw + final)
sys.stderr.write("stub stderr retained\n")
sys.exit(int(os.environ.get("AUDIT_TEST_RC", "0")))
'''
GH_STUB = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
root = Path(os.environ["AUDIT_TEST_CASE"])
with (root / "gh-calls.jsonl").open("a") as f:
    f.write(json.dumps(args) + "\n")
assert sys.stdin.read() == ""
kind = " ".join(args[:2])
fields = {
    "pr view": "number,title,state,isDraft,mergedAt,closedAt,mergeStateStatus,headRefOid,autoMergeRequest,closingIssuesReferences,statusCheckRollup",
    "issue view": "number,title,state,closedAt,comments",
    "pr list": "number,title,state,mergedAt",
}
assert args[-2:] == ["--json", fields[kind]], args
if os.environ.get("AUDIT_TEST_GH_FAIL") in ("all", kind):
    sys.stderr.write("error connecting to api.github.com\n")
    sys.exit(1)
if os.environ.get("AUDIT_TEST_GH_BAD_JSON"):
    sys.stdout.write("not JSON\n")
    sys.exit(0)
if kind == "pr view" and args[2] == "850":
    sys.stderr.write("GraphQL: Could not resolve to a PullRequest with the number of 850. (repository.pullRequest)\n")
    sys.exit(1)
if kind == "pr list":
    assert args == ["pr", "list", "--state", "all", "--limit", "40", "--json", fields[kind]], args
    data = [{"number": 882, "title": "CI evidence", "state": "MERGED", "mergedAt": "2026-11-12T19:49:00Z"}]
elif kind == "issue view":
    data = dict(number=int(args[2]), title="Rework", state="OPEN", closedAt=None,
                comments=[dict(body="Owner scope", createdAt="2026-11-12T19:00:00Z")])
else:
    data = dict(number=int(args[2]), title="Change", state="OPEN", isDraft=False,
                mergedAt=None, closedAt=None, mergeStateStatus="CLEAN", headRefOid="a" * 40,
                autoMergeRequest=None, closingIssuesReferences=[], statusCheckRollup=[])
if os.environ.get("AUDIT_TEST_SNAPSHOT_WRITE_FAIL"):
    for directory in Path(os.environ["AUDIT_REPORT_STATE_DIR"]).rglob("github-snapshot-*"):
        (directory / ("pr-list.json" if kind == "pr list" else f"{args[0]}-{args[2]}.json")).mkdir(exist_ok=True)
sys.stdout.write(json.dumps(data) + "\n")
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
        gh = bindir / "gh"
        gh.write_text(GH_STUB)
        gh.chmod(0o755)
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

    def test_summary_status_is_contamination(self):
        console = self.run_gate(claim("The count is 4.") +
                                "Summary: 1 claim checked, none WRONG.\nVERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("1 contamination", console)
        self.assertNotIn("AUDIT MALFORMED", console)

    def test_current_prompt_echo_is_contamination(self):
        console = self.run_gate(claim("The count is 4.") +
                                "status (VERIFIED, WRONG or UNVERIFIABLE)\nVERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("1 contamination", console)
        self.assertNotIn("AUDIT MALFORMED", console)

    def test_malformed_legacy_finding_refuses(self):
        console = self.run_gate(claim("The count is 4.") +
                                "A paraphrased count -> WRONG checked 5\nVERDICT: SEND\n")
        self.assertIn("AUDIT MALFORMED (unbound or unstructured finding)", console)
        self.assertNotIn("VERDICT SEND", console)

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

    def test_failed_auditor_can_retry_same_hash(self):
        self.assertIn("AUDIT INCONCLUSIVE", self.run_gate(AUDIT_TEST_RC="17"))
        self.assertIn("VERDICT SEND", self.run_gate())
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 2)

    def test_repository_change_can_retry_same_hash(self):
        with tempfile.TemporaryDirectory(prefix="audit-selftest-tree-", dir=HARNESS.parents[2]) as temp:
            console = self.run_gate(AUDIT_TEST_REPO_MUTATE=str(Path(temp) / "new-file"))
            self.assertIn("AUDIT INCONCLUSIVE (repository changed during audit)", console)
            self.assertIn("VERDICT SEND", self.run_gate())
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 2)

    def test_retry_does_not_reuse_aborted_final(self):
        self.assertIn("AUDIT INCONCLUSIVE", self.run_gate(AUDIT_TEST_RC="17"))
        self.assertIn("AUDIT MALFORMED (no VERDICT line; missing final answer)",
                      self.run_gate(AUDIT_TEST_NO_FINAL="1"))
        self.assertIn("VERDICT SEND", self.run_gate())
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 3)

    def test_replay_reapplies_wrong_backstop(self):
        self.assertIn("VERDICT BLOCK", self.run_gate(
            claim("The count is 4.", "WRONG", "checked 5") + "VERDICT: SEND\n"))
        decision = next((self.case / "state").rglob("decision.json"))
        record = json.loads(decision.read_text())
        self.assertGreaterEqual(record["counts"]["WRONG"], 1)
        record["verdict"] = "SEND"
        decision.write_text(json.dumps(record))
        console = self.run_gate()
        self.assertIn("VERDICT BLOCK", console)
        self.assertNotIn("VERDICT SEND", console)
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 1)

    def test_final_channel_only(self):
        self.assertIn("VERDICT SEND", self.run_gate(raw=(
            "tool_result 2026-11-05 call-old The count is 4. -> WRONG old evidence\n"
            "VERDICT: BLOCK\n")))

    def test_access_only(self):
        console = self.run_gate(claim("The count is 4.", "UNVERIFIABLE", "gh HTTP 403") + "VERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("0 wrong, 1 unverifiable", console)

    def test_github_snapshot_failure_refuses(self):
        self.draft.write_text("PR #850 is open.\n")
        console = self.run_gate(
            claim("PR #850 is open.", "UNVERIFIABLE", "gh connection failed") +
            "VERDICT: SEND\n", AUDIT_TEST_GH_FAIL="all")
        self.assertIn("AUDIT INCONCLUSIVE (GitHub snapshot failed:", console)
        self.assertNotIn("VERDICT SEND", console)
        self.assertFalse((self.case / "calls.jsonl").exists(),
                         "failed snapshot must stop before the auditor runs")
        self.assertFalse(list((self.case / "state").rglob("decision.json")))
        self.assertEqual(len((self.case / "gh-calls.jsonl").read_text().splitlines()), 1,
                         "a connection failure must not trigger the issue fallback")
        # Negative control: success must run the auditor on the same hash.
        self.assertIn("VERDICT SEND", self.run_gate(
            claim("PR #850 is open.") + "VERDICT: SEND\n"))

    def test_github_snapshot_success_proceeds(self):
        self.draft.write_text("PR #882 is merged; issue #850 is open; see #882.\n")
        console = self.run_gate(claim(self.draft.read_text().strip()) + "VERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        snapshots = list((self.case / "state").rglob("github-snapshot-*"))
        self.assertEqual(len(snapshots), 1)
        directory = snapshots[0]
        self.assertEqual(directory.stat().st_mode & 0o777, 0o500)
        prompt = (self.case / "prompt.txt").read_text()
        self.assertIn(f"GITHUB_SNAPSHOT_DIR={directory}", prompt)
        self.assertIn("A claim contradicted by this snapshot is WRONG.", prompt)
        self.assertEqual({p.name for p in directory.iterdir()},
                         {"pr-882.json", "issue-850.json", "pr-list.json"})
        for path in directory.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o400)
            record = json.loads(path.read_text())
            timestamp = datetime.fromisoformat(record["fetched_at_utc"].replace("Z", "+00:00"))
            self.assertEqual(timestamp.utcoffset().total_seconds(), 0)
            self.assertLess(abs((datetime.now(timezone.utc) - timestamp).total_seconds()), 30)
            self.assertEqual(record["command"][0], "gh")
        issue = json.loads((directory / "issue-850.json").read_text())["data"]
        self.assertEqual(issue["comments"][0]["body"], "Owner scope")
        self.assertEqual(json.loads((directory / "pr-882.json").read_text())["data"]["number"], 882)
        self.assertEqual(json.loads((directory / "pr-list.json").read_text())["data"][0]["state"], "MERGED")
        gh_calls = [json.loads(line) for line in (self.case / "gh-calls.jsonl").read_text().splitlines()]
        self.assertEqual([args[:3] for args in gh_calls], [
            ["pr", "view", "850"], ["issue", "view", "850"],
            ["pr", "view", "882"], ["pr", "list", "--state"]])
        calls = (self.case / "calls.jsonl").read_text().splitlines()
        self.assertEqual(len(calls), 1)
        # Negative control: a checked contradiction still BLOCKs with snapshots.
        self.draft.write_text("PR #882 is open.\n")
        self.assertIn("VERDICT BLOCK", self.run_gate(
            claim("PR #882 is open.", "WRONG", "pr-list.json data[0].state=MERGED") + "VERDICT: SEND\n"))

    def test_without_github_claim_skips_snapshot(self):
        self.draft.write_text("The printer checked merger circuitry.\n")
        self.assertIn("VERDICT SEND", self.run_gate(
            claim("The printer checked merger circuitry.") + "VERDICT: SEND\n",
            AUDIT_TEST_GH_FAIL="all"))
        self.assertFalse((self.case / "gh-calls.jsonl").exists())
        self.assertFalse(list((self.case / "state").rglob("github-snapshot-*")))
        # Negative control: one whole-word GitHub marker requires a snapshot.
        self.draft.write_text("The check passed.\n")
        self.assertIn("AUDIT INCONCLUSIVE (GitHub snapshot failed:", self.run_gate(
            claim("The check passed.") + "VERDICT: SEND\n", AUDIT_TEST_GH_FAIL="all"))
        self.assertEqual(len((self.case / "calls.jsonl").read_text().splitlines()), 1)

    def test_github_claim_detection(self):
        for index, text in enumerate(("See #850.", "A pr exists.", "Already MERGED.",
                                      "An issue exists.", "CI passed.", "The check passed.")):
            with self.subTest(text=text):
                self.env["AUDIT_REPORT_STATE_DIR"] = str(self.case / f"state-{index}")
                self.draft.write_text(text + "\n")
                console = self.run_gate(claim(text) + "VERDICT: SEND\n",
                                        AUDIT_TEST_GH_FAIL="all")
                self.assertIn("AUDIT INCONCLUSIVE (GitHub snapshot failed:", console)
                self.assertNotIn("VERDICT SEND", console)

    def test_github_partial_snapshot_failure_refuses(self):
        for index, kind in enumerate(("issue view", "pr list")):
            with self.subTest(kind=kind):
                self.env["AUDIT_REPORT_STATE_DIR"] = str(self.case / f"state-{index}")
                self.draft.write_text("Issue #850 is open.\n")
                console = self.run_gate(claim("Issue #850 is open.") + "VERDICT: SEND\n",
                                        AUDIT_TEST_GH_FAIL=kind)
                self.assertIn("AUDIT INCONCLUSIVE (GitHub snapshot failed:", console)
                self.assertFalse((self.case / "calls.jsonl").exists())
                self.assertFalse(list(Path(self.env["AUDIT_REPORT_STATE_DIR"]).rglob("decision.json")))
        self.assertIn("VERDICT SEND", self.run_gate(claim("Issue #850 is open.") + "VERDICT: SEND\n"))

    def test_github_snapshot_write_failure_refuses(self):
        self.draft.write_text("PR #882 is open.\n")
        console = self.run_gate(claim("PR #882 is open.") + "VERDICT: SEND\n",
                                AUDIT_TEST_SNAPSHOT_WRITE_FAIL="1")
        self.assertIn("AUDIT INCONCLUSIVE (GitHub snapshot failed:", console)
        self.assertFalse((self.case / "calls.jsonl").exists())
        self.assertIn("VERDICT SEND", self.run_gate(claim("PR #882 is open.") + "VERDICT: SEND\n"))

    def test_github_invalid_json_refuses(self):
        self.draft.write_text("CI passed.\n")
        console = self.run_gate(claim("CI passed.") + "VERDICT: SEND\n", AUDIT_TEST_GH_BAD_JSON="1")
        self.assertIn("AUDIT INCONCLUSIVE (GitHub snapshot failed:", console)
        self.assertFalse((self.case / "calls.jsonl").exists())
        self.assertIn("VERDICT SEND", self.run_gate(claim("CI passed.") + "VERDICT: SEND\n"))

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

    def test_unbound_wrong_is_malformed(self):
        console = self.run_gate(claim("The count is 4.") +
                                claim("Obsolete claim", "WRONG", "old transcript") + "VERDICT: SEND\n")
        self.assertIn("AUDIT MALFORMED (structured WRONG does not quote the current draft)", console)
        self.assertNotIn("VERDICT SEND", console)

    def test_wrong_quote_normalization_blocks(self):
        quotes = ("The count is 4. ", "  The\tcount   is 4.  ",
                  'It\'s "four".', 'It’s “four”.')
        drafts = ("The count is 4.\n", "The count is 4.\n",
                  'It’s\t“four”.\n', 'It\'s "four".\n')
        for index, (quote, draft) in enumerate(zip(quotes, drafts)):
            with self.subTest(quote=quote):
                self.env["AUDIT_REPORT_STATE_DIR"] = str(self.case / f"state-{index}")
                self.draft.write_text(draft)
                console = self.run_gate(claim(draft.strip()) +
                                        claim(quote, "WRONG", "checked 5") + "VERDICT: SEND\n")
                self.assertIn("VERDICT BLOCK", console)
                self.assertNotIn("VERDICT SEND", console)

    def test_lowercase_wrong_quote_is_malformed(self):
        console = self.run_gate(claim("The count is 4.") +
                                claim("the count is 4.", "WRONG", "checked 5") + "VERDICT: SEND\n")
        self.assertIn("AUDIT MALFORMED (structured WRONG does not quote the current draft)", console)
        self.assertNotIn("VERDICT SEND", console)

    def test_unbound_nonwrong_is_contamination(self):
        console = self.run_gate(claim("The count is 4.") + claim("Obsolete claim") +
                                claim("Another obsolete claim", "UNVERIFIABLE", "gh HTTP 403") +
                                "VERDICT: SEND\n")
        self.assertIn("VERDICT SEND", console)
        self.assertIn("2 contamination", console)

    def test_wrong_line_refuses(self):
        console = self.run_gate(claim("The count is 4.") +
                                claim("The count is 4.", "WRONG", "checked 5", line=2) + "VERDICT: SEND\n")
        self.assertNotIn("VERDICT SEND", console)
        self.assertIn("AUDIT MALFORMED (structured WRONG does not quote the current draft)", console)

    def test_wrong_quote_on_different_line_refuses(self):
        self.draft.write_text("The count is 4.\nAnother claim.\n")
        console = self.run_gate(claim("The count is 4.") +
                                claim("The count is 4.", "WRONG", "checked 5", line=2) + "VERDICT: SEND\n")
        self.assertNotIn("VERDICT SEND", console)
        self.assertIn("AUDIT MALFORMED (structured WRONG does not quote the current draft)", console)

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
        self.assertEqual(args[args.index("--sandbox") + 1], "read-only")
        self.assertEqual([args[i + 1] for i, arg in enumerate(args) if arg == "-c"],
                         ['sandbox_mode="read-only"', "memories.use_memories=false",
                          "memories.generate_memories=false"])
        self.assertIn("memories.use_memories=false", args)
        self.assertIn("memories.generate_memories=false", args)
        self.assertNotIn(str(self.draft), (self.case / "prompt.txt").read_text())
        output = Path(args[args.index("--output-last-message") + 1])
        self.assertNotEqual(output.parent, self.draft.parent)
        self.assertIn("Do not read prior audit", (self.case / "prompt.txt").read_text())
        self.assertIn("No summary or narration.", (self.case / "prompt.txt").read_text())


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
