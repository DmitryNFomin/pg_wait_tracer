#!/usr/bin/env python3
"""export_otel.py — Export pg_wait_tracer execution traces to OpenTelemetry OTLP JSON.

Converts recorded PostgreSQL wait profiles, execution lifecycles, and parallel worker
timelines into standard OpenTelemetry Distributed Tracing spans for ingestion into
Jaeger, Tempo, Honeycomb, or other OTel-compliant trace backends.

Adheres to W3C TraceContext standards and the densified wait class index order:
CPU, IO, Lock, LWLock, IPC, Client, Timeout, BufferPin, Activity, Extension.
"""

import argparse
import json
import os
import secrets
import subprocess
import sys
import time

DEFAULT_SERVER_BIN = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pgwt-server"
)

WAIT_CLASSES = [
    "CPU", "IO", "Lock", "LWLock", "IPC",
    "Client", "Timeout", "BufferPin", "Activity", "Extension"
]


def get_class_name(class_idx):
    if 0 <= class_idx < len(WAIT_CLASSES):
        return WAIT_CLASSES[class_idx]
    return "Unknown"


def gen_hex_id(num_bytes):
    return secrets.token_hex(num_bytes)


class ServerClient:
    """Client to query pgwt-server via standard line-delimited JSON interface."""

    def __init__(self, server_bin, trace_dir):
        self.server_bin = server_bin
        self.trace_dir = trace_dir
        self.proc = None
        self.req_id = 1

    def __enter__(self):
        if not os.path.exists(self.server_bin):
            raise FileNotFoundError(f"Server binary not found at {self.server_bin}")
        self.proc = subprocess.Popen(
            [self.server_bin, self.trace_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.proc:
            try:
                self.proc.stdin.close()
                self.proc.terminate()
                self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()

    def query(self, cmd, **kwargs):
        req = {"cmd": cmd, "id": self.req_id}
        req.update(kwargs)
        self.req_id += 1
        line = json.dumps(req)
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

        resp_line = self.proc.stdout.readline()
        if not resp_line:
            err = self.proc.stderr.read()
            raise RuntimeError(f"Server closed connection unexpectedly. Stderr: {err}")
        return json.loads(resp_line)


def build_otel_spans_for_execution(exec_row, detail_data, query_text=None, trace_id=None, parent_span_id=None):
    """Build OTLP span trees for a single execution and its worker lanes."""
    spans = []

    # Use trace_id from detail or query text comment if available, else generate 16-byte hex
    if not trace_id:
        trace_id = detail_data.get("trace_id") or exec_row.get("trace_id")
    if not trace_id:
        trace_id = gen_hex_id(16)

    # Use parent_span_id if available
    if not parent_span_id:
        parent_span_id = detail_data.get("parent_span_id") or exec_row.get("parent_span_id")

    leader_pid = int(exec_row["pid"])
    start_ns = int(exec_row["start_ns"])
    end_ns = int(exec_row["end_ns"]) if exec_row.get("end_ns") is not None else (start_ns + int(exec_row.get("duration_ms", 1) * 1e6))
    if end_ns <= start_ns:
        end_ns = start_ns + 1000

    query_name = query_text or detail_data.get("query_text") or f"Execution PID {leader_pid}"

    # 1. Root span for the execution
    root_span_id = gen_hex_id(8)
    root_attributes = [
        {"key": "db.system", "value": {"stringValue": "postgresql"}},
        {"key": "pg.pid", "value": {"stringValue": str(leader_pid)}},
        {"key": "pg.query_id", "value": {"stringValue": str(exec_row.get("query_id", "0"))}},
    ]
    if query_text or detail_data.get("query_text"):
        root_attributes.append({
            "key": "db.statement",
            "value": {"stringValue": query_text or detail_data.get("query_text", "")}
        })
    plan_tree = detail_data.get("plan_tree", [])
    if plan_tree:
        root_attributes.append({
            "key": "pgwt.plan.nodes_count",
            "value": {"intValue": len(plan_tree)}
        })
        first_node = plan_tree[0]
        if first_node.get("label") or first_node.get("type"):
            root_attributes.append({
                "key": "pgwt.plan.root_node",
                "value": {"stringValue": first_node.get("label") or first_node.get("type", "")}
            })

    root_span = {
        "traceId": trace_id,
        "spanId": root_span_id,
        "name": query_name,
        "kind": 3,  # SPAN_KIND_CLIENT
        "startTimeUnixNano": str(start_ns),
        "endTimeUnixNano": str(end_ns),
        "attributes": root_attributes,
        "status": {}
    }
    if parent_span_id:
        root_span["parentSpanId"] = parent_span_id
    spans.append(root_span)

    # 2. Plan phase span if present
    plan = detail_data.get("plan")
    if plan and plan.get("start_ns") and plan.get("end_ns"):
        p_start = int(plan["start_ns"])
        p_end = int(plan["end_ns"])
        if p_end > p_start:
            plan_span_id = gen_hex_id(8)
            spans.append({
                "traceId": trace_id,
                "spanId": plan_span_id,
                "parentSpanId": root_span_id,
                "name": "Planner",
                "kind": 1,  # SPAN_KIND_INTERNAL
                "startTimeUnixNano": str(p_start),
                "endTimeUnixNano": str(p_end),
                "attributes": [
                    {"key": "pg.pid", "value": {"stringValue": str(leader_pid)}},
                    {"key": "pgwt.phase", "value": {"stringValue": "plan"}}
                ],
                "status": {}
            })

    # 3. Process lanes: Leader and Workers
    leader_lane = detail_data.get("leader", {})
    worker_lanes = detail_data.get("workers", [])

    all_lanes = []
    if leader_lane:
        all_lanes.append((leader_pid, leader_lane, True))
    for w in worker_lanes:
        w_pid = int(w.get("pid", 0))
        all_lanes.append((w_pid, w, False))

    for pid, lane, is_leader in all_lanes:
        events = lane.get("events", [])
        if not events:
            continue

        sorted_events = sorted(events, key=lambda ev: int(ev.get("start_ns", 0)))
        lane_start = int(sorted_events[0]["start_ns"])
        lane_end = int(sorted_events[-1]["start_ns"]) + int(sorted_events[-1]["dur_ns"])

        if is_leader:
            parent_for_events = root_span_id
        else:
            # Parallel worker lane span bounded by worker's own timeline
            worker_span_id = gen_hex_id(8)
            worker_span = {
                "traceId": trace_id,
                "spanId": worker_span_id,
                "parentSpanId": root_span_id,
                "name": f"Parallel Worker (PID {pid})",
                "kind": 1,  # SPAN_KIND_INTERNAL
                "startTimeUnixNano": str(lane_start),
                "endTimeUnixNano": str(lane_end),
                "attributes": [
                    {"key": "pg.pid", "value": {"stringValue": str(pid)}},
                    {"key": "pg.leader_pid", "value": {"stringValue": str(leader_pid)}},
                    {"key": "pgwt.role", "value": {"stringValue": "parallel_worker"}}
                ],
                "status": {}
            }
            spans.append(worker_span)
            parent_for_events = worker_span_id

        # Wait events within this lane
        last_end = lane_start
        for ev in sorted_events:
            ev_start = int(ev["start_ns"])
            ev_dur = int(ev["dur_ns"])
            ev_name = ev.get("name", "Unknown")
            ev_we = int(ev.get("we", 0))

            # Insert inferred CPU gap if there is a gap > 1us
            if ev_start > last_end + 1000:
                cpu_gap_id = gen_hex_id(8)
                spans.append({
                    "traceId": trace_id,
                    "spanId": cpu_gap_id,
                    "parentSpanId": parent_for_events,
                    "name": "CPU",
                    "kind": 1,
                    "startTimeUnixNano": str(last_end),
                    "endTimeUnixNano": str(ev_start),
                    "attributes": [
                        {"key": "pg.pid", "value": {"stringValue": str(pid)}},
                        {"key": "pgwt.wait_class", "value": {"stringValue": "CPU"}}
                    ],
                    "status": {}
                })

            span_id = gen_hex_id(8)
            class_idx = (ev_we >> 24) & 0xFF if ev_we != 0 else 0
            attributes = [
                {"key": "pg.pid", "value": {"stringValue": str(pid)}},
                {"key": "pgwt.wait_class", "value": {"stringValue": get_class_name(class_idx)}}
            ]
            if ev_we != 0:
                attributes.append({"key": "pgwt.wait_event_id", "value": {"stringValue": str(ev_we)}})
            if ev.get("cpu_ns") is not None:
                attributes.append({"key": "pgwt.cpu_ns", "value": {"stringValue": str(ev["cpu_ns"])}})

            spans.append({
                "traceId": trace_id,
                "spanId": span_id,
                "parentSpanId": parent_for_events,
                "name": ev_name,
                "kind": 1,
                "startTimeUnixNano": str(ev_start),
                "endTimeUnixNano": str(ev_start + ev_dur),
                "attributes": attributes,
                "status": {}
            })
            last_end = ev_start + ev_dur

    return spans


def build_otel_payload(all_spans):
    """Wrap spans into standard OpenTelemetry ResourceSpans envelope."""
    return {
        "resourceSpans": [
            {
                "resource": {
                    "attributes": [
                        {"key": "service.name", "value": {"stringValue": "postgresql"}},
                        {"key": "telemetry.sdk.name", "value": {"stringValue": "pg_wait_tracer"}},
                        {"key": "telemetry.sdk.version", "value": {"stringValue": "1.0.0"}},
                    ]
                },
                "scopeSpans": [
                    {
                        "scope": {
                            "name": "pg_wait_tracer",
                            "version": "1.0.0"
                        },
                        "spans": all_spans
                    }
                ]
            }
        ]
    }


def export_traces(server_bin, trace_dir, query_id=None, pid=None):
    """Fetch executions and details from pgwt-server and build OTLP payload."""
    with ServerClient(server_bin, trace_dir) as client:
        info = client.query("info")
        from_ns = info.get("from_ns", 0)
        to_ns = info.get("to_ns", 0)

        if from_ns == 0 or to_ns == 0:
            return build_otel_payload([])

        # Query executions
        req_params = {"from_ns": from_ns, "to_ns": to_ns, "limit": 500}
        if query_id:
            req_params["query_id"] = str(query_id)
        if pid:
            req_params["pid"] = int(pid)

        execs_resp = client.query("executions", **req_params)
        rows = execs_resp.get("rows", [])
        if not rows:
            return build_otel_payload([])

        all_spans = []
        for row in rows:
            p = row["pid"]
            s_ns = row["start_ns"]
            e_ns = row.get("end_ns") or str(int(s_ns) + int(row.get("duration_ms", 1) * 1e6))

            detail_resp = client.query("execution_detail", pid=int(p), start_ns=str(s_ns), end_ns=str(e_ns))
            spans = build_otel_spans_for_execution(row, detail_resp)
            all_spans.extend(spans)

        return build_otel_payload(all_spans)


def main():
    parser = argparse.ArgumentParser(description="Export pg_wait_tracer traces to OpenTelemetry OTLP JSON.")
    parser.add_argument("--trace-dir", default="/tmp/pgwt_trace", help="Path to trace directory.")
    parser.add_argument("--server-bin", default=DEFAULT_SERVER_BIN, help="Path to pgwt-server binary.")
    parser.add_argument("--query-id", help="Optional query_id to filter.")
    parser.add_argument("--pid", type=int, help="Optional leader PID to filter.")
    parser.add_argument("--output", help="Optional output JSON file path.")
    args = parser.parse_args()

    if not os.path.exists(args.trace_dir):
        print(f"Error: Trace directory '{args.trace_dir}' does not exist.", file=sys.stderr)
        sys.exit(1)

    payload = export_traces(args.server_bin, args.trace_dir, query_id=args.query_id, pid=args.pid)
    out_json = json.dumps(payload, indent=2)

    if args.output:
        with open(args.output, "w") as f:
            f.write(out_json)
        print(f"Exported OpenTelemetry trace to {args.output}", file=sys.stderr)
    else:
        print(out_json)


if __name__ == "__main__":
    main()
