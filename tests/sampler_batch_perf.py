"""Fail-closed parser/assertions for the status-row batch live test.

The live test counts syscalls from the summary that `perf trace -S` prints at
its end (`parse_summary` / `assess_counts`): it needs no path names, which
neither the Ubuntu 24.04 (perf 6.8) nor the EL8 (perf 4.18, no BPF skeletons)
image can resolve (`filename: 0x...`). `parse_trace` / `assess` are the older
form that read per-event text lines and path names; they are kept unchanged
because tests/test_sampled_attr_io.py still exercises them, and the live test
no longer calls them.
No product-derived expected values: the syscall bound is the accepted design.
"""
import re

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
