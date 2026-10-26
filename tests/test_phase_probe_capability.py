#!/usr/bin/env python3
"""Offline #622 contracts. All Linux evidence here is simulated on macOS.

Exercise the shared resolver, real Python entrypoints and the actual shell
assertion blocks. No root, PG, BPF, build, lock, or remote machine is used.
"""
from contextlib import ExitStack, nullcontext, redirect_stderr, redirect_stdout
from dataclasses import asdict
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import phase_probe_capability as cap

HERE = Path(__file__).resolve().parent
ESC = (HERE / "test_escalation.sh").read_text()
RUNNER = (HERE / "run_all.sh").read_text()


def between(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def notes(names=cap.PHASE_NOTES, provider="postgresql"):
    return "\n".join(f"  Provider: {provider}\n  Name: {name}\n"
                     "  Location: 0x1234, Base: 0x0, Semaphore: 0xabcd\n"
                     "  Arguments: 8@%rax\n" for name in sorted(names))


def shell(text, env=None):
    return subprocess.run(["bash"], input=text, text=True,
                          capture_output=True, env=env, timeout=30)


def available_evidence():
    """Derive availability from both OS terms, without an env override."""
    stack = ExitStack()
    stack.enter_context(patch.dict(os.environ, {}, clear=True))
    stack.enter_context(patch.object(cap, "REF_CTR_OFFSET"))
    stack.enter_context(patch.object(cap.os, "readlink", return_value="/pg18/bin/postgres"))
    stack.enter_context(patch.object(cap.subprocess, "run", return_value=Mock(
        returncode=0, stdout=notes(), stderr="")))
    return stack


CHECK = between(ESC, "check() {", "# One pgwt-server data command")
JGET = "jget() { python3 -c \"import json,sys; print(json.load(sys.stdin)$1)\"; }\n"


class Resolver(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.dict(os.environ, {}, clear=True))
        self.attribute = self.stack.enter_context(patch.object(cap, "REF_CTR_OFFSET"))
        self.attribute.__str__.return_value = "/sys/bus/event_source/devices/uprobe/format/ref_ctr_offset"
        self.readlink = self.stack.enter_context(patch.object(cap.os, "readlink", return_value="/pg18/bin/postgres"))
        self.readelf = self.stack.enter_context(patch.object(cap.subprocess, "run", return_value=Mock(returncode=0, stdout=notes(), stderr="")))

    def test_absent_kernel_attribute_refuses_with_evidence(self):
        self.attribute.stat.side_effect = FileNotFoundError
        capability = cap.resolve_phase_probes(123)
        self.assertFalse(capability.expected)
        self.assertEqual(capability.phase_mask, 0)
        self.assertIn("not available on this kernel", capability.reason)
        self.assertIn("/sys/bus/event_source/devices/uprobe/format/ref_ctr_offset is absent", capability.reason)
        self.readlink.assert_not_called()
        self.readelf.assert_not_called()
        with self.assertRaises(SystemExit) as exc, redirect_stdout(io.StringIO()):
            capability.require("test")
        self.assertEqual(exc.exception.code, 78)

    def test_capable_kernel_and_target_notes_expect_full_bundle(self):
        capability = cap.resolve_phase_probes(123)
        self.assertTrue(capability.expected)
        self.assertEqual(capability.phase_mask, 0xC)
        self.readlink.assert_called_once_with("/proc/123/exe")
        self.assertEqual(self.readelf.call_args.args[0], ["readelf", "--notes", "/pg18/bin/postgres"])
        self.assertEqual(self.readelf.call_args.kwargs["env"]["LC_ALL"], "C")

    def test_no_notes_is_binary_unavailability_not_kernel_unavailability(self):
        for output in ("", notes(provider="unrelated")):
            with self.subTest(output=output):
                self.readelf.return_value.stdout = output
                capability = cap.resolve_phase_probes(123)
                self.assertFalse(capability.expected)
                self.assertIn("PostgreSQL build", capability.reason)
                self.assertIn("is present", capability.reason)
                self.assertIn("/pg18/bin/postgres", capability.reason)
                self.assertNotIn("not available on this kernel", capability.reason)

    def test_partial_bundle_resolves_each_pair_independently(self):
        # EXEC and PLAN attach independently at runtime (src/exact_probe.c);
        # a binary carrying only one pair's notes is a real, nameable partial
        # capability, not an error to raise past (#622 rework: a combined
        # all-or-nothing bundle check cannot express this).
        self.readelf.return_value.stdout = notes({"query__plan__start", "query__plan__done"})
        capability = cap.resolve_phase_probes(123)
        self.assertFalse(capability.exec_expected)
        self.assertTrue(capability.plan_expected)
        self.assertFalse(capability.expected)  # the combined gate needs both
        self.assertEqual(capability.phase_mask, cap.PLAN_BIT)
        self.assertIn("plan", capability.reason)
        self.assertIn("not execute", capability.reason)

        self.readelf.return_value.stdout = notes({"query__execute__start", "query__execute__done"})
        capability = cap.resolve_phase_probes(123)
        self.assertTrue(capability.exec_expected)
        self.assertFalse(capability.plan_expected)
        self.assertFalse(capability.expected)
        self.assertEqual(capability.phase_mask, cap.EXEC_BIT)
        self.assertIn("execute", capability.reason)
        self.assertIn("not plan", capability.reason)

    def test_half_a_pair_note_missing_is_not_capable_for_that_probe(self):
        # pgwt_resolve_exec_sites/attach_usdt_pair need BOTH start and done;
        # one note of a pair present is the same as neither for that probe.
        self.readelf.return_value.stdout = notes({"query__execute__start",
                                                  "query__plan__start", "query__plan__done"})
        capability = cap.resolve_phase_probes(123)
        self.assertFalse(capability.exec_expected)
        self.assertTrue(capability.plan_expected)

    def test_unreadable_evidence_is_failure(self):
        for target in (self.attribute.stat, self.readlink, self.readelf):
            with self.subTest(target=target):
                target.side_effect = PermissionError("unreadable")
                with self.assertRaises(PermissionError):
                    cap.resolve_phase_probes(123)
                target.side_effect = None
        self.readelf.return_value.returncode = 1
        self.readelf.return_value.stderr = "bad ELF"
        with self.assertRaisesRegex(ValueError, "cannot inspect SDT.*bad ELF"):
            cap.resolve_phase_probes(123)

    def test_override_is_explicit_and_strict(self):
        for raw, expected in (("0", False), ("1", True), (" 1 \n", True)):
            with patch.dict(os.environ, {cap.EXPECT_PHASE_ENV: raw}):
                capability = cap.resolve_phase_probes(123)
                self.assertEqual(capability.expected, expected)
                self.assertIn("explicit override", capability.reason)
        self.attribute.stat.assert_not_called()
        self.readelf.assert_not_called()
        for raw in ("", " ", "yes", "2"):
            with patch.dict(os.environ, {cap.EXPECT_PHASE_ENV: raw}), self.assertRaises(ValueError):
                cap.resolve_phase_probes(123)


class Wiring(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.env = dict(os.environ)
        self.env.pop(cap.EXPECT_PHASE_ENV, None)
        self.env["PYTHONPATH"] = os.pathsep.join((self.temp.name, str(HERE)))
        # Mock only the OS evidence. The real resolver and entrypoints run.
        (Path(self.temp.name) / "sitecustomize.py").write_text('''
import phase_probe_capability as cap
class MissingKernelAttribute:
    def stat(self):
        raise FileNotFoundError("simulated EL8 sysfs")
    def __str__(self):
        return "/sys/bus/event_source/devices/uprobe/format/ref_ctr_offset"
cap.REF_CTR_OFFSET = MissingKernelAttribute()
''')

    def test_real_python_entrypoints_refuse_before_live_work(self):
        for name in ("test_execution_cardinality.py", "test_execution_span_live.py"):
            with self.subTest(name=name):
                result = subprocess.run([sys.executable, str(HERE / name), "--pid", "123"], env=self.env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
                self.assertIn("REFUSED (phase capability)", result.stdout)
                self.assertIn("ref_ctr_offset is absent", result.stdout)

    def ui_walk(self, tabs=None, empty=False, bad_tab=None):
        # Real --url main loop, run_tab, navigation, screenshots and verdicts;
        # only the browser protocol/clock are simulated. No phase env override.
        import ui_live_smoke as smoke
        from test_ui_live_smoke_footage import Browser, Page
        visited = []

        class WalkBrowser(Browser):
            def new_context(self, **kwargs):
                self.tab = Path(kwargs["record_video_dir"]).name
                visited.append(self.tab)
                return super().new_context(**kwargs)

            def close(self):
                if hasattr(self, "page"):
                    super().close()

        browser = WalkBrowser(None)
        driver = SimpleNamespace(chromium=SimpleNamespace(launch=lambda: browser))
        out_dir = Path(self.temp.name) / "ui"
        argv = ["ui_live_smoke.py", "--url", "http://fixture/", "--pid", "123",
                "--out-dir", str(out_dir), "--device-scale-factor", "1",
                "--first-data-timeout", "0.01"]
        if tabs:
            argv += ["--tabs", ",".join(tabs)]
        evaluate = Page.evaluate

        def panel(page, js, arg=None):
            if js in smoke.PANEL_CHECKS.values() and (empty or page.tab == bad_tab):
                return {"ok": False, "detail": "empty data"}
            return evaluate(page, js, arg)

        with patch.object(sys, "argv", argv), \
                patch.object(smoke, "sync_playwright", lambda: nullcontext(driver)), \
                patch.object(smoke.time, "monotonic", lambda: browser.page.now / 1000), \
                patch.object(Page, "evaluate", panel), \
                redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()):
            rc = smoke.main()
        return rc, json.loads((out_dir / "summary.json").read_text()), out.getvalue(), visited

    def test_ui_invokes_shared_resolver_and_refuses(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(cap, "REF_CTR_OFFSET") as attribute:
            attribute.stat.side_effect = FileNotFoundError
            attribute.__str__.return_value = "/sys/bus/event_source/devices/uprobe/format/ref_ctr_offset"
            rc, summary, output, visited = self.ui_walk()
        self.assertEqual(rc, 78, output)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["refused_tabs"], ["waterfall", "scatter"])
        self.assertEqual(summary["failed_tabs"], [])
        import ui_live_smoke as smoke
        self.assertEqual(visited, [t for t in smoke.lib.TABS if t not in ("waterfall", "scatter")])
        self.assertEqual(len(visited), 9)
        for tab in visited:
            self.assertTrue(summary["tabs"][tab]["ok"], tab)
        self.assertIn("test_ui_live_smoke [waterfall]", output)
        self.assertIn("test_ui_live_smoke [scatter]", output)
        self.assertIn("ref_ctr_offset is absent", output)

    def test_ui_refusal_cannot_hide_other_tab_failure(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(cap, "REF_CTR_OFFSET") as attribute:
            attribute.stat.side_effect = FileNotFoundError
            rc, summary, output, visited = self.ui_walk(bad_tab="sessions")
        self.assertEqual(rc, 1, output)
        self.assertEqual(summary["failed_tabs"], ["sessions"])
        self.assertEqual(summary["refused_tabs"], ["waterfall", "scatter"])
        self.assertIn("matrix", visited)

    def test_ui_all_refusal_cannot_pass(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(cap, "REF_CTR_OFFSET") as attribute:
            attribute.stat.side_effect = FileNotFoundError
            rc, summary, output, visited = self.ui_walk(tabs=["waterfall", "scatter"])
        self.assertEqual(rc, 78, output)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["failed_tabs"], [])
        self.assertEqual(summary["refused_tabs"], ["waterfall", "scatter"])
        self.assertEqual(visited, [])

    def test_ui_self_test_failure_takes_precedence_over_refusal(self):
        ui = (HERE / "ui_live_smoke.sh").read_text()
        tail = ui[ui.index('if [[ "$SELF_TEST_RC" -ne 0 ]]; then'):]
        for smoke_rc, self_test_rc, expected in ((78, 0, 78), (78, 1, 1),
                                                 (1, 0, 1), (1, 78, 1), (0, 0, 0)):
            with self.subTest(smoke_rc=smoke_rc, self_test_rc=self_test_rc):
                result = shell(f'SMOKE_RC={smoke_rc}\nSELF_TEST_RC={self_test_rc}\n' + tail,
                               self.env)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def escalation_data(self, expected, prior_failures=0):
        # Use actual #108 branch + final exit precedence, faking only capture
        # and server inputs. Both captures contain zero phase-marker rows.
        if expected:
            with available_evidence():
                capability = cap.resolve_phase_probes(123)
        else:
            capability = cap.PhaseCapability(False, False, "not available on this kernel: ref_ctr_offset is absent (simulated)")
        prefix = (f'SCRIPT_DIR={shlex.quote(str(HERE))}\n'
                  f'PHASE_CAPABILITY={shlex.quote(json.dumps(asdict(capability)))}\n'
                  f'passed=0\nfailed={prior_failures}\nphase_refused=0\nPSQL_BIN=psql\n'
                  + CHECK + JGET + '''
cleanup() { :; }
esc108_cleanup() { :; }
run_tiered_capture() {
    CAP_DIR=$(mktemp -d)
    CAP_MODE=tiered; CAP_TIER=sampled; CAP_TIER_END=sampled
    if [[ "$2" == escalate ]]; then CAP_WINDOWS=1; else CAP_WINDOWS=0; fi
}
exec_rows() { echo 0; }
''')
        block = ESC[ESC.index('if [[ $(echo "$PHASE_CAPABILITY"'):]
        return shell(prefix + block, self.env)

    def test_escalation_refusal_and_prior_failure_precedence(self):
        result = self.escalation_data(False)
        self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
        self.assertIn("REFUSED (phase capability): test_escalation #108", result.stdout)
        self.assertIn("ref_ctr_offset is absent", result.stdout)
        self.assertEqual(self.escalation_data(False, prior_failures=1).returncode, 1)

    def test_available_but_zero_executions_is_red(self):
        result = self.escalation_data(True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("PASS: #108: escalated capture opened an escalation window", result.stdout)
        self.assertIn("FAIL: #108: a TIERED escalation produces Waterfall executions (got: 0)", result.stdout)
        self.assertIn("FAIL: #108: a TIERED escalation produces Scatter points (got: 0)", result.stdout)
        self.assertNotIn("REFUSED", result.stdout)

    def test_raw_open_window_status_and_exact_masks(self):
        block = between(ESC, '# tier must now be escalated', '# ESC-9:')
        # (expected, mask, tier, want, want_degraded_line): `expected` toggles
        # the WHOLE bundle capable/not (EXEC==PLAN here; the asymmetric split
        # is exercised directly in tests/test_uprobe_mask_gate.py). mask=3
        # against expected=True is #622's measured case (predicted capable,
        # phase bits did not attach) -- accepted, not failed, and reported.
        cases = ((False, 3, "escalated", 0, False),
                (True, 15, "escalated", 0, False),
                (True, 3, "escalated", 0, True),
                (False, 15, "escalated", 1, False),
                (False, 0, "sampled", 1, False))
        for expected, mask, tier, want, want_degraded in cases:
            with self.subTest(expected=expected, mask=mask, tier=tier):
                status = json.dumps(dict(tier=tier, escalation_seconds_remaining=3 if tier == "escalated" else 0, exact_probe_attached_mask=mask))
                capability = json.dumps(asdict(cap.PhaseCapability(expected, expected, "simulated")))
                prefix = f'SCRIPT_DIR={shlex.quote(str(HERE))}\nPHASE_CAPABILITY={shlex.quote(capability)}\npassed=0\nfailed=0\n' + CHECK + JGET
                prefix += f'ctl() {{ echo {shlex.quote(status)}; }}\n'
                result = shell(prefix + block + '\n[[ $failed -eq 0 ]]\n', self.env)
                self.assertEqual(result.returncode, want, result.stdout + result.stderr)
                self.assertIn("open-window status: " + status, result.stdout)
                if want_degraded:
                    self.assertIn("degraded (not asserted)", result.stdout)
                else:
                    self.assertNotIn("degraded (not asserted)", result.stdout)

    def test_runner_refusal_is_separate_and_all_refusal_cannot_pass(self):
        prefix = 'set -uo pipefail\nREQUIRE_LIVE=1\n' + between(RUNNER, 'passed=0', '# Step 0:')
        prefix += f'source {shlex.quote(str(HERE / "run_all_tiers.sh"))}\n'
        summary = RUNNER[RUNNER.index('# Summary\n'):]
        refusal = 'run_test test_execution_span_live python3 "$SCRIPT_DIR/test_execution_span_live.py" --pid 123\n'
        prefix += f'SCRIPT_DIR={shlex.quote(str(HERE))}\n'
        result = shell(prefix + 'run_test real_test true\n' + refusal + summary, self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Executed: 1 (passed 1, failed 0', result.stdout)
        self.assertIn('refused 1', result.stdout)
        self.assertIn('ref_ctr_offset is absent', result.stdout)
        result = shell(prefix + refusal + summary, self.env)
        self.assertEqual(result.returncode, 1)
        self.assertIn('no test was executed', result.stdout)
        result = shell(prefix + 'run_test ordinary_test bash -c "exit 78"\n' + summary, self.env)
        self.assertEqual(result.returncode, 1)
        self.assertIn('failed 1', result.stdout)
        result = shell(prefix + 'skip_live_test missing "no PG"\n' + summary, self.env)
        self.assertEqual(result.returncode, 1)
        self.assertIn('--require-live set, but: no PG', result.stdout)

    def test_available_cardinality_with_missing_fields_still_fails(self):
        import test_execution_cardinality as cardinality
        empty = dict(found={}, executions={name: {"rows": [], "completed_count": 0, "open_count": 0} for name in list(cardinality.COHORTS) + ["nested"]}, top_queries={"rows": []}, error_executions=None, error_row=None)
        with available_evidence(), redirect_stdout(io.StringIO()) as out:
            cap.require_phase_probes("cardinality", 123)
            with patch.object(cardinality, "tests_failed", 0):
                cardinality.assert_fixed_phase(empty)
                self.assertGreater(cardinality.tests_failed, 0)
        self.assertIn("FAIL: fixed/update: exec_count == N (None", out.getvalue())

    def test_available_span_with_zero_markers_still_fails(self):
        import test_execution_span_live as span
        server = Mock()
        server.__enter__ = Mock(return_value=server)
        server.__exit__ = Mock(return_value=False)
        server.query.side_effect = [{"from_ns": 1, "to_ns": 100}, {"rows": []}]
        with available_evidence(), patch.object(span, "ServerHarness", return_value=server), redirect_stdout(io.StringIO()) as out:
            cap.require_phase_probes("span", 123)
            with self.assertRaises(SystemExit) as exc:
                span.assert_spans(self.temp.name, 123, 10, 20, 2, 30)
        self.assertEqual(exc.exception.code, 1)
        self.assertIn("only 0 complete marker-identified executions", out.getvalue())

    def test_available_ui_with_empty_phase_panels_still_fails(self):
        import ui_live_smoke as smoke
        from test_ui_live_smoke_failure import Browser, Page
        for tab in ("waterfall", "scatter"):
            with self.subTest(tab=tab), available_evidence():
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    cap.require_phase_probes("UI", 123)
                    result = smoke.run_tab(Browser(Page()), tab, "http://fixture/",
                                           self.temp.name, 6, 0.01,
                                           smoke.lib.BLINK_THRESHOLD)
                self.assertFalse(result["ok"])
                self.assertIn("panel did not render", result["error"])
                rc, summary, output, visited = self.ui_walk(tabs=[tab], empty=True)
                result = summary["tabs"][tab]
                self.assertEqual(rc, 1, output)
                self.assertEqual(summary["failed_tabs"], [tab])
                self.assertEqual(summary["refused_tabs"], [])
                self.assertEqual(visited, [tab])
                self.assertFalse(result["ok"])
                self.assertIn("empty data", result["error"])
                self.assertNotIn("REFUSED", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
