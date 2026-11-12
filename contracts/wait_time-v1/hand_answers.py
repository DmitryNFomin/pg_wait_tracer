#!/usr/bin/env python3
"""Transcribe the hand ledger below into JSON; NOT a reference evaluator.

Written before reference/time_grid.py. Every number, status, and derivation is
manually specified. This script does not read cases or evaluate metric formulas.
The later reference must not import this file or read expected answers.
"""

import json
from pathlib import Path


# id, total, closed, open, count, covered, required, T gap?, N gap?, derivation
HAND = [
    (1, 4, 4, 0, 1, 6, 6, False, False, [
        "closed=(6-4)+(10-8)=2+2=4; open=0; total=4.",
        "Closes in [4,10): {6}, count=1; required=10-4=6; covered=6."]),
    (2, 2, 2, 0, 0, 2, 2, False, False, [
        "closed=6-4=2; open=0; total=2.",
        "6 is excluded from [4,6): count=0; required=6-4=2; covered=2."]),
    (3, 2, 2, 0, 1, 4, 4, False, False, [
        "closed=10-8=2; first wait contributes 0; open=0; total=2.",
        "Closes in [6,10): {6}, count=1; required=10-6=4; covered=4."]),
    (4, 0, 0, 0, 1, 2, 2, False, False, [
        "PgSleep occupation in [6,8) is empty: closed=0; open=0; total=0.",
        "Close 6 is included: count=1; required=8-6=2; covered=2."]),
    (5, 2, 2, 0, 1, 20, 20, False, False, [
        "Lock closed=8-6=2; open=0; total=2.",
        "Lock closes at 8: count=1; required=20-0=20; covered=20."]),
    (6, 0, 0, 0, 0, 20, 20, False, False, [
        "No IO records: closed=0; open=0; total=0; count=0.",
        "required=20-0=20; covered=20; both measured zeros are complete."]),
    (7, 3, 3, 0, 0, 3, 3, False, False, [
        "closed=12-9=3; open=0; total=3.",
        "Close 14 is outside [9,12): count=0; required=12-9=3; covered=3."]),
    (8, 4, 4, 0, 1, 10, 15, True, True, [
        "recorded_ns=14-10=4; open=0; close 14 gives recorded_count=1.",
        "required=25-10=15; covered=20-10=10; gap=5, so both unavailable."]),
    (9, 0, 0, 0, 0, 0, 10, True, True, [
        "No matching occupation or closes in [30,40): recorded_ns=0; recorded_count=0.",
        "required=40-30=10; covered=0; gap=10, so both unavailable."]),
    (10, 5, 5, 0, 2, 15, 15, False, False, [
        "closed=(9-5)+(13-12)=4+1=5; open=0; total=5.",
        "Closes {9,13}: count=2; R=[5,20), required=20-5=15; covered=15."]),
    (11, 0, 0, 0, 0, 0, 0, False, False, [
        "[20,30) is after exit 15: R is empty; required=covered=0.",
        "No occupied time or close in W: closed=0; open=0; total=0; count=0."]),
    (12, 3, 3, 0, 1, 3, 3, False, False, [
        "closed=15-12=3; open=0; total=3; close 15 in [12,30) gives count=1.",
        "R=[12,15), required=15-12=3; covered=3; span ends at exit 15."]),
    (13, 5, 5, 0, 2, 17, 20, True, True, [
        "recorded_ns=(4-1)+(12-10)=3+2=5; closes {4,12}: recorded_count=2.",
        "required=20; covered=(6-0)+(20-9)=6+11=17; gap=3, both unavailable."]),
    (14, 2, 2, 0, 1, 11, 11, False, False, [
        "closed=12-10=2; open=0; total=2; close 12 gives count=1.",
        "required=20-9=11; covered=11; gap [6,9) is outside W."]),
    (15, 11, 3, 8, 1, 18, 18, False, False, [
        "closed=4-1=3; open=18-10=8; total=3+8=11.",
        "Only closed instant 4 counts: count=1; required=18-0=18; covered=18."]),
    (16, 3, 0, 3, 0, 3, 3, False, False, [
        "closed=0; open=15-12=3; total=3; open never contributes a close, count=0.",
        "required=15-12=3; covered=3; as_of 18 is after the window."]),
    (17, 6, 0, 6, 0, 6, 13, True, True, [
        "closed=0; open=18-12=6; recorded_ns=6; recorded_count=0.",
        "required=25-12=13; covered=18-12=6; gap=7, both unavailable."]),
    (18, 3, 3, 0, 1, 18, 18, False, False, [
        "PgSleep closed=4-1=3; open Lock contributes 0; total=3; count=1 at 4.",
        "required=18-0=18; covered=18; both complete."]),
    (19, 1, 1, 0, 1, 3, 3, False, False, [
        "closed=(5-5)+(8-7)=0+1=1; open=0; total=1.",
        "Close 5 included, 9 excluded: count=1; required=8-5=3; covered=3."]),
    (20, 0, 0, 0, 0, 3, 3, False, False, [
        "No occupation in [2,5): closed=0; open=0; total=0.",
        "Close 5 is at excluded to: count=0; required=5-2=3; covered=3."]),
    (21, 2, 2, 0, 1, 2, 2, False, False, [
        "closed=(6-5)+(7-6)=1+1=2; open=0; total=2.",
        "Only close 6 in [5,7): count=1; required=7-5=2; covered=2."]),
    (22, 1, 1, 0, 0, 1, 1, False, False, [
        "closed=6-5=1; open=0; total=1.",
        "Close 6 is excluded: count=0; required=6-5=1; covered=1."]),
    (26, 18446744073709550999, 18446744073709550999, 0, 1,
     18446744073709551615, 18446744073709551615, False, False, [
        "closed=18446744073709551000-1=18446744073709550999; open=0; total=18446744073709550999.",
        "0 <= close 18446744073709551000 < 18446744073709551615: count=1.",
        "required=18446744073709551615-0=18446744073709551615; covered=18446744073709551615."]),
    (27, 0, 0, 0, 0, 6, 6, False, False, [
        "Empty waits and no open: closed=0; open=0; total=0; count=0.",
        "required=9-3=6; covered=6, so both zeros are complete."]),
    (28, 3, 3, 0, 1, 0, 10, True, True, [
        "recorded_ns=5-2=3; open=0; close 5 gives recorded_count=1 even without coverage.",
        "required=10-0=10; covered=0; gap=10, so both unavailable."]),
    (29, 6, 6, 0, 1, 10, 10, False, False, [
        "closed=8-2=6; open=0; total=6; close 8 gives count=1.",
        "required=10; covered=(5-0)+(10-5)=5+5=10; touching spans leave no gap."]),
    (31, 0, 0, 0, 1, 0, 0, False, False, [
        "R=[10,12) intersect [0,10) is empty: required=covered=0; closed=open=total=0.",
        "10 <= close 10 < 12: count=1; coverage [0,10) certifies exit 10."]),
    (32, 0, 0, 0, 1, 0, 0, False, True, [
        "R is empty: required=covered=0; closed=open=total=0, so wait_time is complete.",
        "10 <= close 10 < 12: recorded_count=1; no span ends at exit, so completed_waits unavailable."]),
    (34, 2, 2, 0, 3, 10, 10, False, False, [
        "closed=(4-2)+(6-6)+(6-6)=2; open=0; total=2.",
        "Closes {4,6,6} are distinct records: count=3; required=10-0=10; covered=10."]),
    (41, 6, 0, 6, 0, 8, 8, False, False, [
        "as_of 8 < exit 10 is valid; closed=0; open=8-2=6; total=6; count=0.",
        "R=[0,8): required=8-0=8; covered=8; exit is not in W."]),
    (42, 6, 0, 6, 0, 8, 10, True, True, [
        "closed=0; open=8-2=6; recorded_ns=6; no recorded close, recorded_count=0.",
        "R=[0,10): required=10; covered=8; gap=2 and exit 10 uncertified, both unavailable."]),
    (43, 2, 2, 0, 1, 9, 9, False, False, [
        "closed=5-3=2; open=0; total=2; close 5 gives count=1.",
        "Actual W=[3,12): required=12-3=9; covered=9; exit 15 is outside W (C01)."]),
    (44, 3, 3, 0, 0, 3, 3, False, False, [
        "closed=15-12=3; open=0; total=3; close 15 equals excluded to, count=0.",
        "Actual W=[12,15): required=15-12=3; covered=3; exit not in W (C01)."]),
    (46, 4, 4, 0, 1, 6, 6, False, False, [
        "List order irrelevant: closed=(10-8)+(6-4)=2+2=4; open=0; total=4.",
        "Close 6 included, 14 excluded: count=1; required=10-4=6; covered=6."]),
    (48, 0, 0, 0, 1, 0, 0, False, True, [
        "life_start=life_end=7: required=covered=0; legal wait has 7-7=0 occupied ns; T complete at 0.",
        "0 <= close 7 < 20: recorded_count=1; no span ends at exit 7, so N unavailable (C02)."]),
]

# id, explicitly chosen sorted reasons, two lines showing the violated rules
INVALID = [
    (23, ["invalid_window"], [
        "from=10 >= to=10: invalid_window; arithmetic is not evaluated.",
        "Evidence valid; coverage reason suppressed for invalid window; all diagnostics=null."]),
    (24, ["evidence_invalid"], [
        "Occupied overlap [6,8) has 8-6=2 ns: rule 4 violated across events.",
        "Coverage [0,20) fills W, so no coverage reason; both values and all diagnostics=null."]),
    (25, ["evidence_invalid"], [
        "Wait start 8 > end 2: rule 3 violated.",
        "Coverage fills [0,20), so no coverage reason; both values and all diagnostics=null."]),
    (30, ["evidence_invalid"], [
        "Coverage starts 5 then 0: 5 > 0, violating sorted-by-start rule 7.",
        "Coverage itself invalid: no coverage_incomplete inference; values and diagnostics=null."]),
    (33, ["evidence_invalid"], [
        "Both zero-length records have 2 < 5 < 8, strictly inside the positive wait: rule 5 violated.",
        "Coverage fills [0,10), so no gap reason; both values and all diagnostics=null."]),
    (35, ["coverage_incomplete", "evidence_invalid"], [
        "Waits overlap [3,6), length 6-3=3: evidence_invalid.",
        "Valid coverage=4-0=4; required=10-0=10; gap=6 adds coverage_incomplete; diagnostics still all null."]),
    (36, ["evidence_invalid", "invalid_window"], [
        "Wait start 8 > end 2: evidence_invalid; query from 10 >= to 5: invalid_window.",
        "Both reasons retained alphabetically; no coverage reason for invalid window; all diagnostics=null."]),
    (37, ["evidence_invalid"], [
        "Coverage end 10 > open as_of 5: rule 8 violated.",
        "Coverage validity gate fails, so no coverage reason; both values and all diagnostics=null."]),
    (38, ["evidence_invalid"], [
        "Closed wait end 7 > open start 2: rule 6 violated even though occupied spans are disjoint.",
        "Coverage [0,4) fills W and ends at as_of 4; no gap; all diagnostics=null."]),
    (39, ["evidence_invalid"], [
        "Wait start -1 < 0 violates unsigned timestamp rule 1 and life_start 0 <= start.",
        "Coverage [0,2) fills W; no gap under either C07 reading; values and diagnostics=null."]),
    (40, ["evidence_invalid"], [
        "Distinct duplicate positive waits overlap for 18446744073709551615-0 ns: rule 4 violated.",
        "Coverage fills W; do not sum invalid waits or deduplicate; both values and all diagnostics=null."]),
    (45, ["invalid_query"], [
        "event has length 0, violating nonempty string requirement; 0 < 10 is otherwise a valid window.",
        "Evidence valid; no coverage reason for invalid query; both values and all diagnostics=null."]),
    (47, ["evidence_invalid"], [
        "Coverage start 0 < life_start 5: rule 7 violated; do not clip to repair evidence.",
        "Coverage validity gate fails, so no coverage reason; both values and all diagnostics=null."]),
]


def result(metric, value, recorded, covered, required, reasons, detail=None):
    obj = {
        "metric": metric, "version": 1, "value": value,
        "unit": "ns" if metric == "wait_time" else "count",
        "basis": "observed",
        "completeness": "unavailable" if reasons else "complete",
        "reasons": reasons,
    }
    if detail is not None:
        obj["detail"] = detail
    key = "recorded_ns" if metric == "wait_time" else "recorded_count"
    obj["diagnostic"] = {key: recorded, "covered_ns": covered, "required_ns": required}
    return obj


def main():
    destination = Path(__file__).resolve().parent / "contracts" / "expected"
    destination.mkdir(parents=True, exist_ok=True)
    answers = {}
    for i, total, closed, opened, count, covered, required, tgap, ngap, derivation in HAND:
        answers[i] = {
            "id": f"wait_window_{i:02}",
            "wait_time": result(
                "wait_time", None if tgap else total, total, covered, required,
                ["coverage_incomplete"] if tgap else [],
                None if tgap else {"closed_ns": closed, "open_ns": opened}),
            "completed_waits": result(
                "completed_waits", None if ngap else count, count, covered, required,
                ["coverage_incomplete"] if ngap else []),
            "derivation": "\n".join(derivation),
        }
    for i, reasons, derivation in INVALID:
        assert i not in answers
        answers[i] = {
            "id": f"wait_window_{i:02}",
            "wait_time": result("wait_time", None, None, None, None, reasons),
            "completed_waits": result("completed_waits", None, None, None, None, reasons),
            "derivation": "\n".join(derivation),
        }
    assert set(answers) == set(range(1, 49))
    for i in sorted(answers):
        path = destination / f"wait_window_{i:02}.json"
        path.write_text(json.dumps(answers[i], indent=2) + "\n", encoding="utf-8")
    print("Wrote 48 manually specified expected answers; no case inputs were evaluated.")


if __name__ == "__main__":
    main()
