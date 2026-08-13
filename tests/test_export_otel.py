#!/usr/bin/env python3
"""test_export_otel.py — Unit tests for tools/export_otel.py (F-C3).

Validates OpenTelemetry span tree generation, parallel-worker span hierarchy,
wait-class densified naming, and W3C traceparent context extraction.
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from export_otel import (
    build_otel_spans_for_execution,
    build_otel_payload,
    get_class_name,
    WAIT_CLASSES,
)


class TestExportOtel(unittest.TestCase):

    def test_densified_wait_classes(self):
        """Wait class array must match the densified server ordering."""
        expected = [
            "CPU", "IO", "Lock", "LWLock", "IPC",
            "Client", "Timeout", "BufferPin", "Activity", "Extension"
        ]
        self.assertEqual(WAIT_CLASSES, expected)
        self.assertEqual(get_class_name(0), "CPU")
        self.assertEqual(get_class_name(1), "IO")
        self.assertEqual(get_class_name(2), "Lock")
        self.assertEqual(get_class_name(3), "LWLock")
        self.assertEqual(get_class_name(4), "IPC")
        self.assertEqual(get_class_name(9), "Extension")

    def test_single_execution_spans(self):
        """Test span tree for a simple single-backend execution."""
        exec_row = {
            "pid": 1000,
            "query_id": "1234567890",
            "start_ns": "1000000000",
            "end_ns": "1050000000",
            "duration_ms": 50.0,
            "trace_id": "60b6417fa7216a6552a4be6058e5ec9b",
            "parent_span_id": "325b138ff40c1d29",
        }
        detail_data = {
            "query_id": "1234567890",
            "query_text": "SELECT 1",
            "trace_id": "60b6417fa7216a6552a4be6058e5ec9b",
            "parent_span_id": "325b138ff40c1d29",
            "plan": {
                "start_ns": "995000000",
                "end_ns": "1000000000",
            },
            "leader": {
                "pid": 1000,
                "events": [
                    {
                        "we": 0x01000015,  # IO:DataFileRead (class 1)
                        "name": "IO:DataFileRead",
                        "start_ns": "1010000000",
                        "dur_ns": "15000000",
                    },
                    {
                        "we": 0,  # CPU
                        "name": "CPU",
                        "start_ns": "1030000000",
                        "dur_ns": "10000000",
                    }
                ]
            },
            "workers": []
        }

        spans = build_otel_spans_for_execution(exec_row, detail_data, query_text="SELECT 1")
        self.assertTrue(len(spans) >= 3, "Root + Plan + 2 Events (+ inferred CPU gaps)")

        # Verify Root Span
        root = spans[0]
        self.assertEqual(root["traceId"], "60b6417fa7216a6552a4be6058e5ec9b")
        self.assertEqual(root["parentSpanId"], "325b138ff40c1d29")
        self.assertEqual(root["name"], "SELECT 1")
        self.assertEqual(root["kind"], 3)  # SPAN_KIND_CLIENT
        self.assertEqual(root["startTimeUnixNano"], "1000000000")
        self.assertEqual(root["endTimeUnixNano"], "1050000000")

        # Verify Plan Span
        plan_span = spans[1]
        self.assertEqual(plan_span["name"], "Planner")
        self.assertEqual(plan_span["parentSpanId"], root["spanId"])
        self.assertEqual(plan_span["startTimeUnixNano"], "995000000")
        self.assertEqual(plan_span["endTimeUnixNano"], "1000000000")

        # Verify OTLP payload envelope
        payload = build_otel_payload(spans)
        self.assertIn("resourceSpans", payload)
        self.assertEqual(len(payload["resourceSpans"]), 1)
        res = payload["resourceSpans"][0]
        self.assertEqual(res["scopeSpans"][0]["scope"]["name"], "pg_wait_tracer")
        self.assertEqual(len(res["scopeSpans"][0]["spans"]), len(spans))

    def test_parallel_worker_spans(self):
        """Parallel worker spans must be nested under root and bound to worker's own timeline."""
        exec_row = {
            "pid": 2000,
            "query_id": "9999",
            "start_ns": "2000000000",
            "end_ns": "2100000000",
            "duration_ms": 100.0,
        }
        detail_data = {
            "query_id": "9999",
            "leader": {
                "pid": 2000,
                "events": [
                    {
                        "we": 0,
                        "name": "CPU",
                        "start_ns": "2000000000",
                        "dur_ns": "50000000",
                    }
                ]
            },
            "workers": [
                {
                    "pid": 2001,
                    "events": [
                        {
                            "we": 0x01000015,
                            "name": "IO:DataFileRead",
                            "start_ns": "2020000000",
                            "dur_ns": "30000000",
                        }
                    ]
                }
            ]
        }

        spans = build_otel_spans_for_execution(exec_row, detail_data)
        root = spans[0]
        self.assertEqual(root["startTimeUnixNano"], "2000000000")

        # Find worker span
        worker_spans = [s for s in spans if s.get("name") == "Parallel Worker (PID 2001)"]
        self.assertEqual(len(worker_spans), 1)
        w_span = worker_spans[0]
        self.assertEqual(w_span["parentSpanId"], root["spanId"])
        self.assertEqual(w_span["startTimeUnixNano"], "2020000000")
        self.assertEqual(w_span["endTimeUnixNano"], "2050000000")

        # Worker's wait event must be nested under worker_span
        w_event_spans = [s for s in spans if s.get("name") == "IO:DataFileRead"]
        self.assertEqual(len(w_event_spans), 1)
        self.assertEqual(w_event_spans[0]["parentSpanId"], w_span["spanId"])

    def test_plan_tree_attributes(self):
        """Plan tree metadata must enrich the root span with operator and node count."""
        exec_row = {
            "pid": 3000,
            "query_id": "8888",
            "start_ns": "3000000000",
            "end_ns": "3050000000",
        }
        detail_data = {
            "query_id": "8888",
            "query_text": "SELECT * FROM large_table",
            "plan_tree": [
                {
                    "id": 0,
                    "tag": 364,
                    "type": "Gather",
                    "label": "Gather (2 workers)",
                    "workers": 2,
                    "left_id": 1,
                },
                {
                    "id": 1,
                    "tag": 335,
                    "type": "Seq Scan",
                    "rel": "large_table",
                    "label": "Seq Scan on large_table",
                }
            ],
            "leader": {"pid": 3000, "events": []},
            "workers": []
        }

        spans = build_otel_spans_for_execution(exec_row, detail_data)
        root = spans[0]
        attr_dict = {a["key"]: a["value"] for a in root.get("attributes", [])}
        self.assertIn("pgwt.plan.nodes_count", attr_dict)
        self.assertEqual(attr_dict["pgwt.plan.nodes_count"]["intValue"], 2)
        self.assertIn("pgwt.plan.root_node", attr_dict)
        self.assertEqual(attr_dict["pgwt.plan.root_node"]["stringValue"], "Gather (2 workers)")


if __name__ == "__main__":
    unittest.main()
