#!/usr/bin/env python3
"""demo_rehearsal_orchestrator_lib.py -- issue #176 pure logic for
scripts/demo-rehearsal.sh.

`make demo-rehearsal` used to run the whole 35-minute capture in a foreground
ssh command driven FROM the Mac: if this laptop's own memory manager killed
the local bash process (it has, four times), the ssh channel dropped, the
remote demo_rehearsal.sh got SIGHUP and died with it, and the ephemeral VM
was left running (billing) unless a trap fired -- SIGKILL never lets a trap
fire.

The fix (scripts/demo-rehearsal.sh) now starts the capture DETACHED on the
VM (nohup + setsid -w, so it is a child of nothing the ssh session owns) and
treats "wait for it" as resumable: a small state file records enough to
reattach after the launcher itself dies, so a killed launcher only means a
LATER `--collect` invocation finishes the job -- it never means a lost run
or a leaked machine.

This module holds the three decisions that are actual logic (and so get a
unit test here, tests/test_demo_rehearsal_orchestrator_lib.py) instead of
being buried in shell string-parsing:
  - parse_state_line: turn one line from the remote status probe into a
    (state, rc) pair.
  - compute_wait_budget_s: how long the launcher's SINGLE long sleep should
    be, given when the detached run actually started -- the same formula
    whether this is the original launch or a `--collect` reattach, so a
    `--collect` invoked seconds after a kill still waits out the right
    remaining budget instead of re-sleeping the full window.
  - validate_results_dir: is a just-rsynced tests/results/demo_rehearsal/
    a COMPLETE, FRESH result for THIS invocation, or something that must
    never be allowed to overwrite a previous good run (an empty dir, a
    truncated transfer, someone else's stale run.id)?
  - decide_outcome: combine remote lifecycle state + wrapper exit code +
    results validity into the launcher's own exit code and verdict line.

Nothing here touches the network or spawns a process -- validate_results_dir
reads local files a caller already rsynced down, everything else is plain
data in, plain data out.

Usage (CLI, called from scripts/demo-rehearsal.sh):
  python3 tests/demo_rehearsal_orchestrator_lib.py parse-state "STATE=... RC=..."
  python3 tests/demo_rehearsal_orchestrator_lib.py wait-budget START NOW DURATION_MIN
  python3 tests/demo_rehearsal_orchestrator_lib.py validate-results DIR EXPECTED_RUN_ID
  python3 tests/demo_rehearsal_orchestrator_lib.py decide-outcome STATE RC RESULTS_OK
"""
import json
import os
import re
import sys

# 15 minutes of slack on top of DURATION_MIN for provisioning-already-done
# + `make -j` + `make -C tests` + `make pgwt-client` on a fresh ephemeral
# VM -- generous (the ubuntu build is a couple of minutes on a cx33) but
# never the thing that decides pass/fail, only how long the launcher's one
# long sleep is before it even looks.
BUILD_BUFFER_S = 900

# If still "running" after the primary budget, ONE further single sleep
# (never a loop of short ones) before giving up and treating it as stuck.
GRACE_S = 600

VALID_STATES = {"finished", "running", "died", "not-started", "unreachable"}

_STATE_RE = re.compile(r"^STATE=(\S+)\s+RC=(\S+)\s*$")


class OrchestratorError(Exception):
    pass


def parse_state_line(line):
    """Parse one line of the form 'STATE=<state> RC=<rc-or-dash>' emitted
    by the remote status probe (scripts/demo-rehearsal.sh's remote_state())
    into {"state": str, "rc": int|None}. Raises OrchestratorError on
    anything that isn't exactly that shape -- an ssh hiccup or a probe
    change must never be silently read as a real state."""
    line = (line or "").strip()
    m = _STATE_RE.match(line)
    if not m:
        raise OrchestratorError(f"unparseable remote-state line: {line!r}")
    state, rc_raw = m.group(1), m.group(2)
    if state not in VALID_STATES:
        raise OrchestratorError(f"unknown remote state {state!r} in line: {line!r}")
    rc = None
    if rc_raw != "-":
        if not re.match(r"^-?\d+$", rc_raw):
            raise OrchestratorError(f"non-numeric RC {rc_raw!r} in line: {line!r}")
        rc = int(rc_raw)
    return {"state": state, "rc": rc}


def compute_wait_budget_s(start_epoch, now_epoch, duration_min,
                          build_buffer_s=BUILD_BUFFER_S):
    """Seconds remaining until the detached run's expected deadline
    (start_epoch + duration_min*60 + build_buffer_s), clamped to >= 0.
    Same formula for a fresh launch (now_epoch == start_epoch, so this is
    the full budget) and a `--collect` reattach any time later (the
    remaining budget only, never the full window again)."""
    deadline = float(start_epoch) + float(duration_min) * 60.0 + float(build_buffer_s)
    remaining = deadline - float(now_epoch)
    return max(0, int(round(remaining)))


def validate_results_dir(path, expected_run_id):
    """Is `path` (a just-rsynced tests/results/demo_rehearsal/, or a
    staging copy of it) a COMPLETE result for the invocation that started
    at `expected_run_id` (the run.id demo_rehearsal.py was told to write,
    tests/demo_rehearsal.py's PGWT_RUN_MARKER)? Returns (ok: bool,
    reason: str) -- ok is False (never an exception) for every bypass
    shape this exists to catch: a missing/empty directory, a missing or
    truncated summary.json, a summary.json missing required keys, or a
    run.id that does not match (a stale directory from an earlier run, or
    a transfer that silently dropped the file)."""
    if not path or not os.path.isdir(path):
        return False, f"no directory at {path!r}"

    run_id_path = os.path.join(path, "run.id")
    if not os.path.isfile(run_id_path):
        return False, "run.id missing"
    try:
        with open(run_id_path) as f:
            run_id_raw = f.read().strip()
    except OSError as e:
        return False, f"run.id unreadable: {e}"
    if not re.match(r"^\d+$", run_id_raw):
        return False, f"run.id is not numeric: {run_id_raw!r}"
    if str(expected_run_id) != run_id_raw:
        return False, (f"run.id {run_id_raw} does not match this invocation's "
                        f"{expected_run_id} -- stale/foreign results directory")

    summary_path = os.path.join(path, "summary.json")
    if not os.path.isfile(summary_path):
        return False, "summary.json missing"
    try:
        with open(summary_path) as f:
            summary = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        return False, f"summary.json unreadable/truncated: {e}"
    if not isinstance(summary, dict) or "ok" not in summary or "failed" not in summary:
        return False, "summary.json missing required keys (ok/failed)"
    if not isinstance(summary.get("failed"), list):
        return False, "summary.json 'failed' is not a list"

    return True, "complete"


def decide_outcome(state, rc, results_ok):
    """Combine the remote lifecycle state (see parse_state_line), the
    wrapper's exit code (None unless state == 'finished'), and whether
    collect_results() got back a validated, fresh results directory, into
    the launcher's own exit code + a one-line verdict.

    Exit codes:
      0  finished, rc == 0, results valid              -- real pass
      1  a definite failure (bad rc, died, never started, unreachable, or
         finished-but-results-invalid)
      2  still running -- not a failure, caller should wait longer
         (exactly one more grace sleep, never a retry loop) before
         treating it as failure 1
    """
    if state == "running":
        return 2, "remote run still in progress"
    if state == "not-started":
        return 1, "remote run never started (provisioning/launch did not complete)"
    if state == "unreachable":
        return 1, "could not reach the VM to check run state"
    if state == "died":
        return 1, "remote run died (process gone, no completion marker)"
    if state == "finished":
        if rc != 0:
            return 1, f"rehearsal exited rc={rc}"
        if not results_ok:
            return 1, "rehearsal exited 0 but the results transfer was invalid/incomplete"
        return 0, "rehearsal passed, results collected"
    raise OrchestratorError(f"unknown state {state!r}")


def _cli():
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, rest = args[0], args[1:]
    try:
        if cmd == "parse-state":
            (line,) = rest
            r = parse_state_line(line)
            print(f"{r['state']}\t{r['rc'] if r['rc'] is not None else '-'}")
            return 0
        if cmd == "wait-budget":
            start, now, duration_min = rest
            print(compute_wait_budget_s(start, now, duration_min))
            return 0
        if cmd == "validate-results":
            path, expected_run_id = rest
            ok, reason = validate_results_dir(path, expected_run_id)
            print(reason)
            return 0 if ok else 1
        if cmd == "decide-outcome":
            state, rc_raw, results_ok_raw = rest
            rc = None if rc_raw == "-" else int(rc_raw)
            results_ok = results_ok_raw in ("1", "true", "True")
            code, verdict = decide_outcome(state, rc, results_ok)
            print(f"{code}\t{verdict}")
            return 0
        print(f"unknown subcommand: {cmd}", file=sys.stderr)
        return 2
    except OrchestratorError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except (TypeError, ValueError) as e:
        print(f"bad arguments for {cmd}: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(_cli())
