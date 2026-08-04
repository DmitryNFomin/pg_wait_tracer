"""server_harness.py — Test harness for pgwt-server.

Spawns pgwt-server, sends JSON commands on stdin, reads JSON responses.
Shared by all Layer 0B (synthetic data correctness) tests.

Usage:
    from server_harness import ServerHarness
    with ServerHarness(trace_dir) as srv:
        resp = srv.query("time_model")
        resp = srv.query("top_events", filters={"class": "io"})
"""
import json
import os
import subprocess
import sys
import tempfile
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
SERVER_BIN = os.path.join(PROJECT_DIR, "pgwt-server")
GEN_BIN = os.path.join(SCRIPT_DIR, "gen_test_traces")


class ServerHarness:
    """Manages a pgwt-server subprocess for testing."""

    def __init__(self, trace_dir, env=None):
        self.trace_dir = trace_dir
        self.proc = None
        self._next_id = 1
        self._extra_env = env  # extra environment vars for pgwt-server

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.stop()

    def start(self):
        if not os.path.exists(SERVER_BIN):
            raise FileNotFoundError(f"pgwt-server not found at {SERVER_BIN}")
        env = None
        if self._extra_env:
            env = dict(os.environ)
            env.update({k: str(v) for k, v in self._extra_env.items()})
        self.proc = subprocess.Popen(
            [SERVER_BIN, self.trace_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        # Query info to get time range, then expand slightly so all events
        # in the first/last block are included
        info = self.query("info")
        self._from_ns = info.get("from_ns", 0)
        self._to_ns = info.get("to_ns", 0)
        # Expand range by 30s each direction to ensure all events are captured.
        # Keep total range < 120s to avoid the summary path (which needs real
        # clock offsets that synthetic traces don't have).
        if self._from_ns > 0:
            self._from_ns -= 30_000_000_000
        if self._to_ns > 0:
            self._to_ns += 30_000_000_000

    def stop(self):
        if self.proc:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
            self.proc = None

    def query(self, cmd, **kwargs):
        """Send a command and return parsed JSON response.

        Args:
            cmd: Command name (time_model, top_events, etc.)
            **kwargs: Optional fields — filters, from_ns (as 'from_'), to_ns (as 'to_'),
                      num_buckets (as 'buckets'), detail.
        """
        req = {"id": self._next_id, "cmd": cmd}
        self._next_id += 1

        if "from_" in kwargs:
            req["from"] = kwargs.pop("from_")
        if "to_" in kwargs:
            req["to"] = kwargs.pop("to_")
        if "buckets" in kwargs:
            req["buckets"] = kwargs.pop("buckets")
        if "detail" in kwargs:
            req["detail"] = kwargs.pop("detail")
        if "filters" in kwargs:
            req["filters"] = kwargs.pop("filters")

        # Additive command-specific fields (for example limit/max_points and
        # execution_detail's decimal-string bounds) pass through unchanged.
        req.update(kwargs)

        # Auto-inject expanded time range if not specified
        if "from" not in req and hasattr(self, "_from_ns") and self._from_ns > 0:
            req["from"] = self._from_ns
        if "to" not in req and hasattr(self, "_to_ns") and self._to_ns > 0:
            req["to"] = self._to_ns

        line = json.dumps(req) + "\n"
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

        resp_line = self.proc.stdout.readline()
        if not resp_line:
            stderr = self.proc.stderr.read()
            raise RuntimeError(f"pgwt-server closed stdout. stderr: {stderr}")

        return json.loads(resp_line)


def generate_traces(scenario, output_dir=None, wall_offset_ns=None, rotate=None):
    """Generate trace files from a scenario dict.

    Args:
        scenario: Dict with 'backends', 'queries', 'events' keys.
        output_dir: Directory to write to (created if None, using tempdir).
        wall_offset_ns: mono→wall offset patched into this run's file headers
            (default 0 — wall == mono). Simulates an NTP step between files.
        rotate: if set (e.g. "2025-01-01_10"), rename this run's current.*
            to <rotate>.trace.lz4 / <rotate>.summary.lz4 so another run can
            add a second file to the same dir.

    Returns:
        Path to the trace directory.
    """
    if output_dir is None:
        output_dir = tempfile.mkdtemp(prefix="pgwt_test_")

    if not os.path.exists(GEN_BIN):
        raise FileNotFoundError(
            f"gen_test_traces not found at {GEN_BIN}. Run 'make' in tests/."
        )

    scenario_json = json.dumps(scenario)
    if len(scenario_json) > 65536:
        # Large scenarios exceed the OS argv limit — pass via a file.
        scenario_path = os.path.join(output_dir, "_scenario.json")
        with open(scenario_path, "w") as f:
            f.write(scenario_json)
        cmd = [GEN_BIN, "-o", output_dir, "-s", scenario_path]
    else:
        cmd = [GEN_BIN, "-o", output_dir, "--inline", scenario_json]
    if wall_offset_ns is not None:
        cmd += ["--wall-offset", str(wall_offset_ns)]
    if rotate is not None:
        cmd += ["--rotate", rotate]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"gen_test_traces failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )

    return output_dir


def cleanup_traces(trace_dir):
    """Remove a temporary trace directory."""
    if trace_dir and os.path.isdir(trace_dir):
        shutil.rmtree(trace_dir, ignore_errors=True)


# ── Well-known event IDs ──────────────────────────────────────

# Class bytes (high byte of wait_event_info)
CLASS_LWLOCK    = 0x01
CLASS_LOCK      = 0x03
CLASS_BUFFERPIN = 0x04
CLASS_ACTIVITY  = 0x05
CLASS_CLIENT    = 0x06
CLASS_EXTENSION = 0x07
CLASS_IPC       = 0x08
CLASS_TIMEOUT   = 0x09
CLASS_IO        = 0x0A

def make_event_id(cls, event_num):
    """Build a wait_event_info value from class and event number."""
    return (cls << 24) | event_num

# Common event IDs (PG18 numbering — alphabetical order within class)
CPU = 0
IO_DATA_FILE_READ  = make_event_id(CLASS_IO, 21)        # IO:DataFileRead
IO_DATA_FILE_WRITE = make_event_id(CLASS_IO, 24)        # IO:DataFileWrite
IO_WAL_SYNC        = make_event_id(CLASS_IO, 78)        # IO:WALSync
IO_WAL_WRITE       = make_event_id(CLASS_IO, 80)        # IO:WALWrite
LOCK_RELATION      = make_event_id(CLASS_LOCK, 0)       # Lock:relation
LOCK_TRANSACTIONID = make_event_id(CLASS_LOCK, 5)       # Lock:transactionid
LWLOCK_WAL_WRITE   = make_event_id(CLASS_LWLOCK, 8)     # LWLock:WALWrite
CLIENT_READ        = make_event_id(CLASS_CLIENT, 0)     # Client:ClientRead
TIMEOUT_PG_SLEEP   = make_event_id(CLASS_TIMEOUT, 2)    # Timeout:PgSleep
ACTIVITY_IDLE      = make_event_id(CLASS_ACTIVITY, 0)   # Activity:ArchiverMain (any activity event)
EXTENSION_EXT      = make_event_id(CLASS_EXTENSION, 1)  # Extension event
IPC_BGWORKER       = make_event_id(CLASS_IPC, 6)        # IPC:BgWorkerStartup


# ── Test helpers ──────────────────────────────────────────────

class TestRunner:
    """Simple test runner with pass/fail tracking."""

    def __init__(self, name):
        self.name = name
        self.passed = 0
        self.failed = 0

    def check(self, condition, msg):
        if condition:
            self.passed += 1
            print(f"  PASS: {msg}")
        else:
            self.failed += 1
            print(f"  FAIL: {msg}")

    def check_eq(self, got, expected, msg):
        if got == expected:
            self.passed += 1
            print(f"  PASS: {msg}")
        else:
            self.failed += 1
            print(f"  FAIL: {msg} (got {got}, expected {expected})")

    def check_approx(self, got, expected, tolerance, msg):
        if expected == 0:
            ok = abs(got) <= tolerance
        else:
            ok = abs(got - expected) / expected <= tolerance
        if ok:
            self.passed += 1
            print(f"  PASS: {msg} ({got})")
        else:
            self.failed += 1
            print(f"  FAIL: {msg} (got {got}, expected {expected} ±{tolerance*100}%)")

    def summary(self):
        total = self.passed + self.failed
        print(f"\n{self.passed}/{total} tests passed")
        return self.failed == 0
