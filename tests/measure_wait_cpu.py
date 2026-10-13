#!/usr/bin/env python3
"""Measure per-event wait CPU via the offline execution_detail protocol.

No rehearsal gate. Exit 2 on incomplete data, 1 on a pure-sleep exceedance,
0 only for complete execution-associated data with a supplied threshold.
Without --threshold-ns this is calibration only (exit 3, no clean verdict).
Reports full original event durations/CPU, including events crossing the
window boundary: scaling cpu_ns could conceal the outlier we seek.
CPU* gaps are checked for null CPU but excluded from wait-class statistics.
Waits outside marker-identified executions (planning, idle, background work)
are outside this endpoint's scope; this cannot certify all raw trace records.
"""
import argparse
from collections import Counter, defaultdict
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

import server_harness

def pure_sleep(name):
    return name == "Timeout:PgSleep" or name.startswith("Lock:")


REFUSALS = (
    "window_empty", "no_executions", "execution_pages_incomplete",
    "truncated_lanes", "omitted_lane_events", "lane_count_mismatch",
    "detail_count_mismatch", "null_cpu_events", "invalid_events",
    "execution_event_shortfall", "empty_eventful_executions",
    "detail_errors", "nonexact_responses", "execution_count_mismatch",
    "no_pure_sleep_events", "server_errors",
    "census_errors", "pure_sleep_events_unrepresented",
    "pure_sleep_count_inconsistent",
)


def quantile(values, fraction):
    """Nearest-rank quantile, deterministic even for very small classes."""
    import math
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


class Measurement:
    def __init__(self, query, start, end, threshold=None, evidence=None):
        self.query, self.start, self.end = query, start, end
        self.threshold, self.evidence = threshold, evidence
        self.refusals = Counter({name: 0 for name in REFUSALS})
        self.counts = Counter()
        self.rows, self.events = {}, {}
        self.classes = defaultdict(list)
        self.census = {}

    def request(self, cmd, **args):
        self.counts[cmd + "_requests"] += 1
        response = self.query(cmd, **args)
        if self.evidence:
            self.evidence.write(json.dumps({"cmd": cmd, "args": args,
                                            "response": response}) + "\n")
        if response.get("error") or response.get("unavailable"):
            self.counts["error:" + response.get("code", "unknown")] += 1
            return None
        if response.get("fidelity") != "exact":
            self.refusals["nonexact_responses"] += 1
            return None
        return response

    def discover(self, start, end):
        response = self.request("executions", from_=str(start),
                                to_=str(end), limit=500)
        if response is None:
            self.refusals["execution_pages_incomplete"] += 1
            return
        rows = response["rows"]
        if response["truncated"] or response["total_count"] > len(rows):
            self.counts["split_execution_pages"] += 1
            if end - start <= 1:
                self.refusals["execution_pages_incomplete"] += 1
                return
            mid = (start + end) // 2
            # Inclusive endpoints intentionally overlap. Deduplicate by start.
            self.discover(start, mid)
            self.discover(mid, end)
            return
        if response["total_count"] != len(rows):
            self.refusals["execution_count_mismatch"] += 1
        for row in rows:
            key = (row["pid"], int(row["start_ns"]))
            if key in self.rows:
                self.counts["duplicate_execution_rows"] += 1
                # A row's n_events is window-local. Preserve the greatest
                # count, and the latest known end across discovery pages.
                old = self.rows[key]
                row["n_events"] = max(old["n_events"], row["n_events"])
                if old["end_ns"] is not None:
                    row["end_ns"] = old["end_ns"]
            self.rows[key] = row

    def detail(self, row):
        start = int(row["start_ns"])
        end = min(int(row["end_ns"]) if row["end_ns"] else self.end, self.end)
        if end <= start:
            self.counts["zero_span_executions"] += 1
            self.refusals["invalid_events"] += row["n_events"]
            return
        response = self.request("execution_detail", pid=row["pid"],
                                start_ns=str(start), end_ns=str(end))
        if response is None:
            self.refusals["detail_errors"] += 1
            return
        if response["total_count"] < row["n_events"]:
            self.refusals["execution_event_shortfall"] += (
                row["n_events"] - response["total_count"])
        if response["total_count"] == 0:
            self.refusals["empty_eventful_executions"] += 1
        lanes = [response["leader"], *response["workers"]]
        kept, total = 0, 0
        for lane in lanes:
            events = lane["events"]
            kept += len(events)
            total += lane["total_count"]
            if lane["truncated"] or lane["total_count"] > len(events):
                self.refusals["truncated_lanes"] += 1
                self.refusals["omitted_lane_events"] += max(
                    0, lane["total_count"] - len(events))
            elif lane["total_count"] != len(events):
                self.refusals["lane_count_mismatch"] += 1
            for event in events:
                # Nested executions and worker lanes can expose a record
                # twice. Identity excludes cpu so conflicting CPU is detected.
                key = (lane["pid"], event["we"], int(event["start_ns"]),
                       int(event["dur_ns"]))
                if key[2] + key[3] <= self.start or key[2] >= self.end:
                    self.counts["outside_window_events"] += 1
                    continue
                if key in self.events:
                    self.counts["duplicate_events"] += 1
                    if self.events[key] != event:
                        self.refusals["invalid_events"] += 1
                    continue
                self.events[key] = event
                self.counts["unique_lane_events"] += 1
                if event["cpu_ns"] is None:
                    self.refusals["null_cpu_events"] += 1
                    continue
                dur, cpu = int(event["dur_ns"]), int(event["cpu_ns"])
                if dur <= 0 or cpu < 0:
                    self.refusals["invalid_events"] += 1
                    continue
                if not event["we"]:
                    self.counts["cpu_gap_events"] += 1
                    continue
                self.classes[event["name"]].append((cpu, dur, key))
        if (response["kept_count"] != kept or response["total_count"] != total
                or bool(response["truncated"]) != (kept < total)):
            self.refusals["detail_count_mismatch"] += 1

    def run(self):
        if self.end <= self.start:
            self.refusals["window_empty"] += 1
        else:
            # Bound work per page even when the full capture is enormous.
            step = 60_000_000_000
            for start in range(self.start, self.end, step):
                self.discover(start, min(start + step, self.end))
        if not self.rows:
            self.refusals["no_executions"] += 1
        last_update = time.monotonic()
        for index, row in enumerate(sorted(self.rows.values(),
                                          key=lambda r: int(r["start_ns"]))):
            # n_events counts ALL transitions (CPU gaps included) attributed
            # to this execution in the discovery window, not just waits.
            # Zero-event rows have no input records for detail to serialize.
            if row["n_events"] == 0:
                self.counts["eventless_executions"] += 1
                continue
            self.counts["eventful_executions"] += 1
            self.counts["open_execution_rows"] += int(row["end_ns"] is None)
            self.counts["inferred_execution_ends"] += int(row["end_inferred"])
            self.detail(row)
            if time.monotonic() - last_update > 30:
                print(f"detail: {index + 1}/{len(self.rows)} executions", file=sys.stderr)
                last_update = time.monotonic()
        if not self.counts["unique_lane_events"]:
            self.refusals["window_empty"] += int(self.refusals["window_empty"] == 0)
        if not any(pure_sleep(name) for name in self.classes):
            self.refusals["no_pure_sleep_events"] += 1
        if self.end > self.start:
            census = self.request("top_events", from_=str(self.start), to_=str(self.end))
            if census is None:
                self.refusals["census_errors"] += 1
            else:
                self.census = {r["name"]: int(r["count"]) for r in census["rows"]
                               if r["name"] != "CPU*"}
                # LOCK utility statements need not have an EXEC_START at all.
                # A detail-only walk could silently certify a class it missed.
                for name in set(self.census) | set(self.classes):
                    if not pure_sleep(name):
                        continue
                    found = len(self.classes.get(name, []))
                    expected = self.census.get(name, 0)
                    if expected > found:
                        self.refusals["pure_sleep_events_unrepresented"] += expected - found
                    elif found > expected:
                        self.refusals["pure_sleep_count_inconsistent"] += found - expected
        return self.report()

    def report(self):
        classes = {}
        for name, events in sorted(self.classes.items()):
            cpus = [cpu for cpu, _, _ in events]
            ratios = [cpu / dur for cpu, dur, _ in events]
            worst = max(events, key=lambda e: e[0])
            classes[name] = {
                "count": len(events), "pure_sleep": pure_sleep(name),
                "cpu_ns_total": sum(cpus), "duration_ns_total": sum(e[1] for e in events),
                "cpu_ns_mean": sum(cpus) / len(cpus), "cpu_ns_max": max(cpus),
                "cpu_ns_p50": quantile(cpus, .5), "cpu_ns_p95": quantile(cpus, .95),
                "cpu_ns_p99": quantile(cpus, .99),
                "cpu_dur_mean": sum(ratios) / len(ratios),
                "cpu_dur_max": max(ratios),
                "cpu_dur_weighted": sum(cpus) / sum(e[1] for e in events),
                "above_threshold": (sum(c > self.threshold for c in cpus)
                                    if self.threshold is not None else None),
                "max_cpu_event": {"pid": worst[2][0], "start_ns": str(worst[2][2]),
                                  "duration_ns": worst[1], "cpu_ns": worst[0]},
            }
        crossings = sum(row["above_threshold"] or 0 for row in classes.values()
                        if row["pure_sleep"])
        status = ("REFUSED" if any(self.refusals.values()) else
                  "CALIBRATION" if self.threshold is None else
                  "EXCEEDED" if crossings else "COMPLETE_NO_EXCEEDANCE")
        return {"status": status, "scope": "execution-associated original wait events",
                "window": {"from_ns": str(self.start), "to_ns": str(self.end)},
                "threshold_ns": self.threshold, "executions_found": len(self.rows),
                "counts": dict(sorted(self.counts.items())),
                "refusals": dict(self.refusals), "classes": classes,
                "pure_sleep_exceedances": crossings,
                "capture_wait_census": self.census,
                "pure_sleep_coverage": {name: {"capture_events": self.census.get(name, 0),
                    "measured_events": len(self.classes.get(name, []))}
                    for name in sorted(set(self.census) | set(self.classes)) if pure_sleep(name)},
                "PgSleep_events": len(self.classes.get("Timeout:PgSleep", []))}


@contextmanager
def replay(server, trace, log):
    server_harness.SERVER_BIN = str(server.resolve())
    with server_harness.ServerHarness(str(trace), stderr_path=str(log)) as srv:
        yield srv


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-dir", required=True, type=Path)
    parser.add_argument("--server", type=Path, default=Path("pgwt-server"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--from-ns", type=int)
    parser.add_argument("--to-ns", type=int)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--protocol-log", type=Path,
                        help="Optional complete request/response JSONL evidence")
    args = parser.parse_args()
    if args.threshold_ns is not None and args.threshold_ns <= 0:
        parser.error("--threshold-ns must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    evidence = args.protocol_log.open("w") if args.protocol_log else None
    measurement = None
    try:
        with replay(args.server, args.trace_dir, args.output.with_suffix(".server.log")) as srv:
            info = srv.query("info", timeout=60)
            start = args.from_ns if args.from_ns is not None else int(info["from_ns"])
            end = args.to_ns if args.to_ns is not None else int(info["to_ns"])
            measurement = Measurement(lambda cmd, **kw: srv.query(cmd, timeout=60, **kw),
                                      start, end, args.threshold_ns, evidence)
            result = measurement.run()
            result["server_info"] = info
    except (OSError, RuntimeError, TimeoutError, ValueError, KeyError, TypeError,
            subprocess.SubprocessError) as exc:
        measurement = measurement or Measurement(None, args.from_ns or 0, args.to_ns or 0,
                                                 args.threshold_ns)
        measurement.refusals["server_errors"] += 1
        result = measurement.report()
        result["error"] = str(exc)
    finally:
        if evidence:
            evidence.close()
    # Fingerprint the actual input, not an assumed trace identity.
    result["trace_dir"] = str(args.trace_dir.resolve())
    result["trace_sha256"] = {}
    for path in sorted(args.trace_dir.glob("*.trace")):
        with path.open("rb") as source:
            result["trace_sha256"][path.name] = hashlib.file_digest(source, "sha256").hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return {"REFUSED": 2, "EXCEEDED": 1, "CALIBRATION": 3,
            "COMPLETE_NO_EXCEEDANCE": 0}[result["status"]]


if __name__ == "__main__":
    sys.exit(main())
