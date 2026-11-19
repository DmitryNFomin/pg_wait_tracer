#!/usr/bin/env python3
"""Host-only #959 signatures, remaining checks, arm ordering and caught mutants."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import types
import unittest
from unittest.mock import patch

import test_init_hint_recovery as recovery


SOURCE = Path(recovery.__file__).read_text()
K1 = ("EXIT-BEFORE-ATTACH: pid=11 opportunity=with entry_present=1 bootstrap_armed=1 "
      "post_arm_checked=1 post_arm_ptr=0x6195a6b4cb40 addr_shared=0 resolve_errno=0 "
      "shared_errno=0 counter_deltas_valid=1 lifecycle_init_no_state_delta=0 wait_read_fail_delta=0")
K2_INIT = ("INIT-LOST: clock=CLOCK_MONOTONIC pid=12 init_ts=2238395279097 "
           "exit_ts=2238405710043 last_sweep_start_ns=2238405832876 "
           "batch_start_ns=2238405832326 batch_records=1 since_epoll_return_ns=23549 "
           "no_opportunity=0 exit_ts_source=record")
K2_WP = ("WP-ATTACH-FAIL: pid=12 site=watchpoint_open errno=3 resolve_errno=0 "
         "liveness=exiting liveness_result=1 liveness_errno=0 exit_proof=2")
K3_EXIT = ("EXIT-BEFORE-ATTACH: pid=13 opportunity=unknown entry_present=1 bootstrap_armed=0 "
           "post_arm_checked=0 post_arm_ptr=0x0 addr_shared=0 resolve_errno=0 shared_errno=0 "
           "counter_deltas_valid=1 lifecycle_init_no_state_delta=0 wait_read_fail_delta=0")
K3_WP = ("WP-ATTACH-FAIL: pid=13 site=bootstrap_watchpoint_open/backend_wait_addr_resolve "
         "errno=3 resolve_errno=3 liveness=exiting liveness_result=1 liveness_errno=0 exit_proof=2")
HEALTHY = "INFO: PID 99 initialized (client)"


def fixture(k1=False, k2=False, k3=False):
    text = [HEALTHY]
    pids = {99}
    deltas = dict.fromkeys((
        "lifecycle_exit_before_attach_total",
        "lifecycle_exit_before_attach_with_opportunity_total",
        "lifecycle_exit_before_attach_no_opportunity_total",
        "lifecycle_exit_before_attach_unknown_total",
        "lifecycle_init_lost_total", "lifecycle_init_lost_with_opportunity_total",
        "lifecycle_init_lost_no_opportunity_total", "lifecycle_init_lost_untraced_ns_total",
        "wp_attach_failures_total", "wp_attach_exit_race_total",
        "wp_attach_failures_live_total", "wp_attach_failures_unknown_total",
        "wp_attach_failures_gone_total", "ringbuf_lifecycle_fork_drops_total",
        "ringbuf_lifecycle_exit_drops_total", "ringbuf_drops_total",
        "state_map_full_total", "init_state_read_fail_total"), 0)
    if k1:
        text.append(K1)
        pids.add(11)
        deltas["lifecycle_exit_before_attach_total"] = 1
        deltas["lifecycle_exit_before_attach_with_opportunity_total"] = 1
    if k2:
        text.extend((K2_WP, K2_INIT))
        pids.add(12)
        deltas["lifecycle_init_lost_total"] = 1
        deltas["lifecycle_init_lost_with_opportunity_total"] = 1
        deltas["wp_attach_exit_race_total"] = 1
    if k3:
        text.extend((K3_EXIT, K3_WP))
        pids.add(13)
        deltas["lifecycle_exit_before_attach_total"] += 1
        deltas["lifecycle_exit_before_attach_unknown_total"] = 1
        deltas["wp_attach_exit_race_total"] += 1
    return "\n".join(text), deltas, pids, set(pids)


class KnownClassCases(unittest.TestCase):
    module = recovery

    def classify(self, data):
        try:
            result = self.module.classify_churn(*data)
            return (93 if any(result.values()) else 0), result
        except AssertionError:
            return 1, None

    def test_clean(self):
        self.assertEqual(self.classify(fixture())[0], 0)

    def test_k1_only(self):
        self.assertEqual(self.classify(fixture(k1=True)), (93, {"K1": {11}, "K2": set(), "K3": set()}))

    def test_k2_only(self):
        self.assertEqual(self.classify(fixture(k2=True)), (93, {"K1": set(), "K2": {12}, "K3": set()}))

    def test_both(self):
        self.assertEqual(self.classify(fixture(True, True)), (93, {"K1": {11}, "K2": {12}, "K3": set()}))

    def test_unknown_only(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text.replace("addr_shared=0", "addr_shared=1"),
                                        deltas, pids, short))[0], 1)

    def test_mixed_known_unknown(self):
        text, deltas, pids, short = fixture(True, True)
        self.assertEqual(self.classify((text.replace("liveness=exiting", "liveness=live"),
                                        deltas, pids, short))[0], 1)

    def test_k3_cohort(self):
        self.assertEqual(self.classify(fixture(k3=True)),
                         (93, {"K1": set(), "K2": set(), "K3": {13}}))
        self.assertEqual(self.fake_arm(fixture(k3=True), churn=True)["K3"], {13})

    def test_k3_non_cohort(self):
        text, deltas, pids, short = fixture(k3=True)
        data = text, deltas, pids - {13}, short - {13}
        self.assertEqual(self.classify(data)[0], 93)
        self.assertEqual(self.fake_arm(data)["K3"], {13})

    def test_k2_gone(self):
        text, deltas, pids, short = fixture(k2=True)
        text = text.replace("liveness=exiting liveness_result=1", "liveness=gone liveness_result=0")
        text = text.replace("exit_proof=2", "exit_proof=1")
        self.assertEqual(self.classify((text, deltas, pids, short))[0], 93)

    def test_k3_gone(self):
        text, deltas, pids, short = fixture(k3=True)
        text = text.replace("liveness=exiting liveness_result=1", "liveness=gone liveness_result=0")
        text = text.replace("exit_proof=2", "exit_proof=1").replace("errno=3 resolve_errno=3", "errno=3 resolve_errno=2")
        self.assertEqual(self.classify((text, deltas, pids, short))[0], 93)

    def test_non_cohort_k1_k2_long_lived(self):
        text, deltas, _, _ = fixture(True, True)
        data = text, deltas, {99}, set()
        self.assertEqual(self.classify(data)[0], 93)
        self.assertEqual(self.fake_arm(data), {"K1": {11}, "K2": {12}, "K3": set()})

    def test_k3_live_attach_stays_red(self):
        text, deltas, pids, short = fixture(k3=True)
        # Leave counters unchanged to ensure the signature check itself fails.
        text = text.replace("liveness=exiting", "liveness=live").replace("exit_proof=2", "exit_proof=0")
        with self.assertRaisesRegex(AssertionError, "K3 attach failure lacks exact exit proof"):
            self.module.classify_churn(text, deltas, pids, short)
        self.assertEqual(self.classify((text, deltas, pids, short))[0], 1)

    def test_k3_exact_exit_proof(self):
        text, deltas, pids, short = fixture(k3=True)
        # A positive proof code with inconsistent liveness is not the signature.
        text = text.replace("liveness_result=1", "liveness_result=0")
        self.assertEqual(self.classify((text, deltas, pids, short))[0], 1)

    def test_k3_diagnostic_multiplicity(self):
        text, deltas, pids, short = fixture(k3=True)
        for changed in (text.replace(K3_WP, ""), text + "\n" + K3_WP,
                        text.replace("pid=13 site=", "pid=14 site="), text + "\n" + K3_EXIT):
            with self.subTest(text=changed):
                self.assertEqual(self.classify((changed, deltas, pids, short))[0], 1)
                self.assertTrue(self.rejection_lines((changed, deltas, pids, short), raises=True))

    def test_k3_exact_fields(self):
        text, deltas, pids, short = fixture(k3=True)
        for old, new in (("post_arm_ptr=0x0", "post_arm_ptr=0x1"),
                         ("bootstrap_armed=0", "bootstrap_armed=1"),
                         ("counter_deltas_valid=1", "counter_deltas_valid=0"),
                         ("errno=3 resolve_errno=3", "errno=3 resolve_errno=0"),
                         ("liveness_errno=0", "liveness_errno=13")):
            with self.subTest(field=old):
                self.assertEqual(self.classify((text.replace(old, new), deltas, pids, short))[0], 1)

    def test_k3_counter_mismatch(self):
        text, deltas, pids, short = fixture(k3=True)
        for key in ("lifecycle_exit_before_attach_unknown_total", "lifecycle_exit_before_attach_total",
                    "wp_attach_exit_race_total"):
            with self.subTest(counter=key):
                bad = {**deltas, key: 2}
                self.assertEqual(self.classify((text, bad, pids, short))[0], 1)
                lines = self.rejection_lines((text, bad, pids, short), raises=True)
                self.assertEqual(len(lines), 1)
                self.assertIn(key, lines[0])

    def test_non_cohort_overlap(self):
        text, deltas, _, _ = fixture(k3=True)
        self.assertEqual(self.classify((text + "\nINFO: PID 13 initialized (worker)",
                                        deltas, {99}, set()))[0], 1)

    def test_overlapping_classes(self):
        text, deltas, pids, short = fixture(k1=True, k3=True)
        text = text.replace("pid=13 ", "pid=11 ")
        with self.assertRaisesRegex(AssertionError, "overlapping known-class outcomes"):
            self.module.classify_churn(text, deltas, pids, short)

    def test_all_classes_share_cap(self):
        self.assertEqual(self.classify(fixture(True, True, True))[0], 1)

    def test_over_cap(self):
        text, deltas, _, _ = fixture(k1=True)
        text = HEALTHY + "\n" + "\n".join(K1.replace("pid=11 ", f"pid={pid} ")
                                           for pid in (11, 13, 14))
        deltas["lifecycle_exit_before_attach_total"] = 3
        deltas["lifecycle_exit_before_attach_with_opportunity_total"] = 3
        pids = {99, 11, 13, 14}
        self.assertEqual(self.classify((text, deltas, pids, pids))[0], 1)

    def test_cap_boundary(self):
        self.assertEqual(self.classify(fixture(True, True))[0], 93)

    def test_combined_over_cap(self):
        text, deltas, pids, short = fixture(True, True)
        text += "\n" + K1.replace("pid=11 ", "pid=13 ")
        deltas["lifecycle_exit_before_attach_total"] += 1
        deltas["lifecycle_exit_before_attach_with_opportunity_total"] += 1
        self.assertEqual(self.classify((text, deltas, pids | {13}, short | {13}))[0], 1)

    def test_missing_diagnostic(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text.replace(K1, ""), deltas, pids, short))[0], 1)

    def test_partial_diagnostic(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text.replace("post_arm_checked=1 ", ""),
                                        deltas, pids, short))[0], 1)

    def test_missing_same_pid_attach_proof(self):
        text, deltas, pids, short = fixture(k2=True)
        self.assertEqual(self.classify((text.replace("pid=12 site=", "pid=13 site="),
                                        deltas, pids, short))[0], 1)

    def test_unstored_init(self):
        text, deltas, pids, short = fixture(k2=True)
        self.assertEqual(self.classify((text.replace("init_ts=2238395279097", "init_ts=0"),
                                        deltas, pids, short))[0], 1)

    def test_bad_counter_delta(self):
        text, deltas, pids, short = fixture(k1=True)
        deltas["lifecycle_exit_before_attach_with_opportunity_total"] = 2
        self.assertEqual(self.classify((text, deltas, pids, short))[0], 1)

    def test_duplicates(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text + "\n" + K1, deltas, pids, short))[0], 1)

    def test_overlap(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text + "\nINFO: PID 11 initialized (client)",
                                        deltas, pids, short))[0], 1)

    def test_event_ring_drop_matches_master(self):
        # Master bc554a73 gates lifecycle drops, not event-ring NMI drops.
        text, deltas, pids, short = fixture()
        deltas["ringbuf_drops_total"] = 1
        data = text, deltas, pids, short
        self.assertEqual(self.classify(data)[0], 0)
        outcome = self.fake_arm(data, churn=True)
        self.assertEqual(outcome, {"K1": set(), "K2": set(), "K3": set()})
        with patch.object(self.module, "run_arm", side_effect=[outcome, outcome, None]), \
             redirect_stdout(io.StringIO()):
            self.assertEqual(self.module.run_arms(1), 0)

    def test_lifecycle_drops_stay_red(self):
        for counter in ("ringbuf_lifecycle_fork_drops_total", "ringbuf_lifecycle_exit_drops_total"):
            for known in (False, True):
                with self.subTest(counter=counter, known_k1=known):
                    text, deltas, pids, short = fixture(k1=known)
                    deltas[counter] = 1
                    data = text, deltas, pids, short
                    self.assertEqual(self.classify(data)[0], 1)
                    with self.assertRaises(AssertionError) as raised:
                        self.fake_arm(data, churn=True)
                    self.assertEqual(raised.exception.args, ((counter, 1),))

    def test_baseline_exit_race_matches_master(self):
        # A pre-cohort race remains in the full log and absolute counters.
        # Master's check_cohort reconciles total diagnostics with after;
        # the exception classifier's delta-only reconciliation rejects it.
        text, deltas, pids, short = fixture()
        text += "\n" + K2_WP.replace("pid=12 ", "pid=42 ")
        data = text, deltas, pids, short
        self.assertEqual(self.classify(data)[0], 1)
        self.assertEqual(self.fake_arm(data, churn=True, baseline={"wp_attach_exit_race_total": 1}),
                         {"K1": set(), "K2": set(), "K3": set()})

    def test_remaining_pid_still_checked(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text.replace(HEALTHY, ""), deltas, pids, short))[0], 1)

    def test_remaining_duplicate_still_checked(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text + "\n" + HEALTHY, deltas, pids, short))[0], 1)

    def test_not_short_lived(self):
        text, deltas, pids, short = fixture(k1=True)
        self.assertEqual(self.classify((text, deltas, pids, short - {11}))[0], 1)

    def fake_arm(self, data, skip_store=False, churn=False, baseline=None, output=None):
        """Run the real live-arm assertions with only the external I/O replaced."""
        module = self.module
        text, deltas, pids, short = data
        text += ("\nPGWT_TEST_DROP_INIT_HINT\n" +
                 "\n".join(f"INFO: fork detected PID {pid}, bootstrap watchpoint attached"
                            for pid in pids))
        before = dict.fromkeys(deltas, 0)
        before.update(baseline or {})
        before["lifecycle_init_hint_drops_total"] = 0
        after = {key: before[key] + value for key, value in deltas.items()}
        after["lifecycle_init_hint_drops_total"] = 1

        def process(*args, **kwargs):
            kwargs["stderr"].write(text)
            kwargs["stderr"].flush()
            return object()

        with patch.object(module.subprocess, "Popen", side_effect=process), \
             patch.object(module, "wait_for"), patch.object(module, "stop"), \
             patch.object(module, "ctl", side_effect=[before, after]), \
             patch.object(module, "cohort", return_value=pids), \
             patch.object(module, "short_cohort", return_value=(pids, short)), \
             redirect_stdout(io.StringIO() if output is None else output):
            return module.run_arm(1, skip_store=skip_store, churn=churn)

    def rejection_lines(self, data, raises, baseline=None):
        output = io.StringIO()
        if raises:
            with self.assertRaises(AssertionError):
                self.fake_arm(data, churn=True, baseline=baseline, output=output)
        else:
            self.fake_arm(data, churn=True, baseline=baseline, output=output)
        return [line for line in output.getvalue().splitlines()
                if line.startswith("known-class classifier rejected:")]

    def test_rejection_reason_printed(self):
        # A rejected classification names its reason and PIDs, then falls
        # through to master's assertions unchanged.
        text, deltas, pids, short = fixture(k1=True)
        rejected = (text.replace("addr_shared=0", "addr_shared=1"), deltas, pids, short)
        self.assertEqual(self.rejection_lines(rejected, raises=True), [
            "known-class classifier rejected: "
            "missing, partial or duplicate known-class diagnostic pids=[11]"])
        text, deltas, pids, short = fixture()
        text += "\n" + K2_WP.replace("pid=12 ", "pid=42 ")
        self.assertEqual(self.rejection_lines((text, deltas, pids, short), raises=False,
                                              baseline={"wp_attach_exit_race_total": 1}), [
            "known-class classifier rejected: "
            "wp_attach_exit_race_total delta does not match PID diagnostics pids=[]"])

    def test_accepted_classification_prints_no_rejection(self):
        for data in (fixture(), fixture(True, True)):
            self.assertEqual(self.rejection_lines(data, raises=False), [])

    def test_long_lived_loss_stays_red(self):
        for data in (fixture(k1=True), fixture(k2=True), fixture(k3=True)):
            with self.subTest(data=data), self.assertRaises(AssertionError):
                self.fake_arm(data)

    def test_churn_integration(self):
        self.assertEqual(self.fake_arm(fixture(True, True), churn=True),
                         {"K1": {11}, "K2": {12}, "K3": set()})

    def test_unclassified_attach_failure_preserves_master_args(self):
        text, deltas, pids, short = fixture()
        text += ("\nWP-ATTACH-FAIL: pid=42 site=watchpoint_open errno=3 resolve_errno=0 "
                 "liveness=unknown liveness_result=-1 liveness_errno=13")
        deltas["wp_attach_failures_total"] = 1
        deltas["wp_attach_failures_unknown_total"] = 1
        with self.assertRaises(AssertionError) as raised:
            self.fake_arm((text, deltas, pids | {42}, short | {42}), churn=True)
        self.assertIs(type(raised.exception), AssertionError)
        self.assertEqual(raised.exception.args, (("wp_attach_failures_total", 1),))

    def test_no_pending_bootstrap_stays_red(self):
        original = self.module.re.findall
        with patch.object(self.module.re, "findall", wraps=self.module.re.findall) as find:
            find.side_effect = lambda pattern, *args, **kwargs: (
                [] if pattern.startswith("fork detected PID") else original(pattern, *args, **kwargs))
            with self.assertRaisesRegex(AssertionError, "no cohort backend entered pending bootstrap"):
                self.fake_arm(fixture(k1=True), churn=True)

    def run_arms(self, fail_control=False, known=True):
        calls = []

        def arm(pid, skip_store, churn=False):
            calls.append((skip_store, churn))
            if skip_store and fail_control:
                raise AssertionError("skipped-store control failed")
            return {"K1": {11} if churn and known else set(), "K2": set(), "K3": set()}

        output = io.StringIO()
        with patch.object(self.module, "run_arm", side_effect=arm), redirect_stdout(output):
            try:
                code = self.module.run_arms(1)
            except AssertionError:
                code = 1
        return code, calls, output.getvalue()

    def test_third_arm_failure_stays_red(self):
        code, calls, output = self.run_arms(fail_control=True)
        self.assertEqual(code, 1, "third-arm failure must override known churn")
        self.assertEqual(calls, [(False, False), (False, True), (True, False)])
        self.assertNotIn("KNOWN-CLASS", output)

    def test_known_exit_after_control(self):
        code, calls, output = self.run_arms()
        self.assertEqual(code, 93)
        self.assertEqual(calls, [(False, False), (False, True), (True, False)])
        self.assertEqual(output.strip(),
                         "KNOWN-CLASS #959 arm=churn K1=[11] K2=[] K3=[] count=1 cap=2")

    def test_shared_cap_across_arms(self):
        long_known = self.fake_arm((fixture(k1=True)[0], fixture(k1=True)[1], {99}, set()))
        churn_known = self.fake_arm(fixture(k2=True, k3=True), churn=True)
        with patch.object(self.module, "run_arm", side_effect=[long_known, churn_known, None]), \
             redirect_stdout(io.StringIO()), \
             self.assertRaisesRegex(AssertionError, "known-class shared run cap exceeded"):
            self.module.run_arms(1)

    def test_shared_cap_boundary_and_pid_reuse(self):
        # Each arm has its own daemon: the same numeric PID is two losses.
        known = {"K1": {11}, "K2": set(), "K3": set()}
        output = io.StringIO()
        with patch.object(self.module, "run_arm", side_effect=[known, known, None]), redirect_stdout(output):
            self.assertEqual(self.module.run_arms(1), 93)
        self.assertEqual(output.getvalue().count("count=2 cap=2"), 2)

    def test_long_cohort_no_opportunity_stays_red(self):
        text, deltas, pids, short = fixture()
        text += "\n" + K2_INIT.replace("pid=12 ", "pid=42 ").replace("no_opportunity=0", "no_opportunity=1")
        deltas["lifecycle_init_lost_total"] = 1
        deltas["lifecycle_init_lost_no_opportunity_total"] = 1
        # Include a valid non-cohort exception: it cannot excuse a missed cohort PID.
        text += "\n" + K1
        deltas["lifecycle_exit_before_attach_total"] = 1
        deltas["lifecycle_exit_before_attach_with_opportunity_total"] = 1
        with self.assertRaisesRegex(AssertionError, "long-lived cohort PID did not attach"):
            self.fake_arm((text, deltas, pids | {42}, short))

    def test_clean_arms_exit_zero(self):
        code, calls, output = self.run_arms(known=False)
        self.assertEqual(code, 0)
        self.assertEqual(calls, [(False, False), (False, True), (True, False)])
        self.assertIn("test_init_hint_recovery: PASS", output)


def mutants():
    definitions = (
        ("classifier always known", "    records = {prefix: {} for prefix in",
         '    return {"K1": {11}, "K2": set(), "K3": set()}\n    records = {prefix: {} for prefix in',
         "test_unknown_only"),
        ("cap ignored", "if len(known) > KNOWN_CLASS_CAP:", "if False:", "test_over_cap"),
        ("diagnostic requirement dropped", "if not valid:", "if False:", "test_partial_diagnostic"),
        ("K3 exit proof dropped", '            if not exit_proved(wp[0]):',
         '            if False:', "test_k3_exact_exit_proof"),
        ("K3 duplicate WP accepted", 'len(retired) != 1 or len(wp) != 1 or init:',
         'len(retired) != 1 or not wp or init:', "test_k3_diagnostic_multiplicity"),
        ("non-cohort acceptance removed", 'if known & pids - short_lived:',
         'if not known <= pids & short_lived:', "test_non_cohort_k1_k2_long_lived"),
        ("K2 gone proof removed", '(("exiting", "1", "2"), ("gone", "0", "1"))',
         '(("exiting", "1", "2"),)', "test_k2_gone"),
        ("long-lived classification disabled", '                if not skip_store:\n',
         '                if churn and not skip_store:\n', "test_non_cohort_k1_k2_long_lived"),
        ("cap only per arm", '    if count > KNOWN_CLASS_CAP:',
         '    if False:', "test_shared_cap_across_arms"),
        ("long-lived cohort accepted", 'if known & pids - short_lived:',
         'if False:', "test_not_short_lived"),
        ("long-lived attachment requirement dropped",
         'if not churn and not skip_store and pids - attached:',
         'if False:', "test_long_cohort_no_opportunity_stays_red"),
        ("third arm skipped", "    run_arm(pm_pid, skip_store=True)\n", "",
         "test_third_arm_failure_stays_red"),
        ("unclassified attach args changed", "error = AssertionError((key, after[key]))",
         "error = AssertionError(key, after[key])",
         "test_unclassified_attach_failure_preserves_master_args"),
        ("generic ringbuf drop gate restored", "    for key in (*ATTACH_COUNTERS[:1],",
         '    for key, value in deltas.items():\n'
         '        if key.startswith("ringbuf_") and key.endswith("drops_total") and value != 0:\n'
         '            raise CohortFailure(f"{key} must be zero", set())\n'
         '    for key in (*ATTACH_COUNTERS[:1],',
         "test_event_ring_drop_matches_master"),
        ("rejected classifier overrides master",
         "                              f\"pids={sorted(getattr(rejection, 'pids', ()))}\")\n",
         "                              f\"pids={sorted(getattr(rejection, 'pids', ()))}\")\n"
         "                        raise\n", "test_baseline_exit_race_matches_master"),
        ("rejection reason not printed", "                        print(f\"known-class classifier rejected: \"",
         "                        (f\"known-class classifier rejected: \"", "test_rejection_reason_printed"),
    )
    for name, old, new, case in definitions:
        assert SOURCE.count(old) == 1, name
        module = types.ModuleType("known_class_mutant")
        module.__file__ = recovery.__file__
        exec(compile(SOURCE.replace(old, new, 1), module.__file__, "exec"), module.__dict__)
        test = KnownClassCases(case)
        test.module = module
        result = unittest.TestResult()
        test.run(result)
        assert result.testsRun == 1 and len(result.failures) == 1 and not result.errors, (
            name, result.failures, result.errors)
        print(f"MUTANT CAUGHT: {name} -> {case}")
    print(f"Mutants: {len(definitions)}/{len(definitions)} caught at intended assertions")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(KnownClassCases)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    mutants()
