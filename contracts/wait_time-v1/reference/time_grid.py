#!/usr/bin/env python3
"""Independent, deliberately slow contract-v1 evaluator (standard library only).

Written after all expected answers. Never reads/imports expected answers or the
manual ledger. Small queries enumerate every nanosecond, including time outside
the lifetime. Interval endpoint comparisons in validation check consistency;
they do not calculate small-query durations.

Output has id, the two contract-shaped metric objects, and an evaluator field
identifying the method. The method is metadata, not part of either metric.
Integers are Python arbitrary-precision integers. See CHALLENGES.md for explicit
choices about JSON types, ambiguous invalid-input combinations, and empty spans.
"""

import argparse
import json
from pathlib import Path


MAX_TIMESTAMP = (1 << 64) - 1
GRID_LIMIT = 100_000


def timestamp(value):
    # bool is a subclass of int in Python, but is not a JSON integer token.
    return type(value) is int and 0 <= value <= MAX_TIMESTAMP


def event_name(value):
    return isinstance(value, str) and len(value) > 0


def array(value, length):
    return isinstance(value, list) and len(value) == length


def record_shape(value):
    return (array(value, 3) and event_name(value[0])
            and timestamp(value[1]) and timestamp(value[2]))


def query_errors(query):
    if not isinstance(query, dict):
        return {"invalid_query"}
    valid_endpoints = timestamp(query.get("from")) and timestamp(query.get("to"))
    errors = set()
    if not valid_endpoints or not event_name(query.get("event")):
        errors.add("invalid_query")
    if valid_endpoints and query["from"] >= query["to"]:
        errors.add("invalid_window")
    return errors


def lifetime_valid(life):
    return (array(life, 2) and timestamp(life[0])
            and (life[1] is None or (timestamp(life[1]) and life[0] <= life[1])))


def positive_overlap(first, second):
    """Validation predicate only; empty spans cannot have occupied overlap."""
    a, b = first
    c, d = second
    return a < b and c < d and a < d and c < b


def coverage_valid(evidence):
    """Can a coverage gap be independently established? (C07.)

    Check lifetime and coverage types/range/order/lifetime containment, positive
    overlap, and rule 8. Unrelated invalid waits do not affect this predicate.
    With an open record, a trustworthy as_of is needed even for empty coverage.
    """
    life = evidence.get("life")
    spans = evidence.get("coverage")
    if not lifetime_valid(life) or not isinstance(spans, list):
        return False
    first, last = life
    for i, span in enumerate(spans):
        if not (array(span, 2) and timestamp(span[0]) and timestamp(span[1])):
            return False
        a, b = span
        if a > b or a < first or (last is not None and b > last):
            return False
        if i and spans[i - 1][0] > a:
            return False
        for earlier in spans[:i]:
            if positive_overlap(earlier, span):
                return False
    if "open" not in evidence:
        return False
    opened = evidence["open"]
    if opened is not None:
        if not (array(opened, 3) and timestamp(opened[2])):
            return False
        if any(span[1] > opened[2] for span in spans):
            return False
    return True


def evidence_valid(evidence):
    if not isinstance(evidence, dict):
        return False
    if not {"life", "coverage", "waits", "open"}.issubset(evidence):
        return False
    if not coverage_valid(evidence):
        return False
    life_start, life_end = evidence["life"]
    waits = evidence["waits"]
    if not isinstance(waits, list):
        return False
    for wait in waits:
        if not record_shape(wait):
            return False
        _, start, end = wait
        if start > end or start < life_start:
            return False
        if life_end is not None and end > life_end:
            return False
    # Pairwise checks deliberately preserve input order and duplicate records.
    for i, first in enumerate(waits):
        for second in waits[i + 1:]:
            if positive_overlap(first[1:], second[1:]):
                return False
            _, a, b = first
            _, c, d = second
            if a == b and c < a < d:
                return False
            if c == d and a < c < b:
                return False
    opened = evidence["open"]
    if opened is not None:
        if not record_shape(opened):
            return False
        _, start, as_of = opened
        if start > as_of or start < life_start:
            return False
        if life_end is not None and as_of >= life_end:
            return False
        for _, wait_start, wait_end in waits:
            if wait_end > start:
                return False
            if wait_start == wait_end and wait_start > start:
                return False
    return True


def scannable_records(evidence):
    """Only for safe classification of invalid input; never salvages a metric.

    All valid records are retained, including duplicates and every event.
    Negative/out-of-range integer endpoints can still be compared during a
    diagnostic scan, but the validation failure nulls all reported diagnostics.
    """
    waits = evidence.get("waits", [])
    if not isinstance(waits, list):
        waits = []
    closed = [w for w in waits if array(w, 3)
              and type(w[1]) is int and type(w[2]) is int]
    opened = evidence.get("open")
    if not (array(opened, 3) and type(opened[1]) is int and type(opened[2]) is int):
        opened = None
    return closed, opened


def scannable_coverage(evidence):
    spans = evidence.get("coverage", [])
    if not isinstance(spans, list):
        return []
    return [s for s in spans if array(s, 2)
            and type(s[0]) is int and type(s[1]) is int]


def nanosecond_grid(evidence, query, usable_records):
    """Walk EVERY tick in W. No interval intersection/length arithmetic here.

    Each integer t represents [t,t+1). Integer endpoints make this exhaustive.
    At each tick classify lifetime, coverage, all occupied record identities,
    open occupation, and closes. Close counts are not gated by lifetime/state:
    the tick at a known exit may contain legal close instants without occupation.
    """
    life = evidence.get("life")
    valid_life = lifetime_valid(life)
    spans = scannable_coverage(evidence)
    waits, opened = scannable_records(evidence)
    required = covered = closed_ns = open_ns = completed = 0
    for tick in range(query["from"], query["to"]):
        in_lifetime = valid_life and life[0] <= tick and (life[1] is None or tick < life[1])
        is_covered = any(a <= tick < b for a, b in spans)
        inside_closed = [index for index, (_, start, end) in enumerate(waits)
                         if start <= tick < end]
        inside_open = opened is not None and opened[1] <= tick < opened[2]
        closes_now = [index for index, (_, _, end) in enumerate(waits) if end == tick]
        if in_lifetime:
            required += 1
            if is_covered:
                covered += 1
        if usable_records:
            for index in inside_closed:
                if waits[index][0] == query["event"]:
                    closed_ns += 1
            if inside_open and opened[0] == query["event"]:
                open_ns += 1
            for index in closes_now:
                if waits[index][0] == query["event"]:
                    completed += 1
    return required, covered, closed_ns, open_ns, completed


# LARGE-WINDOW FALLBACK ONLY: exact integer interval arithmetic.
# This deliberately separate function is never used for a small query.
def exact_integer_intervals(evidence, query, usable_records, usable_coverage):
    """Avoid an infeasible 2^64-step walk; use no floats or bounded accumulators."""
    left, right = query["from"], query["to"]
    life = evidence.get("life")
    required = covered = closed_ns = open_ns = completed = 0
    if lifetime_valid(life):
        r_left = max(left, life[0])
        r_right = right if life[1] is None else min(right, life[1])
        required = max(0, r_right - r_left)
        if usable_coverage:
            # Valid positive coverage interiors are disjoint; empties add zero.
            for a, b in evidence["coverage"]:
                covered += max(0, min(r_right, b) - max(r_left, a))
    if usable_records:
        for event, start, end in evidence["waits"]:
            if event == query["event"]:
                closed_ns += max(0, min(end, right) - max(start, left))
                if left <= end < right:
                    completed += 1
        opened = evidence["open"]
        if opened is not None and opened[0] == query["event"]:
            open_ns = max(0, min(opened[2], right) - max(opened[1], left))
    return required, covered, closed_ns, open_ns, completed


def evaluate(case):
    query = case.get("query")
    raw_evidence = case.get("evidence")
    evidence = raw_evidence if isinstance(raw_evidence, dict) else {}
    q_errors = query_errors(query)
    valid_evidence = evidence_valid(raw_evidence)
    valid_coverage = coverage_valid(evidence)
    base_errors = set(q_errors)
    if not valid_evidence:
        base_errors.add("evidence_invalid")

    required = covered = closed_ns = open_ns = completed = None
    missing_time = missing_closes = False
    mode = "validation_only"
    explanation = "Invalid query/window; no metric arithmetic is defined."
    if not q_errors:
        if query["to"] - query["from"] <= GRID_LIMIT:
            mode = "nanosecond_grid"
            explanation = "Walked every integer nanosecond in the query window."
            required, covered, closed_ns, open_ns, completed = nanosecond_grid(
                evidence, query, valid_evidence)
        else:
            mode = "exact_integer_intervals"
            explanation = ("Window exceeds 100000 ns; used the separate exact integer "
                           "interval function instead of a nanosecond walk.")
            required, covered, closed_ns, open_ns, completed = exact_integer_intervals(
                evidence, query, valid_evidence, valid_coverage)
        if valid_coverage:
            missing_time = covered != required
            exit_time = evidence["life"][1]
            exit_inside = exit_time is not None and query["from"] <= exit_time < query["to"]
            exit_certified = any(b == exit_time for _, b in evidence["coverage"])
            missing_closes = missing_time or (exit_inside and not exit_certified)

    result = {"id": case.get("id")}
    for metric, missing in (("wait_time", missing_time), ("completed_waits", missing_closes)):
        reasons = set(base_errors)
        if missing:
            reasons.add("coverage_incomplete")
        time_metric = metric == "wait_time"
        recorded_key = "recorded_ns" if time_metric else "recorded_count"
        recorded = None
        if not base_errors:
            recorded = closed_ns + open_ns if time_metric else completed
        item = {
            "metric": metric,
            "version": 1,
            "value": None if reasons else recorded,
            "unit": "ns" if time_metric else "count",
            "basis": "observed",
            "completeness": "unavailable" if reasons else "complete",
            "reasons": sorted(reasons),
        }
        if time_metric and not reasons:
            item["detail"] = {"closed_ns": closed_ns, "open_ns": open_ns}
        item["diagnostic"] = {
            recorded_key: recorded,
            "covered_ns": None if base_errors else covered,
            "required_ns": None if base_errors else required,
        }
        result[metric] = item
    result["evaluator"] = {"mode": mode, "explanation": explanation}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path, help="JSON case file")
    args = parser.parse_args()
    with args.case.open(encoding="utf-8") as stream:
        case = json.load(stream)
    if not isinstance(case, dict):
        parser.error("case must be a JSON object")
    print(json.dumps(evaluate(case), indent=2))


if __name__ == "__main__":
    main()
