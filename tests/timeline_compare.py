#!/usr/bin/env python3
"""timeline_compare.py -- trap reference vs timeline waits (#1006 unit 1, S5).

Compares, for one two-tracer run, the trap reference (tests/dump_transitions
output of the `--mode full` tracer) with the waits of the `--mode timeline`
tracer (.pgwtl segments) and applies the acceptance criteria of #1006 plan
comment 7100809610, amendment 6:

  a. strict 1:1: >= 96.0% of trap waits matched by exactly one same-event
     timeline wait with |start err| <= 50 us and |end err| <= 50 us;
  b. time credited by OVERLAP only: >= 96.0% of trap wait time;
  c. extra (timeline wait time overlapping no same-event trap wait)
     <= 0.01% of trap wait time;
  d. every pid with >= 100 timeline calls has trap records.
ZERO_MARKER waits have no duration; the comparator may pad its own matching
window for them, the product never does.

S0: the frozen interface. S5 ports tl-parse's matching (tl-strict.py) into
these functions; every function raises NotImplementedError until then.

Usage: timeline_compare.py <trap_export.txt> <timeline_dir>   (exit 0 pass,
1 fail, 2 refused/invalid input)
"""
import dataclasses
import sys

STRICT_ERR_NS = 50_000

THRESHOLDS = {
    "strict_wait_pct": 96.0,    # (a)
    "overlap_time_pct": 96.0,   # (b)
    "extra_time_pct": 0.01,     # (c), an upper bound
    "pid_min_calls": 100,       # (d)
}


@dataclasses.dataclass(frozen=True)
class Interval:
    pid: int
    start_ns: int
    end_ns: int
    event: int          # wait_event_info; 0 = no wait (trap NULL interval)
    flags: int = 0      # timeline record flags (PGWT_TL_F_*); 0 for trap
    n_syscalls: int = 0


@dataclasses.dataclass
class CompareResult:
    trap_waits: int = 0
    strict_matched: int = 0
    trap_wait_ns: int = 0
    overlap_ns: int = 0
    extra_ns: int = 0
    timeline_waits: int = 0
    zero_marker_waits: int = 0
    # pid -> timeline call count, for pids with timeline calls and no trap rows
    pids_without_trap: dict = dataclasses.field(default_factory=dict)
    # event -> (trap waits, strict matched, trap ns, overlap ns)
    per_event: dict = dataclasses.field(default_factory=dict)

    def strict_wait_pct(self):
        return 100.0 * self.strict_matched / self.trap_waits if self.trap_waits else float("nan")

    def overlap_time_pct(self):
        return 100.0 * self.overlap_ns / self.trap_wait_ns if self.trap_wait_ns else float("nan")

    def extra_time_pct(self):
        return 100.0 * self.extra_ns / self.trap_wait_ns if self.trap_wait_ns else float("nan")


def load_trap(path):
    """dump_transitions output -> {pid: [Interval sorted by start]}."""
    raise NotImplementedError("timeline_compare.load_trap: #1006 S5")


def load_timeline(directory):
    """.pgwtl segments of one capture -> {pid: [Interval sorted by start]}
    (WAIT records only; refuses a TRUNCATED segment)."""
    raise NotImplementedError("timeline_compare.load_timeline: #1006 S5")


def compare(trap, timeline, *, err_ns=STRICT_ERR_NS):
    """Match per pid inside the window where the trap has records."""
    raise NotImplementedError("timeline_compare.compare: #1006 S5")


def evaluate(result, thresholds=THRESHOLDS):
    """List of failed criteria as text; empty means pass."""
    raise NotImplementedError("timeline_compare.evaluate: #1006 S5")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print("usage: timeline_compare.py <trap_export.txt> <timeline_dir>", file=sys.stderr)
        return 2
    print("timeline_compare: not implemented yet (#1006 S5)", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
