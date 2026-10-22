#!/usr/bin/env python3
"""Hermetic launcher provenance checks; no SSH, box, build or live workload.

Run: python3 scripts/test-demo-rehearsal-provenance.py
Optional launcher path allows checking the pre-change version for red evidence.
"""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = (Path(sys.argv.pop(1))
            if len(sys.argv) > 1 and not sys.argv[1].startswith("-")
            else ROOT / "scripts/demo-rehearsal.sh")


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


class RemoteDirIsolationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        source = LAUNCHER.read_text()
        helper = source[source.index("compute_remote_dir() {"):].split("\n}\n", 1)[0] + "\n}\n"
        self.helpers = self.base / "remote-dir-helpers.sh"
        self.helpers.write_text(helper)
        self.repo = self.base / "checkout-1"
        self.repo.mkdir()
        self.git(self.repo, "init", "-q")
        (self.repo / "payload").write_text("candidate\n")
        self.git(self.repo, "add", "payload")
        self.git(self.repo, "-c", "user.name=Fixture", "-c",
                 "user.email=fixture@example.invalid", "commit", "-qm", "candidate")
        self.sha = self.git(self.repo, "rev-parse", "HEAD")
        self.assertRegex(self.sha, r"^[0-9a-f]{40}$")
        self.other_repo = self.base / "checkout-2"
        self.git(self.base, "clone", "-q", str(self.repo), str(self.other_repo))
        for repo in (self.repo, self.other_repo):
            self.git(repo, "checkout", "--detach", "-q", self.sha)
            self.assertEqual(self.git(repo, "rev-parse", "--abbrev-ref", "HEAD"), "HEAD")
            self.assertEqual(self.git(repo, "rev-parse", "HEAD"), self.sha)
        self.remote_root = self.base / "remote-root"
        self.remote_root.mkdir()

    def git(self, repo, *args):
        return subprocess.check_output(["git", *args], cwd=repo, text=True).strip()

    def shell(self, repo, commands):
        return subprocess.run(
            ["bash", "-c", "set -euo pipefail\nsource " +
             shlex.quote(str(self.helpers)) + "\n" + commands],
            cwd=repo, env={**os.environ, "demo_rehearsal_start_epoch": "1234567890"},
            text=True, capture_output=True)

    def remote_dir(self, repo, commands="compute_remote_dir"):
        # Each call is a separate launcher process: real $$ and $RANDOM,
        # with the same epoch even for launches in the same second.
        result = self.shell(repo, commands)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        return result.stdout.strip()

    def sync_snapshot(self, remote_dir, run):
        snapshot = self.base / f"snapshot-{run}"
        snapshot.mkdir()
        (snapshot / "payload").write_text(f"snapshot for {run}\n")
        destination = self.remote_root / remote_dir
        destination.mkdir(exist_ok=True)
        # Real destructive rsync against a local stand-in for the SSH target.
        subprocess.run(["rsync", "-az", "--delete", str(snapshot) + "/",
                        str(destination) + "/"], check=True, capture_output=True, text=True)
        return destination

    def retain_trace(self, destination, run):
        sentinel = destination / "tests/results/demo_rehearsal/trace" / f"sentinel-{run}.marker"
        sentinel.parent.mkdir(parents=True, exist_ok=True)
        sentinel.write_text(f"retained trace for {run}\n")
        return sentinel

    def test_persistent_launch_uses_compute_remote_dir(self):
        source = LAUNCHER.read_text()
        launch = source.index('if [[ "$MODE" == "launch" ]]; then')
        persistent = source.index('if [[ "$PERSISTENT" == "1" ]]; then', launch)
        start = source.index('server_id="persistent"', persistent)
        end = source.index('echo "demo-rehearsal: targeting persistent box', start)
        block = source[start:end]
        # Helper-only checks cannot catch a bypass at the real launch call site.
        self.assertIn("compute_remote_dir", block)
        self.assertNotIn("git rev-parse --abbrev-ref HEAD", block)

    def test_separate_invocations_at_same_detached_sha_and_epoch_are_unique(self):
        # Use the SAME checkout as well as the same SHA and epoch here.
        first = self.remote_dir(self.repo)
        second = self.remote_dir(self.repo)
        self.assertNotEqual(first, second)
        for value in (first, second):
            self.assertIn(self.sha, value)
            self.assertNotIn("HEAD", value)
            self.assertNotIn("detached", value)
            self.assertRegex(value, rf"^pgwt-demo-rehearsal-{self.sha}-1234567890-[0-9]+-[0-9]+$")

    def test_missing_candidate_sha_fails_without_printing_a_directory(self):
        outside_repo = self.shell(self.base, "compute_remote_dir")
        self.assertNotEqual(outside_repo.returncode, 0)
        self.assertEqual(outside_repo.stdout, "")
        # Also reject empty success and output accompanied by a failed exit.
        for git_body in ("return 0", "echo HEAD; return 1"):
            with self.subTest(git_body=git_body):
                result = self.shell(self.repo, "git() { " + git_body + "; }\ncompute_remote_dir")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")

    def test_real_rsync_preserves_both_traces_and_state_reattachment(self):
        first = self.remote_dir(self.repo)
        sentinel1 = self.retain_trace(self.sync_snapshot(first, "run1"), "run1")
        second = self.remote_dir(self.other_repo)
        self.assertNotEqual(first, second)
        destination2 = self.sync_snapshot(second, "run2")
        self.assertTrue(sentinel1.exists())
        sentinel2 = self.retain_trace(destination2, "run2")
        self.assertTrue(sentinel2.exists())

        for run, remote_dir, own, other in (
                ("run1", first, sentinel1, sentinel2),
                ("run2", second, sentinel2, sentinel1)):
            with self.subTest(run=run):
                state = self.base / f"{run}.state"
                state.write_text(f"REMOTE_DIR={remote_dir}\n")
                result = self.shell(self.base, "source " + shlex.quote(str(state)) +
                                    '\nprintf \'%s\\n\' "$REMOTE_DIR"')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), remote_dir)
                reattached = self.remote_root / result.stdout.strip()
                own_relative = Path("tests/results/demo_rehearsal/trace") / own.name
                other_relative = own_relative.with_name(other.name)
                self.assertEqual((reattached / own_relative).read_text(), f"retained trace for {run}\n")
                self.assertFalse((reattached / other_relative).exists())

    def test_old_branch_mutant_reproduces_destructive_rsync(self):
        mutant = ('branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null | tr \'/\' \'_\'); '
                  'echo "pgwt-demo-rehearsal-${branch:-detached}"')
        first = self.remote_dir(self.repo, mutant)
        sentinel1 = self.retain_trace(self.sync_snapshot(first, "run1"), "run1")
        self.assertTrue(sentinel1.exists())
        second = self.remote_dir(self.other_repo, mutant)
        self.assertEqual(first, "pgwt-demo-rehearsal-HEAD")
        self.assertEqual(first, second)
        destination2 = self.sync_snapshot(second, "run2")
        self.assertFalse(sentinel1.exists())
        sentinel2 = self.retain_trace(destination2, "run2")
        self.assertTrue(sentinel2.exists())


if __name__ == "__main__":
    unittest.main()
