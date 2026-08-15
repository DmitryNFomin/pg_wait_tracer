#!/usr/bin/env python3
"""Synthetic policy tests for the sampled-overhead gate."""

import unittest
from types import SimpleNamespace

from sampled_overhead_gate import aggregate_liveness_errors, structural_errors


def valid_metrics(*, scans=1, resolved=1, query_fires=0, mask=0):
    return {
        "pgbackend_layout_validation": "validated",
        "pg_pid": 1234,
        "mode": "sampled",
        "tier": "sampled",
        "sampler_healthy": True,
        "sampled_attr_source": "pgbackend_status",
        "samples_total": 1,
        "exact_query_uprobe_fires_total": query_fires,
        "exact_activity_uprobe_fires_total": 0,
        "exact_probe_attached_mask": mask,
        "sampled_text_pgss_scans_total": scans,
        "sampled_text_resolved_total": resolved,
        "uptime_s": 12.0,
    }


class SampledOverheadPolicyTest(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(
            pid=1234,
            min_distinct_shapes=231,
            max_pgss_scans_per_sec=1.0,
            tracer_env=[],
        )

    @staticmethod
    def pair(metrics, shapes):
        return {"metrics": metrics, "distinct_shapes": shapes}

    def test_one_starved_pair_does_not_fail_aggregate_liveness(self):
        starved = valid_metrics(scans=0, resolved=0)
        exercised = valid_metrics(scans=1, resolved=200)

        self.assertEqual(
            structural_errors(self.args, "high-cardinality", starved, 0), [])
        summary, errors = aggregate_liveness_errors(
            self.args, "high-cardinality",
            [self.pair(starved, 0), self.pair(exercised, 256)])

        self.assertEqual(errors, [])
        self.assertEqual(summary["max_distinct_shapes"], 256)
        self.assertEqual(summary["total_pgss_scans"], 1)
        self.assertEqual(summary["total_resolved"], 200)

    def test_all_starved_pairs_fail_aggregate_liveness(self):
        starved = valid_metrics(scans=0, resolved=0)
        _, errors = aggregate_liveness_errors(
            self.args, "high-cardinality",
            [self.pair(starved, 0), self.pair(starved, 100)])

        self.assertEqual(len(errors), 3)

    def test_scan_rate_and_exact_probe_teeth_remain_per_pair(self):
        storm = valid_metrics(scans=20, resolved=200)
        probe = valid_metrics(query_fires=1, mask=1)

        self.assertTrue(any(
            "scan rate unbounded" in error for error in
            structural_errors(self.args, "high-cardinality", storm, 256)))
        self.assertTrue(any(
            "exact probes fired/attached" in error for error in
            structural_errors(self.args, "high-cardinality", probe, 256)))


if __name__ == "__main__":
    unittest.main()
