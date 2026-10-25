#!/usr/bin/env python3
"""test_dump_report.py — the --dump text report must account for its own time.

THE DEFECT (#553's unfinished surface). `pgwt-server --dump` prints a Top
Events table with a "% DB" column. #553 made the CPU* row carry MEASURED
on-CPU nanoseconds instead of the on-CPU gap's wall, which moved the runqueue
remainder out of every row and into `pgwt_events_result.offcpu_ms` -- a field
the JSON response reports and this text report printed nowhere. So on a
measured (v3) capture the column silently stopped summing to 100%: on the
fixture below it summed to 60.0%, with 40% of DB Time on no printed line and
nothing saying so. The Time Model section of the SAME dump printed that 40% as
its own row ("CPU (waiting for a core)"), so one report disagreed with itself.

WHAT THIS PINS
  1. the off-CPU row exists in the Top Events table, with the hand-computed
     60.0 ms / 40.0% of this fixture (section 1);
  2. the printed "% DB" column sums to 100% (section 1) -- the arithmetic the
     reader actually does;
  3. both sections of one dump use the SAME name and the SAME number for it
     (section 1), so the cross-section contradiction cannot come back;
  4. CPU* is the measured 40.0 ms, not the 100.0 ms wall -- without this the
     sum above is satisfied by reverting #553 entirely (section 1).

WHY SECTION 2 IS HALF THE FILE (false negatives). A gate that cannot see must
refuse, never approve. Every way this check could be satisfied without
checking anything is walked: a capture with NO measurement (where the row must
be ABSENT and the sum must still be 100% -- the thing being checked absent
rather than wrong), a dump whose table has no rows at all, a table of nothing
but "—" sentinels, a server that exits non-zero, and a missing binary. Each
asserts that the PARSER REFUSES, because a parser that returns an empty row
list turns every assertion in section 1 into a vacuous truth.

Runs anywhere pgwt-server and tests/gen_test_traces build: no root, no PG, no
BPF, no clock dependence (every timestamp is a literal offset from BASE).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (
    SERVER_BIN, TestRunner, generate_traces, cleanup_traces,
    CPU, IO_DATA_FILE_READ,
)

BASE = 10_000_000_000_000   # 10000 s in ns (> 1 h, see server_harness)
S = 1_000_000_000
MS = 1_000_000
N = 10                      # gaps / waits in the fixture

# The fixture, hand-computed. N on-CPU gaps of 10 ms wall carrying 4 ms of
# MEASURED cpu_ns, and N IO waits of 5 ms with no measurement:
#   DB Time  = N*(10 + 5) ms = 150.0 ms   (wall; the CPU row's wall included)
#   CPU*     = N*4 ms         =  40.0 ms  (measured)   26.7% of DB Time
#   off-CPU  = N*(10 - 4) ms  =  60.0 ms  (remainder)  40.0%
#   IO       = N*5 ms         =  50.0 ms  (wall)       33.3%
DB_TIME_MS = float(N * 15)
CPU_MS = float(N * 4)
OFFCPU_MS = float(N * 6)
IO_MS = float(N * 5)
OFFCPU_ROW = "CPU (waiting for a core)"


def _events():
    """N measured on-CPU gaps interleaved with N unmeasured IO waits."""
    evs = []
    for i in range(N):
        evs.append({"pid": 1000, "ts": BASE + i * S + S // 2, "dur": 10 * MS,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100,
                    "cpu": 4 * MS})
        evs.append({"pid": 1000, "ts": BASE + i * S + S // 2 + 20 * MS,
                    "dur": 5 * MS, "old": IO_DATA_FILE_READ, "new": CPU,
                    "qid": 100})
    return evs


def scenario(measured):
    """The same stream, with and without the v3 CPU measurement.

    cpu_measured=0 is the legacy shape: the writer stamps every record
    UNKNOWN, so CPU* is the unsplit gap wall and there is no remainder to
    report. That is section 2a's input, and it is a real capture shape, not a
    mutation.
    """
    sc = {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": _events(),
    }
    if measured:
        sc["cpu_measured"] = 1
    return sc


# ── the parser, which must REFUSE rather than return nothing ───────────────

class Unparseable(Exception):
    """The dump did not contain a readable table. Never a silent pass."""


def dump(trace_dir):
    """Run `pgwt-server --dump`. Raises unless it exits 0 with output.

    126 (not executable) and 127 (not found) are the two exits that look like
    "nothing to check" and must look like failure instead.
    """
    if not os.path.exists(SERVER_BIN):
        raise Unparseable(f"pgwt-server missing at {SERVER_BIN}")
    r = subprocess.run([SERVER_BIN, "--dump", trace_dir],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise Unparseable(f"--dump exited {r.returncode}: "
                          f"{r.stderr.strip()[-200:]}")
    if "=== Top Events ===" not in r.stdout:
        raise Unparseable("--dump printed no Top Events section")
    return r.stdout


# Top Events: "  <name>  <waits>  <total>  <avg>  <pct>%" — the name may
# contain spaces and parentheses, so anchor on the four trailing columns.
ROW_RE = re.compile(
    r"^  (?P<name>\S.*?)\s{2,}"
    r"(?P<waits>[\d—]+)\s+"
    r"(?P<total>[\d.]+)\s+"
    r"(?P<avg>[\d.—]+)\s+"
    r"(?P<pct>[\d.]+|—)%?\s*$")

# Time Model: "  <name>  <ms> ms  <pct>%", indented by nesting level. A
# different shape, so a different pattern -- reusing ROW_RE here would have
# parsed nothing and (without parse_rows' refusal) reported agreement.
TM_RE = re.compile(
    r"^\s+(?P<name>\S.*?)\s{2,}"
    r"(?P<total>[\d.]+) ms\s+"
    r"(?P<pct>[\d.]+|—)%?\s*$")


def section(text, title, until):
    try:
        body = text.split(f"=== {title} ===", 1)[1].split(f"=== {until} ===",
                                                           1)[0]
    except IndexError:
        raise Unparseable(f"no '{title}' section in the dump")
    return body


def parse_rows(body, what="table", rx=ROW_RE):
    """[(name, total_ms, pct_or_None)]. Raises if nothing parsed.

    A table that yields no rows is the bypass this exists to block: every
    "the row is right" assertion is vacuously true over an empty list, and so
    is every sum (0 == 0 only if you also accept 0 as the total).
    """
    rows = []
    for line in body.splitlines():
        m = rx.match(line.rstrip("\n"))
        if not m:
            continue
        pct = None if m.group("pct") == "—" else float(m.group("pct"))
        rows.append((m.group("name").strip(), float(m.group("total")), pct))
    if not rows:
        raise Unparseable(f"{what}: no rows parsed out of {len(body.splitlines())} "
                          f"lines -- refusing to report agreement")
    if all(pct is None for _, _, pct in rows):
        raise Unparseable(f"{what}: every row's %DB is the not-applicable "
                          f"sentinel -- there is no column to sum")
    return rows


def find(rows, name):
    for n, total, pct in rows:
        if n == name:
            return total, pct
    return None, None


# ── 1. the measured capture: the column adds up, and both sections agree ───

def test_measured_dump_accounts_for_offcpu(t):
    print("\n### 1. measured capture: the % DB column sums to 100% ###")
    d = generate_traces(scenario(measured=True))
    try:
        text = dump(d)
    finally:
        cleanup_traces(d)

    ev = parse_rows(section(text, "Top Events", "Top Sessions"),
                    "Top Events")
    tm = parse_rows(section(text, "Time Model", "Top Events"), "Time Model",
                    TM_RE)

    # PREMISE, asserted before anything else: this fixture really is the
    # measured shape. If CPU* carried wall (#553 reverted) the column would
    # sum to 100% with no off-CPU row at all, and every check below would be
    # satisfied by the defect.
    cpu_total, cpu_pct = find(ev, "CPU*")
    t.check_eq(cpu_total, CPU_MS,
               f"premise: CPU* is the MEASURED {CPU_MS} ms "
               f"(the gap wall would read {CPU_MS + OFFCPU_MS})")
    io_total, _ = find(ev, "IO:DataFileRead")
    t.check_eq(io_total, IO_MS,
               f"premise: the IO wait keeps its full {IO_MS} ms wall")

    off_total, off_pct = find(ev, OFFCPU_ROW)
    t.check_eq(off_total, OFFCPU_MS,
               f"Top Events prints '{OFFCPU_ROW}' = {OFFCPU_MS} ms, the time "
               f"the CPU* row no longer claims")
    t.check_eq(off_pct, round(OFFCPU_MS / DB_TIME_MS * 100.0, 1),
               "...with its share of DB Time, not a bare millisecond count")

    # The arithmetic a reader does. Three rows are each rounded to 0.1%, so
    # the printed column can be off by at most 0.15 -- the tolerance is the
    # PRINTING, not the measurement.
    printed = [pct for _, _, pct in ev if pct is not None]
    total_pct = sum(printed)
    t.check(abs(total_pct - 100.0) <= 0.15,
            f"the printed % DB column sums to {total_pct:.1f}% "
            f"(100% ± one printed rounding step)")
    t.check(len(printed) == 3,
            f"...over all three rows, none of them a sentinel (got "
            f"{len(printed)})")

    # One dump, two sections, one answer. This is the contradiction that
    # existed before: the Time Model printed the remainder and Top Events
    # did not.
    tm_total, tm_pct = find(tm, OFFCPU_ROW)
    t.check_eq(tm_total, off_total,
               f"the Time Model section prints the same '{OFFCPU_ROW}' "
               f"millisecond value as Top Events ({off_total})")
    t.check_eq(tm_pct, off_pct,
               "...and the same share, so one report cannot disagree with "
               "itself")
    t.check(find(tm, "CPU (running)")[0] == cpu_total,
            "...and the Time Model's measured CPU row equals Top Events' "
            "CPU* row")


# ── 2. the false negatives ─────────────────────────────────────────────────

def test_unmeasured_capture_prints_no_row(t):
    print("\n### 2a. no measurement: the row must be ABSENT, not 0.0 ###")
    d = generate_traces(scenario(measured=False))
    try:
        text = dump(d)
    finally:
        cleanup_traces(d)
    ev = parse_rows(section(text, "Top Events", "Top Sessions"),
                    "Top Events")

    # PREMISE: the same stream, unmeasured. CPU* is now the unsplit wall.
    cpu_total, _ = find(ev, "CPU*")
    t.check_eq(cpu_total, CPU_MS + OFFCPU_MS,
               f"premise: without a measurement CPU* is the whole "
               f"{CPU_MS + OFFCPU_MS} ms gap wall")
    off_total, _ = find(ev, OFFCPU_ROW)
    t.check(off_total is None,
            f"no '{OFFCPU_ROW}' row: a 0.0 row would claim off-CPU time was "
            f"measured as zero when it was not measured at all")
    printed = [pct for _, _, pct in ev if pct is not None]
    t.check(abs(sum(printed) - 100.0) <= 0.15,
            f"...and the column still sums to {sum(printed):.1f}% without it")
    # And the Time Model agrees that there is nothing to split, so 2a is not
    # just "this section prints less".
    tm = parse_rows(section(text, "Time Model", "Top Events"), "Time Model",
                    TM_RE)
    t.check(find(tm, OFFCPU_ROW)[0] is None,
            "...and the Time Model section omits it too, on the same input")


def test_parser_refuses_what_it_cannot_read(t):
    print("\n### 2b. the parser refuses instead of returning nothing ###")
    cases = [
        ("an empty table body", ""),
        ("a header with no rows",
         "\n  Wait Event   Waits   Total (ms)   Avg (us)     % DB\n"
         "  ──────────────────────────────────────────────────────\n"),
        ("rows whose every column is a sentinel",
         "\n  Client:ClientRead                    10         50.0  "
         "        —        —\n"),
        ("a truncated row missing its % column",
         "\n  CPU*                                 10\n"),
    ]
    for label, body in cases:
        try:
            rows = parse_rows(body, "probe")
            t.check(False, f"{label}: parser returned {len(rows)} rows "
                           f"instead of refusing")
        except Unparseable:
            t.check(True, f"{label}: parser refuses")

    # And a body that DOES parse must still parse -- a refusal that fires on
    # everything is just as blind.
    good = ("\n  CPU*                                 10         40.0     "
            "4000.0    26.7%\n")
    try:
        rows = parse_rows(good, "probe")
        t.check(rows == [("CPU*", 40.0, 26.7)],
                f"a real row still parses ({rows})")
    except Unparseable as e:
        t.check(False, f"a real row must still parse, got refusal: {e}")


def test_dump_failure_is_not_a_skip(t):
    print("\n### 2c. a server that cannot answer FAILS the gate ###")
    try:
        dump(os.path.join(os.path.sep, "nonexistent-pgwt-trace-dir"))
        t.check(False, "--dump on a missing trace dir must not look healthy")
    except Unparseable as e:
        t.check(True, f"a non-zero exit / empty report is a refusal ({e})")
    except subprocess.TimeoutExpired:
        t.check(False, "--dump hung on a missing trace dir")

    # The two dependency failures that read as "nothing to run" and must read
    # as failure: 127 (binary absent) and 126 (present, not executable). Both
    # are real here -- a missing path and a chmod 0644 file -- not two spellings
    # of the same missing path.
    tmp = tempfile.mkdtemp(prefix="pgwt_dump_probe_")
    not_exec = os.path.join(tmp, "pgwt-server")
    with open(not_exec, "w") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(not_exec, 0o644)
    saved = globals()["SERVER_BIN"]
    try:
        for path, label in ((os.path.join(tmp, "absent-pgwt-server"),
                             "a missing binary (127)"),
                            (not_exec, "a non-executable binary (126)")):
            globals()["SERVER_BIN"] = path
            try:
                dump("/tmp")
                t.check(False, f"{label} must not look healthy")
            except Unparseable as e:
                t.check(True, f"{label} is a refusal ({str(e)[:60]})")
            except OSError as e:
                t.check(True, f"{label} raises, not skips ({type(e).__name__})")
    finally:
        globals()["SERVER_BIN"] = saved
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    t = TestRunner("test_dump_report")
    print(f"=== {t.name}: --dump accounts for every millisecond ===")
    # A file that ran no assertions proves nothing: the floor is checked
    # against the number of checks actually executed.
    test_measured_dump_accounts_for_offcpu(t)
    test_unmeasured_capture_prints_no_row(t)
    test_parser_refuses_what_it_cannot_read(t)
    test_dump_failure_is_not_a_skip(t)
    ran = t.passed + t.failed
    # 9 + 4 + 5 + 3 = 21 assertions; raise this WITH the suite.
    if ran < 21:
        print(f"FAIL: only {ran} assertions ran — a section did not execute")
        t.failed += 1
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
