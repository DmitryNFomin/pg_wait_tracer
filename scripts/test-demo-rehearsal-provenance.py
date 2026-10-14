#!/usr/bin/env python3
"""Hermetic launcher provenance checks; no SSH, box, build or live workload.

Run: python3 scripts/test-demo-rehearsal-provenance.py
Optional launcher path allows checking the pre-change version for red evidence.
"""

import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / "scripts/demo-rehearsal.sh"


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / "checkout"
        (self.repo / "scripts").mkdir(parents=True)
        (self.repo / "tests/results").mkdir(parents=True)
        shutil.copy(ROOT / "scripts/tree-hash.sh", self.repo / "scripts/tree-hash.sh")
        shutil.copy(ROOT / "scripts/hash-synced-tree.sh", self.repo / "scripts/hash-synced-tree.sh")
        shutil.copy(ROOT / "tests/demo_rehearsal_orchestrator_lib.py", self.repo / "tests")
        (self.repo / ".gitignore").write_text("tests/results/\n")
        (self.repo / "payload").write_text("candidate\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-qm", "candidate")
        source = LAUNCHER.read_text()
        guard = source[source.index("refuse_previous_results() {"):].split("\n}\n", 1)[0] + "\n}\n"
        helpers = source[source.index("record_launcher_start() {"):source.index("# ── --collect:")]
        collect = source[source.index("collect_results() {"):source.index("# See the top-of-file comment:")]
        reattach = source[source.index('if [[ "$MODE" == "collect" ]]; then'):
                          source.index('if [[ "$PERSISTENT" != "1" && -z "$token" ]]; then')]
        self.helpers = self.base / "helpers.sh"
        self.helpers.write_text(guard + helpers + collect)
        self.reattach = self.base / "reattach.sh"
        self.reattach.write_text(reattach)
        self.sidecar = self.repo / "tests/results/provenance.json"
        self.log = self.repo / "tests/results/launcher.log"
        self.remote = self.base / "remote-results"
        self.remote.mkdir()
        self.remote_tree = self.base / "remote-checkout"
        self.remote_tree.mkdir()
        self.summary = {"ok": True, "failed": [], "tag": None,
                        "extra_checks": {"sentinel": "preserve"}}
        (self.remote / "run.id").write_text("100\n")
        (self.remote / "summary.json").write_text(json.dumps(self.summary))

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.repo, text=True).strip()

    def shell(self, commands, success=True):
        # Only launcher functions are sourced. Network-facing commands are
        # shell mocks; no real launcher startup, sweep, token or trap runs.
        prefix = f"""
set -uo pipefail
source {shlex.quote(str(self.helpers))}
PROVENANCE_FILE={shlex.quote(str(self.sidecar))}
log={shlex.quote(str(self.log))}
demo_rehearsal_start_epoch=100
target=user@fixture-box
remote_dir=fixture-run
server_id=persistent
server_name=unused
PERSISTENT=1
PG=18
BOX_LOCK_QUEUE_LINE=''
synced_back=0
results_ok=0
results_collection_refused=0
ssh_strict=''
FIXTURE_REMOTE_TREE={shlex.quote(str(self.remote_tree))}
HASH_MODE=compute
REMOTE_MUTATION=''
FIXTURE_UPTIME='up at start, load average: 1.00'
FIXTURE_STARTED=112
FAIL_SSH=0
ssh() {{
    [[ "$FAIL_SSH" == 0 ]] || return 255
    case "${{*: -1}}" in
        uptime) echo "$FIXTURE_UPTIME" ;;
        hostname) echo fixture-box ;;
        *rehearsal.started*) echo "$FIXTURE_STARTED" ;;
        'bash -s -- '*)
            case "$HASH_MODE" in
                compute) bash -s -- "$FIXTURE_REMOTE_TREE" ;;
                missing-tool)
                    git() {{ return 127; }}
                    export -f git
                    bash -s -- "$FIXTURE_REMOTE_TREE" ;;
                invalid) echo not-a-tree-hash ;;
            esac
            ;;
        *) return 99 ;;
    esac
}}
rsync() {{
    if [[ "${{*: -1}}" == "$target:$remote_dir/" ]]; then
        command rsync "${{@:1:$#-1}}" "$FIXTURE_REMOTE_TREE/" || return
        case "$REMOTE_MUTATION" in
            changed) echo damaged > "$FIXTURE_REMOTE_TREE/payload" ;;
            missing) rm "$FIXTURE_REMOTE_TREE/payload" ;;
            extra) echo stale > "$FIXTURE_REMOTE_TREE/extra-source.py" ;;
            mode) chmod +x "$FIXTURE_REMOTE_TREE/payload" ;;
        esac
    else
        cp -R {shlex.quote(str(self.remote))}/. "${{*: -1}}"
    fi
}}
"""
        result = subprocess.run(["bash", "-c", prefix + commands], cwd=self.repo,
                                text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def collected(self):
        return json.loads((self.repo / "tests/results/demo_rehearsal/summary.json").read_text())

    def test_clean_and_dirty_checkout_identity(self):
        expected_sha = self.git("rev-parse", "HEAD")
        self.shell("record_launcher_start")
        clean = json.loads(self.sidecar.read_text())
        self.assertEqual(clean["candidate_sha"], expected_sha)
        self.assertEqual(clean["tree_hash"], self.git("rev-parse", "HEAD^{tree}"))
        self.assertTrue(clean["git_status_porcelain_empty"])
        (self.repo / "payload").write_text("changed\n")
        self.shell("record_launcher_start")
        dirty = json.loads(self.sidecar.read_text())
        self.assertEqual(dirty["candidate_sha"], expected_sha)
        self.assertFalse(dirty["git_status_porcelain_empty"])
        self.assertNotEqual(dirty["tree_hash"], clean["tree_hash"])

    def test_collection_preserves_launch_identity_and_verdict(self):
        self.shell("record_launcher_start; sync_launcher_tree")
        launch = json.loads(self.sidecar.read_text())
        (self.repo / "payload").write_text("collector's different candidate\n")
        self.git("add", "payload")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-qm", "collector")
        self.shell("FIXTURE_UPTIME='up at end, load average: 2.00'; collect_results; [[ $results_ok == 1 ]]")
        summary = self.collected()
        record = summary.pop("launcher_provenance")
        self.assertEqual(summary, self.summary)
        self.assertEqual(record["candidate_sha"], launch["candidate_sha"])
        self.assertNotEqual(record["candidate_sha"], self.git("rev-parse", "HEAD"))
        self.assertEqual(record["tree_hash"], launch["tree_hash"])
        self.assertEqual(record["remote_tree_hash"], launch["tree_hash"])
        self.assertEqual(record["tree_hash_verification"], "match")
        self.assertEqual(record["box_uptime_start"], "up at start, load average: 1.00")
        self.assertEqual(record["box_uptime_end"], "up at end, load average: 2.00")
        self.assertEqual(record["box_target"], "user@fixture-box")
        self.assertEqual(record["box_hostname"], "fixture-box")
        self.assertIn("queued 12s behind the box lock", record["box_lock_queue_line"])
        log = self.log.read_text()
        for value in (record["candidate_sha"], record["tree_hash"],
                      record["box_uptime_start"], record["box_uptime_end"],
                      record["box_lock_queue_line"]):
            self.assertIn(value, log)
        self.assertIn('"git_status_porcelain_empty": true', log)

    def test_sync_hashes_the_exported_snapshot_on_the_receiver(self):
        # Tracked results used to disappear under the blanket rsync exclude.
        tracked = self.repo / "tests/results/tracked.json"
        tracked.write_text('{"fixture": true}\n')
        self.git("add", "-f", "tests/results/tracked.json")
        (self.repo / "untracked.py").write_text("print('candidate')\n")
        (self.repo / "executable.sh").write_text("#!/bin/sh\n")
        (self.repo / "executable.sh").chmod(0o755)
        (self.repo / "link").symlink_to("payload")
        (self.remote_tree / "stale.py").write_text("old source\n")
        self.shell("record_launcher_start; sync_launcher_tree")
        record = json.loads(self.sidecar.read_text())
        self.assertEqual(record["tree_hash_verification"], "match")
        self.assertEqual(record["remote_tree_hash"], record["tree_hash"])
        self.assertTrue((self.remote_tree / "tests/results/tracked.json").exists())
        self.assertTrue((self.remote_tree / "untracked.py").exists())
        self.assertTrue((self.remote_tree / "link").is_symlink())
        self.assertFalse((self.remote_tree / "stale.py").exists())
        self.assertFalse((self.remote_tree / ".git").exists())
        self.assertFalse((self.remote_tree / "tests/results/provenance.json").exists())

    def test_remote_hash_mismatch_refuses_launch_and_records_both_hashes(self):
        for mutation in ("changed", "missing", "extra", "mode"):
            with self.subTest(mutation=mutation):
                result = self.shell(f"""
record_launcher_start
REMOTE_MUTATION={mutation}
sync_launcher_tree && touch capture-started
""", success=False)
                record = json.loads(self.sidecar.read_text())
                self.assertEqual(record["tree_hash_verification"], "mismatch")
                self.assertIsNotNone(record["remote_tree_hash"])
                self.assertNotEqual(record["remote_tree_hash"], record["tree_hash"])
                self.assertFalse((self.repo / "capture-started").exists())
                self.assertIn("refusing to launch", result.stderr)
                self.assertIn(record["remote_tree_hash"], self.log.read_text())

    def test_unavailable_remote_hash_refuses_launch_and_records_absence(self):
        for failure in ("HASH_MODE=missing-tool", "HASH_MODE=invalid", "FAIL_SSH=1"):
            with self.subTest(failure=failure):
                result = self.shell(f"""
record_launcher_start
{failure}
sync_launcher_tree && touch capture-started
""", success=False)
                record = json.loads(self.sidecar.read_text())
                self.assertIsNone(record["remote_tree_hash"])
                self.assertEqual(record["tree_hash_verification"], "unavailable")
                self.assertFalse((self.repo / "capture-started").exists())
                self.assertIn("remote tree hash unavailable", result.stderr)

    def test_sync_and_verification_gate_precedes_capture(self):
        source = LAUNCHER.read_text()
        gate = source.index("    sync_launcher_tree || exit 1")
        capture = source.index('echo "demo-rehearsal: starting the capture DETACHED')
        self.assertLess(gate, capture)

    def test_failed_uptime_and_start_probes_report_absence(self):
        self.shell("FAIL_SSH=1; record_launcher_start; collect_results; [[ $results_ok == 1 ]]")
        record = self.collected()["launcher_provenance"]
        for field in ("box_uptime_start", "box_uptime_end", "box_hostname"):
            self.assertIsNone(record[field])
        self.assertIn("queue duration unavailable", record["box_lock_queue_line"])

    def test_collect_reloads_saved_provenance_instead_of_environment(self):
        self.shell("record_launcher_start")
        saved = self.base / "saved provenance.json"
        self.sidecar.rename(saved)
        state = self.base / "state.sh"
        state.write_text("\n".join((
            "SERVER_ID=persistent", "SERVER_IP=fixture-box", "TARGET=user@fixture-box",
            "REMOTE_DIR=fixture-run", "START_EPOCH=100", "DURATION_MIN=35",
            "PG_VERSION_REQUESTED=18", "PG_VERSION_CONFIRMED=18",
            "PROVENANCE_FILE=" + shlex.quote(str(saved)),
        )) + "\n")
        self.shell("MODE=collect\nSTATE_FILE=" + shlex.quote(str(state)) +
                   "\nPROVENANCE_FILE=/bogus/environment.json\nsource " +
                   shlex.quote(str(self.reattach)) +
                   "\ncollect_results; [[ $results_ok == 1 ]]")
        self.assertTrue(self.collected()["launcher_provenance"]["launch_provenance_available"])
        self.assertEqual(self.collected()["launcher_provenance"]["candidate_sha"],
                         self.git("rev-parse", "HEAD"))

    def test_old_state_does_not_invent_launch_identity(self):
        self.shell("PROVENANCE_FILE=''; collect_results; [[ $results_ok == 1 ]]")
        record = self.collected()["launcher_provenance"]
        self.assertFalse(record["launch_provenance_available"])
        for field in ("candidate_sha", "tree_hash", "git_status_porcelain_empty",
                      "worktree", "box_uptime_start"):
            self.assertIsNone(record[field])

    def test_missing_saved_sidecar_does_not_invent_launch_identity(self):
        self.shell("collect_results; [[ $results_ok == 1 ]]")
        record = self.collected()["launcher_provenance"]
        self.assertFalse(record["launch_provenance_available"])
        self.assertIsNone(record["candidate_sha"])

    def test_foreign_sidecar_refuses_to_replace_previous_result(self):
        self.shell("record_launcher_start")
        record = json.loads(self.sidecar.read_text())
        record["run_id"] = "99"
        self.sidecar.write_text(json.dumps(record))
        previous = self.repo / "tests/results/demo_rehearsal"
        previous.mkdir()
        (previous / "summary.json").write_text('{"previous": true}\n')
        self.shell("record_launcher_end", success=False)
        self.shell("collect_results; [[ $results_ok == 0 ]]")
        self.assertEqual((previous / "summary.json").read_text(), '{"previous": true}\n')

    def test_failed_attachment_refuses_publication_and_keeps_staged_evidence(self):
        # Corrupt the sidecar after saving it, at the rsync boundary, so
        # attachment itself fails after validate-results has succeeded.
        self.shell("""
record_launcher_start
rsync() {
    cp -R """ + shlex.quote(str(self.remote)) + """/. "${*: -1}"
    echo broken > "$PROVENANCE_FILE"
}
collect_results
[[ $results_ok == 0 ]]
""")
        self.assertFalse((self.repo / "tests/results/demo_rehearsal").exists())
        self.assertTrue((self.repo / "tests/results/.demo_rehearsal_staging/summary.json").exists())

    def test_previous_result_refusal_keeps_its_status_before_provenance(self):
        previous = self.repo / "tests/results/demo_rehearsal"
        previous.mkdir()
        (previous / "run.id").write_text("99\n")
        (previous / "summary.json").write_text('{"previous": true}\n')
        result = self.shell("""
record_launcher_end() { touch provenance-contacted; return 0; }
collect_results
rc=$?
[[ $rc == 1 && $results_ok == 0 && $results_collection_refused == 1 ]]
""")
        self.assertIn("run.id=99", result.stderr)
        self.assertFalse((self.repo / "provenance-contacted").exists())
        self.assertEqual((previous / "summary.json").read_text(), '{"previous": true}\n')

    def test_foreign_sidecar_refusal_keeps_its_status_without_previous_results(self):
        self.shell("record_launcher_start")
        record = json.loads(self.sidecar.read_text())
        record["run_id"] = "99"
        self.sidecar.write_text(json.dumps(record))
        self.shell("""
collect_results
rc=$?
[[ $rc == 1 && $results_ok == 0 && $results_collection_refused == 1 ]]
""")
        self.assertFalse((self.repo / "tests/results/demo_rehearsal").exists())
        self.assertFalse((self.repo / "tests/results/.demo_rehearsal_staging").exists())


if __name__ == "__main__":
    unittest.main()
