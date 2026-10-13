#!/usr/bin/env python3
"""Focused protocol/refusal tests; run manually, not a rehearsal gate."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from measure_wait_cpu import Measurement


def event(cpu="4321", name="Lock:advisory", we=1, start="120", dur="100000000"):
    return dict(cpu_ns=cpu, name=name, we=we, start_ns=start, dur_ns=dur)


def row(pid=42, start="110", n=1):
    return dict(pid=pid, start_ns=start, end_ns="200000000", n_events=n,
                end_inferred=False)


def detail(events, workers=None):
    lanes = [dict(pid=42, events=events, total_count=len(events), truncated=False),
             *(workers or [])]
    n = sum(l["total_count"] for l in lanes)
    return dict(fidelity="exact", leader=lanes[0], workers=lanes[1:],
                total_count=n, kept_count=n, truncated=False)


class ProtocolTests(unittest.TestCase):
    def measure(self, response, rows=None, threshold=43210):
        rows = rows if rows is not None else [row()]
        def query(cmd, **args):
            if cmd == "executions":
                return dict(fidelity="exact", rows=rows,
                            total_count=len(rows), truncated=False)
            if cmd == "top_events":
                names = {}
                for lane in [response.get("leader", {}), *response.get("workers", [])]:
                    for e in lane.get("events", []):
                        names[e["name"]] = names.get(e["name"], 0) + 1
                return dict(fidelity="exact", rows=[dict(name=n, count=c) for n, c in names.items()])
            return copy.deepcopy(response)
        return Measurement(query, 100, 300000000, threshold).run()

    def test_absent_pgsleep_is_explicit(self):
        result = self.measure(detail([event()]))
        self.assertEqual(result["status"], "COMPLETE_NO_EXCEEDANCE")
        self.assertEqual(result["PgSleep_events"], 0)
        self.assertEqual(set(result["classes"]), {"Lock:advisory"})

    def test_outlier_not_hidden_by_average(self):
        events = [event(start=str(120 + i)) for i in range(1000)]
        events.append(event(cpu="1234567", start="9000"))
        result = self.measure(detail(events), [row(n=len(events))])
        stats = result["classes"]["Lock:advisory"]
        self.assertLess(stats["cpu_ns_mean"], 43210)
        self.assertEqual(stats["cpu_ns_max"], 1234567)
        self.assertEqual(stats["above_threshold"], 1)
        self.assertEqual(result["status"], "EXCEEDED")

    def test_null_wait_and_cpu_gap_refuse(self):
        result = self.measure(detail([event(None), event(None, "CPU*", 0, "130")]),
                              [row(n=2)])
        self.assertEqual(result["refusals"]["null_cpu_events"], 2)
        self.assertEqual(result["status"], "REFUSED")

    def test_truncated_worker_refuses_with_omitted_count(self):
        response = detail([event()], [dict(pid=43, events=[event()],
                           total_count=2003, truncated=True)])
        response.update(total_count=2004, kept_count=2, truncated=True)
        result = self.measure(response)
        self.assertEqual(result["refusals"]["truncated_lanes"], 1)
        self.assertEqual(result["refusals"]["omitted_lane_events"], 2002)
        self.assertEqual(result["status"], "REFUSED")

    def test_no_executions_and_empty_window_refuse(self):
        result = self.measure(detail([]), [])
        for key in ["window_empty", "no_executions", "no_pure_sleep_events"]:
            self.assertEqual(result["refusals"][key], 1)
        self.assertEqual(result["status"], "REFUSED")

    def test_empty_eventful_execution_refuses(self):
        result = self.measure(detail([]), [row(n=3)])
        self.assertEqual(result["refusals"]["execution_event_shortfall"], 3)
        self.assertEqual(result["refusals"]["empty_eventful_executions"], 1)

    def test_eventless_rows_counted_without_detail(self):
        result = self.measure(detail([]), [row(n=0)])
        self.assertEqual(result["counts"]["eventless_executions"], 1)
        self.assertNotIn("execution_detail_requests", result["counts"])
        self.assertEqual(result["status"], "REFUSED")

    def test_missing_start_and_sampled_fidelity_refuse(self):
        result = self.measure(dict(error="execution start marker not found", code="not_found"))
        self.assertEqual(result["refusals"]["detail_errors"], 1)
        result = self.measure(dict(fidelity="sampled"))
        self.assertEqual(result["refusals"]["nonexact_responses"], 1)

    def test_dedup_nested_execution_and_workers(self):
        response = detail([event()], [dict(pid=43, events=[event()], total_count=1,
                                          truncated=False)])
        result = self.measure(response, [row(), row(start="111")])
        self.assertEqual(result["classes"]["Lock:advisory"]["count"], 2)
        self.assertEqual(result["counts"]["duplicate_events"], 2)

    def test_inconsistent_counts_refuse(self):
        response = detail([event()])
        response["kept_count"] = 2
        result = self.measure(response)
        self.assertEqual(result["refusals"]["detail_count_mismatch"], 1)

    def test_calibration_is_not_a_clean_verdict(self):
        result = self.measure(detail([event()]), threshold=None)
        self.assertEqual(result["status"], "CALIBRATION")
        self.assertIsNone(result["classes"]["Lock:advisory"]["above_threshold"])

    def test_eventful_zero_span_refuses(self):
        invalid = row(n=3)
        invalid["end_ns"] = invalid["start_ns"]
        result = self.measure(detail([]), [invalid])
        self.assertEqual(result["refusals"]["invalid_events"], 3)
        self.assertEqual(result["status"], "REFUSED")

    def test_recovered_page_truncation_deduplicates(self):
        def query(cmd, **args):
            if cmd == "execution_detail":
                return detail([event()])
            if cmd == "top_events":
                return dict(fidelity="exact", rows=[dict(name="Lock:advisory", count=1)])
            start, end = int(args["from_"]), int(args["to_"])
            if end - start > 150000000:
                return dict(fidelity="exact", rows=[row()], total_count=501, truncated=True)
            return dict(fidelity="exact", rows=[row()], total_count=1, truncated=False)
        result = Measurement(query, 100, 300000000, 43210).run()
        self.assertEqual(result["executions_found"], 1)
        self.assertEqual(result["counts"]["split_execution_pages"], 1)
        self.assertEqual(result["refusals"]["execution_pages_incomplete"], 0)

    def test_unrecoverable_page_truncation_refuses(self):
        result = Measurement(lambda *a, **kw: dict(fidelity="exact", rows=[],
                                                  total_count=501, truncated=True),
                             100, 101, 43210).run()
        self.assertEqual(result["refusals"]["execution_pages_incomplete"], 1)
        self.assertEqual(result["status"], "REFUSED")

    def test_wait_without_execution_start_is_counted_refusal(self):
        def query(cmd, **args):
            if cmd == "executions":
                return dict(fidelity="exact", rows=[row()], total_count=1, truncated=False)
            if cmd == "top_events":
                return dict(fidelity="exact", rows=[dict(name="Lock:advisory", count=1),
                                                     dict(name="Lock:relation", count=436)])
            return detail([event()])
        result = Measurement(query, 100, 300000000, 43210).run()
        self.assertEqual(result["refusals"]["pure_sleep_events_unrepresented"], 436)
        self.assertEqual(result["status"], "REFUSED")
        self.assertEqual(result["pure_sleep_coverage"]["Lock:relation"]["measured_events"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
