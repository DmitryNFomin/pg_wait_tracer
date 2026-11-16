"""Fail-closed parser/assertions for the status-row batch live test.

The live test counts syscalls from the summary that `perf trace -S` prints at
its end (`parse_summary` / `assess_counts`): it needs no path names, which
neither the Ubuntu 24.04 (perf 6.8) nor the EL8 (perf 4.18, no BPF skeletons)
image can resolve (`filename: 0x...`). `parse_trace` / `assess` are the older
form that read per-event text lines and path names; they are kept unchanged
because tests/test_sampled_attr_io.py still exercises them, and the live test
no longer calls them.
No product-derived expected values: the syscall bound is the accepted design.

The PG13 far-row arm uses explicit readv entry/exit tracepoints and a symbol
entry tick marker instead of perf trace's syscall pretty-printer or stacks.
Only closed tick intervals are qualified; any loss anywhere invalidates a run.
"""
import re
from bisect import bisect_left, bisect_right

CALLS = "open,openat,pread64,close,process_vm_readv"
HEADER = re.compile(r"^\s*(\d+\.\d+)\s+\([^)]*\):\s*(?:(\d+)\s+)?(.*)$")
CALL = re.compile(r"\b(open|openat|pread64|close|process_vm_readv)\((.*)\)\s*(.*)$")
MEM = re.compile(r"/proc/(?:\d+|self)/mem\b")
SUPPORTED_MAJORS = (13, 17, 18)


def parse_trace(text, start_ms, end_ms, initial_fds=None):
    if re.search(r"\bLOST\s+[1-9]\d*\s+events|lost\s+[1-9]\d*", text, re.I):
        raise AssertionError("perf lost events; syscall absence is unproven")
    counts = dict.fromkeys(CALLS.split(","), 0)
    counts.update(mem_open=0, mem_pread=0, mem_close=0)
    fds = dict(initial_fds or {})
    pending = {}
    seen = 0
    for line in text.splitlines():
        header = HEADER.match(line)
        if not header:
            if any(f"{name}(" in line for name in counts):
                raise AssertionError(f"unparseable perf syscall line: {line}")
            continue
        timestamp, tid, body = header.groups()
        timestamp = float(timestamp)
        call = CALL.search(body)
        if not call:
            continue
        name, args, tail = call.groups()
        key = (tid, name)
        in_window = start_ms <= timestamp <= end_ms
        if tail.strip() == "...":
            pending[key] = (timestamp, args)
            continue
        if not tail.lstrip().startswith("="):
            if in_window:
                raise AssertionError(f"incomplete perf syscall: {line}")
            continue
        if "[continued]" in body:
            original = pending.pop(key, None)
            if original is None:
                if in_window:
                    raise AssertionError("continued syscall lacks its entry")
                continue
            timestamp, args = original
            in_window = start_ms <= timestamp <= end_ms
        result = re.match(r"=\s*(-?\d+)", tail.lstrip())
        # perf prints successful open as `= 7` and errors as `= -1 E...`.
        if result is None:
            if in_window:
                raise AssertionError(f"unparseable perf result: {line}")
            continue
        ret = int(result[1])
        fd_match = re.search(r"\bfd:\s*(\d+)", args)
        fd = int(fd_match[1]) if fd_match else None
        if in_window:
            seen += 1
            counts[name] += 1
        if name in ("open", "openat"):
            filename = re.search(r"\bfilename:\s*(.*?)(?:,\s*\w+:|$)", args)
            path = filename[1].strip().strip('"') if filename else ""
            if in_window and (not path or re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", path)):
                raise AssertionError("perf did not resolve an open pathname")
            if path == "mem" and re.search(r"dfd:.*?/proc/\d+", args):
                path = "/proc/self/mem"
            if in_window and MEM.search(path):
                counts["mem_open"] += 1
            if ret >= 0:
                fds[ret] = path
        elif name in ("pread64", "close"):
            if fd is None and in_window:
                raise AssertionError("perf did not decode a file descriptor")
            path = fds.get(fd, "")
            if in_window and (MEM.search(args) or MEM.search(path)):
                counts["mem_pread" if name == "pread64" else "mem_close"] += 1
            if name == "close" and ret == 0:
                fds.pop(fd, None)
    if not seen or counts["process_vm_readv"] == 0:
        raise AssertionError("perf observed no process_vm_readv in the measured window")
    if any(start_ms <= timestamp <= end_ms for timestamp, _ in pending.values()):
        raise AssertionError("unfinished perf syscall in measured window")
    return counts


def assess(counts, major, population, ticks, read_failures, idle_samples):
    if major not in SUPPORTED_MAJORS:
        raise ValueError(f"PG{major} is outside the status-batch qualification matrix")
    assert population >= 504 and ticks >= 200
    assert read_failures >= 0 and idle_samples >= 0
    # One wait-word batch, at most two torn-row retries and one gate recheck.
    ranges = 4 if major == 13 else 3
    row_calls = (ranges * population + 1023) // 1024
    bound = row_calls + 3
    minimum = row_calls + 1  # status rows in addition to the wait-word call
    failure_rate = read_failures / (population * ticks)
    errors = []
    for key in ("mem_open", "mem_pread", "mem_close"):
        if counts[key] != 0:
            errors.append(f"{key}={counts[key]} (required zero)")
    # Even if filename augmentation fails to label an old cached fd, no
    # per-backend pread is allowed to evade the memory-file assertion.
    if counts["pread64"] != 0:
        errors.append(f"pread64={counts['pread64']} (required zero in steady state)")
    if counts["process_vm_readv"] > bound * ticks:
        errors.append(f"vm/tick={counts['process_vm_readv']/ticks:.3f} > {bound}")
    if counts["process_vm_readv"] < minimum * ticks:
        errors.append(f"vm/tick={counts['process_vm_readv']/ticks:.3f} < {minimum}")
    if idle_samples == 0:
        errors.append("idle attributed samples=0 (required positive)")
    # At 504+ targets this permits transient tears but no structurally unread
    # fixed population. Bound decided here, not fitted from box output.
    if failure_rate > 0.001:
        errors.append(f"read-failure rate={failure_rate:.6f} > 0.001")
    return {"ranges_per_row": ranges, "row_calls_per_tick": row_calls,
            "vm_calls_per_tick_minimum": minimum, "idle_attributed_samples": idle_samples,
            "vm_calls_per_tick_bound": bound, "read_failure_rate": failure_rate,
            "read_failure_rate_bound": 0.001, "errors": errors}


# ---------------------------------------------------------------------------
# Summary form (what the live test uses).
# ---------------------------------------------------------------------------
# Per-backend file I/O (open, pread, close) per sampler tick. The steady state
# of the batched build is a few calls per tick (wait-address resolution and the
# CPU-capacity refresh: a constant, independent of the number of backends);
# master makes one of each per backend per tick (about 500 opens and 4000
# preads at 504 targets). 64 is far from both and does not scale with N.
FILE_IO_PER_TICK_BOUND = 64
FILE_IO_CALLS = ("open", "openat", "pread64", "close")
SUMMARY_HEADER = re.compile(r"^\s*Summary of events:\s*$", re.M)
SUMMARY_COLUMNS = re.compile(r"^\s*syscall\s+calls\s+errors\b", re.M)
SUMMARY_ROW = re.compile(r"^\s+([a-z_][a-z0-9_]*)\s+(\d+)\s+(\d+)\s+\d+(?:\.\d+)?\s+"
                         r"\d+(?:\.\d+)?\s+\d+(?:\.\d+)?\s+\d+(?:\.\d+)?\s+\d+(?:\.\d+)?%\s*$")
LOST = re.compile(r"\bLOST\s+[1-9]\d*\s+events|\blost\s+[1-9]\d*", re.I)


def parse_summary(text, extra_streams=()):
    """Total calls by syscall name over every thread in a `perf trace -S/-s` summary.

    Fail closed: no summary, no column header, an unparsable row for a counted
    syscall, or lost events (`extra_streams` carries perf's stderr) all raise.
    """
    for stream in (text, *extra_streams):
        if LOST.search(stream):
            raise AssertionError("perf lost events; syscall absence is unproven")
    header = SUMMARY_HEADER.search(text)
    if not header:
        raise AssertionError("perf printed no syscall summary")
    summary = text[header.end():]
    if not SUMMARY_COLUMNS.search(summary):
        raise AssertionError("perf summary has no syscall/calls/errors table")
    counts = dict.fromkeys(CALLS.split(","), 0)
    threads = 0
    for line in summary.splitlines():
        if re.match(r"^\s*\S.*\(\d+\),\s+\d+ events", line):
            threads += 1
            continue
        first = line.split()[:1]
        if not first or first[0] not in counts:
            continue
        row = SUMMARY_ROW.match(line)
        if not row:
            raise AssertionError(f"unparseable perf summary row: {line}")
        counts[row[1]] += int(row[2])
    if not threads:
        raise AssertionError("perf summary names no traced thread")
    counts["threads"] = threads
    return counts


def assess_counts(counts, major, population, ticks_inside, ticks_spanning,
                  read_failures, idle_samples):
    """Assertions over summary counts.

    `ticks_inside` are sampler ticks that certainly ran inside perf's window,
    `ticks_spanning` ticks of a window that certainly contains it. Each bound
    divides by the one of them that cannot create a false RED: the upper
    bounds by the larger count, the lower bound by the smaller.
    """
    if major not in SUPPORTED_MAJORS:
        raise ValueError(f"PG{major} is outside the status-batch qualification matrix")
    assert population >= 504 and ticks_inside >= 200 and ticks_spanning >= ticks_inside
    assert read_failures >= 0 and idle_samples >= 0
    # One wait-word batch, at most two torn-row retries and one gate recheck.
    ranges = 4 if major == 13 else 3
    row_calls = (ranges * population + 1023) // 1024
    bound = row_calls + 3
    minimum = row_calls + 1  # status rows in addition to the wait-word call
    per_tick = {"openat": (counts["open"] + counts["openat"]) / ticks_spanning,
                "pread64": counts["pread64"] / ticks_spanning,
                "close": counts["close"] / ticks_spanning,
                "process_vm_readv_max": counts["process_vm_readv"] / ticks_spanning,
                "process_vm_readv_min": counts["process_vm_readv"] / ticks_inside}
    errors = []
    for name in ("openat", "pread64", "close"):
        if per_tick[name] > FILE_IO_PER_TICK_BOUND:
            errors.append(f"{name}/tick={per_tick[name]:.1f} > {FILE_IO_PER_TICK_BOUND} "
                          "(per-backend file I/O must be absent in steady state)")
    if per_tick["process_vm_readv_max"] > bound:
        errors.append(f"vm/tick={per_tick['process_vm_readv_max']:.3f} > {bound}")
    if per_tick["process_vm_readv_min"] < minimum:
        errors.append(f"vm/tick={per_tick['process_vm_readv_min']:.3f} < {minimum}")
    if idle_samples == 0:
        errors.append("idle attributed samples=0 (required positive)")
    failure_rate = read_failures / (population * ticks_inside)
    # At 504+ targets this permits transient tears but no structurally unread
    # fixed population. Bound decided here, not fitted from box output.
    if failure_rate > 0.001:
        errors.append(f"read-failure rate={failure_rate:.6f} > 0.001")
    return {"ranges_per_row": ranges, "row_calls_per_tick": row_calls,
            "vm_calls_per_tick_minimum": minimum, "vm_calls_per_tick_bound": bound,
            "file_io_per_tick_bound": FILE_IO_PER_TICK_BOUND, "per_tick": per_tick,
            "idle_attributed_samples": idle_samples, "read_failure_rate": failure_rate,
            "read_failure_rate_bound": 0.001, "errors": errors}


def file_io_reds(errors):
    """The per-backend file I/O assertions among `assess_counts` errors."""
    return [e for e in errors if re.match(r"(?:openat|pread64|close)/tick=", e)]


def pg13_clock_bounds(alignment):
    """Independent realtime-minus-monotonic bounds, never fitted to samples.

    Each realtime read is bracketed by monotonic reads on the same host as
    PostgreSQL. Keep the entire observed offset envelope (including drift).
    Any resulting ambiguity about T fails closed.
    perf must explicitly select mono; the sample reader retains raw mono ns.
    """
    assert alignment and alignment.get("perf_clock") == "CLOCK_MONOTONIC", \
        "clock alignment unavailable: perf must explicitly use CLOCK_MONOTONIC"
    probes = alignment.get("probes", [])
    assert len(probes) >= 2, "clock alignment unavailable: need bracketing clock probes"
    lower, upper, previous = [], [], 0
    for probe in probes:
        before, wall, after = (probe[k] for k in ("mono_before_ns", "wall_ns", "mono_after_ns"))
        assert 0 < before <= after and before >= previous and wall > 0, \
            "clock alignment unavailable: invalid clock probe ordering"
        previous = after
        lower.append(wall - after)
        upper.append(wall - before)
    return min(lower), max(upper), probes[0]["mono_before_ns"], probes[-1]["mono_after_ns"]


def parse_attr_failures(text, before, after):
    """Join debug outcomes to the EXACT aggregate counter window, not log arrival."""
    assert type(before) is int and type(after) is int and 0 <= before <= after, \
        "churn read-failure counter unavailable or moved backwards"
    pattern = re.compile(r"SAMPLED-ATTR-FAILURE: pid=(\d+) incarnation=(\d+) "
                         r"tick_ns=(\d+) phase=(first|gate) read_begin_ns=(\d+) "
                         r"read_end_ns=(\d+) sequence=(\d+) error=(\d+)")
    records = []
    for line in text.splitlines():
        if "SAMPLED-ATTR-FAILURE:" not in line:
            continue
        match = pattern.fullmatch(line)
        assert match, f"malformed attribution failure diagnostic: {line}"
        keys = ("pid", "incarnation", "tick_ns", "phase", "read_begin_ns",
                "read_end_ns", "sequence", "error")
        row = {key: value if key == "phase" else int(value)
               for key, value in zip(keys, match.groups())}
        if before < row["sequence"] <= after:
            records.append(row)
    assert [r["sequence"] for r in records] == list(range(before + 1, after + 1)), \
        "failure diagnostics do not reconcile with churn counter window"
    return records


def assess_pg13_timing(cohort, samples, ticks, alignment, churn_read_failures,
                       *, failures=None, lifecycle=None, gate_read_failures=None):
    """E = SQL statement_timestamp(); T = first observed tick starting >= E.

    Every one of 200 backends needs a nonzero-query-id sample at T or earlier.
    No nominal period or elapsed-time allowance participates in this gate.
    Every failure needs its own pid/incarnation/tick/phase and exemption.
    Pre-T samples are diagnostic only; they never buy a failure allowance.
    A client-observed statement return is a conservative upper bound on end:
    only reads beginning at/after it (or an observed EXIT) are terminal.
    Samples keep their raw monotonic stamp, so rotated file anchors cannot
    move a sample into an earlier tick. The final marker closes the last tick.
    """
    assert len(cohort) == 200, "need 200 short-session starts"
    starts = {row["pid"]: row["statement_start_ns"] for row in cohort}
    assert len(starts) == 200 and all(pid > 0 and ts > 0 for pid, ts in starts.items())
    samples = list(samples)
    evidence_errors, attribution_errors, failure_errors = [], [], []
    first, eligible, pre_e, late, missing = {}, {}, set(), {}, []
    sample_ticks = {pid: set() for pid in starts}
    holes, classified, late_details = {}, [], []
    classes = dict.fromkeys(("pre_eligibility", "terminal", "eligible", "background",
                             "incarnation_mismatch", "unclassified"), 0)
    phases = dict.fromkeys(("first", "gate"), 0)
    cohort_by_pid = {row["pid"]: row for row in cohort}
    lifecycle_supplied = lifecycle is not None
    lifecycle = lifecycle or {"forks": [], "exits": []}
    try:
        low, high, probe_start, probe_end = pg13_clock_bounds(alignment)
        assert len(ticks) >= 2 and all(a < b for a, b in zip(ticks, ticks[1:])), \
            "clock alignment unavailable: need ordered closed tick intervals"
        assert probe_start <= ticks[0] < ticks[-1] <= probe_end, \
            "clock alignment unavailable: clock probes do not bracket perf ticks"
        for pid, wall in starts.items():
            e_low, e_high = wall - high, wall - low
            t = bisect_left(ticks, e_low)
            assert t == bisect_left(ticks, e_high), \
                f"clock alignment ambiguous at statement start: pid={pid}"
            assert 0 < t < len(ticks) - 1, f"statement start outside closed perf ticks: pid={pid}"
            eligible[pid] = t
            if lifecycle_supplied:
                forks = [r["timestamp_ns"] for r in lifecycle["forks"] if r["pid"] == pid]
                assert len(forks) == 1 and ticks[0] <= forks[0] <= e_low, \
                    f"missing, reused or out-of-window fork evidence: pid={pid}"
        for row in samples:
            pid = row["pid"]
            if pid not in starts:
                continue
            assert "monotonic_ns" in row, "clock alignment unavailable: sample lacks raw monotonic timestamp"
            ts = row["monotonic_ns"]
            tick = bisect_right(ticks, ts) - 1
            assert 0 <= tick < len(ticks) - 1, f"sample outside closed perf ticks: pid={pid}"
            # The perf marker defines a tick's start. A sample stamped a few
            # instructions after that marker still belongs to that same tick,
            # including when E falls between marker and sample timestamp.
            sample_ticks[pid].add(tick)
            if tick < eligible[pid]:
                pre_e.add((pid, tick))
            if row["query_id"]:
                first[pid] = min(first.get(pid, tick), tick)
    except (AssertionError, KeyError, TypeError) as error:
        evidence_errors.append(str(error))
    # Partial/ambiguous evidence cannot be accepted as a negative control.
    if not evidence_errors:
        missing = sorted(starts.keys() - first.keys())
        late = {pid: first[pid] - eligible[pid] for pid in first if first[pid] > eligible[pid]}
        if missing:
            attribution_errors.append(f"first attribution missing: pids={missing}")
        if late:
            attribution_errors.append(f"first attribution late: extra observed ticks={late} (required <= 0)")
        for pid, seen in sample_ticks.items():
            if seen:
                absent = sorted(set(range(min(seen), max(seen) + 1)) - seen)
                if absent:
                    holes[pid] = absent
        if holes:
            failure_errors.append(f"cohort sample holes: {holes}")
        # Failure diagnostics are independently reconciled, with no netting
        # between phases, backgrounds, incarnations or eligibility classes.
        failures = [] if failures is None else list(failures)
        if type(churn_read_failures) is not int or churn_read_failures < 0:
            failure_errors.append("churn read-failure counter unavailable or moved backwards")
        elif len(failures) != churn_read_failures:
            classes["unclassified"] += max(0, churn_read_failures - len(failures))
            failure_errors.append(f"failure diagnostic census={len(failures)} != counter={churn_read_failures}")
        identities = set()
        for failure in failures:
            result = dict(failure, classification="unclassified")
            try:
                pid, incarnation = failure["pid"], failure["incarnation"]
                assert pid > 0 and incarnation > 0, "missing failure identity"
                phase = failure["phase"]
                assert phase in phases, "unknown read phase"
                phases[phase] += 1
                stamp, begin, end = (failure[k] for k in ("tick_ns", "read_begin_ns", "read_end_ns"))
                tick = bisect_right(ticks, stamp) - 1
                assert 0 <= tick < len(ticks) - 1, "failure outside closed perf ticks"
                assert ticks[tick] <= stamp <= begin <= end < ticks[tick + 1], "invalid failure read bracket"
                key = (pid, incarnation, tick, phase)
                assert key not in identities, "duplicate failure outcome"
                identities.add(key)
                result["tick"] = tick
                if pid not in starts:
                    result["classification"] = "background"
                elif incarnation != cohort_by_pid[pid].get("incarnation"):
                    result["classification"] = "incarnation_mismatch"
                elif tick < eligible[pid]:
                    result["classification"] = "pre_eligibility"
                else:
                    returned = cohort_by_pid[pid].get("statement_return_mono_ns")
                    forks = [r["timestamp_ns"] for r in lifecycle["forks"] if r["pid"] == pid]
                    exits = [r["timestamp_ns"] for r in lifecycle["exits"] if r["pid"] == pid]
                    # Multiple forks for the pid make a lifecycle exemption
                    # ambiguous. Do not join a successor's EXIT to this reader.
                    exit_time = exits[0] if len(forks) == len(exits) == 1 and exits[0] >= forks[0] else None
                    terminal = [(returned, "statement_return"), (exit_time, "EXIT")]
                    proof = next(((ts, source) for ts, source in terminal
                                  if ts is not None and begin >= ts), None)
                    if proof:
                        result.update(classification="terminal", terminal_ns=proof[0], terminal_evidence=proof[1])
                    else:
                        result["classification"] = "eligible"
            except (AssertionError, KeyError, TypeError) as error:
                result.update(classification="unclassified", evidence_error=str(error))
            classes[result["classification"]] += 1
            classified.append(result)
        for name in ("eligible", "background", "incarnation_mismatch", "unclassified"):
            if classes[name]:
                failure_errors.append(f"unexplained {name} failures={classes[name]} (required zero)")
        if gate_read_failures is not None and phases["gate"] != gate_read_failures:
            failure_errors.append(f"gate failure census={phases['gate']} != counter={gate_read_failures}")
        for pid, extra in late.items():
            forks = [r["timestamp_ns"] for r in lifecycle["forks"] if r["pid"] == pid]
            detail = {"pid": pid, "incarnation": cohort_by_pid[pid].get("incarnation"),
                      "extra_ticks": extra, "fork_mono_ns": forks[0] if len(forks) == 1 else None,
                      "E_wall_ns": starts[pid], "E_mono_bounds_ns": [starts[pid] - high, starts[pid] - low],
                      "T_tick": eligible[pid], "T_mono_ns": ticks[eligible[pid]],
                      "first_attribution_tick": first[pid], "first_attribution_mono_ns": ticks[first[pid]]}
            late_details.append(detail)
            if detail["fork_mono_ns"] is None:
                failure_errors.append(f"fork evidence unavailable for late pid={pid}")
    return {"churn_sessions": len(starts), "attributed_sessions": len(first),
            "extra_observed_ticks_bound": 0, "first_attribution_tick": first,
            "eligible_tick": eligible, "missing_attribution_pids": missing,
            "late_attribution_ticks": late, "cohort_pre_e_ticks": len(pre_e),
            "churn_read_failures": churn_read_failures, "clock_alignment": alignment,
            "cohort_sample_holes": holes, "failure_class_counts": classes,
            "failure_phase_counts": phases, "classified_failures": classified,
            "late_backend_timeline": late_details,
            "evidence_errors": evidence_errors, "attribution_errors": attribution_errors,
            "churn_failure_errors": failure_errors,
            "timing_errors": evidence_errors + attribution_errors + failure_errors}


def require_pg13_negative_control(result):
    """A setup, route or perf failure cannot stand in for the timing RED."""
    assert not result["evidence_errors"], f"pre-fix negative control has invalid timing evidence: {result['evidence_errors']}"
    reds = result["attribution_errors"]
    assert reds, "pre-fix negative control did not redden first attribution (missing or late)"
    return "pre-fix PG13 negative control: RED at " + "; ".join(reds)


# Explicit syscall tracepoints, decoded by `perf script -F tid,time,event,trace`.
VM_EVENTS = "syscalls:sys_enter_process_vm_readv,syscalls:sys_exit_process_vm_readv"
VM_RAW = re.compile(r"^\s*(\d+)\s+(\d+\.\d+):\s+syscalls:sys_(enter|exit)_process_vm_readv:\s*(.*)$")
VM_RENDERED = re.compile(r"^\s*(\d+\.\d+)\s+\([^)]*\):\s*(\d+)\s+(.*)$")
# Expected perf probe marker under `perf script --ns -F tid,time,event,trace`:
#   <decimal tid> <seconds>.<1-9 fractional digits>: <exact group:event>: <opaque payload>
# Example (synthetic; still needs box confirmation):
#   77 1234567.000000000: pgwt866_test:tick: (0x1234)
# Whitespace before tid/event is allowed; the probe payload is not interpreted.
VM_TICK = r"^\s*(\d+)\s+(\d+\.\d+):\s+{event}:.*$"


def require_lossless(*streams):
    for stream in streams:
        # --show-lost-events prints PERF_RECORD_LOST (including records with
        # a zero count); a loss record is never evidence of syscall absence.
        if LOST.search(stream) or "PERF_RECORD_LOST" in stream:
            raise AssertionError("perf lost events; syscall absence is unproven")


# Lifecycle tracepoints the tracer under test never attaches BPF to. Its own
# tp/sched/sched_process_{fork,exit} programs return 0, which drops every perf
# sample of those tracepoints system-wide. task_newtask fires in copy_process
# in the parent (perf's tid column); sched_process_free is never before EXIT.
PG13_FORK_EVENT = "task:task_newtask"
PG13_EXIT_EVENT = "sched:sched_process_free"


def parse_pg13_lifecycle(text, extra_streams=(), *, postmaster=None):
    """sched events are system-wide: attaching only to the daemon misses PG forks."""
    require_lossless(text, *extra_streams)
    lifecycle = {"forks": [], "exits": []}
    pattern = re.compile(r"^\s*(\d+)\s+(\d+)\.(\d{1,9}):\s+"
                         r"(task:task_newtask|sched:sched_process_free):\s*(.*)$")
    for line in text.splitlines():
        if not re.search(r"(?:task:task_newtask|sched:sched_process_free):", line):
            continue
        match = pattern.fullmatch(line)
        assert match, f"unparseable scheduler lifecycle event: {line}"
        tid, seconds, fraction, kind, body = match.groups()
        ts = int(seconds) * 1_000_000_000 + int(fraction.ljust(9, "0"))
        def number(key):
            field = re.search(r"(?:^|\s)" + key + r"=(\d+)(?=\s|$)", body)
            assert field, f"missing {key} in scheduler event: {line}"
            return int(field[1])
        if kind == PG13_FORK_EVENT:
            parent, pid = int(tid), number("pid")
            if postmaster is None or parent == postmaster:
                lifecycle["forks"].append({"pid": pid, "parent_pid": parent, "timestamp_ns": ts})
        else:
            lifecycle["exits"].append({"pid": number("pid"), "timestamp_ns": ts})
    for rows in lifecycle.values():
        rows.sort(key=lambda row: row["timestamp_ns"])
    return lifecycle


def vm_arguments(body):
    values = {}
    for name in ("pid", "liovcnt", "riovcnt"):
        match = re.search(r"\b" + name + r"\s*[:=]\s*(0x[0-9a-fA-F]+|[0-9]+)(?=\s*(?:,|\(|\)|$))", body)
        assert match, f"readv arguments not decoded: {body}"
        values[name] = int(match[1], 0) if match[1].startswith("0x") else int(match[1])
    return values


def parse_vm_trace(text, extra_streams=(), tick_event=None):
    """Keep each entry and its exit together, across threads and CPU migration.

    Raw tracepoints are the live evidence: sort by timestamp, pair per tid,
    and retain the entry timestamp as the call's identity. Rendered perf trace
    recordings are also readable for regression fixtures. Their timestamps
    are ENTRY times, even on a completed/continued line. An entry-only line
    at T must never consume a complete call at a different T on the same tid
    (the recorded EL8 failure). Unmatched entries remain visible with ret=None;
    qualification rejects incomplete evidence within its closed tick window.
    """
    require_lossless(text, *extra_streams)
    records, raw, rendered, ticks, unmatched_exits = [], [], {}, [], []
    marker = re.compile(VM_TICK.format(event=re.escape(tick_event))) if tick_event else None
    for line in text.splitlines():
        tick = marker.match(line) if marker else None
        if tick:
            tid, timestamp = tick.groups()
            seconds, fraction = timestamp.split(".")
            assert len(fraction) <= 9, f"invalid perf timestamp: {timestamp}"
            raw.append((int(seconds) * 1_000_000_000 + int(fraction.ljust(9, "0")),
                        int(tid), "tick", ""))
        elif "sys_enter_process_vm_readv" in line or "sys_exit_process_vm_readv" in line:
            match = VM_RAW.match(line)
            assert match, f"unparseable readv tracepoint: {line}"
            tid, timestamp, kind, body = match.groups()
            # Integer ns: floats lose ordering at the precision of perf --ns.
            seconds, fraction = timestamp.split(".")
            assert len(fraction) <= 9, f"invalid perf timestamp: {timestamp}"
            raw.append((int(seconds) * 1_000_000_000 + int(fraction.ljust(9, "0")),
                        int(tid), kind, body))
        elif "process_vm_readv(" in line:
            header = VM_RENDERED.match(line)
            assert header, f"unparseable rendered readv: {line}"
            timestamp, tid, body = header.groups()
            call = CALL.search(body)
            assert call and call[1] == "process_vm_readv", f"unparseable readv: {line}"
            key = (int(tid), timestamp)
            previous = rendered.get(key)
            if "[continued]" in body:
                assert previous, "readv continuation without matching tid/timestamp entry"
                args = {k: previous[k] for k in ("pid", "liovcnt", "riovcnt")}
            else:
                args = vm_arguments(call[2])
            result = re.match(r"\s*=\s*(-?\d+)", call[3])
            assert result or call[3].strip() == "...", f"readv lacks a result: {line}"
            record = dict(args, tid=int(tid), timestamp_ns=int(timestamp.replace(".", "")) *
                          10 ** (6 - len(timestamp.split(".")[1])),
                          ret=int(result[1]) if result else None)
            if previous:
                assert all(previous[k] == record[k] for k in args), "readv entry/exit arguments differ"
                assert previous["ret"] is None and record["ret"] is not None, "duplicate readv entry/exit"
            rendered[key] = record
    assert not (raw and rendered), "mixed raw and rendered readv evidence"
    if raw:
        pending, current_tick = {}, {}
        for timestamp, tid, kind, body in sorted(raw):
            if kind == "tick":
                assert tid not in pending, "tick marker inside an unfinished syscall"
                assert (tid, timestamp) not in ticks, "duplicate tick marker"
                ticks.append((tid, timestamp))
                current_tick[tid] = timestamp
            elif kind == "enter":
                assert tid not in pending, f"overlapping raw readv entries: tid={tid} timestamp={timestamp}"
                pending[tid] = dict(vm_arguments(body), tid=tid, timestamp_ns=timestamp, ret=None)
                if tick_event:
                    pending[tid]["tick_ns"] = current_tick.get(tid)
            else:
                if tid not in pending:
                    assert tick_event, f"readv exit without entry: tid={tid} timestamp={timestamp}"
                    unmatched_exits.append((tid, timestamp))
                    continue
                # perf script prints a bare value, not a `ret:` field. Match
                # the entire payload so an undecoded suffix cannot pass.
                result = re.fullmatch(r"\s*(0x[0-9a-fA-F]+|-?[0-9]+)\s*", body)
                assert result, f"readv exit result not decoded: {body}"
                if result[1].startswith("0x"):
                    value = int(result[1], 16)
                    assert value < 1 << 64, f"readv exit result outside 64 bits: {body}"
                    if value >= 1 << 63:
                        value -= 1 << 64
                else:
                    value = int(result[1])
                    assert -(1 << 63) <= value < 1 << 63, f"readv exit result outside 64 bits: {body}"
                record = pending.pop(tid)
                record["ret"] = value
                records.append(record)
        records.extend(pending.values())
    else:
        records = list(rendered.values())
        if SUMMARY_HEADER.search(text):
            counts = parse_summary(text, extra_streams)
            assert sum(row["ret"] is not None for row in records) == counts["process_vm_readv"], \
                "readv entry/exit-summary census differs"
    assert records, "perf observed no process_vm_readv entries"
    return {"calls": sorted(records, key=lambda row: (row["timestamp_ns"], row["tid"])),
            "ticks": ticks, "unmatched_exits": unmatched_exits}


def parse_vm_calls(text, extra_streams=()):
    return parse_vm_trace(text, extra_streams)["calls"]


def pg13_vm_diagnostics(text, extra_streams=()):
    """Non-gating syscall diagnostics for the timing negative control.

    Loss still invalidates the capture, in either mode. Structural/syscall
    assertions cannot substitute for the required far-row timing RED.
    """
    require_lossless(text, *extra_streams)
    try:
        return {"calls": {"process_vm_readv": len(parse_vm_calls(text, extra_streams))}}
    except AssertionError as error:
        return {"evidence_error": str(error)}


def assess_pg13_vm_calls(text, postmaster, extra_streams=(), *, ticks_inside=0,
                        ticks_spanning=None, read_faults=0, wait_pids=(),
                        wait_iov_min=488, wait_iov_max=1023, tick_event=None):
    """Qualify calls inside independently marked sampler ticks.

    There is exactly one permitted non-postmaster wait-shaped call per full
    tick: a stable independently enumerated reader, equal population-sized
    iovec counts, and four returned bytes per iovec. Identical argument/result
    tuples cannot identify C functions; this is an explicit shape exemption,
    never a claim of semantic identification. A second exemption in a tick
    fails. Every other call targets the postmaster; short transfers which could
    be discovery count AGAINST its budget, even if they could be status retries.
    """
    assert read_faults == 0, "wait-word faults prevent the one-batch wait exemption"
    trace = parse_vm_trace(text, extra_streams, tick_event)
    captured, ticks = trace["calls"], trace["ticks"]
    if not tick_event:
        assert all(row["ret"] is not None for row in captured), "unfinished readv in perf evidence"
    assert tick_event and len(ticks) >= 2, "need independent sampler tick markers"
    # Qualify CLOSED tick intervals only. A recorder may attach within a
    # syscall, or stop after an entry. Keep boundary evidence in diagnostics;
    # never guess its arguments/result or claim absence outside this window.
    start, end = ticks[0][1], ticks[-1][1]
    assert not any(start <= timestamp < end for _, timestamp in trace["unmatched_exits"]), \
        "readv exit without entry inside qualified ticks"
    records = [row for row in captured if start <= row["timestamp_ns"] < end]
    assert records and all(row["ret"] is not None for row in records), "unfinished readv in qualified ticks"
    assert wait_pids and postmaster not in wait_pids, "need independent non-postmaster wait-reader pids"
    discovery = wait = status = maximum = 0
    wait_reader = None
    sampler_tids = {tid for tid, _ in ticks}
    assert len(sampler_tids) == 1, "tick markers outside sampler tid"
    sampler_tid = next(iter(sampler_tids))
    assert all(row["tid"] == sampler_tid for row in records), "readv outside sampler tid"
    per_tick = {timestamp: {"discovery": 0, "wait": 0} for _, timestamp in ticks[:-1]}
    for row in records:
        pid, iovs, remote, result = (row[k] for k in ("pid", "liovcnt", "riovcnt", "ret"))
        bucket = per_tick[row["tick_ns"]]
        assert 0 < iovs == remote <= 1024, f"readv iovec shape: {row}"
        if pid != postmaster:
            assert (pid in wait_pids and wait_iov_min <= iovs <= wait_iov_max
                    and result == 4 * iovs), f"process_vm_readv on non-postmaster outside wait exemption: {row}"
            if wait_reader is None:
                wait_reader = pid
            assert (pid, row["tid"]) == (wait_reader, sampler_tid), "wait reader/tid changed"
            wait += 1
            bucket["wait"] += 1
            assert bucket["wait"] <= 1, "extra wait-shaped call inside sampler tick"
        elif result <= 12 * iovs:
            # Includes negative/short/ambiguous status returns. Never let a
            # third discovery hide by labelling a small transfer 'status'.
            discovery += 1
            bucket["discovery"] += 1
        else:
            assert iovs == 1024 or iovs % 4 == 0 or iovs % 3 == 0, f"status batch iovec shape: {row}"
            status += 1
    maximum = max(bucket["discovery"] for bucket in per_tick.values())
    # Every closed marker interval contains exactly one wait-shaped call.
    for _, timestamp in ticks[:-1]:
        assert per_tick[timestamp]["wait"] == 1, f"missing wait-word batch in sampler tick {timestamp}"
    assert discovery and wait and status, "need pending discovery, status and wait-word calls"
    assert len(ticks) - 1 >= ticks_inside, "fewer closed tick markers than observed sampler ticks"
    assert wait >= ticks_inside, "fewer wait-word batches than observed sampler ticks"
    if ticks_spanning is not None:
        assert len(ticks) <= ticks_spanning, "extra tick markers compared with observed sampler ticks"
        assert wait <= ticks_spanning, "extra wait-word batches compared with observed sampler ticks"
    assert maximum <= 2, f"possible discovery calls/tick={maximum} > 2"
    return {"possible_discovery_calls": discovery, "max_discovery_calls_per_tick": maximum,
            "status_calls": status, "wait_word_calls": wait, "wait_reader_pid": wait_reader,
            "calls": {"process_vm_readv": len(records)},
            "ticks_inside": ticks_inside, "ticks_spanning": ticks_spanning,
            "wait_read_faults": read_faults, "tick_markers": len(ticks),
            "qualified_ticks": len(ticks) - 1, "qualified_start_ns": start, "qualified_end_ns": end,
            "boundary_entries": len(captured) - len(records), "boundary_exits": len(trace["unmatched_exits"]),
            "wait_exemption": "one stable independently enumerated pid; equal population iov counts; 4 bytes/iov"}
