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
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ARMS = ("shared", "perf", "perf_boundary", "adversarial")
CASES = ("idle", "one", "all", "migrate")
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


def metadata(out):
    release = platform.release()
    info = {"kernel_release": release, "uname": list(platform.uname()),
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "allowed_cpus": sorted(os.sched_getaffinity(0)),
            "cmdline": Path("/proc/cmdline").read_text().strip(),
            "online_cpus": Path("/sys/devices/system/cpu/online").read_text().strip(),
            "clocksource": Path("/sys/devices/system/clocksource/clocksource0/current_clocksource").read_text().strip(),
            "available_clocksources": Path("/sys/devices/system/clocksource/clocksource0/available_clocksource").read_text().strip(),
            "cpu_accounting": "/proc/stat participating CPUs: user+nice+system+irq+softirq+steal; guest already in user; USER_HZ granularity",
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


def base_errors(run, cpus, case, boundary):
    errors = []
    if (not run["accepted"] or run["stage"] != "complete" or run["libbpf_error"] or
            run["bad_records"] or not run["accounting_ok"] or run["seq_reorders"]):
        errors.append("load/runtime/framing/accounting failure")
    rows = run["per_cpu"]
    if len(rows) != cpus or len({c["cpu"] for c in rows}) != cpus:
        errors.append("participating CPU population mismatch")
    for cpu, site in sites(run):
        if (not site["reconciled"] or site["produced"] - site["delivered"] != site["loss"] or
                site["loss"] != site["seq_gaps"]):
            errors.append(f"CPU {cpu['cpu']} {site['site']}: loss/sequence mismatch")
    for cpu in rows:
        if [s["site"] for s in cpu["sites"]] != ["DATA", "BOUNDARY"]:
            errors.append(f"CPU {cpu['cpu']}: site population mismatch")
            continue
        data, b = cpu["sites"]
        if boundary and (not b["produced"] or not b["delivered"]):
            errors.append(f"CPU {cpu['cpu']}: boundary did not fire and reach reader")
        if not boundary and b["produced"]:
            errors.append("unexpected boundary producer")
        if case == "idle" and data["produced"]:
            errors.append("idle case produced DATA")
        if case == "all" and not data["produced"]:
            errors.append(f"CPU {cpu['cpu']}: active writer did not fire")
        if case == "migrate" and not data["produced"]:
            errors.append(f"CPU {cpu['cpu']}: migrating writer did not visit")
    produced_data = sum(c["sites"][0]["produced"] for c in rows)
    if produced_data != run["hits"]:
        errors.append("watchpoint attempts do not equal actual user stores")
    if case != "idle" and not produced_data:
        errors.append("DATA producer did not fire")
    if case == "one" and sum(c["sites"][0]["produced"] > 0 for c in rows) != 1:
        errors.append("one-active CPU population mismatch")
    if case == "migrate" and cpus > 1 and not run["migrations"]:
        errors.append("writer did not migrate")
    return errors


def normal_errors(run, cpus, case, boundary):
    errors = base_errors(run, cpus, case, boundary)
    for cpu, site in sites(run):
        if site["loss"] or site["failures"] or site["successes"] != site["delivered"]:
            errors.append(f"CPU {cpu['cpu']} {site['site']}: nonzero loss")
    if (run["seal_violations"] or run["timestamp_regressions"] or
            any(c.get("seal_violations", 0) or c.get("timestamp_regressions", 0) for c in run["per_cpu"])):
        errors.append("seal violation or timestamp regression")
    if run["lost_notifications"]:
        errors.append("positive PERF_RECORD_LOST (reported separately)")
    if boundary:
        for cpu in run["per_cpu"]:
            b = cpu["sites"][1]
            if b["produced"] != b["successes"]:
                errors.append(f"CPU {cpu['cpu']}: boundary fires != outputs")
            if cpu["max_boundary_gap_ns"] > MAX_GAP_NS or cpu["max_boundary_stamp_gap_ns"] > MAX_GAP_NS:
                errors.append(f"CPU {cpu['cpu']}: boundary gap exceeds 100 ms")
    return errors


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
    if not perf["attempts"] or not shared["attempts"] or not shared["busy_cpu_ns"] or not perf["busy_cpu_ns"]:
        return None
    return (perf["busy_cpu_ns"] / perf["attempts"]) / (shared["busy_cpu_ns"] / shared["attempts"])


def idle_increment(boundary, perf, cpus):
    return (boundary["busy_cpu_ns"] / boundary["elapsed"] -
            perf["busy_cpu_ns"] / perf["elapsed"]) / 1e9 / cpus


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
    errors = [f"requested {k}={v!r}, observed {run.get(k)!r}" for k, v in expected.items() if run.get(k) != v]
    if [c["cpu"] for c in run["per_cpu"]] != list(cpu_ids[:cpus]):
        errors.append("CPU identities differ from requested online affinity subset")
    if sum(run["buffer_bytes"]) != total:
        errors.append("buffer capacities do not sum to requested total")
    return errors


def run_one(out, label, args):
    command = [str(HERE / "nmi_transport"), "--object", str(HERE / "transport.bpf.o")] + args
    timeout = int(args[args.index("--duration") + 1]) + 120
    try:
        p = subprocess.run(command, cwd=ROOT, universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout, check=False)
        result = {"command": command, "exit": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
    except subprocess.TimeoutExpired as exc:
        # Keep partial diagnostics and refuse a hung consumer/attachment.
        result = {"command": command, "exit": "TIMEOUT", "timeout_seconds": timeout,
                  "stdout": (exc.stdout or b"").decode(errors="replace"),
                  "stderr": (exc.stderr or b"").decode(errors="replace")}
    # Save raw stdout, stderr, exit and exact command even if JSON is malformed.
    write_json(out / (label + ".raw.json"), result)
    if result["exit"]:
        raise RuntimeError(f"{label}: command failed with exit {result['exit']}")
    run = json.loads(result["stdout"])
    if not isinstance(run, dict):
        raise ValueError(f"{label}: expected one JSON object")
    errors = requested_errors(run, args, sorted(os.sched_getaffinity(0)))
    if errors:
        raise RuntimeError(label + ": " + "; ".join(errors))
    return run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=positive, default=3)
    parser.add_argument("--seconds", type=positive, default=30)
    parser.add_argument("--period-ns", type=positive, default=10_000_000)
    parser.add_argument("--bytes", type=positive, default=67_108_864)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("Linux only; never run BPF on macOS")
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error("--out must be outside the repository")
    out.mkdir(parents=True, exist_ok=False)
    try:
        build = capture(["make", "-C", str(HERE.parent), "bench-nmi-transport"])
        write_json(out / "build.json", build)
        if build["exit"]:
            raise RuntimeError("measurement build failed")
        info = metadata(out)
        info["protocol"] = dict(vars(args), out=str(out))
        info["claim"] = "measurement only; kernel argument and reviewer judgment still required"
        write_json(out / "metadata.json", info)
    except (OSError, ValueError, RuntimeError) as exc:
        write_json(out / "VOID.json", {"status": "VOID", "error": str(exc)})
        return 1
    available = len(info["allowed_cpus"])
    any_failure = False
    summaries = []
    # Keep the named 4 and all populations even on a four-CPU box: these are
    # distinct scheduled cells, never silently deduplicated or skipped.
    for population, cpus in (("1", 1), ("4", 4), ("all", available)):
        for case in CASES:
            for repeat in range(1, args.repeats + 1):
                label = f"cpu-{population}-{case}-r{repeat}"
                summary = {"cell": label, "cpus": cpus, "case": case, "repeat": repeat,
                           "metadata": "metadata.json", "runs": {}, "errors": [], "status": "VOID"}
                try:
                    if cpus > available:
                        raise RuntimeError(f"required {cpus}-CPU cell unavailable; only {available} online allowed")
                    common = ["--cpus", str(cpus), "--bytes", str(args.bytes)]
                    # Controls are fresh for EVERY quartet, same binary/object,
                    # participating CPU set, invocation and output directory.
                    controls = common + ["--duration", "2", "--transport", "perf", "--case", "all"]
                    loss = run_one(out, label + "-loss-control", controls + ["--one-page", "--pause-drain"])
                    mutant = run_one(out, label + "-seal-control", controls + ["--period-ns", "100000", "--mutant-boundary"])
                    summary["controls"] = {"loss": loss, "seal": mutant}
                    errors = control_errors(loss, mutant, cpus)
                    if errors:
                        raise RuntimeError("control VOID: " + "; ".join(errors))
                    # Rotate the quartet to interleave paired arms both ways.
                    rotation = (repeat - 1) % len(ARMS)
                    order = ARMS[rotation:] + ARMS[:rotation]
                    summary["arm_order"] = order
                    for arm in order:
                        command = common + ["--case", case, "--duration", str(args.seconds),
                                            "--transport", "shared" if arm == "shared" else "perf"]
                        if arm in ("perf_boundary", "adversarial"):
                            command += ["--period-ns", str(args.period_ns if arm == "perf_boundary" else 100_000)]
                        run = run_one(out, label + "-" + arm, command)
                        summary["runs"][arm] = run
                        summary["errors"].extend(arm + ": " + e for e in
                                                 normal_errors(run, cpus, case, arm in ("perf_boundary", "adversarial")))
                    runs = summary["runs"]
                    ratio = cpu_cost_ratio(runs["perf"], runs["shared"])
                    summary["perf_shared_cpu_ns_per_attempt_ratio"] = ratio
                    if case != "idle" and (ratio is None or not math.isfinite(ratio) or ratio > 1.10):
                        summary["errors"].append("paired perf/shared CPU-ns-per-attempt exceeds 1.10 or unavailable")
                    if case == "idle":
                        deltas = {arm: idle_increment(runs[arm], runs["perf"], cpus)
                                  for arm in ("perf_boundary", "adversarial")}
                        summary["boundary_increment_core_fraction_per_cpu"] = deltas
                        for arm, delta in deltas.items():
                            if delta > 0.01:
                                summary["errors"].append(arm + ": boundary-only idle-box incremental CPU exceeds 1% per CPU")
                    summary["status"] = "FAIL" if summary["errors"] else "PASS"
                except (OSError, ValueError, KeyError, TypeError, RuntimeError, ZeroDivisionError) as exc:
                    summary["errors"].append(str(exc))
                write_json(out / (label + ".summary.json"), summary)
                summaries.append({"cell": label, "status": summary["status"]})
                print(label + ": " + summary["status"], flush=True)
                any_failure |= summary["status"] != "PASS"
    write_json(out / "summary.json", {"status": "FAIL" if any_failure else "PASS", "cells": summaries,
                                     "qualification_claim": False})
    return int(any_failure)


if __name__ == "__main__":
    sys.exit(main())
