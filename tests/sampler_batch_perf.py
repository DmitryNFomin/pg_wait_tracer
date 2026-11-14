"""Fail-closed parser/assertions for the status-row batch live test.

Input is `perf trace -T --no-comm` (timestamps are monotonic milliseconds).
The format and interrupted/continued pairs follow tools/perf/builtin-trace.c.
No product-derived expected values: the syscall bound is the accepted design.
"""
import re

CALLS = "open,openat,pread64,close,process_vm_readv"
HEADER = re.compile(r"^\s*(\d+\.\d+)\s+\([^)]*\):\s*(?:(\d+)\s+)?(.*)$")
CALL = re.compile(r"\b(open|openat|pread64|close|process_vm_readv)\((.*)\)\s*(.*)$")
MEM = re.compile(r"/proc/(?:\d+|self)/mem\b")


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
    assert major in (13, 17, 18) and population >= 504 and ticks >= 200
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
