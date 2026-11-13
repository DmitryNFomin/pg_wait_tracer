#!/usr/bin/env python3
"""Box-only frozen measurement protocol, never a claim of qualification. Refs #871."""
import argparse
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ARMS = ("shared", "perf", "perf_boundary", "syscall_boundary", "syscall_request", "adversarial")
SYSCALL_ARMS = ("syscall_boundary", "syscall_request")
BOUNDARY_ARMS = ("perf_boundary",) + SYSCALL_ARMS
CASES = ("idle", "one", "all", "migrate", "hog")
CPU_FIELDS = ("user", "nice", "system", "irq", "softirq", "steal", "idle", "iowait")
BUSY_FIELDS = CPU_FIELDS[:6]
MAX_GAP_NS = 100_000_000


def positive(value):
    n = int(value)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(args):
    p = subprocess.run(args, cwd=ROOT, universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    return {"command": args, "exit": p.returncode, "stdout": p.stdout, "stderr": p.stderr}


def write_json(path, value):
    with path.open("x") as f:
        json.dump(value, f, indent=2, allow_nan=False)
        f.write("\n")


def irq_time_accounting(config):
    return b"CONFIG_IRQ_TIME_ACCOUNTING=y" in config.splitlines()


def metadata(out):
    release = platform.release()
    info = {"kernel_release": release, "uname": list(platform.uname()),
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "allowed_cpus": sorted(os.sched_getaffinity(0)),
            "cmdline": Path("/proc/cmdline").read_text().strip(),
            "online_cpus": Path("/sys/devices/system/cpu/online").read_text().strip(),
            "clocksource": Path("/sys/devices/system/clocksource/clocksource0/current_clocksource").read_text().strip(),
            "available_clocksources": Path("/sys/devices/system/clocksource/clocksource0/available_clocksource").read_text().strip(),
            "precise_cpu_accounting": "/proc/schedstat v15..17 CPU column 7, task running nanoseconds; absent/disabled is UNMEASURED",
            "cpu_accounting": "/proc/stat participating CPUs: user+nice+system+irq+softirq+steal; guest already in user; USER_HZ granularity",
            "syscall_helper_cpu_accounting": "per-helper CLOCK_THREAD_CPUTIME_ID over production; synchronous syscall/BPF included; wake-up IRQ and other-task scheduling costs not fully included",
            "head": capture(["git", "rev-parse", "HEAD"]),
            "status": capture(["git", "status", "--porcelain"]),
            "compiler": capture([os.environ.get("CC", "cc"), "--version"]),
            "bpf_compiler": capture([os.environ.get("CLANG", "clang"), "--version"])}
    config = None
    for path in (Path("/boot/config-" + release), Path("/proc/config.gz")):
        if path.is_file():
            config = gzip.decompress(path.read_bytes()) if path.suffix == ".gz" else path.read_bytes()
            info["kernel_config_source"] = str(path)
            break
    if config is None:
        raise RuntimeError("kernel config unavailable: require /boot/config-KERNEL or /proc/config.gz")
    (out / "kernel.config").write_bytes(config)
    info["kernel_config_sha256"] = hashlib.sha256(config).hexdigest()
    info["irq_time_accounting"] = irq_time_accounting(config)
    if Path("/usr/bin/rpm").exists():
        package = capture(["rpm", "-q", "--qf", "%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\nSOURCERPM=%{SOURCERPM}\n", "-f", "/boot/vmlinuz-" + release])
    else:
        package = capture(["dpkg-query", "-W", "-f=${Package} ${Version} ${Architecture}\n", "linux-image-" + release])
    if package["exit"] or not package["stdout"].strip():
        raise RuntimeError("exact running-kernel package unavailable: " + str(package))
    info["kernel_package"] = package
    info["linked_libraries"] = capture(["ldd", str(HERE / "nmi_transport")])
    info["build_environment"] = {k: os.environ.get(k, "") for k in
                                 ("CC", "CLANG", "CFLAGS", "BPF_CFLAGS", "LIBBPF_INC", "LIBBPF_LIB")}
    paths = sorted(HERE.glob("*.c")) + sorted(HERE.glob("*.h")) + sorted(HERE.glob("*.py"))
    paths += [HERE / "Makefile", HERE / "qualify.sh", HERE / "run.sh", HERE / "nmi_transport", HERE / "transport.bpf.o",
              ROOT / "src/bpf/pg_wait_tracer.bpf.c", ROOT / "src/pg_wait_tracer.h"]
    info["sha256"] = {str(p.relative_to(ROOT)): digest(p) for p in paths}
    return info


def sites(run):
    for cpu in run["per_cpu"]:
        for site in cpu["sites"]:
            yield cpu, site


def process_snapshot(proc=Path("/proc")):
    """PID plus starttime prevents reuse from creating a spurious CPU delta."""
    records = {}
    unreadable = 0
    for path in proc.iterdir():
        if not path.name.isdigit():
            continue
        try:
            stat = (path / "stat").read_text()
            left, right = stat.index("("), stat.rindex(")")
            fields = stat[right + 1:].split()
            records[(int(path.name), int(fields[19]))] = {
                "comm": stat[left + 1:right], "ticks": int(fields[11]) + int(fields[12])}
        except (OSError, ValueError, IndexError):
            unreadable += 1
    return {"records": records, "unreadable": unreadable, "observed_ns": int(time.monotonic() * 1e9)}


def process_deltas(before, after, bench_pid, clock_ticks):
    old, new = before["records"], after["records"]
    rows = []
    for identity in old.keys() & new.keys():
        pid, starttime = identity
        if pid == bench_pid:
            continue
        ticks = new[identity]["ticks"] - old[identity]["ticks"]
        if ticks < 0:
            raise ValueError("process CPU counter regressed")
        rows.append({"pid": pid, "starttime_ticks": starttime, "comm": new[identity]["comm"],
                     "cpu_ticks_start": old[identity]["ticks"], "cpu_ticks_end": new[identity]["ticks"],
                     "cpu_ticks_delta": ticks, "cpu_ns_delta": ticks * 1_000_000_000 // clock_ticks})
    rows.sort(key=lambda r: (-r["cpu_ticks_delta"], r["pid"]))
    return {"top3": rows[:3], "clock_ticks": clock_ticks, "bench_pid": bench_pid,
            "before_ns": before["observed_ns"], "after_ns": after["observed_ns"],
            "matched_non_bench": len(rows), "started": len(new.keys() - old.keys()),
            "exited": len(old.keys() - new.keys()), "unreadable_before": before["unreadable"],
            "unreadable_after": after["unreadable"],
            "coverage": "matching PID/starttime at both snapshots only; new, exited and unreadable processes excluded; window brackets bench setup, production and teardown"}


def accounting_errors(run):
    """Missing or inconsistent diagnostic evidence must not certify a cell."""
    errors = []
    try:
        def integer(n, minimum=0):
            return type(n) is int and n >= minimum

        def ticks(row):
            for phase in ("start", "end", "delta"):
                if set(row[phase]) != set(CPU_FIELDS):
                    return False
                if not all(type(v) is int and (phase == "delta" or v >= 0) for v in row[phase].values()):
                    return False
            return all(row["end"][f] - row["start"][f] == row["delta"][f] for f in CPU_FIELDS)

        rows = run["per_cpu"]
        total = run["proc_stat_participating_ticks"]
        if not ticks(total) or not ticks(run["proc_stat_whole_box_ticks"]) or any(not ticks(c["proc_stat_ticks"]) for c in rows):
            errors.append("invalid /proc/stat field deltas")
        for phase in ("start", "end", "delta"):
            if any(total[phase][f] != sum(c["proc_stat_ticks"][phase][f] for c in rows) for f in CPU_FIELDS):
                errors.append("/proc/stat participating sum mismatch")
        for phase in ("start", "end"):
            if sum(total[phase][f] for f in BUSY_FIELDS) != run["participating_busy_ticks_" + phase]:
                errors.append("/proc/stat busy ticks mismatch")
        hz = run["clock_ticks"]
        if not integer(hz, 1):
            errors.append("invalid CPU clock tick rate")
        elif run["participating_busy_cpu_ns"] != sum(total["delta"][f] for f in BUSY_FIELDS) * 1_000_000_000 // hz:
            errors.append("/proc/stat busy CPU nanoseconds mismatch")
        if (not integer(run["process_cpu_ns"]) or not math.isfinite(run["elapsed"]) or run["elapsed"] <= 0):
            errors.append("invalid bench process CPU time or elapsed time")
        if not math.isfinite(run["max_drain_gap_ms"]) or run["max_drain_gap_ms"] < 0:
            errors.append("invalid consumer drain gap")
        for c in rows:
            for field in ("helper_cpu_ns", "helper_wakeups", "helper_syscalls", "boundary_expected",
                          "hog_tid", "hog_iterations", "frontier_ns", "frontier_seen_ns",
                          "max_frontier_gap_ns", "max_frontier_stamp_gap_ns",
                          "max_boundary_gap_ns", "max_boundary_stamp_gap_ns"):
                if not integer(c[field]):
                    errors.append("invalid " + field)
            sched = c["schedstat_running_ns"]
            if sched["status"] == "UNMEASURED":
                if any(sched[k] is not None for k in ("start", "end", "delta")):
                    errors.append("unmeasured schedstat must not report zero")
            elif (sched["status"] != "MEASURED" or not all(integer(sched[k]) for k in ("start", "end", "delta")) or
                  sched["end"] - sched["start"] != sched["delta"]):
                errors.append("invalid schedstat running nanoseconds")
            for field in ("requests", "request_acknowledgements", "max_request_to_frontier_ns", "request_outstanding_age_ns", "phase_offset_ns"):
                if not integer(c[field]):
                    errors.append("invalid " + field)
            if type(c["request_outstanding"]) is not bool:
                errors.append("invalid request outstanding flag")
            if c["phase_offset_ns"] >= 1_000_000 or (not run["phase_jitter"] and c["phase_offset_ns"]):
                errors.append("invalid periodic helper phase offset")
            expected = (math.ceil(run["duration"] * 1e9 / run["period_ns"])
                        if run["period_ns"] and run.get("boundary_source") != "syscall_request" else 0)
            if c["boundary_expected"] != expected:
                errors.append("boundary expected-period count mismatch")
        if (not integer(run["cpu_accounting_elapsed_ns"], 1) or not integer(run["schedstat_elapsed_ns"], 1) or not integer(run["online_cpus"], 1) or
                type(run["phase_jitter"]) is not bool or not integer(run["request_after_ms"]) or
                not integer(run["coverage_ms"])):
            errors.append("invalid precise accounting interval or request options")
        processes = run["non_bench_process_cpu"]
        top = processes["top3"]
        if (processes["clock_ticks"] != hz or not integer(processes["bench_pid"], 1) or
                processes["after_ns"] < processes["before_ns"] or not processes["coverage"] or
                len(top) != min(3, processes["matched_non_bench"])):
            errors.append("invalid non-bench process CPU coverage")
        for field in ("matched_non_bench", "started", "exited", "unreadable_before", "unreadable_after", "before_ns", "after_ns"):
            if not integer(processes[field]):
                errors.append("invalid process snapshot " + field)
        if len({r["pid"] for r in top}) != len(top) or top != sorted(top, key=lambda r: (-r["cpu_ticks_delta"], r["pid"])):
            errors.append("invalid top-3 process CPU ordering")
        for r in top:
            if (not integer(r["pid"], 1) or r["pid"] == processes["bench_pid"] or
                    not integer(r["starttime_ticks"]) or not isinstance(r["comm"], str) or
                    not integer(r["cpu_ticks_start"]) or not integer(r["cpu_ticks_end"]) or
                    r["cpu_ticks_end"] - r["cpu_ticks_start"] != r["cpu_ticks_delta"] or
                    not integer(r["cpu_ticks_delta"]) or not integer(r["cpu_ns_delta"]) or
                    r["cpu_ns_delta"] != r["cpu_ticks_delta"] * 1_000_000_000 // hz):
                errors.append("invalid non-bench process CPU delta")
    except (KeyError, TypeError, ValueError, ZeroDivisionError, OverflowError):
        errors.append("missing or malformed CPU/frontier/cadence evidence")
    return errors


def base_errors(run, cpus, case, boundary, reconcile=True, request=False):
    errors = []
    if (not run["accepted"] or run["stage"] != "complete" or run["libbpf_error"] or
            run["bad_records"] or (reconcile and not run["accounting_ok"]) or run["seq_reorders"]):
        errors.append("load/runtime/framing/accounting failure")
    rows = run["per_cpu"]
    if len(rows) != cpus or len({c["cpu"] for c in rows}) != cpus:
        errors.append("participating CPU population mismatch")
    if reconcile:
        for cpu, site in sites(run):
            if (not site["reconciled"] or site["produced"] - site["delivered"] != site["loss"] or
                    site["loss"] != site["seq_gaps"]):
                errors.append(f"CPU {cpu['cpu']} {site['site']}: loss/sequence mismatch")
    for cpu in rows:
        if [s["site"] for s in cpu["sites"]] != ["DATA", "BOUNDARY"]:
            errors.append(f"CPU {cpu['cpu']}: site population mismatch")
            continue
        data, b = cpu["sites"]
        if case == "hog" and (not cpu["hog_tid"] or not cpu["hog_iterations"]):
            errors.append(f"CPU {cpu['cpu']}: untraced hog did not run")
        if boundary and not request and (not b["produced"] or not b["delivered"]):
            errors.append(f"CPU {cpu['cpu']}: boundary did not fire and reach reader")
        if not boundary and b["produced"]:
            errors.append("unexpected boundary producer")
        if case in ("idle", "hog") and data["produced"]:
            errors.append(f"{case} case produced DATA")
        if case == "all" and not data["produced"]:
            errors.append(f"CPU {cpu['cpu']}: active writer did not fire")
        if case == "migrate" and not data["produced"]:
            errors.append(f"CPU {cpu['cpu']}: migrating writer did not visit")
    produced_data = sum(c["sites"][0]["produced"] for c in rows)
    if produced_data != run["hits"]:
        errors.append("watchpoint attempts do not equal actual user stores")
    if case not in ("idle", "hog") and not produced_data:
        errors.append("DATA producer did not fire")
    if case == "one" and sum(c["sites"][0]["produced"] > 0 for c in rows) != 1:
        errors.append("one-active CPU population mismatch")
    if case == "migrate" and cpus > 1 and not run["migrations"]:
        errors.append("writer did not migrate")
    return errors


def normal_errors(run, cpus, case, boundary, arm="perf"):
    # The shared ring is a cost baseline. The adversarial treatment checks
    # seals/timestamps; normal perf transport arms must be lossless.
    evidence_errors = accounting_errors(run)
    if evidence_errors:
        return evidence_errors
    loss_gate = arm in ("perf",) + BOUNDARY_ARMS
    errors = base_errors(run, cpus, case, boundary, reconcile=loss_gate, request=arm == "syscall_request")
    if loss_gate:
        for cpu, site in sites(run):
            if site["loss"] or site["failures"] or site["successes"] != site["delivered"]:
                errors.append(f"CPU {cpu['cpu']} {site['site']}: nonzero loss")
    if (run["seal_violations"] or run["timestamp_regressions"] or
            any(c.get("seal_violations", 0) or c.get("timestamp_regressions", 0) for c in run["per_cpu"])):
        errors.append("seal violation or timestamp regression")
    if loss_gate and run["lost_notifications"]:
        errors.append("positive PERF_RECORD_LOST (reported separately)")
    if boundary and loss_gate:
        for cpu in run["per_cpu"]:
            b = cpu["sites"][1]
            if b["produced"] != b["successes"]:
                errors.append(f"CPU {cpu['cpu']}: boundary fires != outputs")
            if b["successes"] != b["delivered"]:
                errors.append(f"CPU {cpu['cpu']}: boundary outputs != consumer-delivered BOUNDARY")
            if arm != "syscall_request" and (cpu["max_frontier_gap_ns"] > MAX_GAP_NS or cpu["max_frontier_stamp_gap_ns"] > MAX_GAP_NS):
                errors.append(f"CPU {cpu['cpu']}: frontier gap exceeds 100 ms")
    if arm in SYSCALL_ARMS:
        if run["wrong_cpu_events"] or any(c["wrong_cpu_events"] for c in run["per_cpu"]):
            errors.append("wrong-CPU syscall boundary event")
        if run["boundary_fires"] != run["boundary_outputs"]:
            errors.append("syscall boundary fires != outputs")
        for cpu in run["per_cpu"]:
            b = cpu["sites"][1]
            if (not cpu["helper_tid"] or (arm != "syscall_request" and not cpu["helper_wakeups"]) or
                    cpu["syscall_fires"] != b["produced"] or
                    cpu["syscall_outputs"] != b["successes"] or
                    cpu["helper_syscalls"] != cpu["syscall_fires"] or
                    cpu["helper_syscalls"] > cpu["helper_wakeups"] or
                    (arm == "syscall_boundary" and cpu["helper_syscalls"] != cpu["helper_wakeups"])):
                errors.append(f"CPU {cpu['cpu']}: syscall helper/counter mismatch")
        if (sum(c["syscall_fires"] for c in run["per_cpu"]) != run["boundary_fires"] or
                sum(c["syscall_outputs"] for c in run["per_cpu"]) != run["boundary_outputs"]):
            errors.append("syscall boundary aggregate/per-CPU counter mismatch")
    if arm == "syscall_request":
        errors.extend(held_data_errors(run))
        for c in run["per_cpu"]:
            if (c["request_outstanding"] or c["request_acknowledgements"] > c["requests"] or
                    c["requests"] - c["request_acknowledgements"] != int(c["request_outstanding"]) or
                    c["helper_wakeups"] != c["requests"] or c["helper_syscalls"] != c["requests"]):
                errors.append(f"CPU {c['cpu']}: request/coalescing counter mismatch")
        if case in ("idle", "hog") and run["coverage_ms"] == 0 and any(c["requests"] for c in run["per_cpu"]):
            errors.append("request helper woke without staged DATA or coverage")
    return errors


def held_data_errors(run):
    try:
        h = run["held_data"]
        integer_fields = ("arrived", "released", "queue_depth", "queue_max_depth", "queue_growth_records",
                          "max_queue_growth_per_drain", "max_held_data_age_ns", "oldest_pending_age_ns")
        if not all(type(h[k]) is int and h[k] >= 0 for k in integer_fields):
            return ["invalid held DATA evidence"]
        data = sum(c["sites"][0]["delivered"] for c in run["per_cpu"])
        if (h["arrived"] != data or h["arrived"] - h["released"] != h["queue_depth"] or
                h["queue_growth_records"] != h["queue_depth"] or h["queue_max_depth"] < h["queue_depth"] or
                (h["arrived"] > 0 and h["queue_max_depth"] == 0) or
                h["max_queue_growth_per_drain"] > h["queue_max_depth"] or
                not math.isclose(h["queue_growth_records_per_second"], h["queue_depth"] / run["elapsed"], abs_tol=1e-8)):
            return ["held DATA/queue accounting mismatch"]
        errors = []
        if h["max_held_data_age_ns"] > MAX_GAP_NS or h["oldest_pending_age_ns"] > MAX_GAP_NS:
            errors.append("held DATA age exceeds 100 ms")
        if h["queue_depth"] or h["released"] != data:
            errors.append("consumed DATA remains held at end of cell")
        return errors
    except (KeyError, TypeError, ValueError):
        return ["missing or malformed held DATA evidence"]


def control_errors(loss, mutant, cpus):
    errors = base_errors(loss, cpus, "all", False) + base_errors(mutant, cpus, "all", True)
    if not loss["pause_drain"] or any(b != os.sysconf("SC_PAGESIZE") for b in loss["buffer_bytes"]):
        errors.append("loss control did not use paused one-page buffers")
    if sum(s["loss"] for _, s in sites(loss)) <= 0:
        errors.append("one-page loss control did not fire (LOST callback is not required)")
    if mutant["boundary_offset_ns"] != 1_000_000 or mutant["seal_violations"] <= 0:
        errors.append("+1 ms boundary control did not fire")
    return errors


def cpu_cost_ratio(perf, shared):
    # Idle has zero attempts: its cost is the boundary-only delta instead.
    if not perf["attempts"] or not shared["attempts"] or not shared["participating_busy_cpu_ns"] or not perf["participating_busy_cpu_ns"]:
        return None
    return (perf["participating_busy_cpu_ns"] / perf["attempts"]) / (shared["participating_busy_cpu_ns"] / shared["attempts"])


def idle_increment(boundary, perf, cpus):
    return (boundary["participating_busy_cpu_ns"] / boundary["elapsed"] -
            perf["participating_busy_cpu_ns"] / perf["elapsed"]) / 1e9 / cpus


def accounting_closure(run):
    elapsed = run["cpu_accounting_elapsed_ns"]
    def closure(ticks, cpus):
        observed_scaled = sum(ticks["delta"].values()) * 1_000_000_000
        expected = cpus * elapsed
        expected_scaled = expected * run["clock_ticks"]
        observed = observed_scaled / run["clock_ticks"]
        difference = (observed_scaled - expected_scaled) / expected_scaled
        return {"state_sum_ns": observed, "cpus_x_elapsed_ns": expected,
                "difference_fraction": difference, "status": "SUSPECT" if abs(observed_scaled - expected_scaled) * 200 > expected_scaled else "OK"}
    return {"participating": closure(run["proc_stat_participating_ticks"], len(run["per_cpu"])),
            "whole_box": closure(run["proc_stat_whole_box_ticks"], run["online_cpus"]),
            "per_cpu": {str(c["cpu"]): closure(c["proc_stat_ticks"], 1) for c in run["per_cpu"]}}


def schedstat_increment(boundary, perf):
    if perf is None:
        return None
    old = {c["cpu"]: c["schedstat_running_ns"] for c in perf["per_cpu"]}
    result = {}
    for c in boundary["per_cpu"]:
        before = old.get(c["cpu"])
        after = c["schedstat_running_ns"]
        if not before or before["status"] != "MEASURED" or after["status"] != "MEASURED":
            return None
        result[str(c["cpu"])] = (after["delta"] / boundary["schedstat_elapsed_ns"] -
                                  before["delta"] / perf["schedstat_elapsed_ns"])
    return result


def schedstat_over_limit(boundary, perf):
    # Compare the exact rational increment with 1/100; report rounding never
    # changes a boundary decision, even with unequal observation intervals.
    old = {c["cpu"]: c["schedstat_running_ns"]["delta"] for c in perf["per_cpu"]}
    after_ns, before_ns = boundary["schedstat_elapsed_ns"], perf["schedstat_elapsed_ns"]
    return any((c["schedstat_running_ns"]["delta"] * before_ns - old[c["cpu"]] * after_ns) * 100 >
               after_ns * before_ns for c in boundary["per_cpu"])


def evaluate_runs(runs, cpus, case, irq_accounting):
    errors = []
    for arm in runs:
        errors.extend(arm + ": " + e for e in accounting_errors(runs[arm]))
    if errors:
        return {"status": "FAIL", "errors": errors}
    for arm in runs:
        errors.extend(arm + ": " + e for e in normal_errors(
            runs[arm], cpus, case, arm in BOUNDARY_ARMS + ("adversarial",), arm=arm))
    ratio = cpu_cost_ratio(runs["perf"], runs["shared"]) if "perf" in runs and "shared" in runs else None
    if "shared" in runs and "perf" in runs and case not in ("idle", "hog") and (ratio is None or not math.isfinite(ratio) or ratio > 1.10):
        errors.append("paired perf/shared CPU-ns-per-attempt exceeds 1.10 or unavailable")
    result = {"perf_shared_cpu_ns_per_attempt_ratio": ratio, "errors": errors}
    gates = {arm: {"arm": arm, "status": "NOT_APPLICABLE"} for arm in BOUNDARY_ARMS if arm in runs}
    helper_cpu = {arm: {str(c["cpu"]): c["helper_cpu_ns"] / runs[arm]["elapsed"] / 1e9
                         for c in runs[arm]["per_cpu"]} for arm in SYSCALL_ARMS if arm in runs}
    result["syscall_helper_core_fraction_per_cpu"] = helper_cpu
    result["boundary_cadence_per_cpu"] = {
        arm: {str(c["cpu"]): {"fires": c["sites"][1]["produced"], "expected": c["boundary_expected"],
                              "fires_per_expected": (c["sites"][1]["produced"] / c["boundary_expected"] if c["boundary_expected"] else None),
                              "helper_wakeups": c["helper_wakeups"], "helper_syscalls": c["helper_syscalls"]}
              for c in runs[arm]["per_cpu"]} for arm in BOUNDARY_ARMS + ("adversarial",) if arm in runs}
    result["cpu_accounting"] = {arm: {
        "proc_stat_whole_box_ticks": r["proc_stat_whole_box_ticks"],
        "proc_stat_participating_ticks": r["proc_stat_participating_ticks"],
        "proc_stat_ticks_per_cpu": {str(c["cpu"]): c["proc_stat_ticks"] for c in r["per_cpu"]},
        "helper_cpu_ns_per_cpu": {str(c["cpu"]): c["helper_cpu_ns"] for c in r["per_cpu"]},
        "schedstat_running_ns_per_cpu": {str(c["cpu"]): c["schedstat_running_ns"] for c in r["per_cpu"]},
        "proc_stat_accounting_closure": accounting_closure(r),
        "bench_process_cpu_ns": r["process_cpu_ns"],
        "non_bench_process_cpu": r["non_bench_process_cpu"]} for arm, r in runs.items()}
    result["held_data_gate"] = {"status": "NOT_APPLICABLE"}
    if "syscall_request" in runs:
        held_errors = held_data_errors(runs["syscall_request"])
        evidence = runs["syscall_request"].get("held_data")
        result["held_data_gate"] = dict(evidence) if isinstance(evidence, dict) else {}
        result["held_data_gate"].update(status="FAIL" if held_errors else "PASS", limit_ns=MAX_GAP_NS,
                                        errors=held_errors)
    result["timing_diagnostics"] = {arm: {
        "consumer_drain_gap_ms": r["max_drain_gap_ms"],
        "per_cpu": {str(c["cpu"]): {
            "request_to_frontier_latency_ns": c["max_request_to_frontier_ns"] if c["request_acknowledgements"] else None,
            "request_outstanding_age_ns": c["request_outstanding_age_ns"],
            "producer_stamp_gap_ns": c["max_frontier_stamp_gap_ns"],
            "contract_frontier_gap_ns": c["max_frontier_gap_ns"],
            "boundary_only_gap_ns": c["max_boundary_gap_ns"],
            "boundary_only_stamp_gap_ns": c["max_boundary_stamp_gap_ns"]} for c in r["per_cpu"]}}
        for arm, r in runs.items()}
    precise_gates = {}
    result["schedstat_idle_cpu_gates"] = precise_gates
    if case == "idle":
        for arm in gates:
            delta = schedstat_increment(runs[arm], runs.get("perf"))
            gate = {"increment_core_fraction_per_cpu": delta, "limit": 0.01}
            if delta is None:
                gate.update(status="UNMEASURED", reason="schedstat or paired perf baseline unavailable")
            elif schedstat_over_limit(runs[arm], runs["perf"]):
                gate["status"] = "FAIL"
                errors.append(arm + ": schedstat idle increment exceeds 1% on a CPU")
            else:
                gate["status"] = "PASS"
            precise_gates[arm] = gate
        deltas = {arm: idle_increment(runs[arm], runs["perf"], cpus)
                  for arm in BOUNDARY_ARMS + ("adversarial",) if arm in runs and "perf" in runs}
        result["proc_stat_increment_core_fraction_per_participating_cpu"] = deltas
        for arm, gate in gates.items():
            if arm == "syscall_request":
                gate.update(precise_gates[arm])
                continue
            if arm not in deltas:
                gate.update(status="UNMEASURED", reason="paired perf baseline unavailable")
                continue
            gate["proc_stat_increment_core_fraction_per_participating_cpu"] = deltas[arm]
            if not irq_accounting:
                gate.update(status="UNMEASURED", reason=(
                    "CONFIG_IRQ_TIME_ACCOUNTING is not enabled in the running kernel config; "
                    "idle interrupt time is booked as idle, so /proc/stat cannot measure total boundary cost"))
            elif not math.isfinite(deltas[arm]) or deltas[arm] > 0.01:
                gate["status"] = "FAIL"
                errors.append(arm + ": boundary-only idle-box incremental CPU exceeds 1% per CPU")
            else:
                gate["status"] = "PASS"
        for arm in SYSCALL_ARMS:
            if arm not in runs:
                continue
            gates[arm]["helper_core_fraction_per_cpu"] = helper_cpu[arm]
            if any(v > 0.01 for v in helper_cpu[arm].values()):
                gates[arm]["status"] = "FAIL"
                errors.append(arm + ": helper CPU exceeds 1% on a CPU")
    result["boundary_cpu_gate"] = gates.get("perf_boundary", {"status": "NOT_APPLICABLE"}) # retained report field
    result["boundary_cpu_gates"] = gates
    result["status"] = "FAIL" if errors else "VOID" if any(
        g["status"] == "UNMEASURED" for g in list(gates.values()) + list(precise_gates.values())) else "PASS"
    return result


def requested_errors(run, args, cpu_ids):
    """Refuse mislabeled treatment data before applying measurement gates."""
    def value(flag):
        return args[args.index(flag) + 1]
    cpus = int(value("--cpus"))
    period = int(value("--period-ns")) if "--period-ns" in args else 0
    total = cpus * os.sysconf("SC_PAGESIZE") if "--one-page" in args else int(value("--bytes"))
    expected = {"transport": value("--transport"), "case": value("--case"), "cpus": cpus,
                "duration": int(value("--duration")), "period_ns": period,
                "buffer_bytes_total": total, "pause_drain": "--pause-drain" in args,
                "boundary_offset_ns": 1_000_000 if "--mutant-boundary" in args else 0}
    expected.update(request_after_ms=int(value("--request-after-ms")) if "--request-after-ms" in args else 50,
                    coverage_ms=int(value("--coverage-ms")) if "--coverage-ms" in args else 0,
                    phase_jitter="--phase-jitter" in args)
    expected["boundary_source"] = ("syscall_request" if "--syscall-request" in args else
                                   "syscall" if "--syscall-boundary" in args else "perf" if period else "none")
    errors = [f"requested {k}={v!r}, observed {run.get(k)!r}" for k, v in expected.items() if run.get(k) != v]
    if [c["cpu"] for c in run["per_cpu"]] != list(cpu_ids[:cpus]):
        errors.append("CPU identities differ from requested online affinity subset")
    if sum(run["buffer_bytes"]) != total:
        errors.append("buffer capacities do not sum to requested total")
    return errors


def run_one(out, label, args):
    command = [str(HERE / "nmi_transport"), "--object", str(HERE / "transport.bpf.o")] + args
    timeout = int(args[args.index("--duration") + 1]) + 120
    before = process_snapshot()
    with subprocess.Popen(command, cwd=ROOT, universal_newlines=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as p:
        try:
            stdout, stderr = p.communicate(timeout=timeout)
            result = {"command": command, "exit": p.returncode, "stdout": stdout, "stderr": stderr}
        except subprocess.TimeoutExpired:
            p.kill()
            stdout, stderr = p.communicate()
            result = {"command": command, "exit": "TIMEOUT", "timeout_seconds": timeout,
                      "stdout": stdout, "stderr": stderr}
    after = process_snapshot()
    result["non_bench_process_cpu"] = process_deltas(before, after, p.pid, os.sysconf("SC_CLK_TCK"))
    # Save raw stdout, stderr, exit and exact command even if JSON is malformed.
    write_json(out / (label + ".raw.json"), result)
    if result["exit"]:
        raise RuntimeError(f"{label}: command failed with exit {result['exit']}")
    run = json.loads(result["stdout"])
    if not isinstance(run, dict):
        raise ValueError(f"{label}: expected one JSON object")
    run["non_bench_process_cpu"] = result["non_bench_process_cpu"]
    errors = requested_errors(run, args, sorted(os.sched_getaffinity(0)))
    if errors:
        raise RuntimeError(label + ": " + "; ".join(errors))
    run["proc_stat_accounting_closure"] = accounting_closure(run)
    write_json(out / (label + ".json"), run)
    return run


def nonnegative(value):
    n = int(value)
    if n < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return n


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=positive, default=3)
    parser.add_argument("--seconds", type=positive, default=30)
    cadence = parser.add_mutually_exclusive_group()
    cadence.add_argument("--periods", type=positive, nargs="+", default=[10_000_000],
                        help="boundary cadences in ns; run every boundary arm at each cadence")
    cadence.add_argument("--period-ns", type=positive, help="legacy alias for one --periods value")
    parser.add_argument("--bytes", type=positive, default=67_108_864)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cases", choices=CASES, nargs="+", default=list(CASES))
    parser.add_argument("--arms", choices=ARMS, nargs="+", default=list(ARMS))
    parser.add_argument("--request-after-ms", type=nonnegative, default=50)
    parser.add_argument("--coverage-ms", type=nonnegative, default=0)
    parser.add_argument("--phase-jitter", action="store_true")
    parser.add_argument("--crossover", type=positive, metavar="N",
                        help="N idle/all-CPU perf/syscall_boundary pairs at 10 ms; random starting arm, then alternate")
    args = parser.parse_args(argv)
    if args.period_ns is not None:
        args.periods = [args.period_ns]
    for name in ("periods", "cases", "arms"):
        if len(set(getattr(args, name))) != len(getattr(args, name)):
            parser.error("--" + name + " must be distinct")
    if args.request_after_ms > (2**64 - 1) // 1_000_000 or args.coverage_ms > (2**64 - 1) // 1_000_000:
        parser.error("millisecond option exceeds uint64 nanoseconds")
    if args.crossover and (args.cases != list(CASES) or args.arms != list(ARMS) or
                           args.periods != [10_000_000]):
        parser.error("--crossover fixes cases, arms and periods; omit those filters")
    return args


def plan_cells(args, available):
    cells = []
    if args.crossover:
        order = ["perf", "syscall_boundary"]
        random.SystemRandom().shuffle(order)
        for repeat in range(1, args.crossover + 1):
            cells.append({"cell": f"crossover-r{repeat}", "cpus": available, "case": "idle",
                          "repeat": repeat, "period_ns": 10_000_000, "arm_order": list(order)})
    else:
        for population, cpus in (("1", 1), ("4", 4), ("all", available)):
            for case in args.cases:
                for period in args.periods:
                    for repeat in range(1, args.repeats + 1):
                        rotation = (repeat - 1) % len(args.arms)
                        order = args.arms[rotation:] + args.arms[:rotation]
                        cells.append({"cell": f"cpu-{population}-{case}-p{period}-r{repeat}", "cpus": cpus,
                                      "case": case, "repeat": repeat, "period_ns": period, "arm_order": order})
    arm_runs = sum(len(c["arm_order"]) for c in cells)
    control_runs = 3 * len(cells)
    return {"planned_cell_count": len(cells), "planned_arm_runs": arm_runs,
            "planned_control_runs": control_runs,
            "estimated_wall_seconds": arm_runs * args.seconds + control_runs * 2 + (arm_runs + control_runs) * 2,
            "estimate_basis": "requested durations plus 2 seconds setup/teardown per invocation; stalls/build excluded",
            "cells": cells}


def arm_command(arm, common, case, seconds, period, args):
    command = common + ["--case", case, "--duration", str(seconds),
                        "--transport", "shared" if arm == "shared" else "perf"]
    if arm in BOUNDARY_ARMS + ("adversarial",):
        command += ["--period-ns", str(period if arm in BOUNDARY_ARMS else 100_000)]
    if arm in SYSCALL_ARMS:
        command += ["--syscall-request" if arm == "syscall_request" else "--syscall-boundary"]
    if arm == "syscall_request":
        command += ["--request-after-ms", str(args.request_after_ms), "--coverage-ms", str(args.coverage_ms)]
    if args.phase_jitter:
        command += ["--phase-jitter"]
    return command


def main():
    args = parse_args()
    if platform.system() != "Linux":
        raise SystemExit("Linux only; never run BPF on macOS")
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        raise SystemExit("--out must be outside the repository")
    out.mkdir(parents=True, exist_ok=False)
    try:
        info = None
        # CPU discovery precedes building so the plan is printed at startup.
        available = len(os.sched_getaffinity(0))
        plan = plan_cells(args, available)
        print(f"Planned cells: {plan['planned_cell_count']}; arm runs: {plan['planned_arm_runs']}; "
              f"estimated wall time: {plan['estimated_wall_seconds'] / 60:.1f} minutes", flush=True)
        write_json(out / "plan.json", plan)
        build = capture(["make", "-C", str(HERE.parent), "bench-nmi-transport"])
        write_json(out / "build.json", build)
        if build["exit"]:
            raise RuntimeError("measurement build failed")
        info = metadata(out)
        if len(info["allowed_cpus"]) != available:
            raise RuntimeError("CPU population changed after planning")
        info["protocol"] = dict(vars(args), out=str(out))
        info["plan"] = "plan.json"
        info["claim"] = "measurement only; kernel argument and reviewer judgment still required"
        write_json(out / "metadata.json", info)
    except (OSError, ValueError, RuntimeError) as exc:
        write_json(out / "VOID.json", {"status": "VOID", "error": str(exc)})
        return 1
    any_failure = False
    summaries = []
    for cell in plan["cells"]:
        label, cpus, case, period = (cell[k] for k in ("cell", "cpus", "case", "period_ns"))
        summary = dict(cell, metadata="metadata.json", runs={}, errors=[], status="VOID")
        try:
            if cpus > available:
                raise RuntimeError(f"required {cpus}-CPU cell unavailable; only {available} online allowed")
            common = ["--cpus", str(cpus), "--bytes", str(args.bytes)]
            # Preserve fresh controls for every selected cell, including crossover pairs.
            controls = common + ["--duration", "2", "--transport", "perf", "--case", "all"]
            loss = run_one(out, label + "-loss-control", controls + ["--one-page", "--pause-drain"])
            mutant = run_one(out, label + "-seal-control", controls + ["--period-ns", "100000", "--mutant-boundary"])
            summary["controls"] = {"loss": loss, "seal": mutant}
            errors = control_errors(loss, mutant, cpus)
            syscall_mutant = run_one(out, label + "-syscall-seal-control", controls + [
                "--syscall-boundary", "--period-ns", "100000", "--mutant-boundary"])
            summary["controls"]["syscall_seal"] = syscall_mutant
            errors += control_errors(loss, syscall_mutant, cpus)
            if errors:
                raise RuntimeError("control VOID: " + "; ".join(errors))
            for arm in cell["arm_order"]:
                command = arm_command(arm, common, case, args.seconds, period, args)
                summary["runs"][arm] = run_one(out, label + "-" + arm, command)
            summary.update(evaluate_runs(summary["runs"], cpus, case, info["irq_time_accounting"]))
        except (OSError, ValueError, KeyError, TypeError, RuntimeError, ZeroDivisionError) as exc:
            summary["errors"].append(str(exc))
        write_json(out / (label + ".summary.json"), summary)
        summaries.append({"cell": label, "status": summary["status"]})
        print(label + ": " + summary["status"], flush=True)
        any_failure |= summary["status"] != "PASS"
    write_json(out / "summary.json", {"status": "FAIL" if any_failure else "PASS", "cells": summaries,
                                     "plan": "plan.json", "qualification_claim": False})
    return int(any_failure)


if __name__ == "__main__":
    sys.exit(main())
