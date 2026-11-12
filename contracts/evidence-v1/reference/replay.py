#!/usr/bin/env python3
"""Executable logical reference; Python 3 standard library only.

Output is JSONL, in this exact order:
  {"projection": [{"generation": {"boot": str, "capture": str, "pid": int,
                    "fork": int|null},
                   "start_known": bool|null, "flags": ["LIFE_INVALID"]|[],
                   "core": {"life": [start,exit]|null,
                            "coverage": [[a,b],...],
                            "waits": [[event,start,end],...],
                            "open": [event,start,as_of]|null}}, ...],
   "target": {"capture": str, "pid": int, "fork": int|null}}
  <wait_time result object, exactly wait_time-v1 section 5>
  <completed_waits result object, exactly wait_time-v1 section 5>
Generations are in first-evidence order; each core is selected independently for
the scenario query. Results concern only target. LIFE_INVALID is an adapter flag,
not a new metric reason. No other status flags are defined by evidence-v1.

If an unresolved normative choice affects replay, the entire scenario instead
emits just {"ambiguity": "<section and question>"}. No guessed metric follows.
The executable consumes logical storage fixtures, not raw merge inputs: merge
qualification is asserted by the descriptor (evidence-v1 sections 3 and 7).
"""

import copy
import json
import sys
from dataclasses import dataclass, field


MAX_TIME = (1 << 64) - 1
IDENTITY_SLOTS = (0, 1, 2, 10, 12, 14)


class Ambiguity(Exception):
    """A question the supplied normative text does not resolve."""


def uint(value, maximum=MAX_TIME):
    """Evidence §1; wait_time §7.5: bool and float are not JSON integers."""
    return type(value) is int and 0 <= value <= maximum


def name(value):
    """Evidence §2: resolved, nonempty wait name, excluding the exit sentinel."""
    return isinstance(value, str) and bool(value) and value != "$EXIT"


def vector(value):
    """Evidence §§2–3: the ordered 15-component loss vector."""
    return isinstance(value, list) and len(value) == 15 and all(map(uint, value))


@dataclass
class Generation:
    key: tuple
    life: object = None
    start_known: object = None
    discovery: object = None
    invalid: bool = False
    tainted: bool = False
    life_order: object = None
    runs: list = field(default_factory=list)


@dataclass
class Run:
    attach: dict
    order: int
    epoch: int
    end: object = None
    frozen: bool = False
    snapshots: list = field(default_factory=list)


@dataclass
class Capture:
    boot: str
    capture: str
    qualified: bool
    baseline: object
    generations: dict = field(default_factory=dict)
    transitions: list = field(default_factory=list)
    pending: list = field(default_factory=list)
    frontiers: list = field(default_factory=list)
    highest: object = None
    latest_void: object = None
    identity_void: object = None
    epoch: int = 0

    def generation(self, pid, fork):
        key = (self.boot, self.capture, pid, fork)
        if key not in self.generations:
            self.generations[key] = Generation(key)
        return self.generations[key]


def section_1_order(files):
    """Evidence §1: physical chain order and physical-position deduplication.

    In §7 fixtures, array indices are the intact physical positions. Equal
    payloads at different indices are deliberately never deduplicated.
    """
    for file in files:
        for position, record in enumerate(file["records"]):
            yield file, position, record


def section_2_record(record):
    """Evidence §2: validate logical fields without inventing missing evidence."""
    if not isinstance(record, dict) or not uint(record.get("t")):
        return False
    kind = record.get("kind")
    if kind in ("LIFE", "ATTACH_OK", "END", "TRANS"):
        if not uint(record.get("pid"), (1 << 32) - 1) or record["pid"] == 0:
            return False
    if kind in ("LIFE", "ATTACH_OK", "END"):
        if "fork" not in record or not (record["fork"] is None or uint(record["fork"])):
            return False
    if kind == "LIFE":
        if not {"start", "start_known", "exit"} <= record.keys():
            return False
        s, x, k = record["start"], record["exit"], record["start_known"]
        return (uint(s) and type(k) is bool and s <= record["t"]
                and ((k and record["fork"] == s) or (not k and record["fork"] is None))
                and (x is None or (uint(x) and s <= x <= record["t"])))
    if kind == "TRANS":
        return ({"old", "new", "duration"} <= record.keys()
                and (record["old"] is None or name(record["old"]))
                and (record["new"] is None or name(record["new"]) or record["new"] == "$EXIT")
                and uint(record["duration"]) and record["duration"] <= record["t"]
                and record["old"] != record["new"])
    if kind == "ATTACH_OK":
        return ({"seed", "state", "counters"} <= record.keys()
                and record.get("method") in ("reread", "transition")
                and all(record[k] is None or name(record[k]) for k in ("seed", "state"))
                and vector(record["counters"]))
    if kind == "FRONTIER":
        return vector(record.get("counters"))
    if kind == "VOID":
        return ({"counters", "invalid_from", "reason"} <= record.keys()
                and (record["counters"] is None or vector(record["counters"]))
                and (record["invalid_from"] is None or uint(record["invalid_from"]))
                and isinstance(record["reason"], str) and bool(record["reason"]))
    if kind == "END":
        return record.get("reason") in ("detach", "deescalate", "stop")
    return False


def section_3_certificate(capture, record):
    """Evidence §3: qualification and baseline are necessary certificate gates."""
    if not capture.qualified:
        return False
    if record["kind"] in ("ATTACH_OK", "FRONTIER"):
        return capture.baseline is not None and record["counters"] == capture.baseline
    return True


def apply_life(capture, record, order):
    """Evidence §§2,4: immutable lifetime facts and lifecycle-loss eligibility."""
    g = capture.generation(record["pid"], record["fork"])
    if g.tainted:
        return
    if capture.identity_void is not None and g.life is None:
        if not (record["start_known"] and record["fork"] >= capture.identity_void
                and capture.baseline is not None):
            g.tainted = True
            return
    if g.life is not None:
        if (g.life[0] != record["start"] or g.start_known != record["start_known"]
                or (g.life[1] is not None and g.life[1] != record["exit"])):
            g.invalid = True
            return
        g.life[1] = record["exit"]
    else:
        g.life = [record["start"], record["exit"]]
        g.start_known = record["start_known"]
        g.discovery = record["t"]
        g.life_order = order


def apply_void(capture, record):
    """Evidence §4: rollback certificates, freeze runs, replace baseline, taint."""
    r, counters = record["invalid_from"], record["counters"]
    uncertain = (r is not None or counters is None or capture.baseline is None
                 or any(counters[i] != capture.baseline[i] for i in IDENTITY_SLOTS))
    if r is not None:
        capture.frontiers = [f for f in capture.frontiers if f < r]
    for g in capture.generations.values():
        if uncertain:
            g.tainted = True
        for run in g.runs:
            if r is not None:
                run.snapshots = [s for s in run.snapshots if s["frontier"] < r]
            run.frozen = True
            # Pending END never supplies a bound after loss.
            if not run.snapshots or (run.end is not None and run.end > run.snapshots[-1]["b"]):
                run.end = None
    capture.pending.clear()
    capture.transitions = []  # surviving snapshots own all certified history
    capture.baseline = copy.deepcopy(counters)
    capture.latest_void = record["t"]
    if uncertain:
        capture.identity_void = record["t"]
    capture.epoch += 1


def resolve_transitions(capture, cutoff):
    """Evidence §4: assign TRANS using lifetime timestamps, never current PID."""
    assigned = {key: [] for key in capture.generations}
    blocked = set()
    for order, epoch, trans in capture.transitions:
        if trans["t"] > cutoff:
            continue
        choices = []
        for key, g in capture.generations.items():
            if key[2] != trans["pid"] or g.life is None or g.invalid or g.tainted:
                continue
            start = key[3] if key[3] is not None else g.discovery
            x = g.life[1]
            matches = (x == trans["t"] if trans["new"] == "$EXIT"
                       else start <= trans["t"] and (x is None or trans["t"] < x))
            if matches:
                choices.append(key)
        if len(choices) == 1:
            assigned[choices[0]].append((trans["t"], order, epoch, trans))
        else:
            blocked.update(key for key in capture.generations if key[2] == trans["pid"])
    for history in assigned.values():
        history.sort(key=lambda item: (item[0], item[1]))
    return assigned, blocked


def track_run(g, run, history, b):
    """Evidence §§3–4: reconcile an anchor, then all genuine edges through b."""
    a = run.attach["t"]
    transitions = [item[3] for item in history
                   if item[2] == run.epoch and a <= item[0] <= b]
    state, entry = run.attach["state"], None
    trim = a
    if run.attach["method"] == "transition":
        if not transitions or transitions[0]["t"] != a:
            return None  # unresolved anchor dependency withholds certification
        anchor = transitions.pop(0)
        if anchor["new"] != state:
            raise ValueError("anchor state mismatch")
        trim = a + (anchor["old"] is not None)
        entry = a if state is not None else None
    else:
        if run.attach["seed"] != state:
            raise ValueError("reread seed mismatch")
        transitions = [t for t in transitions if t["t"] > a]
        if state is not None:
            trim = None
    waits, terminal_exit = [], False
    for trans in transitions:
        t, old, new = trans["t"], trans["old"], trans["new"]
        if terminal_exit or old != state:
            raise ValueError("transition state mismatch")
        unknown_close = old is not None and entry is None
        if old is not None and entry is not None:
            if t - trans["duration"] != entry:
                raise ValueError("transition duration mismatch")
            waits.append([old, entry, t])
        if unknown_close:
            trim = t + 1
        if new == "$EXIT":
            if g.life[1] != t:
                return None
            terminal_exit = not unknown_close
            state, entry = None, None
        else:
            state, entry = new, t if new is not None else None
    return {"b": b, "trim": trim, "waits": waits,
            "open": [state, entry, b] if state is not None and entry is not None else None,
            "exit_certified": terminal_exit}


def commit_frontier(capture, record):
    """Evidence §4: validate dependencies before atomically accepting a frontier."""
    f = record["t"]
    if capture.highest is not None and f <= capture.highest:
        raise Ambiguity("evidence-v1 §4: nonadvancing FRONTIER is invalid; ignore it, freeze runs, or stop the chain?")
    # Resolve attachment/END dependencies only once all preceding LIFE is present.
    unresolved = []
    for order, epoch, r in capture.pending:
        g = capture.generation(r["pid"], r["fork"])
        if g.invalid or g.tainted:
            continue
        if g.life is None:
            unresolved.append((order, epoch, r))
            continue
        if r["kind"] == "ATTACH_OK":
            if capture.identity_void is not None and order <= g.life_order:
                continue  # lifecycle recovery requires LIFE, THEN a fresh attach
            if r["t"] < g.life[0] or (g.life[1] is not None and r["t"] >= g.life[1]):
                raise Ambiguity("evidence-v1 §§2,5: invalid attachment lifetime: does its global VOID taint identity and what baseline follows?")
            previous = [run for run in g.runs if not run.frozen]
            if any(run.end is None or run.end > r["t"] for run in previous):
                raise Ambiguity("evidence-v1 §§4,5: overlapping attachments are invalid; which preceding certificate is retained and what baseline follows?")
            g.runs.append(Run(r, order, epoch))
        else:
            choices = [run for run in g.runs if not run.frozen
                       and run.attach["t"] <= r["t"] and run.end is None]
            if len(choices) != 1:
                raise Ambiguity("evidence-v1 §§3,4: END has no unique attachment; what is the invalid-record recovery baseline?")
            choices[0].end = r["t"]
    capture.pending = unresolved
    assigned, blocked = resolve_transitions(capture, f)
    commits = []
    try:
        for key, g in capture.generations.items():
            if g.invalid or g.tainted or g.life is None or key in blocked:
                continue
            runs = [run for run in g.runs if not run.frozen]
            touching = {run.attach["t"] for run in runs
                        if any(other is not run and other.end == run.attach["t"] for other in runs)}
            if any(item[0] in touching for item in assigned[key]):
                continue  # §3 explicitly withholds affected certification
            for run in runs:
                if run.attach["t"] > f:
                    continue
                bounds = [f] + [v for v in (run.end, g.life[1]) if v is not None]
                snapshot = track_run(g, run, assigned[key], min(bounds))
                if snapshot is not None:
                    snapshot["frontier"] = f
                    commits.append((run, snapshot))
    except ValueError as error:
        raise Ambiguity("evidence-v1 §5: run consistency failure (" + str(error)
                        + ") acts as global VOID; what are its counter baseline and lifecycle-uncertainty instant?") from error
    capture.highest = f
    capture.frontiers.append(f)
    for run, snapshot in commits:
        run.snapshots.append(snapshot)


def section_4_replay(capture, files):
    """Evidence §4: append-order state machine; LIFE is independent of coverage."""
    for order, (_, _, record) in enumerate(section_1_order(files)):
        if not section_2_record(record):
            if isinstance(record, dict) and record.get("kind") == "LIFE" and "pid" in record and "fork" in record:
                capture.generation(record["pid"], record["fork"]).invalid = True
            raise Ambiguity("evidence-v1 §5: malformed/unsupported record acts as global VOID, but its counter baseline and lifecycle uncertainty are unspecified")
        kind = record["kind"]
        if kind == "LIFE":
            apply_life(capture, record, order)
        elif kind == "VOID":
            apply_void(capture, record)
        elif kind == "TRANS":
            capture.transitions.append((order, capture.epoch, record))
        elif not capture.qualified:
            continue
        elif not section_3_certificate(capture, record):
            # An unexpected readable vector supplies the loss observation.
            apply_void(capture, {"t": record["t"], "counters": record["counters"], "invalid_from": None})
        elif kind in ("ATTACH_OK", "END"):
            if kind == "ATTACH_OK" and capture.latest_void is not None and record["t"] < capture.latest_void:
                continue
            capture.generation(record["pid"], record["fork"])
            capture.pending.append((order, capture.epoch, record))
        elif kind == "FRONTIER":
            commit_frontier(capture, record)


def query_reasons(query):
    """wait_time §§3,7.4: independent query and window validation."""
    if not isinstance(query, dict):
        return ["invalid_query"]
    endpoints = all(k in query and uint(query[k]) for k in ("from", "to"))
    reasons = []
    if not endpoints or not isinstance(query.get("event"), str) or not query["event"]:
        reasons.append("invalid_query")
    if endpoints and query["from"] >= query["to"]:
        reasons.append("invalid_window")
    return reasons


def required(life, query):
    """wait_time §§1,4 and lifetime clarification: retain conservative start."""
    left = max(life[0], query["from"])
    right = query["to"] if life[1] is None else min(query["to"], life[1])
    return left, max(left, right)


def section_5_projection(g, query):
    """Evidence §5: trim EACH run, then choose exactly ONE, event-independently."""
    empty = {"life": None if g.invalid or g.life is None else g.life[:],
             "coverage": [], "waits": [], "open": None}
    if empty["life"] is None:
        return empty
    candidates = []
    for run in g.runs:
        if not run.snapshots:
            continue
        snapshot = run.snapshots[-1]
        x = g.life[1]
        if x is not None and snapshot["b"] >= x and not snapshot["exit_certified"]:
            earlier = [s for s in run.snapshots if s["frontier"] < x and s["b"] < x]
            if not earlier:
                continue
            snapshot = earlier[-1]
        a, b = snapshot["trim"], snapshot["b"]
        if a is None or a > MAX_TIME or a > b:
            continue
        core = {"life": g.life[:], "coverage": [[a, b]],
                "waits": copy.deepcopy(snapshot["waits"]), "open": copy.deepcopy(snapshot["open"])}
        candidates.append((a, b, run.order, snapshot["exit_certified"], core))
    if not candidates:
        return empty
    if query_reasons(query):
        return min(candidates, key=lambda c: (c[0], c[1], c[2]))[4]
    left, right = required(g.life, query)
    exit_needed = g.life[1] is not None and query["from"] <= g.life[1] < query["to"]

    def preference(candidate):
        a, b, order, exit_certified, _ = candidate
        overlap = max(0, min(b, right) - max(a, left))
        full = overlap == right - left and (not exit_needed or (b == g.life[1] and exit_certified))
        return (not full, -overlap, a, b, order)

    return min(candidates, key=preference)[4]


def section_6_files(files):
    """Evidence §6: stop each chain at its first gap; captures never stitch."""
    captures, chains, stopped = {}, {}, set()
    for file in files:
        key = (file["boot"], file["capture"])
        if key in stopped:
            continue
        previous = chains.get(key)
        good_link = ((previous is None and file["file_no"] == 0 and file["previous"] is None)
                     or (previous is not None and file["file_no"] == previous + 1 and file["previous"] == previous))
        if not file.get("present", True) or not good_link or file["version"] not in (2, 3, 4, 5):
            stopped.add(key)
            continue
        if key not in captures:
            captures[key] = Capture(*key, file.get("qualified") is True,
                                    copy.deepcopy(file.get("loss_baseline")))
        capture = captures[key]
        if file["version"] != 5:
            # Legacy records reveal a target handle, never a valid lifetime.
            for record in file["records"]:
                if "pid" in record:
                    capture.generation(record["pid"], None)
        else:
            # Rotation carries no independent reset of the counter baseline.
            chains[key] = file["file_no"]
    # Replay only the validated prefix collected above, without reading any suffix.
    for key, capture in captures.items():
        prefix = []
        for file in files:
            if (file["boot"], file["capture"]) != key:
                continue
            if not file.get("present", True) or file["version"] != 5:
                break
            if file["file_no"] > chains.get(key, -1):
                break
            prefix.append(file)
        section_4_replay(capture, prefix)
    return captures


def core_evidence_valid(core):
    """wait_time §2 and §7.2: full validation and independently knowable gaps."""
    life, coverage, waits, opened = (core[k] for k in ("life", "coverage", "waits", "open"))
    life_ok = (isinstance(life, list) and len(life) == 2 and uint(life[0])
               and (life[1] is None or (uint(life[1]) and life[0] <= life[1])))
    if not life_ok:
        return False, False

    def inside(s, e):
        return uint(s) and uint(e) and life[0] <= s <= e and (life[1] is None or e <= life[1])

    coverage_ok = isinstance(coverage, list)
    last_a, last_positive_end = -1, -1
    if coverage_ok:
        for span in coverage:
            if not isinstance(span, list) or len(span) != 2 or not inside(*span):
                coverage_ok = False
                break
            a, b = span
            if a < last_a or (a < b and a < last_positive_end):
                coverage_ok = False
            last_a = a
            if a < b:
                last_positive_end = b
    asof_ok = opened is None or (isinstance(opened, list) and len(opened) == 3 and uint(opened[2]))
    gap_knowable = coverage_ok and asof_ok
    if gap_knowable and opened is not None:
        gap_knowable = all(b <= opened[2] for a, b in coverage)
    valid = gap_knowable and isinstance(waits, list)
    if not isinstance(waits, list):
        return False, gap_knowable
    for wait in waits:
        if not isinstance(wait, list) or len(wait) != 3 or not isinstance(wait[0], str) or not wait[0] or not inside(*wait[1:]):
            return False, gap_knowable
    for i, (_, s, e) in enumerate(waits):
        for _, u, v in waits[i + 1:]:
            if max(s, u) < min(e, v) or (s == e and u < s < v) or (u == v and s < u < e):
                valid = False
    if opened is not None:
        if (not isinstance(opened, list) or len(opened) != 3
                or not isinstance(opened[0], str) or not opened[0]
                or not inside(*opened[1:]) or (life[1] is not None and opened[2] >= life[1])):
            valid = False
        elif any(e > opened[1] or (s == e and s > opened[1]) for _, s, e in waits):
            valid = False
    return valid, gap_knowable


def core_results(core, query):
    """wait_time §§3–5,7: exact arithmetic, independent metrics and all reasons."""
    valid, gap_knowable = core_evidence_valid(core)
    qreasons = query_reasons(query)
    results = []
    for metric, unit, diagnostic_key in (("wait_time", "ns", "recorded_ns"),
                                         ("completed_waits", "count", "recorded_count")):
        reasons = qreasons[:]
        if not valid:
            reasons.append("evidence_invalid")
        required_ns = covered_ns = recorded = None
        closed_ns = open_ns = 0
        if not qreasons and gap_knowable:
            left, right = required(core["life"], query)
            required_ns = right - left
            covered_ns = sum(max(0, min(b, right) - max(a, left)) for a, b in core["coverage"])
            exit_at = core["life"][1]
            exit_missing = (metric == "completed_waits" and exit_at is not None
                            and query["from"] <= exit_at < query["to"]
                            and not any(b == exit_at for a, b in core["coverage"]))
            if covered_ns < required_ns or exit_missing:
                reasons.append("coverage_incomplete")
        if valid and not qreasons:
            def occupied(s, e):
                return max(0, min(e, query["to"]) - max(s, query["from"]))

            matching = [w for w in core["waits"] if w[0] == query["event"]]
            closed_ns = sum(occupied(s, e) for _, s, e in matching)
            opened = core["open"]
            if opened is not None and opened[0] == query["event"]:
                open_ns = occupied(*opened[1:])
            recorded = (closed_ns + open_ns if metric == "wait_time" else
                        sum(query["from"] <= e < query["to"] for _, s, e in matching))
        if not valid or qreasons:
            required_ns = covered_ns = recorded = None
        result = {"metric": metric, "version": 1, "value": None if reasons else recorded,
                  "unit": unit, "basis": "observed",
                  "completeness": "unavailable" if reasons else "complete", "reasons": sorted(set(reasons))}
        if metric == "wait_time" and not reasons:
            result["detail"] = {"closed_ns": closed_ns, "open_ns": open_ns}
        result["diagnostic"] = {diagnostic_key: recorded, "covered_ns": covered_ns, "required_ns": required_ns}
        results.append(result)
    return results


def section_7_scenario(scenario):
    """Evidence §7: one target, one unchanged core query, both metrics."""
    captures = section_6_files(scenario["files"])
    target, query = scenario["target"], scenario["query"]
    matches = [c for c in captures.values() if c.capture == target["capture"]]
    if len(matches) > 1:
        raise Ambiguity("evidence-v1 §§6,7: target omits boot; which clock domain does this query address?")
    if not matches:
        boots = {f["boot"] for f in scenario["files"] if f["capture"] == target["capture"]}
        if len(boots) != 1:
            raise Ambiguity("evidence-v1 §7: target capture has no unique boot descriptor")
        capture = Capture(boots.pop(), target["capture"], False, None)
        captures[(capture.boot, capture.capture)] = capture
    else:
        capture = matches[0]
    target_g = capture.generation(target["pid"], target["fork"])
    projection, target_core = [], None
    for c in captures.values():
        for g in c.generations.values():
            core = section_5_projection(g, query)
            boot, capture_id, pid, fork = g.key
            projection.append({"generation": {"boot": boot, "capture": capture_id, "pid": pid, "fork": fork},
                               "start_known": g.start_known, "flags": ["LIFE_INVALID"] if core["life"] is None else [],
                               "core": core})
            if g is target_g:
                target_core = core
    return [{"projection": projection, "target": target}] + core_results(target_core, query)


def main():
    """Evidence §7: argv[1] is a scenario; stdout contains JSON objects only."""
    if len(sys.argv) != 2:
        print("usage: replay.py SCENARIO.json", file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as source:
        scenario = json.load(source)
    try:
        output = section_7_scenario(scenario)
    except Ambiguity as error:
        output = [{"ambiguity": str(error)}]
    for item in output:
        print(json.dumps(item, ensure_ascii=False, separators=(",", ":"), allow_nan=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
