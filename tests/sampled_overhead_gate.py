#!/usr/bin/env python3
"""Paired A/B regression gate for the always-on sampled capture tier.

This is intentionally a coarse catastrophic-regression gate, not a claim that
shared-runner timing can prove the sampled tier's sub-1% overhead target.  Each
pair runs no-tracer and sampled measurements back-to-back, alternates AB/BA
order, and gates on the median paired TPS loss.  Standard read-only and
read-write pgbench are joined by a mandatory many-query-shape workload because
ordinary pgbench cannot expose pg_stat_statements resolver scan storms.

Run as root against a dedicated PostgreSQL port/postmaster.  The script creates
and removes one dedicated database and never restarts or reconfigures PostgreSQL.
Use --characterize to report data without applying the timing threshold.
"""

import argparse
import fcntl
import json
import math
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parent.parent
TRACER = PROJECT_DIR / "pg_wait_tracer"
DEFAULT_WORKLOADS = ("ro", "rw", "high-cardinality")
TPS_RE = re.compile(r"^tps = ([0-9.]+) ", re.MULTILINE)


class GateError(RuntimeError):
    pass


def run(argv, *, env=None, timeout=120, check=True, capture=True):
    result = subprocess.run(
        [str(v) for v in argv], env=env, timeout=timeout, check=False,
        text=True, stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None)
    if check and result.returncode:
        detail = ((result.stderr or "") + (result.stdout or ""))[-2000:]
        shown = " ".join(map(str, argv[:24]))
        if len(argv) > 24:
            shown += f" ... ({len(argv) - 24} more arguments)"
        raise GateError(f"command failed ({result.returncode}): "
                        f"{shown}\n{detail}")
    return result


def psql(args, sql, *, database="postgres", tuples=True, timeout=30):
    argv = [args.psql, "-X", "-v", "ON_ERROR_STOP=1", "-h", args.host,
            "-p", str(args.port), "-U", args.user, "-d", database]
    if tuples:
        argv += ["-tA"]
    argv += ["-c", sql]
    return run(argv, timeout=timeout).stdout.strip()


def quote_ident(value):
    return '"' + value.replace('"', '""') + '"'


def wait_for_socket(proc, trace_dir, timeout=12):
    path = trace_dir / "pgwt.sock"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if proc.poll() is not None:
            _, stderr = proc.communicate()
            raise GateError("tracer exited before control socket: " +
                            stderr.decode(errors="replace")[-2000:])
        time.sleep(0.05)
    raise GateError("tracer control socket did not appear")


def control(trace_dir, command):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(5)
    try:
        client.connect(str(trace_dir / "pgwt.sock"))
        client.sendall((json.dumps({"cmd": command}) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        if not data:
            raise GateError("empty tracer control response")
        response = json.loads(data.splitlines()[0])
        if not response.get("ok"):
            raise GateError(f"tracer control error: {response}")
        return response
    finally:
        client.close()


def stop_tracer(proc, trace_dir):
    stderr = b""
    if proc is not None:
        if proc.poll() is None:
            proc.terminate()
        try:
            _, stderr = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, stderr = proc.communicate(timeout=5)
    shutil.rmtree(trace_dir, ignore_errors=True)
    return stderr.decode(errors="replace")


def high_cardinality_scripts(root, count):
    """Create cheap but structurally distinct point-query pgbench scripts."""
    expressions = (
        "aid + 0", "aid - 0", "aid * 1", "aid / 1",
        "aid % 2147483647", "aid & 2147483647", "aid | 0", "aid # 0",
        "abs(aid)", "greatest(aid, 0)",
    )
    if count > (1 << len(expressions)):
        raise GateError(f"high-cardinality shapes must be <= {1 << len(expressions)}")
    paths = []
    shapes_per_script = 4
    for first in range(0, count, shapes_per_script):
        group = range(first, min(first + shapes_per_script, count))
        lines = ["\\set aid random(1, 100000 * :scale)",
                 f"\\set variant random(0, {len(group) - 1})"]
        for variant, shape in enumerate(group):
            lines.append(("\\if" if variant == 0 else "\\elif") +
                         f" :variant = {variant}")
            targets = ["abalance"]
            targets += [expr for bit, expr in enumerate(expressions)
                        if shape & (1 << bit)]
            lines.append(f"SELECT {', '.join(targets)} "
                         "FROM pgbench_accounts WHERE aid = :aid;")
        lines.append("\\endif")
        path = root / f"shapes-{first:04d}.sql"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        paths.append(path)
    return paths


def workload_argv(args, workload, scripts):
    argv = [args.pgbench, "-n", "-h", args.host, "-p", str(args.port),
            "-U", args.user, "-d", args.database, "-c", str(args.clients),
            "-j", str(args.jobs)]
    if workload == "ro":
        argv.append("-S")
    elif workload == "high-cardinality":
        for path in scripts:
            argv += ["-f", path]
    return argv


def reset_pgss(args):
    # Three-argument form is available on every supported PG13+ version and
    # scopes the reset to this invocation's database.  A zero-argument reset
    # would erase statistics for unrelated databases on a shared box.
    psql(args, "SELECT pg_stat_statements_reset("
          "0, (SELECT oid FROM pg_database "
          "WHERE datname = current_database()), 0)",
          database=args.database)


def run_pgbench(args, workload, scripts, duration, *, warmup=False):
    argv = workload_argv(args, workload, scripts) + ["-T", str(duration)]
    result = run(argv, timeout=duration + 90)
    values = TPS_RE.findall(result.stdout)
    if not values:
        raise GateError(f"pgbench produced no TPS for {workload}: "
                        f"{(result.stderr + result.stdout)[-2000:]}")
    return float(values[-1])


def distinct_shapes(args):
    sql = """
        SELECT count(*)
          FROM pg_stat_statements
         WHERE dbid = (SELECT oid FROM pg_database
                        WHERE datname = current_database())
           AND query LIKE 'SELECT %pgbench_accounts%aid = $%'
    """
    value = psql(args, sql, database=args.database)
    return int(value or 0)


def measured_run(args, workload, scripts, sampled):
    reset_pgss(args)
    proc = None
    trace_dir = Path(tempfile.mkdtemp(prefix="pgwt_sampled_overhead_"))
    metrics = None
    stderr = ""
    try:
        if sampled:
            env = os.environ.copy()
            for item in args.tracer_env:
                name, sep, value = item.partition("=")
                if not sep or not name.startswith("PGWT_TEST_"):
                    raise GateError("--tracer-env requires PGWT_TEST_NAME=value")
                env[name] = value
            proc = subprocess.Popen(
                [str(TRACER), "--mode", "sampled", "--pid", str(args.pid),
                 "--trace-dir", str(trace_dir), "--duration",
                 str(args.warmup + args.duration + 30), "--interval", "1",
                 "--quiet"], stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, env=env)
            wait_for_socket(proc, trace_dir)
        if args.warmup:
            run_pgbench(args, workload, scripts, args.warmup, warmup=True)
        tps = run_pgbench(args, workload, scripts, args.duration)
        shapes = distinct_shapes(args) if workload == "high-cardinality" else 0
        if sampled:
            metrics = control(trace_dir, "metrics")
            metrics.update(control(trace_dir, "status"))
        return tps, metrics, shapes
    finally:
        stderr = stop_tracer(proc, trace_dir)
        if metrics is not None:
            metrics["tracer_stderr_tail"] = stderr[-4000:]
        if proc is not None and proc.returncode not in (0, -15):
            raise GateError(f"tracer failed ({proc.returncode}): {stderr[-2000:]}")


def percentile(values, fraction):
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    point = fraction * (len(ordered) - 1)
    lo = math.floor(point)
    hi = math.ceil(point)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (point - lo)


def summarize(values):
    med = statistics.median(values)
    q1 = percentile(values, 0.25)
    q3 = percentile(values, 0.75)
    return {
        "median_pct": med,
        "q1_pct": q1,
        "q3_pct": q3,
        "iqr_pct": q3 - q1,
        "min_pct": min(values),
        "max_pct": max(values),
        "prefix_medians_pct": [statistics.median(values[:n])
                                for n in range(3, len(values) + 1)],
    }


def structural_errors(args, workload, metrics, shapes):
    errors = []
    if not metrics:
        return ["sampled run returned no metrics"]
    validation = metrics.get("pgbackend_layout_validation")
    if validation != "validated":
        errors.append(f"layout is {validation!r}, expected 'validated'")
    if int(metrics.get("pg_pid", -1)) != args.pid:
        errors.append(f"tracer reports postmaster {metrics.get('pg_pid')}, "
                      f"expected {args.pid}")
    if metrics.get("mode") != "sampled" or metrics.get("tier") != "sampled":
        errors.append(f"tracer is not pure sampled mode/tier: "
                      f"{metrics.get('mode')}/{metrics.get('tier')}")
    if metrics.get("sampler_healthy") is not True:
        errors.append("sampled provider reports unhealthy reads")
    if metrics.get("sampled_attr_source") != "pgbackend_status":
        errors.append(f"sampled attribution source is "
                      f"{metrics.get('sampled_attr_source')!r}")
    if int(metrics.get("samples_total", 0)) <= 0:
        errors.append("sampled provider produced zero samples")
    qfires = int(metrics.get("exact_query_uprobe_fires_total", -1))
    afires = int(metrics.get("exact_activity_uprobe_fires_total", -1))
    mask = int(metrics.get("exact_probe_attached_mask", -1))
    if (qfires, afires, mask) != (0, 0, 0):
        errors.append(f"sampled exact probes fired/attached: "
                      f"query={qfires} activity={afires} mask={mask}")
    if workload == "high-cardinality":
        if shapes < args.min_distinct_shapes:
            errors.append(f"only {shapes} distinct query shapes; "
                          f"need >= {args.min_distinct_shapes}")
        required = ("sampled_text_pgss_scans_total",
                    "sampled_text_resolved_total")
        missing = [name for name in required if name not in metrics]
        if missing:
            errors.append(f"missing mandatory resolver metrics: {missing}")
        else:
            scans = int(metrics["sampled_text_pgss_scans_total"])
            resolved = int(metrics["sampled_text_resolved_total"])
            uptime = float(metrics.get("uptime_s", 0))
            allowed = math.ceil(uptime * args.max_pgss_scans_per_sec) + 1
            if scans <= 0 or resolved <= 0:
                errors.append(f"pgss resolver was not exercised: "
                              f"scans={scans} resolved={resolved}")
            if scans > allowed:
                errors.append(f"pgss scan rate unbounded: {scans} scans in "
                              f"{uptime:.2f}s (allowed {allowed})")
    stderr = metrics.get("tracer_stderr_tail", "")
    hooks = {
        "PGWT_TEST_SAMPLED_UPROBES": "restoring always-on sampled attribution traps",
        "PGWT_TEST_PGSS_UNTHROTTLED": "restoring pre-throttle 32-key pgss scans",
    }
    enabled = {item.partition("=")[0] for item in args.tracer_env}
    for name, marker in hooks.items():
        if name in enabled and marker not in stderr:
            errors.append(f"injected hook {name} did not report activation")
    return errors


def run_pair(args, workload, scripts, rep):
    order = (False, True) if rep % 2 else (True, False)
    values = {}
    sampled_metrics = None
    shapes = 0
    for sampled in order:
        tps, metrics, observed_shapes = measured_run(
            args, workload, scripts, sampled)
        values[sampled] = tps
        if sampled:
            sampled_metrics = metrics
            shapes = observed_shapes
    overhead = 100.0 * (1.0 - values[True] / values[False])
    pair = {
        "pair": rep,
        "order": "AB" if order[0] is False else "BA",
        "baseline_tps": values[False],
        "sampled_tps": values[True],
        "overhead_pct": overhead,
        "distinct_shapes": shapes,
        "metrics": sampled_metrics,
    }
    errors = structural_errors(args, workload, sampled_metrics, shapes)
    return pair, errors


def setup_database(args):
    version = int(psql(args, "SHOW server_version_num")) // 10000
    port = int(psql(args, "SHOW port"))
    if port != args.port:
        raise GateError(f"connected server reports port {port}, expected {args.port}")
    if args.pg_version and version != args.pg_version:
        raise GateError(f"connected PG{version}, expected PG{args.pg_version}")
    data_dir = Path(psql(args, "SHOW data_directory"))
    try:
        server_pid = int((data_dir / "postmaster.pid").read_text(
            encoding="utf-8").splitlines()[0])
    except (OSError, ValueError, IndexError) as exc:
        raise GateError(f"cannot validate postmaster.pid in {data_dir}") from exc
    if server_pid != args.pid:
        raise GateError(f"port {port} belongs to postmaster {server_pid}, "
                        f"not requested PID {args.pid}")
    exists = psql(args, "SELECT 1 FROM pg_database WHERE datname = "
                  + "'" + args.database.replace("'", "''") + "'")
    if exists:
        raise GateError(f"refusing to replace pre-existing database "
                        f"{args.database!r}")
    psql(args, f"CREATE DATABASE {quote_ident(args.database)}", tuples=False)
    args.database_created = True
    psql(args, "CREATE EXTENSION pg_stat_statements", database=args.database,
         tuples=False)
    run([args.pgbench, "-i", "-q", "-s", str(args.scale), "-h", args.host,
         "-p", str(args.port), "-U", args.user, "-d", args.database],
        timeout=300)
    return version


def drop_database(args):
    psql(args, f"DROP DATABASE {quote_ident(args.database)} WITH (FORCE)",
         tuples=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--pg-version", type=int, default=0)
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--host", default="/var/run/postgresql")
    parser.add_argument("--user", default="postgres")
    parser.add_argument("--database",
                        help="new dedicated database name (default: run-unique)")
    parser.add_argument("--psql", default="psql")
    parser.add_argument("--pgbench", default="pgbench")
    parser.add_argument("--reps", type=int, default=7)
    parser.add_argument("--confirm-reps", type=int, default=7,
                        help="extra pairs after a candidate hard failure")
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--scale", type=int, default=10)
    parser.add_argument("--high-cardinality-shapes", type=int, default=256)
    parser.add_argument("--min-distinct-shapes", type=int, default=0,
                        help="required observed shapes (default: 90%% of corpus)")
    parser.add_argument("--max-overhead-pct", type=float, default=15.0,
                        help="hard-fail paired-median TPS loss")
    parser.add_argument("--warn-overhead-pct", type=float, default=10.0,
                        help="nonblocking paired-median warning band")
    parser.add_argument("--max-pgss-scans-per-sec", type=float, default=1.0)
    parser.add_argument("--workloads", nargs="+", choices=DEFAULT_WORKLOADS,
                        default=list(DEFAULT_WORKLOADS))
    parser.add_argument("--characterize", action="store_true")
    parser.add_argument("--keep-database", action="store_true")
    parser.add_argument("--output-json")
    parser.add_argument("--tracer-env", action="append", default=[])
    args = parser.parse_args()

    if args.database is None:
        args.database = f"pgwt_sampled_overhead_{args.pid}_{os.getpid()}"
    args.database_created = False
    if args.min_distinct_shapes == 0:
        args.min_distinct_shapes = math.ceil(
            args.high_cardinality_shapes * 0.9)

    if os.geteuid() != 0:
        raise GateError("must run as root")
    if not TRACER.is_file() or not os.access(TRACER, os.X_OK):
        raise GateError(f"build first: {TRACER} is not executable")
    if (args.reps < 3 or args.confirm_reps < 0 or args.duration < 1 or
            args.warmup < 0):
        raise GateError("need >=3 reps, nonnegative confirmation/warmup, "
                        "and positive duration")
    if (args.warn_overhead_pct >= args.max_overhead_pct or
            args.max_pgss_scans_per_sec <= 0):
        raise GateError("warning threshold must be below the hard threshold "
                        "and the pgss scan-rate limit must be positive")

    lock_path = f"/tmp/pgwt_sampled_overhead_gate_{args.port}.lock"
    with open(lock_path, "w", encoding="utf-8") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise GateError(f"another gate owns PostgreSQL port {args.port}") from exc

        workdir = Path(tempfile.mkdtemp(prefix="pgwt_overhead_workload_"))
        scripts = high_cardinality_scripts(
            workdir, args.high_cardinality_shapes)
        report = {"config": dict(vars(args)), "workloads": {},
                  "structural_failures": [], "timing_failures": [],
                  "timing_warnings": []}
        failed = False
        try:
            version = setup_database(args)
            report["postgres_major"] = version
            print(f"sampled-overhead: PG{version} port={args.port} "
                  f"pairs={args.reps}+{args.confirm_reps}-on-fail "
                  f"duration={args.duration}s "
                  f"clients/jobs={args.clients}/{args.jobs}")
            for workload in args.workloads:
                pairs = []
                for rep in range(1, args.reps + 1):
                    pair, structural = run_pair(
                        args, workload, scripts, rep)
                    report["structural_failures"] += [
                        f"{workload} pair {rep}: {error}" for error in structural]
                    failed |= bool(structural)
                    pairs.append(pair)
                    print(f"  {workload:16s} pair={rep} "
                          f"order={pair['order']} "
                          f"overhead={pair['overhead_pct']:+.2f}%")
                primary_stats = summarize(
                    [pair["overhead_pct"] for pair in pairs])
                confirmation_triggered = (
                    not args.characterize and args.confirm_reps > 0 and
                    primary_stats["median_pct"] >= args.max_overhead_pct)
                if confirmation_triggered:
                    print(f"  {workload:16s} candidate hard failure "
                          f"({primary_stats['median_pct']:+.2f}%); "
                          f"running {args.confirm_reps} confirmation pairs")
                    first = args.reps + 1
                    for rep in range(first, first + args.confirm_reps):
                        pair, structural = run_pair(
                            args, workload, scripts, rep)
                        report["structural_failures"] += [
                            f"{workload} pair {rep}: {error}"
                            for error in structural]
                        failed |= bool(structural)
                        pairs.append(pair)
                        print(f"  {workload:16s} pair={rep} "
                              f"order={pair['order']} "
                              f"overhead={pair['overhead_pct']:+.2f}%")
                stats = summarize(
                    [pair["overhead_pct"] for pair in pairs])
                report["workloads"][workload] = {
                    "pairs": pairs,
                    "primary_reps": args.reps,
                    "confirmation_reps_run": len(pairs) - args.reps,
                    "confirmation_triggered": confirmation_triggered,
                    "primary_stats": primary_stats,
                    **stats,
                }
                verdict = "PASS"
                if stats["median_pct"] >= args.max_overhead_pct:
                    verdict = "FAIL"
                    report["timing_failures"].append(
                        f"{workload}: median {stats['median_pct']:.2f}% >= "
                        f"{args.max_overhead_pct:.2f}%")
                    if not args.characterize:
                        failed = True
                elif stats["median_pct"] >= args.warn_overhead_pct:
                    verdict = "WARN"
                    report["timing_warnings"].append(
                        f"{workload}: median {stats['median_pct']:.2f}% >= "
                        f"{args.warn_overhead_pct:.2f}%")
                print(f"  {workload:16s} median={stats['median_pct']:+.2f}% "
                      f"IQR={stats['iqr_pct']:.2f}pp "
                      f"range=[{stats['min_pct']:+.2f},{stats['max_pct']:+.2f}] "
                      f"{verdict if not args.characterize else 'CHARACTERIZE'}")
        finally:
            active_error = sys.exc_info()[0] is not None
            try:
                if args.database_created and not args.keep_database:
                    drop_database(args)
            except Exception as cleanup_error:
                if active_error:
                    print(f"sampled-overhead: cleanup also failed: "
                          f"{cleanup_error}", file=sys.stderr)
                else:
                    raise
            finally:
                shutil.rmtree(workdir, ignore_errors=True)

        report["verdict"] = "fail" if failed else "pass"
        if args.output_json:
            Path(args.output_json).write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        for error in report["structural_failures"]:
            print(f"  STRUCTURAL FAIL: {error}")
        for warning in report["timing_warnings"]:
            print(f"  TIMING WARN: {warning}")
            if os.environ.get("GITHUB_ACTIONS"):
                print(f"::warning title=Sampled overhead::{warning}")
        if failed:
            print("sampled-overhead: FAIL")
            return 1
        print("sampled-overhead: PASS")
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (GateError, OSError, ValueError) as exc:
        print(f"sampled-overhead: ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
